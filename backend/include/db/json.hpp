// ============================================================================
//  json.hpp - Serializacion JSON minima (sin dependencias)
//
//  Solo se necesita generar JSON y leer un par de campos string del cuerpo de
//  la peticion, asi que no se justifica arrastrar una libreria completa.
// ============================================================================
#pragma once

#include <string>
#include <vector>

namespace db {
namespace json {

inline std::string escape(const std::string& s) {
    std::string o;
    o.reserve(s.size() + 8);
    for (unsigned char c : s) {
        switch (c) {
            case '"':  o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n";  break;
            case '\r': o += "\\r";  break;
            case '\t': o += "\\t";  break;
            default:
                if (c < 0x20) {
                    static const char* hex = "0123456789abcdef";
                    o += "\\u00";
                    o += hex[(c >> 4) & 0xF];
                    o += hex[c & 0xF];
                } else {
                    o += static_cast<char>(c);
                }
        }
    }
    return o;
}

inline std::string str(const std::string& s) { return "\"" + escape(s) + "\""; }

inline std::string num(double v) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.4f", v);
    return buf;
}

inline std::string arr(const std::vector<std::string>& elementos) {
    std::string o = "[";
    for (std::size_t i = 0; i < elementos.size(); ++i) {
        if (i) o += ",";
        o += elementos[i];
    }
    return o + "]";
}

// Extrae el valor string de una clave de primer nivel. Devuelve false si no
// aparece. Suficiente para cuerpos del tipo {"sql": "..."} o {"table": "..."}.
inline bool getString(const std::string& body, const std::string& clave, std::string* out) {
    const std::string patron = "\"" + clave + "\"";
    std::size_t k = body.find(patron);
    if (k == std::string::npos) return false;
    std::size_t i = body.find(':', k + patron.size());
    if (i == std::string::npos) return false;
    ++i;
    while (i < body.size() && std::isspace(static_cast<unsigned char>(body[i]))) ++i;
    if (i >= body.size() || body[i] != '"') return false;
    ++i;
    std::string v;
    while (i < body.size() && body[i] != '"') {
        if (body[i] == '\\' && i + 1 < body.size()) {
            char c = body[i + 1];
            switch (c) {
                case 'n':  v += '\n'; break;
                case 'r':  v += '\r'; break;
                case 't':  v += '\t'; break;
                case '"':  v += '"';  break;
                case '\\': v += '\\'; break;
                case 'u': {
                    if (i + 5 < body.size()) {
                        int cp = std::stoi(body.substr(i + 2, 4), nullptr, 16);
                        if (cp < 0x80) v += static_cast<char>(cp);
                        else if (cp < 0x800) { v += static_cast<char>(0xC0 | (cp >> 6)); v += static_cast<char>(0x80 | (cp & 0x3F)); }
                        else { v += static_cast<char>(0xE0 | (cp >> 12)); v += static_cast<char>(0x80 | ((cp >> 6) & 0x3F)); v += static_cast<char>(0x80 | (cp & 0x3F)); }
                        i += 4;
                    }
                    break;
                }
                default: v += c;
            }
            i += 2;
            continue;
        }
        v += body[i];
        ++i;
    }
    *out = v;
    return true;
}

}  // namespace json
}  // namespace db
