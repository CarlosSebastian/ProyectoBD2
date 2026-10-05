#include "db/table.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <sstream>

namespace db {

using Clock = std::chrono::high_resolution_clock;

std::string PlanInfo::str() const {
    std::ostringstream os;
    os.precision(3);
    os << metodo;
    if (!columna.empty()) os << " sobre " << columna;
    os << " | paginas_disco=" << paginas_leidas
       << " | registros=" << registros
       << " | " << std::fixed << ms << " ms";
    return os.str();
}

static std::string joinPath(const std::string& dir, const std::string& file) {
    if (dir.empty()) return file;
    char last = dir[dir.size() - 1];
    if (last == '/' || last == '\\') return dir + file;
    return dir + "/" + file;
}

// ---------------------------------------------------------------------------
Table::Table(const std::string& data_dir, const TableInfo& info, int pool_size)
    : info_(info), data_dir_(data_dir), pool_size_(pool_size) {
    store_disk_ = std::make_unique<DiskManager>(joinPath(data_dir, info_.heap_file));
    store_bp_   = std::make_unique<BufferPool>(store_disk_.get(), pool_size);

    if (info_.engine == "SEQUENTIAL") {
        // La clave que ordena el archivo es la PRIMARY KEY; si no se declaro,
        // se usa la primera columna (que es lo que hace un CREATE TABLE sin PK).
        seq_col_ = info_.key_column.empty() ? 0 : info_.schema.indexOf(info_.key_column);
        if (seq_col_ < 0) seq_col_ = 0;

        std::string ovf = info_.heap_file;
        std::size_t punto = ovf.find_last_of('.');
        ovf = (punto == std::string::npos ? ovf : ovf.substr(0, punto)) + ".ovf";

        ovf_disk_ = std::make_unique<DiskManager>(joinPath(data_dir, ovf));
        ovf_bp_   = std::make_unique<BufferPool>(ovf_disk_.get(), pool_size);

        auto s = std::make_unique<SequentialFile>(store_bp_.get(), ovf_bp_.get(),
                                                  info_.schema, seq_col_);
        seq_    = s.get();
        engine_ = std::move(s);
    } else {
        engine_ = std::make_unique<HeapFile>(store_bp_.get());
    }

    // Un indice por cada entrada del catalogo. Cada uno con su archivo, su
    // DiskManager y su BufferPool propios.
    for (const IndexInfo& meta : info_.indexes) {
        Indice ix;
        ix.col     = info_.schema.indexOf(meta.column);
        ix.kind    = meta.kind;
        ix.columna = meta.column;
        if (ix.col < 0) throw DBException("La columna indexada no existe: " + meta.column);

        const Type kt = info_.schema[static_cast<std::size_t>(ix.col)].type;
        if (meta.kind == IndexKind::RTREE) {
            if (kt != Type::POINT && kt != Type::POLYGON)
                throw DBException("Un indice RTREE solo se crea sobre POINT o POLYGON: '" +
                                  meta.column + "' es " + typeName(kt));
        } else {
            if (kt == Type::DOUBLE)
                throw DBException("Aun no se indexan columnas DOUBLE");
            if (kt == Type::POINT || kt == Type::POLYGON)
                throw DBException("Una columna " + typeName(kt) +
                                  " solo admite un indice RTREE: '" + meta.column + "'");
        }

        ix.disk = std::make_unique<DiskManager>(joinPath(data_dir, meta.file));
        ix.bp   = std::make_unique<BufferPool>(ix.disk.get(), pool_size);

        bool hay_que_repoblar = false;
        try {
            abrirIndice(ix);
        } catch (const DBException&) {
            // El .idx que hay en disco no es del tipo que declara el catalogo:
            // es un archivo viejo que quedo de un indice anterior. El catalogo
            // manda, y un indice siempre se puede reconstruir desde los datos,
            // asi que se tira el archivo y se repuebla en vez de dejar la tabla
            // inaccesible.
            hay_que_repoblar = true;
        }
        indices_.push_back(std::move(ix));
        if (hay_que_repoblar) buildIndex();
    }
}

// Crea (o abre) la estructura concreta segun el tipo de indice y el de la columna.
void Table::abrirIndice(Indice& ix) {
    const Type kt = info_.schema[static_cast<std::size_t>(ix.col)].type;
    switch (ix.kind) {
        case IndexKind::RTREE:
            ix.rt = std::make_unique<RTree>(ix.bp.get());
            break;
        case IndexKind::BPLUS:
            if (kt == Type::INT) ix.bt_int = std::make_unique<BPlusTree<std::int64_t>>(ix.bp.get());
            else                 ix.bt_str = std::make_unique<BPlusTree<Key32>>(ix.bp.get());
            break;
        case IndexKind::HASH:
            if (kt == Type::INT) ix.hs_int = std::make_unique<ExtendibleHash<std::int64_t>>(ix.bp.get());
            else                 ix.hs_str = std::make_unique<ExtendibleHash<Key32>>(ix.bp.get());
            break;
    }
}

const Table::Indice* Table::indicePara(const std::string& col) const {
    for (const Indice& ix : indices_) if (ix.columna == col) return &ix;
    return nullptr;
}
Table::Indice* Table::indicePara(const std::string& col) {
    for (Indice& ix : indices_) if (ix.columna == col) return &ix;
    return nullptr;
}

bool Table::claveSecuencial(const std::string& col) const {
    return seq_ != nullptr && seq_col_ >= 0 && info_.schema[seq_col_].name == col;
}
long long Table::lecturasTotales() const {
    long long n = store_disk_->readCount() + (ovf_disk_ ? ovf_disk_->readCount() : 0);
    for (const Indice& ix : indices_) n += ix.disk->readCount();
    return n;
}

// Indice de la columna si es de tipo POINT; -1 en cualquier otro caso.
int Table::columnaPunto(const std::string& col) const {
    int ci = info_.schema.indexOf(col);
    if (ci < 0) return -1;
    return info_.schema[static_cast<std::size_t>(ci)].type == Type::POINT ? ci : -1;
}

int Table::columnaGeom(const std::string& col) const {
    int ci = info_.schema.indexOf(col);
    if (ci < 0) return -1;
    const Type t = info_.schema[static_cast<std::size_t>(ci)].type;
    return (t == Type::POINT || t == Type::POLYGON) ? ci : -1;
}

// Lo que el R-Tree guarda de una geometria: el punto mismo, o la envolvente
// del poligono. Para el poligono es solo una aproximacion, y de ahi que las
// consultas tengan que refinar.
MBR Table::cajaDe(const Value& v) {
    if (v.type == Type::POLYGON) return poly::envolvente(v.poly);
    return MBR::point(v.d, v.y);
}

// Mantener los indices al dia es responsabilidad de la tabla: una insercion
// toca TODOS los indices, no solo el de la clave primaria.
void Table::insertIntoIndex(const Tuple& t, const RID& rid) {
    for (Indice& ix : indices_) {
        const Value& key = t.values[static_cast<std::size_t>(ix.col)];
        if (ix.rt)          ix.rt->insert(cajaDe(key), rid);
        else if (ix.bt_int) ix.bt_int->insert(key.i, rid);
        else if (ix.hs_int) ix.hs_int->insert(key.i, rid);
        else if (ix.bt_str) ix.bt_str->insert(Key32(key.s), rid);
        else if (ix.hs_str) ix.hs_str->insert(Key32(key.s), rid);
    }
}
void Table::removeFromIndex(const Tuple& t, const RID& rid) {
    for (Indice& ix : indices_) {
        const Value& key = t.values[static_cast<std::size_t>(ix.col)];
        if (ix.rt)          ix.rt->remove(cajaDe(key), rid);
        else if (ix.bt_int) ix.bt_int->remove(key.i, rid);
        else if (ix.hs_int) ix.hs_int->remove(key.i, rid);
        else if (ix.bt_str) ix.bt_str->remove(Key32(key.s), rid);
        else if (ix.hs_str) ix.hs_str->remove(Key32(key.s), rid);
    }
}
std::vector<RID> Table::indexLookup(const Indice& ix, const Value& v) {
    if (ix.bt_int) return ix.bt_int->search(v.i);
    if (ix.hs_int) return ix.hs_int->search(v.i);
    if (ix.bt_str) return ix.bt_str->search(Key32(v.s));
    if (ix.hs_str) return ix.hs_str->search(Key32(v.s));
    if (ix.rt)     return ix.rt->search(cajaDe(v));   // punto o envolvente del poligono
    return {};
}
std::vector<RID> Table::indexRange(const Indice& ix, const Value& lo, const Value& hi) {
    if (ix.bt_int) return ix.bt_int->rangeSearch(lo.i, hi.i);
    if (ix.bt_str) return ix.bt_str->rangeSearch(Key32(lo.s), Key32(hi.s));
    return {};   // ni el hash ni el R-Tree ordenan: no resuelven rangos 1D
}

RID Table::insert(const Tuple& t) {
    // La comprobacion va ANTES de insertar: reorganizar mueve todos los
    // registros, asi que hacerlo despues devolveria un RID ya invalido.
    if (seq_ && auto_reorg_ && seq_->necesitaReorganizacion()) reorganize();

     // Restriccion de PRIMARY KEY: no se admite un segundo registro con la misma clave 
    if (!info_.key_column.empty()) {
        int pki = info_.schema.indexOf(info_.key_column);
        if (pki >= 0 && !searchRIDsEq(info_.key_column, t.values[static_cast<std::size_t>(pki)]).empty()) {
            throw DBException("Violacion de PRIMARY KEY: ya existe un registro con " +
                              info_.key_column + " = " + t.values[static_cast<std::size_t>(pki)].str());
        }
    }

    std::string bytes = serializeTuple(info_.schema, t);
    RID rid = engine_->insert(bytes);
    insertIntoIndex(t, rid);
    return rid;
}

bool Table::getByRID(const RID& rid, Tuple& out) const {
    std::string bytes;
    if (!engine_->get(rid, bytes)) return false;
    out = deserializeTuple(info_.schema, bytes.data(), static_cast<int>(bytes.size()));
    return true;
}

void Table::flush() {
    if (store_bp_) store_bp_->flushAll();
    if (ovf_bp_)   ovf_bp_->flushAll();
    for (Indice& ix : indices_) ix.bp->flushAll();
}

bool Table::removeByRID(const RID& rid) {
    Tuple t;
    if (!getByRID(rid, t)) return false;
    removeFromIndex(t, rid);
    return engine_->erase(rid);
}

// ---------------------------------------------------------------------------
//  Busquedas que devuelven RIDs. Aqui vive la seleccion de ruta de acceso.
// ---------------------------------------------------------------------------
std::vector<RID> Table::searchRIDsEq(const std::string& col, const Value& v) {
    auto t0 = Clock::now();
    long long r0 = lecturasTotales();

    std::vector<RID> out;
    int ci = info_.schema.indexOf(col);
    if (ci < 0) throw DBException("No existe la columna: " + col);

    const Indice* ix = indicePara(col);
    const bool geom_poly = info_.schema[static_cast<std::size_t>(ci)].type == Type::POLYGON;
    if (ix) {
        // Igualdad sobre un punto: es una ventana degenerada de area cero.
        // Sobre un poligono el indice solo puede buscar por caja envolvente, y
        // dos poligonos distintos pueden compartirla: hay que refinar.
        last_plan_.metodo = (ix->kind != IndexKind::RTREE)
                              ? ((ix->kind == IndexKind::BPLUS) ? "INDEX BPLUS" : "INDEX HASH")
                              : (geom_poly ? "INDEX RTREE (igualdad + refinamiento)"
                                           : "INDEX RTREE (punto)");
        out = indexLookup(*ix, v);
        if (ix->kind == IndexKind::RTREE && geom_poly) {
            std::vector<RID> exactos;
            exactos.reserve(out.size());
            for (const RID& rid : out) {
                Tuple t;
                if (getByRID(rid, t) && t.values[static_cast<std::size_t>(ci)] == v)
                    exactos.push_back(rid);
            }
            last_plan_.descartados = static_cast<long long>(out.size() - exactos.size());
            out.swap(exactos);
        }
    } else if (claveSecuencial(col)) {
        last_plan_.metodo = "SEQ BINARY SEARCH";
        out = seq_->searchEq(v);
    } else {
        last_plan_.metodo = "SEQ SCAN";
        for (const RID& rid : engine_->scanAll()) {
            Tuple t;
            if (getByRID(rid, t) && t.values[ci] == v) out.push_back(rid);
        }
    }

    last_plan_.columna        = col;
    last_plan_.registros      = static_cast<long long>(out.size());
    last_plan_.paginas_leidas = lecturasTotales() - r0;
    last_plan_.ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
    return out;
}

std::vector<RID> Table::searchRIDsRange(const std::string& col, const Value& lo, const Value& hi) {
    auto t0 = Clock::now();
    long long r0 = lecturasTotales();

    std::vector<RID> out;
    int ci = info_.schema.indexOf(col);
    if (ci < 0) throw DBException("No existe la columna: " + col);

    const Indice* ix = indicePara(col);
    if (ix && ix->kind == IndexKind::BPLUS) {
        last_plan_.metodo = "INDEX BPLUS (rango)";
        out = indexRange(*ix, lo, hi);
    } else if (claveSecuencial(col)) {
        // El archivo esta ordenado por esta columna: se ubica el inicio con
        // busqueda binaria y se recorre hacia adelante. No hay full scan.
        last_plan_.metodo = "SEQ BINARY SEARCH (rango)";
        out = seq_->searchRange(lo, hi);
    } else {
        if (ix && ix->kind == IndexKind::RTREE)
            last_plan_.metodo = "SEQ SCAN (el R-Tree no ordena: use WITHIN)";
        else if (ix)
            last_plan_.metodo = "SEQ SCAN (hash no soporta rango)";
        else
            last_plan_.metodo = "SEQ SCAN";
        for (const RID& rid : engine_->scanAll()) {
            Tuple t;
            if (!getByRID(rid, t)) continue;
            if (!(t.values[ci] < lo) && !(hi < t.values[ci])) out.push_back(rid);
        }
    }

    last_plan_.columna        = col;
    last_plan_.registros      = static_cast<long long>(out.size());
    last_plan_.paginas_leidas = lecturasTotales() - r0;
    last_plan_.ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
    return out;
}

std::vector<Tuple> Table::searchEq(const std::string& col, const Value& v) {
    std::vector<Tuple> out;
    for (const RID& rid : searchRIDsEq(col, v)) {
        Tuple t;
        if (getByRID(rid, t)) out.push_back(t);
    }
    last_plan_.registros = static_cast<long long>(out.size());
    return out;
}

std::vector<Tuple> Table::searchRange(const std::string& col, const Value& lo, const Value& hi) {
    std::vector<Tuple> out;
    for (const RID& rid : searchRIDsRange(col, lo, hi)) {
        Tuple t;
        if (getByRID(rid, t)) out.push_back(t);
    }
    last_plan_.registros = static_cast<long long>(out.size());
    return out;
}

std::vector<Tuple> Table::scan() {
    auto t0 = Clock::now();
    long long r0 = lecturasTotales();

    std::vector<Tuple> out;
    for (const RID& rid : engine_->scanAll()) {
        Tuple t;
        if (getByRID(rid, t)) out.push_back(t);
    }

    last_plan_.metodo         = "SEQ SCAN";
    last_plan_.columna        = "";
    last_plan_.registros      = static_cast<long long>(out.size());
    last_plan_.paginas_leidas = lecturasTotales() - r0;
    last_plan_.ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
    return out;
}

// ---------------------------------------------------------------------------
//  Consultas espaciales. Son las dos rutas que estrena el Entregable 2.
// ---------------------------------------------------------------------------
std::vector<RID> Table::searchRIDsWithin(const std::string& col,
                                         double x0, double y0, double x1, double y1) {
    auto t0 = Clock::now();
    long long r0 = lecturasTotales();

    int ci = columnaGeom(col);
    if (ci < 0) throw DBException("La columna '" + col + "' no es de tipo POINT ni POLYGON");
    const bool es_poligono = info_.schema[static_cast<std::size_t>(ci)].type == Type::POLYGON;
    const MBR ventana(x0, y0, x1, y1);

    std::vector<RID> out;
    const Indice* ix = indicePara(col);
    if (ix && ix->rt) {
        last_plan_.metodo = es_poligono ? "INDEX RTREE (ventana + refinamiento)"
                                        : "INDEX RTREE (ventana)";
        out = ix->rt->search(ventana);
        if (es_poligono) {
            // PASO DE REFINAMIENTO. El indice solo sabe de cajas envolventes:
            // devuelve poligonos cuya caja toca la ventana aunque la geometria
            // real no la toque. Hay que mirar la geometria y descartarlos.
            std::vector<RID> exactos;
            exactos.reserve(out.size());
            for (const RID& rid : out) {
                Tuple t;
                if (!getByRID(rid, t)) continue;
                if (poly::intersecaRect(t.values[static_cast<std::size_t>(ci)].poly, ventana))
                    exactos.push_back(rid);
            }
            last_plan_.descartados = static_cast<long long>(out.size() - exactos.size());
            out.swap(exactos);
        }
    } else {
        // Sin indice espacial hay que mirar geometria por geometria: es el full
        // scan contra el que se compara el R-Tree en el experimento.
        last_plan_.metodo = "SEQ SCAN (ventana)";
        for (const RID& rid : engine_->scanAll()) {
            Tuple t;
            if (!getByRID(rid, t)) continue;
            const Value& v = t.values[static_cast<std::size_t>(ci)];
            const bool ok = es_poligono ? poly::intersecaRect(v.poly, ventana)
                                        : MBR::point(v.d, v.y).interseca(ventana);
            if (ok) out.push_back(rid);
        }
    }

    last_plan_.columna        = col;
    last_plan_.registros      = static_cast<long long>(out.size());
    last_plan_.paginas_leidas = lecturasTotales() - r0;
    last_plan_.ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
    return out;
}

std::vector<Tuple> Table::searchWithin(const std::string& col,
                                       double x0, double y0, double x1, double y1) {
    std::vector<Tuple> out;
    for (const RID& rid : searchRIDsWithin(col, x0, y0, x1, y1)) {
        Tuple t;
        if (getByRID(rid, t)) out.push_back(t);
    }
    last_plan_.registros = static_cast<long long>(out.size());
    return out;
}

std::vector<Tuple> Table::searchKNN(const std::string& col, double px, double py, int k) {
    auto t0 = Clock::now();
    long long r0 = lecturasTotales();

    int ci = columnaPunto(col);
    if (ci < 0) throw DBException("La columna '" + col + "' no es de tipo POINT");

    std::vector<Tuple> out;
    const Indice* ix = indicePara(col);
    if (ix && ix->rt && k >= 0) {
        last_plan_.metodo = "INDEX RTREE (knn)";
        for (const auto& par : ix->rt->knn(px, py, k)) {
            Tuple t;
            if (getByRID(par.second, t)) out.push_back(t);
        }
    } else {
        // Sin indice (o sin LIMIT) hay que calcular la distancia de TODAS las
        // filas y ordenar. Con LIMIT se usa nth_element + sort parcial, que es
        // O(n) en vez de O(n log n), pero sigue leyendo el archivo entero.
        last_plan_.metodo = (k >= 0) ? "SEQ SCAN (knn)" : "SEQ SCAN (orden por distancia)";
        std::vector<std::pair<double, Tuple>> todos;
        for (const RID& rid : engine_->scanAll()) {
            Tuple t;
            if (!getByRID(rid, t)) continue;
            const Value& v = t.values[static_cast<std::size_t>(ci)];
            double dx = v.d - px, dy = v.y - py;
            todos.emplace_back(std::sqrt(dx * dx + dy * dy), std::move(t));
        }
        auto antes = [](const std::pair<double, Tuple>& a, const std::pair<double, Tuple>& b) {
            return a.first < b.first;
        };
        std::size_t tope = (k >= 0 && static_cast<std::size_t>(k) < todos.size())
                         ? static_cast<std::size_t>(k) : todos.size();
        if (tope < todos.size()) {
            std::nth_element(todos.begin(), todos.begin() + static_cast<long>(tope), todos.end(), antes);
            todos.resize(tope);
        }
        std::sort(todos.begin(), todos.end(), antes);
        for (auto& par : todos) out.push_back(std::move(par.second));
    }

    last_plan_.columna        = col;
    last_plan_.registros      = static_cast<long long>(out.size());
    last_plan_.paginas_leidas = lecturasTotales() - r0;
    last_plan_.ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
    return out;
}

// --- ST_CONTAINS: poligonos que contienen a un punto ----------------------
//  Es el ejemplo mas claro del esquema filtrar + refinar: el R-Tree reduce
//  millones de poligonos a un punado de candidatos en dos o tres accesos a
//  pagina, y el lanzamiento de rayo decide cuales de esos lo contienen de
//  verdad.
std::vector<RID> Table::searchRIDsContains(const std::string& col, double px, double py) {
    auto t0 = Clock::now();
    long long r0 = lecturasTotales();

    int ci = info_.schema.indexOf(col);
    if (ci < 0) throw DBException("No existe la columna: " + col);
    if (info_.schema[static_cast<std::size_t>(ci)].type != Type::POLYGON)
        throw DBException("ST_CONTAINS exige una columna POLYGON: '" + col + "' es " +
                          typeName(info_.schema[static_cast<std::size_t>(ci)].type));

    const MBR punto = MBR::point(px, py);
    std::vector<RID> candidatos;
    const Indice* ix = indicePara(col);
    if (ix && ix->rt) {
        last_plan_.metodo = "INDEX RTREE (contiene + refinamiento)";
        candidatos = ix->rt->search(punto);
    } else {
        last_plan_.metodo = "SEQ SCAN (contiene)";
        candidatos = engine_->scanAll();
    }

    std::vector<RID> out;
    long long descartados = 0;
    for (const RID& rid : candidatos) {
        Tuple t;
        if (!getByRID(rid, t)) continue;
        if (poly::contienePunto(t.values[static_cast<std::size_t>(ci)].poly, px, py))
            out.push_back(rid);
        else
            ++descartados;
    }
    last_plan_.descartados = (ix && ix->rt) ? descartados : 0;

    last_plan_.columna        = col;
    last_plan_.registros      = static_cast<long long>(out.size());
    last_plan_.paginas_leidas = lecturasTotales() - r0;
    last_plan_.ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
    return out;
}

std::vector<Tuple> Table::searchContains(const std::string& col, double px, double py) {
    std::vector<Tuple> out;
    for (const RID& rid : searchRIDsContains(col, px, py)) {
        Tuple t;
        if (getByRID(rid, t)) out.push_back(t);
    }
    last_plan_.registros = static_cast<long long>(out.size());
    return out;
}

// --- Variantes geograficas: distancia real en metros sobre la esfera -------
std::vector<Tuple> Table::searchKNNGeo(const std::string& col, double lon, double lat, int k) {
    auto t0 = Clock::now();
    long long r0 = lecturasTotales();

    int ci = columnaPunto(col);
    if (ci < 0) throw DBException("La columna '" + col + "' no es de tipo POINT");

    std::vector<Tuple> out;
    const Indice* ix = indicePara(col);
    if (ix && ix->rt && k >= 0) {
        last_plan_.metodo = "INDEX RTREE (knn geo)";
        for (const auto& par : ix->rt->knnGeo(lon, lat, k)) {
            Tuple t;
            if (getByRID(par.second, t)) out.push_back(t);
        }
    } else {
        last_plan_.metodo = (k >= 0) ? "SEQ SCAN (knn geo)" : "SEQ SCAN (orden por distancia geo)";
        std::vector<std::pair<double, Tuple>> todos;
        for (const RID& rid : engine_->scanAll()) {
            Tuple t;
            if (!getByRID(rid, t)) continue;
            const Value& v = t.values[static_cast<std::size_t>(ci)];
            todos.emplace_back(geo::haversine(lon, lat, v.d, v.y), std::move(t));
        }
        auto antes = [](const std::pair<double, Tuple>& a, const std::pair<double, Tuple>& b) {
            return a.first < b.first;
        };
        std::size_t tope = (k >= 0 && static_cast<std::size_t>(k) < todos.size())
                         ? static_cast<std::size_t>(k) : todos.size();
        if (tope < todos.size()) {
            std::nth_element(todos.begin(), todos.begin() + static_cast<long>(tope), todos.end(), antes);
            todos.resize(tope);
        }
        std::sort(todos.begin(), todos.end(), antes);
        for (auto& par : todos) out.push_back(std::move(par.second));
    }

    last_plan_.columna        = col;
    last_plan_.registros      = static_cast<long long>(out.size());
    last_plan_.paginas_leidas = lecturasTotales() - r0;
    last_plan_.ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
    return out;
}

std::vector<RID> Table::searchRIDsRadio(const std::string& col, double lon, double lat,
                                        double metros) {
    auto t0 = Clock::now();
    long long r0 = lecturasTotales();

    int ci = columnaPunto(col);
    if (ci < 0) throw DBException("La columna '" + col + "' no es de tipo POINT");

    std::vector<RID> out;
    const Indice* ix = indicePara(col);
    if (ix && ix->rt) {
        last_plan_.metodo = "INDEX RTREE (radio)";
        out = ix->rt->searchRadio(lon, lat, metros);
    } else {
        last_plan_.metodo = "SEQ SCAN (radio)";
        for (const RID& rid : engine_->scanAll()) {
            Tuple t;
            if (!getByRID(rid, t)) continue;
            const Value& v = t.values[static_cast<std::size_t>(ci)];
            if (geo::haversine(lon, lat, v.d, v.y) <= metros) out.push_back(rid);
        }
    }

    last_plan_.columna        = col;
    last_plan_.registros      = static_cast<long long>(out.size());
    last_plan_.paginas_leidas = lecturasTotales() - r0;
    last_plan_.ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
    return out;
}

std::vector<Tuple> Table::searchRadio(const std::string& col, double lon, double lat,
                                      double metros) {
    std::vector<Tuple> out;
    for (const RID& rid : searchRIDsRadio(col, lon, lat, metros)) {
        Tuple t;
        if (getByRID(rid, t)) out.push_back(t);
    }
    last_plan_.registros = static_cast<long long>(out.size());
    return out;
}

// ---------------------------------------------------------------------------
// Vacia el archivo de indice y recrea la estructura desde cero.
// Es OBLIGATORIO antes de repoblar: si el .idx ya existia, el constructor del
// B+ o del Hash ve que el archivo no esta vacio y NO lo inicializa, sino que
// reutiliza la META que encuentre. Con un archivo del tipo equivocado --por
// ejemplo un indice hash que se vuelve a declarar como BTREE-- eso hace que el
// arbol lea bytes de otra estructura y termine pidiendo una pagina inexistente.
void Table::recrearIndiceVacio() {
    for (Indice& ix : indices_) {
        ix.bp->invalidateAll();
        ix.disk->truncate();
        ix.soltarEstructuras();
        abrirIndice(ix);
    }
}

void Table::buildIndex() {
    if (indices_.empty()) return;
    recrearIndiceVacio();
    // Una sola pasada por el heap repuebla TODOS los indices a la vez.
    for (const RID& rid : engine_->scanAll()) {
        Tuple t;
        if (getByRID(rid, t)) insertIntoIndex(t, rid);
    }
}

bool Table::reorganize(double fill_factor) {
    if (!seq_) return false;
    seq_->reorganize(fill_factor);

    // reorganize() reescribe el area principal, asi que TODOS los RID cambian.
    // Cualquier indice que apunte a esta tabla queda invalido y hay que
    // reconstruirlo desde cero.
    // buildIndex() ya vacia y recrea cada indice antes de repoblarlo.
    if (!indices_.empty()) buildIndex();
    return true;
}

int Table::indexPages() const {
    int n = 0;
    for (const Indice& ix : indices_) n += ix.disk->numPages();
    return n;
}

int Table::indexHeight() const {
    for (const Indice& ix : indices_) {
        if (ix.bt_int) return ix.bt_int->getHeight();
        if (ix.bt_str) return ix.bt_str->getHeight();
        if (ix.rt)     return ix.rt->getHeight();
    }
    return 0;
}

int Table::indexHeight(const std::string& col) const {
    const Indice* ix = indicePara(col);
    if (!ix) return 0;
    if (ix->bt_int) return ix->bt_int->getHeight();
    if (ix->bt_str) return ix->bt_str->getHeight();
    if (ix->rt)     return ix->rt->getHeight();
    return 0;
}

void Table::resetStats() {
    store_disk_->resetStats();
    store_bp_->resetStats();
    if (ovf_disk_) ovf_disk_->resetStats();
    if (ovf_bp_)   ovf_bp_->resetStats();
    for (Indice& ix : indices_) { ix.disk->resetStats(); ix.bp->resetStats(); }
}

}  // namespace db
