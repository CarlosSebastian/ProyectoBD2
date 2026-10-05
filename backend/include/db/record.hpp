// ============================================================================
//  record.hpp - Esquema, valores y serializacion de tuplas
//
//  Una tupla se guarda en el heap file como una secuencia de bytes:
//      INT     -> 8 bytes (int64)
//      DOUBLE  -> 8 bytes
//      POINT   -> 16 bytes (dos doubles: x, y)
//      POLYGON -> int32 numero de vertices + 2n doubles (x1,y1,x2,y2,...)
//      VARCHAR -> int32 longitud + los bytes del texto
//  Se serializa en el orden de las columnas del esquema, sin cabecera: el
//  esquema (que vive en el catalogo) es lo que permite volver a leerla.
// ============================================================================
#pragma once

#include <string>
#include <vector>

#include "db/common.hpp"

namespace db {

// POINT es una coordenada 2D de ancho fijo (16 bytes). El R-Tree lo trata como
// una caja envolvente degenerada en la que las cuatro esquinas coinciden.
//
// POLYGON es un anillo cerrado de vertices, de longitud variable. El R-Tree lo
// indexa por su CAJA ENVOLVENTE, que es solo una aproximacion: por eso toda
// consulta sobre poligonos tiene dos pasos, filtrado por MBR en el indice y
// REFINAMIENTO exacto sobre la geometria real. Es como trabajan los motores
// espaciales de verdad.
enum class Type { INT, DOUBLE, VARCHAR, POINT, POLYGON };

std::string typeName(Type t);
Type        typeFromName(const std::string& s);

struct Column {
    std::string name;
    Type        type    = Type::INT;
    int         max_len = 0;   // solo para VARCHAR

    Column() = default;
    Column(std::string n, Type t, int len = 0) : name(std::move(n)), type(t), max_len(len) {}
};

class Schema {
public:
    Schema() = default;
    explicit Schema(std::vector<Column> cols) : cols_(std::move(cols)) {}

    const std::vector<Column>& columns() const { return cols_; }
    std::size_t size() const { return cols_.size(); }
    const Column& operator[](std::size_t i) const { return cols_[i]; }

    int indexOf(const std::string& name) const;   // -1 si no existe

private:
    std::vector<Column> cols_;
};

// ---------------------------------------------------------------------------
//  Value: un valor de cualquiera de los tres tipos soportados.
// ---------------------------------------------------------------------------
struct Value {
    Type         type = Type::INT;
    std::int64_t i    = 0;
    double       d    = 0.0;   // DOUBLE, y tambien la coordenada X de un POINT
    double       y    = 0.0;   // solo POINT: coordenada Y
    std::string  s;
    std::vector<double> poly;  // solo POLYGON: x1,y1,x2,y2,... (anillo cerrado)

    Value() = default;
    static Value makeInt(std::int64_t v)      { Value x; x.type = Type::INT;     x.i = v; return x; }
    static Value makeDouble(double v)         { Value x; x.type = Type::DOUBLE;  x.d = v; return x; }
    static Value makeStr(const std::string& v){ Value x; x.type = Type::VARCHAR; x.s = v; return x; }
    static Value makePoint(double px, double py) {
        Value x; x.type = Type::POINT; x.d = px; x.y = py; return x;
    }
    // vertices = [x1, y1, x2, y2, ...]; el anillo se cierra solo.
    static Value makePolygon(std::vector<double> vertices) {
        Value x; x.type = Type::POLYGON; x.poly = std::move(vertices); return x;
    }
    std::size_t numVertices() const { return poly.size() / 2; }

    std::string str() const;
    bool operator==(const Value& o) const;
    bool operator<(const Value& o) const;
};

struct Tuple {
    std::vector<Value> values;
    const Value& at(std::size_t i) const { return values[i]; }
    std::string str() const;
};

// Serializacion segun el esquema.
std::string serializeTuple(const Schema& sch, const Tuple& t);
Tuple       deserializeTuple(const Schema& sch, const char* data, int len);

// Extrae SOLO el valor de una columna, saltando las anteriores sin
// materializarlas. El Sequential File compara claves en cada paso de la
// busqueda binaria; deserializar la tupla completa cada vez seria absurdo.
Value extractColumn(const Schema& sch, int col, const char* data, int len);

}  // namespace db
