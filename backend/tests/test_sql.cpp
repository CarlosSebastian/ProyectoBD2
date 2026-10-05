// ============================================================================
//  test_sql.cpp - Parser SQL y ejecutor (Database) de extremo a extremo
// ============================================================================
#include <filesystem>
#include <fstream>
#include <string>

#include "db/database.hpp"
#include "db/disk_manager.hpp"
#include "db/sql.hpp"
#include "test_util.hpp"

using namespace db;

static bool lanza(const std::string& sql) {
    try { parseSQL(sql); } catch (const DBException&) { return true; }
    return false;
}

int main() {
    SECTION("CREATE TABLE");
    {
        Statement st = parseSQL(
            "CREATE TABLE empleados ( id INT PRIMARY KEY, nombre CHAR(30), "
            "dept CHAR(20), salario FLOAT ) USING HEAP;");
        CHECK_EQ(static_cast<int>(st.kind), static_cast<int>(StmtKind::CREATE_TABLE), "tipo de sentencia");
        CHECK_EQ(st.table, std::string("empleados"), "nombre de tabla");
        CHECK_EQ(st.columns.size(), static_cast<std::size_t>(4), "cantidad de columnas");
        CHECK(st.columns[0].primary_key, "PRIMARY KEY reconocido");
        CHECK_EQ(static_cast<int>(st.columns[1].type), static_cast<int>(Type::VARCHAR), "CHAR -> VARCHAR");
        CHECK_EQ(st.columns[1].max_len, 30, "longitud del CHAR");
        CHECK_EQ(static_cast<int>(st.columns[3].type), static_cast<int>(Type::DOUBLE), "FLOAT -> DOUBLE");
        CHECK_EQ(static_cast<int>(st.engine), static_cast<int>(EngineKind::HEAP), "motor HEAP");

        Statement st2 = parseSQL("CREATE TABLE t (a INT) USING SEQUENTIAL");
        CHECK_EQ(static_cast<int>(st2.engine), static_cast<int>(EngineKind::SEQUENTIAL), "motor SEQUENTIAL");
        Statement st3 = parseSQL("create table t (a int)");          // sin USING y en minusculas
        CHECK_EQ(static_cast<int>(st3.engine), static_cast<int>(EngineKind::HEAP), "motor por defecto HEAP");
    }

    SECTION("CREATE INDEX");
    {
        Statement st = parseSQL("CREATE INDEX idx_emp_id ON empleados ( id ) USING BTREE;");
        CHECK_EQ(static_cast<int>(st.kind), static_cast<int>(StmtKind::CREATE_INDEX), "tipo");
        CHECK_EQ(st.index_name, std::string("idx_emp_id"), "nombre del indice");
        CHECK_EQ(st.table, std::string("empleados"), "tabla");
        CHECK_EQ(st.index_column, std::string("id"), "columna");
        CHECK_EQ(static_cast<int>(st.index_kind), static_cast<int>(IndexKind::BPLUS), "BTREE");
        Statement h = parseSQL("CREATE INDEX i ON t (c) USING HASH;");
        CHECK_EQ(static_cast<int>(h.index_kind), static_cast<int>(IndexKind::HASH), "HASH");
    }

    SECTION("INSERT");
    {
        Statement st = parseSQL("INSERT INTO empleados VALUES (101, 'Ada Lovelace', 'Analytics', 5200.0);");
        CHECK_EQ(static_cast<int>(st.kind), static_cast<int>(StmtKind::INSERT), "tipo");
        CHECK_EQ(st.rows.size(), static_cast<std::size_t>(1), "cantidad de tuplas");
        CHECK_EQ(st.rows[0].size(), static_cast<std::size_t>(4), "cantidad de valores");
        CHECK_EQ(st.rows[0][0].i, static_cast<std::int64_t>(101), "entero");
        CHECK_EQ(st.rows[0][1].s, std::string("Ada Lovelace"), "cadena con espacio");
        CHECK_EQ(st.rows[0][3].d, 5200.0, "decimal");
    }

    SECTION("SELECT y predicados");
    {
        Statement a = parseSQL("SELECT * FROM empleados WHERE id = 101;");
        CHECK(a.select_columns.empty(), "SELECT * sin proyeccion");
        CHECK_EQ(static_cast<int>(a.where.kind), static_cast<int>(PredKind::EQ), "predicado de igualdad");
        CHECK_EQ(a.where.eq.i, static_cast<std::int64_t>(101), "valor del igual");

        Statement b = parseSQL("SELECT * FROM empleados WHERE id >= 100 AND id <= 500;");
        CHECK_EQ(static_cast<int>(b.where.kind), static_cast<int>(PredKind::RANGE), "rango con AND");
        CHECK_EQ(b.where.lo.i, static_cast<std::int64_t>(100), "cota inferior");
        CHECK_EQ(b.where.hi.i, static_cast<std::int64_t>(500), "cota superior");
        CHECK(!b.where.lo_abierto && !b.where.hi_abierto, "rango cerrado por ambos lados");

        Statement c = parseSQL("SELECT id, nombre FROM empleados WHERE id BETWEEN 1 AND 9 LIMIT 5");
        CHECK_EQ(c.select_columns.size(), static_cast<std::size_t>(2), "proyeccion de 2 columnas");
        CHECK_EQ(static_cast<int>(c.where.kind), static_cast<int>(PredKind::RANGE), "BETWEEN es rango");
        CHECK_EQ(c.limit, static_cast<long long>(5), "LIMIT");

        Statement d = parseSQL("SELECT * FROM t WHERE x > 7");
        CHECK(d.where.hi_abierto, "rango abierto por arriba");
        CHECK(!d.where.lo_abierto, "con cota inferior");
        CHECK(b.extra.empty(), "dos cotas de la MISMA columna no generan condicion extra");
    }

    SECTION("WHERE con condiciones sobre columnas distintas");
    {
        // La clave del parser hibrido: las condiciones de la misma columna se
        // fusionan en un rango, las de columnas distintas quedan separadas.
        Statement a = parseSQL("SELECT * FROM t WHERE id > 1 AND ubic WITHIN (0,0,50,50);");
        CHECK_EQ(a.extra.size(), static_cast<std::size_t>(1), "una condicion extra");
        CHECK_EQ(a.where.column, std::string("id"), "la primera queda en where");
        CHECK_EQ(static_cast<int>(a.where.kind), static_cast<int>(PredKind::RANGE), "id es rango");
        CHECK_EQ(a.extra[0].column, std::string("ubic"), "la segunda queda en extra");
        CHECK_EQ(static_cast<int>(a.extra[0].kind), static_cast<int>(PredKind::WITHIN), "ubic es ventana");
        CHECK_EQ(a.extra[0].wx1, 50.0, "esquina de la ventana");

        Statement b2 = parseSQL("SELECT * FROM t WHERE id >= 10 AND id <= 20 AND cat = 'x';");
        CHECK_EQ(b2.extra.size(), static_cast<std::size_t>(1), "el rango de id NO se parte en dos");
        CHECK_EQ(static_cast<int>(b2.where.kind), static_cast<int>(PredKind::RANGE), "rango fusionado");
        CHECK_EQ(b2.where.lo.i, static_cast<std::int64_t>(10), "cota inferior tras fusionar");
        CHECK_EQ(b2.where.hi.i, static_cast<std::int64_t>(20), "cota superior tras fusionar");
        CHECK_EQ(b2.extra[0].column, std::string("cat"), "la condicion de otra columna sobrevive");

        Statement c2 = parseSQL("SELECT * FROM t WHERE a = 1 AND b = 2 AND c = 3;");
        CHECK_EQ(c2.extra.size(), static_cast<std::size_t>(2), "tres columnas = where + 2 extras");

        // El AND del BETWEEN no debe confundirse con el AND que une condiciones
        Statement d2 = parseSQL("SELECT * FROM t WHERE id BETWEEN 1 AND 9 AND cat = 'z';");
        CHECK_EQ(static_cast<int>(d2.where.kind), static_cast<int>(PredKind::RANGE), "BETWEEN sigue siendo rango");
        CHECK_EQ(d2.where.hi.i, static_cast<std::int64_t>(9), "el 9 es la cota, no otra condicion");
        CHECK_EQ(d2.extra.size(), static_cast<std::size_t>(1), "y la condicion de cat se separa");
        CHECK_EQ(d2.extra[0].column, std::string("cat"), "columna de la extra");
    }

    SECTION("DELETE y errores de sintaxis");
    {
        Statement st = parseSQL("DELETE FROM empleados WHERE id = 101;");
        CHECK_EQ(static_cast<int>(st.kind), static_cast<int>(StmtKind::DELETE_), "tipo DELETE");
        CHECK(lanza("DELETE FROM empleados;"), "DELETE sin WHERE se rechaza");
        CHECK(lanza("SELECT * FROM"), "FROM sin tabla se rechaza");
        CHECK(lanza("UPDATE t SET a = 1"), "UPDATE aun no soportado, error claro");
        CHECK(lanza("CREATE TABLE t (a FOO)"), "tipo desconocido se rechaza");
        CHECK(lanza("SELECT * FROM t WHERE a = 'sin cerrar"), "cadena sin cerrar se rechaza");
        CHECK(lanza("SELECT * FROM t basura"), "texto sobrante se rechaza");
    }

    SECTION("Ejecutor end-to-end sobre el motor real");
    {
        // Directorio propio para no pisar el catalogo de los otros tests.
        std::filesystem::create_directories("data/_tsql");
        resetFile("data/_tsql/catalog.txt");
        resetFile("data/_tsql/empleados.dat");
        resetFile("data/_tsql/empleados_id.idx");

        Database db("data/_tsql");

        QueryResult r = db.execute("CREATE TABLE empleados (id INT PRIMARY KEY, nombre CHAR(30), salario FLOAT) USING HEAP;");
        CHECK(r.ok, "CREATE TABLE ejecuta sin error");

        for (int i = 1; i <= 300; ++i) {
            QueryResult ins = db.execute("INSERT INTO empleados VALUES (" + std::to_string(i) +
                                         ", 'emp" + std::to_string(i) + "', " + std::to_string(1000 + i) + ".5);");
            if (!ins.ok) { std::cerr << ins.error << "\n"; }
            CHECK(ins.ok, "INSERT ejecuta sin error");
        }

        QueryResult sinIdx = db.execute("SELECT * FROM empleados WHERE id = 150;");
        CHECK(sinIdx.ok, "SELECT sin indice ejecuta");
        CHECK_EQ(sinIdx.row_count, static_cast<long long>(1), "encuentra la fila");
        CHECK_EQ(sinIdx.metodo, std::string("SEQ SCAN"), "sin indice usa full scan");

        QueryResult ci = db.execute("CREATE INDEX idx_id ON empleados (id) USING BTREE;");
        CHECK(ci.ok, "CREATE INDEX ejecuta");

        QueryResult conIdx = db.execute("SELECT * FROM empleados WHERE id = 150;");
        CHECK(conIdx.ok, "SELECT con indice ejecuta");
        CHECK_EQ(conIdx.row_count, static_cast<long long>(1), "misma fila que sin indice");
        CHECK_EQ(conIdx.metodo, std::string("INDEX BPLUS"), "ahora usa el indice");
        CHECK_EQ(conIdx.columns.size(), static_cast<std::size_t>(3), "columnas del SELECT *");
        CHECK_EQ(conIdx.rows[0][1], std::string("emp150"), "contenido correcto");
        CHECK(conIdx.plan.size() >= 4, "el plan reporta sus etapas");
        CHECK(conIdx.parse_ms >= 0.0, "reporta tiempo de parseo");

        QueryResult rango = db.execute("SELECT id FROM empleados WHERE id >= 100 AND id <= 199;");
        CHECK_EQ(rango.row_count, static_cast<long long>(100), "rango devuelve 100 filas");
        CHECK_EQ(rango.metodo, std::string("INDEX BPLUS (rango)"), "el B+ resuelve el rango");
        CHECK_EQ(rango.columns.size(), static_cast<std::size_t>(1), "proyeccion de una columna");

        QueryResult lim = db.execute("SELECT * FROM empleados WHERE id >= 1 AND id <= 50 LIMIT 7;");
        CHECK_EQ(lim.row_count, static_cast<long long>(7), "LIMIT recorta el resultado");

        QueryResult del = db.execute("DELETE FROM empleados WHERE id = 150;");
        CHECK(del.ok, "DELETE ejecuta");
        CHECK_EQ(del.row_count, static_cast<long long>(1), "borra una fila");
        QueryResult tras = db.execute("SELECT * FROM empleados WHERE id = 150;");
        CHECK_EQ(tras.row_count, static_cast<long long>(0), "la fila ya no aparece por el indice");

        QueryResult err = db.execute("SELECT * FROM noexiste WHERE id = 1;");
        CHECK(!err.ok, "tabla inexistente devuelve error, no crash");
        CHECK(!err.error.empty(), "el error trae mensaje");

        SECTION("motor SEQUENTIAL por SQL");
        resetFile("data/_tsql/ventas.dat");
        resetFile("data/_tsql/ventas.ovf");
        QueryResult seq = db.execute(
            "CREATE TABLE ventas (id INT PRIMARY KEY, glosa CHAR(20)) USING SEQUENTIAL;");
        CHECK(seq.ok, "CREATE TABLE con SEQUENTIAL");
        for (int i = 200; i >= 1; --i) {           // insertados al reves a proposito
            QueryResult x = db.execute("INSERT INTO ventas VALUES (" + std::to_string(i) +
                                       ", 'v" + std::to_string(i) + "');");
            CHECK(x.ok, "INSERT en tabla SEQUENTIAL");
        }
        QueryResult sq = db.execute("SELECT * FROM ventas WHERE id = 137;");
        CHECK(sq.ok, "SELECT puntual sobre SEQUENTIAL");
        CHECK_EQ(sq.row_count, static_cast<long long>(1), "encuentra la fila");
        CHECK_EQ(sq.rows[0][1], std::string("v137"), "contenido correcto");
        CHECK_EQ(sq.metodo, std::string("SEQ BINARY SEARCH"), "uso busqueda binaria, no full scan");

        QueryResult sr = db.execute("SELECT id FROM ventas WHERE id >= 50 AND id <= 59;");
        CHECK_EQ(sr.row_count, static_cast<long long>(10), "rango sobre SEQUENTIAL");
        CHECK_EQ(sr.metodo, std::string("SEQ BINARY SEARCH (rango)"), "rango por busqueda binaria");
        CHECK_EQ(sr.rows[0][0], std::string("50"), "el rango sale en orden de clave");
        CHECK_EQ(sr.rows[9][0], std::string("59"), "hasta el final del rango");

        QueryResult reo = db.reorganize("ventas");
        CHECK(reo.ok, "reorganize ejecuta sobre SEQUENTIAL");
        CHECK(!reo.message.empty(), "reorganize informa el antes y despues");
        QueryResult sq2 = db.execute("SELECT * FROM ventas WHERE id = 137;");
        CHECK_EQ(sq2.row_count, static_cast<long long>(1), "la fila sigue ahi tras reorganizar");

        QueryResult mal = db.reorganize("empleados");
        CHECK(!mal.ok, "reorganize sobre una tabla HEAP se rechaza con motivo");

        SECTION("catalogo y metricas de I/O");
        auto ts = db.tables();
        CHECK_EQ(ts.size(), static_cast<std::size_t>(2), "dos tablas en el catalogo");
        bool hallada = false;
        for (const auto& t : ts)
            if (t.name == "empleados") {
                hallada = true;
                CHECK_EQ(t.engine, std::string("HEAP"), "motor reportado");
                CHECK_EQ(t.indexes.size(), static_cast<std::size_t>(1), "indice reportado");
                CHECK_EQ(t.rows, static_cast<long long>(299), "filas tras el DELETE");
            }
        CHECK(hallada, "la tabla aparece en /api/tables");
        CHECK(conIdx.disk_reads >= 0 && conIdx.disk_writes >= 0, "DiskCounter reporta lecturas y escrituras");
    }

    // ---------------------------------------------------------------------
    //  REGRESION: > y < son ESTRICTOS.
    //  El parser colapsaba > con >= y < con <=, asi que "WHERE id > 3" sobre
    //  1..10 devolvia 8 filas en vez de 7. Los tests anteriores solo miraban
    //  el flag hi_abierto del predicado, nunca la semantica.
    // ---------------------------------------------------------------------
    {
        SECTION("operadores estrictos > y <");
        const std::string DIR = "data/test_sql_estrictos";
        std::filesystem::remove_all(DIR);
        std::filesystem::create_directories(DIR);
        Database db(DIR);
        db.execute("CREATE TABLE r (id INT PRIMARY KEY, v INT)");
        for (int i = 1; i <= 10; ++i)
            db.execute("INSERT INTO r VALUES (" + std::to_string(i) + ", " + std::to_string(i * 10) + ")");

        auto filas = [&](const std::string& sql) {
            QueryResult q = db.execute(sql);
            CHECK(q.ok, "la consulta se ejecuta: " + sql + (q.ok ? "" : " -> " + q.error));
            return q.rows.size();
        };
        CHECK_EQ(filas("SELECT * FROM r WHERE id > 3"),  static_cast<std::size_t>(7), "id > 3 excluye el 3");
        CHECK_EQ(filas("SELECT * FROM r WHERE id >= 3"), static_cast<std::size_t>(8), "id >= 3 incluye el 3");
        CHECK_EQ(filas("SELECT * FROM r WHERE id < 3"),  static_cast<std::size_t>(2), "id < 3 excluye el 3");
        CHECK_EQ(filas("SELECT * FROM r WHERE id <= 3"), static_cast<std::size_t>(3), "id <= 3 incluye el 3");
        CHECK_EQ(filas("SELECT * FROM r WHERE id > 3 AND id < 7"),   static_cast<std::size_t>(3), "ambas cotas estrictas");
        CHECK_EQ(filas("SELECT * FROM r WHERE id >= 3 AND id <= 7"), static_cast<std::size_t>(5), "ambas cotas inclusivas");
        CHECK_EQ(filas("SELECT * FROM r WHERE id > 3 AND id <= 7"),  static_cast<std::size_t>(4), "cotas mixtas");
        CHECK_EQ(filas("SELECT * FROM r WHERE id >= 3 AND id < 7"),  static_cast<std::size_t>(4), "cotas mixtas al reves");

        QueryResult del = db.execute("DELETE FROM r WHERE id > 8");
        CHECK(del.ok, "DELETE con operador estricto");
        CHECK_EQ(del.row_count, static_cast<long long>(2), "DELETE id > 8 borra solo el 9 y el 10");
        CHECK_EQ(filas("SELECT * FROM r"), static_cast<std::size_t>(8), "quedan 8 filas");
    }

    // ---------------------------------------------------------------------
    //  REGRESION: un CREATE INDEX invalido no puede dejar la tabla inservible.
    //  El indice se escribia en catalog.txt ANTES de validar el tipo, asi que
    //  al rechazarlo el catalogo quedaba con un indice imposible y la tabla
    //  no se podia volver a abrir NUNCA (no hay DROP INDEX).
    // ---------------------------------------------------------------------
    {
        SECTION("un CREATE INDEX rechazado deja la tabla intacta");
        const std::string DIR = "data/test_sql_idxfloat";
        std::filesystem::remove_all(DIR);
        std::filesystem::create_directories(DIR);
        Database db(DIR);
        db.execute("CREATE TABLE f (id INT PRIMARY KEY, sal FLOAT)");
        db.execute("INSERT INTO f VALUES (1, 2.5)");

        QueryResult mal = db.execute("CREATE INDEX fx ON f (sal) USING BTREE");
        CHECK(!mal.ok, "indexar una columna FLOAT se rechaza");

        QueryResult sel = db.execute("SELECT * FROM f");
        CHECK(sel.ok, "la tabla se sigue pudiendo leer tras el rechazo");
        CHECK_EQ(sel.rows.size(), static_cast<std::size_t>(1), "y conserva sus filas");
        CHECK(db.execute("INSERT INTO f VALUES (2, 3.5)").ok, "y se sigue pudiendo insertar");

        Database db2(DIR);                       // reabrir: el catalogo quedo limpio
        QueryResult tras = db2.execute("SELECT * FROM f");
        CHECK(tras.ok, "tras reiniciar el servidor la tabla sigue accesible");
        CHECK_EQ(tras.rows.size(), static_cast<std::size_t>(2), "con las dos filas");
    }

    // ---------------------------------------------------------------------
    //  REGRESION: un .idx que quedo de un indice ANTERIOR, de otro tipo.
    //  CREATE INDEX no truncaba el archivo, asi que el constructor del B+ veia
    //  un archivo no vacio, reutilizaba la META de un hash y el descenso
    //  terminaba pidiendo una pagina inexistente:
    //      "readPage: pagina fuera de rango: 245"
    //  Ahora cada indice firma su META, y un archivo del tipo equivocado se
    //  descarta y se reconstruye desde los datos.
    // ---------------------------------------------------------------------
    {
        SECTION("un .idx viejo de otro tipo no inutiliza la tabla");
        const std::string DIR = "data/test_sql_idxviejo";
        std::filesystem::remove_all(DIR);
        std::filesystem::create_directories(DIR);
        {
            Database db(DIR);
            db.execute("CREATE TABLE t (id INT PRIMARY KEY, v CHAR(20))");
            for (int i = 1; i <= 300; ++i)
                db.execute("INSERT INTO t VALUES (" + std::to_string(i) + ", 'x')");
            QueryResult h = db.execute("CREATE INDEX ix ON t (id) USING HASH");
            CHECK(h.ok, "se crea primero un indice HASH");
            CHECK_EQ(db.execute("SELECT * FROM t WHERE id = 150").rows.size(),
                     static_cast<std::size_t>(1), "el hash responde");
        }
        // El catalogo pasa a declarar BPLUS, pero el .idx sigue siendo el hash.
        {
            std::ofstream c(DIR + "/catalog.txt", std::ios::trunc);
            c << "TABLE t t.dat HEAP id\n"
                 "COL id INT 0\n"
                 "COL v VARCHAR 20\n"
                 "INDEX id BPLUS t_id.idx\n"
                 "END\n";
        }
        {
            Database db(DIR);
            QueryResult r = db.execute("SELECT * FROM t WHERE id = 150");
            CHECK(r.ok, std::string("la tabla sigue accesible") + (r.ok ? "" : " -> " + r.error));
            CHECK_EQ(r.rows.size(), static_cast<std::size_t>(1), "y devuelve la fila correcta");
            CHECK_EQ(r.metodo, std::string("INDEX BPLUS"), "reconstruido como B+, que es lo que dice el catalogo");
            CHECK_EQ(db.execute("SELECT * FROM t WHERE id > 297").rows.size(),
                     static_cast<std::size_t>(3), "y los rangos tambien funcionan");
        }
    }

    // ------------------------------------------------------------------
    //  Plan hibrido: una condicion conduce el acceso, las demas filtran.
    //  Se contrasta cada consulta contra el calculo a mano sobre los mismos
    //  datos, porque lo que puede salir mal aqui no es que falle sino que
    //  devuelva (o borre) filas de mas sin avisar.
    // ------------------------------------------------------------------
    SECTION("planificador hibrido: relacional + espacial en el mismo WHERE");
    {
        const std::string DIR = "data/_thib";
        std::filesystem::remove_all(DIR);
        std::filesystem::create_directories(DIR);
        Database db(DIR);

        CHECK(db.execute("CREATE TABLE lug (id INT PRIMARY KEY, cat CHAR(8), ubic POINT);").ok,
              "tabla con columna POINT");

        // Rejilla determinista de 20x20 = 400 filas en [0,19]x[0,19].
        int esperado_ventana = 0, esperado_ambas = 0, esperado_tres = 0;
        for (int i = 0; i < 400; ++i) {
            int x = i % 20, y = i / 20;
            std::string cat = "c" + std::to_string(i % 4);
            db.execute("INSERT INTO lug VALUES (" + std::to_string(i) + ",'" + cat + "',POINT(" +
                       std::to_string(x) + "," + std::to_string(y) + "));");
            const bool en_ventana = (x >= 0 && x <= 9 && y >= 0 && y <= 9);
            if (en_ventana)                            ++esperado_ventana;
            if (en_ventana && i > 100)                 ++esperado_ambas;
            if (en_ventana && i > 100 && cat == "c2")  ++esperado_tres;
        }
        CHECK(db.execute("CREATE INDEX ix_id ON lug (id) USING BTREE;").ok,  "B+ sobre la clave");
        CHECK(db.execute("CREATE INDEX ix_ub ON lug (ubic) USING RTREE;").ok, "R-Tree sobre el punto");

        SECTION("la consulta que antes fallaba ahora responde, y responde bien");
        {
            QueryResult r = db.execute("SELECT * FROM lug WHERE id > 100 AND ubic WITHIN (0,0,9,9);");
            CHECK(r.ok, std::string("ya no es error de sintaxis") + (r.ok ? "" : " -> " + r.error));
            CHECK_EQ(r.rows.size(), static_cast<std::size_t>(esperado_ambas),
                     "mismas filas que el calculo a mano");
            CHECK_EQ(r.metodo, std::string("INDEX RTREE (ventana)"),
                     "conduce el R-Tree: la ventana es mas selectiva que el rango");
        }

        SECTION("una igualdad indexada le gana a la ventana aunque la ventana sea barata");
        {
            // La ventana cubre la tabla entera; la igualdad devuelve una fila.
            QueryResult r = db.execute("SELECT * FROM lug WHERE ubic WITHIN (0,0,19,19) AND id = 77;");
            CHECK(r.ok, "consulta valida");
            CHECK_EQ(r.rows.size(), static_cast<std::size_t>(1), "una sola fila");
            CHECK_EQ(r.metodo, std::string("INDEX BPLUS"),
                     "conduce la igualdad, no la ventana que leeria las 400 filas");
        }

        SECTION("tres condiciones, una de ellas sobre una columna sin indice");
        {
            QueryResult r = db.execute(
                "SELECT * FROM lug WHERE cat = 'c2' AND id > 100 AND ubic WITHIN (0,0,9,9);");
            CHECK(r.ok, "consulta valida");
            CHECK_EQ(r.rows.size(), static_cast<std::size_t>(esperado_tres),
                     "mismas filas que el calculo a mano");
        }

        SECTION("el orden en que se escriben las condiciones no cambia el resultado");
        {
            QueryResult a2 = db.execute("SELECT * FROM lug WHERE id > 100 AND ubic WITHIN (0,0,9,9);");
            QueryResult b3 = db.execute("SELECT * FROM lug WHERE ubic WITHIN (0,0,9,9) AND id > 100;");
            CHECK_EQ(a2.rows.size(), b3.rows.size(), "mismo numero de filas en ambos ordenes");
            CHECK_EQ(a2.metodo, b3.metodo, "y la misma ruta de acceso elegida");
        }

        SECTION("DELETE hibrido: el filtro residual evita borrar de mas");
        {
            QueryResult solo_ventana = db.execute("SELECT * FROM lug WHERE ubic WITHIN (0,0,9,9);");
            CHECK_EQ(solo_ventana.rows.size(), static_cast<std::size_t>(esperado_ventana),
                     "la ventana sola trae muchas mas filas");
            CHECK(esperado_ventana > esperado_ambas,
                  "el caso solo prueba algo si la ventana es mas amplia que la interseccion");

            QueryResult d3 = db.execute("DELETE FROM lug WHERE id > 100 AND ubic WITHIN (0,0,9,9);");
            CHECK(d3.ok, "el DELETE se ejecuta");
            CHECK_EQ(d3.row_count, static_cast<long long>(esperado_ambas),
                     "borra SOLO las que cumplen las dos condiciones");

            CHECK_EQ(db.execute("SELECT * FROM lug;").rows.size(),
                     static_cast<std::size_t>(400 - esperado_ambas), "el resto de la tabla sigue ahi");
            CHECK_EQ(db.execute("SELECT * FROM lug WHERE ubic WITHIN (0,0,9,9);").rows.size(),
                     static_cast<std::size_t>(esperado_ventana - esperado_ambas),
                     "y el R-Tree quedo coherente tras el borrado");
        }
    }

    SECTION("POLYGON de extremo a extremo por SQL");
    {
        const std::string DIR = "data/_tpoly";
        std::filesystem::remove_all(DIR);
        std::filesystem::create_directories(DIR);
        Database db(DIR);

        CHECK(db.execute("CREATE TABLE zonas (id INT PRIMARY KEY, nombre CHAR(20), area POLYGON);").ok,
              "tabla con columna POLYGON");
        QueryResult ins = db.execute(
            "INSERT INTO zonas VALUES "
            "(1,'Cuadrado',POLYGON((0,0),(10,0),(10,10),(0,10))),"
            "(2,'Triangulo',POLYGON((20,0),(30,0),(25,10))),"
            "(3,'Diagonal',POLYGON((0,0),(100,100),(99,100),(0,1)));");
        CHECK(ins.ok, std::string("INSERT de poligonos") + (ins.ok ? "" : " -> " + ins.error));
        CHECK(db.execute("CREATE INDEX ix ON zonas (area) USING RTREE;").ok,
              "R-Tree sobre una columna POLYGON");

        SECTION("el poligono sobrevive ida y vuelta por disco");
        {
            QueryResult r = db.execute("SELECT * FROM zonas WHERE id = 2;");
            CHECK_EQ(r.rows.size(), static_cast<std::size_t>(1), "una fila");
            CHECK(r.rows[0][2].find("POLYGON(") == 0, "se reimprime como POLYGON(...)");
            CHECK(r.rows[0][2].find("20.000000,0.000000") != std::string::npos,
                  "con sus vertices intactos");
        }

        SECTION("ST_CONTAINS distingue la caja de la figura");
        {
            CHECK_EQ(db.execute("SELECT * FROM zonas WHERE ST_CONTAINS(area, POINT(5,5));").rows.size(),
                     static_cast<std::size_t>(1), "(5,5) cae en el cuadrado");
            CHECK_EQ(db.execute("SELECT * FROM zonas WHERE ST_CONTAINS(area, POINT(25,2));").rows.size(),
                     static_cast<std::size_t>(1), "(25,2) cae en el triangulo");
            // (21,9) esta dentro de la CAJA del triangulo pero fuera del triangulo.
            QueryResult r = db.execute("SELECT * FROM zonas WHERE ST_CONTAINS(area, POINT(21,9));");
            CHECK_EQ(r.rows.size(), static_cast<std::size_t>(0),
                     "(21,9) esta en la caja pero no en ninguna figura");
            CHECK_EQ(r.metodo, std::string("INDEX RTREE (contiene + refinamiento)"),
                     "y el plan declara que hubo refinamiento");
        }

        SECTION("WITHIN sobre poligonos descarta los falsos positivos del MBR");
        {
            // La caja de 'Diagonal' cubre todo el cuadrante, la figura casi nada.
            QueryResult falso = db.execute("SELECT * FROM zonas WHERE area WITHIN (90,0,95,5);");
            CHECK_EQ(falso.rows.size(), static_cast<std::size_t>(0),
                     "la ventana toca la caja de la diagonal, no la diagonal");
            QueryResult cierto = db.execute("SELECT * FROM zonas WHERE area WITHIN (90,88,95,95);");
            CHECK_EQ(cierto.rows.size(), static_cast<std::size_t>(1),
                     "donde si pasa la diagonal, aparece");
            CHECK_EQ(cierto.rows[0][1], std::string("Diagonal"), "y es la que toca");
        }

        SECTION("un indice que no es RTREE sobre POLYGON se rechaza con la tabla intacta");
        {
            QueryResult r = db.execute("CREATE INDEX mal ON zonas (nombre) USING BTREE;");
            CHECK(r.ok, "un B+ sobre la columna de texto si vale");
            QueryResult r2 = db.execute("CREATE INDEX peor ON zonas (id) USING RTREE;");
            CHECK(!r2.ok, "un RTREE sobre un INT no");
            CHECK(db.execute("SELECT * FROM zonas WHERE ST_CONTAINS(area, POINT(5,5));").ok,
                  "y la tabla sigue respondiendo");
        }
    }

    // ------------------------------------------------------------------
    //  Regresiones encontradas revisando la Parte 2.
    // ------------------------------------------------------------------
    SECTION("un WHERE junto a ORDER BY por distancia NO se puede ignorar");
    {
        const std::string DIR = "data/_tknnf";
        std::filesystem::remove_all(DIR);
        std::filesystem::create_directories(DIR);
        Database db(DIR);
        db.execute("CREATE TABLE g (id INT PRIMARY KEY, cat CHAR(10), ubic POINT);");

        // 400 puntos en fila sobre el eje X. Los 300 primeros --o sea los 300
        // MAS CERCANOS al origen-- no cumplen el filtro. Si el motor pidiera k
        // vecinos y filtrara despues sin sobre-pedir, devolveria vacio.
        std::string ins = "INSERT INTO g VALUES ";
        for (int i = 1; i <= 400; ++i) {
            if (i > 1) ins += ",";
            ins += "(" + std::to_string(i) + ",'" + (i > 300 ? "bueno" : "malo") +
                   "',POINT(" + std::to_string(i) + ",0))";
        }
        CHECK(db.execute(ins + ";").ok, "carga de 400 puntos");
        CHECK(db.execute("CREATE INDEX ix ON g (ubic) USING RTREE;").ok, "R-Tree");
        CHECK_EQ(db.execute("SELECT * FROM g WHERE cat = 'bueno';").rows.size(),
                 static_cast<std::size_t>(100), "hay 100 filas que cumplen el filtro");

        QueryResult sin = db.execute("SELECT * FROM g ORDER BY ubic <-> POINT(0,0) LIMIT 5;");
        CHECK_EQ(sin.rows[0][0], std::string("1"), "sin filtro el mas cercano es el 1");

        QueryResult con = db.execute(
            "SELECT * FROM g WHERE cat = 'bueno' ORDER BY ubic <-> POINT(0,0) LIMIT 5;");
        CHECK_EQ(con.rows.size(), static_cast<std::size_t>(5), "devuelve los 5 pedidos");
        CHECK_EQ(con.rows[0][0], std::string("301"),
                 "y el primero es el 301: el WHERE se aplico de verdad");
        CHECK_EQ(con.rows[4][0], std::string("305"), "y siguen en orden de distancia");

        SECTION("la sobre-peticion se detiene cuando el indice se agota");
        {
            QueryResult r = db.execute(
                "SELECT * FROM g WHERE cat = 'bueno' ORDER BY ubic <-> POINT(0,0) LIMIT 150;");
            CHECK_EQ(r.rows.size(), static_cast<std::size_t>(100),
                     "pedir 150 cuando solo hay 100 devuelve 100, no cuelga");
            QueryResult vacio = db.execute(
                "SELECT * FROM g WHERE id > 99999 ORDER BY ubic <-> POINT(0,0) LIMIT 3;");
            CHECK_EQ(vacio.rows.size(), static_cast<std::size_t>(0),
                     "un filtro imposible devuelve vacio sin colgarse");
        }

        SECTION("dos condiciones mas el KNN");
        {
            QueryResult r = db.execute("SELECT * FROM g WHERE cat = 'bueno' AND id > 350 "
                                       "ORDER BY ubic <-> POINT(0,0) LIMIT 4;");
            CHECK_EQ(r.rows.size(), static_cast<std::size_t>(4), "cuatro filas");
            CHECK_EQ(r.rows[0][0], std::string("351"), "la primera cumple ambas condiciones");
        }

        SECTION("tambien con ST_DISTANCE, sobre coordenadas geograficas de verdad");
        {
            // OJO: la tabla 'g' pone los puntos en longitudes 1..400 grados, que
            // sobre la esfera NO son una fila recta: la longitud 360 es la 0, o
            // sea que ese punto cae encima del origen. Para probar el filtro con
            // distancia geografica hace falta un dominio que no de la vuelta.
            db.execute("CREATE TABLE gg (id INT PRIMARY KEY, cat CHAR(10), ubic POINT);");
            std::string ins2 = "INSERT INTO gg VALUES ";
            for (int i = 1; i <= 400; ++i) {
                if (i > 1) ins2 += ",";
                // longitudes de 0.1 a 40 grados: dentro de rango y monotonas
                ins2 += "(" + std::to_string(i) + ",'" + (i > 300 ? "bueno" : "malo") +
                        "',POINT(" + std::to_string(i / 10.0) + ",0))";
            }
            CHECK(db.execute(ins2 + ";").ok, "carga con longitudes en rango");
            CHECK(db.execute("CREATE INDEX ix2 ON gg (ubic) USING RTREE;").ok, "R-Tree");

            QueryResult r = db.execute("SELECT * FROM gg WHERE cat = 'bueno' "
                                       "ORDER BY ST_DISTANCE(ubic, POINT(0,0)) LIMIT 2;");
            CHECK_EQ(r.rows.size(), static_cast<std::size_t>(2), "dos filas");
            CHECK_EQ(r.rows[0][0], std::string("301"), "el filtro tambien se aplica en el geo");
            CHECK_EQ(r.rows[1][0], std::string("302"), "y el orden por metros se respeta");
        }

        SECTION("la longitud da la vuelta: es correcto, pero conviene verlo");
        {
            // Sobre la tabla 'g' (longitudes hasta 400 grados) el vecino
            // geografico mas cercano al origen NO es el 301 sino el 360, porque
            // 360 grados de longitud es el mismo meridiano que 0. El motor no
            // valida el rango de las coordenadas: ST_DISTANCE interpreta la
            // columna como (lon, lat) y hace la cuenta que toca.
            QueryResult r = db.execute("SELECT * FROM g WHERE cat = 'bueno' "
                                       "ORDER BY ST_DISTANCE(ubic, POINT(0,0)) LIMIT 1;");
            CHECK_EQ(r.rows[0][0], std::string("360"),
                     "la longitud 360 cae sobre el origen: distancia cero");
        }
    }

    SECTION("la igualdad sobre un POLYGON usa su caja, no el origen");
    {
        const std::string DIR = "data/_tpeq";
        std::filesystem::remove_all(DIR);
        std::filesystem::create_directories(DIR);
        Database db(DIR);
        db.execute("CREATE TABLE z (id INT PRIMARY KEY, area POLYGON);");
        // El 1 y el 3 COMPARTEN caja envolvente pero son figuras distintas:
        // el indice los devuelve a los dos y el refinamiento tiene que separarlos.
        db.execute("INSERT INTO z VALUES "
                   "(1,POLYGON((0,0),(10,0),(10,10),(0,10))),"
                   "(2,POLYGON((20,20),(30,20),(30,30),(20,30))),"
                   "(3,POLYGON((0,0),(10,0),(10,10),(5,5)));");
        db.execute("CREATE INDEX c ON z (area) USING RTREE;");

        QueryResult r = db.execute("SELECT * FROM z WHERE area = POLYGON((20,20),(30,20),(30,30),(20,30));");
        CHECK_EQ(r.rows.size(), static_cast<std::size_t>(1), "encuentra el poligono lejos del origen");
        CHECK_EQ(r.rows[0][0], std::string("2"), "y es el correcto");

        QueryResult r2 = db.execute("SELECT * FROM z WHERE area = POLYGON((0,0),(10,0),(10,10),(0,10));");
        CHECK_EQ(r2.rows.size(), static_cast<std::size_t>(1),
                 "de dos figuras con la misma caja devuelve solo la que coincide");
        CHECK_EQ(r2.rows[0][0], std::string("1"), "y es la correcta");
        CHECK_EQ(r2.metodo, std::string("INDEX RTREE (igualdad + refinamiento)"),
                 "el plan declara el refinamiento");
    }

    SECTION("borrar un poligono lo saca tambien del indice espacial");
    {
        const std::string DIR = "data/_tpdel";
        std::filesystem::remove_all(DIR);
        std::filesystem::create_directories(DIR);
        Database db(DIR);
        db.execute("CREATE TABLE z (id INT PRIMARY KEY, area POLYGON);");
        db.execute("INSERT INTO z VALUES (1,POLYGON((0,0),(10,0),(10,10),(0,10))),"
                   "(2,POLYGON((20,20),(30,20),(30,30),(20,30)));");
        db.execute("CREATE INDEX c ON z (area) USING RTREE;");
        CHECK_EQ(db.execute("SELECT * FROM z WHERE ST_CONTAINS(area, POINT(5,5));").rows.size(),
                 static_cast<std::size_t>(1), "antes de borrar lo encuentra");
        CHECK_EQ(db.execute("DELETE FROM z WHERE id = 1;").row_count,
                 static_cast<long long>(1), "se borro una fila");
        CHECK_EQ(db.execute("SELECT * FROM z WHERE ST_CONTAINS(area, POINT(5,5));").rows.size(),
                 static_cast<std::size_t>(0), "el R-Tree ya no lo devuelve");
        CHECK_EQ(db.execute("SELECT * FROM z;").rows.size(),
                 static_cast<std::size_t>(1), "y la otra fila sigue ahi");
    }

    DONE("test_sql");
    return 0;
}
