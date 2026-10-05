// ============================================================================
//  sql.hpp - Lexer y parser SQL (descenso recursivo, escrito a mano)
//
//  Gramatica soportada (la que exige el enunciado):
//
//    CREATE TABLE t ( col TIPO [PRIMARY KEY], ... ) [USING HEAP|SEQUENTIAL];
//    CREATE INDEX nombre ON t ( col ) [USING BTREE|HASH|RTREE];
//    INSERT INTO t VALUES ( v1, v2, ... ) [, ( ... ) ...];
//    SELECT * | c1,c2 FROM t [WHERE <cond>]
//           [ORDER BY col <-> POINT(x,y)] [LIMIT n];
//    DELETE FROM t WHERE <cond>;
//
//    <cond> ::= col = v
//             | col BETWEEN a AND b
//             | col >= a AND col <= b     (cualquier combinacion de < <= > >=)
//             | col > a                   (rango abierto por un extremo)
//             | col WITHIN (minx, miny, maxx, maxy)   -- ventana espacial
//             | <cond> AND <cond>         sobre columnas DISTINTAS
//
//  Varias condiciones unidas por AND: las que caen sobre la misma columna se
//  fusionan en un solo rango; las de columnas distintas quedan como una lista.
//  El planificador elige UNA para resolver el acceso y aplica el resto como
//  filtro sobre las filas recuperadas.
//
//  Tipos: INT | FLOAT | DOUBLE | CHAR(n) | VARCHAR(n) | POINT | POLYGON
//         FLOAT se mapea a DOUBLE y CHAR(n) a VARCHAR(n).
//
//  Espacial (Entregable 2):
//    Un literal POINT se escribe POINT(x, y).
//    'col WITHIN (...)' es la consulta de ventana que resuelve el R-Tree.
//    'ORDER BY col <-> POINT(x,y) LIMIT k' es la consulta de k vecinos mas
//    cercanos; el operador <-> es la distancia euclidiana EN GRADOS, igual que
//    en pgvector.
//    ST_DISTANCE(col, POINT(lon,lat)) es la distancia geografica real EN
//    METROS (haversine), e interpreta x = longitud e y = latitud. Sirve en dos
//    sitios:
//       WHERE ST_DISTANCE(ubic, POINT(-77.03,-12.05)) <= 5000     -- radio
//       ORDER BY ST_DISTANCE(ubic, POINT(-77.03,-12.05)) LIMIT 5  -- KNN
//    Un POLYGON se escribe POLYGON((x1,y1),(x2,y2),...) y el anillo se cierra
//    solo. ST_CONTAINS(col, POINT(x,y)) devuelve los poligonos que contienen
//    al punto; el R-Tree filtra por caja envolvente y despues se refina con la
//    geometria real.
// ============================================================================
#pragma once

#include <string>
#include <vector>

#include "db/catalog.hpp"
#include "db/record.hpp"

namespace db {

enum class StmtKind { CREATE_TABLE, CREATE_INDEX, INSERT, SELECT, DELETE_ };

enum class EngineKind { HEAP, SEQUENTIAL };
std::string engineName(EngineKind e);
EngineKind  engineFromName(const std::string& s);

struct ColumnDef {
    std::string name;
    Type        type    = Type::INT;
    int         max_len = 0;
    bool        primary_key = false;
};

enum class PredKind { NONE, EQ, RANGE, WITHIN, RADIO, CONTIENE };

struct Predicate {
    PredKind    kind = PredKind::NONE;
    std::string column;
    Value       eq;              // para EQ
    Value       lo, hi;          // para RANGE
    bool        lo_abierto = false;   // true => sin cota inferior
    bool        hi_abierto = false;   // true => sin cota superior
    // El motor solo sabe de rangos CERRADOS, asi que > y < se resuelven
    // pidiendo [lo, hi] y descartando despues los valores iguales a la cota.
    bool        lo_estricto = false;  // true => la cota inferior es > y no >=
    bool        hi_estricto = false;  // true => la cota superior es < y no <=

    // WITHIN: rectangulo de busqueda. Se guardan como doubles sueltos para no
    // arrastrar rtree.hpp (y con el buffer_pool) hasta el parser.
    double      wx0 = 0.0, wy0 = 0.0, wx1 = 0.0, wy1 = 0.0;

    // RADIO: ST_DISTANCE(col, POINT(lon,lat)) <= metros
    double      qlon = 0.0, qlat = 0.0, metros = 0.0;
    bool        radio_estricto = false;      // true => '<' en vez de '<='
};

struct Statement {
    StmtKind    kind = StmtKind::SELECT;
    std::string table;

    // CREATE TABLE
    std::vector<ColumnDef> columns;
    EngineKind             engine = EngineKind::HEAP;

    // CREATE INDEX
    std::string index_name;
    std::string index_column;
    IndexKind   index_kind = IndexKind::BPLUS;

    // INSERT
    std::vector<std::vector<Value>> rows; 

    // SELECT / DELETE
    std::vector<std::string> select_columns;   // vacio => SELECT *
    Predicate                where;            // primera condicion
    // Condiciones adicionales sobre OTRAS columnas. El planificador puede
    // ascender cualquiera de ellas a conductora del acceso; las que queden
    // se evaluan como filtro en memoria.
    std::vector<Predicate>   extra;
    long long                limit = -1;

    // ORDER BY col <-> POINT(x,y):  k vecinos mas cercanos.
    // Con LIMIT k se despacha al R-Tree; sin LIMIT se ordena todo el resultado.
    bool        knn = false;
    std::string knn_column;
    double      knn_x = 0.0, knn_y = 0.0;
    // true si se ordeno por ST_DISTANCE (metros sobre la esfera) en vez de por
    // <-> (euclidiana en grados).
    bool        knn_geo = false;
};

// Lanza DBException con un mensaje legible si la sentencia no es valida.
Statement parseSQL(const std::string& sql);                     // una sola sentencia (comportamiento actual)
std::vector<Statement> parseSQLMultiple(const std::string& sql); // varias, separadas por ';'

}  // namespace db
