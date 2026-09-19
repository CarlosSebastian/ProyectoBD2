// ============================================================================
//  demo.cpp - Demostracion end-to-end del Avance 1
//  Crea una tabla, la llena, consulta e imprime el PLAN de cada consulta.
// ============================================================================
#include <cstdio>
#include <iostream>

#include "db/disk_manager.hpp"
#include "db/catalog.hpp"
#include "db/table.hpp"

using namespace db;

static void titulo(const std::string& t) {
    std::cout << "\n" << std::string(64, '=') << "\n  " << t << "\n" << std::string(64, '=') << "\n";
}

int main(int argc, char** argv) {
    const std::string DIR = (argc > 1) ? argv[1] : "data";
    resetFile(DIR + "/catalog.txt");
    resetFile(DIR + "/tiendas.dat");
    resetFile(DIR + "/tiendas_id.idx");

    titulo("1. CREATE TABLE tiendas + CREATE INDEX (B+ Tree) sobre id");
    Catalog cat(DIR + "/catalog.txt");
    TableInfo ti;
    ti.name      = "tiendas";
    ti.heap_file = "tiendas.dat";
    ti.schema    = Schema({Column("id", Type::INT),
                           Column("nombre", Type::VARCHAR, 40),
                           Column("distrito", Type::VARCHAR, 30),
                           Column("ventas", Type::DOUBLE)});
    ti.indexes.push_back(IndexInfo{"id", IndexKind::BPLUS, "tiendas_id.idx"});
    cat.createTable(ti);

    for (const auto& c : cat.get("tiendas").schema.columns())
        std::cout << "   columna " << c.name << " : " << typeName(c.type)
                  << (c.max_len ? "(" + std::to_string(c.max_len) + ")" : "") << "\n";

    titulo("2. INSERT de 5000 filas");
    // Buffer pool deliberadamente pequeno (8 paginas) para que se NOTE la
    // diferencia de I/O entre usar el indice y hacer full scan. Con un pool
    // grande toda la tabla entra en RAM y las lecturas fisicas caen a cero.
    Table t(DIR, cat.get("tiendas"), 8);
    const char* distritos[] = {"Miraflores", "San Isidro", "Surco", "Barranco", "Lince"};
    for (int i = 0; i < 5000; ++i)
        t.insert(Tuple{{Value::makeInt(i),
                        Value::makeStr("Tienda " + std::to_string(i)),
                        Value::makeStr(distritos[i % 5]),
                        Value::makeDouble(1000.0 + i * 3.7)}});
    std::cout << "   buffer pool = 8 paginas (a proposito pequeno)\n";
    std::cout << "   filas=" << t.count() << "  paginas heap=" << t.heapPages()
              << "  paginas indice=" << t.indexPages() << "\n";

    titulo("3. SELECT * FROM tiendas WHERE id = 4321   (columna indexada)");
    t.resetStats();
    for (const auto& f : t.searchEq("id", Value::makeInt(4321)))
        std::cout << "   " << f.str() << "\n";
    std::cout << "   PLAN: " << t.lastPlan().str() << "\n";

    titulo("4. SELECT * FROM tiendas WHERE distrito = 'Barranco'   (SIN indice)");
    t.resetStats();
    auto r2 = t.searchEq("distrito", Value::makeStr("Barranco"));
    std::cout << "   " << r2.size() << " filas (se muestran 3)\n";
    for (int i = 0; i < 3 && i < static_cast<int>(r2.size()); ++i)
        std::cout << "   " << r2[i].str() << "\n";
    std::cout << "   PLAN: " << t.lastPlan().str() << "\n";

    titulo("5. SELECT * FROM tiendas WHERE id BETWEEN 100 AND 120   (rango)");
    t.resetStats();
    auto r3 = t.searchRange("id", Value::makeInt(100), Value::makeInt(120));
    std::cout << "   " << r3.size() << " filas\n";
    std::cout << "   PLAN: " << t.lastPlan().str() << "\n";

    titulo("6. DELETE: se borra la fila id=4321 del heap Y del indice");
    Tuple tmp;
    for (int p = 0; p < t.heapPages(); ++p)
        for (int s = 0; s < 100; ++s)
            if (t.getByRID(RID(p, s), tmp) && tmp.at(0).i == 4321) { t.removeByRID(RID(p, s)); }
    std::cout << "   busqueda posterior devuelve "
              << t.searchEq("id", Value::makeInt(4321)).size() << " filas\n";
    std::cout << "   filas restantes=" << t.count() << "\n";

    std::cout << "\nDemo terminada. Datos en " << DIR << "/\n\n";
    return 0;
}
