# Mini-Gestor de Bases de Datos Multimodal — CS2042 (UTEC) 2026-II

Motor de base de datos implementado **desde cero** en C++17: almacenamiento
paginado en disco, índices B+ Tree y Hash Extensible, parser SQL, planificador
con telemetría de I/O, API REST y cliente web.

Sin motores de BD externos, sin ORMs y sin serializadores de alto nivel: todo
el acceso a disco es `seek` + lectura/escritura binaria de bloques de tamaño
fijo.

---

## Levantar el sistema en un paso

```bash
make run
```

Compila todo y sirve el cliente en **http://localhost:8080**.

Opciones útiles:

```bash
./benchmarks/run_experimentos.sh   # los 4 experimentos -> benchmarks/resultados.md
python3 benchmarks/graficas.py     # regenera las figuras del informe
cd informe && latexmk -pdf informe.tex   # recompila el informe
make run PORT=9000                 # otro puerto
./build/dbserver --pool 8          # buffer pool pequeño: hace visible el I/O físico
make test                          # 6 suites de tests (4358 verificaciones)
make bench                         # corre los cuatro experimentos
make demo                          # recorrido end-to-end por consola
make PAGE=8192 bench               # Experimento 4: variar el tamaño de bloque

# Inspeccionar los binarios por dentro (lo que pide el video):
./build/dump_page data/empleados.dat 0        # cabecera + directorio de slots
./build/dump_page data/empleados.dat --resumen  # una línea por página
./build/dump_page data/empleados_id.idx 1     # nodo del índice B+ o bucket del hash
./build/dump_page data/empleados.dat 0 --hex  # los 4096 bytes completos
```

Requiere `g++` con C++17. **Si no tienes `make`** (por ejemplo en Git Bash),
hay un script que hace lo mismo y solo necesita el compilador:

```bash
./build.sh test        # Git Bash, MSYS2, WSL, Linux, macOS
```

Acepta los mismos objetivos que el Makefile (`all`, `test`, `demo`, `bench`,
`run`, `clean`) y las mismas opciones: `PAGE=8192 ./build.sh bench`,
`POOL=8 ./build.sh run`. En Windows el enlace con Winsock se añade solo.

---

## Estructura

```
backend/
  include/db/      common, disk_manager, disk_counter, buffer_pool, record,
                   heap_file, sequential_file, bplus_tree, extendible_hash,
                   page, storage_engine, catalog, table,
                   sql, database, json
  include/third_party/httplib.h    servidor HTTP (cpp-httplib, MIT)
  src/             implementaciones + api_server.cpp + demo.cpp + dump_page.cpp
  tests/           test_heap, test_bplus, test_hash, test_sequential,
                   test_catalog, test_sql
frontend/
  index.html       cliente SQL de 4 paneles (sin dependencias)
benchmarks/
  gen_dataset.cpp        generador del dataset sintético
  exp1..exp4_*.cpp       los cuatro experimentos del enunciado
  graficas.py            genera las figuras del informe
  run_experimentos.sh    corre los cuatro y escribe resultados.md
  resultados.md          salida cruda de la última corrida
data/              archivos binarios generados (ignorados por git)
                   .gitkeep documenta que hay dentro
```

---

## SQL soportado

```sql
CREATE TABLE empleados (id INT PRIMARY KEY, nombre CHAR(30),
                        dept CHAR(20), salario FLOAT) USING [HEAP|SEQUENTIAL];
CREATE INDEX idx_emp_id ON empleados (id) USING [BTREE|HASH];
INSERT INTO empleados VALUES (101, 'Ada Lovelace', 'Analytics', 5200.0);
SELECT * FROM empleados WHERE id = 101;
SELECT id, nombre FROM empleados WHERE id >= 100 AND id <= 500 LIMIT 50;
SELECT * FROM empleados WHERE id BETWEEN 100 AND 500;
DELETE FROM empleados WHERE id = 101;
```

Con `USING SEQUENTIAL` el archivo queda **ordenado físicamente por la PRIMARY
KEY**: las búsquedas sobre esa columna se resuelven con búsqueda binaria sobre
páginas (log₂ P transferencias) aunque no haya ningún índice creado. Las filas
que ya no entran en su bloque van a un archivo de overflow (`.ovf`), encadenado
por clave, y el motor reorganiza solo cuando el overflow supera el 20 % de las
filas.

## API REST

| Método | Ruta | Cuerpo | Respuesta |
|---|---|---|---|
| POST | `/api/query` | `{"sql": "..."}` | tuplas + plan + I/O + tiempos |
| GET | `/api/tables` | — | tablas, columnas, índices, páginas |
| POST | `/api/tables/reorganize` | `{"table": "..."}` | fusiona principal + overflow y reconstruye índices |
| GET | `/api/health` | — | tamaño de página, pool, I/O acumulado |

## Métricas por consulta

Cada respuesta trae el desglose que exige el enunciado más dos métricas
adicionales que hacen el análisis legible:

- `disk_reads` / `disk_writes` — transferencias **físicas** de bloque (DiskCounter)
- `page_accesses` — accesos **lógicos** a página vía buffer pool
- `buffer_hits` — cuántos de esos se sirvieron desde RAM
- `parse_ms`, `exec_ms`, `total_ms` y el plan desglosado por etapa

La distinción importa: con un buffer pool holgado una tabla pequeña se cachea
entera y `disk_reads` cae a cero, pero `page_accesses` sigue mostrando la
diferencia real entre `IndexScan` y `SeqScan`. Es la misma separación que hace
`EXPLAIN (BUFFERS)` de PostgreSQL entre *shared hit* y *read*.

Ejemplo medido con 3 000 filas y un pool de 8 páginas:

| Consulta | Método | Accesos a página | Lecturas físicas |
|---|---|---|---|
| `WHERE id = 1500` (indexada) | INDEX BPLUS | 5 | 2 |
| `WHERE dept = 'Dept3'` (sin índice) | SEQ SCAN | 3 030 | 60 |

## Rutas de acceso

El planificador elige entre tres, en este orden:

1. **IndexScan / IndexRangeScan** — hay un índice B+ o Hash sobre la columna.
2. **Búsqueda binaria secuencial** — el motor es `SEQUENTIAL` y la columna es la
   que ordena el archivo. No necesita índice.
3. **SeqScan** — todo lo demás. También es donde cae un rango sobre un índice
   Hash, que no tiene orden.

## Estructura de una página (4 KB)

```
[ PageHeader 32 B ][ directorio de slots -> ][ libre ][ <- registros ]

PageHeader: page_id | next_page_id | prev_page_id | record_count
            slot_count | free_space_offset | flags | aux (overflow)
```

Los slots son *append-only*: un insert nunca desplaza entradas del directorio,
así que los RID que ya recibió un índice siguen siendo válidos. Al borrar se
compacta la página pero el índice del slot se conserva.

