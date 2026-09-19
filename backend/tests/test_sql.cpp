// ============================================================================
//  test_sql.cpp - Parser SQL y ejecutor (Database) de extremo a extremo
// ============================================================================
#include <filesystem>
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
        CHECK_EQ(st.values.size(), static_cast<std::size_t>(4), "cantidad de valores");
        CHECK_EQ(st.values[0].i, static_cast<std::int64_t>(101), "entero");
        CHECK_EQ(st.values[1].s, std::string("Ada Lovelace"), "cadena con espacio");
        CHECK_EQ(st.values[3].d, 5200.0, "decimal");
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

    DONE("test_sql");
    return 0;
}
