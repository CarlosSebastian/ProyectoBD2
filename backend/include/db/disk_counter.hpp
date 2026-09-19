// ============================================================================
//  disk_counter.hpp - Monitor global de I/O fisico (DiskCounter)
//
//  El enunciado exige un componente que registre EXACTAMENTE dos variables:
//      disk_reads  : bloques fisicos leidos
//      disk_writes : bloques fisicos escritos
//
//  Cada DiskManager incrementa este contador global ademas de su contador
//  local. El contador global es el que se reporta por consulta en la API:
//  una consulta toca el heap y el indice, que son archivos distintos, y lo
//  que interesa es el total de transferencias de la consulta completa.
//
//  Uso tipico:
//      DiskCounter::Snapshot s0 = DiskCounter::global().snapshot();
//      ... ejecutar la consulta ...
//      DiskCounter::Delta d = DiskCounter::global().since(s0);
//      d.reads, d.writes
// ============================================================================
#pragma once

#include <cstdint>

namespace db {

class DiskCounter {
public:
    struct Snapshot {
        long long reads = 0, writes = 0, page_accesses = 0, buffer_hits = 0;
    };
    struct Delta {
        long long reads = 0, writes = 0, page_accesses = 0, buffer_hits = 0;
    };

    static DiskCounter& global() {
        static DiskCounter instancia;
        return instancia;
    }

    void countRead()  { ++disk_reads_; }
    void countWrite() { ++disk_writes_; }

    // Accesos LOGICOS: cada vez que una capa superior pide una pagina al
    // buffer pool. Si ya estaba cacheada no hay I/O fisico, pero el acceso
    // igual es trabajo del algoritmo. Es la metrica que hace visible la
    // diferencia entre IndexScan y SeqScan aunque todo quepa en RAM
    // (equivale al "shared hit" que reporta EXPLAIN en PostgreSQL).
    void countPageAccess() { ++page_accesses_; }
    void countBufferHit()  { ++buffer_hits_; }

    long long diskReads()    const { return disk_reads_; }
    long long diskWrites()   const { return disk_writes_; }
    long long pageAccesses() const { return page_accesses_; }
    long long bufferHits()   const { return buffer_hits_; }

    Snapshot snapshot() const {
        return Snapshot{disk_reads_, disk_writes_, page_accesses_, buffer_hits_};
    }
    Delta since(const Snapshot& s) const {
        return Delta{disk_reads_ - s.reads, disk_writes_ - s.writes,
                     page_accesses_ - s.page_accesses, buffer_hits_ - s.buffer_hits};
    }
    void reset() { disk_reads_ = disk_writes_ = page_accesses_ = buffer_hits_ = 0; }

private:
    DiskCounter() = default;
    long long disk_reads_    = 0;
    long long disk_writes_   = 0;
    long long page_accesses_ = 0;
    long long buffer_hits_   = 0;
};

}  // namespace db
