"""Utilidades compartidas por la ingesta y el servidor RAG. Solo biblioteca estandar.

Tres servicios externos, todos por HTTP local:
  - el motor C++ (dbserver)  : DB_URL      por defecto http://127.0.0.1:8080
  - Ollama (embeddings, LLM) : OLLAMA_URL  por defecto http://127.0.0.1:11434

Modelos (variables de entorno RAG_EMB y RAG_LLM para cambiarlos):
  - all-minilm : embeddings de 384 dimensiones (solo ingles)
  - qwen2.5:7b : generacion de la respuesta
"""
import json
import os
import urllib.error
import urllib.request

DB_URL = os.environ.get("DB_URL", "http://127.0.0.1:8080")
OLLAMA_URL = os.environ.get("OLLAMA_URL", "http://127.0.0.1:11434")
MODELO_EMB = os.environ.get("RAG_EMB", "all-minilm")
MODELO_LLM = os.environ.get("RAG_LLM", "qwen2.5:7b")


class ServicioCaido(RuntimeError):
    """Un servicio externo no responde. El mensaje dice cual y como levantarlo."""


def _post(url: str, cuerpo: dict, timeout: float, servicio: str, ayuda: str) -> dict:
    req = urllib.request.Request(url, data=json.dumps(cuerpo).encode("utf-8"),
                                 headers={"Content-Type": "application/json"})
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            return json.loads(r.read().decode("utf-8"))
    except urllib.error.HTTPError as e:
        detalle = e.read().decode("utf-8", "replace")[:300]
        raise RuntimeError(f"{servicio} respondio {e.code}: {detalle}") from None
    except (urllib.error.URLError, ConnectionError, TimeoutError) as e:
        raise ServicioCaido(f"No se pudo contactar con {servicio} ({e}). {ayuda}") from None


def embed(textos: list) -> list:
    """Embeddings de una lista de textos, en el mismo orden."""
    r = _post(f"{OLLAMA_URL}/api/embed", {"model": MODELO_EMB, "input": textos},
              300, "Ollama", f"Verifica que Ollama este corriendo y tengas el modelo: ollama pull {MODELO_EMB}")
    if "embeddings" not in r:
        raise RuntimeError(f"Ollama no devolvio embeddings: {str(r)[:200]}")
    return r["embeddings"]


def chat(mensajes: list, temperatura: float = 0.2) -> str:
    r = _post(f"{OLLAMA_URL}/api/chat",
              {"model": MODELO_LLM, "messages": mensajes, "stream": False,
               "options": {"temperature": temperatura}},
              600, "Ollama", f"Verifica que Ollama este corriendo y tengas el modelo: ollama pull {MODELO_LLM}")
    return r["message"]["content"].strip()


def sql(texto: str) -> dict:
    """Ejecuta UNA sentencia en el motor. Devuelve el resultado con sus metricas de I/O.

    Lanza RuntimeError si la sentencia falla, con el mensaje del propio motor.
    """
    r = _post(f"{DB_URL}/api/query", {"sql": texto}, 600, "el motor de base de datos",
              "Levantalo con: ./build.sh run  (desde la raiz del repositorio)")
    res = r["statements"][0] if r.get("statements") else r
    if not res.get("ok"):
        raise RuntimeError(res.get("error") or "el motor rechazo la sentencia")
    return res


def filas(res: dict) -> list:
    """Pasa las filas de un resultado a diccionarios {columna: texto}."""
    cols = res["columns"]
    return [dict(zip(cols, f)) for f in res["rows"]]


def tablas() -> list:
    try:
        with urllib.request.urlopen(f"{DB_URL}/api/tables", timeout=30) as r:
            return json.loads(r.read().decode("utf-8")).get("tables", [])
    except (urllib.error.URLError, ConnectionError, TimeoutError) as e:
        raise ServicioCaido(f"No se pudo contactar con el motor ({e}). "
                            "Levantalo con: ./build.sh run") from None
