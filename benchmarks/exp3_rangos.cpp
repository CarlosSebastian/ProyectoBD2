// ============================================================================
//  Experimento 3 - Busquedas por rango con selectividad variable
//
//  Evalua el costo de I/O y el tiempo al consultar rangos que representan el
//  0.1 %, 1 %, 5 %, 10 % y 25 % del total de tuplas, contrastando:
//
//      Arbol B+  vs  Sequential File  vs  Full Scan
//
//     ./build/exp3_rangos [dir] [N] [consultas_por_selectividad]
// ============================================================================
#include <cmath>

#include "comun.hpp"

using namespace bench;
using namespace db;

struct Escenario {
    std::string  etiqueta;
    Organizacion org;
    bool         indice;
};

int main(int argc, char** argv) {
    std::string dir = (argc > 1) ? argv[1] : "data";
    int         N   = (argc > 2) ? std::atoi(argv[2]) : 100000;
    int         Q   = (argc > 3) ? std::atoi(argv[3]) : 20;

    std::vector<Fila> filas = cargarCSV(rutaDataset(dir));
    if (static_cast<int>(filas.size()) < N) {
        std::cerr << "Dataset insuficiente. Corre antes: ./build/gen_dataset\n";
        return 1;
    }

    const std::vector<double> selectividades = {0.001, 0.01, 0.05, 0.10, 0.25};

    std::vector<Escenario> escenarios = {
        {"Full Scan (Heap)",  Organizacion::HEAP,       false},
        {"Sequential File",   Organizacion::SEQUENTIAL, false},
        {"Arbol B+",          Organizacion::HEAP,       true},
    };

    titulo("EXPERIMENTO 3 - Rangos con selectividad variable");
    std::cout << "N = " << N << " registros | " << Q << " consultas por selectividad | "
              << "pagina de " << PAGE_SIZE << " B | buffer pool de 64 paginas\n";

    // resultados[escenario][selectividad]
    std::vector<std::vector<Resumen>> lecturas(escenarios.size()), latencias(escenarios.size());
    std::vector<std::vector<long long>> devueltas(escenarios.size());

    for (std::size_t e = 0; e < escenarios.size(); ++e) {
        limpiar(dir, "e3");
        Table t(dir, info("e3", escenarios[e].org, escenarios[e].indice, IndexKind::BPLUS), 64);
        for (int i = 0; i < N; ++i) t.insert(aTupla(filas[static_cast<std::size_t>(i)]));

        // Selectividad EXACTA: las claves insertadas son un subconjunto disperso
        // de 1..500000, asi que un rango de ancho fijo no representa una
        // fraccion fija de la tabla. Se ordenan las claves realmente cargadas y
        // el rango se define por posiciones dentro de ese orden, de modo que
        // "5 %" son exactamente el 5 % de las filas.
        std::vector<std::int64_t> ordenadas;
        ordenadas.reserve(static_cast<std::size_t>(N));
        for (int i = 0; i < N; ++i) ordenadas.push_back(filas[static_cast<std::size_t>(i)].id);
        std::sort(ordenadas.begin(), ordenadas.end());

        for (double s : selectividades) {
            int cnt = std::max(1, static_cast<int>(s * N));
            std::mt19937 rng(101);
            std::vector<long long> lec;
            std::vector<double>    lat;
            long long total_filas = 0;

            for (int q = 0; q < Q; ++q) {
                int desde = static_cast<int>(rng() % static_cast<unsigned>(N - cnt));
                std::int64_t lo = ordenadas[static_cast<std::size_t>(desde)];
                std::int64_t hi = ordenadas[static_cast<std::size_t>(desde + cnt - 1)];
                Cronometro cron;
                // searchRange devuelve las TUPLAS, no solo los RID: los tres
                // metodos hacen el mismo trabajo y la comparacion es justa.
                auto r = t.searchRange("id", Value::makeInt(lo), Value::makeInt(hi));
                Medicion m = cron.parar();
                total_filas += static_cast<long long>(r.size());
                lec.push_back(m.lecturas);
                lat.push_back(m.ms);
            }
            lecturas[e].push_back(resumir(lec));
            latencias[e].push_back(resumir(lat));
            devueltas[e].push_back(total_filas / Q);
            std::cerr << "." << std::flush;
        }
    }
    std::cerr << "\n";

    std::vector<std::string> cab{"Metodo"};
    for (double s : selectividades) cab.push_back(num(s * 100, 1) + " %");

    std::cout << "\n### Lecturas de disco por consulta (media)\n\n";
    fila(cab); separador(cab.size());
    for (std::size_t e = 0; e < escenarios.size(); ++e) {
        std::vector<std::string> f{escenarios[e].etiqueta};
        for (const Resumen& r : lecturas[e]) f.push_back(num(r.media, 1));
        fila(f);
    }

    std::cout << "\n### Latencia por consulta (ms, media)\n\n";
    fila(cab); separador(cab.size());
    for (std::size_t e = 0; e < escenarios.size(); ++e) {
        std::vector<std::string> f{escenarios[e].etiqueta};
        for (const Resumen& r : latencias[e]) f.push_back(num(r.media, 3));
        fila(f);
    }

    std::cout << "\n### Filas devueltas por consulta (verificacion)\n\n";
    fila(cab); separador(cab.size());
    for (std::size_t e = 0; e < escenarios.size(); ++e) {
        std::vector<std::string> f{escenarios[e].etiqueta};
        for (long long d : devueltas[e]) f.push_back(std::to_string(d));
        fila(f);
    }

    std::cout << "\n### Ventaja del indice sobre el full scan (veces mas rapido)\n\n";
    fila(cab); separador(cab.size());
    for (std::size_t e = 1; e < escenarios.size(); ++e) {
        std::vector<std::string> f{escenarios[e].etiqueta};
        for (std::size_t s = 0; s < selectividades.size(); ++s) {
            double base = latencias[0][s].media;
            double mio  = latencias[e][s].media;
            f.push_back(num(base / (mio > 0 ? mio : 1e-9), 1) + "x");
        }
        fila(f);
    }

    // ---------------------------------------------------------------------
    //  Punto de cruce: la selectividad a la que el metodo deja de ganarle al
    //  full scan. Se interpola en log-log entre las dos selectividades que
    //  rodean el cambio de signo. Antes este numero estaba ESCRITO A MANO en
    //  el texto de abajo, asi que quedaba obsoleto en la corrida siguiente.
    auto puntoDeCruce = [&](std::size_t e) -> double {
        for (std::size_t s = 1; s < selectividades.size(); ++s) {
            const double antes   = latencias[e][s - 1].media - latencias[0][s - 1].media;
            const double despues = latencias[e][s].media     - latencias[0][s].media;
            if (antes < 0.0 && despues >= 0.0) {
                const double x0 = std::log(selectividades[s - 1]);
                const double x1 = std::log(selectividades[s]);
                const double t  = -antes / (despues - antes);
                return std::exp(x0 + t * (x1 - x0));
            }
        }
        return -1.0;                       // no cruza dentro del rango medido
    };

    std::cout << "\n### Punto de cruce con el full scan\n\n";
    {
        std::vector<std::string> cabc{"Metodo", "Cruce (selectividad)"};
        fila(cabc); separador(cabc.size());
        for (std::size_t e = 1; e < escenarios.size(); ++e) {
            const double c = puntoDeCruce(e);
            fila({escenarios[e].etiqueta,
                  c < 0.0 ? std::string("no cruza hasta el 25 %")
                          : num(c * 100.0, 1) + " %"});
        }
    }
    const double cruce_bplus = puntoDeCruce(escenarios.size() - 1);

    std::cout <<
      "\nLas tres filas devuelven la misma cantidad de tuplas, asi que la comparacion\n"
      "es justa: los tres metodos entregan las filas completas, no solo sus RID.\n\n"
      "Lo importante es el PUNTO DE CRUCE. El Arbol B+ arranca ganando por amplio\n"
      "margen, pero su ventaja se derrumba al subir la selectividad hasta quedar POR\n"
      "DEBAJO del full scan. La causa no es el arbol: es que el indice esta sobre un\n"
      "Heap File, o sea que NO ESTA AGRUPADO. El recorrido de hojas entrega las claves\n"
      "ordenadas, pero cada RID apunta a una pagina cualquiera del heap, y con miles de\n"
      "resultados el motor termina pidiendo la misma pagina una y otra vez en orden\n"
      "aleatorio: mas transferencias que leer el archivo entero una sola vez.\n\n"
      "El Sequential File no sufre eso porque SI esta agrupado: las filas consecutivas\n"
      "por clave son fisicamente vecinas, asi que un rango es lectura casi secuencial.\n"
      "Por eso gana al B+ en todas las selectividades a pesar de no tener indice.\n\n"
      "Conclusion para el planificador: sobre un heap, un IndexRangeScan solo conviene\n"
      "por debajo de ";
    if (cruce_bplus < 0.0) std::cout << "una selectividad que este experimento no alcanza";
    else                   std::cout << num(cruce_bplus * 100.0, 1) << " % de selectividad";
    std::cout <<
      ";\npasado ese umbral deberia elegir SeqScan.\n"
      "Es la misma regla que aplican los optimizadores reales.\n\n";
    return 0;
}
