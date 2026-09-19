// ============================================================================
//  catalog.hpp - Metadatos de tablas e indices (el "diccionario de datos")
//
//  Se persiste en un archivo de texto plano (data/catalog.txt) con formato:
//     TABLE <nombre> <archivo_datos> <motor> <clave_primaria>
//     COL   <nombre> <TIPO> <max_len>
//     INDEX <columna> <BPLUS|HASH> <archivo>
//     END
//  Texto plano a proposito: el catalogo se lee una vez al arrancar y hay que
//  poder inspeccionarlo y depurarlo a mano durante el desarrollo.
// ============================================================================
#pragma once

#include <map>
#include <string>
#include <vector>

#include "db/record.hpp"

namespace db {

enum class IndexKind { BPLUS, HASH };

std::string indexKindName(IndexKind k);
IndexKind   indexKindFromName(const std::string& s);

struct IndexInfo {
    std::string column;
    IndexKind   kind = IndexKind::BPLUS;
    std::string file;
};

struct TableInfo {
    std::string            name;
    Schema                 schema;
    std::string            heap_file;
    std::string            engine = "HEAP";   // HEAP | SEQUENTIAL
    std::string            key_column;        // PRIMARY KEY: ordena el Sequential File
    std::vector<IndexInfo> indexes;

    const IndexInfo* findIndex(const std::string& col) const;
};

class Catalog {
public:
    explicit Catalog(std::string catalog_path);

    void load();
    void save() const;

    bool exists(const std::string& name) const;
    void createTable(const TableInfo& info);
    void addIndex(const std::string& table, const IndexInfo& idx);
    void dropTable(const std::string& name);

    const TableInfo& get(const std::string& name) const;
    std::vector<std::string> tableNames() const;

private:
    std::string                      path_;
    std::map<std::string, TableInfo> tables_;
};

}  // namespace db
