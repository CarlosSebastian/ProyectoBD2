#!/usr/bin/env python3
# ============================================================================
#  graficas.py - Genera las figuras del informe a partir de
#  benchmarks/resultados.md
#
#  No hay numeros escritos a mano: se parsean las tablas del archivo de
#  resultados, asi que si se vuelven a correr los experimentos las figuras
#  quedan actualizadas con solo volver a ejecutar este script.
#
#      python3 benchmarks/graficas.py
#
#  Salida: informe/figuras/*.pdf  (vectorial, para \includegraphics)
# ============================================================================
import os
import math
import re
import sys

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.ticker import FuncFormatter, LogLocator

RAIZ     = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ENTRADA  = os.path.join(RAIZ, "benchmarks", "resultados.md")
SALIDA   = os.path.join(RAIZ, "informe", "figuras")

# --- tokens de la paleta (validados con el validador de la guia) ------------
SUP        = "#fcfcfb"   # superficie del grafico
TINTA      = "#0b0b0b"   # tinta primaria
TINTA_2    = "#52514e"   # tinta secundaria
MUTED      = "#898781"   # ejes y etiquetas
REJILLA    = "#e1e0d9"   # rejilla (hairline)
EJE        = "#c3c2b7"   # linea base

# categoricas, en el orden fijo de la guia
C1, C2, C3, C4 = "#2a78d6", "#eb6834", "#1baf7a", "#eda100"
# rampa secuencial azul (magnitud)
SEQ = ["#86b6ef", "#3987e5", "#256abf", "#0d366b"]

plt.rcParams.update({
    "figure.facecolor": SUP, "axes.facecolor": SUP, "savefig.facecolor": SUP,
    "font.family": "DejaVu Sans", "font.size": 9,
    "axes.edgecolor": EJE, "axes.labelcolor": TINTA_2, "axes.titlecolor": TINTA,
    "axes.titlesize": 10.5, "axes.titleweight": "bold", "axes.labelsize": 9,
    "xtick.color": MUTED, "ytick.color": MUTED,
    "xtick.labelsize": 8.5, "ytick.labelsize": 8.5,
    "grid.color": REJILLA, "grid.linewidth": 0.6,
    # Leyenda sin borde pero con fondo del color de la superficie: asi tapa las
    # lineas que pasen por debajo sin agregar cromo visible.
    "legend.frameon": True, "legend.facecolor": SUP, "legend.edgecolor": SUP,
    "legend.framealpha": 1.0, "legend.borderpad": 0.3,
    "legend.fontsize": 8.5, "legend.labelcolor": TINTA_2,
    "lines.linewidth": 2.0, "lines.markersize": 5.5,
    "axes.spines.top": False, "axes.spines.right": False,
    "figure.dpi": 150,
})


# ---------------------------------------------------------------------------
#  Parseo de las tablas Markdown del archivo de resultados
# ---------------------------------------------------------------------------
def leer_tablas(ruta):
    """Devuelve {titulo_de_seccion: [filas]} con las filas ya separadas."""
    with open(ruta, encoding="utf-8") as f:
        lineas = f.read().splitlines()

    tablas, titulo, filas = {}, None, []
    for ln in lineas:
        s = ln.strip()
        if s.startswith("###"):
            if titulo and filas:
                tablas[titulo] = filas
            titulo, filas = s.lstrip("#").strip(), []
        elif s.startswith("|") and titulo:
            celdas = [c.strip() for c in s.strip("|").split("|")]
            if all(set(c) <= set("-: ") for c in celdas):
                continue                      # separador
            filas.append(celdas)
    if titulo and filas:
        tablas[titulo] = filas
    return tablas


def cruceDeResultados(tablas):
    """Lee el punto de cruce del B+ de la tabla que escribe exp3_rangos.

    Devuelve la selectividad en por ciento, o None si el experimento reporto
    que no hay cruce dentro del rango medido.
    """
    try:
        filas = buscar(tablas, "Punto de cruce")
    except KeyError:
        return None
    for f in filas:
        if f and "B+" in f[0]:
            m = re.search(r"([\d.]+)\s*%", f[1] if len(f) > 1 else "")
            return float(m.group(1)) if m else None
    return None


def buscar(tablas, fragmento):
    for k, v in tablas.items():
        if fragmento.lower() in k.lower():
            return v
    raise KeyError("No se encontro la tabla: " + fragmento)


def numero(txt):
    txt = txt.replace(" ", "")
    if not re.match(r"^-?[\d.]+$", txt):
        return None
    try:
        return float(txt)
    except ValueError:
        return None


def etiquetas_extremo(ax, items, dx=1.05, fuente=8.5, holgura=1.35):
    """Etiquetas directas al final de cada serie, separadas para que no choquen.

    La separacion se calcula en PIXELES, no en unidades de datos: es donde
    ocurre la colision. Dos series con valores parecidos (1978 y 2192 ms)
    estan a 0.04 decadas en un eje logaritmico, o sea unos 2 px: cualquier
    umbral expresado en unidades de datos falla en cuanto cambia el tamano de
    la figura o el rango del eje.
    """
    fig = ax.figure
    fig.canvas.draw()
    # Altura real del texto en pixeles = puntos x dpi / 72. Con 8.5 pt a 150 dpi
    # son 17.7 px: un hueco menor que eso hace que las etiquetas se toquen.
    gap_px = fuente * fig.dpi / 72.0 * holgura
    pts = [ax.transData.transform((x, y)) for x, y, _, _ in items]
    orden = sorted(range(len(items)), key=lambda i: pts[i][1])
    ys = [pts[i][1] for i in orden]
    for k in range(1, len(ys)):
        if ys[k] - ys[k - 1] < gap_px:
            ys[k] = ys[k - 1] + gap_px
    inv = ax.transData.inverted()
    for k, i in enumerate(orden):
        x, y, texto, color = items[i]
        y_etq = inv.transform((pts[i][0], ys[k]))[1]
        ax.annotate(texto, xy=(x, y), xytext=(x * dx, y_etq),
                    color=color, fontsize=fuente, fontweight="bold",
                    va="center", ha="left", annotation_clip=False)


def guardar(fig, nombre):
    os.makedirs(SALIDA, exist_ok=True)
    ruta = os.path.join(SALIDA, nombre)
    fig.savefig(ruta, bbox_inches="tight", pad_inches=0.12)
    plt.close(fig)
    print("  escrito", os.path.relpath(ruta, RAIZ))


miles = FuncFormatter(lambda v, _: f"{int(v):,}".replace(",", " ") if v >= 1 else f"{v:g}")


# ---------------------------------------------------------------------------
#  Figura 1 - Experimento 1: tiempo de carga
# ---------------------------------------------------------------------------
def fig_insercion(tablas):
    filas = buscar(tablas, "Tiempo total de carga")
    cab, datos = filas[0], filas[1:]
    Ns = [int(re.sub(r"\D", "", c)) for c in cab[1:]]

    series = {}
    for f in datos:
        vals = [numero(c) for c in f[1:]]
        series[f[0]] = vals

    quiero = [("Heap File", C1), ("Heap + Hash dinamico", C3),
              ("Heap + B+ Tree", C2), ("Sequential con reorg.", C4)]
    nombres = {"Heap File": "Heap File", "Heap + Hash dinamico": "Heap + Hash",
               "Heap + B+ Tree": "Heap + B+ Tree", "Sequential con reorg.": "Sequential\ncon reorg."}

    fig, ax = plt.subplots(figsize=(6.4, 3.6))
    etiquetas = []
    for clave, color in quiero:
        ys = series.get(clave)
        if not ys:
            continue
        xs = [n for n, y in zip(Ns, ys) if y is not None and y > 0]
        vs = [y for y in ys if y is not None and y > 0]
        etq = nombres[clave].replace("\n", " ")
        ax.plot(xs, vs, color=color, marker="o", markerfacecolor=color,
                markeredgecolor=SUP, markeredgewidth=1.2, label=etq, zorder=3)
        etiquetas.append((xs[-1], vs[-1], etq, color))
    etiquetas_extremo(ax, etiquetas)

    ax.set_xscale("log"); ax.set_yscale("log")
    ax.set_xlabel("Tuplas insertadas (N)")
    ax.set_ylabel("Tiempo total de carga (ms)")
    ax.set_title("Costo de inserción masiva")
    ax.grid(True, which="major", axis="both", zorder=0)
    ax.set_axisbelow(True)
    ax.xaxis.set_major_formatter(miles); ax.yaxis.set_major_formatter(miles)
    ax.set_xlim(800, 3.2e6)
    ax.legend(loc="upper left", ncols=2)
    guardar(fig, "fig1_insercion.pdf")


# ---------------------------------------------------------------------------
#  Figura 2 - Experimento 1: degradacion sin reorganizacion
# ---------------------------------------------------------------------------
def fig_degradacion(tablas):
    filas = buscar(tablas, "Degradacion del Sequential")
    datos = filas[1:]
    Ns  = [numero(f[0]) for f in datos]
    ups = [numero(f[2]) for f in datos]

    # contraparte: us/tupla de la variante con mantenimiento
    t_tiempo = buscar(tablas, "Tiempo total de carga")
    cab = t_tiempo[0]
    Ns_c = [int(re.sub(r"\D", "", c)) for c in cab[1:]]
    con = None
    for f in t_tiempo[1:]:
        if f[0].startswith("Sequential con"):
            con = [numero(c) for c in f[1:]]
    con_x, con_y = [], []
    if con:
        for n, ms in zip(Ns_c, con):
            if ms is not None and ms > 0:
                con_x.append(n); con_y.append(ms * 1000.0 / n)

    fig, ax = plt.subplots(figsize=(6.4, 3.4))
    etiquetas = []
    ax.plot(Ns, ups, color=C2, marker="o", markerfacecolor=C2,
            markeredgecolor=SUP, markeredgewidth=1.2, label="Sin reorganizar", zorder=3)
    etiquetas.append((Ns[-1], ups[-1], "Sin reorganizar", C2))
    if con_x:
        ax.plot(con_x, con_y, color=C1, marker="o", markerfacecolor=C1,
                markeredgecolor=SUP, markeredgewidth=1.2, label="Con reorganización", zorder=3)
        etiquetas.append((con_x[-1], con_y[-1], "Con reorganización", C1))
    etiquetas_extremo(ax, etiquetas)

    # marcar donde la cadena deja de caber en el buffer pool
    lecturas = [numero(f[3]) for f in datos]
    for n, u, r in zip(Ns, ups, lecturas):
        if r and r > 0:
            ax.annotate("aquí la cadena deja de caber\nen el buffer pool",
                        xy=(n, u), xytext=(n * 1.15, u * 0.14),
                        color=TINTA_2, fontsize=8, ha="left",
                        arrowprops=dict(arrowstyle="-", color=MUTED, lw=0.9))
            break

    ax.set_xscale("log"); ax.set_yscale("log")
    ax.set_xlabel("Tuplas insertadas (N)")
    ax.set_ylabel("Costo por tupla (µs)")
    ax.set_title("Sequential File: degradación sin mantenimiento")
    ax.grid(True, which="major", zorder=0); ax.set_axisbelow(True)
    ax.xaxis.set_major_formatter(miles); ax.yaxis.set_major_formatter(miles)
    ax.set_xlim(700, 3.6e6)
    ax.legend(loc="lower right")
    guardar(fig, "fig2_degradacion.pdf")


# ---------------------------------------------------------------------------
#  Figura 3 - Experimento 2: busquedas puntuales
# ---------------------------------------------------------------------------
def fig_puntual(tablas):
    filas = buscar(tablas, "Lecturas de disco por consulta")
    datos = [f for f in filas[1:] if numero(f[1]) is not None]
    nombres = [f[0].replace("Busqueda Binaria", "Búsqueda binaria")
                   .replace("Arbol", "Árbol")
                   .replace("dinamico", "dinámico") for f in datos]
    medias  = [numero(f[1]) for f in datos]

    orden = sorted(range(len(medias)), key=lambda i: medias[i])
    nombres = [nombres[i] for i in orden]
    medias  = [medias[i] for i in orden]
    colores = [SEQ[min(i, len(SEQ) - 1)] for i in range(len(medias))]

    fig, ax = plt.subplots(figsize=(6.0, 2.9))
    y = range(len(medias))
    ax.barh(list(y), medias, color=colores, height=0.62, zorder=3,
            edgecolor=SUP, linewidth=1.2)
    for i, v in zip(y, medias):
        ax.text(v * 1.18, i, f"{v:,.1f}".replace(",", " "), va="center", ha="left",
                color=TINTA, fontsize=8.5, fontweight="bold")

    ax.set_yticks(list(y)); ax.set_yticklabels(nombres, color=TINTA_2, fontsize=8.5)
    ax.set_xscale("log")
    ax.set_xlabel("Lecturas de disco por consulta (media de 1000 consultas)")
    ax.set_title("Búsqueda puntual: transferencias por consulta")
    ax.grid(True, axis="x", which="major", zorder=0); ax.set_axisbelow(True)
    ax.xaxis.set_major_formatter(miles)
    ax.set_xlim(1, max(medias) * 4)
    ax.spines["left"].set_visible(False)
    ax.tick_params(axis="y", length=0)
    guardar(fig, "fig3_puntual.pdf")


# ---------------------------------------------------------------------------
#  Figura 4 - Experimento 3: rangos (la figura clave)
# ---------------------------------------------------------------------------
def fig_rangos(tablas):
    filas = buscar(tablas, "Latencia por consulta (ms, media)")
    cab, datos = filas[0], filas[1:]
    sel = [numero(c.replace("%", "")) for c in cab[1:]]

    serie = {}
    for f in datos:
        serie[f[0]] = [numero(c) for c in f[1:]]

    quiero = [("Full Scan (Heap)", C3, "Full Scan"),
              ("Sequential File", C1, "Sequential File"),
              ("Arbol B+", C2, "Árbol B+")]

    fig, ax = plt.subplots(figsize=(6.4, 3.8))
    etiquetas = []
    for clave, color, etq in quiero:
        ys = serie.get(clave)
        if not ys:
            continue
        ax.plot(sel, ys, color=color, marker="o", markerfacecolor=color,
                markeredgecolor=SUP, markeredgewidth=1.2, label=etq, zorder=3)
        etiquetas.append((sel[-1], ys[-1], etq, color))
    etiquetas_extremo(ax, etiquetas, dx=1.08)

    # Marcar el punto de cruce entre el B+ y el full scan. El valor NO se
    # recalcula aqui: se lee de la tabla que escribe el propio experimento, para
    # que la figura, el texto del informe y la salida cruda no puedan discrepar.
    bp = serie.get("Arbol B+")
    xc = cruceDeResultados(tablas)
    if xc is not None and bp:
        ax.axvline(xc, color=MUTED, lw=0.9, ls=(0, (3, 3)), zorder=2)
        ax.annotate(f"punto de cruce ≈ {xc:.1f} %:\nel B+ deja de convenir",
                    xy=(xc, min(bp) * 1.3), xytext=(xc * 0.30, min(bp) * 0.42),
                    color=TINTA_2, fontsize=8, ha="center",
                    arrowprops=dict(arrowstyle="-", color=MUTED, lw=0.9))

    ax.set_xscale("log"); ax.set_yscale("log")
    ax.set_xlabel("Selectividad de la consulta (% de las tuplas devueltas)")
    ax.set_ylabel("Latencia por consulta (ms)")
    ax.set_title("Rangos: el índice no agrupado pierde al crecer la selectividad", pad=12)
    ax.grid(True, which="major", zorder=0); ax.set_axisbelow(True)
    ax.set_xticks(sel); ax.set_xticklabels([f"{s:g} %" for s in sel])
    ax.xaxis.set_minor_locator(LogLocator(subs=()))
    ax.yaxis.set_major_formatter(miles)
    ax.set_xlim(0.07, 90)
    ax.set_ylim(min(min(v for v in ys if v) for ys in serie.values()) * 0.22, None)
    ax.legend(loc="upper left")
    guardar(fig, "fig4_rangos.pdf")


# ---------------------------------------------------------------------------
#  Figura 5 - Experimento 4: tamano de bloque (dos paneles, nunca doble eje)
# ---------------------------------------------------------------------------
def fig_bloque(tablas):
    filas = buscar(tablas, "Fan-out, altura y transferencias")
    datos = filas[1:]
    B      = [numero(f[0]) for f in datos]
    altura = [numero(f[3]) for f in datos]
    lect   = [numero(f[6]) for f in datos]
    bytes_ = [numero(f[7]) for f in datos]

    fig, (a1, a2) = plt.subplots(1, 2, figsize=(6.4, 2.9))

    a1.plot(B, lect, color=C1, marker="o", markerfacecolor=C1,
            markeredgecolor=SUP, markeredgewidth=1.2, zorder=3)
    for x, y, h in zip(B, lect, altura):
        a1.annotate(f"h={int(h)}", xy=(x, y), xytext=(0, 9), textcoords="offset points",
                    color=TINTA_2, fontsize=8, ha="center")
    a1.set_title("Transferencias por consulta", fontsize=9.5)
    a1.set_ylabel("Lecturas de disco")
    a1.set_ylim(min(lect) * 0.88, max(lect) * 1.16)

    a2.plot(B, bytes_, color=C2, marker="o", markerfacecolor=C2,
            markeredgecolor=SUP, markeredgewidth=1.2, zorder=3)
    a2.set_title("Bytes movidos por consulta", fontsize=9.5)
    a2.set_ylabel("Bytes")
    a2.yaxis.set_major_formatter(miles)

    for a in (a1, a2):
        a.set_xscale("log", base=2)
        a.set_xticks(B); a.set_xticklabels([f"{int(b/1024)} KB" for b in B])
        a.xaxis.set_minor_locator(LogLocator(base=2, subs=()))
        a.set_xlabel("Tamaño de bloque B")
        a.grid(True, which="major", zorder=0); a.set_axisbelow(True)

    fig.suptitle("Sensibilidad al tamaño de bloque", fontsize=10.5,
                 fontweight="bold", color=TINTA, y=1.04)
    fig.tight_layout(w_pad=2.4)
    guardar(fig, "fig5_bloque.pdf")


def main():
    if not os.path.exists(ENTRADA):
        print("No existe", ENTRADA, "- corre antes ./benchmarks/run_experimentos.sh")
        return 1
    tablas = leer_tablas(ENTRADA)
    print("Generando figuras desde", os.path.relpath(ENTRADA, RAIZ))
    fig_insercion(tablas)
    fig_degradacion(tablas)
    fig_puntual(tablas)
    fig_rangos(tablas)
    fig_bloque(tablas)
    print("Listo.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
