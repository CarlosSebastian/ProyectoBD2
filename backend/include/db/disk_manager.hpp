// ============================================================================
//  disk_manager.hpp - Capa mas baja: el archivo visto como arreglo de paginas
//  Nadie fuera de esta clase toca el archivo directamente.
// ============================================================================
#pragma once

#include <fstream>
#include <string>

#include "db/common.hpp"
#include "db/disk_counter.hpp"

namespace db {

// Deja un archivo de datos en cero bytes (equivale a "empezar de nuevo").
// Se usa en tests y demos: es mas portable que borrar el archivo, porque no
// depende de permisos de borrado del sistema de archivos.
void resetFile(const std::string& path);

class DiskManager {
public:
    // Abre (o crea) el archivo de datos. Una instancia = un archivo.
    explicit DiskManager(const std::string& filename);
    ~DiskManager();

    DiskManager(const DiskManager&) = delete;
    DiskManager& operator=(const DiskManager&) = delete;

    void      readPage(page_id_t pid, char* dest);         // lee 4096 bytes
    void      writePage(page_id_t pid, const char* src);   // escribe 4096 bytes
    page_id_t allocatePage();                              // agrega pagina en cero al final
    void      truncate();                                  // deja el archivo en 0 paginas

    int numPages() const { return num_pages_; }
    const std::string& filename() const { return filename_; }

    // --- contadores de I/O: son la metrica de los experimentos del informe ---
    long long readCount()  const { return n_reads_; }
    long long writeCount() const { return n_writes_; }
    void      resetStats() { n_reads_ = 0; n_writes_ = 0; }

private:
    std::string  filename_;
    std::fstream file_;
    int          num_pages_ = 0;
    long long    n_reads_   = 0;
    long long    n_writes_  = 0;
};

}  // namespace db
