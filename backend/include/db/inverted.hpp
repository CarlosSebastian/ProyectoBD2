// ============================================================================
//  inverted.hpp - Indice invertido en disco, construido con SPIMI
//
//  QUE PROBLEMA RESUELVE
//  Para responder "que documentos hablan de 'indice invertido'" un escaneo
//  tendria que leer y tokenizar los N documentos enteros. El indice invertido
//  da la vuelta a la matriz documento x termino: en vez de guardar, por
//  documento, que terminos tiene, guarda por TERMINO la lista de documentos
//  donde aparece. Responder la consulta pasa a ser leer dos listas cortas.
//
//  CONSTRUCCION EXTERNA (SPIMI)
//  El indice completo no cabe en RAM, y ordenar todos los pares (termino, doc)
//  tampoco. SPIMI (Single-Pass In-Memory Indexing) lo resuelve asi:
//     1. recorre los documentos UNA sola vez acumulando un diccionario en RAM
//     2. cuando el diccionario llega al presupuesto de memoria, lo ordena por
//        termino y lo VUELCA a un bloque temporal en disco; la RAM se libera
//     3. al terminar, mezcla los k bloques en una sola pasada con un heap
//  Nunca se ordena nada mas grande que el presupuesto, y cada documento se lee
//  una vez. Esa es toda la gracia del algoritmo.
//
//  EL ARCHIVO
//  Un solo .idx paginado, con cuatro zonas escritas en este orden:
//
//     pagina 0            META: contadores, idioma, MAGIC y donde empieza cada zona
//     [1, fin_post)       POSTINGS: un unico flujo de bytes, sin cabeceras de
//                         pagina. Cada lista son pares (doc_id, tf) en orden
//                         creciente de doc_id, codificados como HUECOS entre
//                         doc_id consecutivos y en VARINT de 7 bits. El hueco
//                         entre documentos vecinos es un numero pequeno, y un
//                         numero pequeno ocupa un byte: el indice se reduce a
//                         un tercio frente a guardar enteros de 32 bits.
//     [fin_post, fin_dic) DICCIONARIO: los terminos ORDENADOS, con su df y la
//                         direccion de su lista. Al estar ordenados se busca
//                         por biseccion de paginas: log2(P) lecturas.
//     [fin_dic, fin_doc)  TABLA DE DOCUMENTOS: indexada por doc_id, guarda el
//                         RID de la fila real, el largo |d| del documento en
//                         terminos y su norma TF-IDF.
//
//  POR QUE HACE FALTA |d|
//  BM25 penaliza a los documentos largos: un documento de mil palabras tiene
//  mas probabilidad de contener cualquier termino por puro azar. Sin |d| y sin
//  el promedio avgdl no se puede normalizar, y el ranking se lo quedarian
//  siempre los documentos mas largos.
//
//  RECUPERACION BOOLEANA
//  Ademas del ranking, el indice resuelve consultas booleanas AND, OR y
//  AND NOT. Y lo hace sin ordenar nada: las listas de postings ya estan
//  ordenadas por doc_id, asi que combinar dos es recorrerlas a la vez con un
//  puntero en cada una, avanzando siempre el menor. Cuesta O(n+m), que es lo
//  mismo que leerlas. Esa es la razon de fondo por la que las listas se
//  guardan ordenadas y no en el orden en que aparecieron los documentos.
//
//  LO QUE ESTE INDICE NO HACE
//  No admite insercion incremental. Un indice invertido se construye en masa;
//  anadir un documento obligaria a desplazar casi todas las listas de postings.
//  Los motores reales lo resuelven con segmentos y fusiones periodicas; aqui la
//  tabla marca el indice como invalido y lo reconstruye, que para el tamano de
//  corpus del proyecto cuesta menos que mantener segmentos.
// ============================================================================
#pragma once

#include <cmath>
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include "db/buffer_pool.hpp"
#include "db/common.hpp"
#include "db/text.hpp"

namespace db {

class InvertedIndex {
public:
    enum class Ranking { BM25, TFIDF };

    struct Resultado {
        RID           rid;
        std::uint32_t doc_id = 0;
        double        score  = 0.0;
    };

    // Parametros clasicos de BM25 (Robertson & Walker). k1 gobierna cuanto
    // suma repetir un termino (saturacion) y b cuanto pesa la longitud del
    // documento; b = 0 desactiva la normalizacion por longitud.
    //
    // Convenio: se puntua cada termino DISTINTO de la consulta una sola vez.
    // La formula completa lleva ademas un factor por la frecuencia del termino
    // EN LA CONSULTA, pero en consultas de usuario (tres o cuatro palabras) ese
    // factor vale uno casi siempre, y omitirlo es lo que hacen Lucene y
    // Elasticsearch. TF-IDF si usa la frecuencia de la consulta, porque ahi el
    // vector de la consulta tiene que ser comparable con el del documento.
    static constexpr double K1 = 1.2;
    static constexpr double B  = 0.75;

    static constexpr std::int32_t MAGIC      = 0x494E5658;  // "INVX"
    static constexpr int          OFF_MAGIC  = 16;

    explicit InvertedIndex(BufferPool* bp);

    // ---- construccion ----
    // 'siguiente' entrega un documento por llamada y devuelve false cuando se
    // acaban. Recibir un callback y no un vector es lo que permite que la
    // memoria usada no dependa del tamano del corpus.
    void construir(const std::function<bool(RID&, std::string&)>& siguiente,
                   texto::Idioma idi,
                   const std::string& prefijo_temporal,
                   std::size_t presupuesto_bytes = 8u * 1024u * 1024u);

    // ---- consulta ----
    // Devuelve como mucho k resultados ordenados por puntaje descendente.
    // k < 0 significa "todos los que coincidan".
    //
    // Si la consulta lleva operadores booleanos, primero se resuelve el
    // conjunto booleano y DESPUES se rankea solo a los supervivientes. Las dos
    // cosas se combinan en una sola consulta a proposito: el booleano dice
    // que documentos son admisibles y el ranking en que orden mostrarlos.
    std::vector<Resultado> buscar(const std::string& consulta, int k,
                                  Ranking r = Ranking::BM25) const;

    // ---- recuperacion booleana ----
    // Gramatica:   expr := operando { (AND | OR | [AND] NOT) operando }
    // Sin parentesis y sin precedencia: se evalua de izquierda a derecha, que
    // es justo lo que permite resolver cada paso con un merge de dos punteros.
    // Los operadores se escriben en MAYUSCULAS para poder distinguirlos de
    // palabras del corpus que se escriban igual.
    // Devuelve los doc_id que satisfacen la expresion, en orden creciente.
    //
    // Un operando que el analisis lexico deja vacio (una stopword, o una
    // palabra mas corta que el minimo) es NEUTRO y se ignora: 'the AND gato'
    // devuelve lo mismo que 'gato'. Fallar ahi seria peor, porque el usuario
    // no tiene por que saber cual es la lista de stopwords.
    std::vector<std::uint32_t> booleana(const std::string& expresion) const;

    // true si la consulta lleva operadores booleanos en mayusculas.
    static bool tieneBooleanos(const std::string& consulta);

    // ---- metadatos ----
    std::uint32_t numDocs()     const { return num_docs_; }
    std::uint32_t numTerminos() const { return num_terminos_; }
    std::uint64_t totalTokens() const { return total_tokens_; }
    double        avgdl()       const { return avgdl_; }
    texto::Idioma idioma()      const { return idioma_; }
    int           numPages()    const { return bp_->disk()->numPages(); }
    int           bloquesSPIMI() const { return bloques_; }

    // Frecuencia de documento de un termino YA analizado (para tests y para
    // explicar el plan). Devuelve 0 si el termino no esta en el diccionario.
    std::uint32_t df(const std::string& termino) const;

    // Terminos en que se descompone una consulta con el idioma del indice.
    std::vector<std::string> analizar(const std::string& s) const {
        return texto::tokenizar(s, idioma_);
    }

    // Lo mismo, pero quitando antes los operadores booleanos. Es lo que hay
    // que mostrar en el plan de ejecucion: AND y NOT no son terminos y no
    // tienen df.
    std::vector<std::string> terminosDe(const std::string& consulta) const;

private:
    struct EntradaDicc {
        std::uint32_t df     = 0;
        std::uint64_t offset = 0;   // byte dentro de la zona de postings
        std::uint32_t bytes  = 0;
    };
    struct Posting { std::uint32_t doc = 0; std::uint32_t tf = 0; };

    void leerMeta();
    void escribirMeta();
    void crearVacio();

    // Lectura de un rango de bytes de una zona paginada sin cabeceras.
    void leerRango(page_id_t zona, std::uint64_t offset, std::uint32_t n, char* dest) const;

    bool buscarTermino(const std::string& t, EntradaDicc* out) const;
    // doc_id donde aparece un termino YA analizado, en orden creciente.
    std::vector<std::uint32_t> docsDe(const std::string& termino) const;
    std::vector<Posting> leerPostings(const EntradaDicc& e) const;
    bool leerDoc(std::uint32_t doc_id, RID* rid, std::uint32_t* largo, float* norma) const;

    BufferPool* bp_;

    std::uint32_t num_docs_     = 0;
    std::uint32_t num_terminos_ = 0;
    std::uint64_t total_tokens_ = 0;
    double        avgdl_        = 0.0;
    texto::Idioma idioma_       = texto::Idioma::EN;
    int           bloques_      = 0;

    page_id_t pag_post_ = 1;   // primera pagina de cada zona
    page_id_t pag_dicc_ = 1;
    page_id_t pag_docs_ = 1;
    page_id_t pag_fin_  = 1;
};

}  // namespace db
