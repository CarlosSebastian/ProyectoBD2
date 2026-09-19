// ============================================================================
//  storage_engine.hpp - Interfaz comun de los metodos de organizacion de
//  archivos (Heap File y Sequential File).
//
//  Es lo que permite que `CREATE TABLE ... USING [HEAP|SEQUENTIAL]` cambie la
//  organizacion fisica sin que la capa de arriba (Table, el planificador, la
//  API) tenga que ramificar en cada operacion.
//
//  Las operaciones propias de cada motor (reorganize del Sequential, o el
//  informe de espacio libre del Heap) NO viven aqui: se piden por
//  dynamic_cast cuando de verdad hacen falta.
// ============================================================================
#pragma once

#include <string>
#include <vector>

#include "db/common.hpp"

namespace db {

class StorageEngine {
public:
    virtual ~StorageEngine() = default;

    virtual RID              insert(const std::string& payload)          = 0;
    virtual bool             get(const RID& rid, std::string& out) const = 0;
    virtual bool             erase(const RID& rid)                       = 0;
    virtual std::vector<RID> scanAll() const                             = 0;
    virtual std::size_t      count() const                               = 0;
    virtual int              numPages() const                            = 0;
    virtual const char*      engineName() const                          = 0;
};

}  // namespace db
