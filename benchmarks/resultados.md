# Resultados experimentales

Generado por `benchmarks/run_experimentos.sh` el 2026-11-12 10:38.
Dataset: 500000 filas sinteticas, claves barajadas con semilla fija.


==========================================================================
  EXPERIMENTO 1 - Costo de insercion masiva
==========================================================================
Pagina de 4096 B | buffer pool de 64 paginas | claves desordenadas

### Tiempo total de carga (ms)

| Estructura | N=1000 | N=10000 | N=50000 | N=100000 | N=250000 | N=500000 |
|---|---|---|---|---|---|---|
| Heap File | 1 | 5 | 24 | 48 | 116 | 266 |
| Sequential sin reorg. | 17 | 21300 | no termina | no termina | no termina | no termina |
| Sequential con reorg. | 8 | 65 | 895 | 2857 | 9695 | 26576 |
| Heap + B+ Tree | 1 | 10 | 210 | 609 | 2041 | 5242 |
| Heap + Hash dinamico | 1 | 15 | 273 | 587 | 2012 | 5888 |

### Escrituras de disco (bloques de 4096 B)

| Estructura | N=1000 | N=10000 | N=50000 | N=100000 | N=250000 | N=500000 |
|---|---|---|---|---|---|---|
| Heap File | 11 | 154 | 1025 | 2118 | 5397 | 10861 |
| Sequential sin reorg. | 13 | 9804 | - | - | - | - |
| Sequential con reorg. | 81 | 2302 | 31263 | 74546 | 223003 | 491628 |
| Heap + B+ Tree | 18 | 224 | 23581 | 67863 | 213808 | 466950 |
| Heap + Hash dinamico | 14 | 241 | 23369 | 67076 | 211178 | 460820 |

### Paginas ocupadas y costo por tupla en el N mas grande medido

| Estructura | N alcanzado | Paginas | us/tupla | Reorganizaciones |
|---|---|---|---|---|
| Heap File | 500000 | 5409 | 0.53 | 0 |
| Sequential sin reorg. | 10000 | 128 | 2129.98 | 0 |
| Sequential con reorg. | 500000 | 6840 | 53.15 | 17 |
| Heap + B+ Tree | 500000 | 8263 | 10.48 | 0 |
| Heap + Hash dinamico | 500000 | 7618 | 11.78 | 0 |

### Degradacion del Sequential File SIN reorganizacion

| N | Tiempo (ms) | us/tupla | Lecturas de disco |
|---|---|---|---|
| 1000 | 16 | 16.5 | 0 |
| 5000 | 539 | 107.9 | 0 |
| 10000 | 45228 | 4522.8 | 6058793 |
| 20000 | 476888 | 23844.4 | 55299674 |

El costo por tupla crece con N, asi que el costo total es cuadratico:
toda la carga cae en una unica cadena de overflow que hay que recorrer
entera en cada insercion para mantener el orden logico. Mientras esa
cadena cabe en el buffer pool el efecto es solo de CPU (0 lecturas);
cuando deja de caber, cada paso se convierte en I/O real y el tiempo
se dispara. Es exactamente el motivo por el que la reorganizacion
periodica no es opcional.


==========================================================================
  EXPERIMENTO 2 - Busquedas puntuales de igualdad
==========================================================================
N = 100000 registros | 1000 consultas aleatorias | pagina de 4096 B | buffer pool de 64 paginas

### Lecturas de disco por consulta

| Metodo | Media | Desv. estandar | Accesos a pagina (media) |
|---|---|---|---|
| Full Scan (Heap) | 2164.9 | 0.3 | 101083.0 |
| Busqueda Binaria (Sequential) | 21.0 | 6.9 | 33.6 |
| Arbol B+ | 1.9 | 0.4 | 6.0 |
| Hash dinamico | 1.8 | 0.4 | 4.0 |

### Latencia por consulta (ms)

| Metodo | Media | Desv. estandar | Speedup vs full scan |
|---|---|---|---|
| Full Scan (Heap) | 45.6069 | 20.4838 | 1.0x |
| Busqueda Binaria (Sequential) | 0.0920 | 0.0396 | 495.6x |
| Arbol B+ | 0.0311 | 0.0199 | 1466.8x |
| Hash dinamico | 0.0206 | 0.0091 | 2210.3x |

### Verificacion y tamano

| Metodo | Filas devueltas | Paginas del archivo |
|---|---|---|
| Full Scan (Heap) | 1000 | 1082 |
| Busqueda Binaria (Sequential) | 1000 | 1120 |
| Arbol B+ | 1000 | 1610 |
| Hash dinamico | 1000 | 1596 |

Todos los metodos devuelven la misma cantidad de filas: la comparacion
es valida. La desviacion estandar del full scan es practicamente cero
porque SIEMPRE recorre el archivo entero, no dependa de donde este la
clave; en los indices la variacion viene de cuantos niveles del arbol o
cuantas paginas del directorio estaban ya en el buffer pool.


==========================================================================
  EXPERIMENTO 3 - Rangos con selectividad variable
==========================================================================
N = 100000 registros | 20 consultas por selectividad | pagina de 4096 B | buffer pool de 64 paginas

### Lecturas de disco por consulta (media)

| Metodo | 0.1 % | 1.0 % | 5.0 % | 10.0 % | 25.0 % |
|---|---|---|---|---|---|
| Full Scan (Heap) | 2259.9 | 2821.2 | 3236.8 | 3246.0 | 3246.0 |
| Sequential File | 31.6 | 290.2 | 1452.6 | 2896.4 | 7192.0 |
| Arbol B+ | 95.9 | 947.2 | 4739.6 | 9475.2 | 23686.6 |

### Latencia por consulta (ms, media)

| Metodo | 0.1 % | 1.0 % | 5.0 % | 10.0 % | 25.0 % |
|---|---|---|---|---|---|
| Full Scan (Heap) | 39.237 | 42.609 | 47.850 | 104.988 | 118.855 |
| Sequential File | 0.513 | 4.103 | 18.910 | 49.550 | 97.400 |
| Arbol B+ | 1.512 | 14.358 | 68.744 | 142.799 | 316.503 |

### Filas devueltas por consulta (verificacion)

| Metodo | 0.1 % | 1.0 % | 5.0 % | 10.0 % | 25.0 % |
|---|---|---|---|---|---|
| Full Scan (Heap) | 100 | 1000 | 5000 | 10000 | 25000 |
| Sequential File | 100 | 1000 | 5000 | 10000 | 25000 |
| Arbol B+ | 100 | 1000 | 5000 | 10000 | 25000 |

### Ventaja del indice sobre el full scan (veces mas rapido)

| Metodo | 0.1 % | 1.0 % | 5.0 % | 10.0 % | 25.0 % |
|---|---|---|---|---|---|
| Sequential File | 76.4x | 10.4x | 2.5x | 2.1x | 1.2x |
| Arbol B+ | 26.0x | 3.0x | 0.7x | 0.7x | 0.4x |

### Punto de cruce con el full scan

| Metodo | Cruce (selectividad) |
|---|---|
| Sequential File | no cruza hasta el 25 % |
| Arbol B+ | 2.5 % |

Las tres filas devuelven la misma cantidad de tuplas, asi que la comparacion
es justa: los tres metodos entregan las filas completas, no solo sus RID.

Lo importante es el PUNTO DE CRUCE. El Arbol B+ arranca ganando por amplio
margen, pero su ventaja se derrumba al subir la selectividad hasta quedar POR
DEBAJO del full scan. La causa no es el arbol: es que el indice esta sobre un
Heap File, o sea que NO ESTA AGRUPADO. El recorrido de hojas entrega las claves
ordenadas, pero cada RID apunta a una pagina cualquiera del heap, y con miles de
resultados el motor termina pidiendo la misma pagina una y otra vez en orden
aleatorio: mas transferencias que leer el archivo entero una sola vez.

El Sequential File no sufre eso porque SI esta agrupado: las filas consecutivas
por clave son fisicamente vecinas, asi que un rango es lectura casi secuencial.
Por eso gana al B+ en todas las selectividades a pesar de no tener indice.

Conclusion para el planificador: sobre un heap, un IndexRangeScan solo conviene
por debajo de 2.5 % de selectividad;
pasado ese umbral deberia elegir SeqScan.
Es la misma regla que aplican los optimizadores reales.


==========================================================================
  EXPERIMENTO 4 - Sensibilidad al tamano de bloque
==========================================================================
N = 100000 registros | Heap + Arbol B+ | 500 consultas puntuales

### Fan-out, altura y transferencias segun B

| B (bytes) | Fan-out hoja | Fan-out interno | Altura h | Pag. indice | Pag. datos | Lecturas/consulta | Bytes/consulta |
|---|---|---|---|---|---|---|---|
| 1024 | 63 | 84 | 3 | 2282 | 4512 | 2.31 | 2365 |
| 2048 | 127 | 169 | 3 | 1163 | 2190 | 1.96 | 4022 |
| 4096 | 255 | 340 | 3 | 528 | 1082 | 1.87 | 7660 |
| 8192 | 511 | 681 | 2 | 261 | 538 | 1.66 | 13631 |

El fan-out crece de forma casi proporcional a B, asi que la altura del arbol
baja y cada busqueda puntual necesita menos transferencias. Pero cada
transferencia mueve mas bytes: la columna de bytes por consulta muestra el
costo real. El optimo no es "la pagina mas grande posible" sino el punto donde
dejar de bajar la altura ya no compensa mover bloques mas grandes.

==========================================================================
  EXPERIMENTO 5 - Escalabilidad del modulo espacial (R-Tree)
==========================================================================
Puntos uniformes en 1000x1000 | pagina de 4096 B | buffer pool de 64 paginas
Fan-out del nodo: 102 entradas por hoja, 113 por nodo interno

### A.1 Construccion del indice

| N | Carga (ms) | us/punto | Pag. datos | Pag. indice | Altura h |
|---|---|---|---|---|---|
| 1000 | 4 | 3.74 | 9 | 16 | 2 |
| 5000 | 16 | 3.25 | 46 | 74 | 2 |
| 10000 | 38 | 3.83 | 92 | 146 | 3 |
| 50000 | 412 | 8.24 | 469 | 727 | 3 |
| 100000 | 1035 | 10.35 | 941 | 1467 | 3 |
| 250000 | 7755 | 31.02 | 2383 | 3643 | 3 |

### A.2 Ventana del 1 % del area: R-Tree vs escaneo completo

| N | Filas | R-Tree accesos | Scan accesos | Ganancia | R-Tree ms | Scan ms |
|---|---|---|---|---|---|---|
| 1000 | 10 | 14 | 1019 | 72.8x | 0.017 | 0.526 |
| 5000 | 51 | 56 | 5097 | 91.0x | 0.028 | 1.153 |
| 10000 | 102 | 110 | 10194 | 92.7x | 0.192 | 3.238 |
| 50000 | 504 | 526 | 50973 | 96.9x | 1.967 | 16.705 |
| 100000 | 994 | 1030 | 101935 | 99.0x | 9.458 | 64.928 |
| 250000 | 2501 | 2574 | 254884 | 99.0x | 34.180 | 194.289 |

### A.3 KNN con k = 10

| N | R-Tree accesos | Scan accesos | Ganancia | R-Tree ms | Scan ms |
|---|---|---|---|---|---|
| 1000 | 14 | 1009 | 72.1x | 0.019 | 0.480 |
| 5000 | 14 | 5046 | 360.4x | 0.030 | 2.237 |
| 10000 | 16 | 10092 | 630.8x | 0.054 | 5.368 |
| 50000 | 16 | 50469 | 3154.3x | 0.122 | 28.868 |
| 100000 | 16 | 100941 | 6308.8x | 0.342 | 115.410 |
| 250000 | 18 | 252383 | 14021.3x | 0.478 | 324.329 |

### A.4 Verificacion: las dos rutas devuelven lo mismo

| N | Ventana | KNN |
|---|---|---|
| 1000 | coinciden | coinciden |
| 5000 | coinciden | coinciden |
| 10000 | coinciden | coinciden |
| 50000 | coinciden | coinciden |
| 100000 | coinciden | coinciden |
| 250000 | coinciden | coinciden |

Las dos rutas devuelven exactamente las mismas filas en todos los tamanos,
asi que la comparacion de costos es valida.

==========================================================================
  B. Selectividad de la ventana, con N = 100000
==========================================================================

### B.1 Costo por consulta segun el area que cubre la ventana

| Area de la ventana | Filas | R-Tree accesos | Scan accesos | R-Tree ms | Scan ms | Ganancia |
|---|---|---|---|---|---|---|
| 0.01 % | 10 | 16 | 100951 | 0.257 | 66.073 | 257.4x |
| 0.10 % | 99 | 110 | 101040 | 1.358 | 71.571 | 52.7x |
| 1.00 % | 998 | 1033 | 101939 | 11.017 | 69.811 | 6.3x |
| 5.00 % | 4966 | 5079 | 105907 | 62.011 | 88.564 | 1.4x |
| 25.00 % | 24906 | 25345 | 125847 | 293.593 | 102.809 | 0.4x |

### B.2 Punto de cruce con el escaneo completo

| Metodo | Cruce (area de la ventana) |
|---|---|
| R-Tree (ventana) | 7.4 % |

==========================================================================
  C. Costo del KNN segun k, con N = 100000
==========================================================================

### C.1 R-Tree best-first vs escaneo con ordenacion parcial

| k | R-Tree accesos | Scan accesos | Ganancia | R-Tree ms | Scan ms |
|---|---|---|---|---|---|
| 1 | 6 | 100941 | 16823.5x | 0.234 | 93.247 |
| 10 | 16 | 100941 | 6308.8x | 0.356 | 91.494 |
| 100 | 110 | 100941 | 917.6x | 1.779 | 104.657 |
| 1000 | 1035 | 100941 | 97.5x | 13.748 | 85.704 |

### C. k-NN y radio sobre coordenadas reales (haversine)

| N | consulta | R-Tree accesos | Scan accesos | Ganancia | R-Tree ms | Scan ms | filas |
|---|---|---|---|---|---|---|---|
| 10000 | k-NN k=5 | 10 | 10092 | 1009.2x | 0.204 | 10.565 | 5.0 |
| 10000 | k-NN k=20 | 26 | 10092 | 388.2x | 0.293 | 10.943 | 20.0 |
| 10000 | k-NN k=50 | 58 | 10092 | 174.0x | 0.413 | 10.177 | 50.0 |
| 10000 | radio 500 m | 11 | 10098 | 918.0x | 0.147 | 9.848 | 6.2 |
| 10000 | radio 2000 m | 114 | 10196 | 89.4x | 0.682 | 10.968 | 104.3 |
| 10000 | radio 5000 m | 662 | 10732 | 16.2x | 2.551 | 10.969 | 640.0 |
| 50000 | k-NN k=5 | 10 | 50469 | 5046.9x | 0.402 | 60.869 | 5.0 |
| 50000 | k-NN k=20 | 27 | 50469 | 1869.2x | 0.592 | 68.596 | 20.0 |
| 50000 | k-NN k=50 | 59 | 50469 | 855.4x | 0.955 | 63.249 | 50.0 |
| 50000 | radio 500 m | 40 | 50501 | 1262.5x | 0.590 | 53.779 | 32.1 |
| 50000 | radio 2000 m | 563 | 51008 | 90.6x | 4.978 | 54.101 | 538.9 |
| 50000 | radio 5000 m | 3373 | 53761 | 15.9x | 29.675 | 58.961 | 3292.2 |
| 100000 | k-NN k=5 | 11 | 100941 | 9176.5x | 0.548 | 127.813 | 5.0 |
| 100000 | k-NN k=20 | 28 | 100941 | 3605.0x | 0.769 | 128.987 | 20.0 |
| 100000 | k-NN k=50 | 61 | 100941 | 1654.8x | 1.366 | 132.185 | 50.0 |
| 100000 | radio 500 m | 77 | 101007 | 1311.8x | 1.179 | 104.812 | 66.2 |
| 100000 | radio 2000 m | 1114 | 102015 | 91.6x | 14.045 | 115.847 | 1074.0 |
| 100000 | radio 5000 m | 6726 | 107522 | 16.0x | 75.297 | 120.234 | 6581.0 |

El costo del k-NN crece con k pero MUCHISIMO menos que con N: pasar de
k=5 a k=50 multiplica por diez los vecinos pedidos y no multiplica por diez
las paginas leidas, porque la poda por MINDIST sigue descartando casi todo
el arbol. El radio se comporta como la ventana: cuanto mas grande, mas se
parece a un escaneo.

### D. Efecto de la dimension: cuando el escaneo le gana al arbol

N = 10000 vectores uniformes en el cubo unitario, k = 10, 20 consultas.
Las paginas del arbol y las del escaneo son las dos de 4096 B, asi que son comparables.

| D | fan-out | nodos (pag.) | altura | arbol: pag/consulta | % del arbol | scan: pag. | arbol ms | scan ms | gana |
|---|---|---|---|---|---|---|---|---|---|
| 2 | 204 | 51 | 2 | 3.6 | 7.2 % | 30 | 0.033 | 0.061 | arbol |
| 4 | 113 | 90 | 2 | 23.2 | 25.8 % | 49 | 0.173 | 0.089 | arbol |
| 8 | 60 | 171 | 3 | 86.0 | 50.3 % | 88 | 0.508 | 0.128 | arbol |
| 16 | 30 | 347 | 3 | 325.4 | 93.8 % | 167 | 1.377 | 0.179 | SCAN |
| 32 | 15 | 716 | 4 | 716.0 | 100.0 % | 323 | 2.638 | 0.323 | SCAN |
| 64 | 7 | 1669 | 5 | 1669.0 | 100.0 % | 635 | 5.825 | 0.689 | SCAN |
| 128 | 3 | 5003 | 9 | 5002.0 | 100.0 % | 1260 | 15.716 | 1.615 | SCAN |

Dos efectos se suman, y los dos empujan en la misma direccion.

El primero es aritmetico y se ve en la columna del fan-out: una entrada de
un nodo guarda 2*D coordenadas, asi que en D = 128 entran tres entradas por
pagina y en D = 2 entran doscientas. El arbol se vuelve alto y flaco, y
bajar por el cuesta mas lecturas aunque la poda funcionara perfecto.

El segundo es geometrico y es el de verdad: al crecer D las cajas
envolventes se solapan tanto que el MINDIST de casi todas es menor que la
distancia al k-esimo vecino, y el best-first deja de podar. La columna
'% del arbol' es la medida directa de eso: cuando se acerca al 100 %, el
arbol esta visitandolo todo y ya no es un indice, es un escaneo con
indirecciones.

La ultima columna marca el cruce EN PAGINAS, que es la medida que no
depende de la maquina. En milisegundos el cruce llega antes, y conviene
decirlo: el escaneo es un bucle apretado y contiguo sobre un arreglo,
mientras que bajar por el arbol son saltos de puntero y una cola de
prioridad. O sea que el arbol pierde incluso un poco antes de lo que la
cuenta de bloques sugiere.

Pasado ese punto, un indice espacial EXACTO no puede ganarle a leer el
archivo de corrido, y es exactamente la razon por la que la Parte 4 no
extiende el R-Tree a 384 dimensiones sino que cambia de estrategia:
renuncia a la exactitud y usa IVF.

Lectura de los resultados.

El KNN es donde el R-Tree gana por mas margen, y por una razon estructural:
la busqueda best-first solo abre los nodos cuyo MINDIST es menor que la
distancia al k-esimo vecino ya encontrado, asi que el costo depende de k y no
de N. El escaneo, en cambio, tiene que calcular la distancia de las N filas
aunque solo devuelva diez.

La ventana se comporta como el rango del Experimento 3: el indice gana por
ordenes de magnitud mientras la ventana es chica, y pierde terreno al crecer,
por el mismo motivo de alla. El indice esta sobre un Heap File, o sea que NO
ESTA AGRUPADO: las hojas del R-Tree entregan los RID de puntos vecinos en el
plano, pero esos RID apuntan a paginas cualesquiera del heap, en el orden en
que se insertaron. Con una ventana suficientemente grande, el motor termina
pidiendo las mismas paginas una y otra vez y leer el archivo completo una sola
vez sale mas barato.

La consecuencia de diseno es la misma que en el caso unidimensional: el
planificador deberia estimar cuantas filas va a devolver la ventana y elegir
SeqScan pasado el umbral, en vez de usar el indice siempre que exista.


==========================================================================
  EXPERIMENTO 6 - Modulo de texto (indice invertido SPIMI / BM25)
==========================================================================
Corpus sintetico con vocabulario zipfiano de 20000 palabras, 60-140 terminos por documento, semilla fija.

### A. Construccion SPIMI segun el numero de documentos

Presupuesto de memoria fijo en 4 MB.

| N docs | tokens | terminos | bloques | construir (ms) | escrituras | pag. indice | indice (KB) | postings sin compr. (KB) |
|---|---|---|---|---|---|---|---|---|
| 1000 | 99572 | 13403 | 1 | 106.4 | 281 | 140 | 560.0 | 773.4 |
| 5000 | 495338 | 16162 | 1 | 436.7 | 789 | 394 | 1576.0 | 3847.2 |
| 25000 | 2483321 | 17739 | 3 | 2106.8 | 3183 | 1591 | 6364.0 | 19287.3 |
| 50000 | 4974792 | 18204 | 6 | 7687.5 | 6167 | 3083 | 12332.0 | 38635.8 |

El tiempo crece de forma practicamente lineal con el numero de
documentos: SPIMI los recorre una sola vez. El indice completo (que
incluye diccionario y tabla de documentos) pesa menos que los
postings sin comprimir, gracias a los huecos y el varint.

### B. Efecto del presupuesto de memoria (N = 20000)

| presupuesto | bloques | construir (ms) | escrituras | terminos | pag. indice |
|---|---|---|---|---|---|
| 0.12 MB | 835 | 6762.9 | 2589 | 17577 | 1294 |
| 0.50 MB | 141 | 4796.2 | 2589 | 17577 | 1294 |
| 2.00 MB | 13 | 3372.2 | 2589 | 17577 | 1294 |
| 8.00 MB | 3 | 3084.6 | 2589 | 17577 | 1294 |
| 64.00 MB | 1 | 2934.8 | 2589 | 17577 | 1294 |

El numero de terminos y el tamano del indice NO cambian: el
presupuesto solo decide cuantas veces se vuelca la memoria y, por
tanto, cuantos bloques hay que mezclar al final. Esa es exactamente
la promesa de SPIMI: el resultado es independiente de la RAM
disponible, y la RAM solo compra velocidad.

### C. Consulta: indice invertido contra escaneo completo

BM25, top-10, 20 consultas de 3 terminos cada una.

| N docs | indice: accesos | indice: ms | scan: accesos | scan: ms | accesos x | ms x | top-10 identico |
|---|---|---|---|---|---|---|---|
| 1000 | 1453 | 4.5 | 1206 | 117.7 | 0.8x | 26.0x | si (3 empate/s) |
| 5000 | 4206 | 2.4 | 6023 | 582.3 | 1.4x | 241.7x | si (3 empate/s) |
| 25000 | 18169 | 25.6 | 30125 | 3183.4 | 1.7x | 124.2x | si (3 empate/s) |
| 50000 | 35418 | 47.0 | 60259 | 6251.9 | 1.7x | 133.1x | si (1 empate/s) |

La ultima columna es la que da sentido a las demas: si los puntajes no
coincidieran, comparar tiempos no significaria nada. Un "empate" es un
par de documentos con BM25 identico en el ultimo puesto: las dos rutas
eligen uno distinto y las dos tienen razon. El escaneo paga
leer y tokenizar los N documentos en cada consulta; el indice solo
lee las listas de los terminos pedidos, cuya longitud depende del df
y no de N.

### D. Costo de la consulta segun la frecuencia del termino

| termino | df | % del corpus | accesos a pagina | ms | resultados |
|---|---|---|---|---|---|
| voc0 | 392 | 1.96 % | 411 | 0.960 | 10 |
| voc1 | 406 | 2.03 % | 425 | 0.762 | 10 |
| voc10 | 391 | 1.96 % | 410 | 0.759 | 10 |
| voc100 | 381 | 1.91 % | 400 | 0.736 | 10 |
| voc1000 | 344 | 1.72 % | 363 | 0.720 | 10 |
| voc5000 | 150 | 0.75 % | 169 | 0.638 | 10 |
| voc15000 | 10 | 0.05 % | 29 | 0.193 | 10 |

Un termino muy frecuente obliga a leer una lista de postings larga
y a puntuar muchos documentos; uno raro se resuelve con unas pocas
paginas. Es el motivo por el que quitar stopwords importa: son
justo los terminos con df cercano a N, los mas caros y los que menos
discriminan.

### E. Recuperacion booleana: indice contra recorrido completo

| operador | terminos | indice: accesos | indice: ms | scan: ms (cota inf.) | ms x | filas | identico |
|---|---|---|---|---|---|---|---|
| AND | 1 | 8.8 | 0.1907 | 24.5853 | 128.9x | 242 | si |
| AND | 3 | 26.6 | 0.3345 | 45.3611 | 135.6x | 0 | si |
| AND | 5 | 44.4 | 0.4178 | 58.3909 | 139.7x | 0 | si |
| OR | 1 | 8.8 | 0.1749 | 28.3150 | 161.9x | 209 | si |
| OR | 3 | 26.4 | 0.2990 | 47.7222 | 159.6x | 660 | si |
| OR | 5 | 44.1 | 0.4084 | 56.2577 | 137.7x | 1110 | si |
| AND NOT | 1 | 9.0 | 0.2016 | 27.7912 | 137.9x | 244 | si |
| AND NOT | 3 | 26.9 | 0.2873 | 46.9328 | 163.3x | 252 | si |
| AND NOT | 5 | 44.0 | 0.3764 | 58.1508 | 154.5x | 270 | si |

El costo lo manda LEER las listas, no mezclarlas: los accesos a pagina
crecen casi proporcionalmente al numero de terminos (de ~8 con uno a ~43 con
cinco) sea cual sea el operador, porque hay que traer una lista por termino.
La mezcla en si es lineal y practicamente gratis al lado de eso.

El AND de tres o mas terminos tomados al azar devuelve CERO documentos casi
siempre. No es un defecto de la medicion, es como se comporta un AND sobre
vocabulario zipfiano: la probabilidad de que un mismo documento contenga tres
palabras independientes es el producto de tres fracciones pequenas. Esa es,
en una linea, la razon por la que los buscadores dejaron de exigir que
aparezcan todos los terminos y pasaron a RANKEAR: con el booleano puro, una
consulta de cinco palabras no devuelve nada.

Frente al recorrido completo el indice gana entre 30 y 55 veces, y eso que la
columna del escaneo es una cota inferior que no incluye leer ni tokenizar.

### F. Export para comparar contra el indice GIN de PostgreSQL

Corpus de 20000 documentos en  data/exp6_corpus.csv
Consultas en                  data/exp6_consultas.txt

PostgreSQL se usa aqui SOLO como punto de comparacion, no como
almacenamiento: la restriccion del enunciado es no apoyarse en un motor
ajeno para guardar o indexar los datos del proyecto, y el indice que se
mide sigue siendo el propio. Para la otra mitad de la tabla:

    python benchmarks/exp6_gin.py --csv data/exp6_corpus.csv --consultas data/exp6_consultas.txt


==========================================================================
  EXPERIMENTO 7 - BoVW (clasico) contra embeddings densos (moderno)
==========================================================================

6899 imagenes, 8 clases, 24 consultas.
BoVW: codebook de 256 palabras visuales (SIFT + K-Means).
Denso: 512 dimensiones.

### A. Construccion de las dos representaciones

| representacion | extraccion (ms) | codebook/-- (ms) | indexar (ms) | pag. datos | pag. indice | detalle |
|---|---|---|---|---|---|---|
| BoVW + invertido | 71268 | 3697 | 1248 | 1435 | 343 | 246 palabras visuales usadas |
| Denso + IVF | (red) | -- | 3174 | 6899 | 3514 | 83 celdas |

Insertar las 6899 filas en las dos tablas: 304 ms.
El BoVW paga su costo ANTES de la consulta: extraer los descriptores y
entrenar el codebook es lo caro, y hay que rehacerlo si cambia la
coleccion. El denso paga una pasada de red por imagen y ya.

### B. Efectividad: Precision@5 y Precision@10

| representacion | indice | P@5 | P@10 | ms/consulta | accesos/consulta | se encuentra a si misma |
|---|---|---|---|---|---|---|
| BoVW (SIFT + K-Means) | invertido TF-IDF | 0.4250 | 0.4292 | 19.050 | 7427.8 | 24/24 |
| Embeddings densos | IVF coseno | 1.0000 | 0.9917 | 7.564 | 755.7 | 24/24 |

La ultima columna es una comprobacion de cordura, no un resultado: la imagen
de consulta esta dentro de la coleccion, asi que tiene que salir primera en
las dos rutas. Si no saliera, el error estaria en la indexacion y no en la
representacion, y el resto de la tabla no significaria nada.

Lo que se espera ver, y es el resultado clasico de la literatura: el BoVW
pierde contra los embeddings densos por un margen amplio. La razon es que
cuantizar un descriptor a la palabra visual mas cercana TIRA informacion ---
dos parches parecidos que caen en celdas distintas dejan de parecerse, y dos
parches distintos que caen en la misma pasan a ser identicos --- y ademas el
histograma olvida donde estaba cada cosa en la imagen. El embedding de una red
entrenada no cuantiza nada y ademas fue optimizado justamente para que la
distancia signifique semejanza semantica.

Lo que el BoVW si conserva: no necesita GPU ni un modelo preentrenado, el
codebook se entrena con los propios datos, y cada resultado se puede explicar
--- 'estas dos imagenes comparten estas palabras visuales' --- mientras que un
embedding denso es opaco. Y, para este motor, tiene una propiedad que vale la
pena remarcar: no hizo falta anadir NADA. Un histograma de palabras visuales
es un documento, y el indice invertido de la Parte 3 lo indexa igual que un
abstract de arXiv.


==========================================================================
  EXPERIMENTO 8 - Eficiencia contra calidad en busqueda aproximada (ANN)
==========================================================================

Vectores agrupados en nubes gaussianas, dimension 128, metrica coseno.
Recall@10 medido contra fuerza bruta sobre el mismo indice, 50 consultas.

### A. Construccion: como crece el indice con N

| N | nlist | ms | escrituras | paginas | KB | iters | media/min/max por celda |
|---|---|---|---|---|---|---|---|
| 1000 | 32 | 43.6 | 409 | 134 | 536 | 12 | 31.2 / 17 / 46 |
| 2500 | 50 | 139.8 | 1167 | 329 | 1316 | 12 | 50.0 / 22 / 101 |
| 5000 | 71 | 297.7 | 2876 | 651 | 2604 | 12 | 70.4 / 23 / 185 |
| 10000 | 100 | 603.1 | 6956 | 1295 | 5180 | 12 | 100.0 / 24 / 360 |
| 20000 | 141 | 1295.9 | 15920 | 2579 | 10316 | 12 | 141.8 / 24 / 643 |

Con nlist = raiz de N (el valor por defecto), el numero de vectores por
celda crece tambien como raiz de N: es lo que mantiene acotado el costo de
una consulta. El tamano del indice crece lineal con N, como tiene que ser:
guarda los vectores dentro (IVF-Flat), no solo punteros.

### B. Numero de celdas: nlist contra costo y calidad

| nlist | inercia | paginas | media/min/max | nprobe def. | recall@10 | accesos/consulta | ms/consulta |
|---|---|---|---|---|---|---|---|
| 1 | 27871.3 | 2562 | 20000.0 / 20000 / 20000 | 1 | 1.0000 | 2720.0 | 22.288 |
| 4 | 26524.9 | 2562 | 5000.0 / 4252 / 5415 | 2 | 0.6940 | 1381.9 | 13.147 |
| 16 | 25107.5 | 2563 | 1250.0 / 860 / 1633 | 4 | 0.6600 | 705.2 | 6.872 |
| 64 | 23185.5 | 2569 | 312.5 / 41 / 808 | 8 | 0.7500 | 417.4 | 3.865 |
| 141 | 22751.5 | 2579 | 141.8 / 24 / 643 | 12 | 0.7460 | 395.0 | 2.747 |
| 400 | 21853.2 | 2612 | 50.0 / 14 / 158 | 20 | 0.6740 | 577.9 | 2.654 |
| 1000 | 20857.4 | 2689 | 20.0 / 5 / 71 | 32 | 0.6800 | 1154.8 | 3.262 |

nlist = 1 es el caso degenerado: una sola celda con todo dentro, o sea
fuerza bruta disfrazada de indice (recall 1 y el costo mas alto). Al subir
nlist las celdas se encogen y la consulta se abarata, pero el recall con el
nprobe por defecto BAJA, no sube: nprobe por defecto es raiz de nlist, asi
que la FRACCION de celdas visitadas se encoge --- de la mitad con nlist=4 a
la treintava parte con nlist=1000 --- y cada vez quedan fuera mas vecinos.
Es la leccion practica del experimento: nlist y nprobe no se eligen por
separado, y el valor por defecto es solo un punto de partida.
La inercia (suma de distancias al centroide) solo baja: mide lo bien que el
clustering describe los datos, no lo buena que es la busqueda.

### C. La curva: recall@10 frente a nprobe

N = 20000, nlist = 141, fuerza bruta: 27.414 ms y 2718.0 accesos a pagina por consulta.

| nprobe | % celdas | recall@10 | accesos/consulta | bloques leidos | ms/consulta | aceleracion | % del I/O exacto |
|---|---|---|---|---|---|---|---|
| 1 | 0.7 % | 0.3280 | 176.0 | 51.5 | 0.540 | 50.7x | 6.5 % |
| 2 | 1.4 % | 0.4600 | 203.6 | 77.2 | 0.841 | 32.6x | 7.5 % |
| 5 | 3.5 % | 0.6180 | 259.6 | 128.4 | 1.453 | 18.9x | 9.5 % |
| 10 | 7.1 % | 0.7160 | 358.5 | 219.2 | 2.464 | 11.1x | 13.2 % |
| 20 | 14.2 % | 0.8000 | 549.9 | 394.5 | 4.180 | 6.6x | 20.2 % |
| 50 | 35.5 % | 0.9180 | 1159.5 | 954.7 | 10.520 | 2.6x | 42.7 % |
| 141 | 100.0 % | 1.0000 | 3076.0 | 2717.2 | 28.835 | 1.0x | 113.2 % |

Esta es la tabla que resume el modulo. El recall sube rapido al principio
y se aplana: las primeras celdas son las que contienen a casi todos los
vecinos verdaderos, y las siguientes aportan cada vez menos por el mismo
precio. Con nprobe = nlist el recall es 1 por definicion, porque se visita
todo: es fuerza bruta, y ahi el indice no aporta nada. De hecho sale algo
PEOR que fuerza bruta --- mas del 100 % de su I/O y una aceleracion por
debajo de 1 --- porque ademas de leer todas las celdas hay que comparar
contra los nlist centroides y ordenarlos: trabajo que la fuerza bruta no
hace. El punto util de la curva es donde el recall ya es alto y el I/O
sigue siendo una fraccion pequena del exacto.

### D. Dimension: por que 384 cambia el diseno

| dim | bytes/entrada | entradas/pagina | paginas | recall@10 (nprobe def.) | ms/consulta |
|---|---|---|---|---|---|
| 8 | 44 | 93.1 | 57 | 0.9960 | 0.039 |
| 32 | 140 | 29.3 | 176 | 0.9080 | 0.243 |
| 128 | 524 | 7.8 | 651 | 0.6980 | 0.850 |
| 384 | 1548 | 2.6 | 1919 | 0.3500 | 2.525 |
| 768 | 3084 | 1.3 | 3821 | 0.3120 | 5.120 |

En dimension 384 una entrada pesa 1548 bytes: entran DOS Y MEDIA en una
pagina de 4 KB, y el indice acaba pesando mas que la tabla. Eso es lo que
obliga a escribir las celdas contiguas: si los vectores estuvieran solo en
el heap, recorrer una celda seria un salto aleatorio por candidato en vez
de una lectura secuencial. Y es tambien la razon de fondo de todo el
modulo: en dimension alta el R-Tree de la parte 2 no poda nada, y hay que
cambiar exactitud por costo.

