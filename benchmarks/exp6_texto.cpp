// ============================================================================
//  Experimento 6 - Modulo de texto: construccion SPIMI y consulta por relevancia
//
//  Mide las tres cosas que distinguen a un indice invertido de un escaneo:
//
//    A. Construccion externa. Como crecen tiempo, escrituras y tamano del
//       indice con el numero de documentos, y cuantos BLOQUES genera SPIMI.
//       El numero de bloques es la variable de control del algoritmo: es lo
//       que decide cuanta memoria se usa.
//    B. Presupuesto de memoria. Con N fijo se varia el presupuesto y se
//       observa el intercambio clasico: menos RAM, mas bloques que mezclar.
//       Lo que hay que ver es que el RESULTADO no cambia, solo el costo.
//    C. Consulta. BM25 por el indice frente a la unica alternativa sin
//       indice: leer los N documentos, tokenizarlos y puntuarlos. Las dos
//       rutas tienen que devolver EL MISMO top-10, o la comparacion no vale.
//
//  El corpus es sintetico pero con vocabulario ZIPFIANO, que es lo que de
//  verdad importa: en lenguaje natural unas pocas palabras aparecen en casi
//  todos los documentos y la mayoria aparece en muy pocos. Un corpus con
//  palabras equiprobables daria listas de postings todas iguales y escondería
//  justo el efecto que el IDF explota.
//
//     ./build/exp6_texto [dir] [N_max] [consultas]
// ============================================================================
#include <cmath>
#include <map>
#include <set>
#include <tuple>

#include "comun.hpp"
#include "db/inverted.hpp"
#include "db/text.hpp"

using namespace bench;
using namespace db;

namespace {

constexpr int VOCABULARIO = 20000;

// Zipf aproximado por el minimo de varias tiradas uniformes: barato, estable y
// suficiente para que la frecuencia de las palabras caiga como una potencia.
int palabraZipf(std::mt19937& rng) {
    int mejor = VOCABULARIO;
    for (int i = 0; i < 4; ++i) {
        const int c = static_cast<int>(rng() % VOCABULARIO);
        if (c < mejor) mejor = c;
    }
    return mejor;
}

std::vector<std::string> generarCorpus(int N, unsigned semilla = 2026) {
    std::mt19937 rng(semilla);
    std::vector<std::string> docs;
    docs.reserve(static_cast<std::size_t>(N));
    for (int d = 0; d < N; ++d) {
        const int largo = 60 + static_cast<int>(rng() % 80);
        std::string s;
        s.reserve(static_cast<std::size_t>(largo) * 7);
        for (int j = 0; j < largo; ++j) {
            s += "voc";
            s += std::to_string(palabraZipf(rng));
            s += ' ';
        }
        docs.push_back(std::move(s));
    }
    return docs;
}

TableInfo infoTexto(const std::string& nombre, bool con_indice) {
    TableInfo ti;
    ti.name      = nombre;
    ti.schema    = Schema({Column("id", Type::INT), Column("cuerpo", Type::VARCHAR, 0)});
    ti.heap_file = nombre + ".dat";
    ti.engine    = "HEAP";
    ti.key_column = "id";
    if (con_indice) {
        IndexInfo ix;
        ix.column = "cuerpo";
        ix.kind   = IndexKind::INVERTED;
        ix.file   = nombre + ".idx";
        ix.idioma = "EN";
        ti.indexes.push_back(ix);
    }
    return ti;
}

// Carga el corpus en una tabla SIN indice (para no pagar la construccion aqui).
void cargar(Table& t, const std::vector<std::string>& docs) {
    for (std::size_t i = 0; i < docs.size(); ++i) {
        Tuple tup;
        tup.values.push_back(Value::makeInt(static_cast<std::int64_t>(i)));
        tup.values.push_back(Value::makeStr(docs[i]));
        t.insert(tup);
    }
    t.flush();
}

// ---------------------------------------------------------------------------
//  La alternativa sin indice: leer TODO, tokenizar TODO y puntuar.
//
//  Se hace en una sola pasada sobre el heap. De cada documento solo se guarda
//  su longitud y la frecuencia de los terminos DE LAS CONSULTAS, que es lo
//  minimo imprescindible: guardar el documento entero seria hacer trampa al
//  reves, inflando el costo del rival.
// ---------------------------------------------------------------------------
// (puntaje, posicion en el recorrido del heap, id de la fila)
using Puntuado = std::tuple<double, std::uint32_t, std::uint32_t>;
struct TopK { std::vector<Puntuado> v; };

std::vector<TopK> bruta(Table& t, const std::vector<std::vector<std::string>>& consultas,
                        int k, texto::Idioma idi) {
    std::set<std::string> interes;
    for (const auto& q : consultas) for (const std::string& x : q) interes.insert(x);

    // El id REAL de la fila, no su posicion en el recorrido: el heap no
    // devuelve los registros en el orden en que se insertaron (reutiliza
    // huecos), asi que confundir una cosa con la otra haria que las dos rutas
    // pareciesen discrepar cuando en realidad dan lo mismo.
    struct Parcial { std::uint32_t id; std::uint32_t largo; std::map<std::string, std::uint32_t> tf; };
    std::vector<Parcial> parciales;
    std::map<std::string, std::uint32_t> df;
    std::uint64_t total_tokens = 0;

    for (const Tuple& fila : t.scan()) {
        const std::vector<std::string> ts = texto::tokenizar(fila.at(1).s, idi);
        Parcial p;
        p.id    = static_cast<std::uint32_t>(fila.at(0).i);
        p.largo = static_cast<std::uint32_t>(ts.size());
        total_tokens += ts.size();
        for (const std::string& x : ts)
            if (interes.count(x)) ++p.tf[x];
        for (const auto& kv : p.tf) ++df[kv.first];
        parciales.push_back(std::move(p));
    }

    const double N = static_cast<double>(parciales.size());
    const double avgdl = N > 0 ? static_cast<double>(total_tokens) / N : 0.0;

    std::vector<TopK> salida;
    for (const auto& q : consultas) {
        std::set<std::string> distintos(q.begin(), q.end());
        std::vector<Puntuado> puntuados;
        for (std::size_t d = 0; d < parciales.size(); ++d) {
            double score = 0.0;
            for (const std::string& x : distintos) {
                auto it = parciales[d].tf.find(x);
                if (it == parciales[d].tf.end()) continue;
                const double dfx = static_cast<double>(df[x]);
                const double idf = std::log(1.0 + (N - dfx + 0.5) / (dfx + 0.5));
                const double tf  = static_cast<double>(it->second);
                const double nrm = InvertedIndex::K1 *
                    (1.0 - InvertedIndex::B + InvertedIndex::B * parciales[d].largo / avgdl);
                score += idf * (tf * (InvertedIndex::K1 + 1.0)) / (tf + nrm);
            }
            if (score > 0.0)
                puntuados.push_back({score, static_cast<std::uint32_t>(d), parciales[d].id});
        }
        // Desempate por la POSICION en el recorrido del heap, no por el id.
        // El indice recorre sus listas en orden de doc_id interno, que es el
        // orden en que SPIMI vio los documentos, o sea el mismo recorrido del
        // heap que se hace aqui. Desempatar por el id de la fila daria otro
        // resultado en el ultimo puesto cuando dos documentos empatan, y
        // pareceria un error del motor cuando solo es un criterio distinto.
        std::sort(puntuados.begin(), puntuados.end(),
                  [](const auto& a, const auto& b) {
                      if (std::get<0>(a) != std::get<0>(b)) return std::get<0>(a) > std::get<0>(b);
                      return std::get<1>(a) < std::get<1>(b);
                  });
        if (static_cast<int>(puntuados.size()) > k)
            puntuados.resize(static_cast<std::size_t>(k));
        salida.push_back(TopK{puntuados});
    }
    return salida;
}

}  // namespace

int main(int argc, char** argv) {
    const std::string dir = argc > 1 ? argv[1] : "data";
    const int N_MAX       = argc > 2 ? std::atoi(argv[2]) : 50000;
    const int N_CONSULTAS = argc > 3 ? std::atoi(argv[3]) : 20;

    titulo("EXPERIMENTO 6 - Modulo de texto (indice invertido SPIMI / BM25)");
    std::cout << "Corpus sintetico con vocabulario zipfiano de " << VOCABULARIO
              << " palabras, 60-140 terminos por documento, semilla fija.\n";

    // ======================================================================
    std::cout << "\n### A. Construccion SPIMI segun el numero de documentos\n\n";
    std::cout << "Presupuesto de memoria fijo en 4 MB.\n\n";
    fila({"N docs", "tokens", "terminos", "bloques", "construir (ms)",
          "escrituras", "pag. indice", "indice (KB)", "postings sin compr. (KB)"});
    separador(9);

    std::vector<int> tamanos;
    for (int n = 1000; n <= N_MAX; n *= 5) tamanos.push_back(n);
    if (tamanos.empty() || tamanos.back() != N_MAX) tamanos.push_back(N_MAX);

    for (int N : tamanos) {
        const std::string nom = "exp6_a";
        limpiar(dir, nom);
        const std::vector<std::string> docs = generarCorpus(N);
        {
            Table t(dir, infoTexto(nom, false), 64);
            cargar(t, docs);
        }
        Table t(dir, infoTexto(nom, true), 64);
        Cronometro c;
        t.reconstruirTexto();
        const Medicion m = c.parar();
        t.flush();

        const InvertedIndex* inv = t.indiceTexto("cuerpo");
        // Cuanto ocuparian los postings guardando cada par (doc_id, tf) como
        // dos enteros de 32 bits, que es la alternativa sin comprimir. La suma
        // de los df de todo el vocabulario es exactamente el numero de pares.
        std::uint64_t sin_comprimir = 0;
        for (int i = 0; i < VOCABULARIO; ++i) {
            const std::uint32_t d = inv->df("voc" + std::to_string(i));
            sin_comprimir += 8ull * d;
        }

        fila({std::to_string(N),
              std::to_string(inv->totalTokens()),
              std::to_string(inv->numTerminos()),
              std::to_string(inv->bloquesSPIMI()),
              num(m.ms, 1),
              std::to_string(m.escrituras),
              std::to_string(inv->numPages()),
              num(static_cast<double>(inv->numPages()) * PAGE_SIZE / 1024.0, 1),
              num(static_cast<double>(sin_comprimir) / 1024.0, 1)});
    }
    std::cout << "\nEl tiempo crece de forma practicamente lineal con el numero de\n"
                 "documentos: SPIMI los recorre una sola vez. El indice completo (que\n"
                 "incluye diccionario y tabla de documentos) pesa menos que los\n"
                 "postings sin comprimir, gracias a los huecos y el varint.\n";

    // ======================================================================
    const int N_FIJO = std::min(N_MAX, 20000);
    std::cout << "\n### B. Efecto del presupuesto de memoria (N = " << N_FIJO << ")\n\n";
    fila({"presupuesto", "bloques", "construir (ms)", "escrituras", "terminos", "pag. indice"});
    separador(6);
    {
        const std::string nom = "exp6_b";
        limpiar(dir, nom);
        const std::vector<std::string> docs = generarCorpus(N_FIJO);
        {
            Table t(dir, infoTexto(nom, false), 64);
            cargar(t, docs);
        }
        for (std::size_t pres : {std::size_t(128u * 1024), std::size_t(512u * 1024),
                                 std::size_t(2u << 20), std::size_t(8u << 20),
                                 std::size_t(64u << 20)}) {
            Table t(dir, infoTexto(nom, true), 64);
            Cronometro c;
            t.reconstruirTexto(pres);
            const Medicion m = c.parar();
            t.flush();
            const InvertedIndex* inv = t.indiceTexto("cuerpo");
            fila({num(static_cast<double>(pres) / 1024.0 / 1024.0, 2) + " MB",
                  std::to_string(inv->bloquesSPIMI()),
                  num(m.ms, 1),
                  std::to_string(m.escrituras),
                  std::to_string(inv->numTerminos()),
                  std::to_string(inv->numPages())});
        }
    }
    std::cout << "\nEl numero de terminos y el tamano del indice NO cambian: el\n"
                 "presupuesto solo decide cuantas veces se vuelca la memoria y, por\n"
                 "tanto, cuantos bloques hay que mezclar al final. Esa es exactamente\n"
                 "la promesa de SPIMI: el resultado es independiente de la RAM\n"
                 "disponible, y la RAM solo compra velocidad.\n";

    // ======================================================================
    std::cout << "\n### C. Consulta: indice invertido contra escaneo completo\n\n";
    std::cout << "BM25, top-10, " << N_CONSULTAS << " consultas de 3 terminos cada una.\n\n";
    fila({"N docs", "indice: accesos", "indice: ms", "scan: accesos", "scan: ms",
          "accesos x", "ms x", "top-10 identico"});
    separador(8);

    for (int N : tamanos) {
        const std::string nom = "exp6_c";
        limpiar(dir, nom);
        const std::vector<std::string> docs = generarCorpus(N);
        {
            Table t(dir, infoTexto(nom, false), 64);
            cargar(t, docs);
        }
        Table t(dir, infoTexto(nom, true), 64);
        t.reconstruirTexto();
        t.flush();

        std::mt19937 rng(99);
        std::vector<std::string> textos;
        std::vector<std::vector<std::string>> analizadas;
        for (int i = 0; i < N_CONSULTAS; ++i) {
            std::string q;
            for (int j = 0; j < 3; ++j) q += "voc" + std::to_string(palabraZipf(rng)) + " ";
            textos.push_back(q);
            analizadas.push_back(texto::tokenizar(q, texto::Idioma::EN));
        }

        // --- por el indice ---
        std::vector<std::vector<std::uint32_t>> top_indice;
        std::vector<std::vector<double>>        score_indice;
        Cronometro ci;
        for (const std::string& q : textos) {
            std::vector<double> ps;
            const std::vector<Tuple> filas = t.searchMatch("cuerpo", q, 10, false, &ps);
            std::vector<std::uint32_t> ids;
            for (const Tuple& f : filas) ids.push_back(static_cast<std::uint32_t>(f.at(0).i));
            top_indice.push_back(ids);
            score_indice.push_back(ps);
        }
        const Medicion mi = ci.parar();

        // --- por escaneo ---
        Cronometro cs;
        const std::vector<TopK> top_scan = bruta(t, analizadas, 10, texto::Idioma::EN);
        const Medicion ms = cs.parar();

        // Comparacion de las dos rutas.
        //
        // Lo que TIENE que coincidir es la secuencia de PUNTAJES: si las dos
        // rutas calculan el mismo BM25 y ordenan igual, el ranking es el
        // mismo. Los identificadores pueden diferir en el ultimo puesto cuando
        // dos documentos empatan exactamente: ahi cualquiera de los dos es una
        // respuesta correcta, y exigir uno concreto seria inventarse un
        // requisito. Se cuentan aparte para que el empate se vea.
        bool mismos_puntajes = true;
        long long empates = 0;
        for (std::size_t i = 0; i < top_indice.size() && mismos_puntajes; ++i) {
            if (top_indice[i].size() != top_scan[i].v.size()) { mismos_puntajes = false; break; }
            for (std::size_t j = 0; j < top_indice[i].size(); ++j) {
                if (std::fabs(score_indice[i][j] - std::get<0>(top_scan[i].v[j])) > 1e-9) {
                    mismos_puntajes = false;
                    break;
                }
                if (top_indice[i][j] != std::get<2>(top_scan[i].v[j])) ++empates;
            }
        }
        const std::string veredicto =
            !mismos_puntajes ? "NO"
            : empates == 0   ? "si"
                             : "si (" + std::to_string(empates) + " empate/s)";

        fila({std::to_string(N),
              std::to_string(mi.accesos), num(mi.ms, 1),
              std::to_string(ms.accesos), num(ms.ms, 1),
              mi.accesos > 0 ? num(static_cast<double>(ms.accesos) / mi.accesos, 1) + "x" : "-",
              mi.ms > 0 ? num(ms.ms / mi.ms, 1) + "x" : "-",
              veredicto});
    }
    std::cout << "\nLa ultima columna es la que da sentido a las demas: si los puntajes no\n"
                 "coincidieran, comparar tiempos no significaria nada. Un \"empate\" es un\n"
                 "par de documentos con BM25 identico en el ultimo puesto: las dos rutas\n"
                 "eligen uno distinto y las dos tienen razon. El escaneo paga\n"
                 "leer y tokenizar los N documentos en cada consulta; el indice solo\n"
                 "lee las listas de los terminos pedidos, cuya longitud depende del df\n"
                 "y no de N.\n";

    // ======================================================================
    std::cout << "\n### D. Costo de la consulta segun la frecuencia del termino\n\n";
    {
        const std::string nom = "exp6_d";
        limpiar(dir, nom);
        const int N = std::min(N_MAX, 20000);
        const std::vector<std::string> docs = generarCorpus(N);
        {
            Table t(dir, infoTexto(nom, false), 64);
            cargar(t, docs);
        }
        Table t(dir, infoTexto(nom, true), 64);
        t.reconstruirTexto();
        t.flush();
        const InvertedIndex* inv = t.indiceTexto("cuerpo");

        fila({"termino", "df", "% del corpus", "accesos a pagina", "ms", "resultados"});
        separador(6);
        for (int w : {0, 1, 10, 100, 1000, 5000, 15000}) {
            const std::string term = "voc" + std::to_string(w);
            const std::uint32_t d = inv->df(term);
            Cronometro c;
            std::vector<double> ps;
            const std::size_t n = t.searchMatch("cuerpo", term, 10, false, &ps).size();
            const Medicion m = c.parar();
            fila({term, std::to_string(d),
                  num(100.0 * d / std::max(1, N), 2) + " %",
                  std::to_string(m.accesos), num(m.ms, 3), std::to_string(n)});
        }
        std::cout << "\nUn termino muy frecuente obliga a leer una lista de postings larga\n"
                     "y a puntuar muchos documentos; uno raro se resuelve con unas pocas\n"
                     "paginas. Es el motivo por el que quitar stopwords importa: son\n"
                     "justo los terminos con df cercano a N, los mas caros y los que menos\n"
                     "discriminan.\n";
    }

    std::cout << "\n";
    return 0;
}
