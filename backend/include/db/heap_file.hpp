// ============================================================================
//  heap_file.hpp - Heap File con SLOTTED PAGES (registros de longitud variable)
//
//  Layout de cada pagina de 4 KB (cabecera comun definida en page.hpp):
//
//     [ PageHeader 32B ][ directorio de slots -> ][ libre ][ <- registros ]
//
//  El directorio crece hacia la derecha y los registros hacia la izquierda; el
//  espacio libre queda en el medio.
//
//  Decisiones de diseno:
//
//  - Las paginas quedan ENCADENADAS por next_page_id / prev_page_id y el
//    recorrido secuencial sigue la cadena, no el orden fisico del archivo.
//  - Al borrar se COMPACTA la pagina para no fragmentarla, pero el indice del
//    slot NO cambia: los RID que ya se entregaron a los indices siguen siendo
//    validos. *Move-the-last* los invalidaria; esta variante los preserva.
//  - El mapa de espacio libre se mantiene en RAM y se reconstruye al abrir:
//    buscar donde insertar es first-fit en memoria, sin I/O. La busqueda NO
//    arranca en la pagina 0 sino en un cursor que apunta a la ultima pagina
//    donde se pudo insertar; si arrancara siempre desde cero, cargar medio
//    millon de filas costaria O(P) por insert y el experimento de insercion
//    masiva no terminaria nunca. Al borrar, el cursor retrocede a la pagina
//    liberada para que ese hueco se vuelva a usar.
// ============================================================================
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "db/buffer_pool.hpp"
#include "db/common.hpp"
#include "db/page.hpp"
#include "db/storage_engine.hpp"

namespace db {

class HeapFile : public StorageEngine {
public:
    static constexpr int MAX_RECORD = SlotDir::MAX_RECORD;

    explicit HeapFile(BufferPool* bp);

    RID              insert(const std::string& payload) override;
    bool             get(const RID& rid, std::string& out) const override;
    bool             erase(const RID& rid) override;
    std::vector<RID> scanAll() const override;
    std::size_t      count() const override;
    int              numPages() const override { return bp_->disk()->numPages(); }
    const char*      engineName() const override { return "HEAP"; }

    // Devuelve el RID resultante (puede cambiar si el registro ya no cabe).
    RID  update(const RID& rid, const std::string& payload);

    int  freeBytes(page_id_t pid) const;
    void printPageInfo(page_id_t pid) const;

private:
    void rebuildFreeMap();

    BufferPool*                bp_;
    std::vector<std::uint16_t> free_map_;              // espacio libre por pagina
    page_id_t                  last_page_ = INVALID_PAGE_ID;   // cola de la cadena
    std::size_t                cursor_    = 0;         // por donde empezar a buscar hueco
};

}  // namespace db
