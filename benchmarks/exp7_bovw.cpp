// ============================================================================
//  Experimento 7 - Paradigma clasico (BoVW) contra paradigma moderno
//                  (embeddings densos)
//
//  Las dos representaciones describen LAS MISMAS imagenes y se consultan con
//  LOS MISMOS indices de este motor, asi que la comparacion es limpia:
//
//    BoVW    descriptores SIFT cuantizados con K-Means en K palabras
//            visuales. Cada imagen es un documento ("w17 w17 w3 ...") y se
//            busca con el INDICE INVERTIDO de la Parte 3, puntuando con
//            TF-IDF y similitud de coseno. Es, literalmente, recuperacion de
//            texto aplicada a imagenes.
//    Denso   un vector de 512 dimensiones de una red preentrenada, buscado
//            con el IVF de la Parte 4.
//
//  Lo que se mide es EFECTIVIDAD, no velocidad: Precision@5 y Precision@10,
//  usando la clase de la imagen como verdad de referencia. Dos imagenes son
//  "relevantes" entre si si son de la misma clase.
//
//  Por que la comparacion es justa aunque los dos indices sean distintos: la
//  pregunta del experimento no es cual indice es mas rapido --- eso lo mide el
//  Experimento 8 --- sino cual REPRESENTACION encuentra mejor. Un BoVW perfecto
//  con un indice lento seguiria siendo mejor representacion.
//
//  Los datos los prepara benchmarks/bovw_preparar.py, que es donde vive el
//  SIFT, el K-Means del codebook y la red. Aqui solo se indexa y se consulta.
//
//     python benchmarks/bovw_preparar.py --sintetico 800 --salida data/_bovw
//     ./build/exp7_bovw data/_bovw
//
//  AVISO sobre --sintetico: sirve para comprobar que la tuberia entera corre,
//  no para el informe. Los numeros de efectividad de un dataset inventado
//  dependen de como se invento, no de las representaciones. Para el informe
//  hay que pasarle las imagenes de verdad.
// ============================================================================
#include <cmath>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

#include "comun.hpp"
#include "db/inverted.hpp"
#include "db/ivf.hpp"

using namespace bench;
using namespace db;

namespace {

struct Imagen {
    int                id = 0;
    std::string        archivo, clase, palabras;
    std::vector<float> emb;
};

// Lector de CSV con comillas. El del motor vive dentro de database.cpp y no
// esta expuesto, y aqui hace falta uno de todas formas para quedarse con los
// vectores, que por la API de consulta solo se ven abreviados.
bool leerFila(std::istream& in, std::vector<std::string>* campos) {
    campos->clear();
    std::string campo;
    bool comillas = false, algo = false;
    int c;
    while ((c = in.get()) != EOF) {
        algo = true;
        if (comillas) {
            if (c == '"') {
                if (in.peek() == '"') { campo += '"'; in.get(); }
                else                    comillas = false;
            } else {
                campo += static_cast<char>(c);
            }
        } else if (c == '"') {
            comillas = true;
        } else if (c == ',') {
            campos->push_back(campo);
            campo.clear();
        } else if (c == '\n') {
            if (!campo.empty() && campo.back() == '\r') campo.pop_back();
            campos->push_back(campo);
            return true;
        } else {
            campo += static_cast<char>(c);
        }
    }
    if (!algo) return false;
    campos->push_back(campo);
    return true;
}

std::vector<float> floatsDe(const std::string& s) {
    std::vector<float> v;
    std::istringstream ss(s);
    double x;
    while (ss >> x) v.push_back(static_cast<float>(x));
    return v;
}

TableInfo infoPalabras(const std::string& n) {
    TableInfo ti;
    ti.name      = n;
    ti.schema    = Schema({Column("id", Type::INT), Column("archivo", Type::VARCHAR, 40),
                           Column("clase", Type::VARCHAR, 20),
                           Column("palabras", Type::VARCHAR, 0)});
    ti.heap_file = n + ".dat";
    ti.engine    = "HEAP";
    IndexInfo ix;
    ix.column  = "palabras";
    ix.kind    = IndexKind::INVERTED;
    ix.file    = n + ".idx";
    ix.idioma  = "EN";
    ix.scoring = "TFIDF_COSINE";
    ti.indexes.push_back(ix);
    return ti;
}

TableInfo infoEmb(const std::string& n, int dim) {
    TableInfo ti;
    ti.name      = n;
    ti.schema    = Schema({Column("id", Type::INT), Column("archivo", Type::VARCHAR, 40),
                           Column("clase", Type::VARCHAR, 20),
                           Column("emb", Type::VECTOR, dim)});
    ti.heap_file = n + ".dat";
    ti.engine    = "HEAP";
    IndexInfo ix;
    ix.column  = "emb";
    ix.kind    = IndexKind::IVF;
    ix.file    = n + ".idx";
    ix.listas  = 0;                 // 0 => raiz de N
    ix.metrica = "COSINE";
    ti.indexes.push_back(ix);
    return ti;
}

// Precision@k: de los k resultados (sin contar la propia consulta), cuantos
// son de la misma clase. Es la medida que pide el enunciado y la unica que
// tiene sentido cuando la verdad de referencia son clases y no un ranking.
double precisionEn(const std::vector<Tuple>& res, int col_clase, int col_id,
                   const std::string& clase_q, int id_q, int k) {
    int vistos = 0, aciertos = 0;
    for (const Tuple& t : res) {
        if (static_cast<int>(t.at(static_cast<std::size_t>(col_id)).i) == id_q) continue;
        if (vistos >= k) break;
        ++vistos;
        if (t.at(static_cast<std::size_t>(col_clase)).s == clase_q) ++aciertos;
    }
    return vistos == 0 ? 0.0 : static_cast<double>(aciertos) / static_cast<double>(k);
}

}  // namespace

int main(int argc, char** argv) {
    const std::string dir = argc > 1 ? argv[1] : "data/_bovw";
    const int PROBES      = argc > 2 ? std::atoi(argv[2]) : 0;   // 0 => el del indice

    titulo("EXPERIMENTO 7 - BoVW (clasico) contra embeddings densos (moderno)");

    // ---------------- cargar lo que preparo el script ----------------
    std::map<int, Imagen> imgs;
    {
        std::ifstream f(dir + "/bovw_palabras.csv");
        if (!f) {
            std::cerr << "\nNo esta " << dir << "/bovw_palabras.csv\n"
                         "Corre antes:  python benchmarks/bovw_preparar.py --sintetico 800 "
                         "--salida " << dir << "\n\n";
            return 1;
        }
        std::vector<std::string> c;
        leerFila(f, &c);                                  // cabecera
        while (leerFila(f, &c)) {
            if (c.size() != 4) continue;
            Imagen im;
            im.id       = std::atoi(c[0].c_str());
            im.archivo  = c[1];
            im.clase    = c[2];
            im.palabras = c[3];
            imgs[im.id] = im;
        }
    }
    {
        std::ifstream f(dir + "/bovw_emb.csv");
        std::vector<std::string> c;
        leerFila(f, &c);
        while (leerFila(f, &c)) {
            if (c.size() != 4) continue;
            const int id = std::atoi(c[0].c_str());
            auto it = imgs.find(id);
            if (it != imgs.end()) it->second.emb = floatsDe(c[3]);
        }
    }
    std::vector<std::pair<int, std::string>> consultas;
    {
        std::ifstream f(dir + "/bovw_consultas.txt");
        std::string linea;
        while (std::getline(f, linea)) {
            const std::size_t tab = linea.find('\t');
            if (tab == std::string::npos) continue;
            consultas.emplace_back(std::atoi(linea.substr(0, tab).c_str()),
                                   linea.substr(tab + 1));
        }
    }
    double ms_extraccion = 0.0, ms_codebook = 0.0;
    int k_codebook = 0;
    {
        std::ifstream f(dir + "/bovw_tiempos.txt");
        std::string clave;
        double v;
        while (f >> clave >> v) {
            if      (clave == "extraccion_ms") ms_extraccion = v;
            else if (clave == "codebook_ms")   ms_codebook   = v;
            else if (clave == "k")             k_codebook    = static_cast<int>(v);
        }
    }

    if (imgs.empty() || consultas.empty()) {
        std::cerr << "\nFaltan datos: " << imgs.size() << " imagenes, "
                  << consultas.size() << " consultas.\n\n";
        return 1;
    }
    const int dim = imgs.begin()->second.emb.empty()
                  ? 0 : static_cast<int>(imgs.begin()->second.emb.size());
    if (dim == 0) { std::cerr << "\nLos embeddings estan vacios.\n\n"; return 1; }

    std::set<std::string> clases;
    for (const auto& kv : imgs) clases.insert(kv.second.clase);
    std::cout << "\n" << imgs.size() << " imagenes, " << clases.size() << " clases, "
              << consultas.size() << " consultas.\n"
              << "BoVW: codebook de " << k_codebook << " palabras visuales (SIFT + K-Means).\n"
              << "Denso: " << dim << " dimensiones.\n";

    // ---------------- construir ----------------
    std::cout << "\n### A. Construccion de las dos representaciones\n\n";
    limpiar(dir, "bovw_pal");
    limpiar(dir, "bovw_emb");
    Table t_pal(dir, infoPalabras("bovw_pal"), 64);
    Table t_emb(dir, infoEmb("bovw_emb", dim), 64);

    Cronometro c_ins;
    for (const auto& kv : imgs) {
        const Imagen& im = kv.second;
        t_pal.insert(Tuple{{Value::makeInt(im.id), Value::makeStr(im.archivo),
                            Value::makeStr(im.clase), Value::makeStr(im.palabras)}});
        t_emb.insert(Tuple{{Value::makeInt(im.id), Value::makeStr(im.archivo),
                            Value::makeStr(im.clase), Value::makeVector(im.emb)}});
    }
    const double ms_insert = c_ins.parar().ms;

    Cronometro c1;
    t_pal.reconstruirTexto();
    const double ms_inv = c1.parar().ms;
    Cronometro c2;
    t_emb.reconstruirVector();
    const double ms_ivf = c2.parar().ms;
    t_pal.flush();
    t_emb.flush();

    const InvertedIndex* inv = t_pal.indiceTexto("palabras");
    const IVFIndex*      ivf = t_emb.indiceVector("emb");

    fila({"representacion", "extraccion (ms)", "codebook/-- (ms)", "indexar (ms)",
          "pag. datos", "pag. indice", "detalle"});
    separador(7);
    fila({"BoVW + invertido", num(ms_extraccion, 0), num(ms_codebook, 0), num(ms_inv, 0),
          std::to_string(t_pal.dataPages()), std::to_string(t_pal.indexPages()),
          inv ? std::to_string(inv->numTerminos()) + " palabras visuales usadas" : "-"});
    fila({"Denso + IVF", "(red)", "--", num(ms_ivf, 0),
          std::to_string(t_emb.dataPages()), std::to_string(t_emb.indexPages()),
          ivf ? std::to_string(ivf->listas()) + " celdas" : "-"});
    std::cout << "\nInsertar las " << imgs.size() << " filas en las dos tablas: "
              << num(ms_insert, 0) << " ms.\n"
              << "El BoVW paga su costo ANTES de la consulta: extraer los descriptores y\n"
                 "entrenar el codebook es lo caro, y hay que rehacerlo si cambia la\n"
                 "coleccion. El denso paga una pasada de red por imagen y ya.\n";

    // ---------------- efectividad ----------------
    std::cout << "\n### B. Efectividad: Precision@5 y Precision@10\n\n";
    const int col_id = 0, col_clase = 2;

    double p5_b = 0, p10_b = 0, ms_b = 0, ac_b = 0;
    double p5_d = 0, p10_d = 0, ms_d = 0, ac_d = 0;
    int se_encuentra_b = 0, se_encuentra_d = 0;

    for (const auto& q : consultas) {
        const Imagen& im = imgs[q.first];

        t_pal.resetStats();
        Cronometro cb;
        const std::vector<Tuple> rb = t_pal.searchMatch("palabras", im.palabras, 11, true);
        const Medicion mb = cb.parar();
        ms_b += mb.ms;
        ac_b += static_cast<double>(mb.accesos);
        p5_b  += precisionEn(rb, col_clase, col_id, im.clase, im.id, 5);
        p10_b += precisionEn(rb, col_clase, col_id, im.clase, im.id, 10);
        if (!rb.empty() && static_cast<int>(rb[0].at(col_id).i) == im.id) ++se_encuentra_b;

        t_emb.resetStats();
        Cronometro cd;
        const std::vector<Tuple> rd =
            t_emb.searchVector("emb", im.emb, 11, true, PROBES > 0 ? PROBES : -1);
        const Medicion md = cd.parar();
        ms_d += md.ms;
        ac_d += static_cast<double>(md.accesos);
        p5_d  += precisionEn(rd, col_clase, col_id, im.clase, im.id, 5);
        p10_d += precisionEn(rd, col_clase, col_id, im.clase, im.id, 10);
        if (!rd.empty() && static_cast<int>(rd[0].at(col_id).i) == im.id) ++se_encuentra_d;
    }
    const double n = static_cast<double>(consultas.size());

    fila({"representacion", "indice", "P@5", "P@10", "ms/consulta",
          "accesos/consulta", "se encuentra a si misma"});
    separador(7);
    fila({"BoVW (SIFT + K-Means)", "invertido TF-IDF", num(p5_b / n, 4), num(p10_b / n, 4),
          num(ms_b / n, 3), num(ac_b / n, 1),
          std::to_string(se_encuentra_b) + "/" + std::to_string(consultas.size())});
    fila({"Embeddings densos", "IVF coseno", num(p5_d / n, 4), num(p10_d / n, 4),
          num(ms_d / n, 3), num(ac_d / n, 1),
          std::to_string(se_encuentra_d) + "/" + std::to_string(consultas.size())});

    std::cout <<
      "\nLa ultima columna es una comprobacion de cordura, no un resultado: la imagen\n"
      "de consulta esta dentro de la coleccion, asi que tiene que salir primera en\n"
      "las dos rutas. Si no saliera, el error estaria en la indexacion y no en la\n"
      "representacion, y el resto de la tabla no significaria nada.\n\n"
      "Lo que se espera ver, y es el resultado clasico de la literatura: el BoVW\n"
      "pierde contra los embeddings densos por un margen amplio. La razon es que\n"
      "cuantizar un descriptor a la palabra visual mas cercana TIRA informacion ---\n"
      "dos parches parecidos que caen en celdas distintas dejan de parecerse, y dos\n"
      "parches distintos que caen en la misma pasan a ser identicos --- y ademas el\n"
      "histograma olvida donde estaba cada cosa en la imagen. El embedding de una red\n"
      "entrenada no cuantiza nada y ademas fue optimizado justamente para que la\n"
      "distancia signifique semejanza semantica.\n\n"
      "Lo que el BoVW si conserva: no necesita GPU ni un modelo preentrenado, el\n"
      "codebook se entrena con los propios datos, y cada resultado se puede explicar\n"
      "--- 'estas dos imagenes comparten estas palabras visuales' --- mientras que un\n"
      "embedding denso es opaco. Y, para este motor, tiene una propiedad que vale la\n"
      "pena remarcar: no hizo falta anadir NADA. Un histograma de palabras visuales\n"
      "es un documento, y el indice invertido de la Parte 3 lo indexa igual que un\n"
      "abstract de arXiv.\n\n";
    return 0;
}
