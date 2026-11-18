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
#     ./build.sh run          levanta el motor en http://localhost:8080 y, si hay
#                             Python, tambien el asistente RAG (:8000)
#     ./build.sh serve        lo mismo que run, sin recompilar
#     ./build.sh clean        borra binarios y datos generados
#
#  Variables:
#     PAGE=8192 ./build.sh bench     cambia el tamano de pagina (Experimento 4)
#     PORT=9000 ./build.sh run       otro puerto
#     POOL=8    ./build.sh run       buffer pool chico: hace visible el I/O fisico
#     RAG=0     ./build.sh run       solo el motor, sin el asistente RAG
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

# ---------------------------------------------------------------------------
#  Asistente RAG (ai/rag_server.py). Es OTRO proceso, en Python, que usa el
#  motor por su API REST. Se levanta junto al motor si hay un Python a mano.
# ---------------------------------------------------------------------------
buscarPython() {
    # En Git Bash / MSYS2 el PATH no siempre incluye el Python de Windows,
    # asi que ademas de los nombres habituales se prueban sus rutas tipicas.
    local c
    for c in python3 python py; do
        if command -v "$c" >/dev/null 2>&1 && "$c" -c "import sys" >/dev/null 2>&1; then
            echo "$c"; return 0
        fi
    done
    if [ -n "$LOCALAPPDATA" ] && command -v cygpath >/dev/null 2>&1; then
        local base; base=$(cygpath -u "$LOCALAPPDATA")
        for c in "$base"/Microsoft/WindowsApps/python.exe "$base"/Programs/Python/Python3*/python.exe; do
            if [ -x "$c" ] && "$c" -c "import sys" >/dev/null 2>&1; then
                echo "$c"; return 0
            fi
        done
    fi
    return 1
}

esperarMotor() {
    # Con el mismo Python del RAG, no con curl: MSYS2 no siempre trae curl, y
    # sin el, esta espera fallaba aunque el motor estuviera arriba.
    "$1" - "$PORT" <<'PY' >/dev/null 2>&1
import sys, time, urllib.request
url = "http://127.0.0.1:%s/api/health" % sys.argv[1]
for _ in range(60):
    try:
        urllib.request.urlopen(url, timeout=1); sys.exit(0)
    except Exception:
        time.sleep(0.25)
sys.exit(1)
PY
}

servir() {
    # El motor corre en PRIMER PLANO, como siempre: asi Ctrl+C le llega directo.
    if [ "${RAG:-1}" != "0" ] && [ -f ai/rag_server.py ]; then
        case "$OSTYPE" in
            msys*|cygwin*|win32*) ragWindows ;;
            *)
                local py
                if py=$(buscarPython); then
                    levantarRag "$py" &
                    local rag=$!
                    trap "kill $rag 2>/dev/null" EXIT
                else
                    echo ">> asistente RAG: no se encontro Python en esta terminal."
                    echo "   Levantalo aparte:  python ai/rag_server.py"
                fi
                ;;
        esac
    fi
    echo ">> motor: http://localhost:$PORT   (Ctrl+C detiene todo)"
    "./build/dbserver$EXE" --port "$PORT" --data data --static frontend --pool "$POOL"
}

faltaTablaRag() {
    # La tabla del RAG vive en data/ y './build.sh clean' la borra. Los
    # embeddings NO se borran (ai/cache/), asi que se puede recargar en ~1 min
    # sin recalcular nada ni necesitar Ollama.
    ! grep -q "^TABLE ${RAG_TABLA:-arxiv} " data/catalog.txt 2>/dev/null \
        && ls ai/cache/emb_*.bin >/dev/null 2>&1 \
        && [ -f ai/corpus/arxiv_papers.jsonl ]
}

ragWindows() {
    # En Windows el RAG se abre en SU PROPIA ventana (minimizada). Correr el
    # Python de Windows dentro de esta terminal de Git Bash / MSYS2 tumbaba al
    # motor sin ningun mensaje; en otra consola no lo toca. Al detener el motor
    # con Ctrl+C se cierra esa ventana tambien.
    local bat=ai/cache/levantar_rag.bat
    mkdir -p ai/cache
    # La ventana nueva hereda el entorno de MSYS2: su PATH no tiene el Python
    # de Windows y su 'timeout' es el de coreutils. Por eso el .bat rearma el
    # PATH de Windows desde el registro y llama a timeout.exe por su ruta.
    {
        echo '@echo off'
        echo 'title Asistente RAG'
        echo 'for /f "tokens=2,*" %%A in ('"'"'reg query "HKLM\SYSTEM\CurrentControlSet\Control\Session Manager\Environment" /v Path 2^>nul'"'"') do set "RAG_SP=%%B"'
        echo 'for /f "tokens=2,*" %%A in ('"'"'reg query "HKCU\Environment" /v Path 2^>nul'"'"') do set "RAG_UP=%%B"'
        echo 'call set "PATH=%RAG_SP%;%RAG_UP%;%LOCALAPPDATA%\Microsoft\WindowsApps"'
        echo "set DB_URL=http://127.0.0.1:$PORT"
        echo '"%SystemRoot%\System32\timeout.exe" /t 2 /nobreak >nul'
        if faltaTablaRag; then
            echo 'echo Falta la tabla del RAG: se recarga desde ai\cache\ (1 minuto)'
            echo 'python ai\ingesta.py'
        fi
        echo 'python ai\rag_server.py'
        echo 'if errorlevel 1 pause'
    } | sed 's/$/\r/' > "$bat"
    cmd.exe //c start "Asistente RAG" //min "ai\cache\levantar_rag.bat"
    trap 'taskkill //FI "WINDOWTITLE eq Asistente RAG*" //T //F >/dev/null 2>&1' EXIT
    echo ">> asistente RAG: ventana 'Asistente RAG' (minimizada), http://localhost:8000"
    echo "   o la pestana 'Asistente RAG' del cliente"
}

levantarRag() {
    local py=$1
    esperarMotor "$py" || echo ">> aviso: el motor tarda en responder en :$PORT"
    if faltaTablaRag; then
        echo ">> asistente RAG: falta la tabla del RAG; se recarga desde ai/cache/"
        DB_URL="http://127.0.0.1:$PORT" "$py" ai/ingesta.py | tail -n 5 \
            || echo "   la recarga fallo; correla a mano:  python ai/ingesta.py"
    fi
    echo ">> asistente RAG: http://localhost:8000  (o la pestana 'Asistente RAG')"
    DB_URL="http://127.0.0.1:$PORT" exec "$py" ai/rag_server.py > ai/cache/rag_server.log 2>&1
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
    run)   compilar; servir ;;
    serve) servir ;;
    clean)
        rm -rf build
        # Lo que genera el motor: se borra sin miedo, se recrea solo.
        rm -f data/*.dat data/*.idx data/*.ovf data/*.tmp data/catalog.txt
        # Indices de dos archivos: invertido (.dic + .inv) e IVF (.cen + .lst).
        rm -f data/*.dic data/*.inv data/*.cen data/*.lst
        # CSV de los benchmarks: tambien se regeneran solos al correrlos.
        rm -f data/dataset_*.csv data/exp6_corpus.csv data/exp6_consultas.txt
        rm -rf data/_demo data/_t*
        # Los de BoVW NO se borran. Prepararlos cuesta ~8 min de SIFT sobre
        # 6 899 imagenes, y esas imagenes son una descarga de 350 MB que no
        # esta en el repositorio. Para borrarlos: rm -f data/bovw_*
        echo ">> limpio  (se conservan data/bovw_* y data/*.onnx)"
        ;;
    *)
        echo "uso: ./build.sh [all|test|demo|bench|run|serve|clean]"
        exit 1
        ;;
esac
