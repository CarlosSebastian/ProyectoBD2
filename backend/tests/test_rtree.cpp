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

std::vector<double> knnGeoFuerzaBruta(const std::vector<Punto>& ps, double lon, double lat, int k) {
    std::vector<double> d;
    for (const Punto& p : ps) d.push_back(geo::haversine(lon, lat, p.x, p.y));
    std::sort(d.begin(), d.end());
    if (static_cast<int>(d.size()) > k) d.resize(static_cast<std::size_t>(k));
    return d;
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

    // ------------------------------------------------------------------
    //  Distancia geografica (ST_DISTANCE): metros reales sobre la esfera.
    // ------------------------------------------------------------------
    SECTION("haversine contra distancias conocidas");
    {
        // Plaza de Armas de Lima -> UTEC Barranco, ~9.9 km en linea recta.
        const double d1 = geo::haversine(-77.0300, -12.0460, -77.0220, -12.1350);
        CHECK(d1 > 9000.0 && d1 < 11000.0, "Lima centro a Barranco ronda los 10 km");
        // Lima -> Cusco, ~570 km.
        const double d2 = geo::haversine(-77.0300, -12.0460, -71.9780, -13.5320);
        CHECK(d2 > 550000.0 && d2 < 590000.0, "Lima a Cusco ronda los 570 km");
        CHECK_EQ(geo::haversine(10.0, 20.0, 10.0, 20.0), 0.0, "distancia de un punto a si mismo");
        // Un grado de longitud mide menos cuanto mas lejos del ecuador.
        const double ecuador = geo::haversine(0.0, 0.0, 1.0, 0.0);
        const double lat60   = geo::haversine(0.0, 60.0, 1.0, 60.0);
        CHECK(lat60 < ecuador * 0.55, "a 60 grados un grado de longitud mide la mitad");
    }

    SECTION("mindistGeo NUNCA se pasa del minimo real (es lo que hace exacto al KNN)");
    {
        // Si la cota superara la distancia real, el best-first podria podar un
        // nodo que si contenia un vecino mas cercano y devolver mal sin avisar.
        std::mt19937 r2(99);
        std::uniform_real_distribution<double> lonU(-180, 180), latU(-85, 85);
        int violaciones = 0;
        for (int t = 0; t < 400; ++t) {
            MBR caja(lonU(r2), latU(r2), lonU(r2), latU(r2));
            const double qlon = lonU(r2), qlat = latU(r2);
            const double cota = caja.mindistGeo(qlon, qlat);
            double real = 1e18;
            for (int i = 0; i <= 25; ++i)
                for (int j = 0; j <= 25; ++j)
                    real = std::min(real, geo::haversine(
                        qlon, qlat,
                        caja.minx + (caja.maxx - caja.minx) * i / 25.0,
                        caja.miny + (caja.maxy - caja.miny) * j / 25.0));
            if (cota > real + 1e-6) ++violaciones;
        }
        CHECK_EQ(violaciones, 0, "la cota inferior se respeta en todas las cajas probadas");
    }

    SECTION("KNN geografico contra fuerza bruta");
    {
        resetFile("data/test_rtree_geo.idx");
        DiskManager dmg("data/test_rtree_geo.idx");
        BufferPool  bpg(&dmg, 64);
        RTree       rg(&bpg);

        // Puntos sobre coordenadas terrestres reales (lon, lat).
        std::mt19937 r3(2026);
        std::uniform_real_distribution<double> lonU(-180.0, 180.0), latU(-80.0, 80.0);
        std::vector<Punto> geos(3000);
        for (std::size_t i = 0; i < geos.size(); ++i) {
            geos[i] = Punto{lonU(r3), latU(r3), RID(static_cast<int>(i) / 50,
                                                    static_cast<int>(i) % 50)};
            rg.insert(MBR::point(geos[i].x, geos[i].y), geos[i].rid);
        }

        const int ks[3] = {1, 5, 25};
        for (int t = 0; t < 25; ++t) {
            const double qlon = lonU(r3), qlat = latU(r3);
            for (int ki = 0; ki < 3; ++ki) {
                std::vector<std::pair<double, RID>> got = rg.knnGeo(qlon, qlat, ks[ki]);
                std::vector<double> esp = knnGeoFuerzaBruta(geos, qlon, qlat, ks[ki]);
                CHECK_EQ(got.size(), esp.size(), "cantidad de vecinos geograficos");
                for (std::size_t i = 0; i < got.size(); ++i) {
                    CHECK(std::fabs(got[i].first - esp[i]) < 1e-6,
                          "la distancia geografica del vecino " + std::to_string(i) +
                          " no coincide con la fuerza bruta");
                    if (i) CHECK(got[i - 1].first <= got[i].first,
                                 "el KNN geografico viene ordenado");
                }
            }
        }

        SECTION("busqueda por radio en metros contra fuerza bruta");
        const double radios[3] = {200000.0, 1000000.0, 5000000.0};
        for (int t = 0; t < 15; ++t) {
            const double qlon = lonU(r3), qlat = latU(r3);
            for (int ri = 0; ri < 3; ++ri) {
                std::vector<RID> got = rg.searchRadio(qlon, qlat, radios[ri]);
                std::sort(got.begin(), got.end());
                std::vector<RID> esp;
                for (const Punto& p : geos)
                    if (geo::haversine(qlon, qlat, p.x, p.y) <= radios[ri]) esp.push_back(p.rid);
                std::sort(esp.begin(), esp.end());
                CHECK_EQ(got.size(), esp.size(), "cantidad de puntos dentro del radio");
                CHECK(got == esp, "los RID dentro del radio no coinciden con la fuerza bruta");
            }
        }
    }

    SECTION("a 60 grados de latitud la euclidiana en grados da el orden EQUIVOCADO");
    {
        // Dos puntos desde (0, 60): uno a un grado al este, otro a 0.9 al norte.
        // En grados el del norte parece mas cerca (0.9 < 1.0), pero en metros
        // esta al doble de distancia, porque un grado de longitud a esa latitud
        // mide la mitad que uno de latitud.
        resetFile("data/test_rtree_n60.idx");
        DiskManager dmn("data/test_rtree_n60.idx");
        BufferPool  bpn(&dmn, 16);
        RTree       rn(&bpn);
        const RID este(0, 1), norte(0, 2);
        rn.insert(MBR::point(1.0, 60.0), este);
        rn.insert(MBR::point(0.0, 60.9), norte);

        std::vector<std::pair<double, RID>> plano = rn.knn(0.0, 60.0, 1);
        std::vector<std::pair<double, RID>> esfera = rn.knnGeo(0.0, 60.0, 1);
        CHECK_EQ(plano.size(), static_cast<std::size_t>(1), "el KNN plano devuelve uno");
        CHECK_EQ(esfera.size(), static_cast<std::size_t>(1), "el KNN geografico devuelve uno");
        CHECK(plano[0].second == norte, "en grados gana el del norte (0.9 < 1.0)");
        CHECK(esfera[0].second == este, "en metros gana el del este: 56 km contra 100 km");
        CHECK(esfera[0].first < 60000.0, "y esta a menos de 60 km");
    }

    // ------------------------------------------------------------------
    //  Geometria de poligonos: filtrado por MBR + refinamiento exacto.
    // ------------------------------------------------------------------
    SECTION("punto en poligono por lanzamiento de rayo");
    {
        const std::vector<double> cuadrado{0,0, 10,0, 10,10, 0,10};
        CHECK(poly::contienePunto(cuadrado, 5, 5),      "centro dentro");
        CHECK(!poly::contienePunto(cuadrado, 15, 5),    "a la derecha fuera");
        CHECK(!poly::contienePunto(cuadrado, -1, 5),    "a la izquierda fuera");
        CHECK(!poly::contienePunto(cuadrado, 5, 20),    "arriba fuera");

        // Un triangulo: su caja envolvente contiene esquinas que el triangulo no.
        const std::vector<double> triangulo{20,0, 30,0, 25,10};
        CHECK(poly::contienePunto(triangulo, 25, 2),    "dentro del triangulo");
        CHECK(!poly::contienePunto(triangulo, 21, 9),   "en la caja pero fuera del triangulo");
        CHECK(poly::envolvente(triangulo).interseca(MBR::point(21, 9)),
              "y sin embargo la caja SI lo contiene: por eso hace falta refinar");

        // Una L: el hueco del angulo esta dentro de la caja pero fuera de la figura.
        const std::vector<double> ele{0,0, 10,0, 10,3, 3,3, 3,10, 0,10};
        CHECK(poly::contienePunto(ele, 1, 1),   "pata de la L");
        CHECK(poly::contienePunto(ele, 8, 1),   "pie de la L");
        CHECK(!poly::contienePunto(ele, 8, 8),  "el hueco de la L no esta dentro");
    }

    SECTION("interseccion poligono-rectangulo: los tres casos");
    {
        const std::vector<double> cuadrado{0,0, 10,0, 10,10, 0,10};
        CHECK(poly::intersecaRect(cuadrado, MBR(5, 5, 15, 15)),  "los bordes se cruzan");
        CHECK(poly::intersecaRect(cuadrado, MBR(2, 2, 3, 3)),    "el rectangulo esta dentro");
        CHECK(poly::intersecaRect(cuadrado, MBR(-5, -5, 20, 20)),"el poligono esta dentro");
        CHECK(!poly::intersecaRect(cuadrado, MBR(20, 20, 30, 30)), "disjuntos");

        // Diagonal fina: su caja ocupa todo el cuadrante, la figura casi nada.
        const std::vector<double> diag{0,0, 100,100, 99,100, 0,1};
        CHECK(poly::envolvente(diag).interseca(MBR(90, 0, 95, 5)),
              "la CAJA de la diagonal si toca esa ventana");
        CHECK(!poly::intersecaRect(diag, MBR(90, 0, 95, 5)),
              "pero la GEOMETRIA no: es el falso positivo que el refinamiento mata");
        CHECK(poly::intersecaRect(diag, MBR(90, 88, 95, 95)),
              "y donde si pasa la diagonal, se detecta");
    }

    SECTION("R-Tree sobre poligonos: filtrar + refinar contra fuerza bruta");
    {
        resetFile("data/test_rtree_poly.idx");
        DiskManager dmp("data/test_rtree_poly.idx");
        BufferPool  bpp(&dmp, 64);
        RTree       rp(&bpp);

        // 600 triangulos repartidos por el plano. El triangulo es la figura que
        // mas castiga al MBR: ocupa la mitad de su caja envolvente.
        std::mt19937 r4(555);
        std::uniform_real_distribution<double> u(0.0, 500.0);
        std::vector<std::vector<double>> figuras;
        std::vector<RID> rids;
        for (int i = 0; i < 600; ++i) {
            const double x = u(r4), y = u(r4), lado = 5.0 + u(r4) / 20.0;
            figuras.push_back({x, y, x + lado, y, x + lado / 2, y + lado});
            rids.push_back(RID(i / 50, i % 50));
            rp.insert(poly::envolvente(figuras.back()), rids.back());
        }

        int total_candidatos = 0, total_exactos = 0;
        for (int t = 0; t < 40; ++t) {
            const double x0 = u(r4), y0 = u(r4);
            const MBR ventana(x0, y0, x0 + 30.0, y0 + 30.0);

            // Paso 1: el indice filtra por caja.
            std::vector<RID> cand = rp.search(ventana);
            // Paso 2: refinamiento con la geometria real.
            std::vector<RID> got;
            for (const RID& rid : cand) {
                std::size_t k = static_cast<std::size_t>(rid.page_id) * 50 +
                                static_cast<std::size_t>(rid.slot);
                if (poly::intersecaRect(figuras[k], ventana)) got.push_back(rid);
            }
            std::sort(got.begin(), got.end());

            // Referencia: comprobar las 600 figuras una por una.
            std::vector<RID> esp;
            for (std::size_t k = 0; k < figuras.size(); ++k)
                if (poly::intersecaRect(figuras[k], ventana)) esp.push_back(rids[k]);
            std::sort(esp.begin(), esp.end());

            CHECK(got == esp, "filtrar+refinar da lo mismo que revisar las 600 figuras");
            total_candidatos += static_cast<int>(cand.size());
            total_exactos    += static_cast<int>(got.size());
        }
        CHECK(total_candidatos >= total_exactos,
              "el indice nunca puede devolver MENOS que la respuesta exacta");
        CHECK(total_candidatos > total_exactos,
              "y con triangulos siempre sobran candidatos: por eso el refinamiento existe");
        std::cout << "   candidatos del indice=" << total_candidatos
                  << "  exactos=" << total_exactos
                  << "  descartados por el refinamiento="
                  << (total_candidatos - total_exactos) << "\n";
    }

    DONE("test_rtree");
    return 0;
}
