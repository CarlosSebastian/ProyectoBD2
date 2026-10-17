#include "db/database.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>
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
    // Un vector tampoco: no hay conversion sensata entre un embedding y un
    // numero o un texto, y convertirlo "por si acaso" esconderia el error de
    // haber escrito un literal del tipo equivocado.
    if (destino == Type::VECTOR || v.type == Type::VECTOR)
        throw DBException("La columna '" + col + "' es de tipo " + typeName(destino) +
                          " y se recibio un valor " + typeName(v.type) +
                          ". Los vectores se escriben entre corchetes: [0.1, -0.5, ...].");
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
        // Un vector no tiene cota: no se recorre por rango. El caso existe para
        // que el switch quede exhaustivo y el compilador avise si manana se
        // anade otro tipo.
        case Type::VECTOR:  return Value::makeVector({});
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
        case Type::VECTOR:  return Value::makeVector({});
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
        case PredKind::MATCH:
            // MATCH no es una condicion de si/no sino un ORDEN por relevancia:
            // no se puede comprobar sobre una fila suelta. El planificador se
            // encarga de que siempre sea el que conduce el acceso, asi que
            // aqui nunca llega como filtro residual.
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
        case PredKind::VECDIST: {
            // Esta SI se puede comprobar fila a fila: la distancia se calcula
            // con la fila ya en memoria, sin tocar el indice. Es lo que permite
            // que un umbral vectorial funcione como filtro residual y que el
            // DELETE sea exacto aunque el IVF sea aproximado.
            if (v.type != Type::VECTOR) return false;
            if (v.vec.size() != w.vector_consulta.size()) return false;
            const double d = vecdist::entre(v.vec, w.vector_consulta, w.vec_coseno);
            return w.vec_estricto ? (d < w.umbral) : (d <= w.umbral);
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
        case PredKind::MATCH:
            // No compite: un MATCH no se puede evaluar fila a fila, asi que
            // tiene que ser el que conduzca. Si hubiera dos, el planificador
            // lo rechaza antes de llegar aqui.
            return -1;
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
        case PredKind::VECDIST:                             // vecinos dentro de un radio
            // Con IVF es la ruta mas barata que existe para esta condicion;
            // sin IVF no hay nada que hacer salvo recorrer y calcular la
            // distancia de cada fila, igual de caro que un scan.
            return (ix && ix->kind == IndexKind::IVF) ? 3 : 50;
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
        case StmtKind::COPY:         r = ejecutarCopy(st);        break;
    }
    if (st.kind == StmtKind::CREATE_TABLE || st.kind == StmtKind::CREATE_INDEX ||
        st.kind == StmtKind::INSERT       || st.kind == StmtKind::DELETE_    ||
        st.kind == StmtKind::COPY) {
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
    if (st.index_kind == IndexKind::INVERTED) {
        if (tipo_ix != Type::VARCHAR)
            throw DBException("Un indice INVERTED solo se crea sobre texto (CHAR/VARCHAR/TEXT): '" +
                              st.index_column + "' es " + typeName(tipo_ix) + ". La tabla queda intacta.");
        texto::idiomaDesdeNombre(st.index_idioma);   // valida antes de tocar nada
    } else if (st.index_kind == IndexKind::IVF) {
        if (tipo_ix != Type::VECTOR)
            throw DBException("Un indice IVF solo se crea sobre VECTOR: '" +
                              st.index_column + "' es " + typeName(tipo_ix) +
                              ". La tabla queda intacta.");
        if (ti.schema[static_cast<std::size_t>(ci_ix)].max_len <= 0)
            throw DBException("La columna '" + st.index_column +
                              "' no declara dimension: declarala como VECTOR(d). "
                              "La tabla queda intacta.");
        IVFIndex::metricaDesdeNombre(st.index_metrica);   // valida antes de tocar nada
    } else if (st.index_kind == IndexKind::RTREE) {
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
        if (tipo_ix == Type::VECTOR)
            throw DBException("La columna '" + st.index_column +
                              "' es VECTOR: use USING IVF. La tabla queda intacta.");
    }

    IndexInfo ix;
    ix.column = st.index_column;
    ix.kind   = st.index_kind;
    ix.file   = st.table + "_" + st.index_column + ".idx";
    if (st.index_kind == IndexKind::INVERTED)
        ix.idioma = texto::nombreIdioma(texto::idiomaDesdeNombre(st.index_idioma));
    if (st.index_kind == IndexKind::IVF) {
        ix.listas  = st.index_listas;          // 0 => que el IVF use sqrt(N)
        ix.metrica = IVFIndex::nombreMetrica(IVFIndex::metricaDesdeNombre(st.index_metrica));
    }
    catalog_.addIndex(st.table, ix);

    // Reabrir la tabla con el indice y poblarlo recorriendo el heap.
    cerrar(st.table);
    Table* t = abrir(st.table);
    t->buildIndex();

    r.metodo  = "DDL";
    r.message = "Indice '" + st.index_name + "' creado sobre " + st.table + "(" + st.index_column +
                ") usando " + indexKindName(st.index_kind) +
                (st.index_kind == IndexKind::INVERTED ? " (idioma " + ix.idioma + ")" : "") + ". " +
                std::to_string(t->count()) + " filas indexadas. La tabla tiene ahora " +
                std::to_string(t->numIndices()) + " indice(s).";
    if (st.index_kind == IndexKind::IVF) {
        const IVFIndex* iv = t->indiceVector(st.index_column);
        if (iv) {
            // Cuantas celdas salieron y como quedo el reparto es LA informacion
            // util de un IVF recien construido: una celda gigante es una lectura
            // larga que ninguna consulta va a poder evitar.
            const std::vector<std::uint32_t> tam = iv->tamanosDeListas();
            std::uint32_t mayor = 0;
            for (std::uint32_t c : tam) if (c > mayor) mayor = c;
            const double media = tam.empty() ? 0.0
                               : static_cast<double>(iv->size()) / static_cast<double>(tam.size());
            r.message += " Metrica " + IVFIndex::nombreMetrica(iv->metrica()) + ", " +
                         std::to_string(iv->listas()) + " celdas (" + num0(media) +
                         " vectores de media, la mayor " + std::to_string(mayor) +
                         "), nprobe por defecto " + std::to_string(iv->nprobePorDefecto()) +
                         ", " + std::to_string(iv->numPages()) + " paginas de indice.";
        }
    }
    r.exec_ms = msDesde(t0);
    r.plan.push_back(PlanStep{"CREATE INDEX", r.exec_ms, indexKindName(st.index_kind)});
    return r;
}

// ---------------------------------------------------------------------------
//  COPY: carga masiva desde un CSV
//
//  Lector de CSV al estilo RFC 4180, que es lo minimo para datos de verdad: un
//  abstract lleva comas, comillas y a veces saltos de linea, asi que partir por
//  comas a secas destroza el archivo. Dentro de un campo entrecomillado, dos
//  comillas seguidas son una comilla literal.
// ---------------------------------------------------------------------------
namespace {
// Lee una fila completa (puede abarcar varias lineas fisicas). Devuelve false
// al llegar al final del archivo.
bool leerFilaCSV(std::istream& in, char delim, std::vector<std::string>* campos) {
    campos->clear();
    std::string campo;
    bool dentro_de_comillas = false;
    bool algo = false;
    int c;
    while ((c = in.get()) != EOF) {
        algo = true;
        const char ch = static_cast<char>(c);
        if (dentro_de_comillas) {
            if (ch == '"') {
                if (in.peek() == '"') { in.get(); campo += '"'; }   // comilla escapada
                else                    dentro_de_comillas = false;
            } else {
                campo += ch;
            }
            continue;
        }
        if (ch == '"' && campo.empty()) { dentro_de_comillas = true; continue; }
        if (ch == delim) { campos->push_back(campo); campo.clear(); continue; }
        if (ch == '\n') { campos->push_back(campo); return true; }
        if (ch == '\r') continue;                                   // CRLF de Windows
        campo += ch;
    }
    if (!algo) return false;
    campos->push_back(campo);
    return true;
}

// Convierte el texto plano del CSV al tipo que pide la columna.
Value valorDesdeTexto(const std::string& s, Type t, const std::string& col, long long linea) {
    try {
        switch (t) {
            case Type::INT:     return Value::makeInt(s.empty() ? 0 : std::stoll(s));
            case Type::DOUBLE:  return Value::makeDouble(s.empty() ? 0.0 : std::stod(s));
            case Type::VARCHAR: return Value::makeStr(s);
            case Type::POINT: {
                // "x y" o "x,y" (la coma solo si el campo venia entrecomillado)
                std::size_t corte = s.find_first_of(" ,;");
                if (corte == std::string::npos)
                    throw DBException("se esperaban dos coordenadas");
                return Value::makePoint(std::stod(s.substr(0, corte)),
                                        std::stod(s.substr(corte + 1)));
            }
            case Type::VECTOR: {
                // El CSV trae el embedding como "[0.1,0.2,...]" o como
                // "0.1 0.2 ...": se aceptan las dos formas porque el script que
                // los genera puede volcarlos de cualquiera de las dos maneras.
                // Es por aqui por donde entran los embeddings de verdad.
                std::string limpio = s;
                for (char& c : limpio)
                    if (c == '[' || c == ']' || c == ',' || c == ';') c = ' ';
                std::vector<float> v;
                std::istringstream ss(limpio);
                double x;
                while (ss >> x) v.push_back(static_cast<float>(x));
                if (v.empty())
                    throw DBException("un vector necesita al menos una componente");
                return Value::makeVector(std::move(v));
            }
            case Type::POLYGON: {
                std::vector<double> v;
                std::istringstream ss(s);
                double x;
                while (ss >> x) { v.push_back(x); if (ss.peek() == ',' || ss.peek() == ';') ss.get(); }
                if (v.size() < 6 || v.size() % 2 != 0)
                    throw DBException("un poligono necesita al menos tres pares de coordenadas");
                return Value::makePolygon(v);
            }
        }
    } catch (const DBException&) {
        throw;
    } catch (const std::exception&) {
        throw DBException("Linea " + std::to_string(linea) + ": no se pudo leer '" + s +
                          "' como " + typeName(t) + " para la columna '" + col + "'");
    }
    return Value::makeInt(0);
}
}  // namespace

QueryResult Database::ejecutarCopy(const Statement& st) {
    QueryResult r;
    auto t0 = Clock::now();

    Table* t = abrir(st.table);
    const Schema& sch = t->schema();

    std::ifstream in(st.copy_file, std::ios::binary);
    if (!in)
        throw DBException("No se pudo abrir el archivo '" + st.copy_file +
                          "'. La ruta se resuelve desde el directorio donde corre el servidor.");

    std::vector<std::string> campos;
    long long linea = 0, insertadas = 0, saltadas = 0;
    std::string primer_error;

    if (st.copy_header) { leerFilaCSV(in, st.copy_delim, &campos); ++linea; }

    while (leerFilaCSV(in, st.copy_delim, &campos)) {
        ++linea;
        // Una linea en blanco al final del archivo es normal; no es un error.
        if (campos.size() == 1 && campos[0].empty()) continue;
        if (campos.size() != sch.size()) {
            ++saltadas;
            if (primer_error.empty())
                primer_error = "linea " + std::to_string(linea) + ": " +
                               std::to_string(campos.size()) + " campos y la tabla tiene " +
                               std::to_string(sch.size());
            continue;
        }
        Tuple tup;
        tup.values.reserve(sch.size());
        try {
            for (std::size_t i = 0; i < sch.size(); ++i)
                tup.values.push_back(valorDesdeTexto(campos[i], sch[i].type, sch[i].name, linea));
        } catch (const DBException& e) {
            ++saltadas;
            if (primer_error.empty()) primer_error = e.what();
            continue;
        }
        t->insert(tup);
        ++insertadas;
    }

    r.metodo  = "COPY";
    r.message = std::to_string(insertadas) + " fila(s) cargada(s) en '" + st.table +
                "' desde " + st.copy_file;
    if (saltadas > 0)
        r.message += ". " + std::to_string(saltadas) + " linea(s) descartada(s) (" +
                     primer_error + ")";
    r.exec_ms = msDesde(t0);
    r.plan.push_back(PlanStep{"COPY", r.exec_ms,
                              std::to_string(insertadas) + " filas insertadas una a una; "
                              "los indices se mantienen al dia salvo el invertido, que se "
                              "reconstruye en la primera consulta MATCH"});
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

    // Dos MATCH en el mismo WHERE no se pueden resolver: uno conduciria y el
    // otro quedaria como filtro, pero un filtro de relevancia no existe. Mejor
    // decirlo que devolver filas de mas en silencio.
    int n_match = 0;
    for (const Predicate& p : todas) if (p.kind == PredKind::MATCH) ++n_match;
    if (n_match > 1)
        throw DBException("Solo se admite un MATCH por consulta: el ranking textual "
                          "conduce el acceso y no puede aplicarse como filtro");
    if (n_match == 1 && (st.knn || st.knn_vectorial))
        throw DBException("MATCH y ORDER BY por distancia piden dos ordenes distintos "
                          "en la misma consulta");

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
    Predicate cond = repartirCondiciones(ti, claveSecuencialDe(ti), st, &filtros);

    // La metrica NO se deduce del indice: la pide el operador (<=> coseno,
    // <-> L2). Si no coincide con la del indice, la consulta se rechaza en vez
    // de responder con la distancia equivocada, que es un error que nadie
    // notaria mirando el resultado.
    const IndexInfo* ix_vec = st.knn_vectorial ? ti.findIndex(st.knn_column) : nullptr;
    const bool knn_cos = st.knn_coseno;

    // Texto de una sola linea con el reparto de las celdas del IVF. Un reparto
    // muy desigual es la explicacion de por que dos consultas con el mismo
    // nprobe pueden costar muy distinto.
    auto detalleIVF = [&](const IVFIndex* iv, int probes_pedidos) {
        std::string d;
        if (!iv) return d;
        const std::vector<std::uint32_t> tam = iv->tamanosDeListas();
        std::uint32_t mayor = 0;
        for (std::uint32_t c : tam) if (c > mayor) mayor = c;
        const int np = probes_pedidos > 0 ? probes_pedidos : iv->nprobePorDefecto();
        d = "metrica " + IVFIndex::nombreMetrica(iv->metrica()) +
            ", nprobe=" + std::to_string(np) + "/" + std::to_string(iv->listas()) +
            " celdas, " + std::to_string(iv->size()) + " vectores (la celda mayor tiene " +
            std::to_string(mayor) + "); resultado APROXIMADO: solo se compara contra las "
            "celdas visitadas";
        return d;
    };

    std::string ruta, detalle_ruta;
    const IndexInfo* ix_knn = st.knn ? ti.findIndex(st.knn_column) : nullptr;
    if (st.knn_vectorial) {
        // La similitud manda sobre el WHERE, igual que el KNN espacial. Y a
        // diferencia del KNN espacial, aqui NO hay plan B: recorrer la tabla
        // comparando 384 floats por fila es justo lo que el indice existe para
        // evitar, asi que el motor prefiere pedir el indice antes que ofrecer
        // una consulta que no escala.
        if (!ix_vec || ix_vec->kind != IndexKind::IVF)
            throw DBException("ORDER BY por similitud necesita un indice IVF sobre '" +
                              st.knn_column + "'. Crealo con: CREATE INDEX ix ON " + st.table +
                              " (" + st.knn_column + ") USING IVF LISTS n METRIC " +
                              std::string(st.knn_coseno ? "COSINE" : "L2") + ";");
        ruta = "IndexVectorKNN";
        detalle_ruta = "IVF sobre " + st.knn_column + ", k=" +
                       (st.limit >= 0 ? std::to_string(st.limit) : std::string("todos")) +
                       ", " + detalleIVF(t->indiceVector(st.knn_column), st.probes);
        if (t->vectorDesactualizado())
            detalle_ruta += "; el indice se reconstruira antes de responder";
    } else if (st.knn) {
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
    } else if (cond.kind == PredKind::MATCH) {
        const IndexInfo* ix = ti.findIndex(cond.column);
        if (!ix || ix->kind != IndexKind::INVERTED)
            throw DBException("MATCH necesita un indice INVERTED sobre '" + cond.column +
                              "'. Crealo con: CREATE INDEX ix ON " + st.table +
                              " (" + cond.column + ") USING INVERTED;");
        ruta = "IndexMatch";
        const InvertedIndex* inv = t->indiceTexto(cond.column);
        detalle_ruta = std::string("indice invertido sobre ") + cond.column +
                       ", ranking " + (cond.usar_tfidf ? "TF-IDF (coseno)" : "BM25") +
                       ", idioma " + ix->idioma;
        if (inv) {
            detalle_ruta += ", " + std::to_string(inv->numTerminos()) + " terminos sobre " +
                            std::to_string(inv->numDocs()) + " documentos";
            // Cuantos documentos toca realmente cada termino de la consulta:
            // es la medida de cuanto trabajo se ahorra frente a leerlos todos.
            std::string dfs;
            for (const std::string& term : inv->analizar(cond.consulta)) {
                if (!dfs.empty()) dfs += ", ";
                dfs += term + " (df=" + std::to_string(inv->df(term)) + ")";
            }
            if (!dfs.empty()) detalle_ruta += "; terminos: " + dfs;
            else              detalle_ruta += "; la consulta no deja ningun termino tras el analisis";
        }
    } else if (cond.kind == PredKind::VECDIST) {
        const IndexInfo* iv = ti.findIndex(cond.column);
        if (iv && iv->kind == IndexKind::IVF) {
            ruta = "IndexVectorRange";
            detalle_ruta = "IVF sobre " + cond.column + ", umbral " +
                           (cond.vec_estricto ? "< " : "<= ") + std::to_string(cond.umbral) +
                           ", " + detalleIVF(t->indiceVector(cond.column), st.probes) +
                           "; se pide top-k y se dobla k hasta cubrir el umbral";
        } else {
            ruta = "SeqScan";
            detalle_ruta = "no hay indice IVF sobre " + cond.column +
                           ": la distancia se calcula fila a fila (exacto, O(N))";
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
    if (cond.kind == PredKind::MATCH && !filtros.empty()) {
        std::string cols;
        for (const Predicate& f : filtros) { if (!cols.empty()) cols += ", "; cols += f.column; }
        detalle_ruta += "  [conduce la relevancia; filtra " + cols +
                        " con sobre-peticion hasta reunir k]";
    } else if (st.knn_vectorial && cond.kind != PredKind::NONE) {
        std::string cols = cond.column;
        for (const Predicate& f : filtros) cols += ", " + f.column;
        detalle_ruta += "  [conduce la similitud; filtra " + cols +
                        " con sobre-peticion hasta reunir k]";
    } else if (st.knn && cond.kind != PredKind::NONE) {
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
    // Paralelo a 'filas' cuando la consulta es por relevancia: el puntaje no
    // es una columna de la tabla, pero sin el no se puede juzgar el ranking.
    std::vector<double> puntajes;
    // Como se llama esa columna y con cuanta precision se imprime: un puntaje
    // BM25 no es lo mismo que una distancia, y llamar "score" a una distancia
    // invitaria a leerla al reves (cuanto mas alto, mejor), que es justo lo
    // contrario de lo que significa.
    const char* columna_extra = "score";
    bool extra_es_distancia = false;
    bool knn_ya_filtrado = false;
    if (cond.kind == PredKind::MATCH) {
        const int k = st.limit >= 0 ? static_cast<int>(st.limit) : -1;

        auto pasaFiltros = [&](const Tuple& f) {
            for (const Predicate& w : filtros) {
                const int cf = sch.indexOf(w.column);
                if (cf < 0) throw DBException("No existe la columna '" + w.column + "'");
                if (!cumple(f.at(static_cast<std::size_t>(cf)), w)) return false;
            }
            return true;
        };

        if (filtros.empty()) {
            filas = t->searchMatch(cond.column, cond.consulta, k, cond.usar_tfidf, &puntajes);
        } else {
            // Mismo problema que el KNN: el indice ordena por relevancia y no
            // sabe nada del resto del WHERE. Si se piden k y despues se
            // filtran, pueden quedar menos de k. Se duplica la peticion hasta
            // reunir k supervivientes o hasta agotar el indice.
            knn_ya_filtrado = true;
            int pedir = (k < 0) ? -1 : std::max(k, 1);
            while (true) {
                std::vector<double> ps;
                std::vector<Tuple> cand =
                    t->searchMatch(cond.column, cond.consulta, pedir, cond.usar_tfidf, &ps);
                filas.clear();
                puntajes.clear();
                for (std::size_t i = 0; i < cand.size(); ++i) {
                    if (!pasaFiltros(cand[i])) continue;
                    filas.push_back(cand[i]);
                    puntajes.push_back(i < ps.size() ? ps[i] : 0.0);
                }
                if (k < 0) break;
                if (static_cast<int>(filas.size()) >= k) break;
                if (static_cast<int>(cand.size()) < pedir) break;
                if (pedir > 1 << 20) break;
                pedir *= 2;
            }
            if (k >= 0 && static_cast<int>(filas.size()) > k) {
                filas.resize(static_cast<std::size_t>(k));
                puntajes.resize(static_cast<std::size_t>(k));
            }
        }
    } else if (st.knn_vectorial) {
        const int ck = sch.indexOf(st.knn_column);
        if (ck < 0) throw DBException("No existe la columna '" + st.knn_column + "'");
        if (sch[ck].type != Type::VECTOR)
            throw DBException("ORDER BY <=> exige una columna VECTOR: '" + st.knn_column +
                              "' es " + typeName(sch[ck].type));
        if (static_cast<int>(st.knn_vector.size()) != sch[ck].max_len)
            throw DBException("El vector de la consulta tiene " +
                              std::to_string(st.knn_vector.size()) + " componentes y '" +
                              st.knn_column + "' es VECTOR(" +
                              std::to_string(sch[ck].max_len) + ")");
        columna_extra     = "distancia";
        extra_es_distancia = true;

        const int k = st.limit >= 0 ? static_cast<int>(st.limit) : -1;

        std::vector<Predicate> todas;
        if (cond.kind != PredKind::NONE) todas.push_back(cond);
        for (const Predicate& f : filtros) todas.push_back(f);
        auto pasaTodas = [&](const Tuple& f) {
            for (const Predicate& w : todas) {
                const int cf = sch.indexOf(w.column);
                if (cf < 0) throw DBException("No existe la columna '" + w.column + "'");
                if (!cumple(f.at(static_cast<std::size_t>(cf)), w)) return false;
            }
            return true;
        };

        {
            // Misma sobre-peticion que en el KNN espacial: el indice ordena por
            // similitud y no sabe nada del WHERE, asi que se dobla k hasta
            // reunir k supervivientes o hasta agotar el indice.
            if (todas.empty()) {
                filas = t->searchVector(st.knn_column, st.knn_vector, k, knn_cos,
                                        st.probes, &puntajes);
            } else {
                knn_ya_filtrado = true;
                int pedir = (k < 0) ? -1 : std::max(k, 1);
                while (true) {
                    std::vector<double> ds;
                    std::vector<Tuple> cand = t->searchVector(st.knn_column, st.knn_vector,
                                                              pedir, knn_cos, st.probes, &ds);
                    filas.clear();
                    puntajes.clear();
                    for (std::size_t i = 0; i < cand.size(); ++i) {
                        if (!pasaTodas(cand[i])) continue;
                        filas.push_back(cand[i]);
                        puntajes.push_back(i < ds.size() ? ds[i] : 0.0);
                    }
                    if (k < 0) break;
                    if (static_cast<int>(filas.size()) >= k) break;
                    if (static_cast<int>(cand.size()) < pedir) break;
                    if (pedir > 1 << 20) break;
                    pedir *= 2;
                }
                if (k >= 0 && static_cast<int>(filas.size()) > k) {
                    filas.resize(static_cast<std::size_t>(k));
                    puntajes.resize(static_cast<std::size_t>(k));
                }
            }
        }
    } else if (cond.kind == PredKind::VECDIST) {
        const int cv = sch.indexOf(cond.column);
        if (cv < 0) throw DBException("No existe la columna '" + cond.column + "'");
        if (sch[cv].type != Type::VECTOR)
            throw DBException("El operador <=> exige una columna VECTOR: '" + cond.column +
                              "' es " + typeName(sch[cv].type));
        if (static_cast<int>(cond.vector_consulta.size()) != sch[cv].max_len)
            throw DBException("El vector de la consulta tiene " +
                              std::to_string(cond.vector_consulta.size()) +
                              " componentes y '" + cond.column + "' es VECTOR(" +
                              std::to_string(sch[cv].max_len) + ")");
        columna_extra      = "distancia";
        extra_es_distancia = true;

        const IndexInfo* iv = ti.findIndex(cond.column);
        if (iv && iv->kind == IndexKind::IVF) {
            filas = t->searchVectorUmbral(cond.column, cond.vector_consulta, cond.umbral,
                                          cond.vec_estricto, cond.vec_coseno, st.probes,
                                          &puntajes);
        } else {
            // Sin indice el umbral se resuelve recorriendo: es exacto y cuesta
            // una distancia por fila.
            for (const Tuple& f : t->scan()) {
                const Value& v = f.at(static_cast<std::size_t>(cv));
                if (v.type != Type::VECTOR || v.vec.size() != cond.vector_consulta.size())
                    continue;
                const double d = vecdist::entre(v.vec, cond.vector_consulta, cond.vec_coseno);
                if (cond.vec_estricto ? (d < cond.umbral) : (d <= cond.umbral)) {
                    filas.push_back(f);
                    puntajes.push_back(d);
                }
            }
            // Un umbral no ordena por si solo, pero devolver los mas parecidos
            // primero es lo que el usuario espera de una consulta de similitud.
            std::vector<std::size_t> idx(filas.size());
            for (std::size_t i = 0; i < idx.size(); ++i) idx[i] = i;
            std::sort(idx.begin(), idx.end(), [&](std::size_t a, std::size_t b) {
                return puntajes[a] != puntajes[b] ? puntajes[a] < puntajes[b] : a < b;
            });
            std::vector<Tuple> fo;
            std::vector<double> po;
            fo.reserve(idx.size());
            po.reserve(idx.size());
            for (std::size_t i : idx) { fo.push_back(filas[i]); po.push_back(puntajes[i]); }
            filas.swap(fo);
            puntajes.swap(po);
        }
    } else if (st.knn) {
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
        // Si la consulta trae una columna calculada (puntaje o distancia), el
        // filtro tiene que recortarla EN PARALELO. Dejarlas desalineadas haria
        // que la columna desapareciera del resultado sin decir nada.
        const bool con_extra = (puntajes.size() == filas.size());
        std::vector<Tuple>  quedan;
        std::vector<double> quedan_p;
        quedan.reserve(filas.size());
        for (std::size_t i = 0; i < filas.size(); ++i) {
            const Tuple& f = filas[i];
            bool ok = true;
            for (const Predicate& w : filtros) {
                int cf = sch.indexOf(w.column);
                if (cf < 0) throw DBException("No existe la columna '" + w.column + "'");
                if (!cumple(f.at(static_cast<std::size_t>(cf)), w)) { ok = false; break; }
            }
            if (!ok) continue;
            quedan.push_back(f);
            if (con_extra) quedan_p.push_back(puntajes[i]);
        }
        filas.swap(quedan);
        if (con_extra) puntajes.swap(quedan_p);
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
    // El puntaje de relevancia no vive en ninguna columna de la tabla, asi que
    // se anade al final del resultado. Sin el, un ranking es una lista de filas
    // en un orden que el usuario no puede verificar.
    const bool con_puntaje = !puntajes.empty() && puntajes.size() == filas.size();
    if (con_puntaje) r.columns.push_back(columna_extra);

    long long tope = (st.limit >= 0) ? st.limit : static_cast<long long>(filas.size());
    for (std::size_t fi = 0; fi < filas.size(); ++fi) {
        if (static_cast<long long>(r.rows.size()) >= tope) break;
        const Tuple& f = filas[fi];
        std::vector<std::string> fila;
        for (int ci : proyeccion) fila.push_back(f.at(static_cast<std::size_t>(ci)).str());
        if (con_puntaje) {
            char buf[32];
            // Una distancia coseno util vive en las milesimas: con cuatro
            // decimales dos vecinos distintos saldrian con el mismo numero.
            std::snprintf(buf, sizeof(buf), extra_es_distancia ? "%.6f" : "%.4f", puntajes[fi]);
            fila.push_back(buf);
        }
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

    // Un MATCH ordena por relevancia; no recorta un conjunto de filas. "Borra
    // lo que se parezca a estas palabras" no tiene un limite definido, asi que
    // el motor prefiere negarse antes que adivinar donde cortar.
    if (cond.kind == PredKind::MATCH)
        throw DBException("DELETE con MATCH no esta soportado: la relevancia ordena, no "
                          "selecciona. Borra por una condicion concreta sobre otra columna.");

    int ci = sch.indexOf(cond.column);
    if (ci < 0) throw DBException("No existe la columna '" + cond.column + "'");
    Type tipo = sch[ci].type;

    auto t_exec = Clock::now();
    std::vector<RID> objetivo;
    bool vecdist_exacto = false;
    if (cond.kind == PredKind::VECDIST) {
        if (tipo != Type::VECTOR)
            throw DBException("El operador <=> exige una columna VECTOR: '" + cond.column +
                              "' es " + typeName(tipo));
        if (static_cast<int>(cond.vector_consulta.size()) != sch[ci].max_len)
            throw DBException("El vector de la consulta tiene " +
                              std::to_string(cond.vector_consulta.size()) +
                              " componentes y '" + cond.column + "' es VECTOR(" +
                              std::to_string(sch[ci].max_len) + ")");
        // A PROPOSITO sin indice. El IVF es aproximado: si el vecino cayo en
        // una celda que no se visito, no aparece. En un SELECT eso es una fila
        // que falta en una lista; en un DELETE seria una fila que SI cumplia la
        // condicion y que se queda sin borrar, sin que nadie se entere. Borrar
        // es irreversible, asi que aqui se recorre la tabla entera y se calcula
        // la distancia de cada fila: exacto, O(N), y sin sorpresas.
        vecdist_exacto = true;
        Tuple f;
        for (const RID& rid : t->scanRIDs()) {
            if (!t->getByRID(rid, f)) continue;
            if (cumple(f.at(static_cast<std::size_t>(ci)), cond)) objetivo.push_back(rid);
        }
    } else if (cond.kind == PredKind::WITHIN) {
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
    if (vecdist_exacto)
        r.plan.push_back(PlanStep{
            "Recorrido exacto (sin IVF)", 0.0,
            "borrar es irreversible y el indice es aproximado: se calcula la distancia "
            "de todas las filas en vez de fiarse de las celdas visitadas"});
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
