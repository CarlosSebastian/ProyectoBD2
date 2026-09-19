// ============================================================================
//  buffer_pool.hpp - Cache de paginas en RAM con politica LRU y pin counts
//
//  Toda la capa de arriba (heap file, indices) pide paginas AQUI, nunca al
//  disco. Asi el numero de lecturas fisicas depende del tamano del pool, que
//  es exactamente lo que se mide en los experimentos.
//
//  Contrato de uso:
//      char* p = bp.fetchPage(pid);   // queda "pineada": no puede ser evictada
//      ... leer/modificar p ...
//      bp.unpinPage(pid, true);       // true si se modifico
// ============================================================================
#pragma once

#include <cstdint>
#include <unordered_map>
#include <vector>

#include "db/common.hpp"
#include "db/disk_manager.hpp"

namespace db {

class BufferPool {
public:
    BufferPool(DiskManager* dm, int pool_size = 64);
    ~BufferPool();

    BufferPool(const BufferPool&) = delete;
    BufferPool& operator=(const BufferPool&) = delete;

    char* fetchPage(page_id_t pid);          // trae la pagina (pin +1)
    char* newPage(page_id_t* out_pid);       // reserva pagina nueva (pin +1)
    void  unpinPage(page_id_t pid, bool dirty);
    void  flushPage(page_id_t pid);
    void  flushAll();
    // Descarta TODOS los frames sin escribirlos. Se usa despues de truncar el
    // archivo por debajo: lo que hubiera cacheado ya no existe en disco.
    void  invalidateAll();

    // --- estadisticas para el informe experimental ---
    long long hits()      const { return hits_; }
    long long misses()    const { return misses_; }
    long long evictions() const { return evictions_; }
    double    hitRate()   const {
        long long t = hits_ + misses_;
        return t == 0 ? 0.0 : static_cast<double>(hits_) / static_cast<double>(t);
    }
    void resetStats() { hits_ = misses_ = evictions_ = 0; }

    DiskManager* disk() const { return disk_; }

private:
    struct Frame {
        std::vector<char> data;
        page_id_t         page_id   = INVALID_PAGE_ID;
        int               pin_count = 0;
        bool              dirty     = false;
        std::uint64_t     last_used = 0;
        Frame() : data(PAGE_SIZE, 0) {}
    };

    int  findVictim();   // indice de frame libre o LRU sin pines; -1 si todo pineado

    DiskManager*                              disk_;
    std::vector<Frame>                        frames_;
    std::unordered_map<page_id_t, int>        table_;   // page_id -> frame
    std::uint64_t                             clock_ = 0;
    long long hits_ = 0, misses_ = 0, evictions_ = 0;
};

}  // namespace db
