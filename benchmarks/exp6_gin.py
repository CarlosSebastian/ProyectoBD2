#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""
exp6_gin.py - la otra mitad del Experimento 6: el indice invertido propio
              contra el indice GIN de PostgreSQL.

Por que esto no contradice la restriccion del enunciado: PostgreSQL se usa
SOLO como punto de comparacion. Los datos del proyecto los sigue guardando e
indexando el motor propio; aqui se carga una copia del mismo corpus en
PostgreSQL para poder decir, con numeros, que tan lejos queda un indice
invertido escrito desde cero de uno de produccion.

Que mide, para consultas booleanas de 1, 3 y 5 terminos:
  - tiempo de ejecucion, con EXPLAIN (ANALYZE, BUFFERS)
  - bloques leidos, que es lo unico comparable de verdad entre dos motores
    distintos: los milisegundos dependen del lenguaje y del hardware, los
    bloques dependen del diseno del indice
  - y lo mismo con el indice desactivado (Seq Scan), que es el equivalente al
    escaneo completo del lado propio

Requiere el cliente psql en el PATH y una base donde crear una tabla.

    python benchmarks/exp6_gin.py --csv data/exp6_corpus.csv \
                                  --consultas data/exp6_consultas.txt
"""

import argparse
import os
import re
import subprocess
import time

TABLA = "exp6_docs"


def psql(sql, args, con_salida=True):
    """Ejecuta SQL y devuelve la salida como texto."""
    cmd = ["psql", "-X", "-q", "-v", "ON_ERROR_STOP=1",
           "-h", args.host, "-p", str(args.port), "-U", args.user, "-d", args.db,
           "-A", "-t", "-c", sql]
    env = dict(os.environ)
    if args.password:
        env["PGPASSWORD"] = args.password
    r = subprocess.run(cmd, capture_output=True, text=True, env=env)
    if r.returncode != 0:
        raise SystemExit("psql fallo:\n" + (r.stderr or r.stdout))
    return r.stdout if con_salida else ""


def copiar_csv(ruta, args):
    """Carga el CSV con \\copy, que corre del lado del cliente."""
    cmd = ["psql", "-X", "-q", "-v", "ON_ERROR_STOP=1",
           "-h", args.host, "-p", str(args.port), "-U", args.user, "-d", args.db,
           "-c", r"\copy %s (id, cuerpo) FROM '%s' WITH (FORMAT csv, HEADER true)"
                 % (TABLA, ruta.replace("'", "''"))]
    env = dict(os.environ)
    if args.password:
        env["PGPASSWORD"] = args.password
    r = subprocess.run(cmd, capture_output=True, text=True, env=env)
    if r.returncode != 0:
        raise SystemExit("la carga fallo:\n" + (r.stderr or r.stdout))


def a_tsquery(expr):
    """'a AND NOT b' -> 'a & !b'   (el formato de to_tsquery)"""
    e = re.sub(r"\bAND NOT\b", "&!", expr)
    e = re.sub(r"\bAND\b", "&", e)
    e = re.sub(r"\bOR\b", "|", e)
    return re.sub(r"\s+", " ", e).strip()


def medir(expr, args, usar_indice):
    """Devuelve (ms, bloques, filas) de una consulta booleana."""
    tsq = a_tsquery(expr)
    cfg = "" if usar_indice else "SET LOCAL enable_indexscan = off; " \
                                 "SET LOCAL enable_bitmapscan = off; "
    sql = ("BEGIN; " + cfg +
           "EXPLAIN (ANALYZE, BUFFERS, TIMING ON) "
           "SELECT id FROM %s WHERE to_tsvector('english', cuerpo) @@ to_tsquery('english', %s); "
           "COMMIT;" % (TABLA, "$q$" + tsq + "$q$"))
    salida = psql(sql, args)
    ms = bloques = filas = 0.0
    for linea in salida.splitlines():
        m = re.search(r"Execution Time: ([\d.]+) ms", linea)
        if m:
            ms = float(m.group(1))
        m = re.search(r"shared hit=(\d+)(?: read=(\d+))?", linea)
        if m:
            bloques += int(m.group(1)) + int(m.group(2) or 0)
        m = re.search(r"actual time=[\d.]+\.\.[\d.]+ rows=(\d+)", linea)
        if m and filas == 0.0:
            filas = float(m.group(1))
    return ms, bloques, filas


def main():
    ap = argparse.ArgumentParser(description="Indice invertido propio vs GIN de PostgreSQL")
    ap.add_argument("--csv", required=True)
    ap.add_argument("--consultas", required=True)
    ap.add_argument("--db", default="postgres")
    ap.add_argument("--user", default="postgres")
    ap.add_argument("--host", default="localhost")
    ap.add_argument("--port", type=int, default=5432)
    ap.add_argument("--password", default=os.environ.get("PGPASSWORD", ""))
    a = ap.parse_args()

    for ruta in (a.csv, a.consultas):
        if not os.path.exists(ruta):
            raise SystemExit("no existe %s. Corre antes ./build/exp6_texto" % ruta)

    print("\n==========================================================================")
    print("  EXPERIMENTO 6 (segunda mitad) - GIN de PostgreSQL sobre el mismo corpus")
    print("==========================================================================\n")

    print(psql("SELECT version();", a).strip().splitlines()[0])

    print("\n-- cargando el corpus --")
    psql("DROP TABLE IF EXISTS %s;" % TABLA, a)
    psql("CREATE TABLE %s (id int PRIMARY KEY, cuerpo text);" % TABLA, a)
    copiar_csv(os.path.abspath(a.csv), a)
    n = psql("SELECT count(*) FROM %s;" % TABLA, a).strip()
    print("   %s documentos" % n)

    print("-- construyendo el indice GIN --")
    t = psql("""\\timing off
        SELECT 1;""", a)
    import time
    t0 = time.time()
    psql("CREATE INDEX ix_gin ON %s USING GIN (to_tsvector('english', cuerpo));" % TABLA, a)
    ms_build = (time.time() - t0) * 1000.0
    tam = psql("SELECT pg_size_pretty(pg_relation_size('ix_gin'));", a).strip()
    print("   %.0f ms, %s en disco" % (ms_build, tam))
    psql("VACUUM ANALYZE %s;" % TABLA, a)

    # agrupar las consultas por (operador, numero de terminos)
    grupos = {}
    with open(a.consultas, encoding="utf-8") as f:
        for linea in f:
            partes = linea.rstrip("\n").split("\t")
            if len(partes) != 3:
                continue
            op, nt, expr = partes
            grupos.setdefault((op, int(nt)), []).append(expr)

    print("\n| operador | terminos | GIN: ms | GIN: bloques | SeqScan: ms | SeqScan: bloques | ms x | filas |")
    print("|---|---|---|---|---|---|---|---|")
    for (op, nt) in sorted(grupos, key=lambda k: (k[0], k[1])):
        exprs = grupos[(op, nt)]
        gi_ms = gi_bl = sc_ms = sc_bl = filas = 0.0
        for e in exprs:
            medir(e, a, True)                      # calentamiento
            m1, b1, f1 = medir(e, a, True)
            m2, b2, _ = medir(e, a, False)
            gi_ms += m1; gi_bl += b1
            sc_ms += m2; sc_bl += b2
            filas += f1
        k = float(len(exprs))
        print("| %s | %d | %.4f | %.0f | %.4f | %.0f | %.1fx | %.0f |"
              % (op, nt, gi_ms / k, gi_bl / k, sc_ms / k, sc_bl / k,
                 (sc_ms / gi_ms) if gi_ms else 0.0, filas / k))

    print("\nLos milisegundos entre dos motores distintos solo dan una idea: PostgreSQL\n"
          "esta en C optimizado desde hace treinta anos y corre sobre su propio buffer\n"
          "pool. La columna que se puede defender en el informe es la de BLOQUES, que\n"
          "depende del diseno del indice y no del lenguaje: ahi es donde se ve si una\n"
          "lista de posteo comprimida con huecos y varint pide mas o menos paginas que\n"
          "un GIN, que guarda arboles de TIDs por lexema.\n")
    psql("DROP TABLE IF EXISTS %s;" % TABLA, a)


if __name__ == "__main__":
    main()
