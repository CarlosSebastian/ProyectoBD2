// ============================================================================
//  test_rtree.cpp - R-Tree 2D en disco, contrastado contra FUERZA BRUTA
//
//  La referencia es el calculo exhaustivo sobre el mismo conjunto de puntos:
//  si el arbol se salta una rama que debio visitar, el resultado difiere y el
//  test falla. Es el equivalente de contrastar el B+ contra un std::multimap.
// ============================================================================
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <iostream>
#include <random>
#include <vector>

#include "db/disk_manager.hpp"
#include "db/rtree.hpp"
#include "test_util.hpp"

using namespace db;

namespace {

struct Punto {
    double x, y;
    RID    rid;
};

// --- referencia por fuerza bruta -------------------------------------------
std::vector<RID> ventanaFuerzaBruta(const std::vector<Punto>& ps, const MBR& w,
                                    const std::vector<char>& vivo) {
    std::vector<RID> out;
    for (std::size_t i = 0; i < ps.size(); ++i) {
        if (!vivo[i]) continue;
        if (MBR::point(ps[i].x, ps[i].y).interseca(w)) out.push_back(ps[i].rid);
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::vector<double> knnFuerzaBruta(const std::vector<Punto>& ps, double px, double py, int k,
                                   const std::vector<char>& vivo) {
    std::vector<double> d;
    for (std::size_t i = 0; i < ps.size(); ++i) {
        if (!vivo[i]) continue;
        double dx = ps[i].x - px, dy = ps[i].y - py;
        d.push_back(std::sqrt(dx * dx + dy * dy));
    }
    std::sort(d.begin(), d.end());
    if (static_cast<int>(d.size()) > k) d.resize(static_cast<std::size_t>(k));
    return d;
}

}  // namespace

int main() {
    const std::string F = "data/test_rtree.idx";
    resetFile(F);

    const int N = 20000;
    std::mt19937 rng(1234);
    std::uniform_real_distribution<double> coord(-180.0, 180.0);

    std::vector<Punto> ps(N);
    for (int i = 0; i < N; ++i) {
        ps[static_cast<std::size_t>(i)] = Punto{coord(rng), coord(rng), RID(i / 50, i % 50)};
    }
    std::vector<char> vivo(static_cast<std::size_t>(N), 1);

    {
        DiskManager dm(F);
        BufferPool  bp(&dm, 64);
        RTree       rt(&bp);

        std::cout << "   capacidad hoja=" << RTree::leafCapacity()
                  << " interno=" << RTree::internalCapacity()
                  << " (minimos " << RTree::LEAF_MIN << "/" << RTree::INT_MIN << ")\n";

        SECTION("insertar 20000 puntos -> fuerza splits y crecimiento de altura");
        for (const Punto& p : ps) rt.insert(MBR::point(p.x, p.y), p.rid);
        CHECK_EQ(rt.size(), static_cast<std::int64_t>(N), "num_entradas en la meta pagina");
        CHECK(rt.getHeight() >= 3, "con 20k puntos y fan-out ~100 el arbol debe tener 3+ niveles");

        SECTION("consulta de ventana contra fuerza bruta (50 rectangulos al azar)");
        for (int t = 0; t < 50; ++t) {
            double x0 = coord(rng), y0 = coord(rng);
            double x1 = x0 + std::fabs(coord(rng)) * 0.15;
            double y1 = y0 + std::fabs(coord(rng)) * 0.15;
            MBR w(x0, y0, x1, y1);

            std::vector<RID> got = rt.search(w);
            std::sort(got.begin(), got.end());
            std::vector<RID> esp = ventanaFuerzaBruta(ps, w, vivo);
            CHECK_EQ(got.size(), esp.size(), "cantidad de puntos en la ventana");
            CHECK(got == esp, "los RID de la ventana no coinciden con la fuerza bruta");
        }

        SECTION("ventana que cubre todo el plano devuelve absolutamente todo");
        CHECK_EQ(rt.search(MBR(-1000, -1000, 1000, 1000)).size(),
                 static_cast<std::size_t>(N), "ventana total");

        SECTION("ventana vacia fuera del dominio no devuelve nada");
        CHECK_EQ(rt.search(MBR(5000, 5000, 6000, 6000)).size(),
                 static_cast<std::size_t>(0), "ventana sin puntos");

        SECTION("KNN contra fuerza bruta (30 consultas, k=1,5,20)");
        const int ks[3] = {1, 5, 20};
        for (int t = 0; t < 30; ++t) {
            double px = coord(rng), py = coord(rng);
            for (int ki = 0; ki < 3; ++ki) {
                int k = ks[ki];
                std::vector<std::pair<double, RID>> got = rt.knn(px, py, k);
                std::vector<double> esp = knnFuerzaBruta(ps, px, py, k, vivo);
                CHECK_EQ(got.size(), esp.size(), "cantidad de vecinos devueltos");
                for (std::size_t i = 0; i < got.size(); ++i) {
                    CHECK(std::fabs(got[i].first - esp[i]) < 1e-9,
                          "la distancia del vecino " + std::to_string(i) +
                          " no coincide con la fuerza bruta");
                    if (i) CHECK(got[i - 1].first <= got[i].first,
                                 "el KNN debe venir ordenado por distancia creciente");
                }
            }
        }

        SECTION("KNN con k mayor que el total devuelve todo el conjunto");
        {
            DiskManager dm2("data/test_rtree_chico.idx");
            resetFile("data/test_rtree_chico.idx");
        }
        {
            resetFile("data/test_rtree_chico.idx");
            DiskManager dm2("data/test_rtree_chico.idx");
            BufferPool  bp2(&dm2, 16);
            RTree       chico(&bp2);
            for (int i = 0; i < 7; ++i) chico.insert(MBR::point(i, i), RID(0, i));
            CHECK_EQ(chico.knn(0, 0, 99).size(), static_cast<std::size_t>(7), "k > n");
            CHECK_EQ(chico.knn(0, 0, 0).size(),  static_cast<std::size_t>(0), "k = 0");
        }

        SECTION("puntos duplicados en la misma coordenada");
        {
            resetFile("data/test_rtree_dup.idx");
            DiskManager dm3("data/test_rtree_dup.idx");
            BufferPool  bp3(&dm3, 16);
            RTree       dup(&bp3);
            for (int i = 0; i < 500; ++i) dup.insert(MBR::point(10.0, 20.0), RID(1, i));
            CHECK_EQ(dup.search(MBR(9, 19, 11, 21)).size(),
                     static_cast<std::size_t>(500), "500 puntos identicos");
            CHECK_EQ(dup.knn(10.0, 20.0, 500).size(),
                     static_cast<std::size_t>(500), "KNN sobre coordenadas repetidas");
        }

        SECTION("borrado coherente: la ventana deja de verlos");
        for (int i = 0; i < N; i += 7) {
            const Punto& p = ps[static_cast<std::size_t>(i)];
            CHECK(rt.remove(MBR::point(p.x, p.y), p.rid), "remove de una entrada existente");
            vivo[static_cast<std::size_t>(i)] = 0;
        }
        CHECK(!rt.remove(MBR::point(999, 999), RID(123, 4)), "remove de algo inexistente");
        for (int t = 0; t < 20; ++t) {
            double x0 = coord(rng), y0 = coord(rng);
            MBR w(x0, y0, x0 + 40.0, y0 + 40.0);
            std::vector<RID> got = rt.search(w);
            std::sort(got.begin(), got.end());
            CHECK(got == ventanaFuerzaBruta(ps, w, vivo), "ventana tras los borrados");
        }
        CHECK_EQ(rt.knn(0, 0, 10).size(), static_cast<std::size_t>(10), "KNN sigue vivo tras borrar");

        bp.flushAll();
    }

    SECTION("persistencia: reabrir el archivo y volver a consultar");
    {
        DiskManager dm(F);
        BufferPool  bp(&dm, 64);
        RTree       rt(&bp);
        long long vivos = 0;
        for (char v : vivo) vivos += v;
        CHECK_EQ(rt.size(), vivos, "el contador sobrevive al cierre");
        MBR w(-50, -50, 50, 50);
        std::vector<RID> got = rt.search(w);
        std::sort(got.begin(), got.end());
        CHECK(got == ventanaFuerzaBruta(ps, w, vivo), "ventana despues de reabrir");
    }

    SECTION("un .idx de otro tipo no se interpreta como R-Tree");
    {
        const std::string G = "data/test_rtree_ajeno.idx";
        resetFile(G);
        {
            DiskManager dm(G);
            BufferPool  bp(&dm, 8);
            page_id_t   pid;
            char* p = bp.newPage(&pid);
            std::memset(p, 0, PAGE_SIZE);
            writeAt<std::int32_t>(p, RTree::OFF_MAGIC, 0x42504C53);   // "BPLS"
            bp.unpinPage(pid, true);
            bp.flushAll();
        }
        DiskManager dm(G);
        BufferPool  bp(&dm, 8);
        bool lanzo = false;
        try { RTree rt(&bp); } catch (const DBException&) { lanzo = true; }
        CHECK(lanzo, "abrir un indice de otro tipo debe lanzar, no leer basura");
    }

    DONE("test_rtree");
    return 0;
}
