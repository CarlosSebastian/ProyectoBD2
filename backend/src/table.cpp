#include "db/table.hpp"

#include <chrono>
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
Table::Table(const std::string& data_dir, const TableInfo& info, int pool_size) : info_(info) {
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

    if (!info_.indexes.empty()) {
        const IndexInfo& ix = info_.indexes.front();   // un indice por tabla
        key_col_ = info_.schema.indexOf(ix.column);
        if (key_col_ < 0) throw DBException("La columna indexada no existe: " + ix.column);

        Type kt = info_.schema[key_col_].type;
        if (kt == Type::DOUBLE)
            throw DBException("Aun no se indexan columnas DOUBLE");

        idx_disk_ = std::make_unique<DiskManager>(joinPath(data_dir, ix.file));
        idx_bp_   = std::make_unique<BufferPool>(idx_disk_.get(), pool_size);

        try {
            if (ix.kind == IndexKind::BPLUS) {
                if (kt == Type::INT) bt_int_ = std::make_unique<BPlusTree<std::int64_t>>(idx_bp_.get());
                else                 bt_str_ = std::make_unique<BPlusTree<Key32>>(idx_bp_.get());
            } else {
                if (kt == Type::INT) hs_int_ = std::make_unique<ExtendibleHash<std::int64_t>>(idx_bp_.get());
                else                 hs_str_ = std::make_unique<ExtendibleHash<Key32>>(idx_bp_.get());
            }
        } catch (const DBException&) {
            // El .idx que hay en disco no es del tipo que declara el catalogo:
            // es un archivo viejo que quedo de un indice anterior. El catalogo
            // manda, y un indice siempre se puede reconstruir desde los datos,
            // asi que se tira el archivo y se repuebla en vez de dejar la tabla
            // inaccesible.
            buildIndex();
        }
    }
}

bool Table::indexedColumn(const std::string& col) const {
    return key_col_ >= 0 && info_.schema[key_col_].name == col;
}
bool Table::claveSecuencial(const std::string& col) const {
    return seq_ != nullptr && seq_col_ >= 0 && info_.schema[seq_col_].name == col;
}
long long Table::lecturasTotales() const {
    return store_disk_->readCount()
         + (ovf_disk_ ? ovf_disk_->readCount() : 0)
         + (idx_disk_ ? idx_disk_->readCount() : 0);
}

void Table::insertIntoIndex(const Value& key, const RID& rid) {
    if (bt_int_)      bt_int_->insert(key.i, rid);
    else if (hs_int_) hs_int_->insert(key.i, rid);
    else if (bt_str_) bt_str_->insert(Key32(key.s), rid);
    else if (hs_str_) hs_str_->insert(Key32(key.s), rid);
}
void Table::removeFromIndex(const Value& key, const RID& rid) {
    if (bt_int_)      bt_int_->remove(key.i, rid);
    else if (hs_int_) hs_int_->remove(key.i, rid);
    else if (bt_str_) bt_str_->remove(Key32(key.s), rid);
    else if (hs_str_) hs_str_->remove(Key32(key.s), rid);
}
std::vector<RID> Table::indexLookup(const Value& v) {
    if (bt_int_) return bt_int_->search(v.i);
    if (hs_int_) return hs_int_->search(v.i);
    if (bt_str_) return bt_str_->search(Key32(v.s));
    if (hs_str_) return hs_str_->search(Key32(v.s));
    return {};
}
std::vector<RID> Table::indexRange(const Value& lo, const Value& hi) {
    if (bt_int_) return bt_int_->rangeSearch(lo.i, hi.i);
    if (bt_str_) return bt_str_->rangeSearch(Key32(lo.s), Key32(hi.s));
    return {};   // el hash no ordena: no puede resolver rangos
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
    if (key_col_ >= 0) insertIntoIndex(t.values[key_col_], rid);
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
    if (idx_bp_)   idx_bp_->flushAll();
}

bool Table::removeByRID(const RID& rid) {
    Tuple t;
    if (!getByRID(rid, t)) return false;
    if (key_col_ >= 0) removeFromIndex(t.values[key_col_], rid);
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

    if (indexedColumn(col)) {
        last_plan_.metodo = (bt_int_ || bt_str_) ? "INDEX BPLUS" : "INDEX HASH";
        out = indexLookup(v);
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

    if (indexedColumn(col) && (bt_int_ || bt_str_)) {
        last_plan_.metodo = "INDEX BPLUS (rango)";
        out = indexRange(lo, hi);
    } else if (claveSecuencial(col)) {
        // El archivo esta ordenado por esta columna: se ubica el inicio con
        // busqueda binaria y se recorre hacia adelante. No hay full scan.
        last_plan_.metodo = "SEQ BINARY SEARCH (rango)";
        out = seq_->searchRange(lo, hi);
    } else {
        last_plan_.metodo = indexedColumn(col) ? "SEQ SCAN (hash no soporta rango)" : "SEQ SCAN";
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
// Vacia el archivo de indice y recrea la estructura desde cero.
// Es OBLIGATORIO antes de repoblar: si el .idx ya existia, el constructor del
// B+ o del Hash ve que el archivo no esta vacio y NO lo inicializa, sino que
// reutiliza la META que encuentre. Con un archivo del tipo equivocado --por
// ejemplo un indice hash que se vuelve a declarar como BTREE-- eso hace que el
// arbol lea bytes de otra estructura y termine pidiendo una pagina inexistente.
void Table::recrearIndiceVacio() {
    if (key_col_ < 0 || !idx_disk_ || info_.indexes.empty()) return;
    idx_bp_->invalidateAll();
    idx_disk_->truncate();
    bt_int_.reset(); bt_str_.reset(); hs_int_.reset(); hs_str_.reset();

    const IndexInfo& ix = info_.indexes.front();
    const Type kt = info_.schema[key_col_].type;
    if (ix.kind == IndexKind::BPLUS) {
        if (kt == Type::INT) bt_int_ = std::make_unique<BPlusTree<std::int64_t>>(idx_bp_.get());
        else                 bt_str_ = std::make_unique<BPlusTree<Key32>>(idx_bp_.get());
    } else {
        if (kt == Type::INT) hs_int_ = std::make_unique<ExtendibleHash<std::int64_t>>(idx_bp_.get());
        else                 hs_str_ = std::make_unique<ExtendibleHash<Key32>>(idx_bp_.get());
    }
}

void Table::buildIndex() {
    if (key_col_ < 0) return;
    recrearIndiceVacio();
    for (const RID& rid : engine_->scanAll()) {
        Tuple t;
        if (getByRID(rid, t)) insertIntoIndex(t.values[key_col_], rid);
    }
}

bool Table::reorganize(double fill_factor) {
    if (!seq_) return false;
    seq_->reorganize(fill_factor);

    // reorganize() reescribe el area principal, asi que TODOS los RID cambian.
    // Cualquier indice que apunte a esta tabla queda invalido y hay que
    // reconstruirlo desde cero.
    // buildIndex() ya vacia y recrea el indice antes de repoblarlo.
    if (key_col_ >= 0 && idx_disk_) buildIndex();
    return true;
}

int Table::indexPages() const { return idx_disk_ ? idx_disk_->numPages() : 0; }

void Table::resetStats() {
    store_disk_->resetStats();
    store_bp_->resetStats();
    if (ovf_disk_) ovf_disk_->resetStats();
    if (ovf_bp_)   ovf_bp_->resetStats();
    if (idx_disk_) idx_disk_->resetStats();
    if (idx_bp_)   idx_bp_->resetStats();
}

}  // namespace db
