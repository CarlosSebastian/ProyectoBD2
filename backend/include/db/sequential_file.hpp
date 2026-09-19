// ============================================================================
//  sequential_file.hpp - Sequential File: area principal ordenada + overflow
//
//  ESTRUCTURA
//
//    Archivo principal (.dat)   paginas FISICAMENTE ORDENADAS por la clave:
//                               todas las claves de la pagina P son menores
//                               que las de la pagina P+1. Eso es lo que hace
//                               posible la BUSQUEDA BINARIA sobre paginas.
//
//    Archivo de overflow (.ovf) tuplas que ya no entraban en su bloque
//                               principal. Cada pagina principal tiene su
//                               propia cadena, enlazada POR CLAVE mediante
//                               punteros ⟨page_id, slot⟩ guardados al inicio
//                               de cada registro de overflow:
//
//                                   [next_page(4)][next_slot(4)][payload]
//
//                               La cabeza de la cadena vive en el campo aux
//                               de la cabecera de la pagina principal.
//
//  COSTO DE BUSQUEDA
//    log2(P) lecturas de pagina para ubicar el bloque, + 1..k para recorrer su
//    cadena de overflow. Cuanto mas crece el overflow, mas se degrada: por eso
//    existe reorganize().
//
//  ORDEN DENTRO DE LA PAGINA
//    Los slots son APPEND-ONLY: un insert nunca desplaza entradas del
//    directorio, de modo que los RID ya entregados a un indice secundario
//    siguen siendo validos. El orden por clave dentro de una pagina se calcula
//    en RAM al leerla (son ~200 registros, no cuesta I/O). El invariante que
//    importa —que las paginas particionan el espacio de claves en orden— se
//    mantiene porque un registro solo se inserta en la pagina cuyo rango le
//    corresponde, nunca en "la primera con espacio".
//
//  REORGANIZE
//    Recorre en orden las dos areas, fusiona, y reescribe un archivo principal
//    limpio con factor de carga configurable (70-80% por defecto), dejando el
//    overflow vacio. Cambia TODOS los RID, asi que quien tenga indices sobre
//    la tabla debe reconstruirlos despues (Table lo hace).
//
//  POR QUE TIENE QUE SER PERIODICO
//    Si solo se insertara, el archivo degenera: partiendo de un archivo vacio
//    la primera pagina se llena y TODO lo demas va a parar a su cadena de
//    overflow, con lo que la busqueda pasa de log2(P) a O(N) y el area
//    principal nunca crece. Por eso necesitaReorganizacion() avisa cuando el
//    overflow supera un umbral (20% de las filas por defecto) y el dueno de la
//    tabla dispara la reorganizacion. Es exactamente el "proceso periodico de
//    mantenimiento" del enunciado, y es lo que se mide en el experimento de
//    insercion masiva "con y sin reorganizacion".
// ============================================================================
#pragma once

#include <algorithm>
#include <string>
#include <vector>

#include "db/buffer_pool.hpp"
#include "db/common.hpp"
#include "db/page.hpp"
#include "db/record.hpp"
#include "db/storage_engine.hpp"

namespace db {

class SequentialFile : public StorageEngine {
public:
    // Los RID del area de overflow se marcan con un bit alto del page_id, para
    // que un mismo RID pueda referirse a cualquiera de los dos archivos.
    static constexpr page_id_t OVF_FLAG   = 0x40000000;
    static constexpr int       OVF_PREFIX = 8;      // [next_page][next_slot]

    static bool      esOverflow(const RID& r) { return (r.page_id & OVF_FLAG) != 0; }
    static RID       ridOvf(page_id_t p, int s) { return RID(p | OVF_FLAG, s); }
    static page_id_t pagOvf(const RID& r)     { return r.page_id & ~OVF_FLAG; }

    static constexpr int MAX_RECORD = SlotDir::MAX_RECORD - OVF_PREFIX;

    SequentialFile(BufferPool* main_bp, BufferPool* ovf_bp, Schema esquema, int key_col,
                   double fill_factor = 0.75, double umbral_overflow = 0.20)
        : main_(main_bp), ovf_(ovf_bp), schema_(std::move(esquema)),
          key_col_(key_col), fill_(fill_factor), umbral_(umbral_overflow) {
        if (key_col_ < 0 || key_col_ >= static_cast<int>(schema_.size()))
            throw DBException("SequentialFile: columna clave invalida");
        if (fill_ < 0.1 || fill_ > 1.0) fill_ = 0.75;
        if (umbral_ <= 0.0 || umbral_ > 1.0) umbral_ = 0.20;
        reconstruirMapaOvf();
        recontar();
    }

    // ----------------------------------------------------------- StorageEngine
    RID insert(const std::string& payload) override {
        int L = static_cast<int>(payload.size());
        if (L <= 0)         throw DBException("insert: registro vacio");
        if (L > MAX_RECORD) throw DBException("insert: el registro no cabe en una pagina");

        Value k = claveDe(payload.data(), L);

        if (main_->disk()->numPages() == 0) {          // archivo vacio: primera pagina
            page_id_t pid;
            char* b = main_->newPage(&pid);
            PageHeader::init(b, pid, PageHeader::FLAG_SEQ_MAIN);
            main_->unpinPage(pid, true);
        }

        page_id_t destino = buscarPagina(k);
        RID rid;
        if (anexarEnPagina(main_, destino, payload, &rid, /*es_ovf=*/false)) {
            ++n_main_;
            return rid;
        }
        RID r = insertarEnOverflow(destino, payload, k);   // el bloque ya no admite mas
        ++n_ovf_;
        return r;
    }

    bool get(const RID& rid, std::string& out) const override {
        if (!rid.valid()) return false;
        if (esOverflow(rid)) {
            page_id_t p = pagOvf(rid);
            if (p >= ovf_->disk()->numPages()) return false;
            const char* b = ovf_->fetchPage(p);
            bool ok = false;
            if (rid.slot < PageHeader::slots(b) && SlotDir::vivo(b, rid.slot)) {
                int off = SlotDir::offset(b, rid.slot);
                int len = SlotDir::length(b, rid.slot);
                out.assign(b + off + OVF_PREFIX, b + off + len);
                ok = true;
            }
            ovf_->unpinPage(p, false);
            return ok;
        }
        if (rid.page_id >= main_->disk()->numPages()) return false;
        const char* b = main_->fetchPage(rid.page_id);
        bool ok = false;
        if (rid.slot < PageHeader::slots(b) && SlotDir::vivo(b, rid.slot)) {
            int off = SlotDir::offset(b, rid.slot);
            int len = SlotDir::length(b, rid.slot);
            out.assign(b + off, b + off + len);
            ok = true;
        }
        main_->unpinPage(rid.page_id, false);
        return ok;
    }

    bool erase(const RID& rid) override {
        if (!rid.valid()) return false;
        if (!esOverflow(rid)) {
            if (rid.page_id >= main_->disk()->numPages()) return false;
            bool ok = borrarSlot(main_, rid.page_id, rid.slot, /*es_ovf=*/false);
            if (ok) --n_main_;
            return ok;
        }

        // Overflow: hay que desenlazarlo de su cadena antes de matarlo. Se
        // ubica la pagina principal duena por la clave y se camina la cadena
        // hasta encontrar al predecesor.
        std::string payload;
        if (!get(rid, payload)) return false;
        Value k = claveDe(payload.data(), static_cast<int>(payload.size()));
        page_id_t duena = buscarPagina(k);

        RID anterior;
        RID cur = cabezaCadena(duena);
        while (cur.valid() && !(cur == rid)) {
            anterior = cur;
            cur = siguienteEnCadena(cur);
        }
        if (!cur.valid()) return false;                 // no estaba en esa cadena

        RID despues = siguienteEnCadena(rid);
        if (anterior.valid()) fijarSiguiente(anterior, despues);
        else                  fijarCabezaCadena(duena, despues);

        bool ok = borrarSlot(ovf_, pagOvf(rid), rid.slot, /*es_ovf=*/true);
        if (ok) --n_ovf_;
        return ok;
    }

    // Devuelve los RID en ORDEN DE CLAVE, fusionando area principal y overflow.
    std::vector<RID> scanAll() const override {
        std::vector<RID> salida;
        int P = main_->disk()->numPages();
        for (page_id_t p = 0; p < P; ++p) {
            std::vector<Entrada> e = entradasDePagina(p);
            for (const Entrada& x : e) salida.push_back(x.rid);
        }
        return salida;
    }

    std::size_t count() const override { return static_cast<std::size_t>(n_main_ + n_ovf_); }

    int         numPages() const override { return main_->disk()->numPages() + ovf_->disk()->numPages(); }
    const char* engineName() const override { return "SEQUENTIAL"; }

    // ------------------------------------------------- propias de este motor
    std::vector<RID> searchEq(const Value& k) const {
        std::vector<RID> salida;
        if (main_->disk()->numPages() == 0) return salida;
        // reorganize() empaqueta por factor de carga sin respetar los limites
        // entre claves iguales, asi que una tirada de duplicados puede quedar
        // repartida entre la pagina P y la P-1. Mirar una sola pagina perderia
        // silenciosamente parte de las filas: hay que retroceder mientras la
        // pagina anterior siga conteniendo la clave.
        page_id_t p = buscarPagina(k);
        for (page_id_t q = p; q >= 0; --q) {
            const std::vector<Entrada> e = entradasDePagina(q);   // ya viene ordenada
            bool alguno = false, desde_el_borde = false;
            for (std::size_t i = 0; i < e.size(); ++i)
                if (e[i].clave == k) {
                    salida.push_back(e[i].rid);
                    alguno = true;
                    if (i == 0) desde_el_borde = true;
                }
            if (!alguno) break;
            // Si la tirada no empieza en la PRIMERA entrada de la pagina, hay
            // una clave menor delante: la tirada empieza aqui y no hace falta
            // mirar la pagina anterior. Con claves unicas esto corta siempre en
            // la primera vuelta, asi que el caso comun no paga nada.
            if (!desde_el_borde) break;
        }
        return salida;
    }

    std::vector<RID> searchRange(const Value& lo, const Value& hi) const {
        std::vector<RID> salida;
        int P = main_->disk()->numPages();
        if (P == 0) return salida;
        // Igual que en searchEq: la cota inferior puede tener duplicados en la
        // pagina anterior, asi que se retrocede hasta la primera que no aporte.
        page_id_t inicio = buscarPagina(lo);
        while (inicio > 0) {
            // Mismo criterio que searchEq: solo se retrocede si la pagina
            // actual arranca ya dentro del rango, porque entonces la cota
            // inferior pudo quedar repartida con la pagina anterior.
            const std::vector<Entrada> e = entradasDePagina(inicio);
            if (e.empty() || e.front().clave < lo) break;
            bool aporta = false;
            for (const Entrada& x : entradasDePagina(inicio - 1))
                if (!(x.clave < lo) && !(hi < x.clave)) { aporta = true; break; }
            if (!aporta) break;
            --inicio;
        }
        for (page_id_t p = inicio; p < P; ++p) {
            bool paso_el_tope = false;
            for (const Entrada& x : entradasDePagina(p)) {
                if (x.clave < lo) continue;
                if (hi < x.clave) { paso_el_tope = true; break; }
                salida.push_back(x.rid);
            }
            if (paso_el_tope) break;
        }
        return salida;
    }

    // Fusiona ambas areas y reescribe el archivo principal al factor de carga.
    void reorganize(double fill_factor = -1.0) {
        double f = (fill_factor > 0.0) ? fill_factor : fill_;
        if (f < 0.1 || f > 1.0) f = 0.75;

        // 1) recolectar TODO en orden de clave
        std::vector<std::string> ordenados;
        int P = main_->disk()->numPages();
        for (page_id_t p = 0; p < P; ++p) {
            for (const Entrada& x : entradasDePagina(p)) {
                std::string s;
                if (get(x.rid, s)) ordenados.push_back(std::move(s));
            }
        }

        // 2) reescribir el area principal desde cero, encadenando las paginas
        main_->invalidateAll();
        main_->disk()->truncate();
        ovf_->invalidateAll();
        ovf_->disk()->truncate();
        reconstruirMapaOvf();

        const int utilizable = PAGE_SIZE - PageHeader::SIZE;
        const int tope       = std::max(64, static_cast<int>(utilizable * f));

        page_id_t actual   = INVALID_PAGE_ID;
        page_id_t anterior = INVALID_PAGE_ID;
        int usado = 0;

        for (const std::string& payload : ordenados) {
            int necesita = static_cast<int>(payload.size()) + SlotDir::SLOT_SIZE;
            bool nueva = (actual == INVALID_PAGE_ID) || (usado + necesita > tope);
            if (nueva) {
                page_id_t pid;
                char* b = main_->newPage(&pid);
                PageHeader::init(b, pid, PageHeader::FLAG_SEQ_MAIN);
                PageHeader::setPrev(b, anterior);
                main_->unpinPage(pid, true);
                if (anterior != INVALID_PAGE_ID) {
                    char* ab = main_->fetchPage(anterior);
                    PageHeader::setNext(ab, pid);
                    main_->unpinPage(anterior, true);
                }
                anterior = pid;
                actual   = pid;
                usado    = 0;
            }
            RID rid;
            if (!anexarEnPagina(main_, actual, payload, &rid, false))
                throw DBException("reorganize: el registro no entro en una pagina recien creada");
            usado += necesita;
        }
        main_->flushAll();
        n_main_ = static_cast<long long>(ordenados.size());
        n_ovf_  = 0;
        ++n_reorg_;
    }

    // ------------------------------------------------------------- metricas
    int  mainPages() const { return main_->disk()->numPages(); }
    int  ovfPages()  const { return ovf_->disk()->numPages(); }
    long long ovfRecords() const { return n_ovf_; }

    // Avisa cuando el area de overflow crecio tanto que la busqueda binaria
    // deja de valer la pena. El dueno de la tabla es quien decide reorganizar,
    // porque es quien tiene que reconstruir los indices despues.
    bool necesitaReorganizacion() const {
        long long total = n_main_ + n_ovf_;
        return total > 0 && static_cast<double>(n_ovf_) > umbral_ * static_cast<double>(total);
    }
    double umbralOverflow() const { return umbral_; }
    long long reorganizaciones() const { return n_reorg_; }
    double fillFactor() const { return fill_; }
    // Paginas leidas por la ultima busqueda binaria: es el log2(P) del analisis.
    int  ultimaBusquedaPaginas() const { return sondeos_; }

private:
    struct Entrada { Value clave; RID rid; };

    Value claveDe(const char* data, int len) const {
        return extractColumn(schema_, key_col_, data, len);
    }

    static std::size_t registrosEn(BufferPool* bp) {
        std::size_t total = 0;
        int n = bp->disk()->numPages();
        for (int p = 0; p < n; ++p) {
            const char* b = bp->fetchPage(p);
            total += static_cast<std::size_t>(PageHeader::records(b));
            bp->unpinPage(p, false);
        }
        return total;
    }

    // --- cadena de overflow ---
    RID cabezaCadena(page_id_t principal) const {
        const char* b = main_->fetchPage(principal);
        RID r = PageHeader::auxRID(b);
        main_->unpinPage(principal, false);
        return r;
    }
    void fijarCabezaCadena(page_id_t principal, const RID& r) {
        char* b = main_->fetchPage(principal);
        PageHeader::setAuxRID(b, r);
        main_->unpinPage(principal, true);
    }
    RID siguienteEnCadena(const RID& r) const {
        page_id_t p = pagOvf(r);
        const char* b = ovf_->fetchPage(p);
        RID sig;
        if (r.slot < PageHeader::slots(b) && SlotDir::vivo(b, r.slot)) {
            int off = SlotDir::offset(b, r.slot);
            sig = RID(readAt<page_id_t>(b, off), readAt<std::int32_t>(b, off + 4));
        }
        ovf_->unpinPage(p, false);
        return sig;
    }
    void fijarSiguiente(const RID& r, const RID& sig) {
        page_id_t p = pagOvf(r);
        char* b = ovf_->fetchPage(p);
        if (r.slot < PageHeader::slots(b) && SlotDir::vivo(b, r.slot)) {
            int off = SlotDir::offset(b, r.slot);
            writeAt<page_id_t>(b, off, sig.page_id);
            writeAt<std::int32_t>(b, off + 4, sig.slot);
        }
        ovf_->unpinPage(p, true);
    }

    // --- lectura ordenada de una pagina principal + su cadena ---
    std::vector<Entrada> entradasDePagina(page_id_t p) const {
        std::vector<Entrada> res;
        const char* b = main_->fetchPage(p);
        int n = PageHeader::slots(b);
        for (int s = 0; s < n; ++s) {
            if (!SlotDir::vivo(b, s)) continue;
            int off = SlotDir::offset(b, s);
            int len = SlotDir::length(b, s);
            res.push_back(Entrada{claveDe(b + off, len), RID(p, s)});
        }
        RID cur = PageHeader::auxRID(b);
        main_->unpinPage(p, false);

        while (cur.valid()) {
            page_id_t op = pagOvf(cur);
            const char* ob = ovf_->fetchPage(op);
            RID sig;
            if (cur.slot < PageHeader::slots(ob) && SlotDir::vivo(ob, cur.slot)) {
                int off = SlotDir::offset(ob, cur.slot);
                int len = SlotDir::length(ob, cur.slot);
                res.push_back(Entrada{claveDe(ob + off + OVF_PREFIX, len - OVF_PREFIX), cur});
                sig = RID(readAt<page_id_t>(ob, off), readAt<std::int32_t>(ob, off + 4));
            }
            ovf_->unpinPage(op, false);
            cur = sig;
        }

        // Orden por clave en RAM: la pagina es chica y esto no cuesta I/O.
        std::stable_sort(res.begin(), res.end(),
                         [](const Entrada& a, const Entrada& c) { return a.clave < c.clave; });
        return res;
    }

    // Menor clave viva de una pagina principal (sin mirar su overflow).
    bool minimaClave(page_id_t p, Value* out) const {
        const char* b = main_->fetchPage(p);
        int n = PageHeader::slots(b);
        bool hay = false;
        Value mejor;
        for (int s = 0; s < n; ++s) {
            if (!SlotDir::vivo(b, s)) continue;
            Value k = claveDe(b + SlotDir::offset(b, s), SlotDir::length(b, s));
            if (!hay || k < mejor) { mejor = k; hay = true; }
        }
        main_->unpinPage(p, false);
        if (hay) *out = mejor;
        return hay;
    }

    // BUSQUEDA BINARIA sobre las paginas del area principal: devuelve la ultima
    // pagina cuya clave minima es <= k, que es la que cubre el rango de k.
    page_id_t buscarPagina(const Value& k) const {
        int P = main_->disk()->numPages();
        sondeos_ = 0;
        if (P <= 1) return 0;

        int lo = 0, hi = P - 1, ans = 0;
        while (lo <= hi) {
            int mid = lo + (hi - lo) / 2;
            Value minima;
            bool hay = minimaClave(mid, &minima);
            ++sondeos_;
            if (!hay) {
                // Pagina sin registros vivos (un DELETE la vacio). NO aporta
                // cota, asi que tampoco puede ser la respuesta: quedarsela
                // descartaria la pagina buena que quedo a su izquierda y
                // romperia el orden del archivo en los INSERT siguientes.
                // Se busca la vecina viva mas cercana para poder decidir.
                int izq = mid - 1;
                while (izq >= lo && !minimaClave(izq, &minima)) --izq;
                if (izq >= lo) { ++sondeos_; if (!(k < minima)) { ans = izq; } }
                lo = mid + 1;                    // explorar hacia la derecha
                continue;
            }
            if (!(k < minima)) { ans = mid; lo = mid + 1; }
            else               { hi = mid - 1; }
        }
        return ans;
    }

    // --- escritura de slots (comun a ambas areas) ---
    bool anexarEnPagina(BufferPool* bp, page_id_t p, const std::string& payload,
                        RID* out, bool es_ovf) {
        int L = static_cast<int>(payload.size()) + (es_ovf ? OVF_PREFIX : 0);
        char* b = bp->fetchPage(p);
        if (SlotDir::espacioLibre(b) < L + SlotDir::SLOT_SIZE) {
            bp->unpinPage(p, false);
            return false;
        }
        int n = PageHeader::slots(b);
        int slot = -1;
        for (int i = 0; i < n; ++i)
            if (!SlotDir::vivo(b, i)) { slot = i; break; }     // reciclar tumba
        if (slot < 0) { slot = n; PageHeader::setSlots(b, n + 1); }

        int off = PageHeader::freeOff(b) - L;
        if (es_ovf) {
            writeAt<page_id_t>(b, off, INVALID_PAGE_ID);
            writeAt<std::int32_t>(b, off + 4, -1);
            std::memcpy(b + off + OVF_PREFIX, payload.data(), payload.size());
        } else {
            std::memcpy(b + off, payload.data(), payload.size());
        }
        SlotDir::set(b, slot, off, L);
        PageHeader::setFreeOff(b, off);
        PageHeader::setRecords(b, PageHeader::records(b) + 1);
        bp->unpinPage(p, true);

        *out = es_ovf ? ridOvf(p, slot) : RID(p, slot);
        return true;
    }

    bool borrarSlot(BufferPool* bp, page_id_t p, int slot, bool es_ovf) {
        (void)es_ovf;
        char* b = bp->fetchPage(p);
        int n    = PageHeader::slots(b);
        int fptr = PageHeader::freeOff(b);
        if (slot >= n || !SlotDir::vivo(b, slot)) { bp->unpinPage(p, false); return false; }

        int off = SlotDir::offset(b, slot);
        int len = SlotDir::length(b, slot);
        std::memmove(b + fptr + len, b + fptr, off - fptr);
        for (int i = 0; i < n; ++i) {
            int o = SlotDir::offset(b, i);
            if (o != 0 && o < off) SlotDir::set(b, i, o + len, SlotDir::length(b, i));
        }
        SlotDir::matar(b, slot);
        PageHeader::setFreeOff(b, fptr + len);
        PageHeader::setRecords(b, PageHeader::records(b) - 1);
        bp->unpinPage(p, true);
        if (p < static_cast<int>(ovf_libre_.size())) actualizarLibreOvf(p);
        return true;
    }

    // --- overflow: mapa de espacio libre e insercion ordenada en la cadena ---
    void recontar() {
        n_main_ = static_cast<long long>(registrosEn(main_));
        n_ovf_  = static_cast<long long>(registrosEn(ovf_));
    }

    void reconstruirMapaOvf() {
        int n = ovf_->disk()->numPages();
        ovf_libre_.assign(n, 0);
        for (int p = 0; p < n; ++p) actualizarLibreOvf(p);
    }
    void actualizarLibreOvf(page_id_t p) {
        if (p >= static_cast<int>(ovf_libre_.size())) ovf_libre_.resize(p + 1, 0);
        const char* b = ovf_->fetchPage(p);
        ovf_libre_[p] = static_cast<std::uint16_t>(std::max(0, SlotDir::espacioLibre(b)));
        ovf_->unpinPage(p, false);
    }

    RID insertarEnOverflow(page_id_t principal, const std::string& payload, const Value& k) {
        // 1) ubicar la posicion en la cadena que mantiene el orden por clave
        RID anterior;
        RID cur = cabezaCadena(principal);
        while (cur.valid()) {
            std::string s;
            if (!get(cur, s)) break;
            if (k < claveDe(s.data(), static_cast<int>(s.size()))) break;
            anterior = cur;
            cur      = siguienteEnCadena(cur);
        }

        // 2) escribir el registro en alguna pagina de overflow con sitio
        int necesita = static_cast<int>(payload.size()) + OVF_PREFIX + SlotDir::SLOT_SIZE;
        page_id_t destino = INVALID_PAGE_ID;
        for (std::size_t p = 0; p < ovf_libre_.size(); ++p)
            if (ovf_libre_[p] >= necesita) { destino = static_cast<page_id_t>(p); break; }

        if (destino == INVALID_PAGE_ID) {
            char* b = ovf_->newPage(&destino);
            PageHeader::init(b, destino, PageHeader::FLAG_SEQ_OVF);
            ovf_->unpinPage(destino, true);
            if (destino >= static_cast<int>(ovf_libre_.size())) ovf_libre_.resize(destino + 1, 0);
        }

        RID nuevo;
        if (!anexarEnPagina(ovf_, destino, payload, &nuevo, /*es_ovf=*/true))
            throw DBException("overflow: no se pudo escribir el registro");
        actualizarLibreOvf(destino);

        // 3) enlazarlo en su sitio
        fijarSiguiente(nuevo, cur);
        if (anterior.valid()) fijarSiguiente(anterior, nuevo);
        else                  fijarCabezaCadena(principal, nuevo);
        return nuevo;
    }

    BufferPool* main_;
    BufferPool* ovf_;
    Schema      schema_;
    int         key_col_;
    double      fill_;
    double      umbral_;
    long long   n_main_  = 0;
    long long   n_ovf_   = 0;
    long long   n_reorg_ = 0;
    std::vector<std::uint16_t> ovf_libre_;
    mutable int sondeos_ = 0;
};

}  // namespace db
