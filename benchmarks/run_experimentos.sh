#!/usr/bin/env bash
# ============================================================================
#  run_experimentos.sh - Ejecuta los cuatro experimentos del enunciado y
#  guarda la salida en benchmarks/resultados.md
#
#     ./benchmarks/run_experimentos.sh [N_dataset]
#
#  PAGE=8192 ./benchmarks/run_experimentos.sh   cambia el tamano de bloque de
#  los experimentos 1-3 (el 4 recompila solo, porque barre los cuatro tamanos).
#
#  El experimento 4 recompila el motor para cada tamano de bloque, porque el
#  fan-out y la altura del arbol se derivan de PAGE_SIZE en tiempo de
#  compilacion.
# ============================================================================
set -e
cd "$(dirname "$0")/.."

N=${1:-500000}
PAGE=${PAGE:-4096}
SALIDA=benchmarks/resultados.md
LIB=$(ls backend/src/*.cpp | grep -v -e 'api_server' -e 'demo' -e 'dump_page')
EXE=""
case "$OSTYPE" in msys*|cygwin*|win32*) EXE=".exe" ;; esac

mkdir -p build data

# Los experimentos abren data/dataset_$DATASET_N.csv. Se pasa por entorno para
# no confundirlo con el N de cada experimento, que el enunciado fija aparte
# (el 2 exige 100 000 registros, el 1 barre seis tamanos).
export DATASET_N=$N

echo "==> generando dataset de $N filas"
g++ -std=c++17 -O2 -Ibackend/include -Ibenchmarks -DDB_PAGE_SIZE=$PAGE benchmarks/gen_dataset.cpp $LIB -o "build/gen_dataset$EXE"
"./build/gen_dataset$EXE" "$N" data

{
  echo "# Resultados experimentales"
  echo ""
  echo "Generado por \`benchmarks/run_experimentos.sh\` el $(date '+%Y-%m-%d %H:%M')."
  echo "Dataset: $N filas sinteticas, claves barajadas con semilla fija."
  echo ""
} > "$SALIDA"

for e in exp1_insercion exp2_puntual exp3_rangos; do
  echo "==> $e"
  g++ -std=c++17 -O2 -Ibackend/include -Ibenchmarks -DDB_PAGE_SIZE=$PAGE "benchmarks/$e.cpp" $LIB -o "build/$e$EXE"
  "./build/$e$EXE" data >> "$SALIDA"
done

echo "==> exp4 (recompilando para cada tamano de bloque)"
{
  echo ""
  echo "=========================================================================="
  echo "  EXPERIMENTO 4 - Sensibilidad al tamano de bloque"
  echo "=========================================================================="
  echo "N = 100000 registros | Heap + Arbol B+ | 500 consultas puntuales"
  echo ""
  echo "### Fan-out, altura y transferencias segun B"
  echo ""
} >> "$SALIDA"

primero=1
for B in 1024 2048 4096 8192; do
  echo "    B=$B"
  g++ -std=c++17 -O2 -Ibackend/include -Ibenchmarks -DDB_PAGE_SIZE=$B \
      benchmarks/exp4_pagina.cpp $LIB -o "build/exp4_$B$EXE"
  if [ $primero -eq 1 ]; then
    "./build/exp4_$B$EXE" data 100000 500 --cabecera >> "$SALIDA"
    primero=0
  else
    "./build/exp4_$B$EXE" data 100000 500 >> "$SALIDA"
  fi
done

cat >> "$SALIDA" <<'TXT'

El fan-out crece de forma casi proporcional a B, asi que la altura del arbol
baja y cada busqueda puntual necesita menos transferencias. Pero cada
transferencia mueve mas bytes: la columna de bytes por consulta muestra el
costo real. El optimo no es "la pagina mas grande posible" sino el punto donde
dejar de bajar la altura ya no compensa mover bloques mas grandes.
TXT

echo ""
echo "==> listo: $SALIDA"
