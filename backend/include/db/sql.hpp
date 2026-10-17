// ============================================================================
//  sql.hpp - Lexer y parser SQL (descenso recursivo, escrito a mano)
//
//  Gramatica soportada (la que exige el enunciado):
//
//    CREATE TABLE t ( col TIPO [PRIMARY KEY], ... ) [USING HEAP|SEQUENTIAL];
//    CREATE INDEX nombre ON t ( col ) [USING BTREE|HASH|RTREE|INVERTED|IVF]
//                                     [LANGUAGE 'EN'|'ES']
//                                     [LISTS n] [METRIC COSINE|L2];
//    INSERT INTO t VALUES ( v1, v2, ... ) [, ( ... ) ...];
//    SELECT * | c1,c2 FROM t [WHERE <cond>]
//           [ORDER BY col <-> POINT(x,y) | col <=> [v1,...,vd]]
//           [LIMIT n] [PROBES n];
//    DELETE FROM t WHERE <cond>;
//    COPY t FROM 'ruta.csv' [HEADER];   -- carga masiva desde un CSV
//
//    <cond> ::= col = v
//             | col BETWEEN a AND b
//             | col >= a AND col <= b     (cualquier combinacion de < <= > >=)
//             | col > a                   (rango abierto por un extremo)
//             | col WITHIN (minx, miny, maxx, maxy)   -- ventana espacial
//             | col MATCH 'palabras' [USING BM25|TFIDF]  -- relevancia textual
//             | col <=> [v1,...,vd] <= d  -- similitud vectorial por umbral
//             | <cond> AND <cond>         sobre columnas DISTINTAS
//
//  Varias condiciones unidas por AND: las que caen sobre la misma columna se
//  fusionan en un solo rango; las de columnas distintas quedan como una lista.
//  El planificador elige UNA para resolver el acceso y aplica el resto como
//  filtro sobre las filas recuperadas.
//
//  Tipos: INT | FLOAT | DOUBLE | CHAR(n) | VARCHAR(n) | POINT | POLYGON
//         | VECTOR(d) | EMBEDDING(d)
//         FLOAT se mapea a DOUBLE y CHAR(n) a VARCHAR(n). EMBEDDING(d) es un
//         sinonimo de VECTOR(d); la dimension es OBLIGATORIA, porque un
//         embedding sin dimension declarada no se puede indexar.
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
//
//  Vectorial (Entregable 2):
//    Un literal vector se escribe entre corchetes: [0.1, -0.5, 2e-3]. Se
//    aceptan exponentes porque un embedding normalizado suele venir impreso en
//    notacion cientifica.
//    Los operadores son DISTANCIAS (no similitudes), para que "mas parecido"
//    sea siempre "menor valor" y el mismo ORDER BY ... LIMIT k sirva igual que
//    en el modulo espacial:
//       <=>  distancia coseno
//       <->  distancia euclidiana (L2); con un POINT a la derecha es la
//            consulta espacial de la parte 2, con un vector es la vectorial
//    La metrica que pide el operador tiene que coincidir con la del indice: no
//    se puede consultar en coseno un IVF entrenado en L2.
//       ORDER BY emb <=> [..] LIMIT 5 PROBES 16      -- k vecinos aproximados
//       WHERE emb <=> [..] <= 0.35                   -- busqueda por umbral
//    PROBES n es el numero de celdas del IVF que se visitan: es el dial entre
//    recall y latencia. Sin PROBES se usa round(sqrt(nlist)).
// ============================================================================
#pragma once

#include <string>
#include <vector>

#include "db/catalog.hpp"
#include "db/record.hpp"

namespace db {

enum class StmtKind { CREATE_TABLE, CREATE_INDEX, INSERT, SELECT, DELETE_, COPY };

enum class EngineKind { HEAP, SEQUENTIAL };
std::string engineName(EngineKind e);
EngineKind  engineFromName(const std::string& s);

struct ColumnDef {
    std::string name;
    Type        type    = Type::INT;
    int         max_len = 0;
    bool        primary_key = false;
};

enum class PredKind { NONE, EQ, RANGE, WITHIN, RADIO, CONTIENE, MATCH, VECDIST };

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

    // MATCH: texto de la consulta en bruto. Lo analiza el indice invertido con
    // SU idioma, porque consulta y documentos tienen que pasar por el mismo
    // tokenizador o los terminos no coinciden.
    std::string consulta;
    bool        usar_tfidf = false;          // false => BM25 (el de por defecto)

    // VECDIST: col <=> [..] <= umbral. El vector se guarda tal como se escribio
    // y la validacion de la dimension se hace al ejecutar, donde ya se conoce el
    // esquema de la tabla.
    std::vector<float> vector_consulta;
    double      umbral = 0.0;
    bool        vec_coseno = true;           // lo fija la metrica del indice
    bool        vec_estricto = false;        // true => '<' en vez de '<='
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
    std::string index_idioma = "EN";          // solo INVERTED
    // Solo IVF. listas = 0 significa "elige tu": el motor usa sqrt(n), que es
    // la regla de dedo habitual para IVF-Flat.
    int         index_listas = 0;
    std::string index_metrica = "COSINE";

    // INSERT
    std::vector<std::vector<Value>> rows; 

    // COPY: cargar un CSV entero. Sin esto, meter doce mil abstracts exigiria
    // doce mil sentencias INSERT, que es inviable a mano y absurdo por HTTP.
    std::string copy_file;
    bool        copy_header = false;     // la primera linea son los nombres
    char        copy_delim  = ',';

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

    // ORDER BY col <=> [..]:  k vecinos mas cercanos en el espacio vectorial.
    // Es un camino aparte del knn espacial porque el indice, la distancia y el
    // tipo de la columna son otros.
    bool        knn_vectorial = false;
    std::vector<float> knn_vector;
    bool        knn_coseno = true;            // lo fija la metrica del indice
    // PROBES n. -1 => usar el valor por defecto del indice.
    int         probes = -1;
};

// Lanza DBException con un mensaje legible si la sentencia no es valida.
Statement parseSQL(const std::string& sql);                     // una sola sentencia (comportamiento actual)
std::vector<Statement> parseSQLMultiple(const std::string& sql); // varias, separadas por ';'

}  // namespace db
