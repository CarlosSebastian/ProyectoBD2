#!/usr/bin/env python3
"""Descarga metadatos de papers de arXiv (titulo + abstract) a un JSONL.

Usa la API publica de arXiv, que no pide clave. Solo biblioteca estandar.

    python ai/descargar_arxiv.py                 # ~25 000 papers, 5 categorias
    python ai/descargar_arxiv.py --por-cat 200   # prueba rapida (1 000 papers)
    python ai/descargar_arxiv.py --cats cs.DB cs.IR --por-cat 3000

Salida: ai/corpus/arxiv_papers.jsonl, una linea por paper:
    {"id": 1, "arxiv_id": "2101.00001", "titulo": "...", "abstract": "...",
     "anio": 2021, "cat": "cs.DB"}

Es reanudable: si el archivo ya existe, salta los papers que ya tiene.
arXiv pide como maximo 1 peticion cada 3 segundos; el script lo respeta.
"""
import argparse
import json
import re
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
import xml.etree.ElementTree as ET
from pathlib import Path

API = "https://export.arxiv.org/api/query"
NS = {"a": "http://www.w3.org/2005/Atom"}
PAUSA = 3.2          # segundos entre peticiones (regla de cortesia de arXiv)
POR_PAGINA = 500
CATS_DEFECTO = ["cs.DB", "cs.IR", "cs.LG", "cs.CL", "cs.AI"]
SALIDA = Path(__file__).resolve().parent / "corpus" / "arxiv_papers.jsonl"


def limpiar(txt: str) -> str:
    return re.sub(r"\s+", " ", txt or "").strip()


def consulta_de(tema: str) -> str:
    """'cs.DB' -> cat:cs.DB;  'cocina=all:cooking' -> all:cooking (etiqueta=consulta).

    Un campo con guion bajo es una frase exacta: 'ti:data_science' -> ti:"data science".
    Evita pelear con las comillas del shell.
    """
    if "=" not in tema:
        return f"cat:{tema}"
    q = tema.split("=", 1)[1]
    return re.sub(r"(\w+):(\w+(?:_\w+)+)",
                  lambda m: f'{m[1]}:"{m[2].replace("_", " ")}"', q)


def pedir(tema: str, inicio: int, n: int) -> bytes:
    params = urllib.parse.urlencode({
        "search_query": consulta_de(tema),
        "start": inicio,
        "max_results": n,
        "sortBy": "submittedDate",
        "sortOrder": "descending",
    })
    req = urllib.request.Request(f"{API}?{params}",
                                 headers={"User-Agent": "ProyectoBD2-RAG/1.0 (curso CS2042)"})
    for intento in range(5):
        try:
            with urllib.request.urlopen(req, timeout=60) as r:
                return r.read()
        except (urllib.error.URLError, TimeoutError) as e:
            espera = 10 * (intento + 1)
            print(f"   fallo de red ({e}); reintento en {espera}s", file=sys.stderr)
            time.sleep(espera)
    raise SystemExit("arXiv no responde; vuelve a correr el script (es reanudable)")


def papers_de(xml: bytes, cat: str):
    raiz = ET.fromstring(xml)
    for e in raiz.findall("a:entry", NS):
        url = e.findtext("a:id", "", NS)
        arxiv_id = url.rsplit("/abs/", 1)[-1]
        titulo = limpiar(e.findtext("a:title", "", NS))
        abstract = limpiar(e.findtext("a:summary", "", NS))
        pub = e.findtext("a:published", "", NS)
        if not (arxiv_id and titulo and abstract and pub[:4].isdigit()):
            continue
        yield {"arxiv_id": arxiv_id, "titulo": titulo, "abstract": abstract,
               "anio": int(pub[:4]), "cat": cat}


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--cats", nargs="+", default=CATS_DEFECTO)
    ap.add_argument("--por-cat", type=int, default=5000, help="papers por categoria")
    ap.add_argument("--salida", type=Path, default=SALIDA)
    a = ap.parse_args()

    a.salida.parent.mkdir(parents=True, exist_ok=True)
    vistos, ultimo_id = set(), 0
    if a.salida.exists():
        with a.salida.open(encoding="utf-8") as f:
            for linea in f:
                p = json.loads(linea)
                vistos.add(p["arxiv_id"].split("v")[0])
                ultimo_id = max(ultimo_id, p["id"])
        print(f"reanudando: {len(vistos)} papers ya descargados")

    nuevos = 0
    with a.salida.open("a", encoding="utf-8") as out:
        for cat in a.cats:
            logrados = 0
            inicio = 0
            while logrados < a.por_cat:
                n = min(POR_PAGINA, a.por_cat - logrados)
                etiqueta = cat.split("=", 1)[0]
                lote = list(papers_de(pedir(cat, inicio, n), etiqueta))
                time.sleep(PAUSA)
                if not lote:
                    break
                inicio += n
                for p in lote:
                    base = p["arxiv_id"].split("v")[0]
                    if base in vistos:
                        continue          # un paper cruzado entre categorias se guarda una vez
                    vistos.add(base)
                    ultimo_id += 1
                    p["id"] = ultimo_id
                    out.write(json.dumps(p, ensure_ascii=False) + "\n")
                    nuevos += 1
                logrados += len(lote)
                out.flush()
                print(f"  {cat}: {logrados}/{a.por_cat}  (total nuevos: {nuevos})")
    print(f"listo: {nuevos} papers nuevos en {a.salida}")


if __name__ == "__main__":
    main()
