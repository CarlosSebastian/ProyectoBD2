// ============================================================================
//  table.hpp - Una tabla = motor de almacenamiento + (opcionalmente) un indice
//
//  Es la primera pieza del motor de consultas: decide sola como resolver una
//  busqueda y deja registrado el plan en lastPlan(), que es lo que alimenta el
//  panel "Plan de ejecucion" del cliente web.
//
//  Rutas de acceso posibles, en orden de preferencia:
//     1. IndexScan / IndexRangeScan   si hay indice aplicable sobre la columna
//     2. Busqueda binaria secuencial  si el motor es SEQUENTIAL y la columna
//                                     es la clave que ordena el archivo
//     3. SeqScan                      en cualquier otro caso
// ============================================================================
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "db/bplus_tree.hpp"
#include "db/buffer_pool.hpp"
#include "db/catalog.hpp"
#include "db/extendible_hash.hpp"
#include "db/heap_file.hpp"
#include "db/record.hpp"
#include "db/sequential_file.hpp"
#include "db/storage_engine.hpp"

namespace db {

// Resumen de como se resolvio la ultima consulta (para el informe y la UI).
struct PlanInfo {
    std::string metodo;          // "INDEX BPLUS", "INDEX HASH", "SEQ BINARY", "SEQ SCAN"
    std::string columna;
    long long   paginas_leidas = 0;
    long long   registros      = 0;
    double      ms             = 0.0;

    std::string str() const;
};

class Table {
public:
    Table(const std::string& data_dir, const TableInfo& info, int pool_size = 64);

    const TableInfo& info()   const { return info_; }
    const Schema&    schema() const { return info_.schema; }
    const char*      engineName() const { return engine_->engineName(); }

    RID  insert(const Tuple& t);
    bool getByRID(const RID& rid, Tuple& out) const;
    bool removeByRID(const RID& rid);

    // Vuelca a disco las paginas sucias de los tres archivos de la tabla.
    // Sin esto, la unica durabilidad seria el destructor de BufferPool, y un
    // Ctrl+C sobre el servidor (que es como dice el README que se detiene)
    // mata el proceso sin ejecutar destructores: se perderia todo lo escrito.
    void flush();

    std::vector<Tuple> searchEq(const std::string& col, const Value& v);
    std::vector<Tuple> searchRange(const std::string& col, const Value& lo, const Value& hi);
    std::vector<Tuple> scan();

    std::vector<RID> searchRIDsEq(const std::string& col, const Value& v);
    std::vector<RID> searchRIDsRange(const std::string& col, const Value& lo, const Value& hi);

    const PlanInfo& lastPlan() const { return last_plan_; }

    void buildIndex();                     // reconstruye el indice recorriendo los datos
    bool reorganize(double fill_factor = -1.0);   // solo SEQUENTIAL; reconstruye el indice

    // Reorganizacion automatica al superar el umbral de overflow. Encendida
    // por defecto; se apaga para el experimento "sin reorganizacion".
    void setAutoReorganize(bool v) { auto_reorg_ = v; }
    bool autoReorganize() const { return auto_reorg_; }
    long long reorganizaciones() const { return seq_ ? seq_->reorganizaciones() : 0; }

    // --------- metricas ---------
    std::size_t count() const { return engine_->count(); }
    int  dataPages()  const { return engine_->numPages(); }
    int  heapPages()  const { return dataPages(); }          // alias historico
    int  indexPages() const;
    // Altura del arbol B+ (0 si el indice no es un B+). La usa el experimento
    // de sensibilidad al tamano de bloque.
    int  indexHeight() const {
        if (bt_int_) return bt_int_->getHeight();
        if (bt_str_) return bt_str_->getHeight();
        return 0;
    }
    int  overflowPages() const { return seq_ ? seq_->ovfPages() : 0; }
    long long overflowRecords() const { return seq_ ? seq_->ovfRecords() : 0; }
    bool esSecuencial() const { return seq_ != nullptr; }
    long long heapReads()  const { return store_disk_->readCount(); }
    long long indexReads() const { return idx_disk_ ? idx_disk_->readCount() : 0; }
    void resetStats();
    double bufferHitRate() const { return store_bp_->hitRate(); }

private:
    void   insertIntoIndex(const Value& key, const RID& rid);
    void   removeFromIndex(const Value& key, const RID& rid);
    bool   indexedColumn(const std::string& col) const;
    bool   claveSecuencial(const std::string& col) const;
    std::vector<RID> indexLookup(const Value& v);
    std::vector<RID> indexRange(const Value& lo, const Value& hi);
    long long lecturasTotales() const;

    TableInfo info_;
    int       key_col_ = -1;          // columna indexada (-1 = sin indice)
    int       seq_col_ = -1;          // columna que ordena el Sequential File

    std::unique_ptr<DiskManager>   store_disk_;
    std::unique_ptr<BufferPool>    store_bp_;
    std::unique_ptr<DiskManager>   ovf_disk_;
    std::unique_ptr<BufferPool>    ovf_bp_;
    std::unique_ptr<StorageEngine> engine_;
    SequentialFile*                seq_ = nullptr;   // no posee: apunta a engine_
    bool                           auto_reorg_ = true;

    std::unique_ptr<DiskManager> idx_disk_;
    std::unique_ptr<BufferPool>  idx_bp_;
    std::unique_ptr<BPlusTree<std::int64_t>>      bt_int_;
    std::unique_ptr<BPlusTree<Key32>>             bt_str_;
    std::unique_ptr<ExtendibleHash<std::int64_t>> hs_int_;
    std::unique_ptr<ExtendibleHash<Key32>>        hs_str_;

    PlanInfo last_plan_;
};

}  // namespace db
