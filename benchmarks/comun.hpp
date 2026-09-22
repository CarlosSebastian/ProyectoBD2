// ============================================================================
//  comun.hpp - Utilidades compartidas por los cuatro experimentos
//
//  Dataset sintetico con el esquema del enunciado:
//      id INT PRIMARY KEY, nombre CHAR(24), dept CHAR(12), monto FLOAT
//
//  Las claves se generan como una permutacion de 1..N y luego se barajan, de
//  modo que la carga siempre llega DESORDENADA: es el caso que castiga al
//  Sequential File y el que hace interesante la comparacion.
// ============================================================================
#pragma once

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <random>
#include <sstream>
#include <cstdlib>
#include <string>
#include <vector>

#include "db/database.hpp"
#include "db/disk_counter.hpp"
#include "db/table.hpp"

namespace bench {

using Clock = std::chrono::steady_clock;

struct Fila {
    std::int64_t id;
    std::string  nombre;
    std::string  dept;
    double       monto;
};

inline db::Schema esquema() {
    return db::Schema({db::Column("id", db::Type::INT),
                       db::Column("nombre", db::Type::VARCHAR, 24),
                       db::Column("dept", db::Type::VARCHAR, 12),
                       db::Column("monto", db::Type::DOUBLE)});
}

inline db::Tuple aTupla(const Fila& f) {
    return db::Tuple{{db::Value::makeInt(f.id),
                      db::Value::makeStr(f.nombre),
                      db::Value::makeStr(f.dept),
                      db::Value::makeDouble(f.monto)}};
}

// Genera N filas con claves 1..N barajadas de forma reproducible.
inline std::vector<Fila> generar(int N, unsigned semilla = 2026) {
    std::vector<std::int64_t> ids(static_cast<std::size_t>(N));
    std::iota(ids.begin(), ids.end(), 1);
    std::mt19937 rng(semilla);
    std::shuffle(ids.begin(), ids.end(), rng);

    static const char* depts[] = {"Ventas", "Logistica", "TI", "RRHH", "Finanzas",
                                  "Legal", "Compras", "Soporte"};
    std::vector<Fila> filas;
    filas.reserve(static_cast<std::size_t>(N));
    for (int i = 0; i < N; ++i) {
        std::int64_t id = ids[static_cast<std::size_t>(i)];
        filas.push_back(Fila{id,
                             "emp_" + std::to_string(id),
                             depts[id % 8],
                             1000.0 + static_cast<double>(id % 9973) * 1.37});
    }
    return filas;
}

inline void guardarCSV(const std::vector<Fila>& filas, const std::string& ruta) {
    std::ofstream out(ruta);
    out << "id,nombre,dept,monto\n";
    for (const Fila& f : filas)
        out << f.id << "," << f.nombre << "," << f.dept << ","
            << std::fixed << std::setprecision(2) << f.monto << "\n";
}

// Ruta del dataset. El tamano viene de la variable de entorno DATASET_N, que
// pone run_experimentos.sh, en vez de estar fijo a 500000: asi el argumento
// [N_dataset] del script funciona de verdad.
inline std::string rutaDataset(const std::string& dir) {
    const char* n = std::getenv("DATASET_N");
    return dir + "/dataset_" + (n && *n ? std::string(n) : std::string("500000")) + ".csv";
}

inline std::vector<Fila> cargarCSV(const std::string& ruta) {
    std::vector<Fila> filas;
    std::ifstream in(ruta);
    if (!in) return filas;
    std::string linea;
    std::getline(in, linea);                       // cabecera
    while (std::getline(in, linea)) {
        if (linea.empty()) continue;
        std::istringstream ss(linea);
        std::string a, b, c, d;
        std::getline(ss, a, ','); std::getline(ss, b, ',');
        std::getline(ss, c, ','); std::getline(ss, d, ',');
        filas.push_back(Fila{std::atoll(a.c_str()), b, c, std::atof(d.c_str())});
    }
    return filas;
}

// ---------------------------------------------------------------------------
//  Construccion de tablas para los experimentos
// ---------------------------------------------------------------------------
enum class Organizacion { HEAP, SEQUENTIAL };

inline db::TableInfo info(const std::string& nombre, Organizacion org,
                          bool con_indice, db::IndexKind tipo_indice) {
    db::TableInfo ti;
    ti.name       = nombre;
    ti.schema     = esquema();
    ti.heap_file  = nombre + ".dat";
    ti.engine     = (org == Organizacion::HEAP) ? "HEAP" : "SEQUENTIAL";
    ti.key_column = "id";
    if (con_indice) ti.indexes.push_back(db::IndexInfo{"id", tipo_indice, nombre + ".idx"});
    return ti;
}

inline void limpiar(const std::string& dir, const std::string& nombre) {
    db::resetFile(dir + "/" + nombre + ".dat");
    db::resetFile(dir + "/" + nombre + ".ovf");
    db::resetFile(dir + "/" + nombre + ".idx");
}

// ---------------------------------------------------------------------------
//  Estadistica y salida
// ---------------------------------------------------------------------------
struct Resumen { double media = 0, desv = 0; };

template <typename T>
inline Resumen resumir(const std::vector<T>& v) {
    if (v.empty()) return {};
    double suma = 0;
    for (const T& x : v) suma += static_cast<double>(x);
    double m = suma / static_cast<double>(v.size());
    double acc = 0;
    for (const T& x : v) { double d = static_cast<double>(x) - m; acc += d * d; }
    return Resumen{m, std::sqrt(acc / static_cast<double>(v.size()))};
}

inline std::string num(double v, int dec = 2) {
    std::ostringstream os;
    os << std::fixed << std::setprecision(dec) << v;
    return os.str();
}

inline void titulo(const std::string& t) {
    std::cout << "\n" << std::string(74, '=') << "\n  " << t << "\n"
              << std::string(74, '=') << "\n";
}

inline void fila(const std::vector<std::string>& celdas) {
    std::cout << "|";
    for (const std::string& c : celdas) std::cout << " " << c << " |";
    std::cout << "\n";
}

inline void separador(std::size_t n) {
    std::cout << "|";
    for (std::size_t i = 0; i < n; ++i) std::cout << "---|";
    std::cout << "\n";
}

// Cronometro con snapshot de I/O: devuelve ms, lecturas y escrituras del bloque.
struct Medicion {
    double    ms      = 0;
    long long lecturas = 0;
    long long escrituras = 0;
    long long accesos = 0;
};

class Cronometro {
public:
    Cronometro() { reiniciar(); }
    void reiniciar() {
        t0_  = Clock::now();
        io0_ = db::DiskCounter::global().snapshot();
    }
    Medicion parar() const {
        db::DiskCounter::Delta d = db::DiskCounter::global().since(io0_);
        return Medicion{std::chrono::duration<double, std::milli>(Clock::now() - t0_).count(),
                        d.reads, d.writes, d.page_accesses};
    }
private:
    Clock::time_point            t0_;
    db::DiskCounter::Snapshot    io0_;
};

}  // namespace bench
