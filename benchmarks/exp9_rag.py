#!/usr/bin/env python3
"""Experimento 9 - Tiempo de respuesta extremo a extremo de la aplicacion RAG.

    python benchmarks/exp9_rag.py                    # los tres modos, 12 preguntas
    python benchmarks/exp9_rag.py --repeticiones 3   # promedia 3 pasadas
    python benchmarks/exp9_rag.py --modos hibrido

Requisitos: el motor corriendo (./build.sh run), Ollama con los dos modelos y
el servidor RAG levantado (python ai/rag_server.py). La ingesta tiene que
haberse hecho antes; si la tabla esta vacia el script lo dice y no mide nada.

Que mide, y por que asi:

El enunciado pide el "tiempo de respuesta extremo a extremo". Un unico numero
no dice nada util, porque la respuesta atraviesa cuatro etapas de costo muy
distinto. El servidor RAG ya instrumenta cada una, asi que este script solo
las recoge y las tabula:

  reescritura -> el LLM convierte la pregunta a una consulta corta en ingles
  embedding   -> all-minilm vectoriza esa consulta (384 dimensiones)
  motor       -> las consultas SQL: BM25 sobre el indice invertido y/o
                 k-NN aproximado sobre el IVF. Es la parte de este proyecto
  generacion  -> el LLM redacta la respuesta con los papers como contexto

El resultado que importa para el informe es el REPARTO: cuanto de la latencia
que percibe el usuario es responsabilidad del motor de base de datos y cuanto
de los modelos. Se mide tambien el I/O del motor, que es la cifra comparable
entre maquinas; los milisegundos de los modelos dependen de la GPU que haya.

Salida: benchmarks/exp9.txt
"""
import argparse
import json
import statistics
import sys
import time
import urllib.error
import urllib.request
from pathlib import Path

RAIZ = Path(__file__).resolve().parent.parent
SALIDA = RAIZ / "benchmarks" / "exp9.txt"

# Preguntas en espanol a proposito: el corpus esta en ingles, asi que la
# reescritura es parte del camino real y tiene que entrar en la medicion.
PREGUNTAS = [
    "¿Que ventajas tiene un indice invertido frente a un escaneo completo?",
    "¿Como funciona la busqueda aproximada de vecinos mas cercanos?",
    "¿Que es el algoritmo SPIMI y para que sirve?",
    "¿Como se comparan BM25 y TF-IDF para ranking de documentos?",
    "¿Que problemas trae la alta dimensionalidad en los indices espaciales?",
    "¿Como se entrenan los modelos de lenguaje con recuperacion aumentada?",
    "¿Que tecnicas hay para comprimir listas de posteo?",
    "¿Cual es la diferencia entre IVF y HNSW para busqueda vectorial?",
    "¿Como se evalua la calidad de un sistema de recuperacion de informacion?",
    "¿Que son los embeddings densos y como se obtienen?",
    "¿Como afecta el tamano de pagina al rendimiento de un arbol B+?",
    "¿Que metodos existen para detectar documentos duplicados?",
]


def pedir(url: str, ruta: str, cuerpo=None, timeout=600):
    datos = json.dumps(cuerpo).encode() if cuerpo is not None else None
    cab = {"Content-Type": "application/json"} if datos else {}
    req = urllib.request.Request(url + ruta, data=datos, headers=cab)
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return json.load(r)


def resumen(valores):
    """media y p95. Con pocas muestras el p95 es el maximo, y esta bien asi:
    lo que interesa es si hay una cola larga, no estimarla con precision."""
    if not valores:
        return 0.0, 0.0
    v = sorted(valores)
    i = min(len(v) - 1, int(round(0.95 * (len(v) - 1))))
    return statistics.fmean(v), v[i]


def medir(url, modo, preguntas, k, reps):
    filas = []
    for n in range(reps):
        for p in preguntas:
            t0 = time.perf_counter()
            try:
                r = pedir(url, "/ask", {"pregunta": p, "modo": modo, "k": k})
            except urllib.error.HTTPError as e:
                print("   fallo en '%s': %s" % (p[:40], e.read()[:120]), file=sys.stderr)
                continue
            total = (time.perf_counter() - t0) * 1000
            t = r.get("tiempos", {})
            mets = [m for m in r.get("metricas", []) if "disk_reads" in m]
            filas.append({
                "total": total,
                "reescritura": t.get("reescritura_ms", 0.0),
                "embedding": t.get("embedding_ms", 0.0),
                "motor": t.get("motor_ms", 0.0),
                "generacion": t.get("generacion_ms", 0.0),
                "disk_reads": sum(m["disk_reads"] for m in mets),
                "accesos": sum(m["page_accesses"] for m in mets),
                "fuentes": len(r.get("fuentes", [])),
            })
            print("   [%s] %5.0f ms  (motor %4.1f ms)  %s" %
                  (modo, total, filas[-1]["motor"], p[:46]))
    return filas


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--url", default="http://localhost:8000")
    ap.add_argument("--modos", nargs="+", default=["bm25", "vectorial", "hibrido"])
    ap.add_argument("--k", type=int, default=5)
    ap.add_argument("--repeticiones", type=int, default=1)
    ap.add_argument("--salida", type=Path, default=SALIDA)
    a = ap.parse_args()

    try:
        s = pedir(a.url, "/api/salud", timeout=120)
    except Exception as e:
        print("No se pudo contactar con el servidor RAG en %s (%s).\n"
              "Levantalo con:  python ai/rag_server.py" % (a.url, e), file=sys.stderr)
        return 1
    print("servidor RAG: motor=%s  ollama=%s  papers=%s  emb=%s  llm=%s"
          % (s["motor"], s["ollama"], s["papers"], s["emb"], s["llm"]))
    if not s["motor"] or not s["ollama"]:
        print("Falta el motor o Ollama; no tiene sentido medir.", file=sys.stderr)
        return 1
    if not s["papers"]:
        print("La tabla de papers esta vacia: corre antes  python ai/ingesta.py",
              file=sys.stderr)
        return 1

    lineas = []
    def w(x=""):
        print(x)
        lineas.append(x)

    w("=" * 74)
    w("  EXPERIMENTO 9 - Tiempo de respuesta extremo a extremo (aplicacion RAG)")
    w("=" * 74)
    w()
    w("%d preguntas x %d repeticion(es), k = %d, corpus de %s papers."
      % (len(PREGUNTAS), a.repeticiones, a.k, s["papers"]))
    w("Embeddings: %s.  Generacion: %s." % (s["emb"], s["llm"]))
    w()

    etapas = [("reescritura", "reescritura (LLM)"), ("embedding", "embedding"),
              ("motor", "MOTOR (SQL)"), ("generacion", "generacion (LLM)")]
    tablas = {}
    for modo in a.modos:
        print("\n-- modo %s --" % modo)
        filas = medir(a.url, modo, PREGUNTAS, a.k, a.repeticiones)
        if filas:
            tablas[modo] = filas

    if not tablas:
        print("No se pudo medir ninguna consulta.", file=sys.stderr)
        return 1

    w("### A. Reparto de la latencia por etapa (ms, media)")
    w()
    w("| modo | " + " | ".join(n for _, n in etapas) + " | total | % del motor |")
    w("|---|" + "---|" * (len(etapas) + 2))
    for modo, filas in tablas.items():
        medias = [statistics.fmean([f[c] for f in filas]) for c, _ in etapas]
        tot = statistics.fmean([f["total"] for f in filas])
        pct = 100.0 * medias[2] / tot if tot else 0.0
        w("| %s | %s | %.1f | %.1f %% |"
          % (modo, " | ".join("%.1f" % m for m in medias), tot, pct))
    w()
    w("La columna que cuenta para este proyecto es la del MOTOR. Las otras tres")
    w("son Ollama, y dependen de la maquina y del modelo, no del diseno de los")
    w("indices.")
    w()

    w("### B. Latencia extremo a extremo: media y cola")
    w()
    w("| modo | media (ms) | p95 (ms) | minimo | maximo | fuentes/consulta |")
    w("|---|---|---|---|---|---|")
    for modo, filas in tablas.items():
        tot = [f["total"] for f in filas]
        media, p95 = resumen(tot)
        w("| %s | %.0f | %.0f | %.0f | %.0f | %.1f |"
          % (modo, media, p95, min(tot), max(tot),
             statistics.fmean([f["fuentes"] for f in filas])))
    w()

    w("### C. Lo que pide el motor en cada pregunta")
    w()
    w("| modo | lecturas de disco | accesos a pagina | ms del motor |")
    w("|---|---|---|---|")
    for modo, filas in tablas.items():
        w("| %s | %.1f | %.1f | %.2f |"
          % (modo,
             statistics.fmean([f["disk_reads"] for f in filas]),
             statistics.fmean([f["accesos"] for f in filas]),
             statistics.fmean([f["motor"] for f in filas])))
    w()
    w("Estas tres columnas son las reproducibles: no dependen de la GPU ni de")
    w("la version del modelo. El modo hibrido paga dos consultas, una por cada")
    w("indice, y a cambio fusiona dos rankings con criterios independientes.")
    w()

    a.salida.parent.mkdir(parents=True, exist_ok=True)
    a.salida.write_text("\n".join(lineas) + "\n", encoding="utf-8")
    try:
        print("\nEscrito", a.salida.relative_to(RAIZ))
    except ValueError:                      # --salida fuera del repositorio
        print("\nEscrito", a.salida)
    return 0


if __name__ == "__main__":
    sys.exit(main())
