// ============================================================================
//  test_bplus.cpp - B+ Tree en disco (contrastado contra un std::multimap)
// ============================================================================
#include <cstdio>
#include <map>
#include <random>
#include <vector>

#include "db/disk_manager.hpp"
#include "db/bplus_tree.hpp"
#include "test_util.hpp"

using namespace db;

int main() {
    const std::string F = "data/test_bplus.idx";
    resetFile(F);

    const int N = 20000;
    std::mt19937 rng(42);
    std::vector<std::int64_t> keys(N);
    for (int i = 0; i < N; ++i) keys[i] = rng() % 50000;   // con duplicados a proposito

    std::multimap<std::int64_t, RID> ref;   // "verdad de referencia" en memoria

    {
        DiskManager dm(F);
        BufferPool  bp(&dm, 64);
        BPlusTree<std::int64_t> tree(&bp);

        std::cout << "   capacidad hoja=" << BPlusTree<std::int64_t>::leafCapacity()
                  << " interno=" << BPlusTree<std::int64_t>::internalCapacity() << "\n";

        SECTION("insertar 20000 claves (con duplicados) -> fuerza splits");
        for (int i = 0; i < N; ++i) {
            RID r(i / 50, i % 50);
            tree.insert(keys[i], r);
            ref.emplace(keys[i], r);
        }
        CHECK_EQ(tree.size(), static_cast<std::int64_t>(N), "num_keys en la meta pagina");
        CHECK(tree.getHeight() >= 2, "el arbol debio crecer en altura");

        SECTION("busqueda puntual contra el multimap de referencia");
        for (int t = 0; t < 300; ++t) {
            std::int64_t k = keys[rng() % N];
            auto got = tree.search(k);
            CHECK_EQ(got.size(), ref.count(k), "misma cantidad de RIDs para la clave");
        }
        CHECK(tree.search(999999).empty(), "clave inexistente devuelve vacio");

        SECTION("busqueda por rango");
        for (int t = 0; t < 30; ++t) {
            std::int64_t lo = rng() % 45000;
            std::int64_t hi = lo + 500;
            auto got = tree.rangeSearch(lo, hi);
            std::size_t esperado = 0;
            for (auto it = ref.lower_bound(lo); it != ref.end() && it->first <= hi; ++it) ++esperado;
            CHECK_EQ(got.size(), esperado, "cantidad de resultados en el rango");
        }

        SECTION("eliminacion");
        int borrados = 0;
        for (int i = 0; i < N; i += 7) {
            RID r(i / 50, i % 50);
            if (tree.remove(keys[i], r)) {
                ++borrados;
                auto rango = ref.equal_range(keys[i]);
                for (auto it = rango.first; it != rango.second; ++it)
                    if (it->second == r) { ref.erase(it); break; }
            }
        }
        CHECK(borrados > 2000, "se borro una cantidad razonable de claves");
        CHECK_EQ(tree.size(), static_cast<std::int64_t>(ref.size()), "contador tras borrar");

        for (int t = 0; t < 200; ++t) {
            std::int64_t k = keys[rng() % N];
            CHECK_EQ(tree.search(k).size(), ref.count(k), "busqueda coherente tras borrados");
        }
    }

    SECTION("persistencia: reabrir el indice");
    {
        DiskManager dm(F);
        BufferPool  bp(&dm, 64);
        BPlusTree<std::int64_t> tree(&bp);
        CHECK_EQ(tree.size(), static_cast<std::int64_t>(ref.size()), "el tamano sobrevive al cierre");
        for (int t = 0; t < 100; ++t) {
            std::int64_t k = keys[t * 37 % N];
            CHECK_EQ(tree.search(k).size(), ref.count(k), "busqueda tras reabrir");
        }
    }

    SECTION("claves de texto (FixedStr<32>)");
    {
        const std::string F2 = "data/test_bplus_str.idx";
        resetFile(F2);
        DiskManager dm(F2);
        BufferPool  bp(&dm, 64);
        BPlusTree<Key32> tree(&bp);

        auto nombre = [](int i) {
            std::string n = std::to_string(i);
            return std::string("user") + std::string(4 - n.size(), '0') + n;
        };
        for (int i = 0; i < 3000; ++i) tree.insert(Key32(nombre(i)), RID(i, 0));

        CHECK_EQ(tree.size(), static_cast<std::int64_t>(3000), "insercion con claves string");
        auto uno = tree.search(Key32(nombre(500)));
        CHECK_EQ(uno.size(), static_cast<std::size_t>(1), "busqueda exacta de una clave string");
        CHECK_EQ(uno[0].page_id, 500, "el RID recuperado es el correcto");
        CHECK(tree.search(Key32("noexiste")).empty(), "clave string inexistente");

        // Rango lexicografico: user0100 .. user0199 son exactamente 100 claves.
        auto rango = tree.rangeSearch(Key32("user0100"), Key32("user0199"));
        CHECK_EQ(rango.size(), static_cast<std::size_t>(100), "rango lexicografico");
    }

    // ---------------------------------------------------------------------
    //  REGRESION: claves DUPLICADAS (indice no unico).
    //  El descenso por upperBound aterrizaba en la hoja mas a la derecha del
    //  grupo de duplicados, asi que search() y rangeSearch() devolvian menos
    //  de la mitad de las filas. Todos los demas casos usan claves unicas,
    //  que es por lo que el fallo paso desapercibido.
    // ---------------------------------------------------------------------
    SECTION("claves duplicadas (indice no unico)");
    {
        const std::string F3 = "data/test_bplus_dup.idx";
        resetFile(F3);
        DiskManager dm(F3);
        BufferPool  bp(&dm, 64);
        BPlusTree<std::int64_t> tree(&bp);

        // 1000 entradas repartidas entre dos claves: mucho mas que los 255
        // pares que caben en una hoja, asi que fuerza varios splits.
        std::vector<RID> rids;
        for (int i = 0; i < 1000; ++i) {
            RID r(i / 10, i % 10);
            rids.push_back(r);
            tree.insert(i % 2 ? 7 : 3, r);
        }
        CHECK_EQ(tree.search(3).size(), static_cast<std::size_t>(500), "search devuelve TODOS los duplicados de la clave 3");
        CHECK_EQ(tree.search(7).size(), static_cast<std::size_t>(500), "search devuelve TODOS los duplicados de la clave 7");
        CHECK_EQ(tree.rangeSearch(3, 7).size(), static_cast<std::size_t>(1000), "el rango cubre las dos tiradas de duplicados");
        CHECK_EQ(tree.rangeSearchDesc(3, 7).size(), static_cast<std::size_t>(1000), "el rango descendente tambien");
        CHECK(tree.search(5).empty(), "una clave intermedia inexistente sigue vacia");

        // remove() tiene que atravesar las hojas que quedan vacias en medio
        // de una tirada de duplicados, porque nunca se fusionan ni se liberan.
        int fallidos = 0;
        for (int i = 0; i < 1000; ++i)
            if (!tree.remove(i % 2 ? 7 : 3, rids[i])) ++fallidos;
        CHECK_EQ(fallidos, 0, "remove no falla sobre entradas que si existen");
        CHECK_EQ(tree.size(), static_cast<std::int64_t>(0), "el contador queda en cero tras borrarlo todo");
        CHECK(tree.search(3).empty(), "no quedan entradas zombis de la clave 3");
    }

    DONE("test_bplus");
    return 0;
}
