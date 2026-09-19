// ============================================================================
//  page.hpp - Cabecera fisica de pagina (Page Layout)
//
//  El enunciado exige que TODO bloque fisico lleve una cabecera con:
//      page_id, record_count, free_space_offset y next_page_id / prev_page_id
//
//  Se define una sola cabecera de 32 bytes compartida por el Heap File y el
//  Sequential File, para que ambos usen exactamente el mismo layout fisico y
//  se puedan inspeccionar con la misma herramienta (util para el video, donde
//  hay que abrir los binarios y mostrarlos).
//
//     offset  tam  campo
//     ------  ---  ------------------------------------------------------
//        0     4   page_id             identificador del bloque en su archivo
//        4     4   next_page_id        encadenamiento hacia adelante
//        8     4   prev_page_id        encadenamiento hacia atras
//       12     2   record_count        registros ACTIVOS (sin contar tumbas)
//       14     2   slot_count          entradas del directorio (con tumbas)
//       16     2   free_space_offset   inicio del espacio contiguo libre
//       18     2   flags               tipo de pagina y banderas
//       20     4   aux_page            uso libre: cabeza de overflow, etc.
//       24     4   aux_slot            uso libre
//       28     4   reservado
//
//  Tras la cabecera viene el directorio de slots, que crece hacia la derecha,
//  y al final de la pagina los registros, que crecen hacia la izquierda.
// ============================================================================
#pragma once

#include "db/common.hpp"

namespace db {

struct PageHeader {
    static constexpr int SIZE = 32;

    // --- offsets ---
    static constexpr int OFF_PAGE_ID    = 0;
    static constexpr int OFF_NEXT       = 4;
    static constexpr int OFF_PREV       = 8;
    static constexpr int OFF_RECORDS    = 12;
    static constexpr int OFF_SLOTS      = 14;
    static constexpr int OFF_FREE       = 16;
    static constexpr int OFF_FLAGS      = 18;
    static constexpr int OFF_AUX_PAGE   = 20;
    static constexpr int OFF_AUX_SLOT   = 24;

    // --- flags (tipo de pagina) ---
    static constexpr std::uint16_t FLAG_HEAP     = 0x0001;
    static constexpr std::uint16_t FLAG_SEQ_MAIN = 0x0002;
    static constexpr std::uint16_t FLAG_SEQ_OVF  = 0x0004;

    // --- lectura ---
    static page_id_t     pageId(const char* b)  { return readAt<page_id_t>(b, OFF_PAGE_ID); }
    static page_id_t     next(const char* b)    { return readAt<page_id_t>(b, OFF_NEXT); }
    static page_id_t     prev(const char* b)    { return readAt<page_id_t>(b, OFF_PREV); }
    static int           records(const char* b) { return readAt<std::uint16_t>(b, OFF_RECORDS); }
    static int           slots(const char* b)   { return readAt<std::uint16_t>(b, OFF_SLOTS); }
    static int           freeOff(const char* b) { return readAt<std::uint16_t>(b, OFF_FREE); }
    static std::uint16_t flags(const char* b)   { return readAt<std::uint16_t>(b, OFF_FLAGS); }
    static page_id_t     auxPage(const char* b) { return readAt<page_id_t>(b, OFF_AUX_PAGE); }
    static std::int32_t  auxSlot(const char* b) { return readAt<std::int32_t>(b, OFF_AUX_SLOT); }
    static RID           auxRID(const char* b)  { return RID(auxPage(b), auxSlot(b)); }

    // --- escritura ---
    static void setPageId(char* b, page_id_t v)     { writeAt<page_id_t>(b, OFF_PAGE_ID, v); }
    static void setNext(char* b, page_id_t v)       { writeAt<page_id_t>(b, OFF_NEXT, v); }
    static void setPrev(char* b, page_id_t v)       { writeAt<page_id_t>(b, OFF_PREV, v); }
    static void setRecords(char* b, int v)          { writeAt<std::uint16_t>(b, OFF_RECORDS, static_cast<std::uint16_t>(v)); }
    static void setSlots(char* b, int v)            { writeAt<std::uint16_t>(b, OFF_SLOTS, static_cast<std::uint16_t>(v)); }
    static void setFreeOff(char* b, int v)          { writeAt<std::uint16_t>(b, OFF_FREE, static_cast<std::uint16_t>(v)); }
    static void setFlags(char* b, std::uint16_t v)  { writeAt<std::uint16_t>(b, OFF_FLAGS, v); }
    static void setAuxRID(char* b, const RID& r) {
        writeAt<page_id_t>(b, OFF_AUX_PAGE, r.page_id);
        writeAt<std::int32_t>(b, OFF_AUX_SLOT, r.slot);
    }

    // Inicializa una pagina vacia: sin slots, espacio libre hasta el final.
    static void init(char* b, page_id_t pid, std::uint16_t tipo) {
        std::memset(b, 0, PAGE_SIZE);
        setPageId(b, pid);
        setNext(b, INVALID_PAGE_ID);
        setPrev(b, INVALID_PAGE_ID);
        setRecords(b, 0);
        setSlots(b, 0);
        setFreeOff(b, PAGE_SIZE);
        setFlags(b, tipo);
        setAuxRID(b, RID());
    }
};

// ---------------------------------------------------------------------------
//  Directorio de slots: entradas de 4 bytes (offset, longitud) justo despues
//  de la cabecera. offset == 0 marca una tumba (slot borrado).
// ---------------------------------------------------------------------------
struct SlotDir {
    static constexpr int SLOT_SIZE = 4;

    static int off(int i) { return PageHeader::SIZE + i * SLOT_SIZE; }

    static int  offset(const char* b, int i) { return readAt<std::uint16_t>(b, off(i)); }
    static int  length(const char* b, int i) { return readAt<std::uint16_t>(b, off(i) + 2); }
    static bool vivo(const char* b, int i)   { return offset(b, i) != 0; }

    static void set(char* b, int i, int offset_reg, int len) {
        writeAt<std::uint16_t>(b, off(i),     static_cast<std::uint16_t>(offset_reg));
        writeAt<std::uint16_t>(b, off(i) + 2, static_cast<std::uint16_t>(len));
    }
    static void matar(char* b, int i) { set(b, i, 0, 0); }

    // Bytes libres entre el final del directorio y el inicio de los registros.
    static int espacioLibre(const char* b) {
        int fin_dir = PageHeader::SIZE + PageHeader::slots(b) * SLOT_SIZE;
        return PageHeader::freeOff(b) - fin_dir;
    }
    static constexpr int MAX_RECORD = PAGE_SIZE - PageHeader::SIZE - SLOT_SIZE;
};

}  // namespace db
