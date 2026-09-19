// ============================================================================
//  test_heap.cpp - Heap file con slotted pages
// ============================================================================
#include <cstdio>
#include <map>
#include <random>
#include <string>

#include "db/disk_manager.hpp"
#include "db/heap_file.hpp"
#include "test_util.hpp"

using namespace db;

static std::string reg(int i) {   // registros de LONGITUD VARIABLE a proposito
    std::string s = "reg-" + std::to_string(i) + "-";
    s.append(static_cast<std::size_t>(10 + (i * 7) % 180), 'x');
    return s;
}

int main() {
    const std::string F = "data/test_heap.dat";
    resetFile(F);

    std::map<int, RID> pos;
    const int N = 3000;

    {
        DiskManager dm(F);
        BufferPool  bp(&dm, 16);
        HeapFile    heap(&bp);

        SECTION("insertar 3000 registros de longitud variable");
        for (int i = 0; i < N; ++i) pos[i] = heap.insert(reg(i));
        CHECK_EQ(heap.count(), static_cast<std::size_t>(N), "cantidad tras insertar");
        CHECK(heap.numPages() > 1, "debio usar varias paginas");

        SECTION("lectura por RID");
        for (int i = 0; i < N; i += 137) {
            std::string out;
            CHECK(heap.get(pos[i], out), "get de un RID valido");
            CHECK_EQ(out, reg(i), "el contenido leido coincide");
        }

        SECTION("borrado + compactacion");
        int libre_antes = heap.freeBytes(0);
        std::string tmp;
        CHECK(heap.erase(pos[1]), "borrar un registro existente");
        CHECK(heap.erase(pos[2]), "borrar otro registro");
        CHECK(!heap.erase(pos[1]), "no se puede borrar dos veces");
        CHECK(!heap.get(pos[1], tmp), "un registro borrado ya no se lee");
        CHECK(heap.freeBytes(0) > libre_antes, "la pagina quedo con mas espacio libre (compacto)");

        SECTION("los RID de los demas registros NO cambian tras compactar");
        for (int i = 3; i < 40; ++i) {
            std::string out;
            CHECK(heap.get(pos[i], out), "el vecino sigue accesible");
            CHECK_EQ(out, reg(i), "y con el contenido correcto");
        }
        CHECK_EQ(heap.count(), static_cast<std::size_t>(N - 2), "cantidad tras 2 borrados");

        SECTION("update");
        RID r = heap.update(pos[10], reg(10));                       // mismo tamano
        CHECK(r == pos[10], "update del mismo tamano conserva el RID");
        RID r2 = heap.update(pos[11], std::string(400, 'z'));        // mas grande
        std::string out;
        CHECK(heap.get(r2, out), "el registro actualizado se lee");
        CHECK_EQ(out, std::string(400, 'z'), "contenido del update");

        SECTION("casos de borde");
        CHECK(!heap.get(RID(9999, 0), out), "RID fuera de rango");
        CHECK(!heap.erase(RID(-1, -1)), "RID invalido");
        bool lanzo = false;
        try { heap.insert(std::string(HeapFile::MAX_RECORD + 1, 'a')); } catch (const DBException&) { lanzo = true; }
        CHECK(lanzo, "un registro mas grande que la pagina debe fallar");
    }

    SECTION("persistencia: reabrir el archivo");
    {
        DiskManager dm(F);
        BufferPool  bp(&dm, 16);
        HeapFile    heap(&bp);
        std::string out;
        CHECK(heap.get(pos[500], out), "el registro sobrevive al cierre");
        CHECK_EQ(out, reg(500), "contenido intacto tras reabrir");
        CHECK_EQ(heap.count(), static_cast<std::size_t>(N - 2), "cantidad intacta");
    }

    DONE("test_heap");
    return 0;
}
