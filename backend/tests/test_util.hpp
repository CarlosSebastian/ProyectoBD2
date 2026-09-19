// Mini framework de asserts para los tests (sin dependencias externas).
#pragma once
#include <cstdlib>
#include <iostream>
#include <string>

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
