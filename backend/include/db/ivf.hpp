// ============================================================================
//  ivf.hpp - Indice vectorial IVF (Inverted File) en disco
//
//  QUE PROBLEMA RESUELVE
//  Un embedding es un punto en 384 dimensiones. Buscar los k mas parecidos a
//  uno dado es un problema de vecinos mas cercanos, pero el R-Tree de la
//  Parte 2 no sirve: en dimension alta las cajas envolventes se solapan tanto
//  que la poda deja de podar y el arbol acaba visitandolo todo. Es la
//  "maldicion de la dimensionalidad", y es la razon de que la busqueda
//  vectorial renuncie a ser exacta y se conforme con ser APROXIMADA.
//
//  LA IDEA DE IVF
//  Se agrupan los vectores en nlist celdas con k-means. Cada celda guarda sus
//  vectores juntos en disco. Para responder una consulta se calcula a que
//  celdas pertenece --- comparando contra los nlist centroides, que son pocos
//  --- y solo se leen las nprobe celdas mas cercanas. Si el vecino de verdad
//  mas cercano cayo en una celda que no se visito, se pierde: de ahi que el
//  resultado sea aproximado y que la medida de calidad sea el RECALL.
//
//  nprobe es la perilla del compromiso:
//     nprobe = 1       lo mas rapido, el recall mas bajo
//     nprobe = nlist   equivale a fuerza bruta, recall 1 y ninguna ventaja
//  La curva recall contra latencia que sale de mover nprobe es exactamente lo
//  que el enunciado pide medir.
//
//  EL ARCHIVO
//     pagina 0            META: dimension, nlist, metrica, MAGIC y zonas
//     [p_cent, p_dir)     CENTROIDES: nlist * dim floats, plano
//     [p_dir,  p_ent)     DIRECTORIO: por celda, donde empieza su lista y
//                         cuantos vectores tiene
//     [p_ent,  p_fin)     ENTRADAS: por vector, RID + norma + las dim
//                         componentes. Los vectores se guardan DENTRO del
//                         indice (variante IVF-Flat) para que recorrer una
//                         celda sea lectura secuencial; tenerlos solo en el
//                         heap obligaria a un salto aleatorio por candidato.
//
//  POR QUE EL VECTOR MANDA EN EL DISENO
//  Con dim = 384 cada entrada pesa 1548 bytes: en una pagina de 4 KB entran
//  dos y media. El indice acaba siendo mas grande que la tabla, y el costo de
//  una consulta se mide en cuantas paginas de celda hay que leer. Por eso las
//  celdas se escriben contiguas y por eso importa que el clustering quede
//  equilibrado: una celda gigante es una lectura larga que no se puede evitar.
//
//  CONSTRUCCION
//  Igual que el indice invertido, es masiva y no admite altas fila a fila:
//  insertar un vector exigiria recolocarlo dentro de su celda y correr todo lo
//  que viene detras. La tabla marca el indice como invalido y lo reconstruye.
// ============================================================================
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "db/buffer_pool.hpp"
#include "db/common.hpp"

namespace db {

// Distancias entre embeddings, sueltas del indice: el planificador las
// necesita para evaluar un umbral sobre una fila que ya tiene en memoria,
// sin pasar por el IVF.
namespace vecdist {
// Distancia coseno en [0,2]: 0 mismo sentido, 1 perpendicular, 2 opuesto.
// Un vector nulo no tiene direccion y queda a distancia 1 de todo.
double coseno(const std::vector<float>& a, const std::vector<float>& b);
double euclidiana(const std::vector<float>& a, const std::vector<float>& b);
inline double entre(const std::vector<float>& a, const std::vector<float>& b, bool cos) {
    return cos ? coseno(a, b) : euclidiana(a, b);
}
}  // namespace vecdist

class IVFIndex {
public:
    enum class Metrica { COSENO, L2 };

    static std::string nombreMetrica(Metrica m);
    static Metrica     metricaDesdeNombre(const std::string& s);

    struct Resultado {
        RID    rid;
        double dist = 0.0;     // distancia coseno en [0,2], o euclidiana
    };

    // Recorre la fuente entregando cada vector con su RID. Se invoca VARIAS
    // veces durante la construccion (una para muestrear, una para asignar),
    // asi que tiene que poder repetirse y dar siempre el mismo orden.
    using Fuente = std::function<void(const std::function<void(const RID&,
                                                               const std::vector<float>&)>&)>;

    static constexpr std::int32_t MAGIC     = 0x49564658;   // "IVFX"
    static constexpr int          OFF_MAGIC = 16;

    explicit IVFIndex(BufferPool* bp);

    // listas <= 0 pide el valor por defecto: raiz de N, que es la regla
    // practica habitual (deja del orden de raiz de N vectores por celda).
    void construir(const Fuente& recorrer, int dimension, int listas,
                   Metrica m, unsigned semilla = 2026, int iteraciones = 12);

    // nprobe <= 0 usa un valor por defecto proporcional a nlist.
    std::vector<Resultado> buscar(const std::vector<float>& consulta,
                                  int k, int nprobe) const;

    // Fuerza bruta sobre TODAS las entradas del indice. Es la referencia con
    // la que se mide el recall: sin ella, "aproximado" no significa nada.
    std::vector<Resultado> buscarExacto(const std::vector<float>& consulta, int k) const;

    int           dimension()   const { return dim_; }
    int           listas()      const { return nlist_; }
    std::uint32_t size()        const { return num_vec_; }
    Metrica       metrica()     const { return metrica_; }
    int           numPages()    const { return bp_->disk()->numPages(); }
    double        inercia()     const { return inercia_; }
    int           iteraciones() const { return iters_; }
    int           nprobePorDefecto() const;

    // Cuantos vectores cayeron en cada celda. Un reparto muy desigual explica
    // por que dos consultas con el mismo nprobe pueden costar muy distinto.
    std::vector<std::uint32_t> tamanosDeListas() const;
    // Centroide de una celda (para los tests y para el informe).
    std::vector<float> centroide(int i) const;

private:
    void leerMeta();
    void escribirMeta();
    void crearVacio();
    void leerRango(page_id_t zona, std::uint64_t off, std::uint32_t n, char* dest) const;
    void escribirRango(page_id_t zona, std::uint64_t off, std::uint32_t n, const char* src);
    // Recorre 'cuantas' entradas consecutivas desde un byte dado, leyendolas
    // por bloques para no volver a pedir la misma pagina en cada entrada.
    void leerRangoEntradas(std::uint64_t desde_byte, std::uint32_t cuantas,
                           const std::function<void(const RID&, float, const float*)>& f) const;

    int bytesPorEntrada() const { return 8 + 4 + dim_ * 4; }

    BufferPool*   bp_;
    std::int32_t  num_vec_ = 0;
    std::int32_t  dim_     = 0;
    std::int32_t  nlist_   = 0;
    Metrica       metrica_ = Metrica::COSENO;
    double        inercia_ = 0.0;
    std::int32_t  iters_   = 0;

    page_id_t p_cent_ = 1, p_dir_ = 1, p_ent_ = 1, p_fin_ = 1;
};

}  // namespace db
