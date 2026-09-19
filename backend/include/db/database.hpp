// ============================================================================
//  database.hpp - Ejecutor SQL: une parser + catalogo + motor de almacenamiento
//
//  Es la fachada que consume la API REST. Cada execute() devuelve, ademas de
//  las tuplas, la telemetria completa que exige el enunciado:
//      bloques leidos, bloques escritos, tiempo de parseo y tiempo de ejecucion
//  desglosados por etapa del plan.
// ============================================================================
#pragma once

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "db/catalog.hpp"
#include "db/sql.hpp"
#include "db/table.hpp"

namespace db {

struct PlanStep {
    std::string nombre;
    double      ms = 0.0;
    std::string detalle;
};

struct QueryResult {
    bool        ok = true;
    std::string error;
    std::string message;                              // para DDL/DML

    std::vector<std::string>              columns;    // para SELECT
    std::vector<std::vector<std::string>> rows;
    long long                             row_count = 0;

    std::string           metodo = "-";               // IndexScan / SeqScan / ...
    std::vector<PlanStep> plan;

    long long disk_reads    = 0;   // transferencias fisicas de bloque
    long long disk_writes   = 0;
    long long page_accesses = 0;   // accesos logicos a pagina (via buffer pool)
    long long buffer_hits   = 0;   // de esos, cuantos se sirvieron desde RAM
    double    parse_ms    = 0.0;
    double    exec_ms     = 0.0;
    double    total_ms    = 0.0;
};

struct TableSummary {
    struct ColSummary { std::string name, type; };
    struct IdxSummary { std::string column, kind; };

    std::string             name;
    std::string             engine;
    std::vector<ColSummary> columns;
    std::vector<IdxSummary> indexes;
    long long               rows           = 0;
    int                     heap_pages     = 0;
    int                     index_pages    = 0;
    int                     overflow_pages = 0;
    long long               overflow_rows  = 0;
};

class Database {
public:
    explicit Database(std::string data_dir, int pool_size = 64);

    int poolSize() const { return pool_size_; }

    QueryResult               execute(const std::string& sql);

    // Vuelca a disco las paginas sucias de todas las tablas abiertas.
    void                      flush();
    std::vector<TableSummary> tables();
    QueryResult               reorganize(const std::string& tabla);

    const std::string& dataDir() const { return data_dir_; }

private:
    Table* abrir(const std::string& nombre);
    void   cerrar(const std::string& nombre);

    QueryResult ejecutarCreateTable(const Statement& st);
    QueryResult ejecutarCreateIndex(const Statement& st);
    QueryResult ejecutarInsert(const Statement& st);
    QueryResult ejecutarSelect(const Statement& st);
    QueryResult ejecutarDelete(const Statement& st);

    std::string data_dir_;
    int         pool_size_;
    Catalog     catalog_;
    std::map<std::string, std::unique_ptr<Table>> abiertas_;
};

}  // namespace db
