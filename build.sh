#!/usr/bin/env bash
# ============================================================================
#  build.sh - Compila y ejecuta el proyecto SIN necesitar `make`.
#
#  Pensado para Git Bash en Windows, donde hay g++ pero no make. Hace
#  exactamente lo mismo que el Makefile.
#
#  Uso:
#     ./build.sh              compila todo en build/
#     ./build.sh test         compila y ejecuta las 6 suites de tests
#     ./build.sh demo         demostracion end-to-end por consola
#
#  Tras compilar queda tambien build/dump_page, el inspector de los binarios.
#     ./build.sh bench        corre los cuatro experimentos del enunciado
#     ./build.sh run          levanta el servidor en http://localhost:8080
#     ./build.sh clean        borra binarios y datos generados
#
#  Variables:
#     PAGE=8192 ./build.sh bench     cambia el tamano de pagina (Experimento 4)
#     PORT=9000 ./build.sh run       otro puerto
#     POOL=8    ./build.sh run       buffer pool chico: hace visible el I/O fisico
# ============================================================================
set -e
cd "$(dirname "$0")"

CXX=${CXX:-g++}
PAGE=${PAGE:-4096}
PORT=${PORT:-8080}
POOL=${POOL:-64}
FLAGS="-std=c++17 -O2 -Wall -Wextra -Ibackend/include -DDB_PAGE_SIZE=$PAGE"

# Winsock solo en Windows (Git Bash / MSYS / Cygwin lo reportan en $OSTYPE).
# Ademas hay que declarar Windows 10: MinGW asume una version mas vieja por
# defecto y cpp-httplib corta la compilacion con
#   #error "cpp-httplib doesn't support Windows 8 or lower"
EXTRA_SRV="-pthread"
case "$OSTYPE" in
  msys*|cygwin*|win32*)
    EXTRA_SRV="-pthread -lws2_32"
    FLAGS="$FLAGS -D_WIN32_WINNT=0x0A00"
    ;;
esac

# Extension del ejecutable: .exe en Windows, nada en Linux/Mac.
EXE=""
case "$OSTYPE" in
  msys*|cygwin*|win32*) EXE=".exe" ;;
esac

mkdir -p build data

# Las fuentes de la biblioteca son todas menos las que tienen main().
LIB=$(ls backend/src/*.cpp | grep -v -e 'api_server' -e 'demo' -e 'dump_page')

compilar() {
    echo ">> compilando (pagina de $PAGE bytes)"
    $CXX $FLAGS backend/src/api_server.cpp $LIB -o "build/dbserver$EXE" $EXTRA_SRV
    $CXX $FLAGS backend/src/demo.cpp       $LIB -o "build/demo$EXE"
    $CXX $FLAGS backend/src/dump_page.cpp  $LIB -o "build/dump_page$EXE"
    for b in benchmarks/*.cpp; do
        [ -e "$b" ] || continue
        $CXX $FLAGS "$b" $LIB -o "build/$(basename "$b" .cpp)$EXE"
    done
    for t in backend/tests/test_*.cpp; do
        [ -e "$t" ] || continue
        $CXX $FLAGS -Ibackend/tests "$t" $LIB -o "build/$(basename "$t" .cpp)$EXE"
    done
    echo ">> listo"
}

case "${1:-all}" in
    all)   compilar ;;
    test)
        compilar
        for t in build/test_*; do
            echo ""
            echo ">>> $(basename "$t")"
            "./$t" || { echo "FALLO en $t"; exit 1; }
        done
        echo ""
        echo "TODOS LOS TESTS PASARON"
        ;;
    demo)  compilar; mkdir -p data/_demo; "./build/demo$EXE" data/_demo ;;
    bench) PAGE=$PAGE ./benchmarks/run_experimentos.sh ;;
    run)
        compilar
        echo ">> http://localhost:$PORT   (Ctrl+C para detener)"
        "./build/dbserver$EXE" --port "$PORT" --data data --static frontend --pool "$POOL"
        ;;
    clean)
        rm -rf build
        rm -f data/*.dat data/*.idx data/*.ovf data/*.csv data/*.tmp data/catalog.txt
        rm -rf data/_demo
        echo ">> limpio"
        ;;
    *)
        echo "uso: ./build.sh [all|test|demo|bench|run|clean]"
        exit 1
        ;;
esac
