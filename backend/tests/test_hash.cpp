// ============================================================================
//  test_hash.cpp - Hash Extensible en disco
// ============================================================================
#include <cstdio>
#include <map>
#include <random>
#include <vector>

#include "db/disk_manager.hpp"
#include "db/extendible_hash.hpp"
#include "test_util.hpp"

using namespace db;

int main() {
    const std::string F = "data/test_hash.idx";
    resetFile(F);

    const int N = 20000;
    std::mt19937 rng(7);
    std::vector<std::int64_t> keys(N);
    for (int i = 0; i < N; ++i) keys[i] = rng() % 50000;

    std::multimap<std::int64_t, RID> ref;

    {
        DiskManager dm(F);
        BufferPool  bp(&dm, 64);
        ExtendibleHash<std::int64_t> h(&bp);

        std::cout << "   capacidad bucket=" << ExtendibleHash<std::int64_t>::bucketCapacity() << "\n";
        CHECK_EQ(h.globalDepth(), 1, "profundidad global inicial");
        CHECK_EQ(h.numBuckets(), 2, "buckets iniciales");

        SECTION("insertar 20000 claves -> fuerza splits y duplicaciones");
        for (int i = 0; i < N; ++i) {
            RID r(i / 50, i % 50);
            h.insert(keys[i], r);
            ref.emplace(keys[i], r);
        }
        CHECK(h.globalDepth() > 1, "el directorio debio duplicarse");
        CHECK(h.numBuckets() > 2, "debieron crearse buckets nuevos");
        std::cout << "   gd=" << h.globalDepth() << " buckets=" << h.numBuckets()
                  << " overflow=" << h.overflowPages() << " paginas=" << h.numPages() << "\n";

        SECTION("busqueda puntual contra el multimap de referencia");
        for (int t = 0; t < 500; ++t) {
            std::int64_t k = keys[rng() % N];
            CHECK_EQ(h.search(k).size(), ref.count(k), "misma cantidad de RIDs");
        }
        CHECK(h.search(999999).empty(), "clave inexistente devuelve vacio");

        SECTION("eliminacion");
        int borrados = 0;
        for (int i = 0; i < N; i += 5) {
            RID r(i / 50, i % 50);
            if (h.remove(keys[i], r)) {
                ++borrados;
                auto rg = ref.equal_range(keys[i]);
                for (auto it = rg.first; it != rg.second; ++it)
                    if (it->second == r) { ref.erase(it); break; }
            }
        }
        CHECK(borrados > 3000, "se borro una cantidad razonable");
        for (int t = 0; t < 300; ++t) {
            std::int64_t k = keys[rng() % N];
            CHECK_EQ(h.search(k).size(), ref.count(k), "busqueda coherente tras borrados");
        }
    }

    SECTION("persistencia: reabrir el indice");
    {
        DiskManager dm(F);
        BufferPool  bp(&dm, 64);
        ExtendibleHash<std::int64_t> h(&bp);
        for (int t = 0; t < 200; ++t) {
            std::int64_t k = keys[t * 53 % N];
            CHECK_EQ(h.search(k).size(), ref.count(k), "busqueda tras reabrir");
        }
    }

    SECTION("claves de texto (FixedStr<32>)");
    {
        const std::string F2 = "data/test_hash_str.idx";
        resetFile(F2);
        DiskManager dm(F2);
        BufferPool  bp(&dm, 64);
        ExtendibleHash<Key32> h(&bp);
        for (int i = 0; i < 5000; ++i) h.insert(Key32("dni-" + std::to_string(i)), RID(i, 0));
        auto got = h.search(Key32("dni-4321"));
        CHECK_EQ(got.size(), static_cast<std::size_t>(1), "busqueda exacta con clave string");
        CHECK_EQ(got[0].page_id, 4321, "el RID recuperado es el correcto");
        CHECK(h.search(Key32("dni-99999")).empty(), "clave string inexistente");
    }

    DONE("test_hash");
    return 0;
}
