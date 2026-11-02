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
//    C. Costo del KNN segun k y del radio r, con k en {5, 20, 50} y
//       N en {10^4, 5x10^4, 10^5}, que es lo que pide el enunciado.
//    D. Efecto de la DIMENSION. Es la pregunta de fondo del Entregable 2: a
//       partir de que D el solapamiento de las cajas hace que recorrer el
//       archivo entero salga mas barato que bajar por el arbol. Y es tambien
//       la justificacion de que la Parte 4 use IVF y no un arbol espacial.
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
#include <queue>
#include <set>

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

// ---------------------------------------------------------------------------
//  Puntos geograficos alrededor de Lima, para las consultas de radio: el
//  haversine solo significa algo sobre coordenadas de verdad.
// ---------------------------------------------------------------------------
std::vector<Punto> generarGeo(int N, unsigned semilla = 2026) {
    std::mt19937 rng(semilla);
    std::uniform_real_distribution<double> ulon(-77.20, -76.90);
    std::uniform_real_distribution<double> ulat(-12.30, -12.00);
    std::vector<Punto> ps;
    ps.reserve(static_cast<std::size_t>(N));
    for (int i = 0; i < N; ++i) ps.push_back(Punto{i + 1, ulon(rng), ulat(rng)});
    return ps;
}

// ===========================================================================
//  R-Tree de D dimensiones, SOLO para este experimento.
//
//  Por que no se generaliza el del motor: su MBR es ⟨minx, miny, maxx, maxy⟩ y
//  de ahi cuelgan el tipo POINT, el POLYGON, el planificador y el visor de
//  mapas. Cambiarlo a D dimensiones para responder una pregunta de un
//  experimento seria rehacer media Parte 2 sin que ninguna consulta real lo
//  aproveche. Lo que el experimento necesita es el COMPORTAMIENTO del arbol
//  al crecer D, y eso se mide con un arbol equivalente.
//
//  Fidelidad al modelo de costo: cada nodo se dimensiona para ocupar EXACTAMENTE
//  una pagina de PAGE_SIZE bytes, asi que "nodos visitados" y "bloques leidos"
//  son el mismo numero. El fan-out sale de la aritmetica de la pagina:
//      entradas = (PAGE_SIZE - cabecera) / (2*D*sizeof(float) + sizeof(int))
//  y por eso se desploma al subir D. Ese desplome es ya media respuesta.
//
//  Como se construye: CARGA MASIVA, no insercion una a una. El arbol se arma
//  de abajo arriba ordenando por el eje de mayor dispersion y partiendo en
//  grupos del tamano de una pagina (la familia Sort-Tile-Recursive). Esto
//  importa para que el experimento signifique algo: con insercion incremental
//  y un split heuristico, la calidad del arbol depende del heuristico, y lo
//  que saldria medido seria mi split y no la dimension. Medido: con un split
//  lineal el arbol ya perdia contra el escaneo en D = 2, que es falso. Con
//  carga masiva las cajas quedan tan buenas como pueden quedar, y lo unico
//  que se degrada al subir D es la geometria --- que es justo lo que se quiere
//  observar.
//
//  Un detalle numerico: como criterio de dispersion se usa el ANCHO por eje y
//  no el volumen. El producto de D lados menores que uno se va a cero en coma
//  flotante mucho antes de D = 128 y todos los ejes empatarian en cero.
// ===========================================================================
class RTreeND {
public:
    RTreeND(int d, int cap) : d_(d), cap_(cap), raiz_(-1) {}

    static int fanOut(int d) {
        const int por_entrada = 2 * d * static_cast<int>(sizeof(float)) +
                                static_cast<int>(sizeof(int));
        int n = (PAGE_SIZE - 8) / por_entrada;
        return n < 2 ? 2 : n;          // con menos de dos entradas no hay arbol
    }

    void cargar(const std::vector<std::vector<float>>& datos) {
        datos_ = &datos;
        orden_.resize(datos.size());
        for (std::size_t i = 0; i < orden_.size(); ++i) orden_[i] = static_cast<int>(i);
        nodos_.clear();
        raiz_ = construir(0, static_cast<int>(orden_.size()));
    }

    // Best-first por MINDIST. Devuelve los ids de los k mas cercanos y deja en
    // *paginas cuantos nodos (= paginas) hubo que abrir.
    std::vector<int> knn(const std::vector<float>& q, int k, long long* paginas) const {
        struct Cand { double d; bool es_punto; int ref; };
        struct Peor { bool operator()(const Cand& a, const Cand& b) const { return a.d > b.d; } };
        std::priority_queue<Cand, std::vector<Cand>, Peor> cola;
        std::vector<int> salida;
        long long abiertas = 0;
        if (raiz_ < 0) { if (paginas) *paginas = 0; return salida; }
        cola.push(Cand{0.0, false, raiz_});

        while (!cola.empty() && static_cast<int>(salida.size()) < k) {
            const Cand c = cola.top();
            cola.pop();
            if (c.es_punto) { salida.push_back(c.ref); continue; }
            ++abiertas;                                   // una pagina leida
            const Nodo& n = nodos_[static_cast<std::size_t>(c.ref)];
            for (const Entrada& e : n.es)
                cola.push(Cand{mindist2(q, e), n.hoja, n.hoja ? e.id : e.hijo});
        }
        if (paginas) *paginas = abiertas;
        return salida;
    }

    std::size_t numNodos() const { return nodos_.size(); }
    int altura() const { return raiz_ < 0 ? 0 : alturaDe(raiz_); }

private:
    struct Entrada { std::vector<float> lo, hi; int hijo = -1; int id = -1; };
    struct Nodo { bool hoja = true; std::vector<Entrada> es; };

    const std::vector<float>& punto(int i) const { return (*datos_)[static_cast<std::size_t>(i)]; }

    double mindist2(const std::vector<float>& q, const Entrada& e) const {
        double s = 0.0;
        for (int j = 0; j < d_; ++j) {
            const double x  = q[static_cast<std::size_t>(j)];
            const double lo = e.lo[static_cast<std::size_t>(j)];
            const double hi = e.hi[static_cast<std::size_t>(j)];
            const double dd = x < lo ? (lo - x) : (x > hi ? (x - hi) : 0.0);
            s += dd * dd;
        }
        return s;
    }

    static void absorber(Entrada* e, const Entrada& otra) {
        for (std::size_t j = 0; j < e->lo.size(); ++j) {
            e->lo[j] = std::min(e->lo[j], otra.lo[j]);
            e->hi[j] = std::max(e->hi[j], otra.hi[j]);
        }
    }

    Entrada envolvente(const Nodo& n) const {
        Entrada e = n.es.front();
        for (const Entrada& x : n.es) absorber(&e, x);
        e.hijo = -1;
        e.id   = -1;
        return e;
    }

    int alturaDe(int nodo) const {
        const Nodo& n = nodos_[static_cast<std::size_t>(nodo)];
        return n.hoja ? 1 : 1 + alturaDe(n.es.front().hijo);
    }

    // Construye el subarbol de orden_[ini, fin) y devuelve su indice de nodo.
    int construir(int ini, int fin) {
        if (fin - ini <= cap_) {
            Nodo h;
            h.hoja = true;
            for (int i = ini; i < fin; ++i) {
                Entrada e;
                e.lo = punto(orden_[static_cast<std::size_t>(i)]);
                e.hi = e.lo;
                e.id = orden_[static_cast<std::size_t>(i)];
                h.es.push_back(std::move(e));
            }
            nodos_.push_back(std::move(h));
            return static_cast<int>(nodos_.size()) - 1;
        }

        // Eje de mayor dispersion dentro de este grupo.
        int eje = 0;
        float mejor = -1.0f;
        for (int j = 0; j < d_; ++j) {
            float lo = punto(orden_[static_cast<std::size_t>(ini)])[static_cast<std::size_t>(j)];
            float hi = lo;
            for (int i = ini + 1; i < fin; ++i) {
                const float v = punto(orden_[static_cast<std::size_t>(i)])[static_cast<std::size_t>(j)];
                lo = std::min(lo, v);
                hi = std::max(hi, v);
            }
            if (hi - lo > mejor) { mejor = hi - lo; eje = j; }
        }
        std::sort(orden_.begin() + ini, orden_.begin() + fin,
                  [&](int a, int b) {
                      return punto(a)[static_cast<std::size_t>(eje)] <
                             punto(b)[static_cast<std::size_t>(eje)];
                  });

        // Cuanto le toca a cada hijo. NO es n/cap_: hay que repartir en grupos
        // del tamano que puede tragarse un subarbol del nivel de abajo, o si no
        // un grupo de 61 puntos con capacidad 60 se parte en 31 grupos de dos y
        // el arbol sale lleno de nodos medio vacios. Se calcula primero la
        // altura que hace falta y de ahi el tamano del subarbol hijo.
        const int n = fin - ini;
        long long capsub = cap_;
        while (capsub < n) capsub *= cap_;          // capacidad del subarbol
        const int tam = static_cast<int>(capsub / cap_);
        std::vector<int> hijos;
        for (int a = ini; a < fin; a += tam) hijos.push_back(construir(a, std::min(a + tam, fin)));

        Nodo in;
        in.hoja = false;
        for (int h : hijos) {
            Entrada e = envolvente(nodos_[static_cast<std::size_t>(h)]);
            e.hijo = h;
            in.es.push_back(std::move(e));
        }
        nodos_.push_back(std::move(in));
        return static_cast<int>(nodos_.size()) - 1;
    }

    int d_, cap_, raiz_;
    const std::vector<std::vector<float>>* datos_ = nullptr;
    std::vector<int>  orden_;
    std::vector<Nodo> nodos_;
};

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

    // =====================================================================
    //  C. k-NN con k en {5, 20, 50} y radio r, sobre coordenadas geograficas
    // =====================================================================
    std::cout << "\n### C. k-NN y radio sobre coordenadas reales (haversine)\n\n";
    {
        std::vector<int> enesC = {10000, 50000, N_GRANDE};
        std::sort(enesC.begin(), enesC.end());
        enesC.erase(std::unique(enesC.begin(), enesC.end()), enesC.end());

        fila({"N", "consulta", "R-Tree accesos", "Scan accesos", "Ganancia",
              "R-Tree ms", "Scan ms", "filas"});
        separador(8);
        for (int N : enesC) {
            const std::vector<Punto> ps = generarGeo(N);
            limpiar(dir, "geo_ix");
            limpiar(dir, "geo_sc");
            Table t_ix(dir, infoEspacial("geo_ix", true), 64);
            Table t_sc(dir, infoEspacial("geo_sc", false), 64);
            for (const Punto& p : ps) { t_ix.insert(aTupla(p)); t_sc.insert(aTupla(p)); }

            std::mt19937 rng(11);
            std::uniform_real_distribution<double> ulon(-77.20, -76.90);
            std::uniform_real_distribution<double> ulat(-12.30, -12.00);

            auto medirPar = [&](const std::string& etiqueta, bool es_knn, int k, double metros) {
                std::vector<Costo> a, b;
                for (int q = 0; q < Q; ++q) {
                    const double lon = ulon(rng), lat = ulat(rng);
                    t_ix.resetStats(); t_sc.resetStats();
                    Cronometro c1;
                    const std::size_t n1 = es_knn ? t_ix.searchKNNGeo("ubic", lon, lat, k).size()
                                                  : t_ix.searchRadio("ubic", lon, lat, metros).size();
                    const Medicion m1 = c1.parar();
                    a.push_back(Costo{m1.ms, m1.lecturas, m1.accesos, static_cast<double>(n1)});
                    Cronometro c2;
                    const std::size_t n2 = es_knn ? t_sc.searchKNNGeo("ubic", lon, lat, k).size()
                                                  : t_sc.searchRadio("ubic", lon, lat, metros).size();
                    const Medicion m2 = c2.parar();
                    b.push_back(Costo{m2.ms, m2.lecturas, m2.accesos, static_cast<double>(n2)});
                    if (n1 != n2)
                        std::cerr << "   AVISO: las dos rutas difieren en " << etiqueta
                                  << " (" << n1 << " vs " << n2 << ")\n";
                }
                const Costo ca = promedio(a), cb = promedio(b);
                const double g = ca.accesos > 0 ? static_cast<double>(cb.accesos) /
                                                  static_cast<double>(ca.accesos) : 0.0;
                fila({std::to_string(N), etiqueta,
                      std::to_string(ca.accesos), std::to_string(cb.accesos),
                      num(g, 1) + "x", num(ca.ms, 3), num(cb.ms, 3), num(ca.filas, 1)});
            };

            for (int k : {5, 20, 50}) medirPar("k-NN k=" + std::to_string(k), true, k, 0.0);
            for (double r : {500.0, 2000.0, 5000.0})
                medirPar("radio " + num(r, 0) + " m", false, 0, r);
            std::cerr << "   ... C con N=" << N << " listo\n";
        }
        std::cout << "\nEl costo del k-NN crece con k pero MUCHISIMO menos que con N: pasar de\n"
                     "k=5 a k=50 multiplica por diez los vecinos pedidos y no multiplica por diez\n"
                     "las paginas leidas, porque la poda por MINDIST sigue descartando casi todo\n"
                     "el arbol. El radio se comporta como la ventana: cuanto mas grande, mas se\n"
                     "parece a un escaneo.\n";
    }

    // =====================================================================
    //  D. Maldicion de la dimensionalidad
    // =====================================================================
    std::cout << "\n### D. Efecto de la dimension: cuando el escaneo le gana al arbol\n\n";
    {
        const int N_D = 10000;
        const int K_D = 10;
        const int Q_D = 20;

        std::cout << "N = " << N_D << " vectores uniformes en el cubo unitario, k = " << K_D
                  << ", " << Q_D << " consultas.\nLas paginas del arbol y las del escaneo son "
                     "las dos de " << PAGE_SIZE << " B, asi que son comparables.\n\n";

        fila({"D", "fan-out", "nodos (pag.)", "altura", "arbol: pag/consulta",
              "% del arbol", "scan: pag.", "arbol ms", "scan ms", "gana"});
        separador(10);

        for (int D : {2, 4, 8, 16, 32, 64, 128}) {
            std::mt19937 rng(2026);
            std::uniform_real_distribution<float> u(0.0f, 1.0f);
            std::vector<std::vector<float>> datos(static_cast<std::size_t>(N_D));
            for (auto& v : datos) {
                v.resize(static_cast<std::size_t>(D));
                for (float& x : v) x = u(rng);
            }

            const int cap = RTreeND::fanOut(D);
            RTreeND arbol(D, cap);
            arbol.cargar(datos);

            // El escaneo lee TODO el archivo: tantas paginas como ocupen los
            // vectores con su identificador.
            const long long bytes_scan =
                static_cast<long long>(N_D) * (D * 4 + 4);
            const long long pag_scan = (bytes_scan + PAGE_SIZE - 1) / PAGE_SIZE;

            double pag_arbol = 0.0, ms_arbol = 0.0, ms_scan = 0.0;
            for (int q = 0; q < Q_D; ++q) {
                std::vector<float> consulta(static_cast<std::size_t>(D));
                for (float& x : consulta) x = u(rng);

                Cronometro c1;
                long long pags = 0;
                const std::vector<int> r1 = arbol.knn(consulta, K_D, &pags);
                ms_arbol  += c1.parar().ms;
                pag_arbol += static_cast<double>(pags);

                // Fuerza bruta sobre los mismos vectores: es la referencia, y
                // ademas comprueba que el arbol no se esta inventando vecinos.
                Cronometro c2;
                std::vector<std::pair<double, int>> todos;
                todos.reserve(static_cast<std::size_t>(N_D));
                for (int i = 0; i < N_D; ++i) {
                    double s = 0.0;
                    for (int j = 0; j < D; ++j) {
                        const double d = consulta[static_cast<std::size_t>(j)] -
                                         datos[static_cast<std::size_t>(i)][static_cast<std::size_t>(j)];
                        s += d * d;
                    }
                    todos.emplace_back(s, i);
                }
                std::partial_sort(todos.begin(), todos.begin() + K_D, todos.end());
                ms_scan += c2.parar().ms;

                if (q == 0) {
                    std::set<int> exacto;
                    for (int i = 0; i < K_D; ++i) exacto.insert(todos[static_cast<std::size_t>(i)].second);
                    int iguales = 0;
                    for (int id : r1) if (exacto.count(id)) ++iguales;
                    if (iguales != K_D)
                        std::cerr << "   AVISO: D=" << D << " el arbol devolvio "
                                  << iguales << "/" << K_D << " de los exactos\n";
                }
            }
            pag_arbol /= Q_D;
            ms_arbol  /= Q_D;
            ms_scan   /= Q_D;

            const double porc = 100.0 * pag_arbol / static_cast<double>(arbol.numNodos());
            fila({std::to_string(D), std::to_string(cap),
                  std::to_string(arbol.numNodos()), std::to_string(arbol.altura()),
                  num(pag_arbol, 1), num(porc, 1) + " %", std::to_string(pag_scan),
                  num(ms_arbol, 3), num(ms_scan, 3),
                  pag_arbol < static_cast<double>(pag_scan) ? "arbol" : "SCAN"});
        }

        std::cout <<
          "\nDos efectos se suman, y los dos empujan en la misma direccion.\n\n"
          "El primero es aritmetico y se ve en la columna del fan-out: una entrada de\n"
          "un nodo guarda 2*D coordenadas, asi que en D = 128 entran tres entradas por\n"
          "pagina y en D = 2 entran doscientas. El arbol se vuelve alto y flaco, y\n"
          "bajar por el cuesta mas lecturas aunque la poda funcionara perfecto.\n\n"
          "El segundo es geometrico y es el de verdad: al crecer D las cajas\n"
          "envolventes se solapan tanto que el MINDIST de casi todas es menor que la\n"
          "distancia al k-esimo vecino, y el best-first deja de podar. La columna\n"
          "'% del arbol' es la medida directa de eso: cuando se acerca al 100 %, el\n"
          "arbol esta visitandolo todo y ya no es un indice, es un escaneo con\n"
          "indirecciones.\n\n"
          "La ultima columna marca el cruce EN PAGINAS, que es la medida que no\n"
          "depende de la maquina. En milisegundos el cruce llega antes, y conviene\n"
          "decirlo: el escaneo es un bucle apretado y contiguo sobre un arreglo,\n"
          "mientras que bajar por el arbol son saltos de puntero y una cola de\n"
          "prioridad. O sea que el arbol pierde incluso un poco antes de lo que la\n"
          "cuenta de bloques sugiere.\n\n"
          "Pasado ese punto, un indice espacial EXACTO no puede ganarle a leer el\n"
          "archivo de corrido, y es exactamente la razon por la que la Parte 4 no\n"
          "extiende el R-Tree a 384 dimensiones sino que cambia de estrategia:\n"
          "renuncia a la exactitud y usa IVF.\n";
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
