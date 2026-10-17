#include "db/record.hpp"

#include <sstream>

namespace db {

std::string typeName(Type t) {
    switch (t) {
        case Type::INT:     return "INT";
        case Type::DOUBLE:  return "DOUBLE";
        case Type::VARCHAR: return "VARCHAR";
        case Type::POINT:   return "POINT";
        case Type::POLYGON: return "POLYGON";
        case Type::VECTOR:  return "VECTOR";
    }
    return "INT";
}

Type typeFromName(const std::string& s) {
    if (s == "INT")     return Type::INT;
    if (s == "DOUBLE")  return Type::DOUBLE;
    if (s == "VARCHAR") return Type::VARCHAR;
    if (s == "POINT")   return Type::POINT;
    if (s == "POLYGON") return Type::POLYGON;
    if (s == "VECTOR")  return Type::VECTOR;
    throw DBException("Tipo desconocido: " + s);
}

int Schema::indexOf(const std::string& name) const {
    for (std::size_t i = 0; i < cols_.size(); ++i)
        if (cols_[i].name == name) return static_cast<int>(i);
    return -1;
}

std::string Value::str() const {
    switch (type) {
        case Type::INT:     return std::to_string(i);
        case Type::DOUBLE: {
            std::ostringstream os;
            os.precision(4);
            os << std::fixed << d;
            return os.str();
        }
        case Type::VARCHAR: return s;
        case Type::POINT: {
            std::ostringstream os;
            os.precision(6);
            os << std::fixed << "(" << d << ", " << y << ")";
            return os.str();
        }
        case Type::POLYGON: {
            std::ostringstream os;
            os.precision(6);
            os << std::fixed << "POLYGON(";
            for (std::size_t k = 0; k + 1 < poly.size(); k += 2) {
                if (k) os << ",";
                os << "(" << poly[k] << "," << poly[k + 1] << ")";
            }
            os << ")";
            return os.str();
        }
        case Type::VECTOR: {
            // Un embedding de 384 componentes impreso entero no le dice nada a
            // nadie y rompe cualquier tabla. Se muestran las tres primeras y la
            // dimension, que es lo unico que se mira a ojo.
            std::ostringstream os;
            os.precision(4);
            os << std::fixed << "[";
            std::size_t cuantas = vec.size() < 3 ? vec.size() : 3;
            for (std::size_t k = 0; k < cuantas; ++k) {
                if (k) os << ", ";
                os << vec[k];
            }
            if (vec.size() > cuantas) os << ", ...";
            os << "] (" << vec.size() << " dim)";
            return os.str();
        }
    }
    return "";
}

bool Value::operator==(const Value& o) const {
    if (type != o.type) return false;
    switch (type) {
        case Type::INT:     return i == o.i;
        case Type::DOUBLE:  return d == o.d;
        case Type::VARCHAR: return s == o.s;
        case Type::POINT:   return d == o.d && y == o.y;
        case Type::POLYGON: return poly == o.poly;
        case Type::VECTOR:  return vec == o.vec;
    }
    return false;
}

bool Value::operator<(const Value& o) const {
    switch (type) {
        case Type::INT:     return i < o.i;
        case Type::DOUBLE:  return d < o.d;
        case Type::VARCHAR: return s < o.s;
        // Los puntos no tienen un orden natural. Se define uno lexicografico
        // (x, luego y) solo para que Value siga siendo comparable; el R-Tree
        // nunca lo usa, ordena por solapamiento de cajas.
        case Type::POINT:   return d != o.d ? d < o.d : y < o.y;
        case Type::POLYGON: return poly < o.poly;
        // Tampoco hay orden natural entre embeddings: se compara componente a
        // componente solo para que Value sea ordenable. Las consultas de
        // similitud no pasan por aqui, pasan por la distancia del IVF.
        case Type::VECTOR:  return vec < o.vec;
    }
    return false;
}

std::string Tuple::str() const {
    std::string out = "{";
    for (std::size_t k = 0; k < values.size(); ++k) {
        if (k) out += ", ";
        out += values[k].str();
    }
    return out + "}";
}

std::string serializeTuple(const Schema& sch, const Tuple& t) {
    if (t.values.size() != sch.size())
        throw DBException("serializeTuple: la tupla no coincide con el esquema");

    std::string out;
    for (std::size_t k = 0; k < sch.size(); ++k) {
        const Column& c = sch[k];
        const Value&  v = t.values[k];
        switch (c.type) {
            case Type::INT: {
                std::int64_t x = v.i;
                out.append(reinterpret_cast<const char*>(&x), sizeof(x));
                break;
            }
            case Type::DOUBLE: {
                double x = v.d;
                out.append(reinterpret_cast<const char*>(&x), sizeof(x));
                break;
            }
            case Type::POINT: {
                double px = v.d, py = v.y;
                out.append(reinterpret_cast<const char*>(&px), sizeof(px));
                out.append(reinterpret_cast<const char*>(&py), sizeof(py));
                break;
            }
            case Type::POLYGON: {
                if (v.poly.size() % 2 != 0)
                    throw DBException("serializeTuple: el poligono tiene una coordenada suelta");
                std::int32_t nv = static_cast<std::int32_t>(v.poly.size() / 2);
                out.append(reinterpret_cast<const char*>(&nv), sizeof(nv));
                for (double coord : v.poly)
                    out.append(reinterpret_cast<const char*>(&coord), sizeof(coord));
                break;
            }
            case Type::VECTOR: {
                if (c.max_len > 0 && static_cast<int>(v.vec.size()) != c.max_len)
                    throw DBException("el vector tiene " + std::to_string(v.vec.size()) +
                                      " dimensiones y la columna '" + c.name +
                                      "' esta declarada VECTOR(" + std::to_string(c.max_len) + ")");
                std::int32_t nd = static_cast<std::int32_t>(v.vec.size());
                out.append(reinterpret_cast<const char*>(&nd), sizeof(nd));
                for (float comp : v.vec)
                    out.append(reinterpret_cast<const char*>(&comp), sizeof(comp));
                break;
            }
            case Type::VARCHAR: {
                std::string s = v.s;
                if (c.max_len > 0 && static_cast<int>(s.size()) > c.max_len)
                    s.resize(c.max_len);           // truncar como haria un VARCHAR(n)
                std::int32_t len = static_cast<std::int32_t>(s.size());
                out.append(reinterpret_cast<const char*>(&len), sizeof(len));
                out.append(s);
                break;
            }
        }
    }
    return out;
}

Value extractColumn(const Schema& sch, int col, const char* data, int len) {
    if (col < 0 || col >= static_cast<int>(sch.size()))
        throw DBException("extractColumn: columna fuera de rango");

    int p = 0;
    for (int k = 0; k <= col; ++k) {
        const Column& c = sch[static_cast<std::size_t>(k)];
        switch (c.type) {
            case Type::INT: {
                if (p + 8 > len) throw DBException("extractColumn: registro corrupto");
                if (k == col) return Value::makeInt(readAt<std::int64_t>(data, p));
                p += 8;
                break;
            }
            case Type::DOUBLE: {
                if (p + 8 > len) throw DBException("extractColumn: registro corrupto");
                if (k == col) return Value::makeDouble(readAt<double>(data, p));
                p += 8;
                break;
            }
            case Type::POINT: {
                if (p + 16 > len) throw DBException("extractColumn: registro corrupto");
                if (k == col) return Value::makePoint(readAt<double>(data, p),
                                                      readAt<double>(data, p + 8));
                p += 16;
                break;
            }
            case Type::POLYGON: {
                if (p + 4 > len) throw DBException("extractColumn: registro corrupto");
                std::int32_t nv = readAt<std::int32_t>(data, p);
                p += 4;
                if (nv < 0 || p + nv * 16 > len) throw DBException("extractColumn: registro corrupto");
                if (k == col) {
                    std::vector<double> vs(static_cast<std::size_t>(nv) * 2);
                    for (std::size_t q = 0; q < vs.size(); ++q)
                        vs[q] = readAt<double>(data, p + static_cast<int>(q) * 8);
                    return Value::makePolygon(std::move(vs));
                }
                p += nv * 16;
                break;
            }
            case Type::VECTOR: {
                if (p + 4 > len) throw DBException("extractColumn: registro corrupto");
                std::int32_t nd = readAt<std::int32_t>(data, p);
                p += 4;
                if (nd < 0 || p + nd * 4 > len) throw DBException("extractColumn: registro corrupto");
                if (k == col) {
                    std::vector<float> vs(static_cast<std::size_t>(nd));
                    for (std::size_t q = 0; q < vs.size(); ++q)
                        vs[q] = readAt<float>(data, p + static_cast<int>(q) * 4);
                    return Value::makeVector(std::move(vs));
                }
                p += nd * 4;
                break;
            }
            case Type::VARCHAR: {
                if (p + 4 > len) throw DBException("extractColumn: registro corrupto");
                std::int32_t n = readAt<std::int32_t>(data, p);
                p += 4;
                if (n < 0 || p + n > len) throw DBException("extractColumn: registro corrupto");
                if (k == col) return Value::makeStr(std::string(data + p, data + p + n));
                p += n;
                break;
            }
        }
    }
    throw DBException("extractColumn: no se alcanzo la columna");
}

Tuple deserializeTuple(const Schema& sch, const char* data, int len) {
    Tuple t;
    int p = 0;
    for (std::size_t k = 0; k < sch.size(); ++k) {
        const Column& c = sch[k];
        switch (c.type) {
            case Type::INT: {
                if (p + 8 > len) throw DBException("deserializeTuple: registro corrupto");
                t.values.push_back(Value::makeInt(readAt<std::int64_t>(data, p)));
                p += 8;
                break;
            }
            case Type::DOUBLE: {
                if (p + 8 > len) throw DBException("deserializeTuple: registro corrupto");
                t.values.push_back(Value::makeDouble(readAt<double>(data, p)));
                p += 8;
                break;
            }
            case Type::POINT: {
                if (p + 16 > len) throw DBException("deserializeTuple: registro corrupto");
                t.values.push_back(Value::makePoint(readAt<double>(data, p),
                                                    readAt<double>(data, p + 8)));
                p += 16;
                break;
            }
            case Type::POLYGON: {
                if (p + 4 > len) throw DBException("deserializeTuple: registro corrupto");
                std::int32_t nv = readAt<std::int32_t>(data, p);
                p += 4;
                if (nv < 0 || p + nv * 16 > len) throw DBException("deserializeTuple: registro corrupto");
                std::vector<double> vs(static_cast<std::size_t>(nv) * 2);
                for (std::size_t q = 0; q < vs.size(); ++q)
                    vs[q] = readAt<double>(data, p + static_cast<int>(q) * 8);
                t.values.push_back(Value::makePolygon(std::move(vs)));
                p += nv * 16;
                break;
            }
            case Type::VECTOR: {
                if (p + 4 > len) throw DBException("deserializeTuple: registro corrupto");
                std::int32_t nd = readAt<std::int32_t>(data, p);
                p += 4;
                if (nd < 0 || p + nd * 4 > len) throw DBException("deserializeTuple: registro corrupto");
                std::vector<float> vs(static_cast<std::size_t>(nd));
                for (std::size_t q = 0; q < vs.size(); ++q)
                    vs[q] = readAt<float>(data, p + static_cast<int>(q) * 4);
                t.values.push_back(Value::makeVector(std::move(vs)));
                p += nd * 4;
                break;
            }
            case Type::VARCHAR: {
                if (p + 4 > len) throw DBException("deserializeTuple: registro corrupto");
                std::int32_t n = readAt<std::int32_t>(data, p);
                p += 4;
                if (n < 0 || p + n > len) throw DBException("deserializeTuple: registro corrupto");
                t.values.push_back(Value::makeStr(std::string(data + p, data + p + n)));
                p += n;
                break;
            }
        }
    }
    return t;
}

}  // namespace db
