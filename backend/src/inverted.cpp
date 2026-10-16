#include "db/inverted.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <queue>

namespace db {

// ---------------------------------------------------------------------------
//  Disposicion de la pagina 0 (META)
// ---------------------------------------------------------------------------
namespace {
constexpr int M_NUM_DOCS  = 0;    // int32
constexpr int M_NUM_TERMS = 4;    // int32
constexpr int M_TOKENS    = 8;    // int64
constexpr int M_MAGIC     = 16;   // int32
constexpr int M_IDIOMA    = 20;   // int32
constexpr int M_PAG_POST  = 24;   // int32
constexpr int M_PAG_DICC  = 28;   // int32
constexpr int M_PAG_DOCS  = 32;   // int32
constexpr int M_PAG_FIN   = 36;   // int32
constexpr int M_AVGDL     = 40;   // double
constexpr int M_BLOQUES   = 48;   // int32

// Una entrada de la tabla de documentos: RID + |d| + norma TF-IDF.
constexpr int DOC_BYTES = 16;
constexpr int DOCS_POR_PAGINA = PAGE_SIZE / DOC_BYTES;

// Cabecera de una pagina del diccionario: cuantas entradas y cuantos bytes.
constexpr int DIC_HDR = 8;

// ---------------------------------------------------------------------------
//  Varint de 7 bits (LEB128 sin signo)
//
//  Los huecos entre doc_id consecutivos y las frecuencias de termino son casi
//  siempre numeros chicos. Un entero de 32 bits gasta 4 bytes para guardar un
//  1; en varint ocupa 1. Como la zona de postings es lo que domina el tamano
//  del indice, y el tamano del indice es I/O, la diferencia se nota.
// ---------------------------------------------------------------------------
inline void ponerVarint(std::string& out, std::uint32_t v) {
    while (v >= 0x80) { out += static_cast<char>((v & 0x7F) | 0x80); v >>= 7; }
    out += static_cast<char>(v);
}
inline std::uint32_t sacarVarint(const char* buf, std::size_t& i) {
    std::uint32_t v = 0;
    int corrimiento = 0;
    while (true) {
        const unsigned char b = static_cast<unsigned char>(buf[i++]);
        v |= static_cast<std::uint32_t>(b & 0x7F) << corrimiento;
        if ((b & 0x80) == 0) break;
        corrimiento += 7;
    }
    return v;
}

// ---------------------------------------------------------------------------
//  Escritor de una zona paginada SIN cabeceras: la zona es un unico arreglo
//  de bytes repartido en paginas consecutivas. Mantiene una sola pagina
//  pineada a la vez, asi que escribir el indice entero no depende del tamano
//  del buffer pool.
// ---------------------------------------------------------------------------
class EscritorZona {
public:
    explicit EscritorZona(BufferPool* bp)
        : bp_(bp), inicio_(static_cast<page_id_t>(bp->disk()->numPages())) {}
    ~EscritorZona() { cerrarPagina(); }

    EscritorZona(const EscritorZona&) = delete;
    EscritorZona& operator=(const EscritorZona&) = delete;

    void escribir(const void* src, std::size_t n) {
        const char* p = static_cast<const char*>(src);
        while (n > 0) {
            if (!pag_) { pag_ = bp_->newPage(&pid_); off_ = 0; }
            const std::size_t cabe = static_cast<std::size_t>(PAGE_SIZE - off_);
            const std::size_t m = (cabe < n) ? cabe : n;
            std::memcpy(pag_ + off_, p, m);
            off_ += static_cast<int>(m);
            p += m; n -= m; total_ += m;
            if (off_ == PAGE_SIZE) cerrarPagina();
        }
    }
    void cerrarPagina() {
        if (pag_) { bp_->unpinPage(pid_, true); pag_ = nullptr; }
    }
    page_id_t inicio() const { return inicio_; }
    page_id_t fin() {
        cerrarPagina();
        return static_cast<page_id_t>(bp_->disk()->numPages());
    }
    std::uint64_t total() const { return total_; }

private:
    BufferPool* bp_;
    page_id_t   inicio_;
    page_id_t   pid_ = INVALID_PAGE_ID;
    char*       pag_ = nullptr;
    int         off_ = 0;
    std::uint64_t total_ = 0;
};

// ---------------------------------------------------------------------------
//  Escritor del diccionario. A diferencia de los postings, aqui cada pagina SI
//  tiene cabecera y las entradas no cruzan el borde: la busqueda binaria
//  necesita poder leer una pagina suelta y entenderla.
// ---------------------------------------------------------------------------
class EscritorDicc {
public:
    explicit EscritorDicc(BufferPool* bp)
        : bp_(bp), inicio_(static_cast<page_id_t>(bp->disk()->numPages())),
          buf_(PAGE_SIZE, 0) {}

    void agregar(const std::string& t, std::uint32_t df,
                 std::uint64_t offset, std::uint32_t bytes) {
        const int necesita = 1 + static_cast<int>(t.size()) + 16;
        if (usado_ + necesita > PAGE_SIZE) volcar();
        char* b = buf_.data();
        b[usado_] = static_cast<char>(static_cast<unsigned char>(t.size()));
        std::memcpy(b + usado_ + 1, t.data(), t.size());
        int o = usado_ + 1 + static_cast<int>(t.size());
        writeAt<std::uint32_t>(b, o, df);
        writeAt<std::uint64_t>(b, o + 4, offset);
        writeAt<std::uint32_t>(b, o + 12, bytes);
        usado_ += necesita;
        ++n_;
    }
    void volcar() {
        if (n_ == 0) return;
        writeAt<std::int32_t>(buf_.data(), 0, n_);
        writeAt<std::int32_t>(buf_.data(), 4, usado_);
        page_id_t pid;
        char* p = bp_->newPage(&pid);
        std::memcpy(p, buf_.data(), PAGE_SIZE);
        bp_->unpinPage(pid, true);
        std::fill(buf_.begin(), buf_.end(), 0);
        n_ = 0;
        usado_ = DIC_HDR;
    }
    page_id_t inicio() const { return inicio_; }
    page_id_t fin() {
        volcar();
        return static_cast<page_id_t>(bp_->disk()->numPages());
    }

private:
    BufferPool*       bp_;
    page_id_t         inicio_;
    std::vector<char> buf_;
    std::int32_t      n_     = 0;
    int               usado_ = DIC_HDR;
};

}  // namespace

// ---------------------------------------------------------------------------
//  Apertura y cabecera
// ---------------------------------------------------------------------------
InvertedIndex::InvertedIndex(BufferPool* bp) : bp_(bp) {
    if (bp_->disk()->numPages() == 0) crearVacio();
    else                              leerMeta();
}

void InvertedIndex::crearVacio() {
    page_id_t pid;
    char* p = bp_->newPage(&pid);
    if (pid != 0) throw DBException("El indice invertido tiene que empezar en la pagina 0");
    std::memset(p, 0, PAGE_SIZE);
    writeAt<std::int32_t>(p, M_MAGIC, MAGIC);
    writeAt<std::int32_t>(p, M_PAG_POST, 1);
    writeAt<std::int32_t>(p, M_PAG_DICC, 1);
    writeAt<std::int32_t>(p, M_PAG_DOCS, 1);
    writeAt<std::int32_t>(p, M_PAG_FIN, 1);
    bp_->unpinPage(pid, true);
    pag_post_ = pag_dicc_ = pag_docs_ = pag_fin_ = 1;
}

void InvertedIndex::leerMeta() {
    char* p = bp_->fetchPage(0);
    const std::int32_t magia = readAt<std::int32_t>(p, M_MAGIC);
    if (magia != MAGIC) {
        bp_->unpinPage(0, false);
        throw DBException("El archivo no es un indice invertido (firma incorrecta)");
    }
    num_docs_     = static_cast<std::uint32_t>(readAt<std::int32_t>(p, M_NUM_DOCS));
    num_terminos_ = static_cast<std::uint32_t>(readAt<std::int32_t>(p, M_NUM_TERMS));
    total_tokens_ = static_cast<std::uint64_t>(readAt<std::int64_t>(p, M_TOKENS));
    idioma_       = readAt<std::int32_t>(p, M_IDIOMA) == 1 ? texto::Idioma::ES : texto::Idioma::EN;
    pag_post_     = readAt<std::int32_t>(p, M_PAG_POST);
    pag_dicc_     = readAt<std::int32_t>(p, M_PAG_DICC);
    pag_docs_     = readAt<std::int32_t>(p, M_PAG_DOCS);
    pag_fin_      = readAt<std::int32_t>(p, M_PAG_FIN);
    avgdl_        = readAt<double>(p, M_AVGDL);
    bloques_      = readAt<std::int32_t>(p, M_BLOQUES);
    bp_->unpinPage(0, false);
}

void InvertedIndex::escribirMeta() {
    char* p = bp_->fetchPage(0);
    writeAt<std::int32_t>(p, M_NUM_DOCS,  static_cast<std::int32_t>(num_docs_));
    writeAt<std::int32_t>(p, M_NUM_TERMS, static_cast<std::int32_t>(num_terminos_));
    writeAt<std::int64_t>(p, M_TOKENS,    static_cast<std::int64_t>(total_tokens_));
    writeAt<std::int32_t>(p, M_MAGIC,     MAGIC);
    writeAt<std::int32_t>(p, M_IDIOMA,    idioma_ == texto::Idioma::ES ? 1 : 0);
    writeAt<std::int32_t>(p, M_PAG_POST,  pag_post_);
    writeAt<std::int32_t>(p, M_PAG_DICC,  pag_dicc_);
    writeAt<std::int32_t>(p, M_PAG_DOCS,  pag_docs_);
    writeAt<std::int32_t>(p, M_PAG_FIN,   pag_fin_);
    writeAt<double>      (p, M_AVGDL,     avgdl_);
    writeAt<std::int32_t>(p, M_BLOQUES,   bloques_);
    bp_->unpinPage(0, true);
}

// ---------------------------------------------------------------------------
//  Lectura de un rango de bytes de una zona sin cabeceras
// ---------------------------------------------------------------------------
void InvertedIndex::leerRango(page_id_t zona, std::uint64_t offset,
                              std::uint32_t n, char* dest) const {
    while (n > 0) {
        const page_id_t pid = zona + static_cast<page_id_t>(offset / PAGE_SIZE);
        const int o = static_cast<int>(offset % PAGE_SIZE);
        const std::uint32_t m = std::min<std::uint32_t>(n, static_cast<std::uint32_t>(PAGE_SIZE - o));
        char* p = bp_->fetchPage(pid);
        std::memcpy(dest, p + o, m);
        bp_->unpinPage(pid, false);
        dest += m; offset += m; n -= m;
    }
}

// ---------------------------------------------------------------------------
//  Busqueda en el diccionario: biseccion sobre las paginas y despues un
//  barrido dentro de la elegida. La biseccion cuesta log2(P) lecturas; el
//  barrido es CPU puro sobre una pagina que ya esta en RAM.
// ---------------------------------------------------------------------------
bool InvertedIndex::buscarTermino(const std::string& t, EntradaDicc* out) const {
    if (pag_dicc_ >= pag_docs_) return false;

    auto primerTermino = [&](page_id_t pid) {
        char* p = bp_->fetchPage(pid);
        const std::int32_t n = readAt<std::int32_t>(p, 0);
        std::string s;
        if (n > 0) {
            const int largo = static_cast<unsigned char>(p[DIC_HDR]);
            s.assign(p + DIC_HDR + 1, static_cast<std::size_t>(largo));
        }
        bp_->unpinPage(pid, false);
        return s;
    };

    page_id_t a = pag_dicc_, b = pag_docs_ - 1, elegida = INVALID_PAGE_ID;
    while (a <= b) {
        const page_id_t m = a + (b - a) / 2;
        if (primerTermino(m) <= t) { elegida = m; a = m + 1; }
        else { if (m == pag_dicc_) break; b = m - 1; }
    }
    if (elegida == INVALID_PAGE_ID) return false;

    char* p = bp_->fetchPage(elegida);
    const std::int32_t n = readAt<std::int32_t>(p, 0);
    int off = DIC_HDR;
    bool encontrado = false;
    for (std::int32_t i = 0; i < n; ++i) {
        const int largo = static_cast<unsigned char>(p[off]);
        if (largo == static_cast<int>(t.size()) &&
            std::memcmp(p + off + 1, t.data(), t.size()) == 0) {
            const int o = off + 1 + largo;
            out->df     = readAt<std::uint32_t>(p, o);
            out->offset = readAt<std::uint64_t>(p, o + 4);
            out->bytes  = readAt<std::uint32_t>(p, o + 12);
            encontrado = true;
            break;
        }
        off += 1 + largo + 16;
    }
    bp_->unpinPage(elegida, false);
    return encontrado;
}

std::uint32_t InvertedIndex::df(const std::string& termino) const {
    EntradaDicc e;
    return buscarTermino(termino, &e) ? e.df : 0;
}

// ---------------------------------------------------------------------------
std::vector<InvertedIndex::Posting> InvertedIndex::leerPostings(const EntradaDicc& e) const {
    std::vector<char> crudo(e.bytes);
    if (e.bytes > 0) leerRango(pag_post_, e.offset, e.bytes, crudo.data());

    std::vector<Posting> out;
    out.reserve(e.df);
    std::size_t i = 0;
    std::uint32_t anterior = 0;
    for (std::uint32_t k = 0; k < e.df && i < crudo.size(); ++k) {
        const std::uint32_t hueco = sacarVarint(crudo.data(), i);
        const std::uint32_t tf    = sacarVarint(crudo.data(), i);
        anterior += hueco;
        out.push_back(Posting{anterior, tf});
    }
    return out;
}

bool InvertedIndex::leerDoc(std::uint32_t doc_id, RID* rid,
                            std::uint32_t* largo, float* norma) const {
    if (doc_id >= num_docs_) return false;
    const page_id_t pid = pag_docs_ + static_cast<page_id_t>(doc_id / DOCS_POR_PAGINA);
    const int off = static_cast<int>(doc_id % DOCS_POR_PAGINA) * DOC_BYTES;
    char* p = bp_->fetchPage(pid);
    rid->page_id = readAt<std::int32_t>(p, off);
    rid->slot    = readAt<std::int32_t>(p, off + 4);
    *largo       = readAt<std::uint32_t>(p, off + 8);
    *norma       = readAt<float>(p, off + 12);
    bp_->unpinPage(pid, false);
    return true;
}

// ===========================================================================
//  SPIMI: construccion externa
// ===========================================================================
void InvertedIndex::construir(const std::function<bool(RID&, std::string&)>& siguiente,
                              texto::Idioma idi,
                              const std::string& prefijo_temporal,
                              std::size_t presupuesto_bytes) {
    // --- 0. borrar lo anterior y dejar solo la pagina META ---
    bp_->invalidateAll();
    bp_->disk()->truncate();
    idioma_       = idi;
    num_docs_     = 0;
    num_terminos_ = 0;
    total_tokens_ = 0;
    avgdl_        = 0.0;
    bloques_      = 0;
    crearVacio();

    struct InfoDoc { RID rid; std::uint32_t largo; };
    std::vector<InfoDoc> docs;

    // --- 1. una sola pasada por los documentos, volcando bloques ---
    std::unordered_map<std::string, std::vector<Posting>> memoria;
    std::vector<std::string> rutas;
    std::size_t usado = 0;

    auto volcarBloque = [&]() {
        if (memoria.empty()) return;
        std::vector<const std::pair<const std::string, std::vector<Posting>>*> orden;
        orden.reserve(memoria.size());
        for (const auto& kv : memoria) orden.push_back(&kv);
        std::sort(orden.begin(), orden.end(),
                  [](const auto* a, const auto* b) { return a->first < b->first; });

        const std::string ruta = prefijo_temporal + "_blk" +
                                 std::to_string(rutas.size()) + ".tmp";
        std::ofstream f(ruta, std::ios::binary | std::ios::trunc);
        if (!f) throw DBException("No se pudo crear el bloque temporal: " + ruta);
        for (const auto* kv : orden) {
            const std::uint16_t largo = static_cast<std::uint16_t>(kv->first.size());
            const std::uint32_t df_t  = static_cast<std::uint32_t>(kv->second.size());
            f.write(reinterpret_cast<const char*>(&largo), 2);
            f.write(kv->first.data(), largo);
            f.write(reinterpret_cast<const char*>(&df_t), 4);
            f.write(reinterpret_cast<const char*>(kv->second.data()),
                    static_cast<std::streamsize>(kv->second.size() * sizeof(Posting)));
        }
        f.close();
        rutas.push_back(ruta);
        memoria.clear();
        usado = 0;
    };

    {
        RID rid;
        std::string documento;
        std::unordered_map<std::string, std::uint32_t> tf_local;
        while (siguiente(rid, documento)) {
            const std::uint32_t doc_id = static_cast<std::uint32_t>(docs.size());
            const std::vector<std::string> terminos = texto::tokenizar(documento, idi);
            tf_local.clear();
            for (const std::string& t : terminos) ++tf_local[t];
            for (const auto& kv : tf_local) {
                std::vector<Posting>& v = memoria[kv.first];
                // Estimacion del costo en RAM: el nodo del hash y la cadena la
                // primera vez, y ocho bytes por cada posting. No hace falta que
                // sea exacta, solo que crezca con lo que de verdad crece.
                if (v.empty()) usado += kv.first.size() + 48;
                v.push_back(Posting{doc_id, kv.second});
                usado += sizeof(Posting);
            }
            docs.push_back(InfoDoc{rid, static_cast<std::uint32_t>(terminos.size())});
            total_tokens_ += terminos.size();
            if (usado >= presupuesto_bytes) volcarBloque();
        }
        volcarBloque();
    }

    num_docs_ = static_cast<std::uint32_t>(docs.size());
    bloques_  = static_cast<int>(rutas.size());
    avgdl_    = num_docs_ ? static_cast<double>(total_tokens_) / num_docs_ : 0.0;

    // --- 2. mezcla de los bloques en una sola pasada ---
    struct Lector {
        std::ifstream        f;
        std::string          termino;
        std::vector<Posting> ps;
        bool                 ok = false;

        void avanzar() {
            std::uint16_t largo = 0;
            if (!f.read(reinterpret_cast<char*>(&largo), 2)) { ok = false; return; }
            termino.assign(largo, '\0');
            if (largo && !f.read(&termino[0], largo))        { ok = false; return; }
            std::uint32_t df_t = 0;
            if (!f.read(reinterpret_cast<char*>(&df_t), 4))  { ok = false; return; }
            ps.resize(df_t);
            if (df_t && !f.read(reinterpret_cast<char*>(ps.data()),
                                static_cast<std::streamsize>(df_t * sizeof(Posting)))) {
                ok = false; return;
            }
            ok = true;
        }
    };

    std::vector<std::unique_ptr<Lector>> lectores;
    using Cabeza = std::pair<std::string, std::size_t>;   // (termino, bloque)
    std::priority_queue<Cabeza, std::vector<Cabeza>, std::greater<Cabeza>> cola;
    for (std::size_t i = 0; i < rutas.size(); ++i) {
        auto L = std::make_unique<Lector>();
        L->f.open(rutas[i], std::ios::binary);
        if (!L->f) throw DBException("No se pudo reabrir el bloque " + rutas[i]);
        L->avanzar();
        if (L->ok) cola.push({L->termino, i});
        lectores.push_back(std::move(L));
    }

    std::vector<double> normas(num_docs_, 0.0);
    const std::string ruta_dicc = prefijo_temporal + "_dic.tmp";

    {
        EscritorZona postings(bp_);
        pag_post_ = postings.inicio();

        std::ofstream fd(ruta_dicc, std::ios::binary | std::ios::trunc);
        if (!fd && !rutas.empty())
            throw DBException("No se pudo crear el diccionario temporal: " + ruta_dicc);

        std::vector<Posting> juntas;
        std::string codificado;
        while (!cola.empty()) {
            const std::string termino = cola.top().first;

            // Todos los bloques que traen este termino. Se consumen en orden de
            // bloque, y como los bloques se generaron recorriendo los
            // documentos en orden, las listas quedan ya ordenadas por doc_id
            // sin tener que ordenarlas.
            juntas.clear();
            while (!cola.empty() && cola.top().first == termino) {
                const std::size_t b = cola.top().second;
                cola.pop();
                Lector& L = *lectores[b];
                juntas.insert(juntas.end(), L.ps.begin(), L.ps.end());
                L.avanzar();
                if (L.ok) cola.push({L.termino, b});
            }

            const std::uint32_t df_t = static_cast<std::uint32_t>(juntas.size());

            // Huecos + varint
            codificado.clear();
            std::uint32_t anterior = 0;
            for (const Posting& p : juntas) {
                ponerVarint(codificado, p.doc - anterior);
                ponerVarint(codificado, p.tf);
                anterior = p.doc;
            }

            const std::uint64_t offset = postings.total();
            postings.escribir(codificado.data(), codificado.size());

            // Entrada del diccionario al archivo temporal.
            const std::uint16_t lt = static_cast<std::uint16_t>(termino.size());
            const std::uint32_t nb = static_cast<std::uint32_t>(codificado.size());
            fd.write(reinterpret_cast<const char*>(&lt), 2);
            fd.write(termino.data(), lt);
            fd.write(reinterpret_cast<const char*>(&df_t), 4);
            fd.write(reinterpret_cast<const char*>(&offset), 8);
            fd.write(reinterpret_cast<const char*>(&nb), 4);
            ++num_terminos_;

            // Norma TF-IDF del documento: hace falta el df definitivo del
            // termino, que recien se conoce aqui, despues de juntar todos los
            // bloques. Por eso se acumula durante la mezcla y no antes.
            if (num_docs_ > 0 && df_t > 0) {
                const double idf = std::log(static_cast<double>(num_docs_) / df_t);
                for (const Posting& p : juntas) {
                    const double w = (1.0 + std::log(static_cast<double>(p.tf))) * idf;
                    normas[p.doc] += w * w;
                }
            }
        }
        fd.close();
        pag_dicc_ = postings.fin();
    }

    // --- 3. el diccionario, del temporal a paginas con cabecera ---
    {
        EscritorDicc dicc(bp_);
        pag_dicc_ = dicc.inicio();
        std::ifstream fd(ruta_dicc, std::ios::binary);
        if (fd) {
            std::string termino;
            while (true) {
                std::uint16_t lt = 0;
                if (!fd.read(reinterpret_cast<char*>(&lt), 2)) break;
                termino.assign(lt, '\0');
                if (lt && !fd.read(&termino[0], lt)) break;
                std::uint32_t df_t = 0, nb = 0;
                std::uint64_t offset = 0;
                if (!fd.read(reinterpret_cast<char*>(&df_t), 4)) break;
                if (!fd.read(reinterpret_cast<char*>(&offset), 8)) break;
                if (!fd.read(reinterpret_cast<char*>(&nb), 4)) break;
                dicc.agregar(termino, df_t, offset, nb);
            }
        }
        pag_docs_ = dicc.fin();
    }

    // --- 4. tabla de documentos ---
    {
        EscritorZona tabla(bp_);
        pag_docs_ = tabla.inicio();
        char fila[DOC_BYTES];
        for (std::uint32_t i = 0; i < num_docs_; ++i) {
            const float n = normas[i] > 0.0 ? static_cast<float>(std::sqrt(normas[i])) : 1.0f;
            writeAt<std::int32_t>(fila, 0, docs[i].rid.page_id);
            writeAt<std::int32_t>(fila, 4, docs[i].rid.slot);
            writeAt<std::uint32_t>(fila, 8, docs[i].largo);
            writeAt<float>(fila, 12, n);
            tabla.escribir(fila, DOC_BYTES);
        }
        pag_fin_ = tabla.fin();
    }

    escribirMeta();
    bp_->flushAll();

    for (const std::string& r : rutas) std::remove(r.c_str());
    std::remove(ruta_dicc.c_str());
}

// ===========================================================================
//  Consulta: recorrido documento-a-documento con heap de tamano k
//
//  Las listas de postings estan ordenadas por doc_id, asi que se avanzan en
//  paralelo como en una mezcla: en cada vuelta se atiende el doc_id mas chico
//  pendiente, se suman las contribuciones de los terminos que lo contienen y
//  el documento queda puntuado de una vez. Asi nunca hay que guardar un
//  acumulador por documento, solo los k mejores vistos hasta ahora.
// ===========================================================================
std::vector<InvertedIndex::Resultado>
InvertedIndex::buscar(const std::string& consulta, int k, Ranking r) const {
    std::vector<Resultado> salida;
    if (num_docs_ == 0) return salida;

    const std::vector<std::string> terminos = texto::tokenizar(consulta, idioma_);
    if (terminos.empty()) return salida;

    std::unordered_map<std::string, std::uint32_t> tf_consulta;
    for (const std::string& t : terminos) ++tf_consulta[t];

    struct Lista {
        std::vector<Posting> ps;
        std::size_t          i   = 0;
        double               idf = 0.0;
        double               wq  = 0.0;   // peso del termino en la consulta (TF-IDF)
    };
    std::vector<Lista> listas;
    listas.reserve(tf_consulta.size());

    const double N = static_cast<double>(num_docs_);
    for (const auto& kv : tf_consulta) {
        EntradaDicc e;
        if (!buscarTermino(kv.first, &e) || e.df == 0) continue;
        Lista L;
        L.ps = leerPostings(e);
        if (L.ps.empty()) continue;
        const double df_t = static_cast<double>(e.df);
        if (r == Ranking::BM25) {
            // IDF de Robertson con el +1 de Lucene: sin el, un termino que
            // aparece en mas de la mitad del corpus daria IDF negativo y
            // restaria puntaje, que no es lo que uno espera de una coincidencia.
            L.idf = std::log(1.0 + (N - df_t + 0.5) / (df_t + 0.5));
        } else {
            L.idf = std::log(N / df_t);
            L.wq  = (1.0 + std::log(static_cast<double>(kv.second))) * L.idf;
        }
        listas.push_back(std::move(L));
    }
    if (listas.empty()) return salida;

    double norma_consulta = 0.0;
    if (r == Ranking::TFIDF) {
        for (const Lista& L : listas) norma_consulta += L.wq * L.wq;
        norma_consulta = std::sqrt(norma_consulta);
        if (norma_consulta <= 0.0) norma_consulta = 1.0;
    }

    // Monticulo de minimos: en la cima esta el PEOR de los k mejores, que es
    // justo el que hay que desalojar cuando aparece uno mejor.
    auto peor_arriba = [](const Resultado& a, const Resultado& b) { return a.score > b.score; };
    std::vector<Resultado> mejores;
    if (k > 0) mejores.reserve(static_cast<std::size_t>(k) + 1);

    while (true) {
        std::uint32_t actual = UINT32_MAX;
        for (const Lista& L : listas)
            if (L.i < L.ps.size() && L.ps[L.i].doc < actual) actual = L.ps[L.i].doc;
        if (actual == UINT32_MAX) break;

        RID rid;
        std::uint32_t largo = 0;
        float norma_doc = 1.0f;
        if (!leerDoc(actual, &rid, &largo, &norma_doc)) {
            for (Lista& L : listas)
                if (L.i < L.ps.size() && L.ps[L.i].doc == actual) ++L.i;
            continue;
        }

        double score = 0.0;
        for (Lista& L : listas) {
            if (L.i >= L.ps.size() || L.ps[L.i].doc != actual) continue;
            const double tf = static_cast<double>(L.ps[L.i].tf);
            if (r == Ranking::BM25) {
                const double dl = static_cast<double>(largo);
                const double norm = K1 * (1.0 - B + B * (avgdl_ > 0 ? dl / avgdl_ : 1.0));
                score += L.idf * (tf * (K1 + 1.0)) / (tf + norm);
            } else {
                const double wd = (1.0 + std::log(tf)) * L.idf;
                score += wd * L.wq;
            }
            ++L.i;
        }
        if (r == Ranking::TFIDF) {
            score /= (static_cast<double>(norma_doc) * norma_consulta);
            // La norma del documento se guarda en coma flotante de 32 bits: a
            // un ranking le sobra, pero el redondeo puede dejar el coseno en
            // 1.00000004. Por desigualdad de Cauchy-Schwarz no puede pasar de
            // uno, asi que se recorta en vez de mostrar un numero imposible.
            if (score > 1.0) score = 1.0;
        }
        if (score <= 0.0) continue;

        Resultado res;
        res.rid    = rid;
        res.doc_id = actual;
        res.score  = score;

        if (k < 0) {
            salida.push_back(res);
        } else if (static_cast<int>(mejores.size()) < k) {
            mejores.push_back(res);
            std::push_heap(mejores.begin(), mejores.end(), peor_arriba);
        } else if (k > 0 && res.score > mejores.front().score) {
            std::pop_heap(mejores.begin(), mejores.end(), peor_arriba);
            mejores.back() = res;
            std::push_heap(mejores.begin(), mejores.end(), peor_arriba);
        }
    }

    if (k >= 0) salida.swap(mejores);
    // Desempate por doc_id para que dos ejecuciones devuelvan lo mismo.
    std::sort(salida.begin(), salida.end(), [](const Resultado& a, const Resultado& b) {
        if (a.score != b.score) return a.score > b.score;
        return a.doc_id < b.doc_id;
    });
    return salida;
}

}  // namespace db
