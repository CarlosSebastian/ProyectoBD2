// ============================================================================
//  common.hpp - Tipos, constantes y utilidades compartidas por todo el motor
//  Proyecto Integrador Multimodal - Base de Datos II (UTEC)
// ============================================================================
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>

namespace db {

// Tamano de pagina. Por defecto 4 KB (igual que PostgreSQL), pero se puede
// fijar en tiempo de compilacion con -DDB_PAGE_SIZE=N: el Experimento 4 del
// enunciado exige medir B en {1024, 2048, 4096, 8192} y ver su efecto en el
// fan-out y la altura del arbol. Como las capacidades de los nodos se derivan
// de esta constante, basta recompilar.
#ifndef DB_PAGE_SIZE
#define DB_PAGE_SIZE 4096
#endif
constexpr int PAGE_SIZE = DB_PAGE_SIZE;
static_assert(PAGE_SIZE >= 1024 && PAGE_SIZE <= 65536, "PAGE_SIZE fuera de rango razonable");

using page_id_t = std::int32_t;
constexpr page_id_t INVALID_PAGE_ID = -1;

// ---------------------------------------------------------------------------
//  RID (Record ID): direccion fisica de un registro = (pagina, slot).
//  Es lo que guardan TODOS los indices como "puntero" al dato real.
// ---------------------------------------------------------------------------
struct RID {
    page_id_t   page_id = INVALID_PAGE_ID;
    std::int32_t slot   = -1;

    RID() = default;
    RID(page_id_t p, std::int32_t s) : page_id(p), slot(s) {}

    bool valid() const { return page_id != INVALID_PAGE_ID && slot >= 0; }
    bool operator==(const RID& o) const { return page_id == o.page_id && slot == o.slot; }
    bool operator!=(const RID& o) const { return !(*this == o); }
    bool operator<(const RID& o) const {
        if (page_id != o.page_id) return page_id < o.page_id;
        return slot < o.slot;
    }
    std::string str() const {
        return "(" + std::to_string(page_id) + "," + std::to_string(slot) + ")";
    }
};

// ---------------------------------------------------------------------------
//  Lectura / escritura de tipos POD dentro del buffer crudo de una pagina.
// ---------------------------------------------------------------------------
template <typename T>
inline T readAt(const char* buf, int off) {
    T v;
    std::memcpy(&v, buf + off, sizeof(T));
    return v;
}

template <typename T>
inline void writeAt(char* buf, int off, const T& v) {
    std::memcpy(buf + off, &v, sizeof(T));
}

// ---------------------------------------------------------------------------
//  FixedStr<N>: clave de texto de longitud fija.
//  Los indices en disco necesitan claves de tamano constante para poder
//  calcular offsets; este tipo envuelve un char[N] con orden lexicografico.
// ---------------------------------------------------------------------------
template <int N>
struct FixedStr {
    char data[N];

    FixedStr() { std::memset(data, 0, N); }
    FixedStr(const std::string& s) {
        std::memset(data, 0, N);
        std::size_t n = std::min(static_cast<std::size_t>(N - 1), s.size());
        std::memcpy(data, s.data(), n);
    }
    FixedStr(const char* s) : FixedStr(std::string(s)) {}

    std::string str() const { return std::string(data); }

    bool operator<(const FixedStr& o)  const { return std::strncmp(data, o.data, N) <  0; }
    bool operator>(const FixedStr& o)  const { return std::strncmp(data, o.data, N) >  0; }
    bool operator==(const FixedStr& o) const { return std::strncmp(data, o.data, N) == 0; }
    bool operator!=(const FixedStr& o) const { return !(*this == o); }
    bool operator<=(const FixedStr& o) const { return !(*this > o); }
    bool operator>=(const FixedStr& o) const { return !(*this < o); }
};

using Key32 = FixedStr<32>;

// ---------------------------------------------------------------------------
//  Funcion de hash para las claves (usada por el Hash Extensible).
//  Mezcla estilo splitmix64: barata y con buena dispersion de bits altos,
//  que es lo que necesita el hash extensible para repartir los buckets.
// ---------------------------------------------------------------------------
inline std::uint64_t mix64(std::uint64_t x) {
    x += 0x9e3779b97f4a7c15ULL;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
    return x ^ (x >> 31);
}

inline std::uint64_t hashKey(std::int64_t k) { return mix64(static_cast<std::uint64_t>(k)); }
inline std::uint64_t hashKey(std::int32_t k) { return mix64(static_cast<std::uint64_t>(k)); }

template <int N>
inline std::uint64_t hashKey(const FixedStr<N>& k) {
    std::uint64_t h = 1469598103934665603ULL;  // FNV-1a
    for (int i = 0; i < N && k.data[i] != '\0'; ++i) {
        h ^= static_cast<unsigned char>(k.data[i]);
        h *= 1099511628211ULL;
    }
    return mix64(h);
}

class DBException : public std::runtime_error {
public:
    explicit DBException(const std::string& msg) : std::runtime_error(msg) {}
};

}  // namespace db
