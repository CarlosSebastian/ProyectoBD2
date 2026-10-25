#!/usr/bin/env python3
"""Carga el corpus de papers en el motor: embeddings + COPY + indices.

    python ai/ingesta.py                 # usa ai/corpus/arxiv_papers.jsonl
    python ai/ingesta.py --corpus otro.jsonl

Requisitos: el motor corriendo (./build.sh run) y Ollama con el modelo de
embeddings (ollama pull all-minilm).

Que hace:
  1. calcula el embedding de cada paper (titulo + abstract) con Ollama. Los
     guarda en ai/cache/ para no recalcularlos si vuelves a correr la ingesta
  2. escribe ai/cache/papers.csv y lo carga con COPY (carga masiva del motor)
  3. crea los tres indices que usa el RAG:
       B+ sobre id, INVERTED sobre abstract (BM25), IVF sobre emb (coseno)

Es repetible: si la tabla ya existe vacia sus filas y recarga. Los indices
INVERTED e IVF no admiten altas incrementales; el motor los reconstruye solo,
asi que se carga TODO primero y se consulta despues.
"""
import argparse
import csv
import json
import math
import struct
import sys
import time
from pathlib import Path

import rag_common as rc

AQUI = Path(__file__).resolve().parent
CORPUS = AQUI / "corpus" / "arxiv_papers.jsonl"
CACHE_DIR = AQUI / "cache"
MAX_ABSTRACT = 1700       # una fila (texto + vector de 384 float) debe caber en 4 KB
LOTE = 16


def recortar(txt: str, n: int) -> str:
    if len(txt) <= n:
        return txt
    corte = txt.rfind(" ", 0, n)
    return txt[: corte if corte > n // 2 else n]


# ---- cache de embeddings: registros binarios [int32 id][dim * float32] ------
def cache_path() -> Path:
    return CACHE_DIR / f"emb_{rc.MODELO_EMB.replace(':', '_')}.bin"


def cargar_cache() -> dict:
    p, emb = cache_path(), {}
    if not p.exists():
        return emb
    datos = p.read_bytes()
    if len(datos) < 8:
        return emb
    dim = struct.unpack_from("<i", datos, 0)[0]          # cabecera: dimension
    paso = 4 + 4 * dim
    for off in range(4, len(datos) - paso + 1, paso):
        pid = struct.unpack_from("<i", datos, off)[0]
        emb[pid] = list(struct.unpack_from(f"<{dim}f", datos, off + 4))
    return emb


def guardar_cache(nuevos: dict) -> None:
    p = cache_path()
    CACHE_DIR.mkdir(parents=True, exist_ok=True)
    with p.open("ab") as f:
        if f.tell() == 0:
            f.write(struct.pack("<i", len(next(iter(nuevos.values())))))
        for pid, v in nuevos.items():
            f.write(struct.pack("<i", pid) + struct.pack(f"<{len(v)}f", *v))


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--corpus", type=Path, default=CORPUS)
    ap.add_argument("--lists", type=int, default=0, help="celdas del IVF (0 = sqrt(n))")
    a = ap.parse_args()

    if not a.corpus.exists():
        sys.exit(f"No existe {a.corpus}. Corre antes: python ai/descargar_arxiv.py")
    papers = [json.loads(l) for l in a.corpus.open(encoding="utf-8") if l.strip()]
    if not papers:
        sys.exit("El corpus esta vacio")
    print(f"{len(papers)} papers en {a.corpus.name}")

    # 1. embeddings ---------------------------------------------------------
    cache = cargar_cache()
    faltan = [p for p in papers if p["id"] not in cache]
    print(f"embeddings: {len(papers) - len(faltan)} en cache, {len(faltan)} por calcular "
          f"({rc.MODELO_EMB})")
    t0 = time.time()
    for i in range(0, len(faltan), LOTE):
        lote = faltan[i:i + LOTE]
        vecs = rc.embed([p["titulo"] + ". " + p["abstract"] for p in lote])
        nuevos = {p["id"]: v for p, v in zip(lote, vecs)}
        guardar_cache(nuevos)           # se guarda por lote: si se corta, no se pierde
        cache.update(nuevos)
        print(f"  {min(i + LOTE, len(faltan))}/{len(faltan)}  ({time.time() - t0:.0f}s)")
    dim = len(next(iter(cache.values())))

    # 2. CSV + COPY ---------------------------------------------------------
    CACHE_DIR.mkdir(parents=True, exist_ok=True)
    ruta_csv = CACHE_DIR / "papers.csv"
    with ruta_csv.open("w", encoding="utf-8", newline="") as f:
        w = csv.writer(f)
        w.writerow(["id", "arxiv_id", "titulo", "abstract", "anio", "cat", "emb"])
        for p in papers:
            w.writerow([p["id"], p["arxiv_id"], p["titulo"], recortar(p["abstract"], MAX_ABSTRACT),
                        p["anio"], p["cat"],
                        "[" + ",".join(f"{x:.6g}" for x in cache[p["id"]]) + "]"])

    existentes = {t["name"]: t for t in rc.tablas()}
    if "papers" not in existentes:
        rc.sql("CREATE TABLE papers (id INT PRIMARY KEY, arxiv_id CHAR(24), titulo CHAR(200), "
               f"abstract TEXT, anio INT, cat CHAR(12), emb VECTOR({dim})) USING HEAP;")
        print("tabla papers creada")
    else:
        rc.sql("DELETE FROM papers WHERE id >= 0;")
        print("tabla papers vaciada")

    r = rc.sql(f"COPY papers FROM '{ruta_csv.as_posix()}' HEADER;")
    print(f"COPY: {r.get('message', '')}  [{r['disk_writes']} escrituras, {r['exec_ms']:.0f} ms]")

    # 3. indices ------------------------------------------------------------
    ya = {i["column"] for t in rc.tablas() if t["name"] == "papers" for i in t["indexes"]}
    n = len(papers)
    listas = a.lists or max(1, min(n, round(math.sqrt(n))))
    for col, ddl in (("id", "CREATE INDEX ix_papers_id ON papers (id) USING BTREE;"),
                     ("abstract", "CREATE INDEX ix_papers_txt ON papers (abstract) USING INVERTED;"),
                     ("emb", f"CREATE INDEX ix_papers_emb ON papers (emb) USING IVF "
                             f"LISTS {listas} METRIC COSINE;")):
        if col in ya:
            print(f"indice sobre {col}: ya existe")
            continue
        t = time.time()
        r = rc.sql(ddl)
        print(f"indice sobre {col}: {r.get('message', '')} ({time.time() - t:.1f}s)")

    # Calentamiento: tras el COPY los indices invertido e IVF estan marcados
    # como invalidos y se reconstruyen en la primera consulta. Se paga aqui,
    # no en la primera pregunta del usuario.
    rc.sql("SELECT id FROM papers WHERE abstract MATCH 'data' LIMIT 1;")
    rc.sql(f"SELECT id FROM papers ORDER BY emb <=> [{','.join(['0.1'] * dim)}] LIMIT 1;")
    print("listo: ya puedes correr  python ai/rag_server.py")


if __name__ == "__main__":
    try:
        main()
    except (rc.ServicioCaido, RuntimeError) as e:
        sys.exit(f"ERROR: {e}")
