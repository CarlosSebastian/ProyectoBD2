// ============================================================================
//  gen_dataset.cpp - Genera el dataset sintetico de los experimentos
//
//     ./build/gen_dataset [N] [directorio]
//
//  Por defecto 500 000 filas en data/. El enunciado exige volumenes de
//  100 000 a 500 000 registros: no se admiten muestras de 10 a 50 filas.
// ============================================================================
#include "comun.hpp"

int main(int argc, char** argv) {
    int         N   = (argc > 1) ? std::atoi(argv[1]) : 500000;
    std::string dir = (argc > 2) ? argv[2] : "data";

    std::cout << "Generando " << N << " filas...\n";
    auto t0 = bench::Clock::now();
    std::vector<bench::Fila> filas = bench::generar(N);
    std::string ruta = dir + "/dataset_" + std::to_string(N) + ".csv";
    bench::guardarCSV(filas, ruta);
    double ms = std::chrono::duration<double, std::milli>(bench::Clock::now() - t0).count();

    std::ifstream in(ruta, std::ios::binary | std::ios::ate);
    double mb = static_cast<double>(in.tellg()) / (1024.0 * 1024.0);

    std::cout << "Escrito " << ruta << "  (" << bench::num(mb) << " MB, "
              << bench::num(ms) << " ms)\n"
              << "Claves: permutacion de 1.." << N << " barajada con semilla fija.\n"
              << "Columnas: id INT PRIMARY KEY, nombre CHAR(24), dept CHAR(12), monto FLOAT\n";
    return 0;
}
