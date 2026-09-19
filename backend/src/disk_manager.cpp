#include "db/disk_manager.hpp"

#include <vector>

namespace db {

void resetFile(const std::string& path) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
}

DiskManager::DiskManager(const std::string& filename) : filename_(filename) {
    // Crear el archivo si aun no existe (fstream no lo crea en modo in|out).
    std::fstream probe(filename_, std::ios::in | std::ios::binary);
    if (!probe) {
        std::ofstream create(filename_, std::ios::binary);
        create.close();
    } else {
        probe.close();
    }

    file_.open(filename_, std::ios::in | std::ios::out | std::ios::binary);
    if (!file_.is_open()) throw DBException("No se pudo abrir el archivo: " + filename_);

    file_.seekg(0, std::ios::end);
    std::streamoff bytes = file_.tellg();
    num_pages_ = static_cast<int>(bytes / PAGE_SIZE);
}

DiskManager::~DiskManager() {
    if (file_.is_open()) {
        file_.flush();
        file_.close();
    }
}

void DiskManager::readPage(page_id_t pid, char* dest) {
    if (pid < 0 || pid >= num_pages_)
        throw DBException("readPage: pagina fuera de rango: " + std::to_string(pid));

    file_.clear();
    file_.seekg(static_cast<std::streamoff>(pid) * PAGE_SIZE, std::ios::beg);
    file_.read(dest, PAGE_SIZE);
    if (file_.gcount() < PAGE_SIZE) {
        // Archivo truncado: completamos con ceros en vez de fallar.
        std::memset(dest + file_.gcount(), 0, PAGE_SIZE - file_.gcount());
        file_.clear();
    }
    ++n_reads_;
    DiskCounter::global().countRead();
}

void DiskManager::writePage(page_id_t pid, const char* src) {
    if (pid < 0) throw DBException("writePage: page_id invalido");

    file_.clear();
    file_.seekp(static_cast<std::streamoff>(pid) * PAGE_SIZE, std::ios::beg);
    file_.write(src, PAGE_SIZE);
    file_.flush();
    if (pid >= num_pages_) num_pages_ = pid + 1;
    ++n_writes_;
    DiskCounter::global().countWrite();
}

void DiskManager::truncate() {
    // Reabrir en modo trunc es la forma portable de dejar el archivo vacio:
    // no depende de permisos de borrado ni de APIs del sistema.
    if (file_.is_open()) { file_.flush(); file_.close(); }
    { std::ofstream vaciar(filename_, std::ios::binary | std::ios::trunc); }
    file_.open(filename_, std::ios::in | std::ios::out | std::ios::binary);
    if (!file_.is_open()) throw DBException("truncate: no se pudo reabrir " + filename_);
    num_pages_ = 0;
}

page_id_t DiskManager::allocatePage() {
    std::vector<char> zeros(PAGE_SIZE, 0);
    page_id_t pid = num_pages_;
    writePage(pid, zeros.data());
    return pid;
}

}  // namespace db
