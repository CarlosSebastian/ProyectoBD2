// ============================================================================
//  test_catalog.cpp - Catalogo, esquemas, serializacion y capa Table
// ============================================================================
#include <cstdio>
#include <string>
#include <vector>

#include "db/disk_manager.hpp"
#include "db/catalog.hpp"
#include "db/table.hpp"
#include "test_util.hpp"

using namespace db;

static TableInfo tiendasInfo(IndexKind kind) {
    TableInfo ti;
    ti.name      = "tiendas";
    ti.heap_file = "t_tiendas.dat";
    ti.schema    = Schema({Column("id", Type::INT),
                           Column("nombre", Type::VARCHAR, 40),
                           Column("ventas", Type::DOUBLE)});
    ti.indexes.push_back(IndexInfo{"id", kind, "t_tiendas_id.idx"});
    return ti;
}

int main() {
    SECTION("serializacion de tuplas");
    {
        Schema sch({Column("id", Type::INT), Column("nombre", Type::VARCHAR, 10), Column("x", Type::DOUBLE)});
        Tuple t{{Value::makeInt(42), Value::makeStr("hola"), Value::makeDouble(3.5)}};
        std::string bytes = serializeTuple(sch, t);
        Tuple back = deserializeTuple(sch, bytes.data(), static_cast<int>(bytes.size()));
        CHECK_EQ(back.at(0).i, static_cast<std::int64_t>(42), "INT ida y vuelta");
        CHECK_EQ(back.at(1).s, std::string("hola"), "VARCHAR ida y vuelta");
        CHECK_EQ(back.at(2).d, 3.5, "DOUBLE ida y vuelta");

        Tuple largo{{Value::makeInt(1), Value::makeStr("textodemasiadolargoparaelcampo"), Value::makeDouble(0)}};
        Tuple corto = deserializeTuple(sch, serializeTuple(sch, largo).data(),
                                       static_cast<int>(serializeTuple(sch, largo).size()));
        CHECK_EQ(corto.at(1).s.size(), static_cast<std::size_t>(10), "VARCHAR(10) trunca");
    }

    SECTION("catalogo: crear, guardar y releer");
    {
        resetFile("data/catalog.txt");
        {
            Catalog cat("data/catalog.txt");
            cat.createTable(tiendasInfo(IndexKind::BPLUS));
            CHECK(cat.exists("tiendas"), "la tabla quedo registrada");
        }
        Catalog cat2("data/catalog.txt");           // se relee del disco
        CHECK(cat2.exists("tiendas"), "la tabla sobrevive al reinicio");
        const TableInfo& ti = cat2.get("tiendas");
        CHECK_EQ(ti.schema.size(), static_cast<std::size_t>(3), "columnas del esquema");
        CHECK_EQ(ti.schema[1].max_len, 40, "max_len del VARCHAR");
        CHECK(ti.findIndex("id") != nullptr, "el indice quedo registrado");
        CHECK(ti.findIndex("nombre") == nullptr, "columna sin indice");
        bool lanzo = false;
        try { cat2.get("noexiste"); } catch (const DBException&) { lanzo = true; }
        CHECK(lanzo, "pedir una tabla inexistente lanza excepcion");
    }

    SECTION("Table con indice B+ Tree: eleccion de plan");
    {
        resetFile("data/t_tiendas.dat");
        resetFile("data/t_tiendas_id.idx");
        Table t("data", tiendasInfo(IndexKind::BPLUS));
        for (int i = 0; i < 2000; ++i)
            t.insert(Tuple{{Value::makeInt(i), Value::makeStr("Tienda " + std::to_string(i)),
                            Value::makeDouble(i * 1.5)}});
        CHECK_EQ(t.count(), static_cast<std::size_t>(2000), "registros insertados");

        auto r = t.searchEq("id", Value::makeInt(1234));
        CHECK_EQ(r.size(), static_cast<std::size_t>(1), "busqueda por la columna indexada");
        CHECK_EQ(r[0].at(1).s, std::string("Tienda 1234"), "fila correcta");
        CHECK_EQ(t.lastPlan().metodo, std::string("INDEX BPLUS"), "uso el indice");

        auto r2 = t.searchEq("nombre", Value::makeStr("Tienda 7"));
        CHECK_EQ(r2.size(), static_cast<std::size_t>(1), "busqueda por columna sin indice");
        CHECK_EQ(t.lastPlan().metodo, std::string("SEQ SCAN"), "cayo a full scan");

        auto r3 = t.searchRange("id", Value::makeInt(100), Value::makeInt(199));
        CHECK_EQ(r3.size(), static_cast<std::size_t>(100), "rango por B+ Tree");
        CHECK_EQ(t.lastPlan().metodo, std::string("INDEX BPLUS (rango)"), "el B+ resuelve rangos");
    }

    SECTION("Table con Hash: los rangos caen a full scan");
    {
        resetFile("data/t_tiendas.dat");
        resetFile("data/t_tiendas_id.idx");
        Table t("data", tiendasInfo(IndexKind::HASH));
        for (int i = 0; i < 2000; ++i)
            t.insert(Tuple{{Value::makeInt(i), Value::makeStr("Tienda " + std::to_string(i)),
                            Value::makeDouble(i * 1.5)}});

        auto r = t.searchEq("id", Value::makeInt(1999));
        CHECK_EQ(r.size(), static_cast<std::size_t>(1), "busqueda puntual por hash");
        CHECK_EQ(t.lastPlan().metodo, std::string("INDEX HASH"), "uso el hash");

        auto r3 = t.searchRange("id", Value::makeInt(100), Value::makeInt(199));
        CHECK_EQ(r3.size(), static_cast<std::size_t>(100), "el rango igual devuelve lo correcto");
        CHECK(t.lastPlan().metodo.find("SEQ SCAN") == 0, "pero resuelto con full scan");

        SECTION("borrado coherente entre heap e indice");
        auto antes = t.searchEq("id", Value::makeInt(500));
        CHECK_EQ(antes.size(), static_cast<std::size_t>(1), "existe antes de borrar");
        auto rids = t.searchEq("id", Value::makeInt(500));
        (void)rids;
        // localizar el RID via scan para borrarlo
        Tuple tmp;
        bool borrado = false;
        for (int p = 0; p < t.heapPages() && !borrado; ++p)
            for (int s = 0; s < 200 && !borrado; ++s) {
                RID rid(p, s);
                if (t.getByRID(rid, tmp) && tmp.at(0).i == 500) { borrado = t.removeByRID(rid); }
            }
        CHECK(borrado, "se borro la fila");
        CHECK(t.searchEq("id", Value::makeInt(500)).empty(), "el indice ya no la devuelve");
    }

    // ------------------------------------------------------------------
    //  Varios indices sobre la MISMA tabla (Entregable 2).
    //  Lo que se verifica aqui no es que cada indice funcione por separado
    //  --eso ya lo hacen test_bplus y test_rtree-- sino que la tabla los
    //  mantenga COHERENTES entre si: un INSERT tiene que entrar en todos y
    //  un DELETE tiene que salir de todos.
    // ------------------------------------------------------------------
    SECTION("dos indices a la vez: B+ sobre la clave y R-Tree sobre un POINT");
    {
        resetFile("data/t_sitios.dat");
        resetFile("data/t_sitios_id.idx");
        resetFile("data/t_sitios_ubic.idx");

        TableInfo ti;
        ti.name       = "sitios";
        ti.heap_file  = "t_sitios.dat";
        ti.key_column = "id";
        ti.schema     = Schema({Column("id", Type::INT),
                                Column("nombre", Type::VARCHAR, 20),
                                Column("ubic", Type::POINT)});
        ti.indexes.push_back(IndexInfo{"id",   IndexKind::BPLUS, "t_sitios_id.idx"});
        ti.indexes.push_back(IndexInfo{"ubic", IndexKind::RTREE, "t_sitios_ubic.idx"});

        const MBR ventana(0.0, 0.0, 100.0, 100.0);
        const int N = 400;
        {
            Table t("data", ti, 32);
            CHECK_EQ(t.numIndices(), static_cast<std::size_t>(2), "la tabla abrio dos indices");

            for (int i = 0; i < N; ++i) {
                // la mitad dentro de la ventana, la mitad fuera
                double x = (i % 2 == 0) ? (i % 100) : (500.0 + i);
                double y = (i % 2 == 0) ? (i % 100) : (500.0 + i);
                t.insert(Tuple{{Value::makeInt(i),
                                Value::makeStr("s" + std::to_string(i)),
                                Value::makePoint(x, y)}});
            }

            SECTION("cada columna usa su propio indice");
            CHECK_EQ(t.searchEq("id", Value::makeInt(123)).size(),
                     static_cast<std::size_t>(1), "busqueda por el B+");
            CHECK_EQ(std::string(t.lastPlan().metodo), std::string("INDEX BPLUS"),
                     "la ruta elegida para 'id' es el B+");
            CHECK_EQ(t.searchWithin("ubic", 0, 0, 100, 100).size(),
                     static_cast<std::size_t>(N / 2), "ventana por el R-Tree");
            CHECK_EQ(std::string(t.lastPlan().metodo), std::string("INDEX RTREE (ventana)"),
                     "la ruta elegida para 'ubic' es el R-Tree");
            CHECK_EQ(t.searchRange("id", Value::makeInt(10), Value::makeInt(19)).size(),
                     static_cast<std::size_t>(10), "rango por el B+");

            SECTION("un INSERT entra en LOS DOS indices");
            t.insert(Tuple{{Value::makeInt(9000), Value::makeStr("nuevo"),
                            Value::makePoint(50.0, 50.0)}});
            CHECK_EQ(t.searchEq("id", Value::makeInt(9000)).size(),
                     static_cast<std::size_t>(1), "lo ve el B+");
            CHECK_EQ(t.searchWithin("ubic", 0, 0, 100, 100).size(),
                     static_cast<std::size_t>(N / 2 + 1), "lo ve el R-Tree");

            SECTION("un DELETE sale de LOS DOS indices");
            std::vector<RID> r = t.searchRIDsEq("id", Value::makeInt(9000));
            CHECK_EQ(r.size(), static_cast<std::size_t>(1), "se localizo la fila a borrar");
            CHECK(t.removeByRID(r[0]), "se borro del heap");
            CHECK(t.searchEq("id", Value::makeInt(9000)).empty(), "el B+ ya no la devuelve");
            CHECK_EQ(t.searchWithin("ubic", 0, 0, 100, 100).size(),
                     static_cast<std::size_t>(N / 2),
                     "el R-Tree tampoco: si quedara, los indices estarian incoherentes");

            SECTION("KNN convive con el indice de la clave");
            CHECK_EQ(t.searchKNN("ubic", 0.0, 0.0, 5).size(),
                     static_cast<std::size_t>(5), "KNN sobre la tabla con dos indices");
            CHECK_EQ(std::string(t.lastPlan().metodo), std::string("INDEX RTREE (knn)"),
                     "el KNN usa el R-Tree, no un escaneo");

            CHECK(t.indexPages() > 0, "indexPages suma las paginas de los dos archivos");
            t.flush();
        }

        SECTION("los dos indices sobreviven a cerrar y reabrir la tabla");
        {
            Table t("data", ti, 32);
            CHECK_EQ(t.numIndices(), static_cast<std::size_t>(2), "dos indices tras reabrir");
            CHECK_EQ(t.searchEq("id", Value::makeInt(123)).size(),
                     static_cast<std::size_t>(1), "el B+ sigue respondiendo");
            CHECK_EQ(t.searchWithin("ubic", 0, 0, 100, 100).size(),
                     static_cast<std::size_t>(N / 2), "el R-Tree sigue respondiendo");
        }

        SECTION("buildIndex reconstruye AMBOS en una sola pasada del heap");
        {
            Table t("data", ti, 32);
            t.buildIndex();
            CHECK_EQ(t.searchEq("id", Value::makeInt(77)).size(),
                     static_cast<std::size_t>(1), "B+ repoblado");
            CHECK_EQ(t.searchWithin("ubic", 0, 0, 100, 100).size(),
                     static_cast<std::size_t>(N / 2), "R-Tree repoblado");
            (void)ventana;
        }
    }

    DONE("test_catalog");
    return 0;
}
