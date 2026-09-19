#include "db/catalog.hpp"

#include <fstream>
#include <sstream>

namespace db {

std::string indexKindName(IndexKind k) { return k == IndexKind::BPLUS ? "BPLUS" : "HASH"; }

IndexKind indexKindFromName(const std::string& s) {
    if (s == "BPLUS") return IndexKind::BPLUS;
    if (s == "HASH")  return IndexKind::HASH;
    throw DBException("Tipo de indice desconocido: " + s);
}

const IndexInfo* TableInfo::findIndex(const std::string& col) const {
    for (const auto& ix : indexes)
        if (ix.column == col) return &ix;
    return nullptr;
}

Catalog::Catalog(std::string catalog_path) : path_(std::move(catalog_path)) { load(); }

void Catalog::load() {
    tables_.clear();
    std::ifstream in(path_);
    if (!in) return;                       // catalogo vacio la primera vez

    std::string line;
    TableInfo   cur;
    std::vector<Column> cols;
    bool open = false;

    while (std::getline(in, line)) {
        std::istringstream ss(line);
        std::string tag;
        if (!(ss >> tag)) continue;

        if (tag == "TABLE") {
            ss >> cur.name >> cur.heap_file;
            std::string motor, clave;
            if (ss >> motor) cur.engine = motor;      // compatibilidad: catalogos viejos
            else             cur.engine = "HEAP";
            if (ss >> clave && clave != "-") cur.key_column = clave;
            else                              cur.key_column.clear();
            cols.clear();
            cur.indexes.clear();
            open = true;
        } else if (tag == "COL" && open) {
            std::string cname, ctype;
            int len = 0;
            ss >> cname >> ctype >> len;
            cols.emplace_back(cname, typeFromName(ctype), len);
        } else if (tag == "INDEX" && open) {
            IndexInfo ix;
            std::string kind;
            ss >> ix.column >> kind >> ix.file;
            ix.kind = indexKindFromName(kind);
            cur.indexes.push_back(ix);
        } else if (tag == "END" && open) {
            cur.schema = Schema(cols);
            tables_[cur.name] = cur;
            open = false;
            cur = TableInfo();
        }
    }
}

void Catalog::save() const {
    std::ofstream out(path_, std::ios::trunc);
    if (!out) throw DBException("No se pudo escribir el catalogo: " + path_);
    for (const auto& kv : tables_) {
        const TableInfo& t = kv.second;
        out << "TABLE " << t.name << " " << t.heap_file << " " << t.engine << " "
            << (t.key_column.empty() ? "-" : t.key_column) << "\n";
        for (const auto& c : t.schema.columns())
            out << "COL " << c.name << " " << typeName(c.type) << " " << c.max_len << "\n";
        for (const auto& ix : t.indexes)
            out << "INDEX " << ix.column << " " << indexKindName(ix.kind) << " " << ix.file << "\n";
        out << "END\n";
    }
}

bool Catalog::exists(const std::string& name) const { return tables_.count(name) > 0; }

void Catalog::createTable(const TableInfo& info) {
    if (exists(info.name)) throw DBException("La tabla ya existe: " + info.name);
    tables_[info.name] = info;
    save();
}

void Catalog::addIndex(const std::string& table, const IndexInfo& idx) {
    auto it = tables_.find(table);
    if (it == tables_.end()) throw DBException("No existe la tabla: " + table);
    if (it->second.findIndex(idx.column)) throw DBException("Ya hay un indice en " + idx.column);
    it->second.indexes.push_back(idx);
    save();
}

void Catalog::dropTable(const std::string& name) {
    tables_.erase(name);
    save();
}

const TableInfo& Catalog::get(const std::string& name) const {
    auto it = tables_.find(name);
    if (it == tables_.end()) throw DBException("No existe la tabla: " + name);
    return it->second;
}

std::vector<std::string> Catalog::tableNames() const {
    std::vector<std::string> out;
    for (const auto& kv : tables_) out.push_back(kv.first);
    return out;
}

}  // namespace db
