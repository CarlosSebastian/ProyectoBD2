// Mini framework de asserts para los tests (sin dependencias externas).
#pragma once
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>

// Cada suite escribe en SU PROPIA carpeta bajo data/.
//
// Antes varias usaban data/ a secas y se pisaban entre si; peor aun,
// test_catalog guardaba su catalogo en data/catalog.txt, que es el de la
// demo. Correr los tests dejaba al servidor sin ninguna de las tablas
// creadas a mano: seguian en disco, pero sin nadie que las declarara.
inline std::string dirPrueba(const char* nombre) {
    const std::string d = std::string("data/") + nombre;
    std::filesystem::create_directories(d);
    return d;
}

static int g_checks = 0;

inline void CHECK(bool cond, const std::string& msg) {
    ++g_checks;
    if (!cond) {
        std::cerr << "   FALLO: " << msg << "\n";
        std::exit(1);
    }
}

template <typename A, typename B>
inline void CHECK_EQ(const A& a, const B& b, const std::string& msg) {
    ++g_checks;
    if (!(a == b)) {
        std::cerr << "   FALLO: " << msg << "  (obtuve " << a << ", esperaba " << b << ")\n";
        std::exit(1);
    }
}

inline void SECTION(const std::string& t) {
    std::cout << "-- " << t << "\n";
}

inline void DONE(const std::string& name) {
    std::cout << "OK  " << name << "  (" << g_checks << " verificaciones)\n";
}
