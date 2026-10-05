#include "db/sql.hpp"

#include <cctype>
#include <cstdlib>

namespace db {

std::string engineName(EngineKind e) { return e == EngineKind::HEAP ? "HEAP" : "SEQUENTIAL"; }

EngineKind engineFromName(const std::string& s) {
    if (s == "HEAP")       return EngineKind::HEAP;
    if (s == "SEQUENTIAL") return EngineKind::SEQUENTIAL;
    throw DBException("Motor de almacenamiento desconocido: " + s);
}

// ---------------------------------------------------------------------------
//  Lexer
// ---------------------------------------------------------------------------
namespace {

enum class Tok { END, IDENT, NUMBER, STRING, SYMBOL };

struct Token {
    Tok         tipo = Tok::END;
    std::string texto;      // IDENT en MAYUSCULAS; STRING sin comillas
    std::string crudo;      // texto original (para identificadores)
    bool        decimal = false;
};

std::string upper(const std::string& s) {
    std::string o = s;
    for (char& c : o) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return o;
}

std::vector<Token> tokenize(const std::string& sql) {
    std::vector<Token> ts;
    std::size_t i = 0;
    while (i < sql.size()) {
        char c = sql[i];
        if (std::isspace(static_cast<unsigned char>(c))) { ++i; continue; }
        if (c == '-' && i + 1 < sql.size() && sql[i + 1] == '-') {         // comentario
            while (i < sql.size() && sql[i] != '\n') ++i;
            continue;
        }
        if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
            std::size_t j = i;
            while (j < sql.size() && (std::isalnum(static_cast<unsigned char>(sql[j])) || sql[j] == '_')) ++j;
            Token t; t.tipo = Tok::IDENT; t.crudo = sql.substr(i, j - i); t.texto = upper(t.crudo);
            ts.push_back(t);
            i = j;
            continue;
        }
        if (std::isdigit(static_cast<unsigned char>(c)) ||
            (c == '-' && i + 1 < sql.size() && std::isdigit(static_cast<unsigned char>(sql[i + 1])))) {
            std::size_t j = i + 1;
            bool dec = false;
            while (j < sql.size() && (std::isdigit(static_cast<unsigned char>(sql[j])) || sql[j] == '.')) {
                if (sql[j] == '.') dec = true;
                ++j;
            }
            Token t; t.tipo = Tok::NUMBER; t.texto = sql.substr(i, j - i); t.decimal = dec;
            ts.push_back(t);
            i = j;
            continue;
        }
        if (c == '\'' || c == '"') {
            char comilla = c;
            std::size_t j = i + 1;
            std::string val;
            while (j < sql.size() && sql[j] != comilla) { val += sql[j]; ++j; }
            if (j >= sql.size()) throw DBException("Cadena sin cerrar en la consulta");
            Token t; t.tipo = Tok::STRING; t.texto = val;
            ts.push_back(t);
            i = j + 1;
            continue;
        }
        // simbolos, incluyendo los de dos y tres caracteres
        std::string sym(1, c);
        // '<->' (distancia) se prueba ANTES que '<=', porque comparten prefijo.
        if (c == '<' && i + 2 < sql.size() && sql[i + 1] == '-' && sql[i + 2] == '>') {
            sym = "<->"; i += 2;
        } else if ((c == '>' || c == '<' || c == '!') && i + 1 < sql.size() && sql[i + 1] == '=') {
            sym += '='; ++i;
        }
        Token t; t.tipo = Tok::SYMBOL; t.texto = sym;
        ts.push_back(t);
        ++i;
    }
    ts.push_back(Token{});
    return ts;
}

// ---------------------------------------------------------------------------
//  Parser de descenso recursivo
// ---------------------------------------------------------------------------
class Parser {
public:
    explicit Parser(std::vector<Token> ts) : ts_(std::move(ts)) {}

    Statement parse() {                       //una sola sentencia
        Statement st = parseOne();
        if (cur().tipo != Tok::END)
            throw DBException("Texto sobrante tras el final de la sentencia: '" + cur().texto + "'");
        return st;
    }

    std::vector<Statement> parseAll() {       //varias sentencias separadas por ';'
        std::vector<Statement> out;
        while (cur().tipo != Tok::END) out.push_back(parseOne());
        if (out.empty()) throw DBException("Consulta vacia");
        return out;
    }

private:
    Statement parseOne() {
        Statement st;
        std::string k = esperaIdent();
        if (k == "CREATE") {
            std::string que = esperaIdent();
            if (que == "TABLE")      return parseCreateTable();
            if (que == "INDEX")      return parseCreateIndex();
            throw DBException("Se esperaba TABLE o INDEX despues de CREATE, se encontro '" + que + "'");
        }
        if (k == "INSERT") return parseInsert();
        if (k == "SELECT") return parseSelect();
        if (k == "DELETE") return parseDelete();
        throw DBException("Sentencia no soportada: '" + k +
                          "'. Se admiten CREATE TABLE, CREATE INDEX, INSERT, SELECT y DELETE.");
    }
    const Token& cur() const { return ts_[p_]; }
    void avanza() { if (p_ + 1 < ts_.size()) ++p_; }

    bool esSimbolo(const std::string& s) const { return cur().tipo == Tok::SYMBOL && cur().texto == s; }
    bool esPalabra(const std::string& s) const { return cur().tipo == Tok::IDENT && cur().texto == s; }

    void esperaSimbolo(const std::string& s) {
        if (!esSimbolo(s)) throw DBException("Se esperaba '" + s + "' y se encontro '" + cur().texto + "'");
        avanza();
    }
    std::string esperaIdent() {
        if (cur().tipo != Tok::IDENT)
            throw DBException("Se esperaba un identificador y se encontro '" + cur().texto + "'");
        std::string s = cur().texto;
        avanza();
        return s;
    }
    std::string esperaNombre() {          // identificador conservando mayus/minus
        if (cur().tipo != Tok::IDENT)
            throw DBException("Se esperaba un nombre y se encontro '" + cur().texto + "'");
        std::string s = cur().crudo;
        avanza();
        return s;
    }
    void esperaPalabra(const std::string& s) {
        if (!esPalabra(s)) throw DBException("Se esperaba " + s + " y se encontro '" + cur().texto + "'");
        avanza();
    }
    int esperaEntero() {
        if (cur().tipo != Tok::NUMBER)
            throw DBException("Se esperaba un numero y se encontro '" + cur().texto + "'");
        int v = std::atoi(cur().texto.c_str());
        avanza();
        return v;
    }
    // Numero con signo, entero o decimal. Lo usan POINT(...) y WITHIN(...).
    double esperaDouble() {
        if (cur().tipo != Tok::NUMBER)
            throw DBException("Se esperaba un numero y se encontro '" + cur().texto + "'");
        double v = std::atof(cur().texto.c_str());
        avanza();
        return v;
    }

    // POLYGON((x1,y1),(x2,y2),...). Anillo cerrado; no hace falta repetir el
    // primer vertice al final.
    Value esperaPoligono() {
        esperaPalabra("POLYGON");
        esperaSimbolo("(");
        std::vector<double> vs;
        while (true) {
            esperaSimbolo("(");
            vs.push_back(esperaDouble());
            esperaSimbolo(",");
            vs.push_back(esperaDouble());
            esperaSimbolo(")");
            if (esSimbolo(",")) { avanza(); continue; }
            break;
        }
        esperaSimbolo(")");
        if (vs.size() < 6)
            throw DBException("Un POLYGON necesita al menos tres vertices; se vieron " +
                              std::to_string(vs.size() / 2) + ".");
        return Value::makePolygon(std::move(vs));
    }

    // POINT(x, y). Es el unico literal compuesto del lenguaje.
    Value esperaPunto() {
        esperaPalabra("POINT");
        esperaSimbolo("(");
        double x = esperaDouble();
        esperaSimbolo(",");
        double y = esperaDouble();
        esperaSimbolo(")");
        return Value::makePoint(x, y);
    }

    Value esperaLiteral() {
        if (esPalabra("POINT"))   return esperaPunto();
        if (esPalabra("POLYGON")) return esperaPoligono();
        if (cur().tipo == Tok::NUMBER) {
            Value v = cur().decimal ? Value::makeDouble(std::atof(cur().texto.c_str()))
                                    : Value::makeInt(std::atoll(cur().texto.c_str()));
            avanza();
            return v;
        }
        if (cur().tipo == Tok::STRING) {
            Value v = Value::makeStr(cur().texto);
            avanza();
            return v;
        }
        throw DBException("Se esperaba un valor literal y se encontro '" + cur().texto + "'");
    }

    Type parseTipo(int* len) {
        std::string t = esperaIdent();
        *len = 0;
        if (t == "INT" || t == "INTEGER" || t == "BIGINT") return Type::INT;
        if (t == "FLOAT" || t == "DOUBLE" || t == "REAL")  return Type::DOUBLE;
        if (t == "CHAR" || t == "VARCHAR" || t == "TEXT") {
            if (esSimbolo("(")) { avanza(); *len = esperaEntero(); esperaSimbolo(")"); }
            if (*len <= 0) *len = 64;
            return Type::VARCHAR;
        }
        if (t == "POINT" || t == "GEOPOINT") return Type::POINT;
        if (t == "POLYGON" || t == "GEOMETRY") return Type::POLYGON;
        throw DBException("Tipo no soportado: '" + t +
                          "'. Use INT, FLOAT/DOUBLE, CHAR(n)/VARCHAR(n), POINT o POLYGON.");
    }

    Statement parseCreateTable() {
        Statement st;
        st.kind  = StmtKind::CREATE_TABLE;
        st.table = esperaNombre();
        esperaSimbolo("(");
        while (true) {
            ColumnDef c;
            c.name = esperaNombre();
            c.type = parseTipo(&c.max_len);
            while (cur().tipo == Tok::IDENT) {                 // PRIMARY KEY, NOT NULL...
                if (esPalabra("PRIMARY")) { avanza(); if (esPalabra("KEY")) avanza(); c.primary_key = true; }
                else if (esPalabra("NOT")) { avanza(); if (esPalabra("NULL")) avanza(); }
                else if (esPalabra("KEY")) { avanza(); c.primary_key = true; }
                else break;
            }
            st.columns.push_back(c);
            if (esSimbolo(",")) { avanza(); continue; }
            break;
        }
        esperaSimbolo(")");
        if (esPalabra("USING")) { avanza(); st.engine = engineFromName(esperaIdent()); }
        finSentencia();
        if (st.columns.empty()) throw DBException("CREATE TABLE sin columnas");
        return st;
    }

    Statement parseCreateIndex() {
        Statement st;
        st.kind       = StmtKind::CREATE_INDEX;
        st.index_name = esperaNombre();
        esperaPalabra("ON");
        st.table = esperaNombre();
        esperaSimbolo("(");
        st.index_column = esperaNombre();
        esperaSimbolo(")");
        if (esPalabra("USING")) {
            avanza();
            std::string k = esperaIdent();
            if (k == "BTREE" || k == "BPLUS" || k == "BTREE+") st.index_kind = IndexKind::BPLUS;
            else if (k == "HASH")                              st.index_kind = IndexKind::HASH;
            else if (k == "RTREE" || k == "RTREE2D" || k == "GIST") st.index_kind = IndexKind::RTREE;
            else throw DBException("Tipo de indice no soportado: '" + k +
                                   "'. Use BTREE, HASH o RTREE.");
        }
        finSentencia();
        return st;
    }

Statement parseInsert() {
    Statement st;
    st.kind = StmtKind::INSERT;
    esperaPalabra("INTO");
    st.table = esperaNombre();
    if (esSimbolo("(")) {                       // lista de columnas: se acepta y se ignora
        avanza();
        while (!esSimbolo(")")) { avanza(); if (cur().tipo == Tok::END) throw DBException("INSERT mal formado"); }
        avanza();
    }
    esperaPalabra("VALUES");
    while (true) {
        esperaSimbolo("(");
        std::vector<Value> fila;
        while (true) {
            fila.push_back(esperaLiteral());
            if (esSimbolo(",")) { avanza(); continue; }
            break;
        }
        esperaSimbolo(")");
        st.rows.push_back(std::move(fila));
        if (esSimbolo(",")) { avanza(); continue; }   // viene otra tupla: (...), (...)
        break;
    }
    finSentencia();
    return st;
}

    Statement parseSelect() {
        Statement st;
        st.kind = StmtKind::SELECT;
        if (esSimbolo("*")) { avanza(); }
        else {
            while (true) {
                st.select_columns.push_back(esperaNombre());
                if (esSimbolo(",")) { avanza(); continue; }
                break;
            }
        }
        esperaPalabra("FROM");
        st.table = esperaNombre();
        if (esPalabra("WHERE")) { avanza(); st.where = parseWhere(&st.extra); }
        if (esPalabra("ORDER")) {
            avanza();
            esperaPalabra("BY");
            if (esPalabra("ST_DISTANCE")) {
                // Orden por distancia geografica real (metros).
                esperaStDistance(&st.knn_column, &st.knn_x, &st.knn_y);
                st.knn     = true;
                st.knn_geo = true;
            } else {
                st.knn_column = esperaNombre();
                if (!esSimbolo("<->"))
                    throw DBException(
                        "Solo se admite ordenar por distancia: ORDER BY " + st.knn_column +
                        " <-> POINT(x, y), o bien ORDER BY ST_DISTANCE(" + st.knn_column +
                        ", POINT(lon, lat)). Se encontro '" + cur().texto + "'.");
                avanza();
                Value p = esperaPunto();
                st.knn   = true;
                st.knn_x = p.d;
                st.knn_y = p.y;
            }
        }
        if (esPalabra("LIMIT")) { avanza(); st.limit = esperaEntero(); }
        finSentencia();
        return st;
    }

    Statement parseDelete() {
        Statement st;
        st.kind = StmtKind::DELETE_;
        esperaPalabra("FROM");
        st.table = esperaNombre();
        if (esPalabra("WHERE")) { avanza(); st.where = parseWhere(&st.extra); }
        finSentencia();
        if (st.where.kind == PredKind::NONE)
            throw DBException("DELETE sin WHERE no esta permitido (borraria la tabla entera)");
        return st;
    }

    // Una sola condicion:
    //    col = v | col BETWEEN a AND b | col WITHIN (...) | col <op> v
    // ST_DISTANCE(col, POINT(lon, lat)) -> deja en 'col' la columna y en
    // (*lon,*lat) el punto de referencia.
    void esperaStDistance(std::string* col, double* lon, double* lat) {
        esperaPalabra("ST_DISTANCE");
        esperaSimbolo("(");
        *col = esperaNombre();
        esperaSimbolo(",");
        Value p = esperaPunto();
        esperaSimbolo(")");
        *lon = p.d; *lat = p.y;
    }

    Predicate parseCondicion() {
        Predicate pr;

        // Contencion: ST_CONTAINS(col, POINT(x,y))
        if (esPalabra("ST_CONTAINS")) {
            avanza();
            esperaSimbolo("(");
            pr.column = esperaNombre();
            esperaSimbolo(",");
            Value q = esperaPunto();
            esperaSimbolo(")");
            pr.qlon = q.d;
            pr.qlat = q.y;
            pr.kind = PredKind::CONTIENE;
            return pr;
        }

        // Radio geografico: ST_DISTANCE(col, POINT(lon,lat)) <= metros
        if (esPalabra("ST_DISTANCE")) {
            esperaStDistance(&pr.column, &pr.qlon, &pr.qlat);
            if (cur().tipo != Tok::SYMBOL ||
                (cur().texto != "<" && cur().texto != "<="))
                throw DBException(
                    "ST_DISTANCE solo se compara con < o <= en el WHERE: "
                    "ST_DISTANCE(" + pr.column + ", POINT(lon,lat)) <= metros. "
                    "Se encontro '" + cur().texto + "'.");
            pr.radio_estricto = (cur().texto == "<");
            avanza();
            pr.metros = esperaDouble();
            pr.kind   = PredKind::RADIO;
            return pr;
        }

        pr.column = esperaNombre();

        if (esPalabra("BETWEEN")) {
            avanza();
            pr.kind = PredKind::RANGE;
            pr.lo   = esperaLiteral();
            esperaPalabra("AND");          // este AND pertenece al BETWEEN
            pr.hi = esperaLiteral();
            return pr;
        }

        // Ventana espacial: col WITHIN (minx, miny, maxx, maxy)
        if (esPalabra("WITHIN")) {
            avanza();
            esperaSimbolo("(");
            pr.wx0 = esperaDouble(); esperaSimbolo(",");
            pr.wy0 = esperaDouble(); esperaSimbolo(",");
            pr.wx1 = esperaDouble(); esperaSimbolo(",");
            pr.wy1 = esperaDouble();
            esperaSimbolo(")");
            pr.kind = PredKind::WITHIN;
            return pr;
        }

        if (cur().tipo != Tok::SYMBOL)
            throw DBException("Se esperaba un operador de comparacion tras '" + pr.column + "'");
        std::string op = cur().texto;
        avanza();
        Value v = esperaLiteral();

        if (op == "=") {
            pr.kind = PredKind::EQ;
            pr.eq   = v;
        } else if (op == ">" || op == ">=") {
            pr.kind = PredKind::RANGE;
            pr.lo = v; pr.hi_abierto = true;
            pr.lo_estricto = (op == ">");
        } else if (op == "<" || op == "<=") {
            pr.kind = PredKind::RANGE;
            pr.hi = v; pr.lo_abierto = true;
            pr.hi_estricto = (op == "<");
        } else {
            throw DBException("Operador no soportado: '" + op + "'. Use =, <, <=, > o >=.");
        }
        return pr;
    }

    // Lista de condiciones unidas por AND. Las de la MISMA columna que sean
    // rangos se fusionan en uno solo --'id >= 100 AND id <= 500' tiene que
    // seguir llegando al B+ como un rango cerrado, no como dos filtros-- y las
    // de columnas distintas quedan como entradas separadas de la lista.
    Predicate parseWhere(std::vector<Predicate>* extra) {
        std::vector<Predicate> todas;
        while (true) {
            Predicate p = parseCondicion();

            bool fusionada = false;
            if (p.kind == PredKind::RANGE) {
                for (Predicate& q : todas) {
                    if (q.column != p.column || q.kind != PredKind::RANGE) continue;
                    if (!p.lo_abierto) { q.lo = p.lo; q.lo_abierto = false; q.lo_estricto = p.lo_estricto; }
                    if (!p.hi_abierto) { q.hi = p.hi; q.hi_abierto = false; q.hi_estricto = p.hi_estricto; }
                    fusionada = true;
                    break;
                }
            }
            if (!fusionada) todas.push_back(p);

            if (esPalabra("AND")) { avanza(); continue; }
            break;
        }

        extra->clear();
        for (std::size_t i = 1; i < todas.size(); ++i) extra->push_back(todas[i]);
        return todas.front();
    }

    void finSentencia() {
        if (esSimbolo(";")) avanza();
    }

    std::vector<Token> ts_;
    std::size_t        p_ = 0;
};

}  // namespace

Statement parseSQL(const std::string& sql) {
    std::vector<Token> ts = tokenize(sql);
    if (ts.size() <= 1) throw DBException("Consulta vacia");
    Parser p(std::move(ts));
    return p.parse();
}

std::vector<Statement> parseSQLMultiple(const std::string& sql) {
    std::vector<Token> ts = tokenize(sql);
    if (ts.size() <= 1) throw DBException("Consulta vacia");
    Parser p(std::move(ts));
    return p.parseAll();
}
}  // namespace db
