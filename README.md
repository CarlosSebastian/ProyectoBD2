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
make run PORT=9000                 # otro puerto
./build/dbserver --pool 8          # buffer pool pequeño: hace visible el I/O físico
make test                          # 6 suites de tests (4365 verificaciones)
make bench                         # corre los cuatro experimentos
make demo                          # recorrido end-to-end por consola
make PAGE=8192 bench               # Experimento 4: variar el tamaño de bloque

# Inspeccionar los binarios por dentro (lo que pide el video):
# Los archivos se nombran según la tabla: CREATE TABLE ventas -> data/ventas.dat
./build/dump_page data/<tabla>.dat 0            # cabecera + directorio de slots
./build/dump_page data/<tabla>.dat --resumen    # una línea por página
./build/dump_page data/<tabla>_<columna>.idx 1  # nodo del B+ o bucket del hash
./build/dump_page data/<tabla>.dat 0 --hex      # los 4096 bytes completos
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
CREATE INDEX idx_emp_id ON empleados (id) USING [BTREE|HASH|RTREE];
INSERT INTO empleados VALUES (101, 'Ada Lovelace', 'Analytics', 5200.0);
INSERT INTO empleados VALUES (102, 'Alan Turing', 'IA', 6100.0),
                             (103, 'Grace Hopper', 'Compiladores', 5900.0);
SELECT * FROM empleados WHERE id = 101;
SELECT id, nombre FROM empleados WHERE id >= 100 AND id <= 500 LIMIT 50;
SELECT * FROM empleados WHERE id BETWEEN 100 AND 500;
DELETE FROM empleados WHERE id = 101;
```

Una misma petición admite **varias sentencias separadas por punto y coma**: el
parser devuelve la lista completa y el motor las ejecuta en orden, respondiendo
con un resultado por cada una más los contadores de I/O sumados. El lote **no es
atómico** (no hay gestor de transacciones): si una sentencia falla, las
anteriores ya están aplicadas y las siguientes se ejecutan igualmente, cada una
con su propio indicador de éxito.

`PRIMARY KEY` impone unicidad: antes de insertar se hace una búsqueda puntual
por la columna clave y la operación se rechaza si el valor ya existe.

### Módulo espacial (R-Tree)

Una columna `POINT` guarda una coordenada 2D de ancho fijo (dos `double`, 16
bytes). Se indexa con un **R-Tree en disco**: una página por nodo, 102 entradas
por hoja y 113 por nodo interno con páginas de 4 KB, `ChooseSubtree` por mínima
ampliación de área y split cuadrático de Guttman.

```sql
CREATE TABLE lugares (id INT PRIMARY KEY, nombre CHAR(40), ubic POINT);
INSERT INTO lugares VALUES (1, 'UTEC Barranco', POINT(-77.0220, -12.1350));
CREATE INDEX idx_ubic ON lugares (ubic) USING RTREE;

-- ventana: todo lo que cae dentro del rectángulo (minx, miny, maxx, maxy)
SELECT * FROM lugares WHERE ubic WITHIN (-77.2, -12.2, -76.9, -12.0);

-- k vecinos más cercanos; <-> es la distancia euclidiana
SELECT * FROM lugares ORDER BY ubic <-> POINT(-77.0300, -12.0460) LIMIT 3;
```

En `POINT(x, y)` la **x es la longitud** y la **y la latitud**, que es el orden
(este, norte) de la cartografía; el visor de mapa las invierte al dibujar
porque Leaflet pide `[lat, lon]`.

#### Distancia geográfica

`<->` es la distancia **euclidiana en grados**, que es lo correcto en un plano
cartesiano pero no mide nada en la Tierra: un grado de latitud son siempre
~111 km, pero uno de longitud mide 111 km en el ecuador y cero en el polo.
`ST_DISTANCE` devuelve **metros reales** (haversine):

```sql
-- radio: todo lo que esté a menos de 5 km
SELECT * FROM lugares WHERE ST_DISTANCE(ubic, POINT(-77.0300,-12.0460)) <= 5000;

-- KNN geográfico
SELECT * FROM lugares ORDER BY ST_DISTANCE(ubic, POINT(-77.0300,-12.0460)) LIMIT 5;
```

La diferencia no es cosmética. A 60° de latitud, un punto a 1,0° al este está a
55,6 km y uno a 0,9° al norte está a 100,1 km: en grados gana el segundo, en
metros el primero. `<->` devuelve el orden equivocado y `ST_DISTANCE` el
correcto; hay un test que fija exactamente ese caso.

#### Polígonos

Además de `POINT` hay `POLYGON`, un anillo de vértices de longitud variable:

```sql
CREATE TABLE zonas (id INT PRIMARY KEY, nombre CHAR(24), area POLYGON);
INSERT INTO zonas VALUES (1,'Centro', POLYGON((-77.05,-12.06),(-77.01,-12.06),(-77.01,-12.03)));
CREATE INDEX ix_area ON zonas (area) USING RTREE;

SELECT * FROM zonas WHERE ST_CONTAINS(area, POINT(-77.03,-12.05));
SELECT * FROM zonas WHERE area WITHIN (-77.1,-12.2,-77.0,-12.0);
```

Acá aparece algo que con puntos no se ve. El R-Tree indexa un polígono por su
**caja envolvente**, que es solo una aproximación: un triángulo ocupa la mitad
de su caja, y una diagonal fina ocupa casi nada de la suya. Por eso toda
consulta sobre polígonos tiene **dos pasos**, igual que en los motores
espaciales reales:

1. **Filtrado** — el índice devuelve los candidatos cuya caja interseca.
2. **Refinamiento** — se comprueba la geometría de verdad (punto en polígono
   por lanzamiento de rayo, o intersección polígono-rectángulo) y se descartan
   los falsos positivos.

El plan lo declara: `INDEX RTREE (contiene + refinamiento)`, con una línea
`Refinamiento geométrico` que dice cuántos candidatos del índice se cayeron al
mirar la geometría. En el test con 600 triángulos, el índice entrega 222
candidatos y el refinamiento deja 205.

Las consultas de distancia (`<->`, `ST_DISTANCE`) son solo para `POINT`: el
best-first supone que las hojas son puntos, y con polígonos esa suposición no
vale.

Las consultas de distancia las resuelve el R-Tree, y el resultado es **exacto**. Eso
exige que el MINDIST usado para podar sea una **cota inferior** de la distancia
real: si se pasara, el best-first descartaría un nodo que sí contenía un vecino
más cercano. La aproximación plana (`Δlon × metros_por_grado`) no sirve — se
pasa cuando la diferencia de longitud es grande, porque el plano estira lo que
la esfera acorta. La cota que usa el motor va por el **acorde en 3D**: se
construye una caja alineada a los ejes en el espacio que contiene al parche
esférico y se mide del punto de consulta a esa caja; como la caja contiene al
parche, esa distancia nunca supera el acorde real. Hay un test que lo verifica
por fuerza bruta sobre cajas al azar.

Una tabla admite **un índice por columna**, no uno en total: lo normal es tener
un B+ sobre la clave primaria y además un R-Tree sobre la columna `POINT`. Cada
índice vive en su propio archivo con su gestor de disco y su buffer pool, y la
tabla los mantiene coherentes: un `INSERT` entra en todos y un `DELETE` sale de
todos.

### Consultas híbridas

Un `WHERE` admite varias condiciones unidas por `AND` sobre **columnas
distintas**. Las que caen sobre la misma columna se fusionan en un solo rango
(`id >= 100 AND id <= 500` sigue llegando al B+ como un rango cerrado); las de
columnas distintas quedan como una lista, y el planificador elige **una** para
resolver el acceso y aplica el resto como filtro sobre las filas recuperadas.

```sql
SELECT * FROM lugares WHERE id > 100 AND ubic WITHIN (0,0,9,9);
```

El criterio es la selectividad esperada por la forma del predicado, que es lo
único que se puede saber sin estadísticas: una igualdad sobre columna indexada
devuelve del orden de una fila, una ventana espacial un área acotada, y un rango
puede devolver media tabla. Por eso una igualdad manda sobre un `WITHIN` aunque
el R-Tree sea barato de recorrer — lo que se minimiza no es el costo del índice
sino cuántas filas llegan al filtro. El panel de plan lo dice explícitamente:

```
Planificacion: IndexScan | BPLUS sobre id  [conduce id; filtra ubic]
Filtro residual | 1 filas del indice -> 1 tras aplicar 1 condicion(es) en memoria
```

Es una heurística, no una estimación: no mira histogramas ni cardinalidades.
Sustituirla por una estimación real de selectividad es la mejora que el
Experimento 3 dejó identificada.

El KNN usa búsqueda *best-first* con una cola de prioridad por MINDIST, así que
el resultado es **exacto**, no aproximado: visita el mínimo de nodos necesario.
Sobre 20 000 puntos, un KNN con k=10 cuesta 18 accesos a página frente a los
20 138 del escaneo completo, y una ventana que devuelve 264 filas cuesta 278
frente a 20 402.

El panel 5 del cliente web dibuja el resultado en un mapa (Leaflet +
OpenStreetMap) y aparece solo cuando la consulta devuelve una columna `POINT`.
Necesita conexión a internet para las teselas; sin ella el panel lo avisa y el
resto del cliente sigue funcionando.

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

## Rutas de acceso

El planificador elige entre cuatro, en este orden:

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

