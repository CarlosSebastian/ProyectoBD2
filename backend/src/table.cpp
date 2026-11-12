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
        if (meta.kind == IndexKind::INVERTED) {
            if (kt != Type::VARCHAR)
                throw DBException("Un indice INVERTED solo se crea sobre texto: '" +
                                  meta.column + "' es " + typeName(kt));
            ix.idioma = texto::idiomaDesdeNombre(meta.idioma.empty() ? "EN" : meta.idioma);
        } else if (meta.kind == IndexKind::IVF) {
            if (kt != Type::VECTOR)
                throw DBException("Un indice IVF solo se crea sobre VECTOR: '" +
                                  meta.column + "' es " + typeName(kt));
            if (info_.schema[static_cast<std::size_t>(ix.col)].max_len <= 0)
                throw DBException("La columna '" + meta.column +
                                  "' no declara dimension; declarala como VECTOR(d)");
            ix.listas  = meta.listas;
            ix.metrica = IVFIndex::metricaDesdeNombre(meta.metrica.empty() ? "COSINE"
                                                                           : meta.metrica);
        } else if (meta.kind == IndexKind::RTREE) {
            if (kt != Type::POINT && kt != Type::POLYGON)
                throw DBException("Un indice RTREE solo se crea sobre POINT o POLYGON: '" +
                                  meta.column + "' es " + typeName(kt));
        } else {
            if (kt == Type::DOUBLE)
                throw DBException("Aun no se indexan columnas DOUBLE");
            if (kt == Type::POINT || kt == Type::POLYGON)
                throw DBException("Una columna " + typeName(kt) +
                                  " solo admite un indice RTREE: '" + meta.column + "'");
            if (kt == Type::VECTOR)
                throw DBException("Una columna VECTOR solo admite un indice IVF: '" +
                                  meta.column + "'. Un B+ o un hash ordenan o reparten por "
                                  "clave, y un embedding no tiene clave: tiene vecinos.");
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
        case IndexKind::INVERTED:
            ix.inv = std::make_unique<InvertedIndex>(ix.bp.get());
            break;
        case IndexKind::IVF:
            ix.ivf = std::make_unique<IVFIndex>(ix.bp.get());
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
int Table::columnaVector(const std::string& col) const {
    int ci = info_.schema.indexOf(col);
    if (ci < 0) return -1;
    return info_.schema[static_cast<std::size_t>(ci)].type == Type::VECTOR ? ci : -1;
}

MBR Table::cajaDe(const Value& v) {
    if (v.type == Type::POLYGON) return poly::envolvente(v.poly);
    return MBR::point(v.d, v.y);
}

// Mantener los indices al dia es responsabilidad de la tabla: una insercion
// toca TODOS los indices, no solo el de la clave primaria.
void Table::insertIntoIndex(const Tuple& t, const RID& rid) {
    for (Indice& ix : indices_) {
        const Value& key = t.values[static_cast<std::size_t>(ix.col)];
        // El indice invertido no se actualiza fila a fila: insertar un
        // documento obligaria a desplazar casi todas las listas de postings.
        // Se marca como desactualizado y se reconstruye en la proxima consulta.
        if (ix.inv)         texto_sucio_ = true;
        // El IVF tampoco: k-means se entrena sobre el conjunto completo, asi
        // que anadir un vector no es insertar en una lista, es reparticionar.
        else if (ix.ivf)    vector_sucio_ = true;
        else if (ix.rt)     ix.rt->insert(cajaDe(key), rid);
        else if (ix.bt_int) ix.bt_int->insert(key.i, rid);
        else if (ix.hs_int) ix.hs_int->insert(key.i, rid);
        else if (ix.bt_str) ix.bt_str->insert(Key32(key.s), rid);
        else if (ix.hs_str) ix.hs_str->insert(Key32(key.s), rid);
    }
}
void Table::removeFromIndex(const Tuple& t, const RID& rid) {
    for (Indice& ix : indices_) {
        const Value& key = t.values[static_cast<std::size_t>(ix.col)];
        if (ix.inv)         texto_sucio_ = true;
        else if (ix.ivf)    vector_sucio_ = true;
        else if (ix.rt)     ix.rt->remove(cajaDe(key), rid);
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

    // Restriccion de PRIMARY KEY: no se admite un segundo registro con la
    // misma clave. Comprobarlo es una busqueda puntual, y lo que cuesta esa
    // busqueda depende de con que se pueda resolver:
    //
    //   con indice de clave sobre la columna   O(log n) en el B+, casi O(1)
    //                                          en el hash
    //   sin indice                             caro, y de dos maneras:
    //      - en un HEAP es un escaneo completo, O(n) por fila
    //      - en un SEQUENTIAL la busqueda binaria solo cubre el area
    //        principal; las filas recien insertadas viven en la cadena de
    //        desbordamiento y hay que recorrerla entera. Cuando esa cadena
    //        crece --- que es justo lo que pasa cargando sin reorganizar ---
    //        el costo es el mismo O(n) disfrazado
    //
    // El tercer caso convierte una carga masiva en O(n^2). Medido: insertar
    // 500 000 filas en un Heap sin indice pasaba de 142 ms a no terminar en
    // ocho horas. Por eso ahi la restriccion NO se hace cumplir.
    //
    // Un motor real evita la disyuntiva creando el indice junto con la clave:
    // en Postgres o MySQL una PRIMARY KEY *es* un indice unico. Este motor
    // prefiere no crear estructuras a espaldas del usuario, asi que la
    // restriccion se hace cumplir solo cuando se puede hacer cumplir barato.
    // La consecuencia esta dicha en el README: una tabla con PRIMARY KEY
    // declarada y sin indice sobre esa columna admite duplicados hasta que se
    // cree el indice. Es la misma regla para HEAP y para SEQUENTIAL: lo que
    // habilita la restriccion es el indice, no el motor de almacenamiento.
    if (!info_.key_column.empty()) {
        const int pki = info_.schema.indexOf(info_.key_column);
        // Solo cuentan los indices que responden una IGUALDAD por clave. Un
        // R-Tree, un invertido o un IVF estan sobre otros tipos de columna y
        // no sirven para esto.
        const Indice* ixk = indicePara(info_.key_column);
        const bool barato = ixk != nullptr && (ixk->bt_int || ixk->bt_str ||
                                              ixk->hs_int || ixk->hs_str);
        if (pki >= 0 && barato &&
            !searchRIDsEq(info_.key_column, t.values[static_cast<std::size_t>(pki)]).empty()) {
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
std::vector<RID> Table::scanRIDs() { return engine_->scanAll(); }

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
//  Consultas espaciales (Parte 2).
//
//  Hay cinco: ventana (WITHIN), k-NN euclidiano (<->), k-NN geografico y radio
//  (ST_DISTANCE, WITHIN_RADIUS) y contencion de punto en poligono
//  (ST_CONTAINS). Todas siguen el mismo esqueleto, asi que basta entender uno:
//
//    1. Resolver la columna y comprobar su tipo. columnaPunto() exige POINT;
//       columnaGeom() acepta POINT o POLYGON, porque la ventana vale para los
//       dos y la distancia solo para puntos.
//    2. Si hay un R-Tree sobre esa columna, pedirle los candidatos. Si no lo
//       hay, recorrer el heap entero. Las dos ramas devuelven lo MISMO: el
//       indice cambia el costo, nunca el resultado. Por eso el motor puede
//       responder sin indice en vez de rechazar la consulta.
//    3. Dejar en last_plan_ el metodo elegido, las paginas leidas y el tiempo.
//       De ahi sale el panel de plan del cliente, que es lo que permite
//       comprobar que el indice se uso de verdad.
//
//  Cada consulta aparece dos veces. searchRIDsX() hace el trabajo y devuelve
//  RIDs; searchX() la envuelve y lee las filas. La separacion existe porque
//  DELETE necesita los RIDs y no las filas, y asi no se duplica la logica.
//
//  Sobre POLYGON el indice es solo un FILTRO: guarda la caja envolvente, no la
//  figura. Un triangulo ocupa la mitad de su caja, asi que el indice devuelve
//  falsos positivos y hace falta un segundo paso de REFINAMIENTO con la
//  geometria real. Ese par filtrar + refinar es como funcionan los motores
//  espaciales de verdad, y se ve explicito en searchRIDsWithin y
//  searchRIDsContains.
// ---------------------------------------------------------------------------
// Ventana: todo lo que cae dentro del rectangulo (x0,y0)-(x1,y1).
// A diferencia de un rango sobre un B+, aqui la busqueda puede bajar por
// VARIAS ramas a la vez: las cajas hermanas de un R-Tree pueden solaparse, asi
// que no hay una sola rama "correcta" por la que descender.
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

// Los envoltorios que siguen son todos iguales: piden los RIDs a la variante
// de arriba y leen cada fila del heap. El contador de registros del plan se
// ajusta aqui porque una fila puede haberse borrado entre medias.
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

// k vecinos mas cercanos por distancia euclidiana, ya ordenados. El R-Tree lo
// resuelve con best-first y el resultado es EXACTO, no aproximado: visita solo
// los nodos que podrian contener un vecino mejor que el peor que ya tiene.
//
// Sin indice, o sin LIMIT, no queda otra que calcular la distancia de todas
// las filas. Con LIMIT se usa nth_element (O(n)) en vez de ordenar entero
// (O(n log n)), pero el archivo se lee completo igual.
std::vector<Tuple> Table::searchKNN(const std::string& col, double px, double py, int k,
                                    std::vector<double>* distancias) {
    if (distancias) distancias->clear();
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
            if (!getByRID(par.second, t)) continue;
            out.push_back(std::move(t));
            if (distancias) distancias->push_back(par.first);
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
        for (auto& par : todos) {
            out.push_back(std::move(par.second));
            if (distancias) distancias->push_back(par.first);
        }
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
std::vector<Tuple> Table::searchKNNGeo(const std::string& col, double lon, double lat, int k,
                                       std::vector<double>* distancias) {
    if (distancias) distancias->clear();
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
            if (!getByRID(par.second, t)) continue;
            out.push_back(std::move(t));
            if (distancias) distancias->push_back(par.first);
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
        for (auto& par : todos) {
            out.push_back(std::move(par.second));
            if (distancias) distancias->push_back(par.first);
        }
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
                                      double metros, std::vector<double>* distancias) {
    if (distancias) distancias->clear();
    // searchRIDsRadio valida la columna y lanza si no es POINT, asi que se
    // llama antes de pedir el indice de la columna.
    const std::vector<RID> rids = searchRIDsRadio(col, lon, lat, metros);
    const int ci = columnaPunto(col);
    std::vector<Tuple> out;
    for (const RID& rid : rids) {
        Tuple t;
        if (!getByRID(rid, t)) continue;
        if (distancias && ci >= 0) {
            const Value& v = t.values[static_cast<std::size_t>(ci)];
            distancias->push_back(geo::haversine(lon, lat, v.d, v.y));
        }
        out.push_back(std::move(t));
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
    // Una sola pasada por el heap repuebla TODOS los indices de clave a la vez.
    for (const RID& rid : engine_->scanAll()) {
        Tuple t;
        if (getByRID(rid, t)) insertIntoIndex(t, rid);
    }
    // Los invertidos y los IVF van aparte: su construccion es masiva y externa,
    // no una sucesion de inserciones.
    reconstruirTexto();
    reconstruirVector();
}

// ---------------------------------------------------------------------------
//  Reconstruccion de los indices invertidos de la tabla.
//
//  Se le pasa a SPIMI un generador que va sacando documentos del heap de uno
//  en uno. Asi el corpus entero no pasa nunca por memoria: lo unico que crece
//  con N es la tabla de documentos, que son doce bytes por fila.
// ---------------------------------------------------------------------------
void Table::reconstruirTexto(std::size_t presupuesto_bytes) {
    bool habia = false;
    for (Indice& ix : indices_) {
        if (!ix.inv) continue;
        habia = true;
        const std::size_t col = static_cast<std::size_t>(ix.col);
        const std::vector<RID> rids = engine_->scanAll();
        std::size_t i = 0;
        const std::string prefijo = joinPath(data_dir_, "spimi_" + info_.name + "_" + ix.columna);
        ix.inv->construir(
            [&](RID& rid, std::string& documento) {
                Tuple t;
                while (i < rids.size()) {
                    const RID r = rids[i++];
                    if (!getByRID(r, t)) continue;
                    rid = r;
                    documento = t.values[col].s;
                    return true;
                }
                return false;
            },
            ix.idioma, prefijo,
            presupuesto_bytes > 0 ? presupuesto_bytes : std::size_t(8u * 1024u * 1024u));
    }
    if (habia) texto_sucio_ = false;
}

const InvertedIndex* Table::indiceTexto(const std::string& col) const {
    const Indice* ix = indicePara(col);
    return (ix && ix->inv) ? ix->inv.get() : nullptr;
}

std::vector<Tuple> Table::searchMatch(const std::string& col, const std::string& consulta,
                                      int k, bool tfidf, std::vector<double>* puntajes) {
    auto t0 = Clock::now();
    const long long r0 = lecturasTotales();

    Indice* ix = indicePara(col);
    if (!ix || !ix->inv)
        throw DBException("No hay un indice INVERTED sobre la columna '" + col + "'");
    if (texto_sucio_) reconstruirTexto();

    const auto res = ix->inv->buscar(consulta, k,
                                     tfidf ? InvertedIndex::Ranking::TFIDF
                                           : InvertedIndex::Ranking::BM25);
    std::vector<Tuple> out;
    out.reserve(res.size());
    if (puntajes) { puntajes->clear(); puntajes->reserve(res.size()); }
    for (const InvertedIndex::Resultado& x : res) {
        Tuple t;
        if (!getByRID(x.rid, t)) continue;
        out.push_back(std::move(t));
        if (puntajes) puntajes->push_back(x.score);
    }

    last_plan_.metodo         = std::string("INDEX INVERTED (") + (tfidf ? "TF-IDF" : "BM25") + ")";
    last_plan_.columna        = col;
    last_plan_.paginas_leidas = lecturasTotales() - r0;
    last_plan_.registros      = static_cast<long long>(out.size());
    last_plan_.descartados    = 0;
    last_plan_.ms             = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
    return out;
}

std::vector<RID> Table::searchRIDsMatch(const std::string& col, const std::string& consulta,
                                        int k, bool tfidf) {
    Indice* ix = indicePara(col);
    if (!ix || !ix->inv)
        throw DBException("No hay un indice INVERTED sobre la columna '" + col + "'");
    if (texto_sucio_) reconstruirTexto();
    std::vector<RID> out;
    for (const InvertedIndex::Resultado& x :
         ix->inv->buscar(consulta, k, tfidf ? InvertedIndex::Ranking::TFIDF
                                            : InvertedIndex::Ranking::BM25))
        out.push_back(x.rid);
    return out;
}

// ---------------------------------------------------------------------------
//  Ruta vectorial (IVF)
// ---------------------------------------------------------------------------

// La metrica no es un parametro de la consulta: es parte del indice. k-means se
// entreno minimizando UNA distancia, y preguntar por la otra devolveria vecinos
// equivocados sin dar ningun error, que es la peor forma de equivocarse.
void Table::exigirMetrica(const Indice& ix, bool coseno) {
    const bool del_indice = (ix.metrica == IVFIndex::Metrica::COSENO);
    if (del_indice == coseno) return;
    throw DBException("El indice IVF de '" + ix.columna + "' esta entrenado con metrica " +
                      IVFIndex::nombreMetrica(ix.metrica) + " y la consulta pide " +
                      std::string(coseno ? "COSINE" : "L2") +
                      ". Las dos distancias no ordenan igual.");
}

void Table::exigirDimension(const Indice& ix, const std::string& col,
                            const std::vector<float>& q) const {
    const int dim = info_.schema[static_cast<std::size_t>(ix.col)].max_len;
    if (static_cast<int>(q.size()) != dim)
        throw DBException("El vector de la consulta tiene " + std::to_string(q.size()) +
                          " componentes y la columna '" + col + "' es VECTOR(" +
                          std::to_string(dim) + ")");
}

// ---------------------------------------------------------------------------
//  Reconstruccion de los indices IVF desde el heap.
//
//  La fuente se recorre TRES veces (muestrear, asignar, colocar), asi que
//  tiene que dar siempre los mismos vectores en el mismo orden. La lista de
//  RIDs se calcula una vez y se reutiliza en las tres pasadas; los vectores en
//  si no se acumulan nunca en memoria, se releen del heap.
// ---------------------------------------------------------------------------
void Table::reconstruirVector() {
    bool habia = false;
    for (Indice& ix : indices_) {
        if (!ix.ivf) continue;
        habia = true;
        const std::size_t col = static_cast<std::size_t>(ix.col);
        const int dim = info_.schema[col].max_len;
        const std::vector<RID> rids = engine_->scanAll();

        IVFIndex::Fuente fuente =
            [&](const std::function<void(const RID&, const std::vector<float>&)>& emitir) {
                Tuple t;
                for (const RID& r : rids) {
                    if (!getByRID(r, t)) continue;
                    emitir(r, t.values[col].vec);
                }
            };
        ix.ivf->construir(fuente, dim, ix.listas, ix.metrica);
    }
    if (habia) vector_sucio_ = false;
}

const IVFIndex* Table::indiceVector(const std::string& col) const {
    const Indice* ix = indicePara(col);
    return (ix && ix->ivf) ? ix->ivf.get() : nullptr;
}

std::vector<RID> Table::searchRIDsVector(const std::string& col, const std::vector<float>& q,
                                         int k, bool coseno, int probes) {
    Indice* ix = indicePara(col);
    if (!ix || !ix->ivf)
        throw DBException("No hay un indice IVF sobre la columna '" + col + "'");
    exigirMetrica(*ix, coseno);
    exigirDimension(*ix, col, q);
    if (vector_sucio_) reconstruirVector();

    const int total = static_cast<int>(ix->ivf->size());
    const int kk    = (k < 0) ? total : k;
    std::vector<RID> out;
    if (kk <= 0 || total == 0) return out;
    for (const IVFIndex::Resultado& r : ix->ivf->buscar(q, kk, probes)) out.push_back(r.rid);
    return out;
}

std::vector<Tuple> Table::searchVector(const std::string& col, const std::vector<float>& q,
                                       int k, bool coseno, int probes,
                                       std::vector<double>* distancias) {
    auto t0 = Clock::now();
    const long long r0 = lecturasTotales();

    Indice* ix = indicePara(col);
    if (!ix || !ix->ivf)
        throw DBException("No hay un indice IVF sobre la columna '" + col + "'");
    exigirMetrica(*ix, coseno);
    exigirDimension(*ix, col, q);
    if (vector_sucio_) reconstruirVector();

    const int total = static_cast<int>(ix->ivf->size());
    const int kk    = (k < 0) ? total : k;
    std::vector<Tuple> out;
    if (distancias) distancias->clear();

    std::vector<IVFIndex::Resultado> res;
    if (kk > 0 && total > 0) res = ix->ivf->buscar(q, kk, probes);
    out.reserve(res.size());
    for (const IVFIndex::Resultado& x : res) {
        Tuple t;
        if (!getByRID(x.rid, t)) continue;
        out.push_back(std::move(t));
        if (distancias) distancias->push_back(x.dist);
    }

    // Igual que en el plan: pedir mas celdas de las que hay no es un error,
    // pero publicar "nprobe=3/2" se lee como un bug.
    int np = probes > 0 ? probes : ix->ivf->nprobePorDefecto();
    if (np > ix->ivf->listas()) np = ix->ivf->listas();
    last_plan_.metodo         = "INDEX IVF (" + IVFIndex::nombreMetrica(ix->metrica) +
                                ", nprobe=" + std::to_string(np) + "/" +
                                std::to_string(ix->ivf->listas()) + ")";
    last_plan_.columna        = col;
    last_plan_.paginas_leidas = lecturasTotales() - r0;
    last_plan_.registros      = static_cast<long long>(out.size());
    last_plan_.descartados    = 0;
    last_plan_.ms             = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
    return out;
}

// El IVF solo sabe responder top-k, pero un umbral no dice cuantas filas son.
// Se pide un k y se DOBLA mientras el ultimo vecino siga dentro del umbral: al
// salir del bucle o el k-esimo ya se paso del umbral (y entonces no falta
// ninguno) o se agoto el indice. Asi el resultado no depende de haber acertado
// el k de entrada.
std::vector<IVFIndex::Resultado> Table::ivfUmbral(Indice& ix, const std::vector<float>& q,
                                                  double umbral, bool estricto, int probes) {
    const int total = static_cast<int>(ix.ivf->size());
    std::vector<IVFIndex::Resultado> res;
    if (total == 0) return res;

    int k = 64;
    while (true) {
        if (k > total) k = total;
        res = ix.ivf->buscar(q, k, probes);
        if (k >= total || static_cast<int>(res.size()) < k) break;   // no hay mas que traer
        const double ultima = res.back().dist;
        if (estricto ? (ultima >= umbral) : (ultima > umbral)) break;  // el umbral ya quedo cubierto
        k *= 2;
    }
    // buscar() devuelve por distancia creciente: en el primer vecino que se
    // pasa del umbral se puede cortar.
    std::size_t corte = 0;
    while (corte < res.size()) {
        const double d = res[corte].dist;
        if (estricto ? (d < umbral) : (d <= umbral)) ++corte;
        else break;
    }
    res.resize(corte);
    return res;
}

std::vector<RID> Table::searchRIDsVectorUmbral(const std::string& col,
                                               const std::vector<float>& q,
                                               double umbral, bool estricto, bool coseno,
                                               int probes) {
    Indice* ix = indicePara(col);
    if (!ix || !ix->ivf)
        throw DBException("No hay un indice IVF sobre la columna '" + col + "'");
    exigirMetrica(*ix, coseno);
    exigirDimension(*ix, col, q);
    if (vector_sucio_) reconstruirVector();

    std::vector<RID> out;
    for (const IVFIndex::Resultado& r : ivfUmbral(*ix, q, umbral, estricto, probes))
        out.push_back(r.rid);
    return out;
}

std::vector<Tuple> Table::searchVectorUmbral(const std::string& col,
                                             const std::vector<float>& q,
                                             double umbral, bool estricto, bool coseno,
                                             int probes, std::vector<double>* distancias) {
    auto t0 = Clock::now();
    const long long r0 = lecturasTotales();

    Indice* ix = indicePara(col);
    if (!ix || !ix->ivf)
        throw DBException("No hay un indice IVF sobre la columna '" + col + "'");
    exigirMetrica(*ix, coseno);
    exigirDimension(*ix, col, q);
    if (vector_sucio_) reconstruirVector();

    const std::vector<IVFIndex::Resultado> res = ivfUmbral(*ix, q, umbral, estricto, probes);
    std::vector<Tuple> out;
    out.reserve(res.size());
    if (distancias) { distancias->clear(); distancias->reserve(res.size()); }
    for (const IVFIndex::Resultado& x : res) {
        Tuple t;
        if (!getByRID(x.rid, t)) continue;
        out.push_back(std::move(t));
        if (distancias) distancias->push_back(x.dist);
    }

    // Igual que en el plan: pedir mas celdas de las que hay no es un error,
    // pero publicar "nprobe=3/2" se lee como un bug.
    int np = probes > 0 ? probes : ix->ivf->nprobePorDefecto();
    if (np > ix->ivf->listas()) np = ix->ivf->listas();
    last_plan_.metodo         = "INDEX IVF UMBRAL (" + IVFIndex::nombreMetrica(ix->metrica) +
                                ", nprobe=" + std::to_string(np) + "/" +
                                std::to_string(ix->ivf->listas()) + ")";
    last_plan_.columna        = col;
    last_plan_.paginas_leidas = lecturasTotales() - r0;
    last_plan_.registros      = static_cast<long long>(out.size());
    last_plan_.descartados    = 0;
    last_plan_.ms             = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
    return out;
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
