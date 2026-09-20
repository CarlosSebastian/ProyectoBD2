# ============================================================================
#  Proyecto Integrador Multimodal - Base de Datos II (CS2042, UTEC)
#  Mini-gestor de bases de datos: motor + API REST + cliente web
#
#  LEVANTAR TODO EN UN PASO:
#      make run          -> compila y sirve http://localhost:8080
#
#  Otros objetivos:
#      make              -> compila todo en build/
#      make test         -> ejecuta las suites de tests
#      make bench        -> comparacion experimental de indices
#      make demo         -> demostracion end-to-end por consola
#      make dump_page    -> inspector de los binarios (para el video)
#      make clean        -> borra binarios y datos generados
#
#  El tamano de pagina es configurable (Experimento 4 del enunciado):
#      make PAGE=8192 bench
# ============================================================================
CXX      := g++
PAGE     ?= 4096
PORT     ?= 8080
POOL     ?= 64
CXXFLAGS := -std=c++17 -O2 -Wall -Wextra -Ibackend/include -DDB_PAGE_SIZE=$(PAGE)
LDLIBS   :=

# En Windows (MinGW) el servidor necesita enlazar contra Winsock.
ifeq ($(OS),Windows_NT)
  LDLIBS += -lws2_32
  # cpp-httplib exige Windows 10; MinGW declara una version mas vieja por defecto.
  CXXFLAGS += -D_WIN32_WINNT=0x0A00
endif

BUILD := build

# Las fuentes se descubren solas: agregar un .cpp nuevo no obliga a tocar
# este archivo. Los dos programas con main() se excluyen de la biblioteca.
MAINS   := backend/src/api_server.cpp backend/src/demo.cpp backend/src/dump_page.cpp
LIB_SRC := $(filter-out $(MAINS),$(wildcard backend/src/*.cpp))
LIB_OBJ := $(LIB_SRC:backend/src/%.cpp=$(BUILD)/%.o)

TESTS   := $(patsubst backend/tests/%.cpp,%,$(wildcard backend/tests/*.cpp))
TESTBIN := $(addprefix $(BUILD)/,$(TESTS))
BENCHES := $(patsubst benchmarks/%.cpp,%,$(wildcard benchmarks/*.cpp))
BENCHBIN := $(addprefix $(BUILD)/,$(BENCHES))

.PHONY: all run server test bench demo clean dirs

all: dirs $(BUILD)/dbserver $(BUILD)/demo $(BUILD)/dump_page $(BENCHBIN) $(TESTBIN)

dirs:
	@mkdir -p $(BUILD) data

$(BUILD)/%.o: backend/src/%.cpp
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD)/dbserver: backend/src/api_server.cpp $(LIB_OBJ)
	$(CXX) $(CXXFLAGS) $< $(LIB_OBJ) -o $@ -pthread $(LDLIBS)

$(BUILD)/demo: backend/src/demo.cpp $(LIB_OBJ)
	$(CXX) $(CXXFLAGS) $< $(LIB_OBJ) -o $@

$(BUILD)/dump_page: backend/src/dump_page.cpp $(LIB_OBJ)
	$(CXX) $(CXXFLAGS) $< $(LIB_OBJ) -o $@

$(BUILD)/%: benchmarks/%.cpp $(LIB_OBJ)
	$(CXX) $(CXXFLAGS) $< $(LIB_OBJ) -o $@

$(BUILD)/%: backend/tests/%.cpp $(LIB_OBJ)
	$(CXX) $(CXXFLAGS) -Ibackend/tests $< $(LIB_OBJ) -o $@

run: dirs $(BUILD)/dbserver
	@./$(BUILD)/dbserver --port $(PORT) --data data --static frontend --pool $(POOL)

server: run

test: dirs $(TESTBIN)
	@for t in $(TESTS); do echo ""; echo ">>> $$t"; ./$(BUILD)/$$t || exit 1; done
	@echo ""
	@echo "TODOS LOS TESTS PASARON"

bench: dirs
	@PAGE=$(PAGE) ./benchmarks/run_experimentos.sh

demo: dirs $(BUILD)/demo
	@mkdir -p data/_demo
	@./$(BUILD)/demo data/_demo

clean:
	rm -rf $(BUILD)
	rm -f data/*.dat data/*.idx data/*.ovf data/*.csv data/*.tmp data/catalog.txt
