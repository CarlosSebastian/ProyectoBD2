#include "db/ivf.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <numeric>
#include <queue>
#include <random>

namespace db {

namespace {
constexpr int M_NUM_VEC = 0;    // int32
constexpr int M_DIM     = 4;    // int32
constexpr int M_NLIST   = 8;    // int32
constexpr int M_METRICA = 12;   // int32
constexpr int M_MAGIC   = 16;   // int32
constexpr int M_P_CENT  = 20;   // int32
constexpr int M_P_DIR   = 24;   // int32
constexpr int M_P_ENT   = 28;   // int32
constexpr int M_P_FIN   = 32;   // int32
constexpr int M_ITERS   = 36;   // int32
constexpr int M_INERCIA = 40;   // double

constexpr int DIR_BYTES = 16;   // int64 offset + int32 count + relleno

// Cuantos vectores se muestrean para entrenar k-means. Entrenar sobre el
// corpus entero no mejora los centroides de forma apreciable y haria que la
// memoria creciera con N; FAISS usa una regla parecida.
constexpr std::size_t MUESTRAS_POR_LISTA = 40;
constexpr std::size_t MUESTRA_MAXIMA     = 100000;

inline double dot(const float* a, const float* b, int d) {
    double s = 0.0;
    for (int i = 0; i < d; ++i) s += static_cast<double>(a[i]) * static_cast<double>(b[i]);
    return s;
}
inline double norma2(const float* a, int d) { return std::sqrt(dot(a, a, d)); }

inline double distL2(const float* a, const float* b, int d) {
    double s = 0.0;
    for (int i = 0; i < d; ++i) {
        const double x = static_cast<double>(a[i]) - static_cast<double>(b[i]);
        s += x * x;
    }
    return std::sqrt(s);
}

// Distancia coseno = 1 - similitud. Vive en [0,2]: 0 es el mismo sentido, 1
// perpendicular, 2 opuesto. Un vector nulo no tiene direccion, asi que se
// define a distancia 1 de todo (ni parecido ni contrario).
inline double distCoseno(const float* a, double na, const float* b, double nb, int d) {
    if (na <= 0.0 || nb <= 0.0) return 1.0;
    double c = dot(a, b, d) / (na * nb);
    if (c > 1.0) c = 1.0;
    if (c < -1.0) c = -1.0;
    return 1.0 - c;
}
}  // namespace

namespace vecdist {
double coseno(const std::vector<float>& a, const std::vector<float>& b) {
    if (a.size() != b.size() || a.empty()) return 1.0;
    const int d = static_cast<int>(a.size());
    return distCoseno(a.data(), norma2(a.data(), d), b.data(), norma2(b.data(), d), d);
}
double euclidiana(const std::vector<float>& a, const std::vector<float>& b) {
    if (a.size() != b.size() || a.empty()) return std::numeric_limits<double>::infinity();
    return distL2(a.data(), b.data(), static_cast<int>(a.size()));
}
}  // namespace vecdist

std::string IVFIndex::nombreMetrica(Metrica m) { return m == Metrica::L2 ? "L2" : "COSENO"; }

IVFIndex::Metrica IVFIndex::metricaDesdeNombre(const std::string& s) {
    std::string u;
    for (char c : s) u += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    if (u == "COSENO" || u == "COSINE" || u == "COS") return Metrica::COSENO;
    if (u == "L2" || u == "EUCLIDEAN" || u == "EUCLIDIANA") return Metrica::L2;
    throw DBException("Metrica no soportada: '" + s + "' (se admiten COSINE y L2)");
}

// ---------------------------------------------------------------------------
IVFIndex::IVFIndex(BufferPool* bp) : bp_(bp) {
    if (bp_->disk()->numPages() == 0) crearVacio();
    else                              leerMeta();
}

void IVFIndex::crearVacio() {
    page_id_t pid;
    char* p = bp_->newPage(&pid);
    if (pid != 0) throw DBException("El indice IVF tiene que empezar en la pagina 0");
    std::memset(p, 0, PAGE_SIZE);
    writeAt<std::int32_t>(p, M_MAGIC, MAGIC);
    writeAt<std::int32_t>(p, M_P_CENT, 1);
    writeAt<std::int32_t>(p, M_P_DIR, 1);
    writeAt<std::int32_t>(p, M_P_ENT, 1);
    writeAt<std::int32_t>(p, M_P_FIN, 1);
    bp_->unpinPage(pid, true);
    p_cent_ = p_dir_ = p_ent_ = p_fin_ = 1;
}

void IVFIndex::leerMeta() {
    char* p = bp_->fetchPage(0);
    if (readAt<std::int32_t>(p, M_MAGIC) != MAGIC) {
        bp_->unpinPage(0, false);
        throw DBException("El archivo no es un indice IVF (firma incorrecta)");
    }
    num_vec_ = readAt<std::int32_t>(p, M_NUM_VEC);
    dim_     = readAt<std::int32_t>(p, M_DIM);
    nlist_   = readAt<std::int32_t>(p, M_NLIST);
    metrica_ = readAt<std::int32_t>(p, M_METRICA) == 1 ? Metrica::L2 : Metrica::COSENO;
    p_cent_  = readAt<std::int32_t>(p, M_P_CENT);
    p_dir_   = readAt<std::int32_t>(p, M_P_DIR);
    p_ent_   = readAt<std::int32_t>(p, M_P_ENT);
    p_fin_   = readAt<std::int32_t>(p, M_P_FIN);
    iters_   = readAt<std::int32_t>(p, M_ITERS);
    inercia_ = readAt<double>(p, M_INERCIA);
    bp_->unpinPage(0, false);
}

void IVFIndex::escribirMeta() {
    char* p = bp_->fetchPage(0);
    writeAt<std::int32_t>(p, M_NUM_VEC, num_vec_);
    writeAt<std::int32_t>(p, M_DIM, dim_);
    writeAt<std::int32_t>(p, M_NLIST, nlist_);
    writeAt<std::int32_t>(p, M_METRICA, metrica_ == Metrica::L2 ? 1 : 0);
    writeAt<std::int32_t>(p, M_MAGIC, MAGIC);
    writeAt<std::int32_t>(p, M_P_CENT, p_cent_);
    writeAt<std::int32_t>(p, M_P_DIR, p_dir_);
    writeAt<std::int32_t>(p, M_P_ENT, p_ent_);
    writeAt<std::int32_t>(p, M_P_FIN, p_fin_);
    writeAt<std::int32_t>(p, M_ITERS, iters_);
    writeAt<double>(p, M_INERCIA, inercia_);
    bp_->unpinPage(0, true);
}

// ---------------------------------------------------------------------------
//  Zonas planas: un arreglo de bytes repartido en paginas consecutivas.
// ---------------------------------------------------------------------------
void IVFIndex::leerRango(page_id_t zona, std::uint64_t off, std::uint32_t n, char* dest) const {
    while (n > 0) {
        const page_id_t pid = zona + static_cast<page_id_t>(off / PAGE_SIZE);
        const int o = static_cast<int>(off % PAGE_SIZE);
        const std::uint32_t m = std::min<std::uint32_t>(n, static_cast<std::uint32_t>(PAGE_SIZE - o));
        char* p = bp_->fetchPage(pid);
        std::memcpy(dest, p + o, m);
        bp_->unpinPage(pid, false);
        dest += m; off += m; n -= m;
    }
}

void IVFIndex::escribirRango(page_id_t zona, std::uint64_t off, std::uint32_t n, const char* src) {
    while (n > 0) {
        const page_id_t pid = zona + static_cast<page_id_t>(off / PAGE_SIZE);
        const int o = static_cast<int>(off % PAGE_SIZE);
        const std::uint32_t m = std::min<std::uint32_t>(n, static_cast<std::uint32_t>(PAGE_SIZE - o));
        char* p = bp_->fetchPage(pid);
        std::memcpy(p + o, src, m);
        bp_->unpinPage(pid, true);
        src += m; off += m; n -= m;
    }
}

std::vector<float> IVFIndex::centroide(int i) const {
    std::vector<float> c(static_cast<std::size_t>(dim_));
    if (i < 0 || i >= nlist_) return c;
    leerRango(p_cent_, static_cast<std::uint64_t>(i) * dim_ * 4,
              static_cast<std::uint32_t>(dim_) * 4, reinterpret_cast<char*>(c.data()));
    return c;
}

std::vector<std::uint32_t> IVFIndex::tamanosDeListas() const {
    std::vector<std::uint32_t> out;
    out.reserve(static_cast<std::size_t>(nlist_));
    for (int i = 0; i < nlist_; ++i) {
        char buf[DIR_BYTES];
        leerRango(p_dir_, static_cast<std::uint64_t>(i) * DIR_BYTES, DIR_BYTES, buf);
        out.push_back(readAt<std::uint32_t>(buf, 8));
    }
    return out;
}

int IVFIndex::nprobePorDefecto() const {
    // Una celda sola casi nunca basta; visitar la raiz de nlist es el punto
    // de partida habitual y suele dejar el recall por encima del 90 %.
    if (nlist_ <= 1) return 1;
    const int r = static_cast<int>(std::lround(std::sqrt(static_cast<double>(nlist_))));
    return std::max(1, std::min(nlist_, r));
}

// ===========================================================================
//  Construccion
//
//  Tres recorridos de la fuente, cada uno con un proposito distinto:
//     A. contar cuantos vectores hay y quedarse con una MUESTRA acotada para
//        entrenar k-means. Entrenar sobre el corpus entero no mejora los
//        centroides lo bastante como para justificar que la memoria crezca
//        con N.
//     B. asignar cada vector a su celda y contar cuantos van en cada una.
//        Las asignaciones (un entero por vector) si caben en memoria y se
//        guardan, para no tener que recalcularlas en la tercera pasada.
//     C. escribir cada vector en el hueco que le toca dentro de su celda.
//
//  La alternativa de una sola pasada seria acumular todos los vectores en RAM
//  y ordenarlos por celda: con 100 000 embeddings de 384 dimensiones eso son
//  150 MB, que es justo lo que un gestor que presume de trabajar en disco no
//  deberia hacer.
// ===========================================================================
void IVFIndex::construir(const Fuente& recorrer, int dimension, int listas,
                         Metrica m, unsigned semilla, int iteraciones) {
    if (dimension <= 0) throw DBException("La dimension del vector tiene que ser positiva");

    bp_->invalidateAll();
    bp_->disk()->truncate();
    dim_     = dimension;
    metrica_ = m;
    num_vec_ = 0;
    nlist_   = 0;
    inercia_ = 0.0;
    iters_   = 0;
    crearVacio();

    const bool coseno = (metrica_ == Metrica::COSENO);
    const std::size_t d = static_cast<std::size_t>(dim_);

    // Para k-means con coseno se trabaja sobre vectores normalizados: asi la
    // distancia euclidiana entre ellos es monotona con la distancia coseno, y
    // la media de un grupo (renormalizada) es su centroide. Es el k-means
    // esferico de toda la vida.
    auto preparar = [&](const std::vector<float>& v, std::vector<float>* out) {
        out->assign(v.begin(), v.end());
        out->resize(d, 0.0f);
        if (coseno) {
            const double n = norma2(out->data(), dim_);
            if (n > 0.0) for (float& x : *out) x = static_cast<float>(x / n);
        }
    };

    // ---------------- pasada A: contar y muestrear ----------------
    // Muestra acotada POR BYTES, no por numero de vectores: lo que no puede
    // crecer sin control es la memoria, y un vector de 1536 dimensiones pesa
    // cuatro veces mas que uno de 384.
    const std::size_t bytes_muestra = 32u * 1024u * 1024u;
    std::size_t tope = bytes_muestra / (d * sizeof(float));
    if (tope < 1000) tope = 1000;
    if (tope > MUESTRA_MAXIMA) tope = MUESTRA_MAXIMA;

    std::mt19937 rng(semilla);
    std::vector<float> muestra;          // plano: nm filas de d componentes
    std::size_t nm = 0, N = 0;
    {
        std::vector<float> tmp;
        recorrer([&](const RID&, const std::vector<float>& v) {
            if (static_cast<int>(v.size()) != dim_)
                throw DBException("Un vector tiene " + std::to_string(v.size()) +
                                  " dimensiones y el indice declara " + std::to_string(dim_));
            preparar(v, &tmp);
            if (nm < tope) {                        // el deposito aun no se llena
                muestra.insert(muestra.end(), tmp.begin(), tmp.end());
                ++nm;
            } else {
                // Muestreo de deposito: cada vector visto tiene la misma
                // probabilidad de acabar en la muestra, sin saber N de antemano.
                const std::size_t j = rng() % (N + 1);
                if (j < tope) std::copy(tmp.begin(), tmp.end(), muestra.begin() + j * d);
            }
            ++N;
        });
    }

    num_vec_ = static_cast<std::int32_t>(N);
    if (N == 0) { escribirMeta(); bp_->flushAll(); return; }

    nlist_ = listas > 0 ? listas
                        : std::max(1, static_cast<int>(std::lround(std::sqrt(static_cast<double>(N)))));
    nlist_ = std::max(1, std::min<int>(nlist_, static_cast<int>(N)));
    // Mas de 40 muestras por celda no cambia los centroides de forma util.
    const std::size_t deseadas = MUESTRAS_POR_LISTA * static_cast<std::size_t>(nlist_);
    if (nm > deseadas && deseadas >= static_cast<std::size_t>(nlist_)) {
        nm = deseadas;
        muestra.resize(nm * d);
    }

    // ---------------- k-means sobre la muestra ----------------
    std::vector<float> cent(static_cast<std::size_t>(nlist_) * d, 0.0f);
    {
        // Siembra k-means++: el primer centroide al azar, y cada siguiente con
        // probabilidad proporcional al cuadrado de su distancia al centroide
        // mas cercano ya elegido. Sembrar al azar puro deja celdas vacias y
        // clusters pegados; esto es barato y evita casi siempre ese problema.
        std::vector<double> d2(nm, std::numeric_limits<double>::infinity());
        std::size_t primero = rng() % nm;
        std::copy(muestra.begin() + primero * d, muestra.begin() + (primero + 1) * d, cent.begin());
        for (int c = 1; c < nlist_; ++c) {
            double total = 0.0;
            for (std::size_t i = 0; i < nm; ++i) {
                const double dd = distL2(&muestra[i * d], &cent[(c - 1) * d], dim_);
                const double sq = dd * dd;
                if (sq < d2[i]) d2[i] = sq;
                total += d2[i];
            }
            std::size_t elegido = 0;
            if (total <= 0.0) {
                elegido = rng() % nm;              // todos iguales: da lo mismo
            } else {
                double r = (static_cast<double>(rng()) / rng.max()) * total;
                for (std::size_t i = 0; i < nm; ++i) {
                    r -= d2[i];
                    if (r <= 0.0) { elegido = i; break; }
                    elegido = i;
                }
            }
            std::copy(muestra.begin() + elegido * d, muestra.begin() + (elegido + 1) * d,
                      cent.begin() + static_cast<std::size_t>(c) * d);
        }

        // Lloyd
        std::vector<int> asig(nm, 0);
        std::vector<double> suma(static_cast<std::size_t>(nlist_) * d);
        std::vector<std::size_t> cuenta(static_cast<std::size_t>(nlist_));
        for (int it = 0; it < iteraciones; ++it) {
            bool cambio = false;
            for (std::size_t i = 0; i < nm; ++i) {
                int mejor = 0;
                double mejor_d = std::numeric_limits<double>::infinity();
                for (int c = 0; c < nlist_; ++c) {
                    const double dd = distL2(&muestra[i * d], &cent[static_cast<std::size_t>(c) * d], dim_);
                    if (dd < mejor_d) { mejor_d = dd; mejor = c; }
                }
                if (asig[i] != mejor) { asig[i] = mejor; cambio = true; }
            }
            ++iters_;
            std::fill(suma.begin(), suma.end(), 0.0);
            std::fill(cuenta.begin(), cuenta.end(), 0u);
            for (std::size_t i = 0; i < nm; ++i) {
                const std::size_t c = static_cast<std::size_t>(asig[i]);
                ++cuenta[c];
                for (std::size_t j = 0; j < d; ++j) suma[c * d + j] += muestra[i * d + j];
            }
            for (int c = 0; c < nlist_; ++c) {
                const std::size_t cc = static_cast<std::size_t>(c);
                if (cuenta[cc] == 0) {
                    // Celda vacia: se la lleva la muestra mas lejana de su
                    // propio centroide. Dejarla vacia desperdicia una celda y
                    // empeora el reparto.
                    std::size_t peor = 0;
                    double peor_d = -1.0;
                    for (std::size_t i = 0; i < nm; ++i) {
                        const double dd = distL2(&muestra[i * d],
                                                 &cent[static_cast<std::size_t>(asig[i]) * d], dim_);
                        if (dd > peor_d) { peor_d = dd; peor = i; }
                    }
                    std::copy(muestra.begin() + peor * d, muestra.begin() + (peor + 1) * d,
                              cent.begin() + cc * d);
                    asig[peor] = c;
                    cambio = true;
                    continue;
                }
                for (std::size_t j = 0; j < d; ++j)
                    cent[cc * d + j] = static_cast<float>(suma[cc * d + j] /
                                                          static_cast<double>(cuenta[cc]));
                if (coseno) {
                    const double n = norma2(&cent[cc * d], dim_);
                    if (n > 0.0) for (std::size_t j = 0; j < d; ++j)
                        cent[cc * d + j] = static_cast<float>(cent[cc * d + j] / n);
                }
            }
            if (!cambio) break;            // converged
        }
    }

    auto masCercano = [&](const std::vector<float>& preparado, double* dist) {
        int mejor = 0;
        double mejor_d = std::numeric_limits<double>::infinity();
        for (int c = 0; c < nlist_; ++c) {
            const double dd = distL2(preparado.data(), &cent[static_cast<std::size_t>(c) * d], dim_);
            if (dd < mejor_d) { mejor_d = dd; mejor = c; }
        }
        if (dist) *dist = mejor_d;
        return mejor;
    };

    // ---------------- pasada B: asignar y contar ----------------
    std::vector<std::int32_t>  asignacion;
    std::vector<std::uint32_t> por_celda(static_cast<std::size_t>(nlist_), 0u);
    asignacion.reserve(N);
    {
        std::vector<float> tmp;
        recorrer([&](const RID&, const std::vector<float>& v) {
            preparar(v, &tmp);
            double dd = 0.0;
            const int c = masCercano(tmp, &dd);
            asignacion.push_back(static_cast<std::int32_t>(c));
            ++por_celda[static_cast<std::size_t>(c)];
            inercia_ += dd;
        });
    }

    // ---------------- reservar las tres zonas ----------------
    auto reservar = [&](std::uint64_t bytes) {
        const page_id_t inicio = static_cast<page_id_t>(bp_->disk()->numPages());
        const std::uint64_t paginas = (bytes + PAGE_SIZE - 1) / PAGE_SIZE;
        for (std::uint64_t i = 0; i < paginas; ++i) {
            page_id_t pid;
            char* p = bp_->newPage(&pid);
            std::memset(p, 0, PAGE_SIZE);
            bp_->unpinPage(pid, true);
        }
        return inicio;
    };

    const int b = bytesPorEntrada();
    p_cent_ = reservar(static_cast<std::uint64_t>(nlist_) * d * 4);
    escribirRango(p_cent_, 0, static_cast<std::uint32_t>(nlist_ * static_cast<int>(d) * 4),
                  reinterpret_cast<const char*>(cent.data()));

    p_dir_ = reservar(static_cast<std::uint64_t>(nlist_) * DIR_BYTES);
    std::vector<std::uint64_t> inicio_celda(static_cast<std::size_t>(nlist_), 0);
    {
        std::uint64_t acum = 0;
        char buf[DIR_BYTES];
        for (int c = 0; c < nlist_; ++c) {
            inicio_celda[static_cast<std::size_t>(c)] = acum;
            std::memset(buf, 0, DIR_BYTES);
            writeAt<std::uint64_t>(buf, 0, acum);
            writeAt<std::uint32_t>(buf, 8, por_celda[static_cast<std::size_t>(c)]);
            escribirRango(p_dir_, static_cast<std::uint64_t>(c) * DIR_BYTES, DIR_BYTES, buf);
            acum += static_cast<std::uint64_t>(por_celda[static_cast<std::size_t>(c)]) *
                    static_cast<std::uint64_t>(b);
        }
    }

    p_ent_ = reservar(static_cast<std::uint64_t>(N) * static_cast<std::uint64_t>(b));

    // ---------------- pasada C: colocar cada vector en su celda ----------------
    {
        std::vector<std::uint64_t> cursor = inicio_celda;
        std::vector<char> buf(static_cast<std::size_t>(b));
        std::size_t i = 0;
        recorrer([&](const RID& rid, const std::vector<float>& v) {
            const std::size_t c = static_cast<std::size_t>(asignacion[i++]);
            writeAt<std::int32_t>(buf.data(), 0, rid.page_id);
            writeAt<std::int32_t>(buf.data(), 4, rid.slot);
            writeAt<float>(buf.data(), 8, static_cast<float>(norma2(v.data(), dim_)));
            std::memcpy(buf.data() + 12, v.data(), d * 4);
            escribirRango(p_ent_, cursor[c], static_cast<std::uint32_t>(b), buf.data());
            cursor[c] += static_cast<std::uint64_t>(b);
        });
    }

    p_fin_ = static_cast<page_id_t>(bp_->disk()->numPages());
    escribirMeta();
    bp_->flushAll();
}

// ===========================================================================
//  Consulta
// ===========================================================================
namespace {
// Monticulo de maximos sobre la distancia: en la cima esta el PEOR de los k
// mejores, que es el que hay que echar cuando aparece uno mas cercano.
struct PeorArriba {
    bool operator()(const IVFIndex::Resultado& a, const IVFIndex::Resultado& b) const {
        return a.dist < b.dist;
    }
};
}  // namespace

// Recorre un tramo de la zona de entradas leyendo por bloques de varias
// entradas. Leerlas de una en una haria que dos entradas de la misma pagina
// contaran como dos accesos, inflando justo la metrica que el informe mide.
void IVFIndex::leerRangoEntradas(std::uint64_t desde_byte, std::uint32_t cuantas,
                                 const std::function<void(const RID&, float, const float*)>& f) const {
    if (cuantas == 0) return;
    const int b = bytesPorEntrada();
    const std::uint32_t por_bloque =
        std::max<std::uint32_t>(1, static_cast<std::uint32_t>(65536 / b));
    std::vector<char> buf(static_cast<std::size_t>(por_bloque) * static_cast<std::size_t>(b));

    std::uint32_t hechas = 0;
    while (hechas < cuantas) {
        const std::uint32_t n = std::min(por_bloque, cuantas - hechas);
        leerRango(p_ent_, desde_byte + static_cast<std::uint64_t>(hechas) * b,
                  n * static_cast<std::uint32_t>(b), buf.data());
        for (std::uint32_t i = 0; i < n; ++i) {
            const char* e = buf.data() + static_cast<std::size_t>(i) * static_cast<std::size_t>(b);
            RID rid(readAt<std::int32_t>(e, 0), readAt<std::int32_t>(e, 4));
            const float norma = readAt<float>(e, 8);
            f(rid, norma, reinterpret_cast<const float*>(e + 12));
        }
        hechas += n;
    }
}

std::vector<IVFIndex::Resultado>
IVFIndex::buscar(const std::vector<float>& consulta, int k, int nprobe) const {
    std::vector<Resultado> salida;
    if (num_vec_ == 0 || nlist_ == 0 || k == 0) return salida;
    if (static_cast<int>(consulta.size()) != dim_)
        throw DBException("La consulta tiene " + std::to_string(consulta.size()) +
                          " dimensiones y el indice " + std::to_string(dim_));

    if (nprobe <= 0) nprobe = nprobePorDefecto();
    nprobe = std::max(1, std::min(nprobe, nlist_));

    const bool coseno = (metrica_ == Metrica::COSENO);
    const double nq = norma2(consulta.data(), dim_);
    std::vector<float> qn = consulta;
    if (coseno && nq > 0.0) for (float& x : qn) x = static_cast<float>(x / nq);

    // --- 1. que celdas visitar: las nprobe de centroide mas cercano ---
    std::vector<std::pair<double, int>> cercania;
    cercania.reserve(static_cast<std::size_t>(nlist_));
    {
        std::vector<float> cent(static_cast<std::size_t>(dim_));
        for (int c = 0; c < nlist_; ++c) {
            leerRango(p_cent_, static_cast<std::uint64_t>(c) * dim_ * 4,
                      static_cast<std::uint32_t>(dim_) * 4, reinterpret_cast<char*>(cent.data()));
            cercania.push_back({distL2(qn.data(), cent.data(), dim_), c});
        }
    }
    const std::size_t np = static_cast<std::size_t>(nprobe);
    std::partial_sort(cercania.begin(), cercania.begin() + static_cast<long>(np), cercania.end());

    // --- 2. recorrer esas celdas y quedarse con los k mejores ---
    std::vector<Resultado> mejores;
    const bool todos = (k < 0);
    if (!todos) mejores.reserve(static_cast<std::size_t>(k) + 1);

    for (std::size_t p = 0; p < np; ++p) {
        const int c = cercania[p].second;
        char dir[DIR_BYTES];
        leerRango(p_dir_, static_cast<std::uint64_t>(c) * DIR_BYTES, DIR_BYTES, dir);
        const std::uint64_t off = readAt<std::uint64_t>(dir, 0);
        const std::uint32_t n   = readAt<std::uint32_t>(dir, 8);

        leerRangoEntradas(off, n, [&](const RID& rid, float norma, const float* v) {
            Resultado r;
            r.rid  = rid;
            r.dist = coseno ? distCoseno(v, static_cast<double>(norma), consulta.data(), nq, dim_)
                            : distL2(v, consulta.data(), dim_);
            if (todos) { salida.push_back(r); return; }
            if (static_cast<int>(mejores.size()) < k) {
                mejores.push_back(r);
                std::push_heap(mejores.begin(), mejores.end(), PeorArriba{});
            } else if (r.dist < mejores.front().dist) {
                std::pop_heap(mejores.begin(), mejores.end(), PeorArriba{});
                mejores.back() = r;
                std::push_heap(mejores.begin(), mejores.end(), PeorArriba{});
            }
        });
    }
    if (!todos) salida.swap(mejores);

    std::sort(salida.begin(), salida.end(), [](const Resultado& a, const Resultado& b) {
        if (a.dist != b.dist) return a.dist < b.dist;
        return a.rid < b.rid;             // desempate estable
    });
    return salida;
}

// Fuerza bruta sobre el indice entero: es la verdad contra la que se mide el
// recall de la busqueda aproximada. Recorre las entradas en el orden fisico
// en que estan escritas, asi que tambien sirve de cota de cuanto costaria no
// tener celdas.
std::vector<IVFIndex::Resultado>
IVFIndex::buscarExacto(const std::vector<float>& consulta, int k) const {
    std::vector<Resultado> salida;
    if (num_vec_ == 0) return salida;
    if (static_cast<int>(consulta.size()) != dim_)
        throw DBException("La consulta tiene " + std::to_string(consulta.size()) +
                          " dimensiones y el indice " + std::to_string(dim_));

    const bool coseno = (metrica_ == Metrica::COSENO);
    const double nq = norma2(consulta.data(), dim_);
    std::vector<Resultado> mejores;
    const bool todos = (k < 0);
    if (!todos) mejores.reserve(static_cast<std::size_t>(k) + 1);

    leerRangoEntradas(0, static_cast<std::uint32_t>(num_vec_),
                      [&](const RID& rid, float norma, const float* v) {
        Resultado r;
        r.rid  = rid;
        r.dist = coseno ? distCoseno(v, static_cast<double>(norma), consulta.data(), nq, dim_)
                        : distL2(v, consulta.data(), dim_);
        if (todos) { salida.push_back(r); return; }
        if (static_cast<int>(mejores.size()) < k) {
            mejores.push_back(r);
            std::push_heap(mejores.begin(), mejores.end(), PeorArriba{});
        } else if (r.dist < mejores.front().dist) {
            std::pop_heap(mejores.begin(), mejores.end(), PeorArriba{});
            mejores.back() = r;
            std::push_heap(mejores.begin(), mejores.end(), PeorArriba{});
        }
    });
    if (!todos) salida.swap(mejores);

    std::sort(salida.begin(), salida.end(), [](const Resultado& a, const Resultado& b) {
        if (a.dist != b.dist) return a.dist < b.dist;
        return a.rid < b.rid;
    });
    return salida;
}

}  // namespace db
