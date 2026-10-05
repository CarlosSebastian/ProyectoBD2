// ============================================================================
//  table.hpp - Una tabla = motor de almacenamiento + N indices
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
//
//  Para columnas POINT se suman dos rutas espaciales (Entregable 2):
//     - ventana  'col WITHIN (x0,y0,x1,y1)'      -> R-Tree, o scan con filtro
//     - KNN      'ORDER BY col <-> POINT(x,y)'   -> R-Tree best-first, o scan
//                                                   con ordenacion parcial
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
#include "db/rtree.hpp"
#include "db/sequential_file.hpp"
#include "db/storage_engine.hpp"

namespace db {

// Resumen de como se resolvio la ultima consulta (para el informe y la UI).
struct PlanInfo {
    std::string metodo;          // "INDEX BPLUS", "INDEX HASH", "SEQ BINARY", "SEQ SCAN"
    std::string columna;
    long long   paginas_leidas = 0;
    long long   registros      = 0;
    // Candidatos que el indice devolvio y el refinamiento descarto. Solo tiene
    // sentido en geometrias no puntuales, donde el MBR es una aproximacion.
    long long   descartados    = 0;
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

    // ---- consultas espaciales (columna POINT) ----
    // Ventana: todos los puntos dentro del rectangulo.
    std::vector<Tuple> searchWithin(const std::string& col,
                                    double x0, double y0, double x1, double y1);
    std::vector<RID>   searchRIDsWithin(const std::string& col,
                                        double x0, double y0, double x1, double y1);
    // k vecinos mas cercanos, ya ordenados por distancia creciente.
    // k < 0 significa "todos", ordenados igual.
    std::vector<Tuple> searchKNN(const std::string& col, double px, double py, int k);
    // Lo mismo pero por distancia GEOGRAFICA en metros (x = lon, y = lat).
    std::vector<Tuple> searchKNNGeo(const std::string& col, double lon, double lat, int k);
    // Todos los puntos a no mas de 'metros' del punto dado.
    std::vector<Tuple> searchRadio(const std::string& col, double lon, double lat, double metros);
    std::vector<RID>   searchRIDsRadio(const std::string& col, double lon, double lat, double metros);
    // Poligonos que contienen al punto. Filtra por MBR en el indice y refina
    // con la geometria real.
    std::vector<Tuple> searchContains(const std::string& col, double px, double py);
    std::vector<RID>   searchRIDsContains(const std::string& col, double px, double py);

    const PlanInfo& lastPlan() const { return last_plan_; }

    void buildIndex();                     // vacia el indice y lo repuebla desde los datos
    void recrearIndiceVacio();             // trunca el .idx y recrea la estructura
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
    // Altura del primer arbol que la tenga (B+ o R-Tree). La usa el
    // experimento de sensibilidad al tamano de bloque, que solo monta uno.
    int  indexHeight() const;
    int  indexHeight(const std::string& col) const;
    std::size_t numIndices() const { return indices_.size(); }
    int  overflowPages() const { return seq_ ? seq_->ovfPages() : 0; }
    long long overflowRecords() const { return seq_ ? seq_->ovfRecords() : 0; }
    bool esSecuencial() const { return seq_ != nullptr; }
    long long heapReads()  const { return store_disk_->readCount(); }
    long long indexReads() const {
        long long n = 0;
        for (const Indice& ix : indices_) n += ix.disk->readCount();
        return n;
    }
    void resetStats();
    double bufferHitRate() const { return store_bp_->hitRate(); }

private:
    // ------------------------------------------------------------------
    //  Un indice vivo: la estructura en disco mas su propio archivo, su
    //  gestor y su buffer pool. Cada indice es un archivo independiente,
    //  asi que una tabla puede tener a la vez un B+ sobre la clave primaria
    //  y un R-Tree sobre una columna POINT sin que se estorben.
    // ------------------------------------------------------------------
    struct Indice {
        int         col  = -1;                 // columna del esquema que indexa
        IndexKind   kind = IndexKind::BPLUS;
        std::string columna;                   // nombre, para los mensajes

        std::unique_ptr<DiskManager> disk;
        std::unique_ptr<BufferPool>  bp;

        std::unique_ptr<BPlusTree<std::int64_t>>      bt_int;
        std::unique_ptr<BPlusTree<Key32>>             bt_str;
        std::unique_ptr<ExtendibleHash<std::int64_t>> hs_int;
        std::unique_ptr<ExtendibleHash<Key32>>        hs_str;
        std::unique_ptr<RTree>                        rt;

        void soltarEstructuras() {
            bt_int.reset(); bt_str.reset(); hs_int.reset(); hs_str.reset(); rt.reset();
        }
    };

    void   abrirIndice(Indice& ix);                       // crea la estructura en disco
    void   insertIntoIndex(const Tuple& t, const RID& rid);
    void   removeFromIndex(const Tuple& t, const RID& rid);
    const Indice* indicePara(const std::string& col) const;
    Indice*       indicePara(const std::string& col);
    bool   indexedColumn(const std::string& col) const { return indicePara(col) != nullptr; }
    int    columnaPunto(const std::string& col) const;   // -1 si no es POINT
    int    columnaGeom(const std::string& col) const;    // -1 si no es POINT ni POLYGON
    // Caja envolvente del valor geometrico de una columna (punto o poligono).
    static MBR cajaDe(const Value& v);
    bool   claveSecuencial(const std::string& col) const;
    static std::vector<RID> indexLookup(const Indice& ix, const Value& v);
    static std::vector<RID> indexRange(const Indice& ix, const Value& lo, const Value& hi);
    long long lecturasTotales() const;

    TableInfo info_;
    int       seq_col_ = -1;          // columna que ordena el Sequential File

    std::unique_ptr<DiskManager>   store_disk_;
    std::unique_ptr<BufferPool>    store_bp_;
    std::unique_ptr<DiskManager>   ovf_disk_;
    std::unique_ptr<BufferPool>    ovf_bp_;
    std::unique_ptr<StorageEngine> engine_;
    SequentialFile*                seq_ = nullptr;   // no posee: apunta a engine_
    bool                           auto_reorg_ = true;

    std::vector<Indice> indices_;
    std::string         data_dir_;
    int                 pool_size_ = 64;

    PlanInfo last_plan_;
};

}  // namespace db
