#include "db/database.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>

#include "db/disk_counter.hpp"

namespace db {

using Clock = std::chrono::high_resolution_clock;
static double msDesde(const Clock::time_point& t0) {
    return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

static std::string unir(const std::string& dir, const std::string& f) {
    if (dir.empty()) return f;
    char c = dir[dir.size() - 1];
    return (c == '/' || c == '\\') ? dir + f : dir + "/" + f;
}

// Adapta un literal del SQL al tipo declarado de la columna.
static Value coerce(const Value& v, Type destino, const std::string& col) {
    if (v.type == destino) return v;
    if (destino == Type::DOUBLE && v.type == Type::INT)  return Value::makeDouble(static_cast<double>(v.i));
    if (destino == Type::INT    && v.type == Type::DOUBLE) {
        if (std::floor(v.d) != v.d)
            throw DBException("La columna '" + col + "' es INT y se recibio el decimal " + v.str());
        return Value::makeInt(static_cast<std::int64_t>(v.d));
    }
    // Una geometria no se convierte desde ningun otro tipo: o viene de
    // POINT(x,y) / POLYGON((..)) o es un error del usuario. Lo mismo al reves.
    const bool geo_dest = (destino == Type::POINT || destino == Type::POLYGON);
    const bool geo_val  = (v.type  == Type::POINT || v.type  == Type::POLYGON);
    if (geo_dest || geo_val)
        throw DBException("La columna '" + col + "' es de tipo " + typeName(destino) +
                          " y se recibio un valor " + typeName(v.type) +
                          ". Los puntos se escriben POINT(x, y) y los poligonos "
                          "POLYGON((x1,y1),(x2,y2),...).");
    if (destino == Type::VARCHAR) return Value::makeStr(v.str());
    throw DBException("Tipo incompatible para la columna '" + col + "': se esperaba " + typeName(destino));
}

// Cotas abiertas: el motor solo sabe buscar rangos cerrados, asi que un
// "col > 100" se traduce a [100, maximo del tipo].
// Los motores (Sequential y B+) solo resuelven rangos CERRADOS. Para > y <
// se les pide el rango cerrado y aqui se descarta lo que cae justo sobre la
// cota. Sale un filtro en RAM sobre las filas ya recuperadas: no cambia la
// ruta de acceso ni el conteo de I/O del plan.
static bool dentroDelPredicado(const Value& v, const Predicate& w,
                               const Value& lo, const Value& hi) {
    if (!w.lo_abierto && w.lo_estricto && !(lo < v)) return false;
    if (!w.hi_abierto && w.hi_estricto && !(v < hi)) return false;
    return true;
}

// Entero como texto, para los detalles del plan.
static std::string num0(double v) {
    return std::to_string(static_cast<long long>(std::llround(v)));
}

static Value cotaMin(Type t) {
    switch (t) {
        case Type::INT:     return Value::makeInt(std::numeric_limits<std::int64_t>::min());
        case Type::DOUBLE:  return Value::makeDouble(-std::numeric_limits<double>::infinity());
        case Type::VARCHAR: return Value::makeStr("");
        case Type::POINT:   return Value::makePoint(-std::numeric_limits<double>::infinity(),
                                                    -std::numeric_limits<double>::infinity());
        case Type::POLYGON: return Value::makePolygon({});
    }
    return Value::makeInt(0);
}
static Value cotaMax(Type t) {
    switch (t) {
        case Type::INT:     return Value::makeInt(std::numeric_limits<std::int64_t>::max());
        case Type::DOUBLE:  return Value::makeDouble(std::numeric_limits<double>::infinity());
        case Type::VARCHAR: return Value::makeStr(std::string(31, '\x7f'));
        case Type::POINT:   return Value::makePoint(std::numeric_limits<double>::infinity(),
                                                    std::numeric_limits<double>::infinity());
        case Type::POLYGON: return Value::makePolygon({});
    }
    return Value::makeInt(0);
}

// Evaluacion completa de una condicion sobre un valor ya materializado.
// Es lo que convierte a las condiciones que NO conducen el acceso en un filtro
// en memoria sobre las filas que trajo la ruta elegida.
static bool cumple(const Value& v, const Predicate& w) {
    switch (w.kind) {
        case PredKind::NONE:
            return true;
        case PredKind::EQ:
            return v == coerce(w.eq, v.type, w.column);
        case PredKind::WITHIN:
            if (v.type == Type::POLYGON)
                return poly::intersecaRect(v.poly, MBR(w.wx0, w.wy0, w.wx1, w.wy1));
            return v.type == Type::POINT &&
                   MBR::point(v.d, v.y).interseca(MBR(w.wx0, w.wy0, w.wx1, w.wy1));
        case PredKind::CONTIENE:
            return v.type == Type::POLYGON && poly::contienePunto(v.poly, w.qlon, w.qlat);
        case PredKind::RADIO: {
            if (v.type != Type::POINT) return false;
            const double d = geo::haversine(w.qlon, w.qlat, v.d, v.y);
            return w.radio_estricto ? (d < w.metros) : (d <= w.metros);
        }
        case PredKind::RANGE: {
            if (!w.lo_abierto) {
                Value lo = coerce(w.lo, v.type, w.column);
                if (w.lo_estricto ? !(lo < v) : (v < lo)) return false;
            }
            if (!w.hi_abierto) {
                Value hi = coerce(w.hi, v.type, w.column);
                if (w.hi_estricto ? !(v < hi) : (hi < v)) return false;
            }
            return true;
        }
    }
    return true;
}

// Cuanto le conviene al motor dejar que ESTA condicion conduzca el acceso.
// Menor es mejor. Es la regla del planificador hibrido: entre varias
// condiciones gana la de ruta mas barata, y las demas se degradan a filtro.
//
// El orden sale de la SELECTIVIDAD ESPERADA por la forma del predicado, que es
// lo unico que se puede saber sin estadisticas: una igualdad sobre una columna
// indexada devuelve del orden de una fila, mientras que una ventana espacial o
// un rango pueden devolver miles. Por eso la igualdad manda sobre el WITHIN
// aunque el R-Tree sea muy barato de recorrer: lo que se quiere minimizar no es
// el costo del indice sino el numero de filas que pasan al filtro.
//
// Es una heuristica, no una estimacion: no mira histogramas ni cardinalidades.
// Sustituirla por una estimacion real de selectividad es la mejora que el
// Experimento 3 del Entregable 1 ya dejaba identificada.
static int puntajeRuta(const TableInfo& ti, const std::string& clave_seq, const Predicate& p) {
    const IndexInfo* ix = ti.findIndex(p.column);
    const bool es_clave_seq = (!clave_seq.empty() && clave_seq == p.column);
    switch (p.kind) {
        case PredKind::EQ:                                  // ~1 fila
            if (ix && ix->kind == IndexKind::HASH)  return 0;
            if (ix && ix->kind == IndexKind::BPLUS) return 1;
            if (es_clave_seq)                       return 2;
            return 50;
        case PredKind::CONTIENE:                            // ~pocos poligonos
            return (ix && ix->kind == IndexKind::RTREE) ? 2 : 50;
        case PredKind::WITHIN:                              // area acotada
        case PredKind::RADIO:                               // circulo acotado
            return (ix && ix->kind == IndexKind::RTREE) ? 3 : 50;
        case PredKind::RANGE:                               // puede ser media tabla
            if (ix && ix->kind == IndexKind::BPLUS) return 4;
            if (es_clave_seq)                       return 5;
            return 50;
        case PredKind::NONE:
            return 99;
    }
    return 99;
}

// Columna que ordena fisicamente una tabla SEQUENTIAL: la PRIMARY KEY
// declarada o, si no se declaro ninguna, la primera columna.
static std::string claveSecuencialDe(const TableInfo& ti) {
    if (ti.engine != "SEQUENTIAL") return "";
    if (!ti.key_column.empty())    return ti.key_column;
    if (ti.schema.size() > 0)      return ti.schema[0].name;
    return "";
}

// ---------------------------------------------------------------------------
Database::Database(std::string data_dir, int pool_size)
    : data_dir_(std::move(data_dir)),
      pool_size_(pool_size),
      catalog_(unir(data_dir_, "catalog.txt")) {}

Table* Database::abrir(const std::string& nombre) {
    auto it = abiertas_.find(nombre);
    if (it != abiertas_.end()) return it->second.get();

    const TableInfo& ti = catalog_.get(nombre);
    auto t = std::make_unique<Table>(data_dir_, ti, pool_size_);
    Table* raw = t.get();
    abiertas_[nombre] = std::move(t);
    return raw;
}

void Database::cerrar(const std::string& nombre) { abiertas_.erase(nombre); }

// ---------------------------------------------------------------------------
void Database::flush() {
    for (auto& par : abiertas_) if (par.second) par.second->flush();
}
QueryResult Database::ejecutarStatement(const Statement& st) {
    QueryResult r;
    switch (st.kind) {
        case StmtKind::CREATE_TABLE: r = ejecutarCreateTable(st); break;
        case StmtKind::CREATE_INDEX: r = ejecutarCreateIndex(st); break;
        case StmtKind::INSERT:       r = ejecutarInsert(st);      break;
        case StmtKind::SELECT:       r = ejecutarSelect(st);      break;
        case StmtKind::DELETE_:      r = ejecutarDelete(st);      break;
    }
    if (st.kind == StmtKind::CREATE_TABLE || st.kind == StmtKind::CREATE_INDEX ||
        st.kind == StmtKind::INSERT       || st.kind == StmtKind::DELETE_) {
        flush();
    }
    return r;
}

std::vector<QueryResult> Database::executeMultiple(const std::string& sql) {
    std::vector<QueryResult> out;

    std::vector<Statement> sts;
    try {
        auto t_parse = Clock::now();
        sts = parseSQLMultiple(sql);
        double parse_total = msDesde(t_parse);
        // el tiempo de parseo se reparte informativamente entre sentencias
        for (auto& _ : sts) (void)_;
        (void)parse_total;
    } catch (const DBException& e) {
        QueryResult r; r.ok = false; r.error = e.what();
        out.push_back(r);
        return out;
    }

    for (const Statement& st : sts) {
        QueryResult r;
        auto t_ini = Clock::now();
        DiskCounter::Snapshot io0 = DiskCounter::global().snapshot();
        try {
            r = ejecutarStatement(st);
            r.ok = true;
        } catch (const DBException& e) {
            r.ok = false; r.error = e.what();
        } catch (const std::exception& e) {
            r.ok = false; r.error = std::string("Error interno: ") + e.what();
        }
        DiskCounter::Delta d = DiskCounter::global().since(io0);
        r.disk_reads = d.reads; r.disk_writes = d.writes;
        r.page_accesses = d.page_accesses; r.buffer_hits = d.buffer_hits;
        r.total_ms = msDesde(t_ini);
        out.push_back(r);

        // Si una sentencia falla a mitad del bloque, las siguientes igual se
        // intentan (semántica "best effort"); si prefieres abortar todo el
        // bloque al primer error, descomenta:
        // if (!r.ok) break;
    }
    return out;
}

QueryResult Database::execute(const std::string& sql) {
    QueryResult r;
    auto t_ini = Clock::now();
    DiskCounter::Snapshot io0 = DiskCounter::global().snapshot();

    try {
        auto t_parse = Clock::now();
        Statement st = parseSQL(sql);
        r.parse_ms = msDesde(t_parse);

        QueryResult x = ejecutarStatement(st);
        x.plan.insert(x.plan.begin(), PlanStep{"Parse SQL", r.parse_ms, ""});
        x.parse_ms = r.parse_ms;
        r = x;
        r.ok = true;
    } catch (const DBException& e) {
        r.ok = false; r.error = e.what();
    } catch (const std::exception& e) {
        r.ok = false; r.error = std::string("Error interno: ") + e.what();
    }

    DiskCounter::Delta d = DiskCounter::global().since(io0);
    r.disk_reads = d.reads; r.disk_writes = d.writes;
    r.page_accesses = d.page_accesses; r.buffer_hits = d.buffer_hits;
    r.total_ms = msDesde(t_ini);
    return r;
}

// ---------------------------------------------------------------------------
QueryResult Database::ejecutarCreateTable(const Statement& st) {
    QueryResult r;
    auto t0 = Clock::now();

    if (catalog_.exists(st.table)) throw DBException("La tabla '" + st.table + "' ya existe");

    std::vector<Column> cols;
    for (const ColumnDef& c : st.columns) cols.emplace_back(c.name, c.type, c.max_len);

    std::string pk;
    for (const ColumnDef& c : st.columns)
        if (c.primary_key) { pk = c.name; break; }
    if (pk.empty() && st.engine == EngineKind::SEQUENTIAL && !cols.empty())
        pk = cols.front().name;          // sin PK declarada: ordena por la primera columna

    TableInfo ti;
    ti.name       = st.table;
    ti.schema     = Schema(cols);
    ti.heap_file  = st.table + ".dat";
    ti.engine     = engineName(st.engine);
    ti.key_column = pk;
    catalog_.createTable(ti);

    abrir(st.table);                     // crea los archivos fisicos

    r.metodo  = "DDL";
    r.message = "Tabla '" + st.table + "' creada con motor " + ti.engine + " y " +
                std::to_string(cols.size()) + " columnas.";
    if (st.engine == EngineKind::SEQUENTIAL)
        r.message += " Ordenada por '" + pk + "'; las inserciones que no entren en su bloque "
                     "van al area de overflow.";
    r.exec_ms = msDesde(t0);
    r.plan.push_back(PlanStep{"CREATE TABLE", r.exec_ms, ti.engine});
    return r;
}

QueryResult Database::ejecutarCreateIndex(const Statement& st) {
    QueryResult r;
    auto t0 = Clock::now();

    const TableInfo& ti = catalog_.get(st.table);
    // Varios indices por tabla, uno por columna: lo normal es tener un B+ sobre
    // la clave primaria y, ademas, un R-Tree sobre la columna POINT.
    if (ti.findIndex(st.index_column))
        throw DBException("La columna '" + st.index_column + "' de '" + st.table +
                          "' ya tiene un indice. El motor admite un indice por columna.");
    int ci_ix = ti.schema.indexOf(st.index_column);
    if (ci_ix < 0)
        throw DBException("La columna '" + st.index_column + "' no existe en '" + st.table + "'");
    // VALIDAR ANTES DE TOCAR EL CATALOGO. Si se escribe el indice primero y
    // luego el constructor de Table rechaza el tipo, el catalogo queda con un
    // indice imposible y la tabla se vuelve inaccesible PARA SIEMPRE: como no
    // hay DROP INDEX, solo se recupera editando catalog.txt a mano.
    const Type tipo_ix = ti.schema[static_cast<std::size_t>(ci_ix)].type;
    if (st.index_kind == IndexKind::RTREE) {
        if (tipo_ix != Type::POINT && tipo_ix != Type::POLYGON)
            throw DBException("Un indice RTREE solo se crea sobre POINT o POLYGON: '" +
                              st.index_column + "' es " + typeName(tipo_ix) + ". La tabla queda intacta.");
    } else {
        if (tipo_ix == Type::DOUBLE)
            throw DBException("Aun no se indexan columnas DOUBLE: '" + st.index_column +
                              "'. La tabla queda intacta.");
        if (tipo_ix == Type::POINT || tipo_ix == Type::POLYGON)
            throw DBException("La columna '" + st.index_column + "' es " + typeName(tipo_ix) +
                              ": use USING RTREE. La tabla queda intacta.");
    }

    IndexInfo ix;
    ix.column = st.index_column;
    ix.kind   = st.index_kind;
    ix.file   = st.table + "_" + st.index_column + ".idx";
    catalog_.addIndex(st.table, ix);

    // Reabrir la tabla con el indice y poblarlo recorriendo el heap.
    cerrar(st.table);
    Table* t = abrir(st.table);
    t->buildIndex();

    r.metodo  = "DDL";
    r.message = "Indice '" + st.index_name + "' creado sobre " + st.table + "(" + st.index_column +
                ") usando " + indexKindName(st.index_kind) + ". " +
                std::to_string(t->count()) + " filas indexadas. La tabla tiene ahora " +
                std::to_string(t->numIndices()) + " indice(s).";
    r.exec_ms = msDesde(t0);
    r.plan.push_back(PlanStep{"CREATE INDEX", r.exec_ms, indexKindName(st.index_kind)});
    return r;
}

QueryResult Database::ejecutarInsert(const Statement& st) {
    QueryResult r;
    auto t0 = Clock::now();

    Table* t = abrir(st.table);
    const Schema& sch = t->schema();

    if (st.rows.empty())
        throw DBException("INSERT sin ninguna tupla en VALUES");

    long long insertadas = 0;
    std::string ultimo_rid;
    for (const std::vector<Value>& fila : st.rows) {
        if (fila.size() != sch.size())
            throw DBException("INSERT con " + std::to_string(fila.size()) + " valores pero la tabla "
                              "tiene " + std::to_string(sch.size()) + " columnas");

        Tuple tup;
        tup.values.reserve(sch.size());
        for (std::size_t i = 0; i < sch.size(); ++i)
            tup.values.push_back(coerce(fila[i], sch[i].type, sch[i].name));

        RID rid = t->insert(tup);
        ultimo_rid = rid.str();
        ++insertadas;
    }

    r.metodo    = "DML";
    r.row_count = insertadas;
    r.message   = std::to_string(insertadas) + " fila(s) insertada(s) en '" + st.table + "'" +
                  (insertadas == 1 ? " con RID " + ultimo_rid + "." : ".");
    r.exec_ms = msDesde(t0);
    r.plan.push_back(PlanStep{"INSERT", r.exec_ms,
                              std::to_string(insertadas) + " fila(s), ultimo RID " + ultimo_rid});
    return r;
}

// Reparte las condiciones del WHERE: devuelve la que conduce el acceso y deja
// en 'filtros' las que se aplicaran en memoria sobre las filas recuperadas.
static Predicate repartirCondiciones(const TableInfo& ti, const std::string& clave_seq,
                                     const Statement& st, std::vector<Predicate>* filtros) {
    filtros->clear();
    if (st.where.kind == PredKind::NONE) return st.where;

    std::vector<Predicate> todas;
    todas.push_back(st.where);
    for (const Predicate& p : st.extra) todas.push_back(p);

    std::size_t mejor = 0;
    int mejor_pts = puntajeRuta(ti, clave_seq, todas[0]);
    for (std::size_t i = 1; i < todas.size(); ++i) {
        int pts = puntajeRuta(ti, clave_seq, todas[i]);
        if (pts < mejor_pts) { mejor_pts = pts; mejor = i; }
    }
    for (std::size_t i = 0; i < todas.size(); ++i)
        if (i != mejor) filtros->push_back(todas[i]);
    return todas[mejor];
}

QueryResult Database::ejecutarSelect(const Statement& st) {
    QueryResult r;
    Table* t = abrir(st.table);
    const Schema& sch = t->schema();

    // ---- Planificacion: elegir la ruta de acceso ----
    auto t_plan = Clock::now();
    const TableInfo& ti = catalog_.get(st.table);
    std::vector<Predicate> filtros;
    const Predicate cond = repartirCondiciones(ti, claveSecuencialDe(ti), st, &filtros);
    std::string ruta, detalle_ruta;
    const IndexInfo* ix_knn = st.knn ? ti.findIndex(st.knn_column) : nullptr;
    if (st.knn) {
        // El KNN manda sobre el WHERE: es lo que decide la ruta de acceso.
        if (ix_knn && ix_knn->kind == IndexKind::RTREE && st.limit >= 0) {
            ruta = st.knn_geo ? "IndexKNN (geografico)" : "IndexKNN";
            detalle_ruta = std::string("R-Tree best-first sobre ") + st.knn_column +
                           ", k=" + std::to_string(st.limit) +
                           (st.knn_geo ? ", distancia haversine en metros"
                                       : ", distancia euclidiana en grados");
        } else if (st.limit < 0) {
            ruta = "SeqScan + orden";
            detalle_ruta = "ORDER BY por distancia sin LIMIT: hay que ordenar todo";
        } else {
            ruta = "SeqScan + orden parcial";
            detalle_ruta = "no hay indice RTREE sobre " + st.knn_column;
        }
    } else if (cond.kind == PredKind::NONE) {
        ruta = "SeqScan"; detalle_ruta = "sin predicado";
    } else if (cond.kind == PredKind::WITHIN || cond.kind == PredKind::RADIO ||
               cond.kind == PredKind::CONTIENE) {
        const IndexInfo* ix = ti.findIndex(cond.column);
        const char* nombre = (cond.kind == PredKind::WITHIN)   ? "IndexWindowScan"
                           : (cond.kind == PredKind::CONTIENE) ? "IndexContainsScan"
                                                               : "IndexRadiusScan";
        if (ix && ix->kind == IndexKind::RTREE) {
            ruta = nombre;
            detalle_ruta = "R-Tree sobre " + cond.column;
            if (cond.kind == PredKind::RADIO)
                detalle_ruta += ", radio de " + num0(cond.metros) + " m";
        } else {
            ruta = "SeqScan";
            detalle_ruta = "no hay indice RTREE sobre " + cond.column;
        }
    } else {
        const IndexInfo* ix = ti.findIndex(cond.column);
        const bool es_clave_seq = (claveSecuencialDe(ti) == cond.column);
        if (!ix && es_clave_seq) {
            ruta = (cond.kind == PredKind::EQ) ? "BinarySearch" : "BinarySearch (rango)";
            detalle_ruta = "archivo ordenado por " + cond.column + " + area de overflow";
        }
        else if (!ix) { ruta = "SeqScan"; detalle_ruta = "no hay indice sobre " + cond.column; }
        else if (cond.kind == PredKind::EQ) {
            ruta = "IndexScan";
            detalle_ruta = indexKindName(ix->kind) + " sobre " + cond.column;
        } else if (ix->kind == IndexKind::BPLUS) {
            ruta = "IndexRangeScan";
            detalle_ruta = "BPLUS sobre " + cond.column;
        } else {
            ruta = "SeqScan";
            detalle_ruta = "el indice HASH no ordena: un rango exige recorrido completo";
        }
    }
    if (st.knn && cond.kind != PredKind::NONE) {
        // El KNN conduce y TODO el WHERE queda como filtro, con sobre-peticion.
        std::string cols = cond.column;
        for (const Predicate& f : filtros) cols += ", " + f.column;
        detalle_ruta += "  [conduce la distancia; filtra " + cols +
                        " con sobre-peticion hasta reunir k]";
    } else if (!filtros.empty()) {
        // Plan hibrido: una condicion conduce el acceso y las demas se quedan
        // como filtro. Decir cual conduce y cuales no es justo lo que hace
        // legible la decision del planificador.
        std::string cols;
        for (const Predicate& f : filtros) { if (!cols.empty()) cols += ", "; cols += f.column; }
        detalle_ruta += "  [conduce " + cond.column + "; filtra " + cols + "]";
    }
    double plan_ms = msDesde(t_plan);
    r.plan.push_back(PlanStep{"Planificacion: " + ruta, plan_ms, detalle_ruta});

    // ---- Ejecucion ----
    auto t_exec = Clock::now();
    DiskCounter::Snapshot io_exec = DiskCounter::global().snapshot();
    std::vector<Tuple> filas;
    bool knn_ya_filtrado = false;
    if (st.knn) {
        int ck = sch.indexOf(st.knn_column);
        if (ck < 0) throw DBException("No existe la columna '" + st.knn_column + "'");
        if (sch[ck].type != Type::POINT)
            throw DBException("ORDER BY por distancia exige una columna POINT: '" +
                              st.knn_column + "' es " + typeName(sch[ck].type));
        const int k = st.limit >= 0 ? static_cast<int>(st.limit) : -1;

        // Un WHERE junto a un ORDER BY por distancia obliga a SOBRE-PEDIR. El
        // indice ordena por cercania, no sabe nada del predicado: si se le
        // piden k vecinos y despues se filtran, pueden quedar menos de k. Se
        // pide el doble cada vez hasta reunir k supervivientes o hasta que el
        // indice se agote. Sin esto, 'WHERE id > 100 ORDER BY ... LIMIT 3'
        // devolveria menos filas de las que existen, o --peor-- las de siempre.
        std::vector<Predicate> todas;
        if (cond.kind != PredKind::NONE) todas.push_back(cond);
        for (const Predicate& f : filtros) todas.push_back(f);

        auto vecinos = [&](int cuantos) {
            return st.knn_geo ? t->searchKNNGeo(st.knn_column, st.knn_x, st.knn_y, cuantos)
                              : t->searchKNN(st.knn_column, st.knn_x, st.knn_y, cuantos);
        };
        auto pasaTodas = [&](const Tuple& f) {
            for (const Predicate& w : todas) {
                int cf = sch.indexOf(w.column);
                if (cf < 0) throw DBException("No existe la columna '" + w.column + "'");
                if (!cumple(f.at(static_cast<std::size_t>(cf)), w)) return false;
            }
            return true;
        };

        if (todas.empty()) {
            filas = vecinos(k);
        } else {
            knn_ya_filtrado = true;
            int pedir = (k < 0) ? -1 : std::max(k, 1);
            while (true) {
                std::vector<Tuple> cand = vecinos(pedir);
                filas.clear();
                for (const Tuple& f : cand) if (pasaTodas(f)) filas.push_back(f);
                if (k < 0) break;                                   // orden total
                if (static_cast<int>(filas.size()) >= k) break;      // ya alcanzan
                if (static_cast<int>(cand.size()) < pedir) break;    // el indice se agoto
                if (pedir > 1 << 24) break;                          // tope de seguridad
                pedir *= 2;
            }
            if (k >= 0 && static_cast<int>(filas.size()) > k)
                filas.resize(static_cast<std::size_t>(k));
        }
    } else if (cond.kind == PredKind::NONE) {
        filas = t->scan();
    } else if (cond.kind == PredKind::WITHIN) {
        filas = t->searchWithin(cond.column, cond.wx0, cond.wy0, cond.wx1, cond.wy1);
    } else if (cond.kind == PredKind::CONTIENE) {
        filas = t->searchContains(cond.column, cond.qlon, cond.qlat);
    } else if (cond.kind == PredKind::RADIO) {
        filas = t->searchRadio(cond.column, cond.qlon, cond.qlat, cond.metros);
        if (cond.radio_estricto) {     // el indice resuelve <=; el < se afina aqui
            const int cr = sch.indexOf(cond.column);
            std::vector<Tuple> quedan;
            quedan.reserve(filas.size());
            for (const Tuple& f : filas)
                if (cumple(f.at(static_cast<std::size_t>(cr)), cond)) quedan.push_back(f);
            filas.swap(quedan);
        }
    } else {
        int ci = sch.indexOf(cond.column);
        if (ci < 0) throw DBException("No existe la columna '" + cond.column + "'");
        Type tipo = sch[ci].type;
        if (cond.kind == PredKind::EQ) {
            filas = t->searchEq(cond.column, coerce(cond.eq, tipo, cond.column));
        } else {
            Value lo = cond.lo_abierto ? cotaMin(tipo) : coerce(cond.lo, tipo, cond.column);
            Value hi = cond.hi_abierto ? cotaMax(tipo) : coerce(cond.hi, tipo, cond.column);
            filas = t->searchRange(cond.column, lo, hi);
            if (cond.lo_estricto || cond.hi_estricto) {
                std::vector<Tuple> filtradas;
                filtradas.reserve(filas.size());
                for (const Tuple& f : filas)
                    if (dentroDelPredicado(f.at(static_cast<std::size_t>(ci)), cond, lo, hi))
                        filtradas.push_back(f);
                filas.swap(filtradas);
            }
        }
    }

    // ---- Filtro residual: las condiciones que no condujeron el acceso ----
    long long antes_del_filtro = static_cast<long long>(filas.size());
    if (!filtros.empty() && !knn_ya_filtrado) {
        std::vector<Tuple> quedan;
        quedan.reserve(filas.size());
        for (const Tuple& f : filas) {
            bool ok = true;
            for (const Predicate& w : filtros) {
                int cf = sch.indexOf(w.column);
                if (cf < 0) throw DBException("No existe la columna '" + w.column + "'");
                if (!cumple(f.at(static_cast<std::size_t>(cf)), w)) { ok = false; break; }
            }
            if (ok) quedan.push_back(f);
        }
        filas.swap(quedan);
    }
    r.exec_ms = msDesde(t_exec);
    r.metodo  = t->lastPlan().metodo;
    DiskCounter::Delta dEjec = DiskCounter::global().since(io_exec);
    r.plan.push_back(PlanStep{"Ejecucion: " + r.metodo, r.exec_ms,
                              std::to_string(dEjec.page_accesses) + " accesos a pagina, " +
                              std::to_string(dEjec.reads) + " lecturas de disco"});
    if (t->lastPlan().descartados > 0) {
        // Visible a proposito: es la diferencia entre lo que el indice puede
        // prometer (cajas envolventes) y lo que la consulta realmente pide.
        r.plan.push_back(PlanStep{
            "Refinamiento geometrico", 0.0,
            std::to_string(t->lastPlan().descartados) +
            " candidato(s) del indice descartado(s) al comprobar la geometria real"});
    }
    if (!filtros.empty() && !knn_ya_filtrado) {
        r.plan.push_back(PlanStep{
            "Filtro residual", 0.0,
            std::to_string(antes_del_filtro) + " filas del indice -> " +
            std::to_string(filas.size()) + " tras aplicar " +
            std::to_string(filtros.size()) + " condicion(es) en memoria"});
    }

    // ---- Materializacion (proyeccion + limite + formato) ----
    auto t_mat = Clock::now();
    std::vector<int> proyeccion;
    if (st.select_columns.empty()) {
        for (std::size_t i = 0; i < sch.size(); ++i) { proyeccion.push_back(static_cast<int>(i)); r.columns.push_back(sch[i].name); }
    } else {
        for (const std::string& c : st.select_columns) {
            int ci = sch.indexOf(c);
            if (ci < 0) throw DBException("No existe la columna '" + c + "' en '" + st.table + "'");
            proyeccion.push_back(ci);
            r.columns.push_back(c);
        }
    }
    long long tope = (st.limit >= 0) ? st.limit : static_cast<long long>(filas.size());
    for (const Tuple& f : filas) {
        if (static_cast<long long>(r.rows.size()) >= tope) break;
        std::vector<std::string> fila;
        for (int ci : proyeccion) fila.push_back(f.at(static_cast<std::size_t>(ci)).str());
        r.rows.push_back(std::move(fila));
    }
    r.row_count = static_cast<long long>(r.rows.size());
    double mat_ms = msDesde(t_mat);
    r.plan.push_back(PlanStep{"Materializacion", mat_ms,
                              std::to_string(r.row_count) + " de " + std::to_string(filas.size()) + " filas"});
    return r;
}

QueryResult Database::ejecutarDelete(const Statement& st) {
    QueryResult r;
    Table* t = abrir(st.table);
    const Schema& sch = t->schema();
    const TableInfo& ti = catalog_.get(st.table);

    std::vector<Predicate> filtros;
    const Predicate cond = repartirCondiciones(ti, claveSecuencialDe(ti), st, &filtros);

    int ci = sch.indexOf(cond.column);
    if (ci < 0) throw DBException("No existe la columna '" + cond.column + "'");
    Type tipo = sch[ci].type;

    auto t_exec = Clock::now();
    std::vector<RID> objetivo;
    if (cond.kind == PredKind::WITHIN) {
        objetivo = t->searchRIDsWithin(cond.column, cond.wx0, cond.wy0, cond.wx1, cond.wy1);
    } else if (cond.kind == PredKind::CONTIENE) {
        objetivo = t->searchRIDsContains(cond.column, cond.qlon, cond.qlat);
    } else if (cond.kind == PredKind::RADIO) {
        objetivo = t->searchRIDsRadio(cond.column, cond.qlon, cond.qlat, cond.metros);
    } else if (cond.kind == PredKind::EQ) {
        objetivo = t->searchRIDsEq(cond.column, coerce(cond.eq, tipo, cond.column));
    } else {
        Value lo = cond.lo_abierto ? cotaMin(tipo) : coerce(cond.lo, tipo, cond.column);
        Value hi = cond.hi_abierto ? cotaMax(tipo) : coerce(cond.hi, tipo, cond.column);
        objetivo = t->searchRIDsRange(cond.column, lo, hi);
        if (cond.lo_estricto || cond.hi_estricto) {
            std::vector<RID> filtrados;
            filtrados.reserve(objetivo.size());
            Tuple f;
            for (const RID& rid : objetivo)
                if (t->getByRID(rid, f) &&
                    dentroDelPredicado(f.at(static_cast<std::size_t>(ci)), cond, lo, hi))
                    filtrados.push_back(rid);
            objetivo.swap(filtrados);
        }
    }
    r.metodo = t->lastPlan().metodo;

    // Las condiciones que no condujeron el acceso TIENEN que aplicarse antes
    // de borrar: olvidarlas aqui no devuelve filas de mas, borra filas de mas.
    const long long candidatas = static_cast<long long>(objetivo.size());
    if (!filtros.empty()) {
        std::vector<RID> quedan;
        quedan.reserve(objetivo.size());
        Tuple f;
        for (const RID& rid : objetivo) {
            if (!t->getByRID(rid, f)) continue;
            bool ok = true;
            for (const Predicate& w : filtros) {
                int cf = sch.indexOf(w.column);
                if (cf < 0) throw DBException("No existe la columna '" + w.column + "'");
                if (!cumple(f.at(static_cast<std::size_t>(cf)), w)) { ok = false; break; }
            }
            if (ok) quedan.push_back(rid);
        }
        objetivo.swap(quedan);
    }

    long long borradas = 0;
    for (const RID& rid : objetivo)
        if (t->removeByRID(rid)) ++borradas;

    r.exec_ms   = msDesde(t_exec);
    r.row_count = borradas;
    r.message   = std::to_string(borradas) + " fila(s) eliminada(s) de '" + st.table + "'.";
    r.plan.push_back(PlanStep{"Localizacion: " + r.metodo, r.exec_ms,
                              std::to_string(candidatas) + " candidatas" +
                              (filtros.empty() ? std::string()
                                               : ", " + std::to_string(objetivo.size()) +
                                                 " tras el filtro residual")});
    r.plan.push_back(PlanStep{"DELETE (heap + indice)", 0.0, std::to_string(borradas) + " borradas"});
    return r;
}

// ---------------------------------------------------------------------------
std::vector<TableSummary> Database::tables() {
    std::vector<TableSummary> out;
    for (const std::string& nombre : catalog_.tableNames()) {
        const TableInfo& ti = catalog_.get(nombre);
        TableSummary s;
        s.name   = ti.name;
        s.engine = ti.engine;
        for (const Column& c : ti.schema.columns()) {
            std::string tn = typeName(c.type);
            if (c.type == Type::VARCHAR) tn += "(" + std::to_string(c.max_len) + ")";
            s.columns.push_back(TableSummary::ColSummary{c.name, tn});
        }
        for (const IndexInfo& ix : ti.indexes)
            s.indexes.push_back(TableSummary::IdxSummary{ix.column, indexKindName(ix.kind)});
        try {
            Table* t = abrir(nombre);
            s.rows            = static_cast<long long>(t->count());
            s.heap_pages      = t->dataPages();
            s.index_pages     = t->indexPages();
            s.overflow_pages  = t->overflowPages();
            s.overflow_rows   = t->overflowRecords();
        } catch (const DBException&) { /* tabla ilegible: se reporta sin metricas */ }
        out.push_back(s);
    }
    return out;
}

QueryResult Database::reorganize(const std::string& tabla) {
    QueryResult r;
    auto t_ini = Clock::now();
    DiskCounter::Snapshot io0 = DiskCounter::global().snapshot();

    try {
        const TableInfo& ti = catalog_.get(tabla);
        if (ti.engine != "SEQUENTIAL")
            throw DBException("reorganize() solo aplica al motor SEQUENTIAL; '" + tabla +
                              "' usa " + ti.engine + ".");

        Table* t = abrir(tabla);
        int  pag_antes = t->dataPages();
        int  ovf_antes = t->overflowPages();
        long rec_antes = static_cast<long>(t->overflowRecords());
        long long filas = static_cast<long long>(t->count());

        auto t0 = Clock::now();
        if (!t->reorganize()) throw DBException("La tabla no expone un area secuencial");
        r.exec_ms = msDesde(t0);

        r.ok      = true;
        r.metodo  = "REORGANIZE";
        r.message = "Tabla '" + tabla + "' reorganizada: " + std::to_string(filas) +
                    " filas fusionadas. Paginas " + std::to_string(pag_antes) + " -> " +
                    std::to_string(t->dataPages()) + ", overflow " + std::to_string(ovf_antes) +
                    " paginas con " + std::to_string(rec_antes) + " registros -> " +
                    std::to_string(t->overflowPages()) + " paginas.";
        r.plan.push_back(PlanStep{"REORGANIZE", r.exec_ms,
                                  "fusion ordenada de area principal y overflow"});
    } catch (const DBException& e) {
        r.ok    = false;
        r.error = e.what();
    }

    DiskCounter::Delta d = DiskCounter::global().since(io0);
    r.disk_reads    = d.reads;
    r.disk_writes   = d.writes;
    r.page_accesses = d.page_accesses;
    r.buffer_hits   = d.buffer_hits;
    r.total_ms      = msDesde(t_ini);
    return r;
}

}  // namespace db
