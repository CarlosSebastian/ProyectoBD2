// ============================================================================
//  Experimento 7 - Modulo vectorial: IVF, recall y latencia
//
//  Un indice vectorial no se juzga como los otros cuatro. El B+, el hash y el
//  R-Tree son EXACTOS: lo unico que se mide en ellos es el costo. El IVF es
//  APROXIMADO, asi que tiene dos ejes y hay que mirarlos juntos: lo que cuesta
//  una consulta y cuanto acierta. Un indice rapidisimo con recall 0.3 no sirve,
//  y uno con recall 1.0 que lee todas las celdas no es un indice.
//
//  Se miden cuatro cosas:
//
//    A. Construccion. Como crecen tiempo, escrituras y paginas con N, y como
//       queda el reparto de las celdas. El reparto importa porque el costo de
//       una consulta es "leer las nprobe celdas mas cercanas": una celda
//       gigante es una lectura larga que ninguna consulta puede evitar.
//    B. Numero de celdas (nlist). Con N fijo se varia nlist y se observa el
//       compromiso: mas celdas = celdas mas chicas = consultas mas baratas,
//       pero tambien mas centroides contra los que comparar siempre y mas
//       riesgo de que el vecino verdadero caiga en una celda no visitada.
//    C. LA CURVA: recall@10 frente a nprobe, con su costo. Es exactamente lo
//       que pide el enunciado. La verdad de referencia la da buscarExacto(),
//       fuerza bruta sobre el mismo indice: sin una referencia exacta la
//       palabra "aproximado" no significa nada.
//    D. Dimension. Por que un embedding de 384 cambia el diseno: cuantos
//       bytes pesa una entrada, cuantas entran en una pagina de 4 KB y cuanto
//       de ese costo se nota en la consulta.
//
//  Los vectores sinteticos se generan AGRUPADOS (nubes gaussianas alrededor de
//  centros aleatorios en la esfera), no uniformes. No es cosmetico: los
//  embeddings reales estan agrupados --- los textos parecidos caen juntos --- y
//  es justo esa estructura la que k-means puede encontrar. Con vectores
//  uniformes no hay nada que agrupar y el IVF saldria injustamente mal.
//
//     ./build/exp7_vectorial [dir] [N_max] [consultas]
// ============================================================================
#include <cmath>
#include <set>

#include "comun.hpp"
#include "db/ivf.hpp"

using namespace bench;
using namespace db;

namespace {

// Nubes gaussianas alrededor de 'grupos' centros tomados al azar en la esfera
// unitaria. Devuelve los vectores planos: N filas de 'dim' componentes.
std::vector<float> generarVectores(int N, int dim, int grupos, unsigned semilla) {
    std::mt19937 rng(semilla);
    std::normal_distribution<float> g(0.0f, 1.0f);

    std::vector<float> centros(static_cast<std::size_t>(grupos) * dim);
    for (float& x : centros) x = g(rng);
    for (int c = 0; c < grupos; ++c) {               // centros en la esfera
        double n = 0.0;
        for (int j = 0; j < dim; ++j) {
            const double v = centros[static_cast<std::size_t>(c) * dim + j];
            n += v * v;
        }
        n = std::sqrt(n);
        if (n > 0.0)
            for (int j = 0; j < dim; ++j) centros[static_cast<std::size_t>(c) * dim + j] /= static_cast<float>(n);
    }

    std::vector<float> datos(static_cast<std::size_t>(N) * dim);
    for (int i = 0; i < N; ++i) {
        const int c = static_cast<int>(rng() % static_cast<unsigned>(grupos));
        for (int j = 0; j < dim; ++j)
            datos[static_cast<std::size_t>(i) * dim + j] =
                centros[static_cast<std::size_t>(c) * dim + j] + 0.25f * g(rng);
    }
    return datos;
}

// Un IVF suelto, con su archivo, su DiskManager y su BufferPool. Se usa el
// indice directo y no una Table para que lo que se mide sea el indice y no el
// heap: el objetivo del experimento es el compromiso recall/latencia.
struct Montaje {
    std::unique_ptr<DiskManager> disk;
    std::unique_ptr<BufferPool>  bp;
    std::unique_ptr<IVFIndex>    ivf;
};

Montaje montar(const std::string& dir, const std::string& nombre) {
    Montaje m;
    const std::string ruta = dir + "/" + nombre + ".idx";
    resetFile(ruta);
    m.disk = std::make_unique<DiskManager>(ruta);
    m.bp   = std::make_unique<BufferPool>(m.disk.get(), 64);
    m.ivf  = std::make_unique<IVFIndex>(m.bp.get());
    return m;
}

// Fuente re-ejecutable sobre los vectores en memoria. El IVF la recorre tres
// veces (muestrear, asignar, colocar), asi que tiene que dar siempre lo mismo.
IVFIndex::Fuente fuenteDe(const std::vector<float>& datos, int N, int dim) {
    return [&datos, N, dim](const std::function<void(const RID&, const std::vector<float>&)>& emitir) {
        std::vector<float> v(static_cast<std::size_t>(dim));
        for (int i = 0; i < N; ++i) {
            std::copy(datos.begin() + static_cast<std::size_t>(i) * dim,
                      datos.begin() + static_cast<std::size_t>(i + 1) * dim, v.begin());
            emitir(RID{i / 100, i % 100}, v);
        }
    };
}

std::vector<float> consultaDe(const std::vector<float>& datos, int dim, int i) {
    return std::vector<float>(datos.begin() + static_cast<std::size_t>(i) * dim,
                              datos.begin() + static_cast<std::size_t>(i + 1) * dim);
}

struct Calidad {
    double recall = 0.0;
    double ms     = 0.0;
    double accesos = 0.0;
};

// Recall@k medio y costo medio sobre un juego de consultas. La referencia es
// buscarExacto() sobre el MISMO indice: asi la unica diferencia entre las dos
// medidas es el numero de celdas visitadas, que es lo que se quiere aislar.
Calidad medir(const IVFIndex& ivf, const std::vector<std::vector<float>>& consultas,
              int k, int nprobe) {
    Calidad q;
    for (const std::vector<float>& c : consultas) {
        std::set<std::pair<int, int>> verdad;
        for (const IVFIndex::Resultado& r : ivf.buscarExacto(c, k))
            verdad.insert({r.rid.page_id, r.rid.slot});

        Cronometro cr;
        const std::vector<IVFIndex::Resultado> aprox = ivf.buscar(c, k, nprobe);
        const Medicion m = cr.parar();

        int aciertos = 0;
        for (const IVFIndex::Resultado& r : aprox)
            if (verdad.count({r.rid.page_id, r.rid.slot})) ++aciertos;

        q.recall  += verdad.empty() ? 1.0 : static_cast<double>(aciertos) / static_cast<double>(verdad.size());
        q.ms      += m.ms;
        q.accesos += static_cast<double>(m.accesos);
    }
    const double n = static_cast<double>(std::max<std::size_t>(1, consultas.size()));
    q.recall /= n;
    q.ms     /= n;
    q.accesos /= n;
    return q;
}

std::string repartoDe(const IVFIndex& ivf) {
    const std::vector<std::uint32_t> t = ivf.tamanosDeListas();
    if (t.empty()) return "-";
    std::uint32_t mn = t[0], mx = t[0];
    for (std::uint32_t c : t) { if (c < mn) mn = c; if (c > mx) mx = c; }
    const double media = static_cast<double>(ivf.size()) / static_cast<double>(t.size());
    return num(media, 1) + " / " + std::to_string(mn) + " / " + std::to_string(mx);
}

}  // namespace

int main(int argc, char** argv) {
    const std::string dir = argc > 1 ? argv[1] : "data/_bench";
    const int N_MAX       = argc > 2 ? std::atoi(argv[2]) : 20000;
    const int N_CONSULTAS = argc > 3 ? std::atoi(argv[3]) : 50;
    const int DIM         = 128;
    const int K           = 10;

    titulo("EXPERIMENTO 7 - Modulo vectorial: IVF, recall y latencia");
    std::cout << "\nVectores agrupados en nubes gaussianas, dimension " << DIM
              << ", metrica coseno.\n"
                 "Recall@" << K << " medido contra fuerza bruta sobre el mismo indice, "
              << N_CONSULTAS << " consultas.\n";

    // ======================================================================
    std::cout << "\n### A. Construccion: como crece el indice con N\n\n";
    fila({"N", "nlist", "ms", "escrituras", "paginas", "KB", "iters", "media/min/max por celda"});
    separador(8);
    for (int N : {1000, 2500, 5000, 10000, 20000, 40000}) {
        if (N > N_MAX) break;
        const std::vector<float> datos = generarVectores(N, DIM, 24, 2026);
        Montaje m = montar(dir, "exp7_a");
        Cronometro c;
        m.ivf->construir(fuenteDe(datos, N, DIM), DIM, 0, IVFIndex::Metrica::COSENO);
        const Medicion med = c.parar();
        fila({std::to_string(N), std::to_string(m.ivf->listas()), num(med.ms, 1),
              std::to_string(med.escrituras), std::to_string(m.ivf->numPages()),
              num(m.ivf->numPages() * (DB_PAGE_SIZE / 1024.0), 0),
              std::to_string(m.ivf->iteraciones()), repartoDe(*m.ivf)});
    }
    std::cout << "\nCon nlist = raiz de N (el valor por defecto), el numero de vectores por\n"
                 "celda crece tambien como raiz de N: es lo que mantiene acotado el costo de\n"
                 "una consulta. El tamano del indice crece lineal con N, como tiene que ser:\n"
                 "guarda los vectores dentro (IVF-Flat), no solo punteros.\n";

    // ======================================================================
    std::cout << "\n### B. Numero de celdas: nlist contra costo y calidad\n\n";
    {
        const int N = std::min(N_MAX, 20000);
        const std::vector<float> datos = generarVectores(N, DIM, 24, 2026);
        std::vector<std::vector<float>> consultas;
        for (int i = 0; i < N_CONSULTAS; ++i)
            consultas.push_back(consultaDe(datos, DIM, (i * 7919) % N));

        fila({"nlist", "inercia", "paginas", "media/min/max", "nprobe def.",
              "recall@10", "accesos/consulta", "ms/consulta"});
        separador(8);
        for (int nl : {1, 4, 16, 64, 141, 400, 1000}) {
            if (nl > N) break;
            Montaje m = montar(dir, "exp7_b");
            m.ivf->construir(fuenteDe(datos, N, DIM), DIM, nl, IVFIndex::Metrica::COSENO);
            const int np = m.ivf->nprobePorDefecto();
            const Calidad q = medir(*m.ivf, consultas, K, np);
            fila({std::to_string(nl), num(m.ivf->inercia(), 1),
                  std::to_string(m.ivf->numPages()), repartoDe(*m.ivf),
                  std::to_string(np), num(q.recall, 4), num(q.accesos, 1), num(q.ms, 3)});
        }
        std::cout << "\nnlist = 1 es el caso degenerado: una sola celda con todo dentro, o sea\n"
                     "fuerza bruta disfrazada de indice (recall 1 y el costo mas alto). Al subir\n"
                     "nlist las celdas se encogen y la consulta se abarata, pero el recall con el\n"
                     "nprobe por defecto BAJA, no sube: nprobe por defecto es raiz de nlist, asi\n"
                     "que la FRACCION de celdas visitadas se encoge --- de la mitad con nlist=4 a\n"
                     "la treintava parte con nlist=1000 --- y cada vez quedan fuera mas vecinos.\n"
                     "Es la leccion practica del experimento: nlist y nprobe no se eligen por\n"
                     "separado, y el valor por defecto es solo un punto de partida.\n"
                     "La inercia (suma de distancias al centroide) solo baja: mide lo bien que el\n"
                     "clustering describe los datos, no lo buena que es la busqueda.\n";
    }

    // ======================================================================
    std::cout << "\n### C. La curva: recall@10 frente a nprobe\n\n";
    {
        const int N = std::min(N_MAX, 20000);
        const std::vector<float> datos = generarVectores(N, DIM, 24, 2026);
        std::vector<std::vector<float>> consultas;
        for (int i = 0; i < N_CONSULTAS; ++i)
            consultas.push_back(consultaDe(datos, DIM, (i * 7919) % N));

        Montaje m = montar(dir, "exp7_c");
        m.ivf->construir(fuenteDe(datos, N, DIM), DIM, 0, IVFIndex::Metrica::COSENO);
        const int nlist = m.ivf->listas();

        // Referencia: fuerza bruta sobre el indice. Es el techo de calidad y el
        // suelo de velocidad; sin ella no se puede decir que algo es "rapido".
        Cronometro cr;
        for (const std::vector<float>& c : consultas) m.ivf->buscarExacto(c, K);
        const Medicion exacta = cr.parar();
        const double ms_exacta = exacta.ms / std::max(1, N_CONSULTAS);
        const double ac_exacta = static_cast<double>(exacta.accesos) / std::max(1, N_CONSULTAS);

        std::cout << "N = " << N << ", nlist = " << nlist
                  << ", fuerza bruta: " << num(ms_exacta, 3) << " ms y "
                  << num(ac_exacta, 1) << " accesos a pagina por consulta.\n\n";

        fila({"nprobe", "% celdas", "recall@10", "accesos/consulta", "ms/consulta",
              "aceleracion", "% del I/O exacto"});
        separador(7);
        for (int np : {1, 2, 4, 8, 16, 32, 64, nlist}) {
            if (np > nlist) continue;
            const Calidad q = medir(*m.ivf, consultas, K, np);
            fila({std::to_string(np), num(100.0 * np / nlist, 1) + " %",
                  num(q.recall, 4), num(q.accesos, 1), num(q.ms, 3),
                  num(q.ms > 0 ? ms_exacta / q.ms : 0.0, 1) + "x",
                  num(ac_exacta > 0 ? 100.0 * q.accesos / ac_exacta : 0.0, 1) + " %"});
        }
        std::cout << "\nEsta es la tabla que resume el modulo. El recall sube rapido al principio\n"
                     "y se aplana: las primeras celdas son las que contienen a casi todos los\n"
                     "vecinos verdaderos, y las siguientes aportan cada vez menos por el mismo\n"
                     "precio. Con nprobe = nlist el recall es 1 por definicion, porque se visita\n"
                     "todo: es fuerza bruta, y ahi el indice no aporta nada. De hecho sale algo\n"
                     "PEOR que fuerza bruta --- mas del 100 % de su I/O y una aceleracion por\n"
                     "debajo de 1 --- porque ademas de leer todas las celdas hay que comparar\n"
                     "contra los nlist centroides y ordenarlos: trabajo que la fuerza bruta no\n"
                     "hace. El punto util de la curva es donde el recall ya es alto y el I/O\n"
                     "sigue siendo una fraccion pequena del exacto.\n";
    }

    // ======================================================================
    std::cout << "\n### D. Dimension: por que 384 cambia el diseno\n\n";
    {
        const int N = std::min(N_MAX, 5000);
        fila({"dim", "bytes/entrada", "entradas/pagina", "paginas", "recall@10 (nprobe def.)",
              "ms/consulta"});
        separador(6);
        for (int dim : {8, 32, 128, 384, 768}) {
            const std::vector<float> datos = generarVectores(N, dim, 24, 2026);
            std::vector<std::vector<float>> consultas;
            for (int i = 0; i < N_CONSULTAS; ++i)
                consultas.push_back(consultaDe(datos, dim, (i * 7919) % N));

            Montaje m = montar(dir, "exp7_d");
            m.ivf->construir(fuenteDe(datos, N, dim), dim, 0, IVFIndex::Metrica::COSENO);
            const int bytes = 8 + 4 + dim * 4;
            const Calidad q = medir(*m.ivf, consultas, K, m.ivf->nprobePorDefecto());
            fila({std::to_string(dim), std::to_string(bytes),
                  num(static_cast<double>(DB_PAGE_SIZE) / bytes, 1),
                  std::to_string(m.ivf->numPages()), num(q.recall, 4), num(q.ms, 3)});
        }
        std::cout << "\nEn dimension 384 una entrada pesa 1548 bytes: entran DOS Y MEDIA en una\n"
                     "pagina de 4 KB, y el indice acaba pesando mas que la tabla. Eso es lo que\n"
                     "obliga a escribir las celdas contiguas: si los vectores estuvieran solo en\n"
                     "el heap, recorrer una celda seria un salto aleatorio por candidato en vez\n"
                     "de una lectura secuencial. Y es tambien la razon de fondo de todo el\n"
                     "modulo: en dimension alta el R-Tree de la parte 2 no poda nada, y hay que\n"
                     "cambiar exactitud por costo.\n";
    }

    std::cout << "\n";
    return 0;
}
