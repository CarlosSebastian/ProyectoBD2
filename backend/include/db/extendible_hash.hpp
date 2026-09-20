// ============================================================================
//  extendible_hash.hpp - Hash Extensible persistente en disco
//
//  Estructura del archivo:
//     pagina 0 : META      -> global_depth(4) | num_buckets(4) | dir_page(4)
//     pagina 1 : DIRECTORIO-> arreglo de 2^gd page_id (tope derivado de PAGE_SIZE: (PAGE_SIZE/4) entradas)
//     pagina k : BUCKET    -> local_depth(4) | n(4) | overflow(4)  [hdr 12]
//                             entradas (clave, RID) consecutivas
//
//  Se indexa por los gd bits BAJOS del hash. Cuando un bucket se llena:
//     local_depth < global_depth  -> se parte el bucket
//     local_depth == global_depth -> se duplica el directorio y se parte
//     global_depth == MAX_GD      -> se encadena una pagina de overflow
//
//  Da busqueda puntual en ~1-2 lecturas, pero NO soporta rangos: ese es
//  justamente el contraste experimental contra el B+ Tree.
//
//  LIMITACION CONOCIDA: las paginas liberadas al fusionar overflow no se
//  reciclan (no hay free list todavia).
// ============================================================================
#pragma once

#include <string>
#include <vector>

#include "db/buffer_pool.hpp"
#include "db/common.hpp"

namespace db {

template <typename K>
class ExtendibleHash {
public:
    static constexpr int BUCKET_HDR = 12;
    static constexpr int KS         = static_cast<int>(sizeof(K));
    static constexpr int VS         = static_cast<int>(sizeof(RID));
    static constexpr int BUCKET_MAX = (PAGE_SIZE - BUCKET_HDR) / (KS + VS);
    // El directorio vive en UNA pagina y cada entrada ocupa 4 B, asi que el
    // techo de la profundidad global lo fija el tamano de pagina, no una
    // constante: con PAGE_SIZE=1024 o 2048 un MAX_GD fijo de 10 hacia que
    // doubleDirectory() escribiera fuera del frame del buffer pool.
    static constexpr int MAX_DIR_ENTRIES = PAGE_SIZE / 4;
    static constexpr int MAX_GD =
        (MAX_DIR_ENTRIES >= 1024) ? 10 :
        (MAX_DIR_ENTRIES >=  512) ?  9 :
        (MAX_DIR_ENTRIES >=  256) ?  8 : 7;
    static_assert((1 << MAX_GD) * 4 <= PAGE_SIZE,
                  "el directorio del hash no cabe en una pagina");
    static constexpr page_id_t DIR_PAGE = 1;

    // Firma en la META, por el mismo motivo que en el arbol B+.
    static constexpr std::int32_t MAGIC = 0x45484153;   // "EHAS"
    static constexpr int OFF_MAGIC = 16;

    explicit ExtendibleHash(BufferPool* bp) : bp_(bp) {
        if (bp_->disk()->numPages() == 0) {
            page_id_t meta;
            char* m = bp_->newPage(&meta);                       // pagina 0
            std::memset(m, 0, PAGE_SIZE);
            bp_->unpinPage(meta, true);

            page_id_t dir;
            char* d = bp_->newPage(&dir);                        // pagina 1
            std::memset(d, 0, PAGE_SIZE);
            bp_->unpinPage(dir, true);

            page_id_t b0 = newBucket(1);
            page_id_t b1 = newBucket(1);

            setGlobalDepth(1);
            setNumBuckets(2);
            setDirEntry(0, b0);
            setDirEntry(1, b1);
        }
        if (bp_->disk()->numPages() > 0) {
            char* m0 = bp_->fetchPage(0);
            const std::int32_t magia = readAt<std::int32_t>(m0, OFF_MAGIC);
            if (magia == 0) { writeAt<std::int32_t>(m0, OFF_MAGIC, MAGIC); bp_->unpinPage(0, true); }
            else {
                bp_->unpinPage(0, false);
                if (magia != MAGIC)
                    throw DBException(
                        "El archivo de indice no contiene un hash extensible. "
                        "Suele pasar cuando se reutiliza un .idx de otro tipo: "
                        "borre el archivo y vuelva a crear el indice.");
            }
        }
    }

    // ------------------------------------------------------------------ API
    void insert(const K& key, const RID& rid) {
        for (int intento = 0; intento < 64; ++intento) {
            int gd  = globalDepth();
            int idx = static_cast<int>(hashKey(key) & mask(gd));
            page_id_t bpid = dirEntry(idx);

            if (tryInsertInChain(bpid, key, rid)) return;

            int ld = localDepth(bpid);
            if (ld < gd)               splitBucket(idx);
            else if (gd < MAX_GD)    { doubleDirectory(); splitBucket(static_cast<int>(hashKey(key) & mask(globalDepth()))); }
            else                     { appendOverflow(bpid, key, rid); return; }
        }
        throw DBException("ExtendibleHash: no se pudo insertar tras 64 intentos");
    }

    std::vector<RID> search(const K& key) const {
        std::vector<RID> out;
        int idx = static_cast<int>(hashKey(key) & mask(globalDepth()));
        page_id_t pid = dirEntry(idx);
        while (pid != INVALID_PAGE_ID) {
            const char* b = bp_->fetchPage(pid);
            int n = bucketN(b);
            for (int i = 0; i < n; ++i)
                if (readAt<K>(b, entKeyOff(i)) == key) out.push_back(readAt<RID>(b, entValOff(i)));
            page_id_t nxt = readAt<page_id_t>(b, 8);
            bp_->unpinPage(pid, false);
            pid = nxt;
        }
        return out;
    }

    bool remove(const K& key, const RID& rid = RID()) {
        int idx = static_cast<int>(hashKey(key) & mask(globalDepth()));
        page_id_t pid = dirEntry(idx);
        while (pid != INVALID_PAGE_ID) {
            char* b = bp_->fetchPage(pid);
            int n = bucketN(b);
            for (int i = 0; i < n; ++i) {
                if (!(readAt<K>(b, entKeyOff(i)) == key)) continue;
                RID r = readAt<RID>(b, entValOff(i));
                if (rid.valid() && !(r == rid)) continue;
                // se reemplaza por la ultima entrada (el orden no importa en un hash)
                writeAt<K>(b, entKeyOff(i),   readAt<K>(b, entKeyOff(n - 1)));
                writeAt<RID>(b, entValOff(i), readAt<RID>(b, entValOff(n - 1)));
                setBucketN(b, n - 1);
                bp_->unpinPage(pid, true);
                return true;
            }
            page_id_t nxt = readAt<page_id_t>(b, 8);
            bp_->unpinPage(pid, false);
            pid = nxt;
        }
        return false;
    }

    // ------------------------------------------------------- info / metricas
    int globalDepth() const { return metaRead<std::int32_t>(0); }
    int numBuckets()  const { return metaRead<std::int32_t>(4); }
    int numPages()    const { return bp_->disk()->numPages(); }

    int overflowPages() const {
        int total = 0;
        int gd = globalDepth();
        std::vector<page_id_t> vistos;
        for (int i = 0; i < (1 << gd); ++i) {
            page_id_t p = dirEntry(i);
            if (std::find(vistos.begin(), vistos.end(), p) != vistos.end()) continue;
            vistos.push_back(p);
            const char* b = bp_->fetchPage(p);
            page_id_t nxt = readAt<page_id_t>(b, 8);
            bp_->unpinPage(p, false);
            while (nxt != INVALID_PAGE_ID) {
                ++total;
                const char* nb = bp_->fetchPage(nxt);
                page_id_t s = readAt<page_id_t>(nb, 8);
                bp_->unpinPage(nxt, false);
                nxt = s;
            }
        }
        return total;
    }

    static int bucketCapacity() { return BUCKET_MAX; }

private:
    static int entKeyOff(int i) { return BUCKET_HDR + i * (KS + VS); }
    static int entValOff(int i) { return BUCKET_HDR + i * (KS + VS) + KS; }

    static std::uint64_t mask(int d) { return (d >= 64) ? ~0ULL : ((1ULL << d) - 1ULL); }

    static int  bucketN(const char* b)      { return readAt<std::int32_t>(b, 4); }
    static void setBucketN(char* b, int n)  { writeAt<std::int32_t>(b, 4, n); }

    template <typename T> T metaRead(int off) const {
        const char* m = bp_->fetchPage(0);
        T v = readAt<T>(m, off);
        bp_->unpinPage(0, false);
        return v;
    }
    template <typename T> void metaWrite(int off, const T& v) const {
        char* m = bp_->fetchPage(0);
        writeAt<T>(m, off, v);
        bp_->unpinPage(0, true);
    }
    void setGlobalDepth(int d) { metaWrite<std::int32_t>(0, d); }
    void setNumBuckets(int n)  { metaWrite<std::int32_t>(4, n); }

    page_id_t dirEntry(int i) const {
        const char* d = bp_->fetchPage(DIR_PAGE);
        page_id_t p = readAt<page_id_t>(d, i * 4);
        bp_->unpinPage(DIR_PAGE, false);
        return p;
    }
    void setDirEntry(int i, page_id_t p) const {
        char* d = bp_->fetchPage(DIR_PAGE);
        writeAt<page_id_t>(d, i * 4, p);
        bp_->unpinPage(DIR_PAGE, true);
    }

    int localDepth(page_id_t pid) const {
        const char* b = bp_->fetchPage(pid);
        int ld = readAt<std::int32_t>(b, 0);
        bp_->unpinPage(pid, false);
        return ld;
    }

    page_id_t newBucket(int local_depth) {
        page_id_t pid;
        char* b = bp_->newPage(&pid);
        std::memset(b, 0, PAGE_SIZE);
        writeAt<std::int32_t>(b, 0, local_depth);
        writeAt<std::int32_t>(b, 4, 0);
        writeAt<page_id_t>(b, 8, INVALID_PAGE_ID);
        bp_->unpinPage(pid, true);
        return pid;
    }

    // Inserta si hay hueco en el bucket principal o en su cadena de overflow.
    bool tryInsertInChain(page_id_t pid, const K& key, const RID& rid) {
        while (pid != INVALID_PAGE_ID) {
            char* b = bp_->fetchPage(pid);
            int n = bucketN(b);
            if (n < BUCKET_MAX) {
                writeAt<K>(b, entKeyOff(n), key);
                writeAt<RID>(b, entValOff(n), rid);
                setBucketN(b, n + 1);
                bp_->unpinPage(pid, true);
                return true;
            }
            page_id_t nxt = readAt<page_id_t>(b, 8);
            bp_->unpinPage(pid, false);
            if (nxt == INVALID_PAGE_ID) return false;   // lleno hasta el final
            pid = nxt;
        }
        return false;
    }

    void appendOverflow(page_id_t head, const K& key, const RID& rid) {
        page_id_t pid = head, prev = INVALID_PAGE_ID;
        while (pid != INVALID_PAGE_ID) {
            const char* b = bp_->fetchPage(pid);
            page_id_t nxt = readAt<page_id_t>(b, 8);
            bp_->unpinPage(pid, false);
            prev = pid;
            pid  = nxt;
        }
        page_id_t np = newBucket(localDepth(head));
        char* pb = bp_->fetchPage(prev);
        writeAt<page_id_t>(pb, 8, np);
        bp_->unpinPage(prev, true);
        tryInsertInChain(np, key, rid);
    }

    void doubleDirectory() {
        int gd = globalDepth();
        if (gd >= MAX_GD) return;
        char* d = bp_->fetchPage(DIR_PAGE);
        int old_size = 1 << gd;
        for (int i = 0; i < old_size; ++i)
            writeAt<page_id_t>(d, (old_size + i) * 4, readAt<page_id_t>(d, i * 4));
        bp_->unpinPage(DIR_PAGE, true);
        setGlobalDepth(gd + 1);
    }

    void splitBucket(int idx) {
        int gd = globalDepth();
        page_id_t old_pid = dirEntry(idx);
        int ld = localDepth(old_pid);
        if (ld >= gd) return;                      // no se puede partir sin duplicar

        // 1) recolectar TODAS las entradas del bucket y su cadena de overflow
        std::vector<K>   ks;
        std::vector<RID> vs;
        page_id_t pid = old_pid;
        while (pid != INVALID_PAGE_ID) {
            const char* b = bp_->fetchPage(pid);
            int n = bucketN(b);
            for (int i = 0; i < n; ++i) {
                ks.push_back(readAt<K>(b, entKeyOff(i)));
                vs.push_back(readAt<RID>(b, entValOff(i)));
            }
            page_id_t nxt = readAt<page_id_t>(b, 8);
            bp_->unpinPage(pid, false);
            pid = nxt;
        }

        // 2) vaciar el bucket viejo (y romper la cadena) y crear el hermano
        {
            char* b = bp_->fetchPage(old_pid);
            writeAt<std::int32_t>(b, 0, ld + 1);
            writeAt<std::int32_t>(b, 4, 0);
            writeAt<page_id_t>(b, 8, INVALID_PAGE_ID);
            bp_->unpinPage(old_pid, true);
        }
        page_id_t new_pid = newBucket(ld + 1);
        setNumBuckets(numBuckets() + 1);

        // 3) reapuntar las entradas del directorio que ahora van al hermano
        int low = idx & static_cast<int>(mask(ld));
        for (int i = 0; i < (1 << gd); ++i) {
            if ((i & static_cast<int>(mask(ld))) != low) continue;
            if ((i >> ld) & 1) setDirEntry(i, new_pid);
            else               setDirEntry(i, old_pid);
        }

        // 4) redistribuir
        for (std::size_t i = 0; i < ks.size(); ++i) {
            int j = static_cast<int>(hashKey(ks[i]) & mask(ld + 1));
            page_id_t dest = ((j >> ld) & 1) ? new_pid : old_pid;
            if (!tryInsertInChain(dest, ks[i], vs[i])) appendOverflow(dest, ks[i], vs[i]);
        }
    }

    BufferPool* bp_;
};

}  // namespace db
