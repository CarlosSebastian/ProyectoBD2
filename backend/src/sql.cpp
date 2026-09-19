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
        // simbolos, incluyendo los de dos caracteres
        std::string sym(1, c);
        if ((c == '>' || c == '<' || c == '!') && i + 1 < sql.size() && sql[i + 1] == '=') {
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

    Statement parse() {
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

private:
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
    Value esperaLiteral() {
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
        throw DBException("Tipo no soportado: '" + t + "'. Use INT, FLOAT/DOUBLE o CHAR(n)/VARCHAR(n).");
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
            else throw DBException("Tipo de indice no soportado: '" + k + "'. Use BTREE o HASH.");
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
        esperaSimbolo("(");
        while (true) {
            st.values.push_back(esperaLiteral());
            if (esSimbolo(",")) { avanza(); continue; }
            break;
        }
        esperaSimbolo(")");
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
        if (esPalabra("WHERE")) { avanza(); st.where = parseWhere(); }
        if (esPalabra("LIMIT")) { avanza(); st.limit = esperaEntero(); }
        finSentencia();
        return st;
    }

    Statement parseDelete() {
        Statement st;
        st.kind = StmtKind::DELETE_;
        esperaPalabra("FROM");
        st.table = esperaNombre();
        if (esPalabra("WHERE")) { avanza(); st.where = parseWhere(); }
        finSentencia();
        if (st.where.kind == PredKind::NONE)
            throw DBException("DELETE sin WHERE no esta permitido (borraria la tabla entera)");
        return st;
    }

    // col = v | col BETWEEN a AND b | col <op> v [AND col <op> v]
    Predicate parseWhere() {
        Predicate pr;
        pr.column = esperaNombre();

        if (esPalabra("BETWEEN")) {
            avanza();
            pr.kind = PredKind::RANGE;
            pr.lo   = esperaLiteral();
            esperaPalabra("AND");
            pr.hi = esperaLiteral();
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

        // segunda cota opcional: ... AND col <op> v
        if (esPalabra("AND")) {
            avanza();
            std::string col2 = esperaNombre();
            if (col2 != pr.column)
                throw DBException("Solo se admiten predicados sobre una columna; se vio '" +
                                  pr.column + "' y '" + col2 + "'");
            if (cur().tipo != Tok::SYMBOL) throw DBException("Se esperaba un operador tras AND");
            std::string op2 = cur().texto;
            avanza();
            Value v2 = esperaLiteral();
            if (op2 == ">" || op2 == ">=")      { pr.lo = v2; pr.lo_abierto = false;
                                                  pr.lo_estricto = (op2 == ">"); }
            else if (op2 == "<" || op2 == "<=") { pr.hi = v2; pr.hi_abierto = false;
                                                  pr.hi_estricto = (op2 == "<"); }
            else throw DBException("Operador no soportado tras AND: '" + op2 + "'");
        }
        return pr;
    }

    void finSentencia() {
        if (esSimbolo(";")) avanza();
        if (cur().tipo != Tok::END)
            throw DBException("Texto sobrante tras el final de la sentencia: '" + cur().texto + "'");
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

}  // namespace db
