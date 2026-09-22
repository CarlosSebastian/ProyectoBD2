# Resultados experimentales

Generado por `benchmarks/run_experimentos.sh` el 2026-09-22 17:00.
Dataset: 500000 filas sinteticas, claves barajadas con semilla fija.


==========================================================================
  EXPERIMENTO 1 - Costo de insercion masiva
==========================================================================
Pagina de 4096 B | buffer pool de 64 paginas | claves desordenadas

### Tiempo total de carga (ms)

| Estructura | N=1000 | N=10000 | N=50000 | N=100000 | N=250000 | N=500000 |
|---|---|---|---|---|---|---|
| Heap File | 0 | 3 | 15 | 30 | 76 | 142 |
| Sequential sin reorg. | 11 | 6546 | no termina | no termina | no termina | no termina |
| Sequential con reorg. | 3 | 47 | 439 | 1118 | 3770 | 10199 |
| Heap + B+ Tree | 1 | 6 | 79 | 179 | 587 | 1415 |
| Heap + Hash dinamico | 0 | 5 | 65 | 170 | 513 | 1433 |

### Escrituras de disco (bloques de 4096 B)

| Estructura | N=1000 | N=10000 | N=50000 | N=100000 | N=250000 | N=500000 |
|---|---|---|---|---|---|---|
| Heap File | 11 | 154 | 1025 | 2118 | 5397 | 10861 |
| Sequential sin reorg. | 13 | 9804 | - | - | - | - |
| Sequential con reorg. | 81 | 2302 | 31263 | 74546 | 223003 | 491628 |
| Heap + B+ Tree | 18 | 224 | 23536 | 67790 | 213696 | 466795 |
| Heap + Hash dinamico | 14 | 241 | 23369 | 67076 | 211178 | 460820 |

### Paginas ocupadas y costo por tupla en el N mas grande medido

| Estructura | N alcanzado | Paginas | us/tupla | Reorganizaciones |
|---|---|---|---|---|
| Heap File | 500000 | 5409 | 0.28 | 0 |
| Sequential sin reorg. | 10000 | 128 | 654.58 | 0 |
| Sequential con reorg. | 500000 | 6840 | 20.40 | 17 |
| Heap + B+ Tree | 500000 | 8263 | 2.83 | 0 |
| Heap + Hash dinamico | 500000 | 7618 | 2.87 | 0 |

### Degradacion del Sequential File SIN reorganizacion

| N | Tiempo (ms) | us/tupla | Lecturas de disco |
|---|---|---|---|
| 1000 | 23 | 23.1 | 0 |
| 5000 | 426 | 85.2 | 0 |
| 10000 | 7759 | 775.9 | 6058793 |
| 20000 | 57333 | 2866.6 | 55299674 |

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
| Full Scan (Heap) | 21.2704 | 2.0970 | 1.0x |
| Busqueda Binaria (Sequential) | 0.0395 | 0.0119 | 538.3x |
| Arbol B+ | 0.0038 | 0.0023 | 5527.4x |
| Hash dinamico | 0.0032 | 0.0016 | 6717.7x |

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
| Arbol B+ | 96.0 | 947.2 | 4739.6 | 9475.2 | 23686.6 |

### Latencia por consulta (ms, media)

| Metodo | 0.1 % | 1.0 % | 5.0 % | 10.0 % | 25.0 % |
|---|---|---|---|---|---|
| Full Scan (Heap) | 22.026 | 21.313 | 23.017 | 24.804 | 27.674 |
| Sequential File | 0.087 | 0.627 | 3.232 | 6.202 | 14.509 |
| Arbol B+ | 0.178 | 1.599 | 7.079 | 13.454 | 32.618 |

### Filas devueltas por consulta (verificacion)

| Metodo | 0.1 % | 1.0 % | 5.0 % | 10.0 % | 25.0 % |
|---|---|---|---|---|---|
| Full Scan (Heap) | 100 | 1000 | 5000 | 10000 | 25000 |
| Sequential File | 100 | 1000 | 5000 | 10000 | 25000 |
| Arbol B+ | 100 | 1000 | 5000 | 10000 | 25000 |

### Ventaja del indice sobre el full scan (veces mas rapido)

| Metodo | 0.1 % | 1.0 % | 5.0 % | 10.0 % | 25.0 % |
|---|---|---|---|---|---|
| Sequential File | 254.5x | 34.0x | 7.1x | 4.0x | 1.9x |
| Arbol B+ | 123.7x | 13.3x | 3.3x | 1.8x | 0.8x |

### Punto de cruce con el full scan

| Metodo | Cruce (selectividad) |
|---|---|
| Sequential File | no cruza hasta el 25 % |
| Arbol B+ | 18.9 % |

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
por debajo de 18.9 % de selectividad;
pasado ese umbral deberia elegir SeqScan.
Es la misma regla que aplican los optimizadores reales.


==========================================================================
  EXPERIMENTO 4 - Sensibilidad al tamano de bloque
==========================================================================
N = 100000 registros | Heap + Arbol B+ | 500 consultas puntuales

### Fan-out, altura y transferencias segun B

| B (bytes) | Fan-out hoja | Fan-out interno | Altura h | Pag. indice | Pag. datos | Lecturas/consulta | Bytes/consulta |
|---|---|---|---|---|---|---|---|
| 1024 | 63 | 84 | 3 | 2282 | 4512 | 2.31 | 2363 |
| 2048 | 127 | 169 | 3 | 1163 | 2190 | 1.96 | 4022 |
| 4096 | 255 | 340 | 3 | 528 | 1082 | 1.87 | 7660 |
| 8192 | 511 | 681 | 2 | 261 | 538 | 1.66 | 13631 |

El fan-out crece de forma casi proporcional a B, asi que la altura del arbol
baja y cada busqueda puntual necesita menos transferencias. Pero cada
transferencia mueve mas bytes: la columna de bytes por consulta muestra el
costo real. El optimo no es "la pagina mas grande posible" sino el punto donde
dejar de bajar la altura ya no compensa mover bloques mas grandes.
