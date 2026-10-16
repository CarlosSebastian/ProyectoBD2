// ============================================================================
//  test_inverted.cpp - Modulo de texto: analisis lexico, SPIMI y ranking
//
//  La estrategia es la misma que en el resto de la suite: no comprobar que el
//  codigo hace lo que el codigo hace, sino contrastarlo contra una referencia
//  independiente. Aqui las referencias son tres:
//     - para el stemmer ingles, el vocabulario de prueba del propio Porter
//     - para SPIMI, el MISMO indice construido en un solo bloque (si la
//       construccion externa es correcta, el numero de bloques no puede
//       cambiar el resultado)
//     - para BM25 y TF-IDF, la formula calculada a mano sobre el corpus
// ============================================================================
#include <algorithm>
#include <cmath>
#include <map>
#include <random>
#include <set>
#include <string>
#include <vector>

#include "db/buffer_pool.hpp"
#include "db/disk_manager.hpp"
#include "db/inverted.hpp"
#include "db/text.hpp"
#include "test_util.hpp"

using namespace db;

static const char* DIR = "data";

static std::string ruta(const std::string& n) { return std::string(DIR) + "/" + n; }

// --- corpus de juguete, pensado para que los df sean faciles de contar ---
static std::vector<std::string> corpusChico() {
    return {
        "the quick brown fox jumps over the lazy dog",                              // 0
        "inverted indexes accelerate full text search over document collections",   // 1
        "a document collection indexed with an inverted index answers queries",     // 2
        "the lazy dog sleeps while the quick fox runs through the forest",          // 3
        "bm25 ranks documents by term frequency and inverse document frequency",    // 4
        "tf idf weighting and cosine similarity are classical ranking models",      // 5
        "spimi builds the inverted index in a single pass over the documents",      // 6
        "external sorting merges sorted blocks with a heap in one single pass",     // 7
    };
}

// Referencia en memoria: diccionario completo termino -> (doc -> tf).
static std::map<std::string, std::map<std::uint32_t, std::uint32_t>>
referencia(const std::vector<std::string>& docs, texto::Idioma idi,
           std::vector<std::uint32_t>* largos) {
    std::map<std::string, std::map<std::uint32_t, std::uint32_t>> d;
    if (largos) largos->clear();
    for (std::size_t i = 0; i < docs.size(); ++i) {
        const std::vector<std::string> ts = texto::tokenizar(docs[i], idi);
        if (largos) largos->push_back(static_cast<std::uint32_t>(ts.size()));
        for (const std::string& t : ts) ++d[t][static_cast<std::uint32_t>(i)];
    }
    return d;
}

static void construir(InvertedIndex& ix, const std::vector<std::string>& docs,
                      texto::Idioma idi, const std::string& pref, std::size_t presupuesto) {
    std::size_t i = 0;
    ix.construir([&](RID& rid, std::string& s) {
        if (i >= docs.size()) return false;
        rid = RID(static_cast<page_id_t>(i), 0);
        s = docs[i];
        ++i;
        return true;
    }, idi, pref, presupuesto);
}

int main() {
    // =======================================================================
    SECTION("normalizacion: minusculas, tildes y puntuacion");
    CHECK_EQ(texto::normalizar("Año de Ñandú: la COMPILACIÓN, París!"),
             std::string("ano de nandu  la compilacion  paris "), "acentos y signos");
    CHECK_EQ(texto::normalizar("C++ es B2B; 100% útil"),
             std::string("c   es b2b  100  util"), "digitos y simbolos");
    CHECK(texto::normalizar("αβγ emoji").find("emoji") != std::string::npos,
          "el multibyte ajeno se descarta sin romper lo demas");

    // =======================================================================
    SECTION("stemmer de Porter contra el vocabulario de prueba oficial");
    {
        // Pares tomados del articulo de Porter (1980) y de su voc.txt.
        const char* pares[][2] = {
            {"caresses","caress"},{"ponies","poni"},{"ties","ti"},{"caress","caress"},
            {"cats","cat"},{"feed","feed"},{"agreed","agre"},{"plastered","plaster"},
            {"motoring","motor"},{"sing","sing"},{"conflated","conflat"},{"troubled","troubl"},
            {"sized","size"},{"hopping","hop"},{"tanned","tan"},{"falling","fall"},
            {"hissing","hiss"},{"fizzed","fizz"},{"failing","fail"},{"filing","file"},
            {"happy","happi"},{"sky","sky"},{"relational","relat"},{"conditional","condit"},
            {"rational","ration"},{"valenci","valenc"},{"hesitanci","hesit"},
            {"digitizer","digit"},{"conformabli","conform"},{"radicalli","radic"},
            {"differentli","differ"},{"vileli","vile"},{"analogousli","analog"},
            {"vietnamization","vietnam"},{"predication","predic"},{"operator","oper"},
            {"feudalism","feudal"},{"decisiveness","decis"},{"hopefulness","hope"},
            {"callousness","callous"},{"formaliti","formal"},{"sensitiviti","sensit"},
            {"sensibiliti","sensibl"},{"triplicate","triplic"},{"formative","form"},
            {"formalize","formal"},{"electriciti","electr"},{"electrical","electr"},
            {"hopeful","hope"},{"goodness","good"},{"revival","reviv"},{"allowance","allow"},
            {"inference","infer"},{"airliner","airlin"},{"gyroscopic","gyroscop"},
            {"adjustable","adjust"},{"defensible","defens"},{"irritant","irrit"},
            {"replacement","replac"},{"adjustment","adjust"},{"dependent","depend"},
            {"adoption","adopt"},{"homologou","homolog"},{"communism","commun"},
            {"activate","activ"},{"angulariti","angular"},{"homologous","homolog"},
            {"effective","effect"},{"bowdlerize","bowdler"},{"probate","probat"},
            {"rate","rate"},{"cease","ceas"},{"controll","control"},{"roll","roll"},
        };
        for (const auto& p : pares)
            CHECK_EQ(texto::stemIngles(p[0]), std::string(p[1]), std::string("Porter: ") + p[0]);
    }

    // =======================================================================
    SECTION("stemmer espanol: las formas de una misma palabra caen en la misma raiz");
    {
        // No se compara contra Snowball byte a byte (el nuestro trabaja sobre
        // texto ya sin tildes), sino contra lo que de verdad importa para
        // recuperar: que las variantes colapsen.
        const std::vector<std::vector<std::string>> familias = {
            {"cantar", "canto", "cantaba", "cantando", "cantaron", "cantaremos"},
            {"vivir", "vivo", "vivia", "viviendo", "vivieron"},
            {"biblioteca", "bibliotecas"},
            {"indice", "indices"},
            {"consulta", "consultas", "consultar", "consultando"},
        };
        for (const std::vector<std::string>& f : familias) {
            const std::string base = texto::stemEspanol(texto::normalizar(f[0]));
            for (const std::string& p : f)
                CHECK_EQ(texto::stemEspanol(texto::normalizar(p)), base,
                         "familia de '" + f[0] + "': " + p);
        }
        // Singular y plural de un sustantivo NUNCA pueden separarse: es el
        // error que mas se nota al buscar.
        const char* plurales[][2] = {
            {"documento", "documentos"}, {"consulta", "consultas"}, {"pagina", "paginas"},
            {"arbol", "arboles"},        {"indice", "indices"},     {"servicio", "servicios"},
            {"bibliotecario", "bibliotecarios"}, {"estudio", "estudios"}, {"cambio", "cambios"},
            {"motor", "motores"},        {"version", "versiones"},  {"red", "redes"},
            {"mes", "meses"},            {"pais", "paises"},        {"libro", "libros"},
            {"palabra", "palabras"},     {"texto", "textos"},       {"archivo", "archivos"},
            {"clave", "claves"},         {"valor", "valores"},      {"nodo", "nodos"},
        };
        for (const auto& par : plurales) {
            const std::string sing = texto::stemEspanol(texto::normalizar(par[0]));
            const std::string plur = texto::stemEspanol(texto::normalizar(par[1]));
            CHECK_EQ(sing, plur, std::string("singular/plural de ") + par[0]);
        }

        // Dos casos que NO colapsan, documentados a proposito porque son
        // limitaciones reales del metodo y no errores de esta implementacion:
        //
        //   ciudad / ciudades  el sufijo verbal -ad (el imperativo "cantad")
        //                      cae dentro de RV y se come la terminacion del
        //                      sustantivo. Le pasa igual al Snowball oficial.
        //   luz / luces        el plural cambia la z por c. Ningun stemmer que
        //                      solo recorte sufijos puede arreglar eso; haria
        //                      falta un diccionario de formas.
        //
        // Se dejan como test para que la limitacion quede registrada: si algun
        // dia alguien la corrige, este test avisara de que cambio el
        // comportamiento en vez de que el cambio pase inadvertido.
        CHECK(texto::stemEspanol("ciudad") != texto::stemEspanol("ciudades"),
              "ciudad/ciudades: limitacion conocida de Snowball espanol");
        CHECK(texto::stemEspanol("luz") != texto::stemEspanol("luces"),
              "luz/luces: el cambio ortografico z->c escapa a un stemmer de sufijos");
    }

    // =======================================================================
    SECTION("stopwords y tokenizado completo");
    {
        const std::vector<std::string> t = texto::tokenizar(
            "The quick brown fox and the lazy dog", texto::Idioma::EN);
        CHECK(std::find(t.begin(), t.end(), "the") == t.end(), "'the' se descarta");
        CHECK(std::find(t.begin(), t.end(), "and") == t.end(), "'and' se descarta");
        CHECK_EQ(t.size(), static_cast<std::size_t>(5), "quedan quick brown fox lazi dog");

        const std::vector<std::string> e = texto::tokenizar(
            "Los indices invertidos de la biblioteca", texto::Idioma::ES);
        CHECK(std::find(e.begin(), e.end(), "los") == e.end(), "'los' se descarta");
        CHECK(std::find(e.begin(), e.end(), "de") == e.end(), "'de' se descarta");

        // Mismo texto, idiomas distintos: el analisis NO puede coincidir, y de
        // ahi que el idioma tenga que viajar en la cabecera del indice.
        CHECK(texto::tokenizar("la casa de los indices", texto::Idioma::ES) !=
              texto::tokenizar("la casa de los indices", texto::Idioma::EN),
              "el idioma cambia el analisis");
    }

    // =======================================================================
    SECTION("SPIMI: el numero de bloques no cambia el indice resultante");
    {
        const std::vector<std::string> docs = corpusChico();
        std::vector<std::uint32_t> largos;
        const auto ref = referencia(docs, texto::Idioma::EN, &largos);

        // Presupuestos crecientes: con 32 bytes se vuelca casi en cada
        // documento, con 1 MB cabe todo y hay un solo bloque.
        std::vector<int> bloques_vistos;
        for (std::size_t presupuesto : {std::size_t(32), std::size_t(256),
                                        std::size_t(1024), std::size_t(1u << 20)}) {
            const std::string f = ruta("ti_" + std::to_string(presupuesto) + ".idx");
            resetFile(f);
            DiskManager dm(f);
            BufferPool bp(&dm, 16);
            InvertedIndex ix(&bp);
            construir(ix, docs, texto::Idioma::EN, ruta("spimi" + std::to_string(presupuesto)),
                      presupuesto);
            bloques_vistos.push_back(ix.bloquesSPIMI());

            CHECK_EQ(ix.numDocs(), static_cast<std::uint32_t>(docs.size()), "num_docs");
            CHECK_EQ(ix.numTerminos(), static_cast<std::uint32_t>(ref.size()), "num_terminos");

            std::uint64_t total = 0;
            for (std::uint32_t l : largos) total += l;
            CHECK_EQ(ix.totalTokens(), total, "total de tokens");
            CHECK(std::fabs(ix.avgdl() - static_cast<double>(total) / docs.size()) < 1e-9,
                  "avgdl");

            // df de CADA termino del vocabulario, contra la referencia.
            for (const auto& kv : ref)
                CHECK_EQ(ix.df(kv.first), static_cast<std::uint32_t>(kv.second.size()),
                         "df de '" + kv.first + "'");
            CHECK_EQ(ix.df("terminoquenoexiste"), static_cast<std::uint32_t>(0),
                     "un termino ausente tiene df 0");
        }
        CHECK(bloques_vistos.front() > bloques_vistos.back(),
              "un presupuesto chico genera mas bloques que uno grande");
        CHECK_EQ(bloques_vistos.back(), 1, "con 1 MB todo cabe en un solo bloque");
        CHECK(bloques_vistos.front() >= 4, "con 32 bytes se vuelca muchas veces");
    }

    // =======================================================================
    SECTION("BM25 contra la formula calculada aparte");
    {
        const std::vector<std::string> docs = corpusChico();
        std::vector<std::uint32_t> largos;
        const auto ref = referencia(docs, texto::Idioma::EN, &largos);
        const double N = static_cast<double>(docs.size());
        double suma = 0;
        for (std::uint32_t l : largos) suma += l;
        const double avgdl = suma / N;

        const std::string f = ruta("ti_bm25.idx");
        resetFile(f);
        DiskManager dm(f);
        BufferPool bp(&dm, 16);
        InvertedIndex ix(&bp);
        construir(ix, docs, texto::Idioma::EN, ruta("spimi_bm25"), 128);

        const char* consultas[] = {"inverted index", "document frequency", "single pass over",
                                   "lazy dog", "ranking models", "heap"};
        for (const char* q : consultas) {
            // Referencia: puntuar TODOS los documentos a mano.
            std::map<std::uint32_t, double> esperado;
            const std::vector<std::string> tq = texto::tokenizar(q, texto::Idioma::EN);
            const std::set<std::string> distintos(tq.begin(), tq.end());
            for (const std::string& t : distintos) {
                auto it = ref.find(t);
                if (it == ref.end()) continue;
                const double df = static_cast<double>(it->second.size());
                const double idf = std::log(1.0 + (N - df + 0.5) / (df + 0.5));
                for (const auto& dt : it->second) {
                    const double tf = static_cast<double>(dt.second);
                    const double dl = static_cast<double>(largos[dt.first]);
                    const double norma = InvertedIndex::K1 *
                        (1.0 - InvertedIndex::B + InvertedIndex::B * dl / avgdl);
                    esperado[dt.first] += idf * (tf * (InvertedIndex::K1 + 1.0)) / (tf + norma);
                }
            }
            const auto obtenido = ix.buscar(q, -1, InvertedIndex::Ranking::BM25);
            CHECK_EQ(obtenido.size(), esperado.size(), std::string("documentos que coinciden con '") + q + "'");
            for (const auto& res : obtenido) {
                auto it = esperado.find(res.doc_id);
                CHECK(it != esperado.end(), std::string("doc ") + std::to_string(res.doc_id) +
                                            " deberia coincidir con '" + q + "'");
                CHECK(std::fabs(res.score - it->second) < 1e-9,
                      std::string("puntaje BM25 del doc ") + std::to_string(res.doc_id) +
                      " para '" + q + "'");
            }
            // El orden tiene que ser decreciente.
            for (std::size_t i = 1; i < obtenido.size(); ++i)
                CHECK(obtenido[i - 1].score >= obtenido[i].score, "orden decreciente");
            // Y el top-k tiene que ser el prefijo del orden total.
            for (int k = 1; k <= 3 && k <= static_cast<int>(obtenido.size()); ++k) {
                const auto topk = ix.buscar(q, k, InvertedIndex::Ranking::BM25);
                CHECK_EQ(topk.size(), static_cast<std::size_t>(k), "tamano del top-k");
                for (int i = 0; i < k; ++i)
                    CHECK_EQ(topk[i].doc_id, obtenido[i].doc_id,
                             "el top-" + std::to_string(k) + " es el prefijo del orden total");
            }
        }
    }

    // =======================================================================
    SECTION("TF-IDF con coseno: puntajes en [0,1] y la consulta igual al documento da 1");
    {
        const std::vector<std::string> docs = corpusChico();
        const std::string f = ruta("ti_tfidf.idx");
        resetFile(f);
        DiskManager dm(f);
        BufferPool bp(&dm, 16);
        InvertedIndex ix(&bp);
        construir(ix, docs, texto::Idioma::EN, ruta("spimi_tfidf"), 128);

        for (const std::string& d : docs) {
            const auto res = ix.buscar(d, 3, InvertedIndex::Ranking::TFIDF);
            CHECK(!res.empty(), "un documento usado como consulta se encuentra a si mismo");
            for (const auto& x : res)
                CHECK(x.score <= 1.0 + 1e-9 && x.score > 0.0, "coseno dentro de [0,1]");
        }
        // Consultar un documento completo tiene que devolverlo en primer lugar:
        // el coseno entre un vector y si mismo es 1.
        for (std::size_t i = 0; i < docs.size(); ++i) {
            const auto res = ix.buscar(docs[i], 1, InvertedIndex::Ranking::TFIDF);
            CHECK_EQ(res.size(), static_cast<std::size_t>(1), "hay un primer resultado");
            CHECK_EQ(res[0].doc_id, static_cast<std::uint32_t>(i),
                     "el documento " + std::to_string(i) + " es el mas parecido a si mismo");
            CHECK(std::fabs(res[0].score - 1.0) < 1e-6, "coseno consigo mismo = 1");
        }
    }

    // =======================================================================
    SECTION("corpus grande y aleatorio: top-k contra fuerza bruta");
    {
        // Vocabulario con distribucion sesgada (unas pocas palabras muy
        // frecuentes y una cola larga), que es como se comporta el lenguaje
        // de verdad y lo que pone a prueba el IDF.
        std::mt19937 rng(20260211);
        std::vector<std::string> vocab;
        for (int i = 0; i < 400; ++i) vocab.push_back("term" + std::to_string(i));
        std::vector<std::string> docs;
        for (int d = 0; d < 600; ++d) {
            const int largo = 20 + static_cast<int>(rng() % 60);
            std::string s;
            for (int j = 0; j < largo; ++j) {
                // Zipf aproximado: el indice sale de un minimo de tres tiradas.
                std::size_t a = rng() % vocab.size(), b = rng() % vocab.size(), c = rng() % vocab.size();
                s += vocab[std::min(a, std::min(b, c))] + " ";
            }
            docs.push_back(s);
        }
        std::vector<std::uint32_t> largos;
        const auto ref = referencia(docs, texto::Idioma::EN, &largos);
        const double N = static_cast<double>(docs.size());
        double suma = 0;
        for (std::uint32_t l : largos) suma += l;
        const double avgdl = suma / N;

        const std::string f = ruta("ti_grande.idx");
        resetFile(f);
        DiskManager dm(f);
        BufferPool bp(&dm, 32);
        InvertedIndex ix(&bp);
        construir(ix, docs, texto::Idioma::EN, ruta("spimi_grande"), 16 * 1024);
        CHECK(ix.bloquesSPIMI() > 1, "el corpus grande no cabe en un bloque");
        CHECK_EQ(ix.numTerminos(), static_cast<std::uint32_t>(ref.size()), "vocabulario completo");

        for (int consulta = 0; consulta < 40; ++consulta) {
            const int nt = 1 + static_cast<int>(rng() % 4);
            std::string q;
            for (int i = 0; i < nt; ++i) q += vocab[rng() % vocab.size()] + " ";

            std::vector<std::pair<double, std::uint32_t>> fuerza;
            {
                std::map<std::uint32_t, double> acc;
                const std::vector<std::string> tq = texto::tokenizar(q, texto::Idioma::EN);
                const std::set<std::string> distintos(tq.begin(), tq.end());
                for (const std::string& t : distintos) {
                    auto it = ref.find(t);
                    if (it == ref.end()) continue;
                    const double df = static_cast<double>(it->second.size());
                    const double idf = std::log(1.0 + (N - df + 0.5) / (df + 0.5));
                    for (const auto& dt : it->second) {
                        const double tf = static_cast<double>(dt.second);
                        const double dl = static_cast<double>(largos[dt.first]);
                        const double norma = InvertedIndex::K1 *
                            (1.0 - InvertedIndex::B + InvertedIndex::B * dl / avgdl);
                        acc[dt.first] += idf * (tf * (InvertedIndex::K1 + 1.0)) / (tf + norma);
                    }
                }
                for (const auto& kv : acc) fuerza.push_back({kv.second, kv.first});
                std::sort(fuerza.begin(), fuerza.end(),
                          [](const auto& a, const auto& b) {
                              if (a.first != b.first) return a.first > b.first;
                              return a.second < b.second;
                          });
            }

            const int k = 10;
            const auto obtenido = ix.buscar(q, k, InvertedIndex::Ranking::BM25);
            const std::size_t esperados = std::min<std::size_t>(fuerza.size(), k);
            CHECK_EQ(obtenido.size(), esperados, "tamano del top-10");
            for (std::size_t i = 0; i < obtenido.size(); ++i) {
                CHECK(std::fabs(obtenido[i].score - fuerza[i].first) < 1e-9,
                      "puntaje de la posicion " + std::to_string(i));
                CHECK_EQ(obtenido[i].doc_id, fuerza[i].second,
                         "documento de la posicion " + std::to_string(i));
                CHECK_EQ(obtenido[i].rid.page_id, static_cast<page_id_t>(fuerza[i].second),
                         "el RID apunta al documento correcto");
            }
        }
    }

    // =======================================================================
    SECTION("persistencia: reabrir el archivo y volver a consultar");
    {
        const std::vector<std::string> docs = corpusChico();
        const std::string f = ruta("ti_persist.idx");
        std::vector<InvertedIndex::Resultado> antes;
        std::uint32_t nt = 0;
        {
            resetFile(f);
            DiskManager dm(f);
            BufferPool bp(&dm, 8);
            InvertedIndex ix(&bp);
            construir(ix, docs, texto::Idioma::EN, ruta("spimi_persist"), 64);
            antes = ix.buscar("inverted index documents", -1);
            nt = ix.numTerminos();
        }
        {
            DiskManager dm(f);
            BufferPool bp(&dm, 8);
            InvertedIndex ix(&bp);   // se abre leyendo la META del disco
            CHECK_EQ(ix.numDocs(), static_cast<std::uint32_t>(docs.size()), "num_docs tras reabrir");
            CHECK_EQ(ix.numTerminos(), nt, "num_terminos tras reabrir");
            CHECK_EQ(ix.idioma() == texto::Idioma::EN, true, "el idioma sobrevive");
            const auto despues = ix.buscar("inverted index documents", -1);
            CHECK_EQ(despues.size(), antes.size(), "mismos resultados tras reabrir");
            for (std::size_t i = 0; i < antes.size(); ++i) {
                CHECK_EQ(despues[i].doc_id, antes[i].doc_id, "mismo orden tras reabrir");
                CHECK(std::fabs(despues[i].score - antes[i].score) < 1e-12, "mismo puntaje");
            }
        }
    }

    // =======================================================================
    SECTION("un idioma distinto al de la consulta no encuentra nada");
    {
        const std::vector<std::string> docs = {
            "los indices invertidos aceleran la busqueda de informacion textual",
            "la compilacion del indice se hace con el algoritmo spimi en una sola pasada",
            "el ranking bm25 pondera la frecuencia de termino y la longitud del documento",
        };
        const std::string f = ruta("ti_es.idx");
        resetFile(f);
        DiskManager dm(f);
        BufferPool bp(&dm, 8);
        InvertedIndex ix(&bp);
        construir(ix, docs, texto::Idioma::ES, ruta("spimi_es"), 128);
        CHECK_EQ(ix.idioma() == texto::Idioma::ES, true, "el indice guarda ES");
        const auto res = ix.buscar("indices invertidos", 5);
        CHECK(!res.empty(), "la consulta en espanol encuentra documentos en espanol");
        CHECK_EQ(res[0].doc_id, static_cast<std::uint32_t>(0), "el primero es el que habla de eso");
        // 'busqueda' y 'busquedas' tienen que dar el mismo resultado.
        CHECK_EQ(ix.buscar("busqueda", 5).size(), ix.buscar("busquedas", 5).size(),
                 "singular y plural recuperan lo mismo");
    }

    // =======================================================================
    SECTION("casos de borde");
    {
        const std::string f = ruta("ti_borde.idx");
        resetFile(f);
        DiskManager dm(f);
        BufferPool bp(&dm, 8);
        InvertedIndex ix(&bp);

        // Indice recien creado, sin construir.
        CHECK_EQ(ix.numDocs(), static_cast<std::uint32_t>(0), "indice vacio");
        CHECK(ix.buscar("lo que sea", 5).empty(), "consultar un indice vacio no revienta");

        // Corpus vacio.
        construir(ix, {}, texto::Idioma::EN, ruta("spimi_vacio"), 1024);
        CHECK_EQ(ix.numDocs(), static_cast<std::uint32_t>(0), "corpus vacio");
        CHECK_EQ(ix.numTerminos(), static_cast<std::uint32_t>(0), "sin terminos");
        CHECK(ix.buscar("algo", 5).empty(), "sin resultados");

        // Documentos que no dejan ningun termino (solo stopwords y signos).
        construir(ix, {"the and of", "!!! ... ???", "a an it"}, texto::Idioma::EN,
                  ruta("spimi_stop"), 1024);
        CHECK_EQ(ix.numDocs(), static_cast<std::uint32_t>(3), "los documentos se cuentan igual");
        CHECK_EQ(ix.numTerminos(), static_cast<std::uint32_t>(0), "pero no aportan terminos");
        CHECK(ix.buscar("the", 5).empty(), "una consulta de solo stopwords no devuelve nada");

        // Un termino que aparece en TODOS los documentos: con BM25 el IDF se
        // acerca a cero pero no se vuelve negativo, asi que sigue devolviendolos.
        construir(ix, {"alfa beta", "alfa gamma", "alfa delta"}, texto::Idioma::EN,
                  ruta("spimi_todos"), 1024);
        const auto todos = ix.buscar("alfa", -1);
        CHECK_EQ(todos.size(), static_cast<std::size_t>(3), "un termino universal devuelve todo");
        for (const auto& x : todos) CHECK(x.score > 0.0, "y con puntaje positivo");

        // k mayor que el numero de documentos.
        CHECK_EQ(ix.buscar("alfa", 100).size(), static_cast<std::size_t>(3),
                 "k mayor que el corpus devuelve el corpus");
    }

    // =======================================================================
    SECTION("un .idx de otro tipo no se interpreta como indice invertido");
    {
        const std::string f = ruta("ti_ajeno.idx");
        resetFile(f);
        {   // se fabrica una pagina 0 con una firma que no es la nuestra
            DiskManager dm(f);
            BufferPool bp(&dm, 4);
            page_id_t pid;
            char* p = bp.newPage(&pid);
            writeAt<std::int32_t>(p, 16, 0x52545245);   // "RTRE"
            bp.unpinPage(pid, true);
            bp.flushAll();
        }
        DiskManager dm(f);
        BufferPool bp(&dm, 4);
        bool lanzo = false;
        try { InvertedIndex ix(&bp); (void)ix; }
        catch (const DBException&) { lanzo = true; }
        CHECK(lanzo, "abrir un R-Tree como indice invertido tiene que fallar");
    }

    // =======================================================================
    SECTION("la compresion por huecos y varint vale la pena");
    {
        // 2000 documentos cortos sobre un vocabulario chico: listas de
        // postings largas, que es donde la codificacion por huecos luce.
        std::mt19937 rng(7);
        std::vector<std::string> docs;
        for (int d = 0; d < 2000; ++d) {
            std::string s;
            for (int j = 0; j < 12; ++j) s += "w" + std::to_string(rng() % 50) + " ";
            docs.push_back(s);
        }
        const std::string f = ruta("ti_compresion.idx");
        resetFile(f);
        DiskManager dm(f);
        BufferPool bp(&dm, 32);
        InvertedIndex ix(&bp);
        construir(ix, docs, texto::Idioma::EN, ruta("spimi_comp"), 64 * 1024);

        // Cuanto ocuparian los postings sin comprimir: 8 bytes por par.
        std::uint64_t sin_comprimir = 0;
        for (int i = 0; i < 50; ++i)
            sin_comprimir += 8ull * ix.df("w" + std::to_string(i));
        const std::uint64_t con_indice = static_cast<std::uint64_t>(ix.numPages()) * PAGE_SIZE;
        CHECK(con_indice < sin_comprimir,
              "el indice entero ocupa menos que los postings sin comprimir");
        std::cout << "   postings sin comprimir=" << sin_comprimir
                  << " B   indice completo=" << con_indice << " B ("
                  << ix.numPages() << " paginas, " << ix.numTerminos() << " terminos, "
                  << ix.numDocs() << " documentos)\n";
    }

    DONE("test_inverted");
    return 0;
}
