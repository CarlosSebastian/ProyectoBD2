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
//
//  Tipos: INT | FLOAT | DOUBLE | CHAR(n) | VARCHAR(n) | POINT
//         FLOAT se mapea a DOUBLE y CHAR(n) a VARCHAR(n).
//
//  Espacial (Entregable 2):
//    Un literal POINT se escribe POINT(x, y).
//    'col WITHIN (...)' es la consulta de ventana que resuelve el R-Tree.
//    'ORDER BY col <-> POINT(x,y) LIMIT k' es la consulta de k vecinos mas
//    cercanos; el operador <-> es la distancia euclidiana, igual que en
//    PostGIS/pgvector.
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

enum class PredKind { NONE, EQ, RANGE, WITHIN };

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
    Predicate                where;
    long long                limit = -1;

    // ORDER BY col <-> POINT(x,y):  k vecinos mas cercanos.
    // Con LIMIT k se despacha al R-Tree; sin LIMIT se ordena todo el resultado.
    bool        knn = false;
    std::string knn_column;
    double      knn_x = 0.0, knn_y = 0.0;
};

// Lanza DBException con un mensaje legible si la sentencia no es valida.
Statement parseSQL(const std::string& sql);                     // una sola sentencia (comportamiento actual)
std::vector<Statement> parseSQLMultiple(const std::string& sql); // varias, separadas por ';'

}  // namespace db
