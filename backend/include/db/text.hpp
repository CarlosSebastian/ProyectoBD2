// ============================================================================
//  text.hpp - Analisis lexico de texto libre para el indice invertido
//
//  Un indice invertido no guarda palabras tal como aparecen en el documento,
//  sino TERMINOS: la forma normalizada de cada palabra. Dos documentos que
//  dicen "Databases" y "database" tienen que caer en la misma lista de
//  postings, o la busqueda no encuentra nada. Esa normalizacion es lo que
//  hace este modulo, en cuatro pasos:
//
//     1. normalizar   pasa a minusculas, quita tildes y convierte en espacio
//                     todo lo que no sea letra ni digito
//     2. separar      parte en palabras por los espacios
//     3. stopwords    descarta las palabras vacias del idioma
//     4. stemming     reduce cada palabra a su raiz
//
//  El idioma se fija al crear el indice y se guarda en su cabecera, porque la
//  consulta TIENE que analizarse exactamente igual que los documentos: si los
//  documentos se indexaron con el stemmer espanol y la consulta se analiza con
//  el ingles, los terminos no coinciden y el resultado es vacio.
//
//  Sobre las tildes: se quitan ANTES del stemming, asi que el stemmer espanol
//  trabaja sobre texto sin acentos y sus listas de sufijos tambien van sin
//  acentos ("acion" y no "accion"/"ación"). Es la variante practica habitual
//  en recuperacion de informacion, y evita arrastrar UTF-8 multibyte por todo
//  el algoritmo. La enie se mapea a 'n', de modo que "ano" y "anio" colisionan;
//  es el precio conocido de esta simplificacion.
// ============================================================================
#pragma once

#include <string>
#include <vector>

namespace db {
namespace texto {

enum class Idioma { EN, ES };

std::string nombreIdioma(Idioma i);
// Acepta "en"/"es" en cualquier combinacion de mayusculas, y tambien
// "ingles"/"english"/"espanol"/"spanish". Lanza DBException si no reconoce.
Idioma idiomaDesdeNombre(const std::string& s);

// Minusculas ASCII, sin tildes, y todo lo que no sea [a-z0-9] pasa a espacio.
std::string normalizar(const std::string& s);

// Palabra vacia del idioma (se asume ya normalizada).
bool esStopword(const std::string& palabra, Idioma i);

// Stemmers. Reciben la palabra YA normalizada (minusculas, sin tildes).
std::string stemIngles(const std::string& palabra);    // Porter (1980)
std::string stemEspanol(const std::string& palabra);   // Snowball espanol, sin tildes

inline std::string stem(const std::string& palabra, Idioma i) {
    return i == Idioma::ES ? stemEspanol(palabra) : stemIngles(palabra);
}

// Los cuatro pasos de arriba, de corrido. Devuelve los terminos EN EL ORDEN
// en que aparecen y CON repeticiones: la frecuencia de termino (tf) que
// necesitan TF-IDF y BM25 sale de contarlos.
std::vector<std::string> tokenizar(const std::string& texto, Idioma i);

// Las palabras de una o dos letras no se indexan: aportan ruido y engordan el
// diccionario. Un termino mas largo que esto se recorta.
constexpr std::size_t LARGO_MINIMO = 3;
constexpr std::size_t LARGO_MAXIMO = 48;

}  // namespace texto
}  // namespace db
