// ============================================================================
//  bplus_tree.hpp - B+ Tree persistente en disco (una pagina = un nodo)
//
//  Estructura del archivo:
//     pagina 0 : META  -> root(4) | height(4) | num_keys(8)
//     pagina k : NODO  -> is_leaf(1) | n(2) | next_leaf(4) | prev_leaf(4)
//                         [header 12 bytes]
//                         claves...            (arreglo de K)
//                         valores RID (hoja) o hijos page_id (interno)
//
//  Todas las claves reales viven en las HOJAS, que estan DOBLEMENTE enlazadas
//  (next_leaf / prev_leaf). Por eso las consultas por rango son O(log n + k)
//  recorriendo hojas contiguas, que es la ventaja frente al hash, y ademas se
//  pueden recorrer en orden descendente sin volver a bajar desde la raiz.
//
//  Se permiten claves duplicadas (indice no unico): search() devuelve todos
//  los RID que coinciden.
//
//  LIMITACION CONOCIDA (documentada en informe/informe.tex, seccion 9): remove() elimina la
//  entrada de la hoja pero NO fusiona ni redistribuye nodos subllenos. El
//  arbol sigue siendo correcto, solo puede quedar menos compacto tras muchos
//  borrados. La fusion queda para el Avance 2.
// ============================================================================
#pragma once

#include <algorithm>
#include <string>
#include <vector>

#include "db/buffer_pool.hpp"
#include "db/common.hpp"

namespace db {

template <typename K>
class BPlusTree {
public:
    static constexpr int NODE_HDR  = 12;
    static constexpr int KS        = static_cast<int>(sizeof(K));
    static constexpr int VS        = static_cast<int>(sizeof(RID));
    static constexpr int LEAF_MAX  = (PAGE_SIZE - NODE_HDR) / (KS + VS);
    static constexpr int INT_MAX_K = (PAGE_SIZE - NODE_HDR - 4) / (KS + 4);

    static_assert(sizeof(K) <= 256, "clave demasiado grande para el nodo");

    explicit BPlusTree(BufferPool* bp) : bp_(bp) {
        if (bp_->disk()->numPages() == 0) {
            page_id_t meta;
            char* m = bp_->newPage(&meta);          // pagina 0 = META
            std::memset(m, 0, PAGE_SIZE);
            writeAt<page_id_t>(m, 0, INVALID_PAGE_ID);
            writeAt<std::int32_t>(m, 4, 0);
            writeAt<std::int64_t>(m, 8, 0);
            bp_->unpinPage(meta, true);
        }
    }

    // ------------------------------------------------------------------ API
    void insert(const K& key, const RID& rid) {
        page_id_t root = getRoot();
        if (root == INVALID_PAGE_ID) {
            page_id_t pid;
            char* b = bp_->newPage(&pid);
            initNode(b, /*leaf=*/true);
            setN(b, 1);
            writeAt<K>(b, leafKeyOff(0), key);
            writeAt<RID>(b, leafValOff(0), rid);
            bp_->unpinPage(pid, true);
            setRoot(pid);
            setHeight(1);
            addCount(1);
            return;
        }

        Split sp = insertRec(root, key, rid);
        if (sp.happened) {                          // la raiz se partio: crece la altura
            page_id_t pid;
            char* b = bp_->newPage(&pid);
            initNode(b, /*leaf=*/false);
            setN(b, 1);
            writeAt<K>(b, intKeyOff(0), sp.key);
            writeAt<page_id_t>(b, intChildOff(0), root);
            writeAt<page_id_t>(b, intChildOff(1), sp.right);
            bp_->unpinPage(pid, true);
            setRoot(pid);
            setHeight(getHeight() + 1);
        }
        addCount(1);
    }

    // Busqueda puntual: devuelve todos los RID con esa clave.
    std::vector<RID> search(const K& key) const {
        std::vector<RID> out;
        page_id_t leaf = findLeaf(key);
        while (leaf != INVALID_PAGE_ID) {
            const char* b = bp_->fetchPage(leaf);
            int n = getN(b);
            int i = lowerBoundLeaf(b, n, key);
            bool done = false;
            for (; i < n; ++i) {
                K k = readAt<K>(b, leafKeyOff(i));
                if (k < key || key < k) { done = true; break; }
                out.push_back(readAt<RID>(b, leafValOff(i)));
            }
            page_id_t next = nextLeaf(b);
            bp_->unpinPage(leaf, false);
            if (done) break;                        // apareció una clave mayor
            leaf = next;                            // duplicados que cruzan de hoja
            if (leaf == INVALID_PAGE_ID) break;
        }
        return out;
    }

    // Busqueda por rango [lo, hi], en orden ascendente.
    std::vector<RID> rangeSearch(const K& lo, const K& hi) const {
        std::vector<RID> out;
        page_id_t leaf = findLeaf(lo);
        while (leaf != INVALID_PAGE_ID) {
            const char* b = bp_->fetchPage(leaf);
            int n = getN(b);
            int i = lowerBoundLeaf(b, n, lo);
            bool stop = false;
            for (; i < n; ++i) {
                K k = readAt<K>(b, leafKeyOff(i));
                if (hi < k) { stop = true; break; }
                out.push_back(readAt<RID>(b, leafValOff(i)));
            }
            page_id_t next = nextLeaf(b);
            bp_->unpinPage(leaf, false);
            if (stop) break;
            leaf = next;
        }
        return out;
    }

    // Busqueda por rango [lo, hi] en orden DESCENDENTE. Baja una sola vez al
    // arbol y desde ahi recorre las hojas hacia atras por prev_leaf, sin
    // volver a la raiz. Es lo que hace util el enlace hacia atras.
    std::vector<RID> rangeSearchDesc(const K& lo, const K& hi) const {
        std::vector<RID> out;
        page_id_t leaf = findLeaf(hi, /*izquierda=*/false);
        while (leaf != INVALID_PAGE_ID) {
            const char* b = bp_->fetchPage(leaf);
            int n = getN(b);
            bool stop = false;
            for (int i = n - 1; i >= 0; --i) {
                K k = readAt<K>(b, leafKeyOff(i));
                if (hi < k) continue;            // todavia por encima del tope
                if (k < lo) { stop = true; break; }
                out.push_back(readAt<RID>(b, leafValOff(i)));
            }
            page_id_t anterior = prevLeaf(b);
            bp_->unpinPage(leaf, false);
            if (stop) break;
            leaf = anterior;
        }
        return out;
    }

    // Borra una entrada. Si rid es invalido borra la primera con esa clave.
    bool remove(const K& key, const RID& rid = RID()) {
        page_id_t leaf = findLeaf(key);
        if (leaf == INVALID_PAGE_ID) return false;

        while (leaf != INVALID_PAGE_ID) {
            char* b = bp_->fetchPage(leaf);
            int n = getN(b);
            int i = lowerBoundLeaf(b, n, key);
            for (; i < n; ++i) {
                K k = readAt<K>(b, leafKeyOff(i));
                if (k < key || key < k) { i = n; break; }
                RID r = readAt<RID>(b, leafValOff(i));
                if (!rid.valid() || r == rid) {
                    for (int j = i; j < n - 1; ++j) {
                        writeAt<K>(b, leafKeyOff(j), readAt<K>(b, leafKeyOff(j + 1)));
                        writeAt<RID>(b, leafValOff(j), readAt<RID>(b, leafValOff(j + 1)));
                    }
                    setN(b, n - 1);
                    bp_->unpinPage(leaf, true);
                    addCount(-1);
                    return true;
                }
            }
            page_id_t next = nextLeaf(b);
            bp_->unpinPage(leaf, false);
            leaf = next;
            if (leaf != INVALID_PAGE_ID) {          // seguir solo si la clave continua
                const char* nb = bp_->fetchPage(leaf);
                // Una hoja vacia es TRANSPARENTE: remove() no fusiona ni libera
                // hojas, asi que en medio de una tirada de duplicados es normal
                // encontrar una vacia. Cortar ahi hacia fallar borrados validos.
                bool cont = getN(nb) == 0 ||
                            (!(readAt<K>(nb, leafKeyOff(0)) > key) &&
                             !(key > readAt<K>(nb, leafKeyOff(0))));
                bp_->unpinPage(leaf, false);
                if (!cont) break;
            }
        }
        return false;
    }

    // ------------------------------------------------------- info / metricas
    page_id_t    getRoot()   const { return metaRead<page_id_t>(0); }
    std::int32_t getHeight() const { return metaRead<std::int32_t>(4); }
    std::int64_t size()      const { return metaRead<std::int64_t>(8); }
    int          numPages()  const { return bp_->disk()->numPages(); }

    static int leafCapacity()     { return LEAF_MAX; }
    static int internalCapacity() { return INT_MAX_K; }

private:
    struct Split {
        bool      happened = false;
        K         key{};
        page_id_t right    = INVALID_PAGE_ID;
    };

    // ---- offsets dentro de un nodo ----
    static int leafKeyOff(int i)  { return NODE_HDR + i * KS; }
    static int leafValOff(int i)  { return NODE_HDR + LEAF_MAX * KS + i * VS; }
    static int intKeyOff(int i)   { return NODE_HDR + i * KS; }
    static int intChildOff(int i) { return NODE_HDR + INT_MAX_K * KS + i * 4; }

    static bool isLeaf(const char* b)     { return readAt<std::uint8_t>(b, 0) != 0; }
    static page_id_t nextLeaf(const char* b) { return readAt<page_id_t>(b, 4); }
    static page_id_t prevLeaf(const char* b) { return readAt<page_id_t>(b, 8); }
    static void setNextLeaf(char* b, page_id_t p) { writeAt<page_id_t>(b, 4, p); }
    static void setPrevLeaf(char* b, page_id_t p) { writeAt<page_id_t>(b, 8, p); }
    static int  getN(const char* b)       { return readAt<std::uint16_t>(b, 1); }
    static void setN(char* b, int n)      { writeAt<std::uint16_t>(b, 1, static_cast<std::uint16_t>(n)); }
    static void initNode(char* b, bool leaf) {
        std::memset(b, 0, PAGE_SIZE);
        writeAt<std::uint8_t>(b, 0, leaf ? 1 : 0);
        writeAt<std::uint16_t>(b, 1, 0);
        writeAt<page_id_t>(b, 4, INVALID_PAGE_ID);   // next_leaf
        writeAt<page_id_t>(b, 8, INVALID_PAGE_ID);   // prev_leaf
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

    // primer indice i tal que keys[i] >= key
    static int lowerBoundLeaf(const char* b, int n, const K& key) {
        int lo = 0, hi = n;
        while (lo < hi) {
            int mid = (lo + hi) / 2;
            if (readAt<K>(b, leafKeyOff(mid)) < key) lo = mid + 1; else hi = mid;
        }
        return lo;
    }
    // primer indice i tal que keys[i] > key  (indice del hijo por el que bajar)
    static int upperBoundInt(const char* b, int n, const K& key) {
        int lo = 0, hi = n;
        while (lo < hi) {
            int mid = (lo + hi) / 2;
            if (key < readAt<K>(b, intKeyOff(mid))) hi = mid; else lo = mid + 1;
        }
        return lo;
    }
    // primer indice i tal que keys[i] >= key. Con claves duplicadas, el split
    // de una hoja copia al padre una clave separadora IGUAL a la duplicada, asi
    // que bajar por upperBound aterriza en la hoja mas a la DERECHA del grupo y
    // los duplicados de las hojas anteriores quedan invisibles. Este descenso
    // por cota inferior es el que hay que usar para buscar.
    static int lowerBoundInt(const char* b, int n, const K& key) {
        int lo = 0, hi = n;
        while (lo < hi) {
            int mid = (lo + hi) / 2;
            if (readAt<K>(b, intKeyOff(mid)) < key) lo = mid + 1; else hi = mid;
        }
        return lo;
    }

    // izquierda = true baja a la PRIMERA hoja que puede contener la clave
    // (imprescindible con duplicados); false baja a la ultima, que es lo que
    // necesita el recorrido descendente.
    page_id_t findLeaf(const K& key, bool izquierda = true) const {
        page_id_t pid = getRoot();
        while (pid != INVALID_PAGE_ID) {
            const char* b = bp_->fetchPage(pid);
            if (isLeaf(b)) { bp_->unpinPage(pid, false); return pid; }
            int n  = getN(b);
            int ci = izquierda ? lowerBoundInt(b, n, key) : upperBoundInt(b, n, key);
            page_id_t next = readAt<page_id_t>(b, intChildOff(ci));
            bp_->unpinPage(pid, false);
            pid = next;
        }
        return INVALID_PAGE_ID;
    }

    Split insertRec(page_id_t pid, const K& key, const RID& rid) {
        char* b = bp_->fetchPage(pid);

        // ------------------------------- HOJA -------------------------------
        if (isLeaf(b)) {
            int n   = getN(b);
            int pos = lowerBoundLeaf(b, n, key);
            while (pos < n && !(key < readAt<K>(b, leafKeyOff(pos)))) ++pos;  // duplicados al final

            if (n < LEAF_MAX) {                       // cabe: solo desplazar
                for (int j = n; j > pos; --j) {
                    writeAt<K>(b, leafKeyOff(j), readAt<K>(b, leafKeyOff(j - 1)));
                    writeAt<RID>(b, leafValOff(j), readAt<RID>(b, leafValOff(j - 1)));
                }
                writeAt<K>(b, leafKeyOff(pos), key);
                writeAt<RID>(b, leafValOff(pos), rid);
                setN(b, n + 1);
                bp_->unpinPage(pid, true);
                return Split{};
            }

            // ---- SPLIT de hoja: n+1 elementos repartidos en dos paginas ----
            std::vector<K>   tk(n + 1);
            std::vector<RID> tv(n + 1);
            for (int i = 0, j = 0; i < n + 1; ++i) {
                if (i == pos) { tk[i] = key; tv[i] = rid; }
                else          { tk[i] = readAt<K>(b, leafKeyOff(j)); tv[i] = readAt<RID>(b, leafValOff(j)); ++j; }
            }
            int mid = (n + 1) / 2;

            page_id_t rpid;
            char* rb = bp_->newPage(&rpid);
            initNode(rb, /*leaf=*/true);
            for (int i = mid; i < n + 1; ++i) {
                writeAt<K>(rb, leafKeyOff(i - mid), tk[i]);
                writeAt<RID>(rb, leafValOff(i - mid), tv[i]);
            }
            setN(rb, n + 1 - mid);

            // Reenlazar la lista doblemente enlazada de hojas:
            //     b  <->  rb  <->  (lo que seguia a b)
            page_id_t seguia = nextLeaf(b);
            setNextLeaf(rb, seguia);
            setPrevLeaf(rb, pid);

            for (int i = 0; i < mid; ++i) {
                writeAt<K>(b, leafKeyOff(i), tk[i]);
                writeAt<RID>(b, leafValOff(i), tv[i]);
            }
            setN(b, mid);
            setNextLeaf(b, rpid);
            if (seguia != INVALID_PAGE_ID) {
                char* sb = bp_->fetchPage(seguia);
                setPrevLeaf(sb, rpid);
                bp_->unpinPage(seguia, true);
            }

            Split sp{true, tk[mid], rpid};
            bp_->unpinPage(rpid, true);
            bp_->unpinPage(pid, true);
            return sp;
        }

        // ------------------------------ INTERNO -----------------------------
        int n  = getN(b);
        int ci = upperBoundInt(b, n, key);
        page_id_t child = readAt<page_id_t>(b, intChildOff(ci));
        bp_->unpinPage(pid, false);                  // soltar antes de bajar

        Split sp = insertRec(child, key, rid);
        if (!sp.happened) return Split{};

        b = bp_->fetchPage(pid);
        n = getN(b);
        if (n < INT_MAX_K) {                         // cabe la clave que subio
            for (int j = n; j > ci; --j)
                writeAt<K>(b, intKeyOff(j), readAt<K>(b, intKeyOff(j - 1)));
            for (int j = n + 1; j > ci + 1; --j)
                writeAt<page_id_t>(b, intChildOff(j), readAt<page_id_t>(b, intChildOff(j - 1)));
            writeAt<K>(b, intKeyOff(ci), sp.key);
            writeAt<page_id_t>(b, intChildOff(ci + 1), sp.right);
            setN(b, n + 1);
            bp_->unpinPage(pid, true);
            return Split{};
        }

        // ---- SPLIT de nodo interno: la clave del medio SUBE (no se copia) ----
        std::vector<K>         tk(n + 1);
        std::vector<page_id_t> tc(n + 2);
        for (int i = 0, j = 0; i < n + 1; ++i) {
            if (i == ci) tk[i] = sp.key;
            else         tk[i] = readAt<K>(b, intKeyOff(j++));
        }
        for (int i = 0, j = 0; i < n + 2; ++i) {
            if (i == ci + 1) tc[i] = sp.right;
            else             tc[i] = readAt<page_id_t>(b, intChildOff(j++));
        }
        int mid = (n + 1) / 2;
        K   up  = tk[mid];

        page_id_t rpid;
        char* rb = bp_->newPage(&rpid);
        initNode(rb, /*leaf=*/false);
        int rn = n - mid;
        for (int i = 0; i < rn; ++i)      writeAt<K>(rb, intKeyOff(i), tk[mid + 1 + i]);
        for (int i = 0; i <= rn; ++i)     writeAt<page_id_t>(rb, intChildOff(i), tc[mid + 1 + i]);
        setN(rb, rn);

        for (int i = 0; i < mid; ++i)     writeAt<K>(b, intKeyOff(i), tk[i]);
        for (int i = 0; i <= mid; ++i)    writeAt<page_id_t>(b, intChildOff(i), tc[i]);
        setN(b, mid);

        bp_->unpinPage(rpid, true);
        bp_->unpinPage(pid, true);
        return Split{true, up, rpid};
    }

    BufferPool* bp_;
};

}  // namespace db
