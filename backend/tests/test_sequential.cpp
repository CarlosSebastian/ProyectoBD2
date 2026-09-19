// ============================================================================
//  test_sequential.cpp - Sequential File: area principal ordenada, overflow
//  encadenado por clave y reorganizacion.
//
//  Como en los otros tests de estructuras, la verdad de referencia es una
//  estructura de la STL (std::multimap): no se afirma que el archivo
//  "funciona", se compara cada consulta contra la respuesta correcta.
// ============================================================================
#include <algorithm>
#include <map>
#include <random>
#include <string>
#include <vector>

#include "db/page.hpp"
#include "db/record.hpp"
#include "db/sequential_file.hpp"
#include "test_util.hpp"

using namespace db;

static Schema esquema() {
    return Schema({Column("id", Type::INT),
                   Column("nombre", Type::VARCHAR, 20),
                   Column("monto", Type::DOUBLE)});
}

static std::string fila(const Schema& sch, std::int64_t id) {
    Tuple t{{Value::makeInt(id),
             Value::makeStr("reg" + std::to_string(id)),
             Value::makeDouble(id * 1.5)}};
    return serializeTuple(sch, t);
}

// Misma clave para muchas filas distintas: hace falta para los tests de
// duplicados, porque fila() usa el id como clave Y como contenido.
static std::string filaConClave(const Schema& sch, std::int64_t clave, std::int64_t n) {
    Tuple t{{Value::makeInt(clave),
             Value::makeStr("dup" + std::to_string(n)),
             Value::makeDouble(static_cast<double>(n))}};
    return serializeTuple(sch, t);
}

static std::int64_t claveDe(const Schema& sch, const std::string& payload) {
    return extractColumn(sch, 0, payload.data(), static_cast<int>(payload.size())).i;
}

int main() {
    const std::string MAIN = "data/test_seq.dat";
    const std::string OVF  = "data/test_seq.ovf";
    Schema sch = esquema();

    const int N = 4000;
    std::mt19937 rng(11);
    std::vector<std::int64_t> claves(N);
    for (int i = 0; i < N; ++i) claves[i] = i + 1;
    std::shuffle(claves.begin(), claves.end(), rng);   // insercion desordenada

    std::multimap<std::int64_t, int> ref;              // clave -> marca

    {
        resetFile(MAIN);
        resetFile(OVF);
        DiskManager dm(MAIN), dov(OVF);
        BufferPool  bm(&dm, 64), bo(&dov, 64);
        SequentialFile sf(&bm, &bo, sch, 0, 0.75);

        SECTION("insercion desordenada de 4000 filas");
        for (int i = 0; i < N; ++i) {
            sf.insert(fila(sch, claves[i]));
            ref.emplace(claves[i], i);
        }
        CHECK_EQ(sf.count(), static_cast<std::size_t>(N), "cantidad de registros");
        CHECK(sf.mainPages() >= 1, "se creo area principal");
        CHECK(sf.ovfRecords() > 0, "el area de overflow se llego a usar");
        std::cout << "   principal=" << sf.mainPages() << " pag | overflow="
                  << sf.ovfPages() << " pag con " << sf.ovfRecords() << " registros\n";

        SECTION("scanAll devuelve las claves en ORDEN");
        {
            auto rids = sf.scanAll();
            CHECK_EQ(rids.size(), static_cast<std::size_t>(N), "scanAll ve todos los registros");
            std::int64_t anterior = -1;
            bool ordenado = true;
            for (const RID& r : rids) {
                std::string s;
                if (!sf.get(r, s)) { ordenado = false; break; }
                std::int64_t k = claveDe(sch, s);
                if (k < anterior) { ordenado = false; break; }
                anterior = k;
            }
            CHECK(ordenado, "el recorrido sale ordenado por clave pese al overflow");
        }

        SECTION("busqueda puntual contra el multimap de referencia");
        for (int t = 0; t < 400; ++t) {
            std::int64_t k = claves[rng() % N];
            auto got = sf.searchEq(Value::makeInt(k));
            CHECK_EQ(got.size(), ref.count(k), "misma cantidad de coincidencias");
            if (!got.empty()) {
                std::string s;
                CHECK(sf.get(got[0], s), "el RID devuelto es legible");
                CHECK_EQ(claveDe(sch, s), k, "y trae la clave pedida");
            }
        }
        CHECK(sf.searchEq(Value::makeInt(999999)).empty(), "clave inexistente devuelve vacio");

        SECTION("la busqueda binaria cuesta ~log2(P), no P");
        {
            sf.searchEq(Value::makeInt(claves[0]));
            int sondeos = sf.ultimaBusquedaPaginas();
            int P = sf.mainPages();
            int cota = 1;
            while ((1 << cota) < P) ++cota;
            std::cout << "   paginas sondeadas=" << sondeos << " con P=" << P
                      << " (log2(P) ~ " << cota << ")\n";
            CHECK(sondeos <= cota + 1, "el numero de sondeos respeta la cota logaritmica");
            CHECK(P <= 1 || sondeos < P, "y es estrictamente menor que recorrer todo");
        }

        SECTION("busqueda por rango");
        for (int t = 0; t < 40; ++t) {
            std::int64_t lo = 1 + (rng() % (N - 200));
            std::int64_t hi = lo + 150;
            auto got = sf.searchRange(Value::makeInt(lo), Value::makeInt(hi));
            std::size_t esperado = 0;
            for (auto it = ref.lower_bound(lo); it != ref.end() && it->first <= hi; ++it) ++esperado;
            CHECK_EQ(got.size(), esperado, "cantidad de filas en el rango");
        }

        SECTION("borrado en area principal y en overflow");
        {
            int borrados = 0;
            for (int i = 0; i < N; i += 11) {
                std::int64_t k = claves[i];
                auto got = sf.searchEq(Value::makeInt(k));
                if (got.empty()) continue;
                if (sf.erase(got[0])) {
                    ++borrados;
                    auto rango = ref.equal_range(k);
                    if (rango.first != rango.second) ref.erase(rango.first);
                }
            }
            CHECK(borrados > 300, "se borro una cantidad razonable");
            CHECK_EQ(sf.count(), ref.size(), "el contador queda coherente");
            for (int t = 0; t < 300; ++t) {
                std::int64_t k = claves[rng() % N];
                CHECK_EQ(sf.searchEq(Value::makeInt(k)).size(), ref.count(k),
                         "busqueda coherente tras los borrados");
            }
        }

        SECTION("reorganize: fusiona, vacia el overflow y respeta el fill factor");
        {
            int ovf_antes = static_cast<int>(sf.ovfRecords());
            CHECK(ovf_antes > 0, "habia registros en overflow antes de reorganizar");

            sf.reorganize(0.70);

            CHECK_EQ(sf.ovfRecords(), static_cast<long long>(0), "el overflow quedo vacio");
            CHECK_EQ(sf.count(), ref.size(), "no se perdio ni se duplico ningun registro");

            auto rids = sf.scanAll();
            std::int64_t anterior = -1;
            bool ordenado = true;
            for (const RID& r : rids) {
                std::string s;
                if (!sf.get(r, s)) { ordenado = false; break; }
                std::int64_t k = claveDe(sch, s);
                if (k < anterior) { ordenado = false; break; }
                anterior = k;
            }
            CHECK(ordenado, "sigue ordenado tras reorganizar");

            for (int t = 0; t < 200; ++t) {
                std::int64_t k = claves[rng() % N];
                CHECK_EQ(sf.searchEq(Value::makeInt(k)).size(), ref.count(k),
                         "las busquedas siguen dando lo mismo");
            }

            // Con fill factor 0.70 cada pagina debe quedar con holgura:
            // al menos un 15% del espacio util libre.
            const int util = PAGE_SIZE - PageHeader::SIZE;
            const char* b = bm.fetchPage(0);
            int libre = SlotDir::espacioLibre(b);
            bm.unpinPage(0, false);
            std::cout << "   tras reorganizar: " << sf.mainPages() << " paginas, "
                      << libre << " B libres en la pagina 0 (util=" << util << ")\n";
            CHECK(libre > util / 7, "la pagina no quedo llena al 100%: respeta el fill factor");
        }
    }

    SECTION("persistencia: reabrir los dos archivos");
    {
        DiskManager dm(MAIN), dov(OVF);
        BufferPool  bm(&dm, 64), bo(&dov, 64);
        SequentialFile sf(&bm, &bo, sch, 0, 0.75);
        CHECK_EQ(sf.count(), ref.size(), "el contenido sobrevive al cierre");
        for (int t = 0; t < 150; ++t) {
            std::int64_t k = claves[t * 17 % N];
            CHECK_EQ(sf.searchEq(Value::makeInt(k)).size(), ref.count(k), "busqueda tras reabrir");
        }

        SECTION("insertar despues de reorganizar vuelve a usar overflow");
        for (int i = 0; i < 300; ++i) {
            std::int64_t k = 100000 + i;
            sf.insert(fila(sch, k));
            ref.emplace(k, 0);
        }
        CHECK_EQ(sf.count(), ref.size(), "las nuevas filas se contabilizan");
        CHECK_EQ(sf.searchEq(Value::makeInt(100150)).size(), static_cast<std::size_t>(1),
                 "y se pueden encontrar");
    }

    SECTION("con reorganizacion periodica vs sin ella");
    {
        // Mismo conjunto de inserciones, dos regimenes. Sin mantenimiento el
        // archivo degenera en una sola pagina principal con una cadena de
        // overflow gigante; con reorganizacion periodica el area principal
        // crece y la busqueda vuelve a costar log2(P).
        auto correr = [&](bool con_reorg, int* pags, long long* ovf, int* sondeos) {
            const std::string M = con_reorg ? "data/test_seq_con.dat" : "data/test_seq_sin.dat";
            const std::string O = con_reorg ? "data/test_seq_con.ovf" : "data/test_seq_sin.ovf";
            resetFile(M); resetFile(O);
            DiskManager dm(M), dov(O);
            BufferPool  bm(&dm, 64), bo(&dov, 64);
            SequentialFile sf(&bm, &bo, sch, 0, 0.75, 0.20);
            for (int i = 0; i < N; ++i) {
                if (con_reorg && sf.necesitaReorganizacion()) sf.reorganize();
                sf.insert(fila(sch, claves[i]));
            }
            sf.searchEq(Value::makeInt(claves[N / 2]));
            *pags    = sf.mainPages();
            *ovf     = sf.ovfRecords();
            *sondeos = sf.ultimaBusquedaPaginas();

            // Verificar contra los DATOS, no contra el contador en RAM: una
            // reorganizacion mal hecha podria duplicar o perder filas sin que
            // el contador se entere.
            auto rids = sf.scanAll();
            CHECK_EQ(rids.size(), static_cast<std::size_t>(N), "scanAll ve exactamente N filas");
            std::map<std::int64_t, int> vistos;
            for (const RID& r : rids) {
                std::string s2;
                if (sf.get(r, s2)) ++vistos[claveDe(sch, s2)];
            }
            CHECK_EQ(vistos.size(), static_cast<std::size_t>(N), "N claves distintas");
            int duplicadas = 0;
            for (const auto& kv : vistos) if (kv.second > 1) ++duplicadas;
            CHECK_EQ(duplicadas, 0, "ninguna clave quedo duplicada tras reorganizar");
            CHECK_EQ(sf.count(), static_cast<std::size_t>(N), "y el contador coincide con los datos");
        };

        int pags_sin = 0, pags_con = 0, sond_sin = 0, sond_con = 0;
        long long ovf_sin = 0, ovf_con = 0;
        correr(false, &pags_sin, &ovf_sin, &sond_sin);
        correr(true,  &pags_con, &ovf_con, &sond_con);

        std::cout << "   sin reorganizar: " << pags_sin << " pag principales, "
                  << ovf_sin << " en overflow, " << sond_sin << " sondeos\n";
        std::cout << "   con reorganizar: " << pags_con << " pag principales, "
                  << ovf_con << " en overflow, " << sond_con << " sondeos\n";

        CHECK(pags_con > pags_sin, "con mantenimiento el area principal si crece");
        CHECK(ovf_con < ovf_sin / 2, "y el overflow queda acotado");
        CHECK(sond_con >= 1, "la busqueda binaria vuelve a tener trabajo real que hacer");
    }

    // ---------------------------------------------------------------------
    //  REGRESION: claves DUPLICADAS repartidas entre paginas.
    //  reorganize() empaqueta por factor de carga sin respetar los limites
    //  entre claves iguales, asi que una tirada de duplicados queda partida
    //  entre dos paginas y searchEq, que miraba una sola, perdia filas.
    // ---------------------------------------------------------------------
    SECTION("claves duplicadas repartidas entre paginas");
    {
        const std::string M = "data/test_seq_dup.dat";
        const std::string O = "data/test_seq_dup.ovf";
        resetFile(M); resetFile(O);
        DiskManager dm(M), dovf(O);
        BufferPool  bm(&dm, 64), bo(&dovf, 64);
        Schema sch2 = esquema();
        SequentialFile sf(&bm, &bo, sch2, 0);

        // 600 filas con solo 3 claves distintas: 200 de cada una.
        for (int i = 0; i < 600; ++i) sf.insert(filaConClave(sch2, i % 3 + 1, i));
        CHECK_EQ(sf.searchEq(Value::makeInt(1)).size(), static_cast<std::size_t>(200), "duplicados antes de reorganizar");

        sf.reorganize();                       // los reparte entre varias paginas
        CHECK(sf.numPages() > 2, "la reorganizacion usa varias paginas principales");
        CHECK_EQ(sf.searchEq(Value::makeInt(1)).size(), static_cast<std::size_t>(200), "clave 1 completa tras reorganizar");
        CHECK_EQ(sf.searchEq(Value::makeInt(2)).size(), static_cast<std::size_t>(200), "clave 2 completa tras reorganizar");
        CHECK_EQ(sf.searchEq(Value::makeInt(3)).size(), static_cast<std::size_t>(200), "clave 3 completa tras reorganizar");
        CHECK_EQ(sf.searchRange(Value::makeInt(1), Value::makeInt(3)).size(),
                 static_cast<std::size_t>(600), "el rango cubre las tres tiradas");
        CHECK_EQ(sf.scanAll().size(), static_cast<std::size_t>(600), "y el scan sigue viendolas todas");
    }

    // ---------------------------------------------------------------------
    //  REGRESION: una pagina principal que se queda VACIA.
    //  buscarPagina se quedaba con la pagina vacia como respuesta, asi que
    //  un DELETE que vaciara un bloque hacia que SELECT devolviera 0 filas y
    //  que los INSERT siguientes fueran a la pagina equivocada.
    // ---------------------------------------------------------------------
    SECTION("una pagina principal vacia no rompe la busqueda binaria");
    {
        const std::string M = "data/test_seq_vacia.dat";
        const std::string O = "data/test_seq_vacia.ovf";
        resetFile(M); resetFile(O);
        DiskManager dm(M), dovf(O);
        BufferPool  bm(&dm, 64), bo(&dovf, 64);
        Schema sch2 = esquema();
        SequentialFile sf(&bm, &bo, sch2, 0);

        for (int i = 0; i < 2000; ++i) sf.insert(fila(sch2, i));
        sf.reorganize();
        const int P = sf.numPages();
        CHECK(P > 3, "hay varias paginas principales");

        // Vaciar por completo la franja de claves 400..700.
        for (int k = 400; k <= 700; ++k) {
            auto rids = sf.searchEq(Value::makeInt(k));
            for (const RID& r : rids) sf.erase(r);
        }
        CHECK_EQ(sf.searchEq(Value::makeInt(5)).size(),   static_cast<std::size_t>(1), "una clave anterior al hueco sigue apareciendo");
        CHECK_EQ(sf.searchEq(Value::makeInt(399)).size(), static_cast<std::size_t>(1), "la clave justo antes del hueco");
        CHECK_EQ(sf.searchEq(Value::makeInt(701)).size(), static_cast<std::size_t>(1), "la clave justo despues del hueco");
        CHECK(sf.searchEq(Value::makeInt(500)).empty(), "y las borradas ya no estan");
        CHECK_EQ(sf.searchRange(Value::makeInt(0), Value::makeInt(50)).size(),
                 static_cast<std::size_t>(51), "un rango al principio sigue completo");

        // Un INSERT posterior debe seguir respetando el orden del archivo.
        sf.insert(filaConClave(sch2, 3, 9999));
        auto todo = sf.scanAll();                  // devuelve RIDs, en orden de clave
        bool ordenado = true;
        std::int64_t anterior = -1;
        for (const RID& r : todo) {
            std::string payload;
            if (!sf.get(r, payload)) { ordenado = false; break; }
            const std::int64_t k = claveDe(sch2, payload);
            if (k < anterior) ordenado = false;
            anterior = k;
        }
        CHECK(ordenado, "scanAll sigue devolviendo las filas en orden de clave");
    }

    DONE("test_sequential");
    return 0;
}
