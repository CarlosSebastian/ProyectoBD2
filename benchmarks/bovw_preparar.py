#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""
bovw_preparar.py - prepara el dataset multimedia del Experimento 7.

Produce las DOS representaciones que el enunciado manda contrastar, sobre las
mismas imagenes:

  1. Paradigma clasico, Bag of Visual Words
     descriptores SIFT de 128 dimensiones -> K-Means -> codebook de K palabras
     visuales -> cada imagen se vuelve un DOCUMENTO de palabras visuales
     ("w17 w17 w3 w250 ..."), que se indexa con el indice invertido de la
     Parte 3 y se puntua con TF-IDF.

  2. Paradigma moderno, embeddings densos
     un vector continuo por imagen (ResNet-18, 512 dimensiones), que se indexa
     con el IVF de la Parte 4.

La idea de fondo del BoVW: si a las palabras visuales las tratas como palabras,
toda la maquinaria de recuperacion de texto --- listas de posteo, TF-IDF,
similitud de coseno --- sirve igual para imagenes sin cambiar una linea del
motor. Por eso aqui no hay codigo de base de datos: solo se escriben dos CSV.

Se escriben DOS tablas y no una porque un registro tiene que caber en UNA
pagina de 4 KB, y el histograma mas el vector de 512 floats no entran juntos.

    pip install numpy opencv-python onnxruntime pillow
    (en modo sintetico basta numpy)

    # con imagenes de verdad (una subcarpeta por clase, como natural_images)
    python benchmarks/bovw_preparar.py --imagenes "C:/Users/carlo/Downloads/lab11/data/natural_images" \
                                       --onnx resnet18-v1-7.onnx --salida data

    # sin imagenes, para probar que la tuberia funciona de punta a punta
    python benchmarks/bovw_preparar.py --sintetico 800 --salida data
"""

import argparse
import csv
import math
import os
import random
import sys
import time

K_PALABRAS = 256          # tamano del codebook
MAX_DESC_KMEANS = 200000  # descriptores que se muestrean para entrenar
DIM_EMB = 512             # ResNet-18 tras el pooling


# ---------------------------------------------------------------------------
#  Modo sintetico: imagenes falsas con estructura de clases
# ---------------------------------------------------------------------------
def generar_sintetico(n, n_clases=8, semilla=2026):
    """Devuelve (nombres, clases, descriptores_por_imagen, embeddings).

    Cada clase tiene sus propios 'motivos' en el espacio de descriptores, asi
    que las imagenes de una misma clase comparten palabras visuales. Es lo
    minimo para que Precision@k signifique algo.

    ESTO NO SIRVE PARA EL INFORME. Es para comprobar que la tuberia completa
    --- descriptores, codebook, CSV, COPY, los dos indices y el experimento ---
    funciona de punta a punta sin tener que esperar a procesar miles de fotos.
    Los numeros de efectividad salen de como se genero el ruido aqui, no de
    ninguna propiedad de las representaciones.
    """
    import numpy as np
    rng = np.random.default_rng(semilla)
    motivos = rng.normal(size=(n_clases, 12, 128)).astype("float32")
    centros = rng.normal(size=(n_clases, DIM_EMB)).astype("float32")
    centros /= np.linalg.norm(centros, axis=1, keepdims=True)

    nombres, clases, descs, embs = [], [], [], []
    for i in range(n):
        c = i % n_clases
        nd = int(rng.integers(80, 160))
        idx = rng.integers(0, 12, size=nd)
        d = motivos[c][idx] + 0.45 * rng.normal(size=(nd, 128)).astype("float32")
        # El ruido se escala por raiz de D: en dimension alta un N(0,1) por
        # componente tiene norma ~sqrt(D) y se comeria al centro, que es
        # unitario. Sin esto los vectores densos salen siendo ruido puro.
        sigma = 0.35 / math.sqrt(DIM_EMB)
        e = centros[c] + sigma * rng.normal(size=DIM_EMB).astype("float32")
        e /= np.linalg.norm(e)
        nombres.append("sint_%s_%04d.png" % (chr(ord("a") + c), i))
        clases.append("clase_%s" % chr(ord("a") + c))
        descs.append(d.astype("float32"))
        embs.append(e.astype("float32"))
    return nombres, clases, descs, embs


# ---------------------------------------------------------------------------
#  Modo real
# ---------------------------------------------------------------------------
def listar_imagenes(raiz):
    exts = (".jpg", ".jpeg", ".png", ".bmp")
    salida = []
    for clase in sorted(os.listdir(raiz)):
        sub = os.path.join(raiz, clase)
        if not os.path.isdir(sub):
            continue
        for f in sorted(os.listdir(sub)):
            if f.lower().endswith(exts):
                salida.append((os.path.join(sub, f), clase))
    return salida


def sift_de(rutas):
    """Descriptores SIFT por imagen. Las que no den ninguno se descartan."""
    import cv2
    import numpy as np
    sift = cv2.SIFT_create()
    descs, buenas = [], []
    for i, (ruta, clase) in enumerate(rutas):
        img = cv2.imread(ruta, cv2.IMREAD_GRAYSCALE)
        if img is None:
            continue
        _, d = sift.detectAndCompute(img, None)
        if d is None or len(d) == 0:
            continue
        descs.append(d.astype("float32"))
        buenas.append((ruta, clase))
        if (i + 1) % 500 == 0:
            print("   SIFT %d/%d" % (i + 1, len(rutas)))
    return buenas, descs


def embeddings_onnx(rutas, modelo):
    """ResNet-18 por onnxruntime: la salida del pooling, 512 dimensiones."""
    import numpy as np
    import onnxruntime as ort
    from PIL import Image

    ses = ort.InferenceSession(modelo, providers=["CPUExecutionProvider"])
    entrada = ses.get_inputs()[0].name
    media = np.array([0.485, 0.456, 0.406], dtype="float32").reshape(3, 1, 1)
    desv  = np.array([0.229, 0.224, 0.225], dtype="float32").reshape(3, 1, 1)

    out = []
    for i, (ruta, _) in enumerate(rutas):
        im = Image.open(ruta).convert("RGB").resize((224, 224))
        x = np.asarray(im, dtype="float32").transpose(2, 0, 1) / 255.0
        x = ((x - media) / desv)[None, :, :, :]
        y = ses.run(None, {entrada: x})[0].reshape(-1).astype("float32")
        n = float(np.linalg.norm(y))
        if n > 0:
            y = y / n
        out.append(y)
        if (i + 1) % 200 == 0:
            print("   embeddings %d/%d" % (i + 1, len(rutas)))
    return out


# ---------------------------------------------------------------------------
class Codebook:
    """K-Means por minilotes, escrito con numpy y nada mas.

    Se evita scikit-learn a proposito: es la unica dependencia que haria falta
    solo para esto, y el algoritmo son veinte lineas. La siembra es k-means++
    sobre una muestra --- el mismo criterio que usa el IVF en C++ --- porque con
    centros al azar en 128 dimensiones varias celdas salen vacias.
    """

    def __init__(self, k, semilla=2026):
        self.k = k
        self.rng = __import__("numpy").random.default_rng(semilla)
        self.centros = None

    def _siembra(self, x):
        import numpy as np
        n = len(x)
        centros = np.empty((self.k, x.shape[1]), dtype="float32")
        centros[0] = x[self.rng.integers(n)]
        d2 = ((x - centros[0]) ** 2).sum(axis=1)
        for i in range(1, self.k):
            total = float(d2.sum())
            if total <= 0.0:
                centros[i] = x[self.rng.integers(n)]
            else:
                centros[i] = x[int(self.rng.choice(n, p=d2 / total))]
            d2 = np.minimum(d2, ((x - centros[i]) ** 2).sum(axis=1))
        return centros

    def entrenar(self, x, iteraciones=25, lote=4096):
        import numpy as np
        sub = x if len(x) <= 20000 else x[self.rng.choice(len(x), 20000, replace=False)]
        self.centros = self._siembra(sub)
        cuenta = np.zeros(self.k, dtype="int64")
        for it in range(iteraciones):
            idx = self.rng.choice(len(x), min(lote, len(x)), replace=False)
            b = x[idx]
            a = self.asignar(b)
            # Actualizacion por minilote: cada centro se mueve hacia su media
            # con un paso que decrece segun cuantos puntos ya ha visto.
            for c in np.unique(a):
                m = b[a == c]
                cuenta[c] += len(m)
                eta = len(m) / float(cuenta[c])
                self.centros[c] = (1.0 - eta) * self.centros[c] + eta * m.mean(axis=0)
        return self

    def asignar(self, x):
        import numpy as np
        # (a-b)^2 = a^2 - 2ab + b^2; el termino a^2 no cambia el argmin.
        prod = x @ self.centros.T
        nc = (self.centros ** 2).sum(axis=1)
        return np.argmin(nc[None, :] - 2.0 * prod, axis=1)


def construir_codebook(descs, k, semilla=2026):
    import numpy as np
    todos = np.vstack(descs)
    if len(todos) > MAX_DESC_KMEANS:
        idx = np.random.default_rng(semilla).choice(len(todos), MAX_DESC_KMEANS, replace=False)
        muestra = todos[idx]
    else:
        muestra = todos
    print("   %d descriptores en total, %d para entrenar, K = %d"
          % (len(todos), len(muestra), k))
    return Codebook(k, semilla).entrenar(muestra)


def documento_de(km, d):
    """La imagen como texto: cada palabra visual repetida su frecuencia.

    Se repite en vez de escribir 'w17:5' porque asi el indice invertido calcula
    el tf solo, sin que haya que ensenarle nada nuevo: para el motor es un
    documento como cualquier otro.
    """
    asign = km.asignar(d)
    cuenta = {}
    for w in asign:
        cuenta[int(w)] = cuenta.get(int(w), 0) + 1
    partes = []
    for w in sorted(cuenta):
        partes.extend(["w%d" % w] * cuenta[w])
    return " ".join(partes)


def main():
    ap = argparse.ArgumentParser(description="BoVW + embeddings densos para el Exp 7")
    ap.add_argument("--imagenes", help="carpeta con una subcarpeta por clase")
    ap.add_argument("--sintetico", type=int, default=0,
                    help="genera N imagenes falsas en vez de leer un dataset")
    ap.add_argument("--onnx", help="ResNet-18 en formato ONNX (modo real)")
    ap.add_argument("--k", type=int, default=K_PALABRAS)
    ap.add_argument("--salida", default="data")
    ap.add_argument("--consultas", type=int, default=3,
                    help="imagenes de consulta por clase")
    a = ap.parse_args()

    if not a.imagenes and not a.sintetico:
        raise SystemExit("usa --imagenes <carpeta> o --sintetico <N>")

    t0 = time.time()
    if a.sintetico:
        print("== modo sintetico: %d imagenes ==" % a.sintetico)
        nombres, clases, descs, embs = generar_sintetico(a.sintetico)
        rutas = list(zip(nombres, clases))
        ms_extraccion = (time.time() - t0) * 1000.0
    else:
        print("== 1. descriptores SIFT ==")
        todas = listar_imagenes(a.imagenes)
        if not todas:
            raise SystemExit("no se encontraron imagenes en " + a.imagenes)
        print("   %d imagenes en %d clases"
              % (len(todas), len({c for _, c in todas})))
        rutas, descs = sift_de(todas)
        ms_extraccion = (time.time() - t0) * 1000.0
        print("== 2. embeddings densos ==")
        if not a.onnx:
            raise SystemExit("falta --onnx con el ResNet-18 (el del Laboratorio 11 sirve)")
        embs = embeddings_onnx(rutas, a.onnx)
        clases = [c for _, c in rutas]
        nombres = [os.path.basename(r) for r, _ in rutas]

    print("== 3. codebook con K-Means ==")
    t1 = time.time()
    km = construir_codebook(descs, a.k)
    ms_codebook = (time.time() - t1) * 1000.0

    print("== 4. escribiendo los CSV ==")
    os.makedirs(a.salida, exist_ok=True)
    ruta_pal = os.path.join(a.salida, "bovw_palabras.csv")
    ruta_emb = os.path.join(a.salida, "bovw_emb.csv")
    ruta_qry = os.path.join(a.salida, "bovw_consultas.txt")

    activas = []
    with open(ruta_pal, "w", encoding="utf-8", newline="") as f:
        w = csv.writer(f)
        w.writerow(["id", "archivo", "clase", "palabras"])
        for i, d in enumerate(descs, start=1):
            doc = documento_de(km, d)
            activas.append(len(set(doc.split())))
            w.writerow([i, nombres[i - 1][:40], clases[i - 1][:20], doc])

    with open(ruta_emb, "w", encoding="utf-8", newline="") as f:
        w = csv.writer(f)
        w.writerow(["id", "archivo", "clase", "emb"])
        for i, e in enumerate(embs, start=1):
            w.writerow([i, nombres[i - 1][:40], clases[i - 1][:20],
                        " ".join("%.6f" % x for x in e)])

    # Consultas: las primeras 'n' de cada clase. Se dejan DENTRO de la
    # coleccion a proposito: con la consulta fuera habria que decidir que es un
    # acierto, y con ella dentro el primer resultado tiene que ser ella misma,
    # que es una comprobacion de cordura gratis.
    por_clase = {}
    with open(ruta_qry, "w", encoding="utf-8") as f:
        for i, c in enumerate(clases, start=1):
            if por_clase.get(c, 0) >= a.consultas:
                continue
            por_clase[c] = por_clase.get(c, 0) + 1
            f.write("%d\t%s\n" % (i, c))

    # El Experimento 7 tiene que reportar el tiempo de extraccion de
    # caracteristicas, y ese tiempo se mide aqui, no en C++.
    with open(os.path.join(a.salida, "bovw_tiempos.txt"), "w", encoding="utf-8") as f:
        f.write("extraccion_ms\t%.1f\n" % ms_extraccion)
        f.write("codebook_ms\t%.1f\n" % ms_codebook)
        f.write("k\t%d\n" % a.k)
        f.write("imagenes\t%d\n" % len(descs))

    media_activas = sum(activas) / float(len(activas)) if activas else 0.0
    print("\nlisto en %.1f s" % (time.time() - t0))
    print("  %-34s %d filas" % (ruta_pal, len(descs)))
    print("  %-34s %d filas" % (ruta_emb, len(embs)))
    print("  %-34s %d consultas" % (ruta_qry, sum(por_clase.values())))
    print("  palabras visuales activas por imagen: %.1f de %d" % (media_activas, a.k))
    print("  extraccion de caracteristicas: %.0f ms   codebook: %.0f ms"
          % (ms_extraccion, ms_codebook))
    print("\nAhora:  ./build/exp7_bovw %s" % a.salida)


if __name__ == "__main__":
    main()
