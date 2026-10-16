#include "db/text.hpp"

#include <algorithm>
#include <cctype>
#include <unordered_set>

#include "db/common.hpp"

namespace db {
namespace texto {

std::string nombreIdioma(Idioma i) { return i == Idioma::ES ? "ES" : "EN"; }

Idioma idiomaDesdeNombre(const std::string& s) {
    std::string u;
    for (char c : s) u += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    if (u == "EN" || u == "ENG" || u == "INGLES" || u == "ENGLISH") return Idioma::EN;
    if (u == "ES" || u == "SPA" || u == "ESPANOL" || u == "SPANISH") return Idioma::ES;
    throw DBException("Idioma no reconocido: '" + s + "' (se admiten EN y ES)");
}

// ---------------------------------------------------------------------------
//  Normalizacion
//
//  El texto entra en UTF-8. Las letras acentuadas del espanol viven todas en
//  el bloque Latin-1 Suplemento, que en UTF-8 son dos bytes empezando por
//  0xC3: basta mirar el segundo byte para saber que vocal es. Cualquier otra
//  secuencia multibyte (griego, matematicas, emojis) se convierte en espacio,
//  igual que la puntuacion.
// ---------------------------------------------------------------------------
static char plegarLatin1(unsigned char segundo) {
    if (segundo >= 0x80 && segundo <= 0x85) return 'a';   // A grave .. A anillo
    if (segundo == 0x87)                    return 'c';   // C cedilla
    if (segundo >= 0x88 && segundo <= 0x8B) return 'e';
    if (segundo >= 0x8C && segundo <= 0x8F) return 'i';
    if (segundo == 0x91)                    return 'n';   // enie mayuscula
    if (segundo >= 0x92 && segundo <= 0x96) return 'o';
    if (segundo >= 0x99 && segundo <= 0x9C) return 'u';
    if (segundo == 0x9D)                    return 'y';
    if (segundo >= 0xA0 && segundo <= 0xA5) return 'a';
    if (segundo == 0xA7)                    return 'c';
    if (segundo >= 0xA8 && segundo <= 0xAB) return 'e';
    if (segundo >= 0xAC && segundo <= 0xAF) return 'i';
    if (segundo == 0xB1)                    return 'n';   // enie minuscula
    if (segundo >= 0xB2 && segundo <= 0xB6) return 'o';
    if (segundo >= 0xB9 && segundo <= 0xBC) return 'u';
    if (segundo == 0xBD || segundo == 0xBF) return 'y';
    return ' ';
}

std::string normalizar(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (c < 0x80) {
            if (std::isalnum(c)) out += static_cast<char>(std::tolower(c));
            else                 out += ' ';
            continue;
        }
        if (c == 0xC3 && i + 1 < s.size()) {
            out += plegarLatin1(static_cast<unsigned char>(s[i + 1]));
            ++i;
            continue;
        }
        // Otra secuencia multibyte: se descarta entera (cabecera + continuacion).
        out += ' ';
        if (c >= 0xC0) {
            int extra = (c >= 0xF0) ? 3 : (c >= 0xE0) ? 2 : 1;
            while (extra-- > 0 && i + 1 < s.size() &&
                   (static_cast<unsigned char>(s[i + 1]) & 0xC0) == 0x80) ++i;
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
//  Stopwords
//
//  Las palabras mas frecuentes del idioma aparecen en casi todos los
//  documentos, asi que su IDF es practicamente cero: no discriminan nada y en
//  cambio sus listas de postings son las mas largas del indice. Quitarlas
//  reduce mucho el tamano del indice y el costo de las consultas.
// ---------------------------------------------------------------------------
static const std::unordered_set<std::string>& stopEn() {
    static const std::unordered_set<std::string> s = {
        "a","about","above","after","again","against","all","am","an","and","any","are","aren",
        "as","at","be","because","been","before","being","below","between","both","but","by",
        "can","cannot","could","couldn","did","didn","do","does","doesn","doing","don","down",
        "during","each","few","for","from","further","had","hadn","has","hasn","have","haven",
        "having","he","her","here","hers","herself","him","himself","his","how","i","if","in",
        "into","is","isn","it","its","itself","let","me","more","most","mustn","my","myself",
        "no","nor","not","of","off","on","once","only","or","other","ought","our","ours",
        "ourselves","out","over","own","same","shan","she","should","shouldn","so","some","such",
        "than","that","the","their","theirs","them","themselves","then","there","these","they",
        "this","those","through","to","too","under","until","up","very","was","wasn","we","were",
        "weren","what","when","where","which","while","who","whom","why","with","won","would",
        "wouldn","you","your","yours","yourself","yourselves"
    };
    return s;
}

static const std::unordered_set<std::string>& stopEs() {
    static const std::unordered_set<std::string> s = {
        "a","al","algo","algun","alguna","algunas","alguno","algunos","ante","antes","aquel",
        "aquella","aquellas","aquellos","aqui","asi","aun","aunque","bajo","bien","cada","como",
        "con","contra","cual","cuales","cuando","cuanto","de","del","desde","donde","dos","el",
        "ella","ellas","ello","ellos","en","entre","era","eran","eres","es","esa","esas","ese",
        "eso","esos","esta","estaba","estaban","estado","estamos","estan","estar","estas","este",
        "esto","estos","estoy","fue","fueron","fui","fuimos","ha","habia","habian","han","hasta",
        "hay","he","hemos","hube","la","las","le","les","lo","los","mas","me","mi","mientras",
        "mis","mucho","muchos","muy","nada","ni","no","nos","nosotros","nuestra","nuestras",
        "nuestro","nuestros","o","os","otra","otras","otro","otros","para","pero","poco","por",
        "porque","que","quien","quienes","se","sea","sean","segun","ser","si","sido","siempre",
        "sin","sobre","sois","somos","son","soy","su","sus","tal","tambien","tanto","te","tenemos",
        "tener","tengo","ti","tiene","tienen","toda","todas","todo","todos","tu","tus","un","una",
        "unas","uno","unos","usted","ustedes","va","vamos","van","vosotros","vuestra","vuestro",
        "y","ya","yo"
    };
    return s;
}

bool esStopword(const std::string& palabra, Idioma i) {
    const std::unordered_set<std::string>& s = (i == Idioma::ES) ? stopEs() : stopEn();
    return s.find(palabra) != s.end();
}

// ---------------------------------------------------------------------------
//  Utilidades compartidas por los dos stemmers
// ---------------------------------------------------------------------------
static bool terminaEn(const std::string& p, const char* suf, std::size_t n) {
    return p.size() >= n && p.compare(p.size() - n, n, suf, n) == 0;
}
template <std::size_t N>
static bool terminaEn(const std::string& p, const char (&suf)[N]) {
    return terminaEn(p, suf, N - 1);
}

// ===========================================================================
//  Stemmer de Porter (ingles, 1980)
//
//  Cinco pasos de reescritura de sufijos, cada uno condicionado por la MEDIDA
//  del radical: escrita una palabra como [C](VC)^m[V], la medida es ese m, o
//  sea cuantas alternancias vocal-consonante tiene. La idea es que un sufijo
//  solo se quita si lo que queda sigue siendo una palabra "con cuerpo": asi
//  "relate" -> "relat" pero "rate" no pierde la 'e'.
// ===========================================================================
namespace {

bool esVocal(const std::string& w, std::size_t i) {
    switch (w[i]) {
        case 'a': case 'e': case 'i': case 'o': case 'u': return true;
        case 'y': return i > 0 && !esVocal(w, i - 1);
        default:  return false;
    }
}

// m de la palabra escrita como [C](VC)^m[V].
int medida(const std::string& w) {
    const std::size_t n = w.size();
    std::size_t i = 0;
    int m = 0;
    while (i < n && !esVocal(w, i)) ++i;          // la C inicial no cuenta
    while (i < n) {
        while (i < n && esVocal(w, i)) ++i;       // V
        if (i >= n) break;                        // termina en V: VC incompleto
        ++m;
        while (i < n && !esVocal(w, i)) ++i;      // C
    }
    return m;
}

bool tieneVocal(const std::string& w) {
    for (std::size_t i = 0; i < w.size(); ++i) if (esVocal(w, i)) return true;
    return false;
}

bool dobleConsonanteFinal(const std::string& w) {
    const std::size_t n = w.size();
    return n >= 2 && w[n - 1] == w[n - 2] && !esVocal(w, n - 1);
}

// El radical acaba en consonante-vocal-consonante y la ultima no es w, x ni y.
// Es la forma de detectar silabas cortas, donde hay que reponer la 'e'
// ("hop" -> "hope") en vez de dejar el radical desnudo.
bool cvc(const std::string& w) {
    const std::size_t n = w.size();
    if (n < 3) return false;
    if (esVocal(w, n - 1) || !esVocal(w, n - 2) || esVocal(w, n - 3)) return false;
    const char c = w[n - 1];
    return c != 'w' && c != 'x' && c != 'y';
}

struct Regla { const char* desde; const char* hacia; int m_minimo; };

// Aplica la primera regla cuyo sufijo coincida. Devuelve true si cambio algo.
bool aplicar(std::string& w, const Regla* reglas, std::size_t n) {
    for (std::size_t i = 0; i < n; ++i) {
        const std::size_t largo = std::char_traits<char>::length(reglas[i].desde);
        if (!terminaEn(w, reglas[i].desde, largo)) continue;
        const std::string radical = w.substr(0, w.size() - largo);
        if (medida(radical) > reglas[i].m_minimo) w = radical + reglas[i].hacia;
        return true;   // el sufijo coincidio: no se prueban los siguientes
    }
    return false;
}

}  // namespace

std::string stemIngles(const std::string& palabra) {
    if (palabra.size() <= 2) return palabra;
    std::string w = palabra;

    // --- Paso 1a: plurales ---
    if      (terminaEn(w, "sses")) w.erase(w.size() - 2);
    else if (terminaEn(w, "ies"))  w.erase(w.size() - 2);
    else if (terminaEn(w, "ss"))   { /* se queda */ }
    else if (terminaEn(w, "s"))    w.erase(w.size() - 1);

    // --- Paso 1b: participios y gerundios ---
    bool paso1b_toco = false;
    if (terminaEn(w, "eed")) {
        if (medida(w.substr(0, w.size() - 3)) > 0) w.erase(w.size() - 1);
    } else if (terminaEn(w, "ed") && tieneVocal(w.substr(0, w.size() - 2))) {
        w.erase(w.size() - 2);
        paso1b_toco = true;
    } else if (terminaEn(w, "ing") && tieneVocal(w.substr(0, w.size() - 3))) {
        w.erase(w.size() - 3);
        paso1b_toco = true;
    }
    if (paso1b_toco) {
        if      (terminaEn(w, "at") || terminaEn(w, "bl") || terminaEn(w, "iz")) w += 'e';
        else if (dobleConsonanteFinal(w)) {
            const char c = w[w.size() - 1];
            if (c != 'l' && c != 's' && c != 'z') w.erase(w.size() - 1);
        } else if (medida(w) == 1 && cvc(w)) {
            w += 'e';
        }
    }

    // --- Paso 1c: la 'y' final se vuelve 'i' si hay vocal en el radical ---
    if (terminaEn(w, "y") && tieneVocal(w.substr(0, w.size() - 1))) w[w.size() - 1] = 'i';

    // --- Paso 2: sufijos dobles ---
    static const Regla paso2[] = {
        {"ational","ate",0}, {"tional","tion",0}, {"enci","ence",0}, {"anci","ance",0},
        {"izer","ize",0},    {"abli","able",0},   {"alli","al",0},   {"entli","ent",0},
        {"eli","e",0},       {"ousli","ous",0},   {"ization","ize",0}, {"ation","ate",0},
        {"ator","ate",0},    {"alism","al",0},    {"iveness","ive",0}, {"fulness","ful",0},
        {"ousness","ous",0}, {"aliti","al",0},    {"iviti","ive",0},   {"biliti","ble",0},
    };
    aplicar(w, paso2, sizeof(paso2) / sizeof(paso2[0]));

    // --- Paso 3 ---
    static const Regla paso3[] = {
        {"icate","ic",0}, {"ative","",0}, {"alize","al",0}, {"iciti","ic",0},
        {"ical","ic",0},  {"ful","",0},   {"ness","",0},
    };
    aplicar(w, paso3, sizeof(paso3) / sizeof(paso3[0]));

    // --- Paso 4: sufijos que se borran si el radical es largo (m > 1) ---
    static const Regla paso4[] = {
        {"al","",1}, {"ance","",1}, {"ence","",1}, {"er","",1}, {"ic","",1},
        {"able","",1}, {"ible","",1}, {"ant","",1}, {"ement","",1}, {"ment","",1},
        {"ent","",1}, {"ou","",1}, {"ism","",1}, {"ate","",1}, {"iti","",1},
        {"ous","",1}, {"ive","",1}, {"ize","",1},
    };
    // '-ion' solo se quita detras de s o t; va aparte porque la condicion no
    // es solo de medida.
    if (terminaEn(w, "ion")) {
        const std::string radical = w.substr(0, w.size() - 3);
        if (medida(radical) > 1 && !radical.empty() &&
            (radical[radical.size() - 1] == 's' || radical[radical.size() - 1] == 't'))
            w = radical;
    } else {
        aplicar(w, paso4, sizeof(paso4) / sizeof(paso4[0]));
    }

    // --- Paso 5a: la 'e' muda ---
    if (terminaEn(w, "e")) {
        const std::string radical = w.substr(0, w.size() - 1);
        const int m = medida(radical);
        if (m > 1 || (m == 1 && !cvc(radical))) w = radical;
    }
    // --- Paso 5b: '-ll' se reduce a '-l' si el radical es largo ---
    if (medida(w) > 1 && dobleConsonanteFinal(w) && w[w.size() - 1] == 'l')
        w.erase(w.size() - 1);

    return w;
}

// ===========================================================================
//  Stemmer espanol (Snowball, adaptado a texto sin tildes)
//
//  Snowball define tres regiones sobre la palabra y condiciona cada sufijo a
//  empezar dentro de una de ellas:
//     R1  despues de la primera consonante que sigue a una vocal
//     R2  lo mismo, pero buscando dentro de R1
//     RV  regla propia del espanol, pensada para que los sufijos verbales
//         cortos (-ar, -io, -an) no se coman la raiz de palabras breves
//
//  Diferencias conocidas respecto al Snowball oficial, consecuencia de que
//  aqui el texto llega sin tildes:
//     - los sufijos de las listas van sin acento, asi que formas que el
//       original distingue ("ando" vs "ándo") quedan fundidas en una
//     - el paso 3 NO borra la 'i' final: en Snowball solo se quita la 'i'
//       ACENTUADA, y sin tildes no hay forma de distinguir "corri" de "casi"
//  Lo que importa para recuperar informacion es que documentos y consultas
//  pasen exactamente por el mismo analisis, y eso se cumple.
// ===========================================================================
namespace {

bool vocalEs(char c) {
    return c == 'a' || c == 'e' || c == 'i' || c == 'o' || c == 'u';
}

std::size_t calcR1(const std::string& w) {
    for (std::size_t i = 1; i < w.size(); ++i)
        if (!vocalEs(w[i]) && vocalEs(w[i - 1])) return i + 1;
    return w.size();
}
std::size_t calcR2(const std::string& w, std::size_t r1) {
    for (std::size_t i = r1 + 1; i < w.size(); ++i)
        if (!vocalEs(w[i]) && vocalEs(w[i - 1])) return i + 1;
    return w.size();
}
std::size_t calcRV(const std::string& w) {
    const std::size_t n = w.size();
    if (n < 3) return n;
    if (!vocalEs(w[1])) {                       // consonante en segunda posicion
        std::size_t i = 2;
        while (i < n && !vocalEs(w[i])) ++i;    // hasta la siguiente vocal
        return (i < n) ? i + 1 : n;
    }
    if (vocalEs(w[0]) && vocalEs(w[1])) {       // dos vocales al principio
        std::size_t i = 2;
        while (i < n && vocalEs(w[i])) ++i;     // hasta la siguiente consonante
        return (i < n) ? i + 1 : n;
    }
    return 3;                                   // consonante + vocal
}

// ¿w termina en 'suf' y ese sufijo empieza dentro de la region?
bool sufijoEn(const std::string& w, const char* suf, std::size_t largo, std::size_t region) {
    return w.size() >= largo && w.size() - largo >= region &&
           w.compare(w.size() - largo, largo, suf, largo) == 0;
}
template <std::size_t N>
bool sufijoEn(const std::string& w, const char (&suf)[N], std::size_t region) {
    return sufijoEn(w, suf, N - 1, region);
}

// Si el sufijo esta en la region, lo borra y devuelve true.
template <std::size_t N>
bool quitarSi(std::string& w, const char (&suf)[N], std::size_t region) {
    if (!sufijoEn(w, suf, N - 1, region)) return false;
    w.erase(w.size() - (N - 1));
    return true;
}

struct SufijoEs { const char* texto; char grupo; };

// La 'u' de 'gu' se protege con una vocal muda: al quitar el sufijo hay que
// quitarla tambien ("sigues" -> "sigu" -> "sig").
void arreglarGu(std::string& w, std::size_t rv) {
    if (w.size() >= 2 && w.size() - 1 >= rv &&
        w[w.size() - 1] == 'u' && w[w.size() - 2] == 'g')
        w.erase(w.size() - 1);
}

}  // namespace

std::string stemEspanol(const std::string& palabra) {
    if (palabra.size() <= 2) return palabra;
    std::string w = palabra;

    const std::size_t r1 = calcR1(w);
    const std::size_t r2 = calcR2(w, r1);
    const std::size_t rv = calcRV(w);

    // --- Paso 0: pronombres enciticos ("damelo", "mirandolas") ---
    static const char* pronombres[] = {
        "selas", "selos", "sela", "selo", "las", "les", "los", "nos",
        "me", "se", "la", "le", "lo"
    };
    for (const char* p : pronombres) {
        const std::size_t lp = std::char_traits<char>::length(p);
        if (!sufijoEn(w, p, lp, rv)) continue;
        const std::string base = w.substr(0, w.size() - lp);
        static const char* gerundios[] = {"iendo", "ando", "ar", "er", "ir"};
        bool pegado = false;
        for (const char* g : gerundios) {
            const std::size_t lg = std::char_traits<char>::length(g);
            if (sufijoEn(base, g, lg, rv)) { pegado = true; break; }
        }
        if (!pegado && sufijoEn(base, "yendo", 5, rv) && base.size() >= 6 &&
            base[base.size() - 6] == 'u')
            pegado = true;
        if (pegado) w = base;
        break;   // solo el pronombre mas largo
    }

    // --- Paso 1: sufijos de derivacion, del mas largo al mas corto ---
    bool hecho = false;
    static const char* sufA[] = {
        "amientos", "imientos", "amiento", "imiento", "anzas", "ismos", "ables",
        "ibles", "istas", "anza", "icos", "icas", "ismo", "able", "ible", "ista",
        "osos", "osas", "ico", "ica", "oso", "osa"
    };
    static const char* sufB[] = {
        "aciones", "adoras", "adores", "ancias", "adora", "acion", "antes",
        "ancia", "ador", "ante"
    };

    // 'logia' y 'ucion' sustituyen en vez de borrar; van antes por longitud.
    if (!hecho && sufijoEn(w, "uciones", 7, r2)) { w.erase(w.size() - 7); w += 'u'; hecho = true; }
    if (!hecho && sufijoEn(w, "ucion",   5, r2)) { w.erase(w.size() - 5); w += 'u'; hecho = true; }
    if (!hecho && sufijoEn(w, "logias",  6, r2)) { w.erase(w.size() - 3); hecho = true; }
    if (!hecho && sufijoEn(w, "logia",   5, r2)) { w.erase(w.size() - 2); hecho = true; }
    if (!hecho && sufijoEn(w, "encias",  6, r2)) { w.erase(w.size() - 6); w += "ente"; hecho = true; }
    if (!hecho && sufijoEn(w, "encia",   5, r2)) { w.erase(w.size() - 5); w += "ente"; hecho = true; }

    if (!hecho && sufijoEn(w, "amente", 6, r1)) {
        w.erase(w.size() - 6);
        if (!quitarSi(w, "iv", r2)) {
            if (!quitarSi(w, "os", r2) && !quitarSi(w, "ic", r2)) quitarSi(w, "ad", r2);
        } else {
            quitarSi(w, "at", r2);
        }
        hecho = true;
    }
    if (!hecho && sufijoEn(w, "mente", 5, r2)) {
        w.erase(w.size() - 5);
        if (!quitarSi(w, "ante", r2) && !quitarSi(w, "able", r2)) quitarSi(w, "ible", r2);
        hecho = true;
    }
    if (!hecho && (sufijoEn(w, "idades", 6, r2) || sufijoEn(w, "idad", 4, r2))) {
        w.erase(w.size() - (terminaEn(w, "idades") ? 6 : 4));
        if (!quitarSi(w, "abil", r2) && !quitarSi(w, "ic", r2)) quitarSi(w, "iv", r2);
        hecho = true;
    }
    if (!hecho) {
        static const char* sufI[] = {"ivas", "ivos", "iva", "ivo"};
        for (const char* s : sufI) {
            const std::size_t l = std::char_traits<char>::length(s);
            if (sufijoEn(w, s, l, r2)) { w.erase(w.size() - l); quitarSi(w, "at", r2); hecho = true; break; }
        }
    }
    if (!hecho) {
        for (const char* s : sufB) {
            const std::size_t l = std::char_traits<char>::length(s);
            if (sufijoEn(w, s, l, r2)) { w.erase(w.size() - l); quitarSi(w, "ic", r2); hecho = true; break; }
        }
    }
    if (!hecho) {
        for (const char* s : sufA) {
            const std::size_t l = std::char_traits<char>::length(s);
            if (sufijoEn(w, s, l, r2)) { w.erase(w.size() - l); hecho = true; break; }
        }
    }

    // --- Pasos 2a y 2b: sufijos verbales, solo si el paso 1 no toco nada ---
    if (!hecho) {
        // 2a: formas con 'y' precedida de 'u' ("construyendo", "leyeron")
        static const char* sufY[] = {"yendo", "yeron", "yamos", "yais", "yan",
                                     "yen", "yas", "yes", "ya", "ye", "yo"};
        bool toco_y = false;
        for (const char* s : sufY) {
            const std::size_t l = std::char_traits<char>::length(s);
            if (!sufijoEn(w, s, l, rv)) continue;
            if (w.size() > l && w[w.size() - l - 1] == 'u') { w.erase(w.size() - l); toco_y = true; }
            break;
        }
        if (!toco_y) {
            // 2b: el resto de la conjugacion. Grupo 'a' arrastra la 'u' de 'gu'.
            static const SufijoEs sufV[] = {
                {"ariamos",'b'},{"eriamos",'b'},{"iriamos",'b'},{"ieramos",'b'},{"iesemos",'b'},
                {"abamos",'b'},{"aremos",'b'},{"eremos",'b'},{"iremos",'b'},{"asemos",'b'},
                {"ierais",'b'},{"ieseis",'b'},{"asteis",'b'},{"isteis",'b'},
                {"arian",'b'},{"erian",'b'},{"irian",'b'},{"arias",'b'},{"erias",'b'},{"irias",'b'},
                {"iamos",'b'},{"abais",'b'},{"arais",'b'},{"aseis",'b'},{"ieran",'b'},{"iesen",'b'},
                {"ieron",'b'},{"ieras",'b'},{"ieses",'b'},{"iendo",'b'},
                {"aria",'b'},{"eria",'b'},{"iria",'b'},{"aban",'b'},{"adas",'b'},{"idas",'b'},
                {"ados",'b'},{"idos",'b'},{"ando",'b'},{"aras",'b'},{"ases",'b'},{"iera",'b'},
                {"iese",'b'},{"aste",'b'},{"iste",'b'},{"amos",'b'},{"imos",'b'},{"aron",'b'},
                {"asen",'b'},{"aran",'b'},{"eran",'b'},{"iran",'b'},{"eras",'b'},{"iras",'b'},
                {"abas",'b'},{"emos",'a'},
                {"aba",'b'},{"ada",'b'},{"ida",'b'},{"ase",'b'},{"ado",'b'},{"ido",'b'},{"ara",'b'},
                {"ian",'b'},{"ias",'b'},{"ais",'b'},{"eis",'a'},
                {"ad",'b'},{"ed",'b'},{"id",'b'},{"an",'b'},{"as",'b'},{"ar",'b'},{"er",'b'},
                {"ir",'b'},{"ia",'b'},{"is",'b'},{"en",'a'},{"es",'a'},
            };
            for (const SufijoEs& s : sufV) {
                const std::size_t l = std::char_traits<char>::length(s.texto);
                if (!sufijoEn(w, s.texto, l, rv)) continue;
                w.erase(w.size() - l);
                if (s.grupo == 'a') arreglarGu(w, rv);
                break;
            }
        }
    }

    // --- Paso 3: vocal final residual ---
    if (!quitarSi(w, "os", rv) && !quitarSi(w, "a", rv) && !quitarSi(w, "o", rv)) {
        if (quitarSi(w, "e", rv)) arreglarGu(w, rv);
    }
    return w;
}

// ---------------------------------------------------------------------------
std::vector<std::string> tokenizar(const std::string& texto, Idioma i) {
    const std::string plano = normalizar(texto);
    std::vector<std::string> terminos;
    std::string palabra;
    palabra.reserve(32);

    auto cerrar = [&]() {
        if (palabra.size() >= LARGO_MINIMO && !esStopword(palabra, i)) {
            if (palabra.size() > LARGO_MAXIMO) palabra.resize(LARGO_MAXIMO);
            std::string t = stem(palabra, i);
            // El stemming puede dejar un radical mas corto que el minimo o que
            // resulte ser una stopword ("tiene" -> "tien" no, pero "era" si):
            // se vuelve a filtrar sobre el termino final.
            if (!t.empty() && !esStopword(t, i)) terminos.push_back(std::move(t));
        }
        palabra.clear();
    };

    for (char c : plano) {
        if (c == ' ') cerrar();
        else          palabra += c;
    }
    cerrar();
    return terminos;
}

}  // namespace texto
}  // namespace db
