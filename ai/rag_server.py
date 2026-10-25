#!/usr/bin/env python3
"""Servidor RAG: recuperacion hibrida en el motor + generacion con un LLM local.

    python ai/rag_server.py            # http://localhost:8000
    python ai/rag_server.py --port 9000

Flujo de POST /ask  {"pregunta": "...", "modo": "hibrido|bm25|vectorial", "k": 5}:
  1. (opcional) el LLM reescribe la pregunta como consulta corta en INGLES. Hace
     falta porque el corpus esta en ingles y all-minilm y el stemmer del indice
     son monolingues: una pregunta en espanol no encontraria casi nada
  2. el embedding de la consulta se calcula con Ollama
  3. DOS consultas SQL al motor:
       BM25      ... WHERE abstract MATCH '...' LIMIT n         (indice invertido)
       vectorial ... ORDER BY emb <=> [...] LIMIT n PROBES p    (IVF, coseno)
  4. los dos rankings se fusionan con Reciprocal Rank Fusion (RRF)
  5. los k mejores papers van al LLM como contexto, que responde citando [id]

Toda la recuperacion pasa por el motor propio; aqui solo se orquesta. La
respuesta devuelve tambien las metricas de I/O de cada consulta del motor.
"""
import argparse
import json
import re
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

import rag_common as rc

AQUI = Path(__file__).resolve().parent
PAGINA = AQUI.parent / "frontend" / "rag.html"
CANDIDATOS = 20      # filas que pide cada consulta antes de fusionar
RRF_K = 60           # constante estandar de Reciprocal Rank Fusion
PROBES = 8           # celdas del IVF a visitar
COLS = "id, arxiv_id, titulo, abstract, anio, cat"

SISTEMA = (
    "Eres un asistente de investigacion. Respondes preguntas usando EXCLUSIVAMENTE los "
    "fragmentos de papers de arXiv que se te entregan en el contexto. Cita cada afirmacion "
    "con el numero del paper entre corchetes, por ejemplo [3]. Si los fragmentos no contienen "
    "la respuesta, dilo con claridad y no inventes nada. Responde en el idioma de la pregunta."
)


def solo_palabras(txt: str) -> str:
    """El lexer SQL no tiene escape de comillas ni de ';': se dejan solo letras, digitos y espacios."""
    return re.sub(r"\s+", " ", re.sub(r"[^\w\s]", " ", txt)).strip()


def reescribir(pregunta: str) -> str:
    out = rc.chat([
        {"role": "system", "content":
            "Convierte la pregunta del usuario en una consulta de busqueda corta en INGLES "
            "(3 a 10 palabras clave, tecnicas). Responde SOLO con la consulta, sin comillas "
            "ni explicaciones."},
        {"role": "user", "content": pregunta}], temperatura=0.0)
    out = solo_palabras(out.splitlines()[0] if out else "")
    return out or solo_palabras(pregunta)


def metricas(nombre: str, sentencia: str, res: dict) -> dict:
    return {"consulta": nombre, "sql": sentencia, "metodo": res.get("metodo", ""),
            "filas": res.get("row_count", 0), "disk_reads": res["disk_reads"],
            "disk_writes": res["disk_writes"], "page_accesses": res["page_accesses"],
            "exec_ms": res["exec_ms"], "total_ms": res["total_ms"],
            "plan": [f'{p["paso"]}: {p["detalle"]}' for p in res.get("plan", [])]}


def buscar_bm25(consulta: str):
    q = solo_palabras(consulta)
    if not q:
        return [], None
    s = f"SELECT {COLS} FROM papers WHERE abstract MATCH '{q}' LIMIT {CANDIDATOS};"
    try:
        r = rc.sql(s)
    except RuntimeError as e:         # p. ej. una consulta de puras stopwords
        return [], {"consulta": "BM25", "sql": s, "error": str(e)}
    return rc.filas(r), metricas("BM25 (indice invertido)", s, r)


def buscar_vectorial(vec: list):
    lit = "[" + ",".join(f"{x:.6g}" for x in vec) + "]"
    s = f"SELECT {COLS} FROM papers ORDER BY emb <=> {lit} LIMIT {CANDIDATOS} PROBES {PROBES};"
    r = rc.sql(s)
    return rc.filas(r), metricas("Vectorial (IVF, coseno)", s.replace(lit, "[...]"), r)


def fusionar(bm25: list, vec: list, k: int) -> list:
    docs = {}
    for lista, campo_rank, campo_val, col in ((bm25, "rank_bm25", "bm25", "score"),
                                              (vec, "rank_vec", "distancia", "distancia")):
        for pos, fila in enumerate(lista, 1):
            d = docs.setdefault(fila["id"], {**{c: fila[c] for c in
                                                ("id", "arxiv_id", "titulo", "abstract", "anio", "cat")},
                                             "rrf": 0.0, "rank_bm25": None, "rank_vec": None,
                                             "bm25": None, "distancia": None})
            d[campo_rank] = pos
            d[campo_val] = float(fila[col])
            d["rrf"] += 1.0 / (RRF_K + pos)
    return sorted(docs.values(), key=lambda d: -d["rrf"])[:k]


def preguntar(cuerpo: dict) -> dict:
    pregunta = (cuerpo.get("pregunta") or "").strip()
    if not pregunta:
        raise ValueError("Escribe una pregunta")
    modo = cuerpo.get("modo", "hibrido")
    if modo not in ("hibrido", "bm25", "vectorial"):
        raise ValueError("modo debe ser hibrido, bm25 o vectorial")
    k = max(1, min(10, int(cuerpo.get("k", 5))))
    t = {}

    t0 = time.time()
    consulta = reescribir(pregunta) if cuerpo.get("reescribir", True) else solo_palabras(pregunta)
    t["reescritura_ms"] = (time.time() - t0) * 1000

    mets, bm25, vec = [], [], []
    if modo in ("hibrido", "vectorial"):
        t0 = time.time()
        emb = rc.embed([consulta])[0]
        t["embedding_ms"] = (time.time() - t0) * 1000
        vec, m = buscar_vectorial(emb)
        mets.append(m)
    if modo in ("hibrido", "bm25"):
        bm25, m = buscar_bm25(consulta)
        if m:
            mets.append(m)

    fuentes = fusionar(bm25, vec, k)
    if not fuentes:
        return {"respuesta": "No encontre papers relacionados con la pregunta en el corpus.",
                "consulta": consulta, "modo": modo, "fuentes": [], "metricas": mets, "tiempos": t}

    contexto = "\n\n".join(f"[{d['id']}] {d['titulo']} ({d['anio']}, {d['cat']})\n{d['abstract']}"
                           for d in fuentes)
    t0 = time.time()
    respuesta = rc.chat([
        {"role": "system", "content": SISTEMA},
        {"role": "user", "content": f"Contexto:\n{contexto}\n\nPregunta: {pregunta}"}])
    t["generacion_ms"] = (time.time() - t0) * 1000
    t["motor_ms"] = sum(m.get("total_ms", 0) for m in mets)
    return {"respuesta": respuesta, "consulta": consulta, "modo": modo,
            "fuentes": fuentes, "metricas": mets, "tiempos": t}


def salud() -> dict:
    out = {"motor": False, "papers": 0, "ollama": False, "emb": rc.MODELO_EMB, "llm": rc.MODELO_LLM}
    try:
        for tb in rc.tablas():
            if tb["name"] == "papers":
                out["papers"] = tb["rows"]
        out["motor"] = True
    except rc.ServicioCaido:
        pass
    try:
        rc.embed(["ping"])
        out["ollama"] = True
    except (rc.ServicioCaido, RuntimeError):
        pass
    return out


class Handler(BaseHTTPRequestHandler):
    def _json(self, codigo: int, obj: dict):
        datos = json.dumps(obj, ensure_ascii=False).encode("utf-8")
        self.send_response(codigo)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(datos)))
        self.end_headers()
        self.wfile.write(datos)

    def do_GET(self):
        if self.path in ("/", "/index.html", "/rag.html"):
            datos = PAGINA.read_bytes()
            self.send_response(200)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.send_header("Content-Length", str(len(datos)))
            self.end_headers()
            self.wfile.write(datos)
        elif self.path == "/api/salud":
            self._json(200, salud())
        else:
            self._json(404, {"error": "no existe"})

    def do_POST(self):
        if self.path != "/ask":
            return self._json(404, {"error": "no existe"})
        try:
            n = int(self.headers.get("Content-Length", 0))
            self._json(200, preguntar(json.loads(self.rfile.read(n) or b"{}")))
        except ValueError as e:
            self._json(400, {"error": str(e)})
        except rc.ServicioCaido as e:
            self._json(503, {"error": str(e)})
        except Exception as e:                      # el servidor no debe caerse por una consulta
            self._json(500, {"error": f"{type(e).__name__}: {e}"})

    def log_message(self, fmt, *args):
        print("  " + fmt % args)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--port", type=int, default=8000)
    a = ap.parse_args()
    print(f"RAG en http://localhost:{a.port}   motor={rc.DB_URL}  ollama={rc.OLLAMA_URL}")
    print(f"  embeddings={rc.MODELO_EMB}  llm={rc.MODELO_LLM}")
    ThreadingHTTPServer(("0.0.0.0", a.port), Handler).serve_forever()


if __name__ == "__main__":
    main()
