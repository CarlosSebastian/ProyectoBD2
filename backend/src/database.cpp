#include "db/database.hpp"

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

static Value cotaMin(Type t) {
    switch (t) {
        case Type::INT:     return Value::makeInt(std::numeric_limits<std::int64_t>::min());
        case Type::DOUBLE:  return Value::makeDouble(-std::numeric_limits<double>::infinity());
        case Type::VARCHAR: return Value::makeStr("");
    }
    return Value::makeInt(0);
}
static Value cotaMax(Type t) {
    switch (t) {
        case Type::INT:     return Value::makeInt(std::numeric_limits<std::int64_t>::max());
        case Type::DOUBLE:  return Value::makeDouble(std::numeric_limits<double>::infinity());
        case Type::VARCHAR: return Value::makeStr(std::string(31, '\x7f'));
    }
    return Value::makeInt(0);
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
    if (!ti.indexes.empty())
        throw DBException("La tabla '" + st.table + "' ya tiene un indice sobre '" +
                          ti.indexes.front().column + "'. El motor admite un indice por tabla "
                          "en esta version.");
    int ci_ix = ti.schema.indexOf(st.index_column);
    if (ci_ix < 0)
        throw DBException("La columna '" + st.index_column + "' no existe en '" + st.table + "'");
    // VALIDAR ANTES DE TOCAR EL CATALOGO. Si se escribe el indice primero y
    // luego el constructor de Table rechaza el tipo, el catalogo queda con un
    // indice imposible y la tabla se vuelve inaccesible PARA SIEMPRE: como no
    // hay DROP INDEX, solo se recupera editando catalog.txt a mano.
    if (ti.schema[static_cast<std::size_t>(ci_ix)].type == Type::DOUBLE)
        throw DBException("Aun no se indexan columnas DOUBLE: '" + st.index_column +
                          "'. La tabla queda intacta.");

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
                std::to_string(t->count()) + " filas indexadas.";
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

QueryResult Database::ejecutarSelect(const Statement& st) {
    QueryResult r;
    Table* t = abrir(st.table);
    const Schema& sch = t->schema();

    // ---- Planificacion: elegir la ruta de acceso ----
    auto t_plan = Clock::now();
    const TableInfo& ti = catalog_.get(st.table);
    std::string ruta, detalle_ruta;
    if (st.where.kind == PredKind::NONE) {
        ruta = "SeqScan"; detalle_ruta = "sin predicado";
    } else {
        const IndexInfo* ix = ti.findIndex(st.where.column);
        const bool es_clave_seq = (claveSecuencialDe(ti) == st.where.column);
        if (!ix && es_clave_seq) {
            ruta = (st.where.kind == PredKind::EQ) ? "BinarySearch" : "BinarySearch (rango)";
            detalle_ruta = "archivo ordenado por " + st.where.column + " + area de overflow";
        }
        else if (!ix) { ruta = "SeqScan"; detalle_ruta = "no hay indice sobre " + st.where.column; }
        else if (st.where.kind == PredKind::EQ) {
            ruta = "IndexScan";
            detalle_ruta = indexKindName(ix->kind) + " sobre " + st.where.column;
        } else if (ix->kind == IndexKind::BPLUS) {
            ruta = "IndexRangeScan";
            detalle_ruta = "BPLUS sobre " + st.where.column;
        } else {
            ruta = "SeqScan";
            detalle_ruta = "el indice HASH no ordena: un rango exige recorrido completo";
        }
    }
    double plan_ms = msDesde(t_plan);
    r.plan.push_back(PlanStep{"Planificacion: " + ruta, plan_ms, detalle_ruta});

    // ---- Ejecucion ----
    auto t_exec = Clock::now();
    DiskCounter::Snapshot io_exec = DiskCounter::global().snapshot();
    std::vector<Tuple> filas;
    if (st.where.kind == PredKind::NONE) {
        filas = t->scan();
    } else {
        int ci = sch.indexOf(st.where.column);
        if (ci < 0) throw DBException("No existe la columna '" + st.where.column + "'");
        Type tipo = sch[ci].type;
        if (st.where.kind == PredKind::EQ) {
            filas = t->searchEq(st.where.column, coerce(st.where.eq, tipo, st.where.column));
        } else {
            Value lo = st.where.lo_abierto ? cotaMin(tipo) : coerce(st.where.lo, tipo, st.where.column);
            Value hi = st.where.hi_abierto ? cotaMax(tipo) : coerce(st.where.hi, tipo, st.where.column);
            filas = t->searchRange(st.where.column, lo, hi);
            if (st.where.lo_estricto || st.where.hi_estricto) {
                std::vector<Tuple> filtradas;
                filtradas.reserve(filas.size());
                for (const Tuple& f : filas)
                    if (dentroDelPredicado(f.at(static_cast<std::size_t>(ci)), st.where, lo, hi))
                        filtradas.push_back(f);
                filas.swap(filtradas);
            }
        }
    }
    r.exec_ms = msDesde(t_exec);
    r.metodo  = t->lastPlan().metodo;
    DiskCounter::Delta dEjec = DiskCounter::global().since(io_exec);
    r.plan.push_back(PlanStep{"Ejecucion: " + r.metodo, r.exec_ms,
                              std::to_string(dEjec.page_accesses) + " accesos a pagina, " +
                              std::to_string(dEjec.reads) + " lecturas de disco"});

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

    int ci = sch.indexOf(st.where.column);
    if (ci < 0) throw DBException("No existe la columna '" + st.where.column + "'");
    Type tipo = sch[ci].type;

    auto t_exec = Clock::now();
    std::vector<RID> objetivo;
    if (st.where.kind == PredKind::EQ) {
        objetivo = t->searchRIDsEq(st.where.column, coerce(st.where.eq, tipo, st.where.column));
    } else {
        Value lo = st.where.lo_abierto ? cotaMin(tipo) : coerce(st.where.lo, tipo, st.where.column);
        Value hi = st.where.hi_abierto ? cotaMax(tipo) : coerce(st.where.hi, tipo, st.where.column);
        objetivo = t->searchRIDsRange(st.where.column, lo, hi);
        if (st.where.lo_estricto || st.where.hi_estricto) {
            std::vector<RID> filtrados;
            filtrados.reserve(objetivo.size());
            Tuple f;
            for (const RID& rid : objetivo)
                if (t->getByRID(rid, f) &&
                    dentroDelPredicado(f.at(static_cast<std::size_t>(ci)), st.where, lo, hi))
                    filtrados.push_back(rid);
            objetivo.swap(filtrados);
        }
    }
    r.metodo = t->lastPlan().metodo;

    long long borradas = 0;
    for (const RID& rid : objetivo)
        if (t->removeByRID(rid)) ++borradas;

    r.exec_ms   = msDesde(t_exec);
    r.row_count = borradas;
    r.message   = std::to_string(borradas) + " fila(s) eliminada(s) de '" + st.table + "'.";
    r.plan.push_back(PlanStep{"Localizacion: " + r.metodo, r.exec_ms,
                              std::to_string(objetivo.size()) + " candidatas"});
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
