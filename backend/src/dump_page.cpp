// ============================================================================
//  dump_page.cpp - Inspector de los archivos binarios en disco.
//
//  El enunciado (seccion 6, punto 3) exige que el video muestre los archivos
//  binarios "por dentro". Esta herramienta abre cualquier .dat / .ovf / .idx
//  del motor, se posiciona con seek() en el bloque pedido y despliega:
//
//     - la cabecera de pagina campo por campo, con su offset y sus bytes crudos
//     - el directorio de slots entrada por entrada, marcando las tumbas
//     - el volcado hexadecimal, con los tramos coloreados por zona
//
//  Uso:
//     dump_page <archivo> [pagina]          una pagina concreta (por defecto 0)
//     dump_page <archivo> --resumen         una linea por pagina del archivo
//     dump_page <archivo> <pagina> --hex    volcado hexadecimal completo
//
//  Ejemplos para el video:
//     ./build/dump_page data/empleados.dat 0
//     ./build/dump_page data/empleados.dat --resumen
//     ./build/dump_page data/empleados_id.idx 1 --hex
// ============================================================================
#include "db/page.hpp"
#include "db/common.hpp"

#include <cctype>
#include <cstdio>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

using namespace db;

namespace {

// ---------------------------------------------------------------- utilidades
std::string hex16(unsigned v, int ancho = 4) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "%0*X", ancho, v);
    return buf;
}

// Bytes crudos de un campo, tal como estan en el archivo (little-endian).
std::string bytesDe(const char* b, int offset, int n) {
    std::string s;
    for (int i = 0; i < n; ++i) {
        char buf[4];
        std::snprintf(buf, sizeof buf, "%02X", static_cast<unsigned char>(b[offset + i]));
        if (i) s += ' ';
        s += buf;
    }
    return s;
}

std::string nombreFlags(std::uint16_t f) {
    if (f == 0) return "(sin marcar)";
    std::string s;
    if (f & PageHeader::FLAG_HEAP)     s += "HEAP ";
    if (f & PageHeader::FLAG_SEQ_MAIN) s += "SEQ_MAIN ";
    if (f & PageHeader::FLAG_SEQ_OVF)  s += "SEQ_OVF ";
    if (s.empty()) s = "(desconocido) ";
    return s.substr(0, s.size() - 1);
}

std::string pid(page_id_t p) {
    return (p == INVALID_PAGE_ID) ? "INVALID_PAGE_ID" : std::to_string(p);
}

void linea(char c = '-') { std::cout << std::string(74, c) << "\n"; }

void campo(const char* nombre, int offset, int tam, const std::string& valor,
           const char* buf) {
    std::cout << "  " << std::setw(4) << std::right << offset
              << "  " << std::setw(2) << tam
              << "  " << std::setw(19) << std::left << nombre
              << "  " << std::setw(18) << std::left << valor
              << "  " << bytesDe(buf, offset, tam) << "\n";
}

// ------------------------------------------------------------------- volcado
void volcadoHex(const char* b, int desde, int hasta) {
    for (int i = desde; i < hasta; i += 16) {
        std::cout << "  " << hex16(static_cast<unsigned>(i)) << "  ";
        for (int j = 0; j < 16; ++j) {
            if (i + j < hasta) {
                char t[4];
                std::snprintf(t, sizeof t, "%02X ", static_cast<unsigned char>(b[i + j]));
                std::cout << t;
            } else {
                std::cout << "   ";
            }
            if (j == 7) std::cout << ' ';
        }
        std::cout << " |";
        for (int j = 0; j < 16 && i + j < hasta; ++j) {
            unsigned char c = static_cast<unsigned char>(b[i + j]);
            std::cout << (std::isprint(c) ? static_cast<char>(c) : '.');
        }
        std::cout << "|\n";
    }
}

// ------------------------------------------------------------------ cabecera
void mostrarPagina(const char* b, int numero, bool hex_completo) {
    linea('=');
    std::cout << "  PAGINA " << numero << "   (" << PAGE_SIZE << " bytes, offset del archivo "
              << static_cast<long long>(numero) * PAGE_SIZE << ")\n";
    linea('=');

    std::cout << "\nCABECERA DE PAGINA (" << PageHeader::SIZE << " bytes)\n";
    std::cout << "  off  tam  campo                valor               bytes en disco\n";
    linea();
    campo("page_id",           PageHeader::OFF_PAGE_ID,  4, pid(PageHeader::pageId(b)), b);
    campo("next_page_id",      PageHeader::OFF_NEXT,     4, pid(PageHeader::next(b)), b);
    campo("prev_page_id",      PageHeader::OFF_PREV,     4, pid(PageHeader::prev(b)), b);
    campo("record_count",      PageHeader::OFF_RECORDS,  2, std::to_string(PageHeader::records(b)), b);
    campo("slot_count",        PageHeader::OFF_SLOTS,    2, std::to_string(PageHeader::slots(b)), b);
    campo("free_space_offset", PageHeader::OFF_FREE,     2, std::to_string(PageHeader::freeOff(b)), b);
    campo("flags",             PageHeader::OFF_FLAGS,    2, nombreFlags(PageHeader::flags(b)), b);
    campo("aux_page",          PageHeader::OFF_AUX_PAGE, 4, pid(PageHeader::auxPage(b)), b);
    campo("aux_slot",          PageHeader::OFF_AUX_SLOT, 4, std::to_string(PageHeader::auxSlot(b)), b);

    const int n      = PageHeader::slots(b);
    const int fin_dir = PageHeader::SIZE + n * SlotDir::SLOT_SIZE;
    const int libre   = SlotDir::espacioLibre(b);

    std::cout << "\n  El directorio ocupa de " << PageHeader::SIZE << " a " << fin_dir
              << ", los registros van de " << PageHeader::freeOff(b) << " a " << PAGE_SIZE
              << ",\n  y entre medias quedan " << libre << " bytes libres.\n";

    // ------------------------------------------------------ directorio
    std::cout << "\nDIRECTORIO DE SLOTS (" << n << " entradas de "
              << SlotDir::SLOT_SIZE << " bytes)\n";
    if (n == 0) {
        std::cout << "  (pagina sin slots)\n";
    } else {
        std::cout << "  slot   off_dir   offset_reg   long   estado    RID\n";
        linea();
        int vivos = 0;
        for (int i = 0; i < n; ++i) {
            const bool v = SlotDir::vivo(b, i);
            if (v) ++vivos;
            std::cout << "  " << std::setw(4) << std::right << i
                      << "   " << std::setw(7) << SlotDir::off(i)
                      << "   " << std::setw(10) << SlotDir::offset(b, i)
                      << "   " << std::setw(4) << SlotDir::length(b, i)
                      << "   " << std::setw(8) << std::left
                      << (v ? "vivo" : "TUMBA")
                      << "  <" << numero << ", " << i << ">\n";
        }
        std::cout << "\n  " << vivos << " vivos, " << (n - vivos)
                  << " tumbas. record_count dice " << PageHeader::records(b)
                  << (vivos == PageHeader::records(b) ? "  (coincide)" : "  (NO COINCIDE)")
                  << "\n";
    }

    // ------------------------------------------------------ hexadecimal
    if (hex_completo) {
        std::cout << "\nVOLCADO HEXADECIMAL COMPLETO\n";
        linea();
        volcadoHex(b, 0, PAGE_SIZE);
    } else {
        std::cout << "\nCABECERA EN HEXADECIMAL (bytes 0.." << PageHeader::SIZE - 1 << ")\n";
        linea();
        volcadoHex(b, 0, PageHeader::SIZE);
        if (n > 0) {
            std::cout << "\nDIRECTORIO EN HEXADECIMAL (bytes " << PageHeader::SIZE
                      << ".." << fin_dir - 1 << ")\n";
            linea();
            volcadoHex(b, PageHeader::SIZE, fin_dir);
        }
        const int ini = PageHeader::freeOff(b);
        if (ini > 0 && ini < PAGE_SIZE) {
            const int hasta = (PAGE_SIZE - ini > 128) ? ini + 128 : PAGE_SIZE;
            std::cout << "\nPRIMEROS REGISTROS (desde free_space_offset = " << ini << ")\n";
            linea();
            volcadoHex(b, ini, hasta);
            if (hasta < PAGE_SIZE)
                std::cout << "  ... (" << PAGE_SIZE - hasta
                          << " bytes mas; use --hex para verlos todos)\n";
        }
        std::cout << "\n  Sugerencia: --hex vuelca los " << PAGE_SIZE << " bytes.\n";
    }
    std::cout << "\n";
}

// ------------------------------------------------------------------- resumen
void resumen(std::ifstream& f, long long paginas) {
    std::cout << "\n  pag    page_id    next    prev   recs  slots  free_off  libre  tipo\n";
    linea();
    std::vector<char> buf(PAGE_SIZE);
    for (long long p = 0; p < paginas; ++p) {
        f.clear();
        f.seekg(p * PAGE_SIZE, std::ios::beg);
        f.read(buf.data(), PAGE_SIZE);
        const char* b = buf.data();
        std::cout << "  " << std::setw(3) << std::right << p
                  << "  " << std::setw(9) << pid(PageHeader::pageId(b))
                  << "  " << std::setw(6) << pid(PageHeader::next(b))
                  << "  " << std::setw(6) << pid(PageHeader::prev(b))
                  << "  " << std::setw(5) << PageHeader::records(b)
                  << "  " << std::setw(5) << PageHeader::slots(b)
                  << "  " << std::setw(8) << PageHeader::freeOff(b)
                  << "  " << std::setw(5) << SlotDir::espacioLibre(b)
                  << "  " << nombreFlags(PageHeader::flags(b)) << "\n";
    }
    std::cout << "\n";
}

// ===========================================================================
//  Los archivos de indice NO usan PageHeader: el B+ y el hash tienen su propio
//  layout de nodo. Se decodifican aparte para que el video pueda mostrar
//  tambien el interior de un indice, no solo el de los datos.
// ===========================================================================
constexpr int BP_HDR      = 12;                        // bplus_tree.hpp
constexpr int BP_KS       = 8;                         // claves int64
constexpr int BP_VS       = 8;                         // sizeof(RID)
constexpr int BP_LEAF_MAX = (PAGE_SIZE - BP_HDR) / (BP_KS + BP_VS);
constexpr int BP_INT_MAXK = (PAGE_SIZE - BP_HDR - 4) / (BP_KS + 4);
constexpr int RT_HDR      = 8;                         // rtree.hpp
constexpr int RT_MBR      = 32;                        // cuatro doubles
constexpr int RT_LEAF_MAX = (PAGE_SIZE - RT_HDR) / (RT_MBR + 8);
constexpr int RT_INT_MAX  = (PAGE_SIZE - RT_HDR) / (RT_MBR + 4);
constexpr std::int32_t RT_MAGIC = 0x52545245;          // "RTRE"
constexpr std::int32_t IV_MAGIC = 0x494E5658;          // "INVX"
constexpr int EH_HDR      = 12;                        // extendible_hash.hpp

void dumpBPlus(const char* b, int numero, bool hex_completo) {
    linea('=');
    std::cout << "  PAGINA " << numero << " del indice B+   (offset "
              << static_cast<long long>(numero) * PAGE_SIZE << ")\n";
    linea('=');

    if (numero == 0) {
        std::cout << "\nPAGINA META\n";
        std::cout << "  off  tam  campo                valor               bytes en disco\n";
        linea();
        campo("raiz",   0, 4, pid(readAt<page_id_t>(b, 0)), b);
        campo("altura", 4, 4, std::to_string(readAt<std::int32_t>(b, 4)), b);
        campo("num_entradas", 8, 8, std::to_string(readAt<std::int64_t>(b, 8)), b);
        std::cout << "\n  Fan-out con esta pagina: " << BP_LEAF_MAX
                  << " pares por hoja, " << BP_INT_MAXK << " claves por nodo interno.\n\n";
        linea();
        volcadoHex(b, 0, 32);
        std::cout << "\n";
        return;
    }

    const bool hoja = readAt<std::uint8_t>(b, 0) != 0;
    const int  n    = readAt<std::uint16_t>(b, 1);
    std::cout << "\nCABECERA DE NODO (" << BP_HDR << " bytes)\n";
    std::cout << "  off  tam  campo                valor               bytes en disco\n";
    linea();
    campo("es_hoja", 0, 1, hoja ? "1 (HOJA)" : "0 (INTERNO)", b);
    campo("n_claves", 1, 2, std::to_string(n), b);
    campo("next_leaf", 4, 4, pid(readAt<page_id_t>(b, 4)), b);
    campo("prev_leaf", 8, 4, pid(readAt<page_id_t>(b, 8)), b);

    const int tope = (n < 12) ? n : 12;
    if (hoja) {
        std::cout << "\nPARES <clave, RID>  (" << n << " de " << BP_LEAF_MAX << " posibles)\n";
        std::cout << "  i      clave        RID          off_clave  off_rid\n";
        linea();
        for (int i = 0; i < tope; ++i) {
            const int ok = BP_HDR + i * BP_KS;
            const int ov = BP_HDR + BP_LEAF_MAX * BP_KS + i * BP_VS;
            const RID r  = readAt<RID>(b, ov);
            std::cout << "  " << std::setw(3) << std::right << i
                      << "  " << std::setw(11) << readAt<std::int64_t>(b, ok)
                      << "   <" << std::setw(4) << r.page_id << ", "
                      << std::setw(3) << r.slot << ">"
                      << "   " << std::setw(8) << ok << "   " << std::setw(7) << ov << "\n";
        }
    } else {
        std::cout << "\nCLAVES SEPARADORAS Y PUNTEROS  (" << n << " claves, "
                  << n + 1 << " hijos)\n";
        std::cout << "  i      clave        hijo_izq   hijo_der\n";
        linea();
        for (int i = 0; i < tope; ++i) {
            const int ok = BP_HDR + i * BP_KS;
            const int oc = BP_HDR + BP_INT_MAXK * BP_KS;
            std::cout << "  " << std::setw(3) << std::right << i
                      << "  " << std::setw(11) << readAt<std::int64_t>(b, ok)
                      << "   " << std::setw(8) << pid(readAt<page_id_t>(b, oc + i * 4))
                      << "   " << std::setw(8) << pid(readAt<page_id_t>(b, oc + (i + 1) * 4))
                      << "\n";
        }
    }
    if (n > tope) std::cout << "  ... y " << n - tope << " mas\n";

    std::cout << "\n" << (hex_completo ? "VOLCADO HEXADECIMAL COMPLETO" : "CABECERA EN HEXADECIMAL") << "\n";
    linea();
    volcadoHex(b, 0, hex_completo ? PAGE_SIZE : 48);
    std::cout << "\n";
}

void dumpHash(const char* b, int numero, bool hex_completo) {
    linea('=');
    std::cout << "  PAGINA " << numero << " del indice HASH   (offset "
              << static_cast<long long>(numero) * PAGE_SIZE << ")\n";
    linea('=');

    if (numero == 0) {
        const int gd = readAt<std::int32_t>(b, 0);
        std::cout << "\nPAGINA META\n";
        std::cout << "  off  tam  campo                valor               bytes en disco\n";
        linea();
        campo("global_depth", 0, 4, std::to_string(gd), b);
        campo("num_buckets",  4, 4, std::to_string(readAt<std::int32_t>(b, 4)), b);
        std::cout << "\n  El directorio tiene 2^" << gd << " = " << (1 << gd)
                  << " entradas y vive en la pagina 1.\n\n";
        linea();
        volcadoHex(b, 0, 16);
        std::cout << "\n";
        return;
    }
    if (numero == 1) {
        std::cout << "\nDIRECTORIO  (entradas de 4 B; cada una apunta a un bucket)\n";
        std::cout << "  entrada   bucket\n";
        linea();
        for (int i = 0; i < 16; ++i)
            std::cout << "  " << std::setw(7) << std::right << i
                      << "   " << std::setw(6) << pid(readAt<page_id_t>(b, i * 4)) << "\n";
        std::cout << "  ... (las demas entradas siguen igual)\n\n";
        linea();
        volcadoHex(b, 0, hex_completo ? PAGE_SIZE : 64);
        std::cout << "\n";
        return;
    }

    const int ld = readAt<std::int32_t>(b, 0);
    const int n  = readAt<std::int32_t>(b, 4);
    std::cout << "\nCABECERA DE BUCKET (" << EH_HDR << " bytes)\n";
    std::cout << "  off  tam  campo                valor               bytes en disco\n";
    linea();
    campo("local_depth", 0, 4, std::to_string(ld), b);
    campo("n_entradas",  4, 4, std::to_string(n), b);
    campo("overflow",    8, 4, pid(readAt<page_id_t>(b, 8)), b);

    const int tope = (n < 12) ? n : 12;
    std::cout << "\nENTRADAS <clave, RID>\n";
    std::cout << "  i      clave        RID\n";
    linea();
    for (int i = 0; i < tope; ++i) {
        const int o = EH_HDR + i * 16;
        const RID r = readAt<RID>(b, o + 8);
        std::cout << "  " << std::setw(3) << std::right << i
                  << "  " << std::setw(11) << readAt<std::int64_t>(b, o)
                  << "   <" << std::setw(4) << r.page_id << ", " << std::setw(3) << r.slot << ">\n";
    }
    if (n > tope) std::cout << "  ... y " << n - tope << " mas\n";

    std::cout << "\n" << (hex_completo ? "VOLCADO HEXADECIMAL COMPLETO" : "CABECERA EN HEXADECIMAL") << "\n";
    linea();
    volcadoHex(b, 0, hex_completo ? PAGE_SIZE : 48);
    std::cout << "\n";
}

// ---------------------------------------------------------------------------
//  Indice invertido. Es el unico de los cuatro que no es un arbol: el archivo
//  son cuatro zonas seguidas y la pagina 0 dice donde empieza cada una.
// ---------------------------------------------------------------------------
void dumpInvertido(const char* b, int numero, bool hex_completo,
                   const std::vector<char>& meta) {
    linea('=');
    std::cout << "  PAGINA " << numero << " del indice invertido   (offset "
              << static_cast<long long>(numero) * PAGE_SIZE << ")\n";
    linea('=');

    const int p_post = readAt<std::int32_t>(meta.data(), 24);
    const int p_dicc = readAt<std::int32_t>(meta.data(), 28);
    const int p_docs = readAt<std::int32_t>(meta.data(), 32);
    const int p_fin  = readAt<std::int32_t>(meta.data(), 36);

    if (numero == 0) {
        std::cout << "\nPAGINA META\n";
        std::cout << "  off  tam  campo                valor               bytes en disco\n";
        linea();
        campo("num_docs",     0, 4, std::to_string(readAt<std::int32_t>(b, 0)), b);
        campo("num_terminos", 4, 4, std::to_string(readAt<std::int32_t>(b, 4)), b);
        campo("total_tokens", 8, 8, std::to_string(readAt<std::int64_t>(b, 8)), b);
        campo("MAGIC",       16, 4, "\"INVX\"", b);
        campo("idioma",      20, 4, readAt<std::int32_t>(b, 20) == 1 ? "1 (ES)" : "0 (EN)", b);
        campo("pag_postings",24, 4, std::to_string(p_post), b);
        campo("pag_dicc",    28, 4, std::to_string(p_dicc), b);
        campo("pag_docs",    32, 4, std::to_string(p_docs), b);
        campo("pag_fin",     36, 4, std::to_string(p_fin), b);
        campo("avgdl",       40, 8, std::to_string(readAt<double>(b, 40)), b);
        campo("bloques_spimi",48,4, std::to_string(readAt<std::int32_t>(b, 48)), b);
        std::cout << "\n  Zonas: postings [" << p_post << ", " << p_dicc
                  << ")   diccionario [" << p_dicc << ", " << p_docs
                  << ")   documentos [" << p_docs << ", " << p_fin << ")\n\n";
        linea();
        volcadoHex(b, 0, 56);
        std::cout << "\n";
        return;
    }

    if (numero >= p_post && numero < p_dicc) {
        std::cout << "\nZONA DE POSTINGS\n"
                     "  Bytes crudos: las listas van codificadas como huecos entre doc_id\n"
                     "  consecutivos y frecuencias, ambos en varint de 7 bits, y una lista\n"
                     "  puede cruzar el borde de la pagina. No hay cabecera que separar.\n\n";
        volcadoHex(b, 0, hex_completo ? PAGE_SIZE : 256);
        std::cout << "\n";
        return;
    }

    if (numero >= p_dicc && numero < p_docs) {
        const int n     = readAt<std::int32_t>(b, 0);
        const int usado = readAt<std::int32_t>(b, 4);
        std::cout << "\nPAGINA DEL DICCIONARIO\n";
        std::cout << "  off  tam  campo                valor               bytes en disco\n";
        linea();
        campo("n_terminos", 0, 4, std::to_string(n), b);
        campo("bytes_usados", 4, 4, std::to_string(usado), b);
        std::cout << "\nTERMINOS (ordenados: por eso la busqueda es por biseccion)\n";
        std::cout << "      i  termino                            df        offset     bytes\n";
        linea();
        int off = 8;
        for (int i = 0; i < n && off < PAGE_SIZE; ++i) {
            const int largo = static_cast<unsigned char>(b[off]);
            std::string t(b + off + 1, static_cast<std::size_t>(largo));
            const int o = off + 1 + largo;
            std::cout << "  " << std::setw(5) << i << "  " << std::setw(32) << std::left << t
                      << std::right << std::setw(6) << readAt<std::uint32_t>(b, o)
                      << std::setw(14) << readAt<std::uint64_t>(b, o + 4)
                      << std::setw(10) << readAt<std::uint32_t>(b, o + 12) << "\n";
            off += 1 + largo + 16;
        }
        std::cout << "\n";
        if (hex_completo) { linea(); volcadoHex(b, 0, PAGE_SIZE); }
        return;
    }

    std::cout << "\nTABLA DE DOCUMENTOS  (16 bytes por documento)\n";
    std::cout << "    doc_id       RID     |d| tokens      norma TF-IDF\n";
    linea();
    const int por_pagina = PAGE_SIZE / 16;
    const int base = (numero - p_docs) * por_pagina;
    for (int i = 0; i < por_pagina; ++i) {
        const int o = i * 16;
        const std::int32_t pg = readAt<std::int32_t>(b, o);
        const std::int32_t sl = readAt<std::int32_t>(b, o + 4);
        const std::uint32_t lg = readAt<std::uint32_t>(b, o + 8);
        if (pg == 0 && sl == 0 && lg == 0) continue;
        std::cout << "  " << std::setw(8) << (base + i)
                  << "  " << std::setw(8) << ("(" + std::to_string(pg) + "," + std::to_string(sl) + ")")
                  << std::setw(14) << lg
                  << std::setw(18) << readAt<float>(b, o + 12) << "\n";
    }
    std::cout << "\n";
    if (hex_completo) { linea(); volcadoHex(b, 0, PAGE_SIZE); }
}

// ---------------------------------------------------------------------------
//  Indice espacial R-Tree. A diferencia del B+, un nodo no guarda claves sino
//  CAJAS ENVOLVENTES de 32 bytes, y las hojas apuntan al dato por RID.
// ---------------------------------------------------------------------------
void dumpRTree(const char* b, int numero, bool hex_completo) {
    linea('=');
    std::cout << "  PAGINA " << numero << " del indice R-Tree   (offset "
              << static_cast<long long>(numero) * PAGE_SIZE << ")\n";
    linea('=');

    if (numero == 0) {
        std::cout << "\nPAGINA META\n";
        std::cout << "  off  tam  campo                valor               bytes en disco\n";
        linea();
        campo("raiz",         0, 4, pid(readAt<page_id_t>(b, 0)), b);
        campo("altura",       4, 4, std::to_string(readAt<std::int32_t>(b, 4)), b);
        campo("num_entradas", 8, 8, std::to_string(readAt<std::int64_t>(b, 8)), b);
        campo("MAGIC",       16, 4, "\"RTRE\"", b);
        std::cout << "\n  Fan-out con esta pagina: " << RT_LEAF_MAX
                  << " entradas por hoja, " << RT_INT_MAX << " por nodo interno.\n"
                     "  Cada entrada es un MBR de " << RT_MBR << " B (minx,miny,maxx,maxy)\n"
                     "  mas su RID (8 B) en las hojas o su page_id (4 B) en los internos.\n\n";
        linea();
        volcadoHex(b, 0, 32);
        std::cout << "\n";
        return;
    }

    const bool hoja = readAt<std::uint8_t>(b, 0) != 0;
    const int  n    = readAt<std::uint16_t>(b, 1);
    const int  cap  = hoja ? RT_LEAF_MAX : RT_INT_MAX;

    std::cout << "\nCABECERA DE NODO (" << RT_HDR << " bytes)\n";
    std::cout << "  off  tam  campo                valor               bytes en disco\n";
    linea();
    campo("es_hoja",   0, 1, hoja ? "1 (HOJA)" : "0 (INTERNO)", b);
    campo("n_entradas", 1, 2, std::to_string(n), b);

    std::cout << "\n" << (hoja ? "CAJAS + RID" : "CAJAS + HIJO")
              << "  (" << n << " de " << cap << " posibles)\n";
    std::cout << "    i          minx        miny        maxx        maxy   "
              << (hoja ? "    RID" : "   hijo") << "   off_mbr\n";
    linea();
    const int tope = (n < 10) ? n : 10;
    for (int i = 0; i < tope; ++i) {
        const int om = RT_HDR + i * RT_MBR;
        std::cout << "  " << std::setw(3) << std::right << i << "  ";
        for (int c = 0; c < 4; ++c)
            std::cout << std::setw(11) << std::right << std::fixed << std::setprecision(3)
                      << readAt<double>(b, om + c * 8) << " ";
        if (hoja) {
            const RID r = readAt<RID>(b, RT_HDR + RT_LEAF_MAX * RT_MBR + i * 8);
            std::cout << "  " << std::setw(9) << std::left << r.str();
        } else {
            const page_id_t h = readAt<page_id_t>(b, RT_HDR + RT_INT_MAX * RT_MBR + i * 4);
            std::cout << "  " << std::setw(9) << std::left << pid(h);
        }
        std::cout << "  " << om << "\n";
    }
    if (n > tope) std::cout << "  ... y " << (n - tope) << " entrada(s) mas\n";

    std::cout << "\n  Un MBR con minx==maxx y miny==maxy es un PUNTO; si difieren,\n"
                 "  es la caja envolvente de un poligono (o de un subarbol entero).\n\n";
    linea();
    volcadoHex(b, 0, hex_completo ? PAGE_SIZE : 96);
    std::cout << "\n";
}

void uso() {
    std::cout <<
      "uso: dump_page <archivo> [pagina] [--hex]\n"
      "     dump_page <archivo> --resumen\n\n"
      "  <archivo>    cualquier binario del motor: .dat, .ovf o .idx\n"
      "  [pagina]     numero de bloque, por defecto 0\n"
      "  --hex        volcado hexadecimal de los " << PAGE_SIZE << " bytes\n"
      "  --resumen    una linea por pagina del archivo\n"
      "  --bplus      forzar la lectura como indice B+\n"
      "  --hash       forzar la lectura como indice hash extensible\n"
      "  --rtree      forzar la lectura como indice espacial R-Tree\n\n"
      "  Los .idx se detectan solos; --bplus/--hash/--rtree solo hacen falta si\n"
      "  el archivo tiene otro nombre.\n";
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) { uso(); return 1; }

    const std::string ruta = argv[1];
    bool quiere_resumen = false, hex_completo = false;
    enum class Formato { SLOTTED, BPLUS, HASH, RTREE, INVERTIDO } formato = Formato::SLOTTED;
    long long numero = 0;
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        if      (a == "--resumen") quiere_resumen = true;
        else if (a == "--hex")     hex_completo   = true;
        else if (a == "--bplus")   formato        = Formato::BPLUS;
        else if (a == "--hash")    formato        = Formato::HASH;
        else if (a == "--rtree")   formato        = Formato::RTREE;
        else if (a == "--invertido" || a == "--inverted") formato = Formato::INVERTIDO;
        else if (a == "--help" || a == "-h") { uso(); return 0; }
        else                       numero = std::atoll(a.c_str());
    }

    std::ifstream f(ruta, std::ios::binary);
    if (!f.is_open()) {
        std::cerr << "No se pudo abrir '" << ruta << "'\n";
        return 1;
    }
    f.seekg(0, std::ios::end);
    const long long bytes   = f.tellg();
    const long long paginas = bytes / PAGE_SIZE;

    std::cout << "\nARCHIVO: " << ruta << "\n"
              << "  " << bytes << " bytes = " << paginas << " paginas de "
              << PAGE_SIZE << " B";
    if (bytes % PAGE_SIZE) std::cout << "  (+" << bytes % PAGE_SIZE << " bytes sueltos)";
    std::cout << "\n";

    if (paginas == 0) { std::cerr << "\nEl archivo esta vacio.\n"; return 1; }

    // Los .idx no llevan PageHeader. Se distingue B+ de hash por la META:
    // el B+ guarda la raiz en el offset 0 (un page_id o INVALID), el hash
    // guarda global_depth, que siempre es un entero pequeno.
    if (formato == Formato::SLOTTED && ruta.size() > 4 &&
        ruta.compare(ruta.size() - 4, 4, ".idx") == 0) {
        // Las dos META empiezan con un entero pequeno (raiz / global_depth),
        // asi que no distinguen. Lo que si distingue es la pagina 1:
        //   B+   -> es un NODO: el byte 0 es la bandera es_hoja, 0 o 1.
        //   hash -> es el DIRECTORIO: el byte 0 es el page_id de un bucket,
        //           y los buckets viven de la pagina 2 en adelante.
        // El R-Tree si deja firma: MAGIC "RTRE" en el offset 16 de la META.
        std::vector<char> p0(PAGE_SIZE);
        f.clear(); f.seekg(0, std::ios::beg); f.read(p0.data(), PAGE_SIZE);
        if (readAt<std::int32_t>(p0.data(), 16) == RT_MAGIC) {
            formato = Formato::RTREE;
        } else if (readAt<std::int32_t>(p0.data(), 16) == IV_MAGIC) {
            formato = Formato::INVERTIDO;
        } else {
            formato = Formato::BPLUS;
            if (paginas > 1) {
                std::vector<char> p1(PAGE_SIZE);
                f.clear(); f.seekg(PAGE_SIZE, std::ios::beg); f.read(p1.data(), PAGE_SIZE);
                if (static_cast<unsigned char>(p1[0]) >= 2) formato = Formato::HASH;
            }
        }
        std::cout << "  formato detectado: indice "
                  << (formato == Formato::RTREE ? "R-TREE ESPACIAL"
                      : formato == Formato::INVERTIDO ? "INVERTIDO (texto)"
                      : formato == Formato::HASH ? "HASH EXTENSIBLE" : "ARBOL B+")
                  << "   (--bplus / --hash / --rtree / --invertido para forzarlo)\n";
    }

    if (quiere_resumen) {
        if (formato != Formato::SLOTTED) {
            std::cerr << "\n  --resumen solo aplica a archivos de datos (.dat / .ovf),\n"
                         "  que son los que llevan la cabecera de pagina comun.\n";
            return 1;
        }
        resumen(f, paginas); return 0;
    }

    if (numero < 0 || numero >= paginas) {
        std::cerr << "\nLa pagina " << numero << " no existe: el archivo tiene "
                  << paginas << " (0.." << paginas - 1 << ").\n";
        return 1;
    }

    std::vector<char> buf(PAGE_SIZE);
    f.clear();
    f.seekg(numero * PAGE_SIZE, std::ios::beg);     // <- seek(offset), como exige el enunciado
    f.read(buf.data(), PAGE_SIZE);
    switch (formato) {
        case Formato::BPLUS: dumpBPlus(buf.data(), static_cast<int>(numero), hex_completo); break;
        case Formato::HASH:  dumpHash (buf.data(), static_cast<int>(numero), hex_completo); break;
        case Formato::RTREE: dumpRTree(buf.data(), static_cast<int>(numero), hex_completo); break;
        case Formato::INVERTIDO: {
            // El volcador necesita la META para saber en que zona cae la pagina.
            std::vector<char> meta(PAGE_SIZE, 0);
            f.clear(); f.seekg(0, std::ios::beg); f.read(meta.data(), PAGE_SIZE);
            dumpInvertido(buf.data(), static_cast<int>(numero), hex_completo, meta);
            break;
        }
        default:             mostrarPagina(buf.data(), static_cast<int>(numero), hex_completo);
    }
    return 0;
}
