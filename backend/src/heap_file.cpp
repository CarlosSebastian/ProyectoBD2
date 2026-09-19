#include "db/heap_file.hpp"

#include <algorithm>
#include <iostream>

namespace db {

HeapFile::HeapFile(BufferPool* bp) : bp_(bp) { rebuildFreeMap(); }

void HeapFile::rebuildFreeMap() {
    int n = bp_->disk()->numPages();
    free_map_.assign(n, 0);
    last_page_ = INVALID_PAGE_ID;

    for (int p = 0; p < n; ++p) {
        const char* buf = bp_->fetchPage(p);
        free_map_[p] = static_cast<std::uint16_t>(std::max(0, SlotDir::espacioLibre(buf)));
        if (PageHeader::next(buf) == INVALID_PAGE_ID) last_page_ = p;   // cola de la cadena
        bp_->unpinPage(p, false);
    }
    if (n > 0 && last_page_ == INVALID_PAGE_ID) last_page_ = n - 1;     // archivo sin encadenar
    cursor_ = 0;
}

int HeapFile::freeBytes(page_id_t pid) const {
    if (pid < 0 || pid >= static_cast<int>(free_map_.size())) return 0;
    return free_map_[pid];
}

RID HeapFile::insert(const std::string& payload) {
    int L = static_cast<int>(payload.size());
    if (L <= 0)         throw DBException("insert: registro vacio");
    if (L > MAX_RECORD) throw DBException("insert: el registro no cabe en una pagina de " +
                                          std::to_string(PAGE_SIZE) + " bytes");

    // 1) first-fit sobre el mapa en RAM, arrancando en el cursor: no cuesta I/O
    //    y evita recorrer todas las paginas en cada insercion.
    page_id_t destino = INVALID_PAGE_ID;
    const std::size_t P = free_map_.size();
    const int necesita = L + SlotDir::SLOT_SIZE;
    for (std::size_t i = 0; i < P; ++i) {
        std::size_t p = cursor_ + i;
        if (p >= P) p -= P;                      // vuelta circular
        if (free_map_[p] >= necesita) { destino = static_cast<page_id_t>(p); cursor_ = p; break; }
    }

    char* buf = nullptr;
    if (destino == INVALID_PAGE_ID) {
        // 2) no hay hueco: pagina nueva, enganchada al final de la cadena
        buf = bp_->newPage(&destino);
        PageHeader::init(buf, destino, PageHeader::FLAG_HEAP);
        PageHeader::setPrev(buf, last_page_);
        if (static_cast<int>(free_map_.size()) <= destino) free_map_.resize(destino + 1, 0);
        cursor_ = static_cast<std::size_t>(destino);

        if (last_page_ != INVALID_PAGE_ID) {
            char* anterior = bp_->fetchPage(last_page_);
            PageHeader::setNext(anterior, destino);
            bp_->unpinPage(last_page_, true);
        }
        last_page_ = destino;
    } else {
        buf = bp_->fetchPage(destino);
    }

    int n_slots = PageHeader::slots(buf);
    int fptr    = PageHeader::freeOff(buf);

    // Reutilizar una tumba si existe, para que el directorio no crezca sin fin.
    int slot = -1;
    for (int i = 0; i < n_slots; ++i)
        if (!SlotDir::vivo(buf, i)) { slot = i; break; }

    if (slot < 0) {
        if (SlotDir::espacioLibre(buf) < L + SlotDir::SLOT_SIZE) {
            bp_->unpinPage(destino, false);
            throw DBException("insert: inconsistencia en el mapa de espacio libre");
        }
        slot = n_slots;
        PageHeader::setSlots(buf, n_slots + 1);
    }

    int off = fptr - L;
    std::memcpy(buf + off, payload.data(), L);
    SlotDir::set(buf, slot, off, L);
    PageHeader::setFreeOff(buf, off);
    PageHeader::setRecords(buf, PageHeader::records(buf) + 1);

    free_map_[destino] = static_cast<std::uint16_t>(std::max(0, SlotDir::espacioLibre(buf)));
    bp_->unpinPage(destino, true);
    return RID(destino, slot);
}

bool HeapFile::get(const RID& rid, std::string& out) const {
    if (!rid.valid() || rid.page_id >= bp_->disk()->numPages()) return false;
    const char* buf = bp_->fetchPage(rid.page_id);
    if (rid.slot >= PageHeader::slots(buf)) { bp_->unpinPage(rid.page_id, false); return false; }
    if (!SlotDir::vivo(buf, rid.slot))      { bp_->unpinPage(rid.page_id, false); return false; }

    int off = SlotDir::offset(buf, rid.slot);
    int len = SlotDir::length(buf, rid.slot);
    out.assign(buf + off, buf + off + len);
    bp_->unpinPage(rid.page_id, false);
    return true;
}

bool HeapFile::erase(const RID& rid) {
    if (!rid.valid() || rid.page_id >= bp_->disk()->numPages()) return false;
    char* buf = bp_->fetchPage(rid.page_id);
    int n_slots = PageHeader::slots(buf);
    int fptr    = PageHeader::freeOff(buf);
    if (rid.slot >= n_slots || !SlotDir::vivo(buf, rid.slot)) {
        bp_->unpinPage(rid.page_id, false);
        return false;
    }

    int off = SlotDir::offset(buf, rid.slot);
    int len = SlotDir::length(buf, rid.slot);

    // COMPACTACION: se corre hacia la derecha todo lo que estaba a la izquierda
    // del registro borrado, y se ajustan los offsets afectados. Los indices de
    // slot no se tocan, asi que los RID vecinos siguen siendo validos.
    std::memmove(buf + fptr + len, buf + fptr, off - fptr);
    for (int i = 0; i < n_slots; ++i) {
        int o = SlotDir::offset(buf, i);
        if (o != 0 && o < off) SlotDir::set(buf, i, o + len, SlotDir::length(buf, i));
    }
    SlotDir::matar(buf, rid.slot);
    PageHeader::setFreeOff(buf, fptr + len);
    PageHeader::setRecords(buf, PageHeader::records(buf) - 1);

    free_map_[rid.page_id] = static_cast<std::uint16_t>(std::max(0, SlotDir::espacioLibre(buf)));
    // Que el cursor retroceda al hueco recien liberado para no desperdiciarlo.
    if (static_cast<std::size_t>(rid.page_id) < cursor_) cursor_ = static_cast<std::size_t>(rid.page_id);
    bp_->unpinPage(rid.page_id, true);
    return true;
}

RID HeapFile::update(const RID& rid, const std::string& payload) {
    std::string viejo;
    if (!get(rid, viejo)) return RID();

    if (viejo.size() == payload.size()) {          // cabe exacto: se pisa en el sitio
        char* buf = bp_->fetchPage(rid.page_id);
        int off = SlotDir::offset(buf, rid.slot);
        std::memcpy(buf + off, payload.data(), payload.size());
        bp_->unpinPage(rid.page_id, true);
        return rid;
    }
    erase(rid);
    return insert(payload);
}

std::vector<RID> HeapFile::scanAll() const {
    // Recorrido por la CADENA de paginas, no por el orden fisico del archivo.
    std::vector<RID> res;
    int total = bp_->disk()->numPages();
    if (total == 0) return res;

    page_id_t p = 0;
    int visitadas = 0;
    while (p != INVALID_PAGE_ID && visitadas <= total) {
        const char* buf = bp_->fetchPage(p);
        int n_slots = PageHeader::slots(buf);
        for (int s = 0; s < n_slots; ++s)
            if (SlotDir::vivo(buf, s)) res.push_back(RID(p, s));
        page_id_t siguiente = PageHeader::next(buf);
        bp_->unpinPage(p, false);
        p = siguiente;
        ++visitadas;
    }
    return res;
}

std::size_t HeapFile::count() const {
    std::size_t total = 0;
    int n = bp_->disk()->numPages();
    for (int p = 0; p < n; ++p) {
        const char* buf = bp_->fetchPage(p);
        total += static_cast<std::size_t>(PageHeader::records(buf));
        bp_->unpinPage(p, false);
    }
    return total;
}

void HeapFile::printPageInfo(page_id_t pid) const {
    const char* buf = bp_->fetchPage(pid);
    std::cout << "   pag " << PageHeader::pageId(buf)
              << ": registros=" << PageHeader::records(buf)
              << " slots=" << PageHeader::slots(buf)
              << " free_off=" << PageHeader::freeOff(buf)
              << " libre=" << SlotDir::espacioLibre(buf) << " B"
              << " prev=" << PageHeader::prev(buf)
              << " next=" << PageHeader::next(buf) << "\n";
    bp_->unpinPage(pid, false);
}

}  // namespace db
