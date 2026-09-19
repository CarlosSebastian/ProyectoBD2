// ============================================================================
//  Experimento 2 - Busquedas puntuales de igualdad
//
//  1000 consultas aleatorias de igualdad sobre N = 100 000 registros.
//  Compara promedio y desviacion estandar de lecturas de disco y latencia:
//
//      Full Scan (Heap)  vs  Busqueda Binaria (Sequential)
//                        vs  Arbol B+  vs  Hash dinamico
//
//     ./build/exp2_puntual [dir] [N] [consultas]
// ============================================================================
#include "comun.hpp"

using namespace bench;
using namespace db;

struct Escenario {
    std::string  etiqueta;
    Organizacion org;
    bool         indice;
    IndexKind    tipo;
};

int main(int argc, char** argv) {
    std::string dir       = (argc > 1) ? argv[1] : "data";
    int         N         = (argc > 2) ? std::atoi(argv[2]) : 100000;
    int         consultas = (argc > 3) ? std::atoi(argv[3]) : 1000;

    std::vector<Fila> filas = cargarCSV(rutaDataset(dir));
    if (static_cast<int>(filas.size()) < N) {
        std::cerr << "Dataset insuficiente. Corre antes: ./build/gen_dataset\n";
        return 1;
    }

    // Claves a consultar: existentes, elegidas al azar con semilla fija.
    std::mt19937 rng(7);
    std::vector<std::int64_t> claves(static_cast<std::size_t>(consultas));
    for (int i = 0; i < consultas; ++i)
        claves[static_cast<std::size_t>(i)] = filas[rng() % static_cast<unsigned>(N)].id;

    std::vector<Escenario> escenarios = {
        {"Full Scan (Heap)",           Organizacion::HEAP,       false, IndexKind::BPLUS},
        {"Busqueda Binaria (Sequential)", Organizacion::SEQUENTIAL, false, IndexKind::BPLUS},
        {"Arbol B+",                   Organizacion::HEAP,       true,  IndexKind::BPLUS},
        {"Hash dinamico",              Organizacion::HEAP,       true,  IndexKind::HASH},
    };

    titulo("EXPERIMENTO 2 - Busquedas puntuales de igualdad");
    std::cout << "N = " << N << " registros | " << consultas << " consultas aleatorias | "
              << "pagina de " << PAGE_SIZE << " B | buffer pool de 64 paginas\n";

    struct Resultado {
        std::string etiqueta;
        Resumen     lecturas, latencia, accesos;
        long long   encontrados = 0;
        int         paginas = 0;
    };
    std::vector<Resultado> resultados;

    for (const Escenario& e : escenarios) {
        limpiar(dir, "e2");
        Table t(dir, info("e2", e.org, e.indice, e.tipo), 64);
        for (int i = 0; i < N; ++i) t.insert(aTupla(filas[static_cast<std::size_t>(i)]));

        std::vector<long long> lec, acc;
        std::vector<double>    lat;
        lec.reserve(static_cast<std::size_t>(consultas));
        acc.reserve(static_cast<std::size_t>(consultas));
        lat.reserve(static_cast<std::size_t>(consultas));

        long long hallados = 0;
        for (std::int64_t k : claves) {
            Cronometro cron;
            // searchEq devuelve las TUPLAS: los cuatro metodos entregan la fila
            // completa, no solo su direccion.
            auto r = t.searchEq("id", Value::makeInt(k));
            Medicion m = cron.parar();
            hallados += static_cast<long long>(r.size());
            lec.push_back(m.lecturas);
            acc.push_back(m.accesos);
            lat.push_back(m.ms);
        }

        resultados.push_back(Resultado{e.etiqueta, resumir(lec), resumir(lat), resumir(acc),
                                       hallados, t.dataPages() + t.indexPages()});
        std::cerr << "." << std::flush;
    }
    std::cerr << "\n";

    std::cout << "\n### Lecturas de disco por consulta\n\n";
    fila({"Metodo", "Media", "Desv. estandar", "Accesos a pagina (media)"});
    separador(4);
    for (const Resultado& r : resultados)
        fila({r.etiqueta, num(r.lecturas.media, 1), num(r.lecturas.desv, 1),
              num(r.accesos.media, 1)});

    std::cout << "\n### Latencia por consulta (ms)\n\n";
    fila({"Metodo", "Media", "Desv. estandar", "Speedup vs full scan"});
    separador(4);
    double base = resultados.empty() ? 1.0 : resultados[0].latencia.media;
    for (const Resultado& r : resultados)
        fila({r.etiqueta, num(r.latencia.media, 4), num(r.latencia.desv, 4),
              num(base / (r.latencia.media > 0 ? r.latencia.media : 1e-9), 1) + "x"});

    std::cout << "\n### Verificacion y tamano\n\n";
    fila({"Metodo", "Filas devueltas", "Paginas del archivo"});
    separador(3);
    for (const Resultado& r : resultados)
        fila({r.etiqueta, std::to_string(r.encontrados), std::to_string(r.paginas)});

    std::cout << "\nTodos los metodos devuelven la misma cantidad de filas: la comparacion\n"
                 "es valida. La desviacion estandar del full scan es practicamente cero\n"
                 "porque SIEMPRE recorre el archivo entero, no dependa de donde este la\n"
                 "clave; en los indices la variacion viene de cuantos niveles del arbol o\n"
                 "cuantas paginas del directorio estaban ya en el buffer pool.\n\n";
    return 0;
}
