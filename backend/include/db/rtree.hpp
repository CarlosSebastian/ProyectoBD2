// ============================================================================
//  rtree.hpp - R-Tree 2D persistente en disco (una pagina = un nodo)
//
//  Es el indice espacial del Entregable 2. Comparte toda la infraestructura
//  del B+ (DiskManager, BufferPool, DiskCounter) y se diferencia en una cosa
//  fundamental: en 2D NO EXISTE un orden total de las claves. Un B+ parte una
//  hoja por la mitad porque sabe cual es "la clave del medio"; aqui hay que
//  elegir el subarbol y el reparto del split minimizando AREA y SOLAPAMIENTO.
//
//  Estructura del archivo:
//     pagina 0 : META  -> raiz(4) | altura(4) | num_entradas(8) | MAGIC(4)@16
//     pagina k : NODO  -> es_hoja(1) | n(2) | relleno        [cabecera 8 bytes]
//                         MBR[0..M-1]                        (32 B cada uno)
//                         RID[0..M-1]   si es hoja           ( 8 B cada uno)
//                         hijo[0..M-1]  si es interno        ( 4 B cada uno)
//
//  Con paginas de 4 KB: 102 entradas por hoja y 113 por nodo interno.
//
//  Operaciones:
//     insert       ChooseSubtree por minima ampliacion de area + split
//                  cuadratico de Guttman cuando el nodo se desborda.
//     search       consulta de ventana: desciende por TODAS las ramas cuyo
//                  MBR interseca el rectangulo (puede ser mas de una: es la
//                  diferencia esencial con un arbol unidimensional).
//     knn          best-first con cola de prioridad por MINDIST. Garantiza el
//                  resultado exacto visitando el minimo de nodos.
//     remove       baja, borra la entrada de la hoja y encoge los MBR del
//                  camino de vuelta.
//
//  LIMITACION CONOCIDA: remove() no aplica CondenseTree, es decir no reinserta
//  las entradas de un nodo que queda por debajo del minimo ni fusiona nodos.
//  El arbol sigue siendo CORRECTO (toda entrada viva es alcanzable y los MBR
//  siguen siendo envolventes validas), solo puede quedar menos compacto tras
//  muchos borrados. Es la misma decision que se tomo en el B+ del Entregable 1.
// ============================================================================
#pragma once

#include <algorithm>
#include <cmath>
#include <limits>
#include <queue>
#include <utility>
#include <vector>

#include "db/buffer_pool.hpp"
#include "db/common.hpp"

namespace db {

// ---------------------------------------------------------------------------
//  MBR: Minimum Bounding Rectangle, la caja envolvente minima.
//  Un punto es una caja degenerada con min == max en ambos ejes.
// ---------------------------------------------------------------------------
struct MBR {
    double minx = 0.0, miny = 0.0, maxx = 0.0, maxy = 0.0;

    MBR() = default;
    MBR(double x0, double y0, double x1, double y1)
        : minx(std::min(x0, x1)), miny(std::min(y0, y1)),
          maxx(std::max(x0, x1)), maxy(std::max(y0, y1)) {}

    static MBR point(double x, double y) { return MBR(x, y, x, y); }

    // Caja vacia: se usa como elemento neutro de la union.
    static MBR vacia() {
        MBR m;
        m.minx = m.miny =  std::numeric_limits<double>::infinity();
        m.maxx = m.maxy = -std::numeric_limits<double>::infinity();
        return m;
    }
    bool esVacia() const { return minx > maxx || miny > maxy; }

    double area() const {
        if (esVacia()) return 0.0;
        return (maxx - minx) * (maxy - miny);
    }

    bool interseca(const MBR& o) const {
        return !(o.minx > maxx || o.maxx < minx || o.miny > maxy || o.maxy < miny);
    }
    bool contiene(const MBR& o) const {
        return o.minx >= minx && o.maxx <= maxx && o.miny >= miny && o.maxy <= maxy;
    }

    MBR unionCon(const MBR& o) const {
        if (esVacia()) return o;
        if (o.esVacia()) return *this;
        MBR m;
        m.minx = std::min(minx, o.minx);
        m.miny = std::min(miny, o.miny);
        m.maxx = std::max(maxx, o.maxx);
        m.maxy = std::max(maxy, o.maxy);
        return m;
    }

    // Cuanto crece el area de esta caja si hubiera que absorber a la otra.
    // Es el criterio de ChooseSubtree: se baja por el hijo que menos crezca.
    double ampliacion(const MBR& o) const { return unionCon(o).area() - area(); }

    // MINDIST: distancia AL CUADRADO del punto (px,py) a la caja; 0 si el
    // punto cae dentro. Es la cota inferior de la distancia a cualquier cosa
    // guardada bajo este nodo, y es lo que hace correcto al KNN best-first:
    // si el mejor nodo pendiente esta mas lejos que el k-esimo resultado ya
    // encontrado, no hace falta abrirlo.
    // Se trabaja al cuadrado para no pagar una raiz por nodo visitado; el
    // orden es el mismo porque la raiz es monotona.
    double mindist2(double px, double py) const {
        double dx = 0.0, dy = 0.0;
        if      (px < minx) dx = minx - px;
        else if (px > maxx) dx = px - maxx;
        if      (py < miny) dy = miny - py;
        else if (py > maxy) dy = py - maxy;
        return dx * dx + dy * dy;
    }

    bool operator==(const MBR& o) const {
        return minx == o.minx && miny == o.miny && maxx == o.maxx && maxy == o.maxy;
    }
};

static_assert(sizeof(MBR) == 32, "El MBR debe ocupar exactamente cuatro doubles");

// ---------------------------------------------------------------------------
class RTree {
public:
    static constexpr int NODE_HDR = 8;
    static constexpr int MBR_SZ   = static_cast<int>(sizeof(MBR));     // 32
    static constexpr int RID_SZ   = static_cast<int>(sizeof(RID));     // 8
    static constexpr int PID_SZ   = 4;

    static constexpr int LEAF_MAX = (PAGE_SIZE - NODE_HDR) / (MBR_SZ + RID_SZ);
    static constexpr int INT_MAX_E = (PAGE_SIZE - NODE_HDR) / (MBR_SZ + PID_SZ);
    // Relleno minimo del 40 %, el valor que recomienda Guttman: por debajo de
    // eso los nodos quedan tan vacios que el arbol degenera.
    static constexpr int LEAF_MIN = LEAF_MAX * 2 / 5;
    static constexpr int INT_MIN  = INT_MAX_E * 2 / 5;

    static_assert(LEAF_MAX >= 4 && INT_MAX_E >= 4, "pagina demasiado chica para un R-Tree");

    static constexpr std::int32_t MAGIC = 0x52545245;   // "RTRE"
    static constexpr int OFF_MAGIC = 16;

    explicit RTree(BufferPool* bp) : bp_(bp) {
        if (bp_->disk()->numPages() == 0) {
            page_id_t meta;
            char* m = bp_->newPage(&meta);              // pagina 0 = META
            std::memset(m, 0, PAGE_SIZE);
            writeAt<page_id_t>(m, 0, INVALID_PAGE_ID);
            writeAt<std::int32_t>(m, 4, 0);
            writeAt<std::int64_t>(m, 8, 0);
            writeAt<std::int32_t>(m, OFF_MAGIC, MAGIC);
            bp_->unpinPage(meta, true);
        } else {
            const char* m = bp_->fetchPage(0);
            const std::int32_t magia = readAt<std::int32_t>(m, OFF_MAGIC);
            bp_->unpinPage(0, false);
            if (magia != MAGIC)
                throw DBException(
                    "El archivo de indice no contiene un R-Tree. Suele pasar "
                    "cuando se reutiliza un .idx de otro tipo: borre el archivo "
                    "y vuelva a crear el indice.");
        }
    }

    // ------------------------------------------------------------------ API
    void insert(const MBR& box, const RID& rid) {
        page_id_t raiz = getRoot();
        if (raiz == INVALID_PAGE_ID) {                  // primer elemento
            page_id_t pid;
            char* b = bp_->newPage(&pid);
            initNode(b, /*hoja=*/true);
            setN(b, 1);
            writeAt<MBR>(b, mbrOff(0), box);
            writeAt<RID>(b, ridOff(0, true), rid);
            bp_->unpinPage(pid, true);
            setRoot(pid);
            setHeight(1);
            addCount(1);
            return;
        }

        MBR   mbr_raiz;
        Split sp = insertRec(raiz, box, rid, &mbr_raiz);
        if (sp.ocurrio) {                               // la raiz se partio
            page_id_t pid;
            char* b = bp_->newPage(&pid);
            initNode(b, /*hoja=*/false);
            setN(b, 2);
            writeAt<MBR>(b, mbrOff(0), sp.mbr_izq);
            writeAt<page_id_t>(b, hijoOff(0, false), raiz);
            writeAt<MBR>(b, mbrOff(1), sp.mbr_der);
            writeAt<page_id_t>(b, hijoOff(1, false), sp.derecha);
            bp_->unpinPage(pid, true);
            setRoot(pid);
            setHeight(getHeight() + 1);
        }
        addCount(1);
    }

    // Consulta de ventana: todos los RID cuya caja interseca el rectangulo.
    std::vector<RID> search(const MBR& ventana) const {
        std::vector<RID> out;
        page_id_t raiz = getRoot();
        if (raiz != INVALID_PAGE_ID) searchRec(raiz, ventana, &out);
        return out;
    }

    // k vecinos mas cercanos al punto, en orden de distancia creciente.
    // Devuelve (distancia euclidiana, RID).
    std::vector<std::pair<double, RID>> knn(double px, double py, int k) const {
        std::vector<std::pair<double, RID>> out;
        if (k <= 0) return out;
        page_id_t raiz = getRoot();
        if (raiz == INVALID_PAGE_ID) return out;

        // Cola de prioridad por MINDIST: mezcla nodos sin abrir y entradas de
        // hoja ya encontradas. Cuando lo primero de la cola es una entrada de
        // hoja, ningun nodo pendiente puede contener nada mas cercano, asi que
        // esa entrada es el siguiente vecino con certeza.
        std::priority_queue<Cand, std::vector<Cand>, MasCerca> cola;
        cola.push(Cand{0.0, false, raiz, RID()});

        while (!cola.empty() && static_cast<int>(out.size()) < k) {
            Cand c = cola.top();
            cola.pop();

            if (c.es_entrada) {
                out.emplace_back(std::sqrt(c.dist2), c.rid);
                continue;
            }
            const char* b = bp_->fetchPage(c.pid);
            const bool hoja = esHoja(b);
            const int  n    = getN(b);
            for (int i = 0; i < n; ++i) {
                MBR m = readAt<MBR>(b, mbrOff(i));
                double d2 = m.mindist2(px, py);
                if (hoja) cola.push(Cand{d2, true, INVALID_PAGE_ID, readAt<RID>(b, ridOff(i, true))});
                else      cola.push(Cand{d2, false, readAt<page_id_t>(b, hijoOff(i, false)), RID()});
            }
            bp_->unpinPage(c.pid, false);
        }
        return out;
    }

    // Borra la entrada (caja, rid). Devuelve false si no estaba.
    bool remove(const MBR& box, const RID& rid) {
        page_id_t raiz = getRoot();
        if (raiz == INVALID_PAGE_ID) return false;
        MBR nuevo;
        bool ok = removeRec(raiz, box, rid, &nuevo);
        if (ok) addCount(-1);
        return ok;
    }

    // ------------------------------------------------------- info / metricas
    page_id_t    getRoot()   const { return metaRead<page_id_t>(0); }
    std::int32_t getHeight() const { return metaRead<std::int32_t>(4); }
    std::int64_t size()      const { return metaRead<std::int64_t>(8); }
    int          numPages()  const { return bp_->disk()->numPages(); }

    static int leafCapacity()     { return LEAF_MAX; }
    static int internalCapacity() { return INT_MAX_E; }

private:
    struct Split {
        bool      ocurrio = false;
        MBR       mbr_izq, mbr_der;
        page_id_t derecha = INVALID_PAGE_ID;
    };

    struct Cand {
        double    dist2 = 0.0;
        bool      es_entrada = false;   // true: hoja ya resuelta; false: nodo por abrir
        page_id_t pid = INVALID_PAGE_ID;
        RID       rid;
    };
    struct MasCerca {
        bool operator()(const Cand& a, const Cand& b) const {
            if (a.dist2 != b.dist2) return a.dist2 > b.dist2;   // min-heap
            return a.es_entrada < b.es_entrada;                 // desempate estable
        }
    };

    // ---- offsets dentro de un nodo ----
    static int mbrOff(int i)              { return NODE_HDR + i * MBR_SZ; }
    static int ridOff(int i, bool)        { return NODE_HDR + LEAF_MAX  * MBR_SZ + i * RID_SZ; }
    static int hijoOff(int i, bool)       { return NODE_HDR + INT_MAX_E * MBR_SZ + i * PID_SZ; }
    static int capacidad(bool hoja)       { return hoja ? LEAF_MAX : INT_MAX_E; }
    static int minimo(bool hoja)          { return hoja ? LEAF_MIN : INT_MIN; }

    static bool esHoja(const char* b)     { return readAt<std::uint8_t>(b, 0) != 0; }
    static int  getN(const char* b)       { return readAt<std::uint16_t>(b, 1); }
    static void setN(char* b, int n)      { writeAt<std::uint16_t>(b, 1, static_cast<std::uint16_t>(n)); }
    static void initNode(char* b, bool hoja) {
        std::memset(b, 0, PAGE_SIZE);
        writeAt<std::uint8_t>(b, 0, hoja ? 1 : 0);
        writeAt<std::uint16_t>(b, 1, 0);
    }

    // MBR que envuelve a todas las entradas vivas de un nodo.
    static MBR mbrDeNodo(const char* b) {
        MBR m = MBR::vacia();
        int n = getN(b);
        for (int i = 0; i < n; ++i) m = m.unionCon(readAt<MBR>(b, mbrOff(i)));
        return m;
    }

    template <typename T> T metaRead(int off) const {
        const char* m = bp_->fetchPage(0);
        T v = readAt<T>(m, off);
        bp_->unpinPage(0, false);
        return v;
    }
    template <typename T> void metaWrite(int off, const T& v) const {
        char* m = bp_->fetchPage(0);
        writeAt<T>(m, off, v);
        bp_->unpinPage(0, true);
    }
    void setRoot(page_id_t p)      { metaWrite<page_id_t>(0, p); }
    void setHeight(std::int32_t h) { metaWrite<std::int32_t>(4, h); }
    void addCount(std::int64_t d) const { metaWrite<std::int64_t>(8, metaRead<std::int64_t>(8) + d); }

    // ----------------------------------------------------------- busquedas
    void searchRec(page_id_t pid, const MBR& ventana, std::vector<RID>* out) const {
        const char* b = bp_->fetchPage(pid);
        const bool hoja = esHoja(b);
        const int  n    = getN(b);

        if (hoja) {
            for (int i = 0; i < n; ++i)
                if (readAt<MBR>(b, mbrOff(i)).interseca(ventana))
                    out->push_back(readAt<RID>(b, ridOff(i, true)));
            bp_->unpinPage(pid, false);
            return;
        }
        // Un nodo interno puede tener VARIOS hijos que intersecan la ventana:
        // las cajas hermanas pueden solaparse. Por eso hay que recoger los
        // candidatos antes de soltar la pagina y bajar por todos.
        std::vector<page_id_t> bajar;
        for (int i = 0; i < n; ++i)
            if (readAt<MBR>(b, mbrOff(i)).interseca(ventana))
                bajar.push_back(readAt<page_id_t>(b, hijoOff(i, false)));
        bp_->unpinPage(pid, false);
        for (page_id_t h : bajar) searchRec(h, ventana, out);
    }

    // ----------------------------------------------------------- insercion
    // Devuelve el split si el nodo se partio. *mbr_actualizado queda con la
    // envolvente del nodo pid despues de la insercion (o la del grupo izquierdo
    // si hubo split).
    Split insertRec(page_id_t pid, const MBR& box, const RID& rid, MBR* mbr_actualizado) {
        char* b = bp_->fetchPage(pid);
        const bool hoja = esHoja(b);

        if (hoja) {
            int n = getN(b);
            if (n < LEAF_MAX) {
                writeAt<MBR>(b, mbrOff(n), box);
                writeAt<RID>(b, ridOff(n, true), rid);
                setN(b, n + 1);
                *mbr_actualizado = mbrDeNodo(b);
                bp_->unpinPage(pid, true);
                return Split{};
            }
            // desbordamiento: n+1 entradas repartidas en dos paginas
            std::vector<MBR> cajas(n + 1);
            std::vector<RID> rids(n + 1);
            for (int i = 0; i < n; ++i) {
                cajas[i] = readAt<MBR>(b, mbrOff(i));
                rids[i]  = readAt<RID>(b, ridOff(i, true));
            }
            cajas[n] = box;
            rids[n]  = rid;
            // Soltar la pagina ANTES de pedir una nueva: newPage() puede
            // necesitar expulsar un marco, y si este sigue pineado el pool se
            // queda sin victimas. Ya tenemos las entradas copiadas en RAM.
            bp_->unpinPage(pid, true);

            std::vector<int> ga, gb;
            splitCuadratico(cajas, /*hoja=*/true, &ga, &gb);

            page_id_t dpid;
            char* db = bp_->newPage(&dpid);
            initNode(db, /*hoja=*/true);
            for (std::size_t i = 0; i < gb.size(); ++i) {
                writeAt<MBR>(db, mbrOff(static_cast<int>(i)), cajas[gb[i]]);
                writeAt<RID>(db, ridOff(static_cast<int>(i), true), rids[gb[i]]);
            }
            setN(db, static_cast<int>(gb.size()));
            MBR mder = mbrDeNodo(db);
            bp_->unpinPage(dpid, true);

            // Reescribir pid con el grupo izquierdo.
            b = bp_->fetchPage(pid);
            initNode(b, /*hoja=*/true);
            for (std::size_t i = 0; i < ga.size(); ++i) {
                writeAt<MBR>(b, mbrOff(static_cast<int>(i)), cajas[ga[i]]);
                writeAt<RID>(b, ridOff(static_cast<int>(i), true), rids[ga[i]]);
            }
            setN(b, static_cast<int>(ga.size()));
            MBR mizq = mbrDeNodo(b);
            bp_->unpinPage(pid, true);

            *mbr_actualizado = mizq;
            return Split{true, mizq, mder, dpid};
        }

        // ------------------------------ INTERNO -----------------------------
        int n = getN(b);
        int ci = elegirSubarbol(b, n, box);
        page_id_t hijo = readAt<page_id_t>(b, hijoOff(ci, false));
        bp_->unpinPage(pid, false);                  // soltar antes de bajar

        MBR   mbr_hijo;
        Split sp = insertRec(hijo, box, rid, &mbr_hijo);

        b = bp_->fetchPage(pid);
        n = getN(b);
        writeAt<MBR>(b, mbrOff(ci), mbr_hijo);       // el hijo crecio: actualizar
        if (!sp.ocurrio) {
            setN(b, n);
            *mbr_actualizado = mbrDeNodo(b);
            bp_->unpinPage(pid, true);
            return Split{};
        }

        if (n < INT_MAX_E) {                          // cabe el hermano nuevo
            writeAt<MBR>(b, mbrOff(n), sp.mbr_der);
            writeAt<page_id_t>(b, hijoOff(n, false), sp.derecha);
            setN(b, n + 1);
            *mbr_actualizado = mbrDeNodo(b);
            bp_->unpinPage(pid, true);
            return Split{};
        }

        // el nodo interno tambien se desborda
        std::vector<MBR>       cajas(n + 1);
        std::vector<page_id_t> hijos(n + 1);
        for (int i = 0; i < n; ++i) {
            cajas[i] = readAt<MBR>(b, mbrOff(i));
            hijos[i] = readAt<page_id_t>(b, hijoOff(i, false));
        }
        cajas[n] = sp.mbr_der;
        hijos[n] = sp.derecha;
        bp_->unpinPage(pid, true);

        std::vector<int> ga, gb;
        splitCuadratico(cajas, /*hoja=*/false, &ga, &gb);

        page_id_t dpid;
        char* db = bp_->newPage(&dpid);
        initNode(db, /*hoja=*/false);
        for (std::size_t i = 0; i < gb.size(); ++i) {
            writeAt<MBR>(db, mbrOff(static_cast<int>(i)), cajas[gb[i]]);
            writeAt<page_id_t>(db, hijoOff(static_cast<int>(i), false), hijos[gb[i]]);
        }
        setN(db, static_cast<int>(gb.size()));
        MBR mder = mbrDeNodo(db);
        bp_->unpinPage(dpid, true);

        b = bp_->fetchPage(pid);
        initNode(b, /*hoja=*/false);
        for (std::size_t i = 0; i < ga.size(); ++i) {
            writeAt<MBR>(b, mbrOff(static_cast<int>(i)), cajas[ga[i]]);
            writeAt<page_id_t>(b, hijoOff(static_cast<int>(i), false), hijos[ga[i]]);
        }
        setN(b, static_cast<int>(ga.size()));
        MBR mizq = mbrDeNodo(b);
        bp_->unpinPage(pid, true);

        *mbr_actualizado = mizq;
        return Split{true, mizq, mder, dpid};
    }

    // ChooseSubtree: el hijo cuya caja menos tenga que crecer para absorber la
    // entrada nueva. Empate -> el de menor area (deja mas margen a futuro).
    static int elegirSubarbol(const char* b, int n, const MBR& box) {
        int    mejor = 0;
        double mejor_amp = std::numeric_limits<double>::infinity();
        double mejor_area = std::numeric_limits<double>::infinity();
        for (int i = 0; i < n; ++i) {
            MBR m = readAt<MBR>(b, mbrOff(i));
            double amp = m.ampliacion(box);
            double ar  = m.area();
            if (amp < mejor_amp || (amp == mejor_amp && ar < mejor_area)) {
                mejor = i; mejor_amp = amp; mejor_area = ar;
            }
        }
        return mejor;
    }

    // ------------------------------------------------- split cuadratico
    // Algoritmo de Guttman (1984):
    //   PickSeeds  O(M^2): la pareja que mas "espacio muerto" dejaria si
    //              fueran juntas; esas dos semillas arrancan los dos grupos.
    //   PickNext   repetidamente la entrada cuya preferencia por un grupo sea
    //              mas marcada, y se le asigna a ese grupo.
    // Es cuadratico en el numero de entradas del nodo, pero eso solo pasa en
    // un desbordamiento y a cambio da cajas mucho menos solapadas que un
    // reparto lineal, que es lo que decide el costo de TODAS las consultas.
    static void splitCuadratico(const std::vector<MBR>& e, bool hoja,
                                std::vector<int>* ga, std::vector<int>* gb) {
        const int total = static_cast<int>(e.size());
        const int m     = minimo(hoja);
        const int cap   = capacidad(hoja);

        // ---- PickSeeds ----
        int s1 = 0, s2 = 1;
        double peor = -std::numeric_limits<double>::infinity();
        for (int i = 0; i < total; ++i) {
            for (int j = i + 1; j < total; ++j) {
                double d = e[i].unionCon(e[j]).area() - e[i].area() - e[j].area();
                if (d > peor) { peor = d; s1 = i; s2 = j; }
            }
        }

        std::vector<char> asignado(static_cast<std::size_t>(total), 0);
        ga->clear(); gb->clear();
        ga->push_back(s1); gb->push_back(s2);
        asignado[static_cast<std::size_t>(s1)] = 1;
        asignado[static_cast<std::size_t>(s2)] = 1;
        MBR ma = e[s1], mb = e[s2];
        int restantes = total - 2;

        while (restantes > 0) {
            // Si a un grupo le falta justo lo que queda para llegar al minimo,
            // se lleva todo el resto: el relleno minimo manda sobre el criterio
            // de area. Lo mismo si el otro ya esta lleno.
            if (static_cast<int>(ga->size()) + restantes == m ||
                static_cast<int>(gb->size()) >= cap) {
                for (int i = 0; i < total; ++i)
                    if (!asignado[static_cast<std::size_t>(i)]) {
                        ga->push_back(i); asignado[static_cast<std::size_t>(i)] = 1;
                        ma = ma.unionCon(e[i]);
                    }
                break;
            }
            if (static_cast<int>(gb->size()) + restantes == m ||
                static_cast<int>(ga->size()) >= cap) {
                for (int i = 0; i < total; ++i)
                    if (!asignado[static_cast<std::size_t>(i)]) {
                        gb->push_back(i); asignado[static_cast<std::size_t>(i)] = 1;
                        mb = mb.unionCon(e[i]);
                    }
                break;
            }

            // ---- PickNext ----
            int    elegido = -1;
            double mayor_dif = -1.0;
            double da_sel = 0.0, db_sel = 0.0;
            for (int i = 0; i < total; ++i) {
                if (asignado[static_cast<std::size_t>(i)]) continue;
                double da = ma.ampliacion(e[i]);
                double db = mb.ampliacion(e[i]);
                double dif = std::fabs(da - db);
                if (dif > mayor_dif) { mayor_dif = dif; elegido = i; da_sel = da; db_sel = db; }
            }
            if (elegido < 0) break;

            bool aA;
            if      (da_sel != db_sel)             aA = da_sel < db_sel;
            else if (ma.area() != mb.area())       aA = ma.area() < mb.area();
            else                                   aA = ga->size() <= gb->size();

            if (aA) { ga->push_back(elegido); ma = ma.unionCon(e[elegido]); }
            else    { gb->push_back(elegido); mb = mb.unionCon(e[elegido]); }
            asignado[static_cast<std::size_t>(elegido)] = 1;
            --restantes;
        }
    }

    // ------------------------------------------------------------ borrado
    bool removeRec(page_id_t pid, const MBR& box, const RID& rid, MBR* mbr_actualizado) {
        char* b = bp_->fetchPage(pid);
        const bool hoja = esHoja(b);
        int n = getN(b);

        if (hoja) {
            for (int i = 0; i < n; ++i) {
                if (readAt<RID>(b, ridOff(i, true)) == rid &&
                    readAt<MBR>(b, mbrOff(i)) == box) {
                    for (int j = i; j < n - 1; ++j) {
                        writeAt<MBR>(b, mbrOff(j), readAt<MBR>(b, mbrOff(j + 1)));
                        writeAt<RID>(b, ridOff(j, true), readAt<RID>(b, ridOff(j + 1, true)));
                    }
                    setN(b, n - 1);
                    *mbr_actualizado = mbrDeNodo(b);
                    bp_->unpinPage(pid, true);
                    return true;
                }
            }
            *mbr_actualizado = mbrDeNodo(b);
            bp_->unpinPage(pid, false);
            return false;
        }

        // Interno: hay que probar TODOS los hijos cuya caja pueda contener la
        // entrada, no solo el primero, porque las cajas hermanas se solapan.
        std::vector<std::pair<int, page_id_t>> candidatos;
        for (int i = 0; i < n; ++i)
            if (readAt<MBR>(b, mbrOff(i)).interseca(box))
                candidatos.emplace_back(i, readAt<page_id_t>(b, hijoOff(i, false)));
        bp_->unpinPage(pid, false);

        for (const auto& c : candidatos) {
            MBR nuevo;
            if (removeRec(c.second, box, rid, &nuevo)) {
                b = bp_->fetchPage(pid);
                writeAt<MBR>(b, mbrOff(c.first), nuevo);   // encoger de vuelta
                *mbr_actualizado = mbrDeNodo(b);
                bp_->unpinPage(pid, true);
                return true;
            }
        }
        b = bp_->fetchPage(pid);
        *mbr_actualizado = mbrDeNodo(b);
        bp_->unpinPage(pid, false);
        return false;
    }

    BufferPool* bp_;
};

}  // namespace db
