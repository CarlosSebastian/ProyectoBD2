// ============================================================================
//  Experimento 1 - Costo de insercion masiva
//
//  Compara tiempo total y escrituras de disco al insertar lotes crecientes en
//  las cinco configuraciones del enunciado:
//
//      Heap File
//      Sequential File SIN reorganizacion
//      Sequential File CON reorganizacion periodica
//      Heap + Arbol B+
//      Heap + Hash dinamico
//
//     ./build/exp1_insercion [dir] [N_max]
// ============================================================================
#include "comun.hpp"

using namespace bench;
using namespace db;

struct Config {
    std::string  etiqueta;
    Organizacion org;
    bool         indice;
    IndexKind    tipo;
    bool         auto_reorg;
    int          tope_n;     // mas alla de esto la configuracion no termina
};

struct Punto {
    double    ms         = -1;
    long long escrituras = -1;
    long long lecturas   = -1;
    int       paginas    = -1;
    long long reorgs     = -1;
    bool      medido     = false;
};

static Punto correr(const Config& c, const std::vector<Fila>& filas, int N,
                    const std::string& dir) {
    limpiar(dir, "e1");
    Table t(dir, info("e1", c.org, c.indice, c.tipo), 64);
    t.setAutoReorganize(c.auto_reorg);

    Cronometro cron;
    for (int i = 0; i < N; ++i) t.insert(aTupla(filas[static_cast<std::size_t>(i)]));
    Medicion m = cron.parar();

    Punto p;
    p.ms         = m.ms;
    p.escrituras = m.escrituras;
    p.lecturas   = m.lecturas;
    p.paginas    = t.dataPages() + t.indexPages();
    p.reorgs     = t.reorganizaciones();
    p.medido     = true;
    return p;
}

int main(int argc, char** argv) {
    std::string dir  = (argc > 1) ? argv[1] : "data";
    int         tope = (argc > 2) ? std::atoi(argv[2]) : 500000;

    std::vector<int> Ns = {1000, 10000, 50000, 100000, 250000, 500000};
    Ns.erase(std::remove_if(Ns.begin(), Ns.end(), [&](int n) { return n > tope; }), Ns.end());

    std::string csv = rutaDataset(dir);
    std::vector<Fila> filas = cargarCSV(csv);
    if (filas.empty()) {
        std::cerr << "No se encontro " << csv << ". Corre antes: ./build/gen_dataset\n";
        return 1;
    }

    // El Sequential SIN reorganizar es super-cuadratico: cada insercion recorre
    // la cadena de overflow entera para mantener el orden logico, y cuando esa
    // cadena deja de caber en el buffer pool cada paso pasa a ser I/O real.
    // Se mide aparte, con su propia serie.
    const int TOPE_SIN_REORG = 10000;

    std::vector<Config> configs = {
        {"Heap File",             Organizacion::HEAP,       false, IndexKind::BPLUS, false, 1 << 30},
        {"Sequential sin reorg.", Organizacion::SEQUENTIAL, false, IndexKind::BPLUS, false, TOPE_SIN_REORG},
        {"Sequential con reorg.", Organizacion::SEQUENTIAL, false, IndexKind::BPLUS, true,  1 << 30},
        {"Heap + B+ Tree",        Organizacion::HEAP,       true,  IndexKind::BPLUS, false, 1 << 30},
        {"Heap + Hash dinamico",  Organizacion::HEAP,       true,  IndexKind::HASH,  false, 1 << 30},
    };

    titulo("EXPERIMENTO 1 - Costo de insercion masiva");
    std::cout << "Pagina de " << PAGE_SIZE << " B | buffer pool de 64 paginas | "
                 "claves desordenadas\n";

    std::vector<std::string> cab{"Estructura"};
    for (int n : Ns) cab.push_back("N=" + std::to_string(n));

    std::vector<std::vector<Punto>> todo(configs.size());
    for (std::size_t c = 0; c < configs.size(); ++c)
        for (int n : Ns) {
            if (n > configs[c].tope_n) { todo[c].push_back(Punto{}); continue; }
            todo[c].push_back(correr(configs[c], filas, n, dir));
            std::cerr << "." << std::flush;
        }
    std::cerr << "\n";

    std::cout << "\n### Tiempo total de carga (ms)\n\n";
    fila(cab); separador(cab.size());
    for (std::size_t c = 0; c < configs.size(); ++c) {
        std::vector<std::string> f{configs[c].etiqueta};
        for (const Punto& p : todo[c]) f.push_back(p.medido ? num(p.ms, 0) : "no termina");
        fila(f);
    }

    std::cout << "\n### Escrituras de disco (bloques de " << PAGE_SIZE << " B)\n\n";
    fila(cab); separador(cab.size());
    for (std::size_t c = 0; c < configs.size(); ++c) {
        std::vector<std::string> f{configs[c].etiqueta};
        for (const Punto& p : todo[c]) f.push_back(p.medido ? std::to_string(p.escrituras) : "-");
        fila(f);
    }

    std::cout << "\n### Paginas ocupadas y costo por tupla en el N mas grande medido\n\n";
    fila({"Estructura", "N alcanzado", "Paginas", "us/tupla", "Reorganizaciones"});
    separador(5);
    for (std::size_t c = 0; c < configs.size(); ++c) {
        int idx = -1;
        for (int i = static_cast<int>(todo[c].size()) - 1; i >= 0; --i)
            if (todo[c][static_cast<std::size_t>(i)].medido) { idx = i; break; }
        if (idx < 0) continue;
        const Punto& p = todo[c][static_cast<std::size_t>(idx)];
        int n = Ns[static_cast<std::size_t>(idx)];
        fila({configs[c].etiqueta, std::to_string(n), std::to_string(p.paginas),
              num(p.ms * 1000.0 / n, 2), std::to_string(p.reorgs)});
    }

    // ------------------------------------------------------------------------
    //  Estudio aparte: como se degrada el Sequential sin mantenimiento
    // ------------------------------------------------------------------------
    std::cout << "\n### Degradacion del Sequential File SIN reorganizacion\n\n";
    fila({"N", "Tiempo (ms)", "us/tupla", "Lecturas de disco"});
    separador(4);
    Config sin = configs[1];
    sin.tope_n = 1 << 30;
    for (int n : {1000, 5000, 10000, 20000}) {
        if (n > tope) break;
        Punto p = correr(sin, filas, n, dir);
        fila({std::to_string(n), num(p.ms, 0), num(p.ms * 1000.0 / n, 1),
              std::to_string(p.lecturas)});
        std::cerr << "." << std::flush;
    }
    std::cerr << "\n";
    std::cout << "\nEl costo por tupla crece con N, asi que el costo total es cuadratico:\n"
                 "toda la carga cae en una unica cadena de overflow que hay que recorrer\n"
                 "entera en cada insercion para mantener el orden logico. Mientras esa\n"
                 "cadena cabe en el buffer pool el efecto es solo de CPU (0 lecturas);\n"
                 "cuando deja de caber, cada paso se convierte en I/O real y el tiempo\n"
                 "se dispara. Es exactamente el motivo por el que la reorganizacion\n"
                 "periodica no es opcional.\n\n";
    return 0;
}
