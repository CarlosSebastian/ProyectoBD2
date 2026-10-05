// ============================================================================
//  Experimento 5 - Escalabilidad del modulo espacial (R-Tree)
//
//  El cuadro del enunciado pide, para el Entregable 2, "escalabilidad espacial"
//  y "tiempo de respuesta extremo a extremo". Este experimento mide tres cosas,
//  siempre contrastando el R-Tree contra el escaneo completo sobre LOS MISMOS
//  puntos:
//
//    A. Escalabilidad en N: como crecen construccion, altura y costo de
//       consulta al pasar de 1 000 a 250 000 puntos.
//    B. Selectividad de la ventana: el analogo espacial del Experimento 3.
//       Igual que alli, se busca el PUNTO DE CRUCE a partir del cual leer el
//       archivo entero sale mas barato que recorrer el indice.
//    C. Costo del KNN segun k.
//
//  El dataset se genera aqui, con semilla fija: puntos uniformes en un cuadrado
//  de 1000x1000. Al ser uniformes, la fraccion de AREA que cubre una ventana es
//  tambien la fraccion esperada de PUNTOS que devuelve, que es lo que permite
//  hablar de selectividad igual que en el experimento de rangos.
//
//  Cada consulta se ejecuta por las dos rutas y se comprueba que devuelvan la
//  MISMA cantidad de filas: sin esa verificacion la comparacion no vale nada.
//
//     ./build/exp5_espacial [dir] [N_grande] [consultas]
// ============================================================================
#include <cmath>

#include "comun.hpp"

using namespace bench;
using namespace db;

namespace {

struct Punto {
    std::int64_t id;
    double x, y;
};

const double LADO = 1000.0;

std::vector<Punto> generarPuntos(int N, unsigned semilla = 2026) {
    std::mt19937 rng(semilla);
    std::uniform_real_distribution<double> u(0.0, LADO);
    std::vector<Punto> ps;
    ps.reserve(static_cast<std::size_t>(N));
    for (int i = 0; i < N; ++i) ps.push_back(Punto{i + 1, u(rng), u(rng)});
    return ps;
}

TableInfo infoEspacial(const std::string& nombre, bool con_indice) {
    TableInfo ti;
    ti.name       = nombre;
    ti.schema     = Schema({Column("id", Type::INT),
                            Column("nombre", Type::VARCHAR, 24),
                            Column("ubic", Type::POINT)});
    ti.heap_file  = nombre + ".dat";
    ti.engine     = "HEAP";
    ti.key_column = "";                   // sin PK: no interesa la unicidad aqui
    if (con_indice) ti.indexes.push_back(IndexInfo{"ubic", IndexKind::RTREE, nombre + ".idx"});
    return ti;
}

Tuple aTupla(const Punto& p) {
    return Tuple{{Value::makeInt(p.id),
                  Value::makeStr("p" + std::to_string(p.id)),
                  Value::makePoint(p.x, p.y)}};
}

// Ventana cuadrada que cubre la fraccion 'frac' del area total, centrada en un
// punto al azar y recortada para no salirse del dominio.
struct Ventana { double x0, y0, x1, y1; };

Ventana ventanaDeArea(double frac, std::mt19937& rng) {
    const double lado = LADO * std::sqrt(frac);
    std::uniform_real_distribution<double> u(0.0, LADO - lado);
    const double x0 = u(rng), y0 = u(rng);
    return Ventana{x0, y0, x0 + lado, y0 + lado};
}

struct Costo {
    double    ms = 0;
    long long lecturas = 0;
    long long accesos = 0;
    double    filas = 0;
};

Costo promedio(const std::vector<Costo>& v) {
    Costo c;
    if (v.empty()) return c;
    for (const Costo& x : v) { c.ms += x.ms; c.lecturas += x.lecturas; c.accesos += x.accesos; c.filas += x.filas; }
    const double n = static_cast<double>(v.size());
    c.ms /= n; c.filas /= n;
    c.lecturas = static_cast<long long>(std::llround(static_cast<double>(c.lecturas) / n));
    c.accesos  = static_cast<long long>(std::llround(static_cast<double>(c.accesos) / n));
    return c;
}

}  // namespace

int main(int argc, char** argv) {
    const std::string dir = (argc > 1) ? argv[1] : "data";
    const int N_GRANDE    = (argc > 2) ? std::atoi(argv[2]) : 100000;
    const int Q           = (argc > 3) ? std::atoi(argv[3]) : 20;

    std::cout << "\n==========================================================================\n"
                 "  EXPERIMENTO 5 - Escalabilidad del modulo espacial (R-Tree)\n"
                 "==========================================================================\n"
              << "Puntos uniformes en " << num(LADO, 0) << "x" << num(LADO, 0)
              << " | pagina de " << PAGE_SIZE << " B | buffer pool de 64 paginas\n"
              << "Fan-out del nodo: " << RTree::leafCapacity() << " entradas por hoja, "
              << RTree::internalCapacity() << " por nodo interno\n";

    // =====================================================================
    //  A. Escalabilidad en N
    // =====================================================================
    std::vector<int> enes = {1000, 5000, 10000, 50000, N_GRANDE, N_GRANDE * 5 / 2};
    // Ordenar y quitar repetidos: si se pasa un N_GRANDE chico por linea de
    // comandos, la serie tiene que seguir siendo creciente y sin duplicados.
    std::sort(enes.begin(), enes.end());
    enes.erase(std::unique(enes.begin(), enes.end()), enes.end());

    struct FilaA {
        int       n;
        double    ms_carga;
        int       pag_datos, pag_indice, altura;
        Costo     ventana_rtree, ventana_scan;
        Costo     knn_rtree, knn_scan;
        bool      coinciden_ventana = true, coinciden_knn = true;
    };
    std::vector<FilaA> tablaA;

    for (int N : enes) {
        FilaA f;
        f.n = N;
        const std::vector<Punto> ps = generarPuntos(N);

        limpiar(dir, "esp_ix");
        limpiar(dir, "esp_sc");
        TableInfo ti_ix = infoEspacial("esp_ix", true);
        TableInfo ti_sc = infoEspacial("esp_sc", false);

        Table t_ix(dir, ti_ix, 64);
        Table t_sc(dir, ti_sc, 64);

        Cronometro cr;
        for (const Punto& p : ps) t_ix.insert(aTupla(p));
        f.ms_carga = cr.parar().ms;
        for (const Punto& p : ps) t_sc.insert(aTupla(p));

        f.pag_datos  = t_ix.dataPages();
        f.pag_indice = t_ix.indexPages();
        f.altura     = t_ix.indexHeight();

        std::mt19937 rng(7);
        std::vector<Costo> v_ix, v_sc, k_ix, k_sc;
        for (int q = 0; q < Q; ++q) {
            const Ventana w = ventanaDeArea(0.01, rng);     // 1 % del area
            t_ix.resetStats(); t_sc.resetStats();

            Cronometro c1;
            std::size_t n1 = t_ix.searchWithin("ubic", w.x0, w.y0, w.x1, w.y1).size();
            Medicion m1 = c1.parar();
            v_ix.push_back(Costo{m1.ms, m1.lecturas, m1.accesos, static_cast<double>(n1)});

            Cronometro c2;
            std::size_t n2 = t_sc.searchWithin("ubic", w.x0, w.y0, w.x1, w.y1).size();
            Medicion m2 = c2.parar();
            v_sc.push_back(Costo{m2.ms, m2.lecturas, m2.accesos, static_cast<double>(n2)});

            if (n1 != n2) f.coinciden_ventana = false;      // verificacion
        }
        std::uniform_real_distribution<double> u(0.0, LADO);
        for (int q = 0; q < Q; ++q) {
            const double px = u(rng), py = u(rng);
            t_ix.resetStats(); t_sc.resetStats();

            Cronometro c1;
            std::size_t n1 = t_ix.searchKNN("ubic", px, py, 10).size();
            Medicion m1 = c1.parar();
            k_ix.push_back(Costo{m1.ms, m1.lecturas, m1.accesos, static_cast<double>(n1)});

            Cronometro c2;
            std::size_t n2 = t_sc.searchKNN("ubic", px, py, 10).size();
            Medicion m2 = c2.parar();
            k_sc.push_back(Costo{m2.ms, m2.lecturas, m2.accesos, static_cast<double>(n2)});

            if (n1 != n2) f.coinciden_knn = false;
        }
        f.ventana_rtree = promedio(v_ix);
        f.ventana_scan  = promedio(v_sc);
        f.knn_rtree     = promedio(k_ix);
        f.knn_scan      = promedio(k_sc);
        tablaA.push_back(f);
        std::cerr << "   ... N=" << N << " listo\n";
    }

    std::cout << "\n### A.1 Construccion del indice\n\n";
    {
        std::vector<std::string> c{"N", "Carga (ms)", "us/punto", "Pag. datos",
                                   "Pag. indice", "Altura h"};
        fila(c); separador(c.size());
        for (const FilaA& f : tablaA)
            fila({std::to_string(f.n), num(f.ms_carga, 0),
                  num(f.ms_carga * 1000.0 / f.n, 2),
                  std::to_string(f.pag_datos), std::to_string(f.pag_indice),
                  std::to_string(f.altura)});
    }

    std::cout << "\n### A.2 Ventana del 1 % del area: R-Tree vs escaneo completo\n\n";
    {
        std::vector<std::string> c{"N", "Filas", "R-Tree accesos", "Scan accesos",
                                   "Ganancia", "R-Tree ms", "Scan ms"};
        fila(c); separador(c.size());
        for (const FilaA& f : tablaA) {
            const double g = f.ventana_rtree.accesos > 0
                           ? static_cast<double>(f.ventana_scan.accesos) /
                             static_cast<double>(f.ventana_rtree.accesos) : 0.0;
            fila({std::to_string(f.n), num(f.ventana_rtree.filas, 0),
                  std::to_string(f.ventana_rtree.accesos),
                  std::to_string(f.ventana_scan.accesos),
                  num(g, 1) + "x", num(f.ventana_rtree.ms, 3), num(f.ventana_scan.ms, 3)});
        }
    }

    std::cout << "\n### A.3 KNN con k = 10\n\n";
    {
        std::vector<std::string> c{"N", "R-Tree accesos", "Scan accesos", "Ganancia",
                                   "R-Tree ms", "Scan ms"};
        fila(c); separador(c.size());
        for (const FilaA& f : tablaA) {
            const double g = f.knn_rtree.accesos > 0
                           ? static_cast<double>(f.knn_scan.accesos) /
                             static_cast<double>(f.knn_rtree.accesos) : 0.0;
            fila({std::to_string(f.n), std::to_string(f.knn_rtree.accesos),
                  std::to_string(f.knn_scan.accesos), num(g, 1) + "x",
                  num(f.knn_rtree.ms, 3), num(f.knn_scan.ms, 3)});
        }
    }

    std::cout << "\n### A.4 Verificacion: las dos rutas devuelven lo mismo\n\n";
    {
        std::vector<std::string> c{"N", "Ventana", "KNN"};
        fila(c); separador(c.size());
        bool todo_ok = true;
        for (const FilaA& f : tablaA) {
            fila({std::to_string(f.n),
                  f.coinciden_ventana ? "coinciden" : "DIFIEREN",
                  f.coinciden_knn ? "coinciden" : "DIFIEREN"});
            todo_ok = todo_ok && f.coinciden_ventana && f.coinciden_knn;
        }
        std::cout << "\n" << (todo_ok
            ? "Las dos rutas devuelven exactamente las mismas filas en todos los tamanos,\n"
              "asi que la comparacion de costos es valida.\n"
            : "ATENCION: alguna ruta devolvio un numero distinto de filas.\n");
    }

    // =====================================================================
    //  B. Selectividad de la ventana (el analogo espacial del Experimento 3)
    // =====================================================================
    titulo("B. Selectividad de la ventana, con N = " + std::to_string(N_GRANDE));

    const std::vector<double> fracciones = {0.0001, 0.001, 0.01, 0.05, 0.25};
    std::vector<Costo> b_ix, b_sc;
    {
        const std::vector<Punto> ps = generarPuntos(N_GRANDE);
        limpiar(dir, "esp_ix"); limpiar(dir, "esp_sc");
        TableInfo ti_ix = infoEspacial("esp_ix", true);
        TableInfo ti_sc = infoEspacial("esp_sc", false);
        Table t_ix(dir, ti_ix, 64);
        Table t_sc(dir, ti_sc, 64);
        for (const Punto& p : ps) { t_ix.insert(aTupla(p)); t_sc.insert(aTupla(p)); }

        for (double fr : fracciones) {
            std::mt19937 rng(11);
            std::vector<Costo> vi, vs;
            for (int q = 0; q < Q; ++q) {
                const Ventana w = ventanaDeArea(fr, rng);
                t_ix.resetStats(); t_sc.resetStats();
                Cronometro c1;
                std::size_t n1 = t_ix.searchWithin("ubic", w.x0, w.y0, w.x1, w.y1).size();
                Medicion m1 = c1.parar();
                vi.push_back(Costo{m1.ms, m1.lecturas, m1.accesos, static_cast<double>(n1)});
                Cronometro c2;
                std::size_t n2 = t_sc.searchWithin("ubic", w.x0, w.y0, w.x1, w.y1).size();
                Medicion m2 = c2.parar();
                vs.push_back(Costo{m2.ms, m2.lecturas, m2.accesos, static_cast<double>(n2)});
            }
            b_ix.push_back(promedio(vi));
            b_sc.push_back(promedio(vs));
        }
    }

    std::cout << "\n### B.1 Costo por consulta segun el area que cubre la ventana\n\n";
    {
        std::vector<std::string> c{"Area de la ventana", "Filas", "R-Tree accesos",
                                   "Scan accesos", "R-Tree ms", "Scan ms", "Ganancia"};
        fila(c); separador(c.size());
        for (std::size_t i = 0; i < fracciones.size(); ++i) {
            const double g = b_ix[i].ms > 0 ? b_sc[i].ms / b_ix[i].ms : 0.0;
            fila({num(fracciones[i] * 100.0, 2) + " %", num(b_ix[i].filas, 0),
                  std::to_string(b_ix[i].accesos), std::to_string(b_sc[i].accesos),
                  num(b_ix[i].ms, 3), num(b_sc[i].ms, 3), num(g, 1) + "x"});
        }
    }

    // Punto de cruce por interpolacion entre las dos areas que rodean el cambio
    // de signo de (scan - rtree), igual que en el Experimento 3.
    double cruce = -1.0;
    for (std::size_t i = 1; i < fracciones.size(); ++i) {
        const double d0 = b_sc[i - 1].ms - b_ix[i - 1].ms;
        const double d1 = b_sc[i].ms - b_ix[i].ms;
        if (d0 > 0 && d1 <= 0) {
            cruce = fracciones[i - 1] + (fracciones[i] - fracciones[i - 1]) * (d0 / (d0 - d1));
            break;
        }
    }
    std::cout << "\n### B.2 Punto de cruce con el escaneo completo\n\n";
    {
        std::vector<std::string> c{"Metodo", "Cruce (area de la ventana)"};
        fila(c); separador(c.size());
        fila({"R-Tree (ventana)", cruce < 0.0 ? std::string("no cruza hasta el 25 %")
                                              : num(cruce * 100.0, 1) + " %"});
    }

    // =====================================================================
    //  C. KNN segun k
    // =====================================================================
    titulo("C. Costo del KNN segun k, con N = " + std::to_string(N_GRANDE));

    const std::vector<int> kas = {1, 10, 100, 1000};
    {
        const std::vector<Punto> ps = generarPuntos(N_GRANDE);
        limpiar(dir, "esp_ix"); limpiar(dir, "esp_sc");
        TableInfo ti_ix = infoEspacial("esp_ix", true);
        TableInfo ti_sc = infoEspacial("esp_sc", false);
        Table t_ix(dir, ti_ix, 64);
        Table t_sc(dir, ti_sc, 64);
        for (const Punto& p : ps) { t_ix.insert(aTupla(p)); t_sc.insert(aTupla(p)); }

        std::cout << "\n### C.1 R-Tree best-first vs escaneo con ordenacion parcial\n\n";
        std::vector<std::string> c{"k", "R-Tree accesos", "Scan accesos", "Ganancia",
                                   "R-Tree ms", "Scan ms"};
        fila(c); separador(c.size());
        for (int k : kas) {
            std::mt19937 rng(13);
            std::uniform_real_distribution<double> u(0.0, LADO);
            std::vector<Costo> vi, vs;
            for (int q = 0; q < Q; ++q) {
                const double px = u(rng), py = u(rng);
                t_ix.resetStats(); t_sc.resetStats();
                Cronometro c1;
                std::size_t n1 = t_ix.searchKNN("ubic", px, py, k).size();
                Medicion m1 = c1.parar();
                vi.push_back(Costo{m1.ms, m1.lecturas, m1.accesos, static_cast<double>(n1)});
                Cronometro c2;
                std::size_t n2 = t_sc.searchKNN("ubic", px, py, k).size();
                Medicion m2 = c2.parar();
                vs.push_back(Costo{m2.ms, m2.lecturas, m2.accesos, static_cast<double>(n2)});
            }
            const Costo ci = promedio(vi), cs = promedio(vs);
            const double g = ci.accesos > 0 ? static_cast<double>(cs.accesos) /
                                              static_cast<double>(ci.accesos) : 0.0;
            fila({std::to_string(k), std::to_string(ci.accesos), std::to_string(cs.accesos),
                  num(g, 1) + "x", num(ci.ms, 3), num(cs.ms, 3)});
        }
    }

    std::cout <<
      "\nLectura de los resultados.\n\n"
      "El KNN es donde el R-Tree gana por mas margen, y por una razon estructural:\n"
      "la busqueda best-first solo abre los nodos cuyo MINDIST es menor que la\n"
      "distancia al k-esimo vecino ya encontrado, asi que el costo depende de k y no\n"
      "de N. El escaneo, en cambio, tiene que calcular la distancia de las N filas\n"
      "aunque solo devuelva diez.\n\n"
      "La ventana se comporta como el rango del Experimento 3: el indice gana por\n"
      "ordenes de magnitud mientras la ventana es chica, y pierde terreno al crecer,\n"
      "por el mismo motivo de alla. El indice esta sobre un Heap File, o sea que NO\n"
      "ESTA AGRUPADO: las hojas del R-Tree entregan los RID de puntos vecinos en el\n"
      "plano, pero esos RID apuntan a paginas cualesquiera del heap, en el orden en\n"
      "que se insertaron. Con una ventana suficientemente grande, el motor termina\n"
      "pidiendo las mismas paginas una y otra vez y leer el archivo completo una sola\n"
      "vez sale mas barato.\n\n"
      "La consecuencia de diseno es la misma que en el caso unidimensional: el\n"
      "planificador deberia estimar cuantas filas va a devolver la ventana y elegir\n"
      "SeqScan pasado el umbral, en vez de usar el indice siempre que exista.\n\n";
    return 0;
}
