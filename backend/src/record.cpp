#include "db/record.hpp"

#include <sstream>

namespace db {

std::string typeName(Type t) {
    switch (t) {
        case Type::INT:     return "INT";
        case Type::DOUBLE:  return "DOUBLE";
        case Type::VARCHAR: return "VARCHAR";
    }
    return "INT";
}

Type typeFromName(const std::string& s) {
    if (s == "INT")     return Type::INT;
    if (s == "DOUBLE")  return Type::DOUBLE;
    if (s == "VARCHAR") return Type::VARCHAR;
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
    }
    return "";
}

bool Value::operator==(const Value& o) const {
    if (type != o.type) return false;
    switch (type) {
        case Type::INT:     return i == o.i;
        case Type::DOUBLE:  return d == o.d;
        case Type::VARCHAR: return s == o.s;
    }
    return false;
}

bool Value::operator<(const Value& o) const {
    switch (type) {
        case Type::INT:     return i < o.i;
        case Type::DOUBLE:  return d < o.d;
        case Type::VARCHAR: return s < o.s;
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
