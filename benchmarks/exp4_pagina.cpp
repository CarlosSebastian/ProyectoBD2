// ============================================================================
//  Experimento 4 - Sensibilidad al tamano de bloque
//
//  Este programa mide UN tamano de pagina: el que se fijo al compilar con
//  -DDB_PAGE_SIZE=N. El script run_experimentos.sh lo recompila para
//  B en {1024, 2048, 4096, 8192} y arma la tabla completa.
//
//  Reporta el factor de ramificacion (fan-out), la altura h del arbol B+, y
//  tanto las TRANSFERENCIAS como los BYTES movidos: con bloques chicos hay mas
//  transferencias pero cada una mueve menos, y solo mirando los bytes se ve
//  cual es realmente el costo.
//
//     ./build/exp4_pagina [dir] [N] [consultas] [--cabecera]
// ============================================================================
#include "comun.hpp"

using namespace bench;
using namespace db;

int main(int argc, char** argv) {
    std::string dir       = "data";
    int         N         = 100000;
    int         consultas = 500;
    bool        cabecera  = false;

    int posicional = 0;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--cabecera") { cabecera = true; continue; }
        switch (posicional++) {
            case 0: dir = a; break;
            case 1: N = std::atoi(a.c_str()); break;
            case 2: consultas = std::atoi(a.c_str()); break;
            default: break;
        }
    }

    if (cabecera) {
        fila({"B (bytes)", "Fan-out hoja", "Fan-out interno", "Altura h",
              "Pag. indice", "Pag. datos", "Lecturas/consulta", "Bytes/consulta"});
        separador(8);
    }

    std::vector<Fila> filas = cargarCSV(rutaDataset(dir));
    if (static_cast<int>(filas.size()) < N) {
        std::cerr << "Dataset insuficiente. Corre antes: ./build/gen_dataset\n";
        return 1;
    }

    limpiar(dir, "e4");
    Table t(dir, info("e4", Organizacion::HEAP, true, IndexKind::BPLUS), 64);
    for (int i = 0; i < N; ++i) t.insert(aTupla(filas[static_cast<std::size_t>(i)]));

    // Altura real del arbol. Se pregunta a la tabla, no reabriendo el archivo:
    // sus paginas sucias todavia no estan en disco.
    int altura = t.indexHeight();

    std::mt19937 rng(7);
    std::vector<long long> lec;
    for (int q = 0; q < consultas; ++q) {
        std::int64_t k = filas[rng() % static_cast<unsigned>(N)].id;
        Cronometro cron;
        t.searchEq("id", Value::makeInt(k));
        lec.push_back(cron.parar().lecturas);
    }
    Resumen r = resumir(lec);

    fila({std::to_string(PAGE_SIZE),
          std::to_string(BPlusTree<std::int64_t>::leafCapacity()),
          std::to_string(BPlusTree<std::int64_t>::internalCapacity()),
          std::to_string(altura),
          std::to_string(t.indexPages()),
          std::to_string(t.dataPages()),
          num(r.media, 2),
          num(r.media * PAGE_SIZE, 0)});
    return 0;
}
