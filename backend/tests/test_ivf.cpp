// ============================================================================
//  test_ivf.cpp - Modulo vectorial: k-means, celdas en disco y recall
//
//  Un indice aproximado no se puede probar como uno exacto: no basta con
//  "devuelve lo correcto", porque por definicion a veces no lo hace. Lo que
//  si tiene que cumplir, y es lo que se comprueba aqui:
//
//    1. Con nprobe = nlist visita todas las celdas, asi que el resultado tiene
//       que coincidir EXACTAMENTE con la fuerza bruta. Un indice que falla
//       esto esta roto, no es aproximado.
//    2. El recall tiene que crecer con nprobe, y nunca bajar.
//    3. Las distancias que devuelve tienen que ser las de verdad, calculadas
//       aparte con la formula.
//    4. Las celdas tienen que contener a TODOS los vectores, sin perder ni
//       duplicar ninguno.
// ============================================================================
#include <algorithm>
#include <cmath>
#include <map>
#include <random>
#include <set>
#include <string>
#include <vector>

#include "db/buffer_pool.hpp"
#include "db/disk_manager.hpp"
#include "db/ivf.hpp"
#include "test_util.hpp"

using namespace db;

static std::string ruta(const std::string& n) { return "data/" + n; }

namespace {

// Vectores agrupados: es como se comportan los embeddings de verdad, donde
// los documentos de un mismo tema caen cerca. Un conjunto uniforme escondería
// el efecto del clustering, que es justo lo que IVF explota.
struct Corpus {
    std::vector<std::vector<float>> v;
    std::vector<std::vector<float>> centros;
    int dim = 0;
};

Corpus generar(int N, int dim, int grupos, unsigned semilla, float dispersion = 1.0f) {
    std::mt19937 rng(semilla);
    std::normal_distribution<float> g(0.0f, 1.0f);
    Corpus c;
    c.dim = dim;
    c.centros.assign(static_cast<std::size_t>(grupos), std::vector<float>(static_cast<std::size_t>(dim)));
    for (auto& cen : c.centros) for (float& x : cen) x = g(rng) * 3.0f;
    c.v.reserve(static_cast<std::size_t>(N));
    for (int i = 0; i < N; ++i) {
        const std::vector<float>& cen = c.centros[rng() % c.centros.size()];
        std::vector<float> x(static_cast<std::size_t>(dim));
        for (int j = 0; j < dim; ++j) x[static_cast<std::size_t>(j)] = cen[static_cast<std::size_t>(j)] + g(rng) * dispersion;
        c.v.push_back(std::move(x));
    }
    return c;
}

IVFIndex::Fuente fuenteDe(const std::vector<std::vector<float>>& v) {
    return [&v](const std::function<void(const RID&, const std::vector<float>&)>& emitir) {
        for (std::size_t i = 0; i < v.size(); ++i)
            emitir(RID(static_cast<page_id_t>(i), 0), v[i]);
    };
}

// Fuerza bruta calculada aqui, sin tocar el indice: es la referencia.
std::vector<std::pair<double, int>> exacto(const std::vector<std::vector<float>>& v,
                                           const std::vector<float>& q, bool coseno, int k) {
    std::vector<std::pair<double, int>> todos;
    for (std::size_t i = 0; i < v.size(); ++i)
        todos.push_back({coseno ? vecdist::coseno(v[i], q) : vecdist::euclidiana(v[i], q),
                         static_cast<int>(i)});
    std::sort(todos.begin(), todos.end(), [](const auto& a, const auto& b) {
        if (a.first != b.first) return a.first < b.first;
        return a.second < b.second;
    });
    if (k >= 0 && static_cast<int>(todos.size()) > k) todos.resize(static_cast<std::size_t>(k));
    return todos;
}

double recall(const std::vector<IVFIndex::Resultado>& aprox,
              const std::vector<std::pair<double, int>>& real) {
    if (real.empty()) return 1.0;
    std::set<int> esperados;
    for (const auto& r : real) esperados.insert(r.second);
    int aciertos = 0;
    for (const auto& r : aprox) if (esperados.count(r.rid.page_id)) ++aciertos;
    return static_cast<double>(aciertos) / static_cast<double>(real.size());
}

}  // namespace

int main() {
    // =======================================================================
    SECTION("distancias: coseno en [0,2] y euclidiana, contra casos conocidos");
    {
        const std::vector<float> a = {1, 0, 0};
        const std::vector<float> b = {0, 1, 0};
        const std::vector<float> c = {-1, 0, 0};
        const std::vector<float> d = {2, 0, 0};     // misma direccion que a, otro modulo
        CHECK(std::fabs(vecdist::coseno(a, a) - 0.0) < 1e-12, "consigo mismo da 0");
        CHECK(std::fabs(vecdist::coseno(a, b) - 1.0) < 1e-12, "perpendicular da 1");
        CHECK(std::fabs(vecdist::coseno(a, c) - 2.0) < 1e-12, "opuesto da 2");
        CHECK(std::fabs(vecdist::coseno(a, d) - 0.0) < 1e-12,
              "al coseno no le importa el modulo, solo la direccion");
        CHECK(std::fabs(vecdist::euclidiana(a, d) - 1.0) < 1e-12,
              "a la euclidiana SI le importa el modulo");
        CHECK(std::fabs(vecdist::euclidiana(a, b) - std::sqrt(2.0)) < 1e-12, "euclidiana conocida");
        // Un vector nulo no tiene direccion: se define a distancia 1 de todo.
        CHECK(std::fabs(vecdist::coseno(a, {0, 0, 0}) - 1.0) < 1e-12, "el vector nulo");
    }

    // =======================================================================
    SECTION("nprobe = nlist tiene que dar EXACTAMENTE lo mismo que la fuerza bruta");
    {
        const Corpus c = generar(2000, 32, 25, 11);
        for (IVFIndex::Metrica met : {IVFIndex::Metrica::COSENO, IVFIndex::Metrica::L2}) {
            const std::string f = ruta("ivf_exacto.idx");
            resetFile(f);
            DiskManager dm(f);
            BufferPool bp(&dm, 64);
            IVFIndex ix(&bp);
            ix.construir(fuenteDe(c.v), c.dim, 0, met);

            std::mt19937 rng(5);
            std::normal_distribution<float> g(0.0f, 1.0f);
            for (int t = 0; t < 15; ++t) {
                std::vector<float> q(static_cast<std::size_t>(c.dim));
                const std::vector<float>& cen = c.centros[rng() % c.centros.size()];
                for (int j = 0; j < c.dim; ++j) q[static_cast<std::size_t>(j)] = cen[static_cast<std::size_t>(j)] + g(rng);

                const bool cos = (met == IVFIndex::Metrica::COSENO);
                const auto real  = exacto(c.v, q, cos, 10);
                const auto todas = ix.buscar(q, 10, ix.listas());
                CHECK_EQ(todas.size(), real.size(), "tamano del top-10 visitando todas las celdas");
                // Los IDENTIFICADORES y su orden tienen que coincidir exacto.
                // Las distancias se comparan con tolerancia de 1e-6 y no de
                // 1e-12 por un motivo concreto: el indice guarda la norma de
                // cada vector en float de 32 bits para no recalcular una raiz
                // por candidato, y la referencia la recalcula en double. Esa
                // diferencia es del orden de 1e-8 y no afecta al orden, pero
                // existe y hay que reconocerla en vez de esconderla.
                for (std::size_t i = 0; i < todas.size(); ++i) {
                    CHECK(std::fabs(todas[i].dist - real[i].first) < 1e-6,
                          "distancia de la posicion " + std::to_string(i));
                    CHECK_EQ(todas[i].rid.page_id, static_cast<page_id_t>(real[i].second),
                             "vector de la posicion " + std::to_string(i));
                }
                // buscarExacto tiene que coincidir con la fuerza bruta de fuera.
                const auto por_indice = ix.buscarExacto(q, 10);
                CHECK_EQ(por_indice.size(), real.size(), "tamano de buscarExacto");
                for (std::size_t i = 0; i < por_indice.size(); ++i) {
                    CHECK(std::fabs(por_indice[i].dist - real[i].first) < 1e-6,
                          "buscarExacto, distancia " + std::to_string(i));
                    CHECK_EQ(por_indice[i].rid.page_id, static_cast<page_id_t>(real[i].second),
                             "buscarExacto, vector " + std::to_string(i));
                }
            }
        }
    }

    // =======================================================================
    SECTION("el recall crece con nprobe y nunca retrocede");
    {
        const Corpus c = generar(4000, 48, 30, 77);
        const std::string f = ruta("ivf_recall.idx");
        resetFile(f);
        DiskManager dm(f);
        BufferPool bp(&dm, 64);
        IVFIndex ix(&bp);
        ix.construir(fuenteDe(c.v), c.dim, 60, IVFIndex::Metrica::COSENO);
        CHECK_EQ(ix.listas(), 60, "se respetan las celdas pedidas");

        std::mt19937 rng(3);
        std::normal_distribution<float> g(0.0f, 1.0f);
        std::vector<std::vector<float>> consultas;
        for (int t = 0; t < 40; ++t) {
            std::vector<float> q(static_cast<std::size_t>(c.dim));
            const std::vector<float>& cen = c.centros[rng() % c.centros.size()];
            for (int j = 0; j < c.dim; ++j) q[static_cast<std::size_t>(j)] = cen[static_cast<std::size_t>(j)] + g(rng);
            consultas.push_back(std::move(q));
        }

        double anterior = -1.0;
        std::vector<double> curva;
        for (int np : {1, 2, 4, 8, 16, 60}) {
            double suma = 0.0;
            for (const std::vector<float>& q : consultas)
                suma += recall(ix.buscar(q, 10, np), exacto(c.v, q, true, 10));
            const double r = suma / static_cast<double>(consultas.size());
            curva.push_back(r);
            CHECK(r >= anterior - 1e-9,
                  "el recall con nprobe=" + std::to_string(np) + " no puede ser peor que con menos celdas");
            anterior = r;
        }
        CHECK(std::fabs(curva.back() - 1.0) < 1e-9, "con todas las celdas el recall es 1");
        CHECK(curva.front() < curva.back(), "con una sola celda se pierde algo");
        CHECK(curva.front() > 0.4, "pero una sola celda ya acierta bastante: el clustering sirve");
        std::cout << "   recall@10 por nprobe {1,2,4,8,16,60} =";
        for (double r : curva) std::cout << " " << r;
        std::cout << "\n";
    }

    // =======================================================================
    SECTION("las celdas reparten TODOS los vectores, sin perder ni duplicar");
    {
        const Corpus c = generar(1500, 16, 12, 909);
        const std::string f = ruta("ivf_reparto.idx");
        resetFile(f);
        DiskManager dm(f);
        BufferPool bp(&dm, 32);
        IVFIndex ix(&bp);
        ix.construir(fuenteDe(c.v), c.dim, 20, IVFIndex::Metrica::L2);

        const std::vector<std::uint32_t> tam = ix.tamanosDeListas();
        CHECK_EQ(tam.size(), static_cast<std::size_t>(20), "hay 20 celdas");
        std::uint64_t suma = 0;
        for (std::uint32_t t : tam) suma += t;
        CHECK_EQ(suma, static_cast<std::uint64_t>(c.v.size()), "las celdas suman todos los vectores");

        // Recorrer el indice entero tiene que devolver cada RID una sola vez.
        std::vector<float> q(static_cast<std::size_t>(c.dim), 0.0f);
        const auto todos = ix.buscarExacto(q, -1);
        CHECK_EQ(todos.size(), c.v.size(), "buscarExacto ve todos los vectores");
        std::set<int> vistos;
        for (const auto& r : todos) vistos.insert(r.rid.page_id);
        CHECK_EQ(vistos.size(), c.v.size(), "y ninguno repetido");

        // Ninguna celda vacia: k-means++ mas el rescate de celdas vacias.
        int vacias = 0;
        for (std::uint32_t t : tam) if (t == 0) ++vacias;
        CHECK_EQ(vacias, 0, "no quedan celdas vacias");
        CHECK(ix.inercia() > 0.0, "la inercia se calcula");
        CHECK(ix.iteraciones() > 0, "k-means hizo al menos una iteracion");
    }

    // =======================================================================
    SECTION("la metrica cambia el resultado, y no da igual cual se elija");
    {
        // Dos vectores en la MISMA direccion pero de modulo muy distinto: el
        // coseno los ve identicos, la euclidiana lejisimos. Es el ejemplo que
        // explica por que la metrica va en el indice y no en la consulta.
        std::vector<std::vector<float>> v = {
            {1.0f, 0.0f, 0.0f},
            {10.0f, 0.0f, 0.0f},
            {0.0f, 1.0f, 0.0f},
        };
        const std::vector<float> q = {2.0f, 0.0f, 0.0f};

        const std::string f1 = ruta("ivf_cos.idx");
        resetFile(f1);
        DiskManager d1(f1); BufferPool b1(&d1, 8); IVFIndex cosi(&b1);
        cosi.construir(fuenteDe(v), 3, 2, IVFIndex::Metrica::COSENO);
        const auto rc = cosi.buscar(q, 2, cosi.listas());
        CHECK_EQ(rc.size(), static_cast<std::size_t>(2), "dos resultados");
        CHECK(std::fabs(rc[0].dist) < 1e-9 && std::fabs(rc[1].dist) < 1e-9,
              "con coseno, los dos alineados estan a distancia 0");

        const std::string f2 = ruta("ivf_l2.idx");
        resetFile(f2);
        DiskManager d2(f2); BufferPool b2(&d2, 8); IVFIndex eucl(&b2);
        eucl.construir(fuenteDe(v), 3, 2, IVFIndex::Metrica::L2);
        const auto rl = eucl.buscar(q, 3, eucl.listas());
        CHECK_EQ(rl[0].rid.page_id, 0, "con L2 gana el de modulo parecido");
        CHECK(std::fabs(rl[0].dist - 1.0) < 1e-9, "a distancia 1");
        CHECK_EQ(rl[2].rid.page_id, 1, "y el de modulo 10 queda el ultimo");
    }

    // =======================================================================
    SECTION("persistencia: reabrir el archivo y obtener lo mismo");
    {
        const Corpus c = generar(800, 24, 10, 404);
        const std::string f = ruta("ivf_persist.idx");
        std::vector<float> q(static_cast<std::size_t>(c.dim));
        for (int j = 0; j < c.dim; ++j) q[static_cast<std::size_t>(j)] = c.centros[0][static_cast<std::size_t>(j)];

        std::vector<IVFIndex::Resultado> antes;
        int nl = 0;
        double inercia = 0.0;
        {
            resetFile(f);
            DiskManager dm(f);
            BufferPool bp(&dm, 16);
            IVFIndex ix(&bp);
            ix.construir(fuenteDe(c.v), c.dim, 15, IVFIndex::Metrica::COSENO);
            antes   = ix.buscar(q, 10, 5);
            nl      = ix.listas();
            inercia = ix.inercia();
        }
        {
            DiskManager dm(f);
            BufferPool bp(&dm, 16);
            IVFIndex ix(&bp);                 // se abre leyendo la META
            CHECK_EQ(ix.size(), static_cast<std::uint32_t>(c.v.size()), "num_vectores tras reabrir");
            CHECK_EQ(ix.dimension(), c.dim, "dimension tras reabrir");
            CHECK_EQ(ix.listas(), nl, "celdas tras reabrir");
            CHECK(ix.metrica() == IVFIndex::Metrica::COSENO, "metrica tras reabrir");
            CHECK(std::fabs(ix.inercia() - inercia) < 1e-9, "inercia tras reabrir");
            const auto despues = ix.buscar(q, 10, 5);
            CHECK_EQ(despues.size(), antes.size(), "mismo numero de resultados");
            for (std::size_t i = 0; i < antes.size(); ++i) {
                CHECK_EQ(despues[i].rid.page_id, antes[i].rid.page_id, "mismo orden");
                CHECK(std::fabs(despues[i].dist - antes[i].dist) < 1e-12, "misma distancia");
            }
        }
    }

    // =======================================================================
    SECTION("la construccion es determinista con la misma semilla");
    {
        const Corpus c = generar(1000, 20, 8, 1234);
        std::vector<double> inercias;
        std::vector<std::vector<std::uint32_t>> repartos;
        for (int intento = 0; intento < 2; ++intento) {
            const std::string f = ruta("ivf_det" + std::to_string(intento) + ".idx");
            resetFile(f);
            DiskManager dm(f);
            BufferPool bp(&dm, 16);
            IVFIndex ix(&bp);
            ix.construir(fuenteDe(c.v), c.dim, 12, IVFIndex::Metrica::COSENO, 2026);
            inercias.push_back(ix.inercia());
            repartos.push_back(ix.tamanosDeListas());
        }
        CHECK(std::fabs(inercias[0] - inercias[1]) < 1e-12,
              "dos construcciones con la misma semilla dan la misma inercia");
        CHECK(repartos[0] == repartos[1], "y el mismo reparto por celdas");
    }

    // =======================================================================
    SECTION("casos de borde");
    {
        const std::string f = ruta("ivf_borde.idx");
        resetFile(f);
        DiskManager dm(f);
        BufferPool bp(&dm, 8);
        IVFIndex ix(&bp);

        // Indice recien creado.
        CHECK_EQ(ix.size(), static_cast<std::uint32_t>(0), "indice vacio");
        CHECK(ix.buscar({1.0f, 2.0f}, 5, 1).empty(), "consultar un indice vacio no revienta");

        // Corpus vacio.
        std::vector<std::vector<float>> nada;
        ix.construir(fuenteDe(nada), 8, 4, IVFIndex::Metrica::COSENO);
        CHECK_EQ(ix.size(), static_cast<std::uint32_t>(0), "corpus vacio");
        CHECK(ix.buscar(std::vector<float>(8, 1.0f), 5, 1).empty(), "sin resultados");

        // Un solo vector, y mas celdas pedidas que vectores.
        std::vector<std::vector<float>> uno = {{1.0f, 0.0f, 0.0f, 0.0f}};
        ix.construir(fuenteDe(uno), 4, 50, IVFIndex::Metrica::COSENO);
        CHECK_EQ(ix.size(), static_cast<std::uint32_t>(1), "un vector");
        CHECK_EQ(ix.listas(), 1, "las celdas se recortan al numero de vectores");
        CHECK_EQ(ix.buscar({1.0f, 0.0f, 0.0f, 0.0f}, 5, 1).size(), static_cast<std::size_t>(1),
                 "k mayor que el corpus devuelve el corpus");

        // Vectores todos iguales: k-means no tiene nada que separar.
        std::vector<std::vector<float>> iguales(50, std::vector<float>{1.0f, 1.0f, 1.0f, 1.0f});
        ix.construir(fuenteDe(iguales), 4, 5, IVFIndex::Metrica::L2);
        CHECK_EQ(ix.size(), static_cast<std::uint32_t>(50), "todos entran igual");
        CHECK_EQ(ix.buscar({1.0f, 1.0f, 1.0f, 1.0f}, 10, ix.listas()).size(),
                 static_cast<std::size_t>(10), "y se pueden consultar");

        // Una consulta con la dimension equivocada tiene que quejarse.
        bool lanzo = false;
        try { ix.buscar({1.0f, 2.0f}, 3, 1); } catch (const DBException&) { lanzo = true; }
        CHECK(lanzo, "una consulta de dimension distinta se rechaza");

        // Y un vector con la dimension equivocada en la construccion tambien.
        std::vector<std::vector<float>> mezcla = {{1.0f, 2.0f, 3.0f}, {1.0f, 2.0f}};
        lanzo = false;
        try { ix.construir(fuenteDe(mezcla), 3, 2, IVFIndex::Metrica::L2); }
        catch (const DBException&) { lanzo = true; }
        CHECK(lanzo, "un vector de otra dimension se rechaza al construir");
    }

    // =======================================================================
    SECTION("un .idx de otro tipo no se interpreta como indice IVF");
    {
        const std::string f = ruta("ivf_ajeno.idx");
        resetFile(f);
        {
            DiskManager dm(f);
            BufferPool bp(&dm, 4);
            page_id_t pid;
            char* p = bp.newPage(&pid);
            writeAt<std::int32_t>(p, 16, 0x494E5658);   // "INVX": el indice invertido
            bp.unpinPage(pid, true);
            bp.flushAll();
        }
        DiskManager dm(f);
        BufferPool bp(&dm, 4);
        bool lanzo = false;
        try { IVFIndex ix(&bp); (void)ix; } catch (const DBException&) { lanzo = true; }
        CHECK(lanzo, "abrir un indice invertido como IVF tiene que fallar");
    }

    // =======================================================================
    SECTION("el reparto en celdas es lo que hace barata la consulta");
    {
        // Con 10 000 vectores y 100 celdas, visitar 5 celdas deberia mirar del
        // orden del 5 % del corpus. Si el k-means dejara una celda gigante,
        // esa promesa se rompe: por eso se mide el desequilibrio.
        const Corpus c = generar(10000, 32, 50, 2026);
        const std::string f = ruta("ivf_reparto2.idx");
        resetFile(f);
        DiskManager dm(f);
        BufferPool bp(&dm, 64);
        IVFIndex ix(&bp);
        ix.construir(fuenteDe(c.v), c.dim, 100, IVFIndex::Metrica::COSENO);

        const std::vector<std::uint32_t> tam = ix.tamanosDeListas();
        const double media = static_cast<double>(c.v.size()) / static_cast<double>(tam.size());
        std::uint32_t mayor = 0;
        for (std::uint32_t t : tam) mayor = std::max(mayor, t);
        CHECK(mayor < media * 12.0,
              "ninguna celda se lleva una porcion absurda del corpus");
        std::cout << "   10000 vectores en " << tam.size() << " celdas: media " << media
                  << ", mayor " << mayor << ", paginas del indice " << ix.numPages() << "\n";

        // Y el recall con pocas celdas tiene que seguir siendo alto.
        std::mt19937 rng(8);
        std::normal_distribution<float> g(0.0f, 1.0f);
        double suma = 0.0;
        const int Q = 20;
        for (int t = 0; t < Q; ++t) {
            std::vector<float> q(static_cast<std::size_t>(c.dim));
            const std::vector<float>& cen = c.centros[rng() % c.centros.size()];
            for (int j = 0; j < c.dim; ++j) q[static_cast<std::size_t>(j)] = cen[static_cast<std::size_t>(j)] + g(rng);
            suma += recall(ix.buscar(q, 10, 10), exacto(c.v, q, true, 10));
        }
        const double r = suma / Q;
        CHECK(r > 0.85, "con 10 de 100 celdas el recall@10 supera el 85 %");
        std::cout << "   recall@10 con nprobe=10 de 100 celdas: " << r << "\n";
    }

    DONE("test_ivf");
    return 0;
}
