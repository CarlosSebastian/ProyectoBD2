// ============================================================================
//  api_server.cpp - Backend REST del mini-gestor
//
//  Endpoints (los que exige el enunciado):
//     POST /api/query              { "sql": "..." }  -> tuplas + plan + I/O + ms
//     GET  /api/tables                               -> tablas, columnas, indices
//     POST /api/tables/reorganize  { "table": "..." }
//     GET  /api/health
//     GET  /                                         -> sirve el cliente web
//
//  El servidor HTTP es cpp-httplib (MIT, cabecera unica en include/third_party).
//  La restriccion del enunciado prohibe motores de BD y ORMs, no servidores
//  HTTP: todo el almacenamiento y los indices son propios.
// ============================================================================
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>

#include "db/database.hpp"
#include "db/disk_counter.hpp"
#include "db/json.hpp"
#include "third_party/httplib.h"

using namespace db;

static std::string planToJson(const std::vector<PlanStep>& pasos) {
    std::vector<std::string> items;
    for (const PlanStep& p : pasos) {
        std::ostringstream os;
        os << "{\"paso\":" << json::str(p.nombre)
           << ",\"ms\":" << json::num(p.ms)
           << ",\"detalle\":" << json::str(p.detalle) << "}";
        items.push_back(os.str());
    }
    return json::arr(items);
}

static std::string resultToJson(const QueryResult& r) {
    std::ostringstream os;
    os << "{\"ok\":" << (r.ok ? "true" : "false")
       << ",\"error\":" << json::str(r.error)
       << ",\"message\":" << json::str(r.message)
       << ",\"metodo\":" << json::str(r.metodo);

    std::vector<std::string> cols;
    for (const std::string& c : r.columns) cols.push_back(json::str(c));
    os << ",\"columns\":" << json::arr(cols);

    std::vector<std::string> filas;
    for (const auto& fila : r.rows) {
        std::vector<std::string> celdas;
        for (const std::string& c : fila) celdas.push_back(json::str(c));
        filas.push_back(json::arr(celdas));
    }
    os << ",\"rows\":" << json::arr(filas)
       << ",\"row_count\":" << r.row_count
       << ",\"plan\":" << planToJson(r.plan)
       << ",\"disk_reads\":" << r.disk_reads
       << ",\"disk_writes\":" << r.disk_writes
       << ",\"page_accesses\":" << r.page_accesses
       << ",\"buffer_hits\":" << r.buffer_hits
       << ",\"parse_ms\":" << json::num(r.parse_ms)
       << ",\"exec_ms\":" << json::num(r.exec_ms)
       << ",\"total_ms\":" << json::num(r.total_ms)
       << "}";
    return os.str();
}
static std::string resultsToJson(const std::vector<QueryResult>& rs) {
    std::vector<std::string> items;
    bool todo_ok = true;
    long long dr = 0, dw = 0, pa = 0, bh = 0;
    double total_ms = 0.0;
    for (const QueryResult& r : rs) {
        items.push_back(resultToJson(r));
        if (!r.ok) todo_ok = false;
        dr += r.disk_reads; dw += r.disk_writes;
        pa += r.page_accesses; bh += r.buffer_hits;
        total_ms += r.total_ms;
    }
    std::ostringstream os;
    os << "{\"ok\":" << (todo_ok ? "true" : "false")
       << ",\"statements\":" << json::arr(items)
       << ",\"disk_reads\":" << dr
       << ",\"disk_writes\":" << dw
       << ",\"page_accesses\":" << pa
       << ",\"buffer_hits\":" << bh
       << ",\"total_ms\":" << json::num(total_ms)
       << "}";
    return os.str();
}
static std::string tablesToJson(const std::vector<TableSummary>& ts) {
    std::vector<std::string> items;
    for (const TableSummary& t : ts) {
        std::vector<std::string> cols;
        for (const auto& c : t.columns)
            cols.push_back("{\"name\":" + json::str(c.name) + ",\"type\":" + json::str(c.type) + "}");
        std::vector<std::string> idx;
        for (const auto& i : t.indexes)
            idx.push_back("{\"column\":" + json::str(i.column) + ",\"kind\":" + json::str(i.kind) + "}");

        std::ostringstream os;
        os << "{\"name\":" << json::str(t.name)
           << ",\"engine\":" << json::str(t.engine)
           << ",\"columns\":" << json::arr(cols)
           << ",\"indexes\":" << json::arr(idx)
           << ",\"rows\":" << t.rows
           << ",\"heap_pages\":" << t.heap_pages
           << ",\"index_pages\":" << t.index_pages
           << ",\"overflow_pages\":" << t.overflow_pages
           << ",\"overflow_rows\":" << t.overflow_rows << "}";
        items.push_back(os.str());
    }
    std::ostringstream os;
    os << "{\"ok\":true,\"tables\":" << json::arr(items)
       << ",\"disk_reads_total\":" << DiskCounter::global().diskReads()
       << ",\"disk_writes_total\":" << DiskCounter::global().diskWrites() << "}";
    return os.str();
}

int main(int argc, char** argv) {
    std::string data_dir = "data";
    std::string www_dir  = "frontend";
    int         puerto   = 8080;
    int         pool     = 64;      // paginas del buffer pool

    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--data")   && i + 1 < argc) data_dir = argv[++i];
        else if (!std::strcmp(argv[i], "--static") && i + 1 < argc) www_dir = argv[++i];
        else if (!std::strcmp(argv[i], "--port")   && i + 1 < argc) puerto  = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--pool")   && i + 1 < argc) pool    = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--help")) {
            std::cout << "uso: dbserver [--port 8080] [--data data] [--static frontend] [--pool 64]\n"
                      << "  --pool N  paginas del buffer pool. Reducirlo hace visibles las\n"
                      << "            lecturas fisicas de disco en el panel de metricas.\n";
            return 0;
        }
    }

    Database   db(data_dir, pool);
    std::mutex mtx;                      // httplib atiende en varios hilos
    httplib::Server srv;

    srv.set_mount_point("/", www_dir);

    srv.Post("/api/query", [&](const httplib::Request& req, httplib::Response& res) {
        std::string sql;
        if (!json::getString(req.body, "sql", &sql)) sql = req.body;   // tambien acepta texto plano
        std::lock_guard<std::mutex> lock(mtx);
        std::vector<QueryResult> rs = db.executeMultiple(sql);
        res.set_content(resultsToJson(rs), "application/json");
    });

    srv.Get("/api/tables", [&](const httplib::Request&, httplib::Response& res) {
        std::lock_guard<std::mutex> lock(mtx);
        try {
            res.set_content(tablesToJson(db.tables()), "application/json");
        } catch (const std::exception& e) {
            res.status = 500;
            res.set_content(std::string("{\"ok\":false,\"error\":") + json::str(e.what()) + "}",
                            "application/json");
        }
    });

    srv.Post("/api/tables/reorganize", [&](const httplib::Request& req, httplib::Response& res) {
        std::string tabla;
        json::getString(req.body, "table", &tabla);
        std::lock_guard<std::mutex> lock(mtx);
        QueryResult r = db.reorganize(tabla);
        if (!r.ok) res.status = 501;                 // Not Implemented
        res.set_content(resultToJson(r), "application/json");
    });

    srv.Get("/api/health", [&](const httplib::Request&, httplib::Response& res) {
        std::ostringstream os;
        os << "{\"ok\":true,\"page_size\":" << PAGE_SIZE
           << ",\"pool_pages\":" << db.poolSize()
           << ",\"disk_reads\":" << DiskCounter::global().diskReads()
           << ",\"disk_writes\":" << DiskCounter::global().diskWrites()
           << ",\"page_accesses\":" << DiskCounter::global().pageAccesses() << "}";
        res.set_content(os.str(), "application/json");
    });

    std::cout << "\n  Mini-gestor BD2 escuchando en http://localhost:" << puerto << "\n"
              << "  datos    : " << data_dir << "\n"
              << "  frontend : " << www_dir  << "\n"
              << "  pagina   : " << PAGE_SIZE << " bytes\n"
              << "  pool     : " << pool << " paginas\n\n"
              << "  Ctrl+C para detener.\n\n";

    if (!srv.listen("0.0.0.0", puerto)) {
        std::cerr << "No se pudo abrir el puerto " << puerto << " (puede estar ocupado)\n";
        return 1;
    }
    return 0;
}
