#include "sql/parser.h"

#include <limits>
#include <stdexcept>

#include "sql/lexer.h"

namespace liteolap::sql {

bool SelectStmt::HasAggregates() const {
    for (const auto& it : items) {
        if (dynamic_cast<const AggregateExpr*>(it.expr.get())) return true;
    }
    return false;
}

namespace {

class Parser {
   public:
    explicit Parser(std::vector<Token> tokens) : tokens_(std::move(tokens)) {}

    std::unique_ptr<Statement> ParseStatement() {
        const Token& t = Peek();
        if (t.kind != TokenKind::kKeyword) Fail("expected statement");
        if (t.text == "SELECT") return ParseSelect();
        if (t.text == "INSERT") return ParseInsert();
        if (t.text == "CREATE") return ParseCreateTable();
        if (t.text == "DROP") return ParseDrop();
        if (t.text == "TRUNCATE") return ParseTruncate();
        Fail("unsupported statement: " + t.text);
    }

   private:
    std::vector<Token> tokens_;
    std::size_t pos_{0};

    [[noreturn]] void Fail(const std::string& m) { throw std::runtime_error("parser: " + m); }
    const Token& Peek() const { return tokens_[pos_]; }
    Token Consume() { return tokens_[pos_++]; }

    bool MatchKw(const std::string& kw) {
        if (Peek().kind == TokenKind::kKeyword && Peek().text == kw) {
            ++pos_;
            return true;
        }
        return false;
    }
    void ExpectKw(const std::string& kw) {
        if (!MatchKw(kw)) Fail("expected keyword " + kw + ", got '" + Peek().text + "'");
    }
    bool MatchPunct(const std::string& p) {
        if (Peek().kind == TokenKind::kPunct && Peek().text == p) {
            ++pos_;
            return true;
        }
        return false;
    }
    void ExpectPunct(const std::string& p) {
        if (!MatchPunct(p)) Fail("expected '" + p + "', got '" + Peek().text + "'");
    }
    std::string ExpectIdent() {
        if (Peek().kind != TokenKind::kIdentifier) Fail("expected identifier, got '" + Peek().text + "'");
        return Consume().text;
    }
    void ExpectEnd() {
        MatchPunct(";");
        if (Peek().kind != TokenKind::kEof) Fail("trailing tokens after statement");
    }

    Value ParseLiteral() {
        const Token& t = Peek();
        if (t.kind == TokenKind::kInteger) {
            auto v = t.int_value;
            ++pos_;
            if (v >= std::numeric_limits<std::int32_t>::min() &&
                v <= std::numeric_limits<std::int32_t>::max()) {
                return Value{static_cast<std::int32_t>(v)};
            }
            return Value{static_cast<std::int64_t>(v)};
        }
        if (t.kind == TokenKind::kFloat) {
            auto v = t.float_value;
            ++pos_;
            return Value{v};
        }
        if (t.kind == TokenKind::kString) {
            std::string s = t.text;
            ++pos_;
            return Value{std::move(s)};
        }
        if (t.kind == TokenKind::kKeyword && t.text == "NULL") {
            ++pos_;
            return Value{Null{}};
        }
        Fail("expected literal, got '" + t.text + "'");
    }

    // --- expressions -------------------------------------------------------
    std::unique_ptr<Expr> ParseExpr() { return ParseOr(); }

    std::unique_ptr<Expr> ParseOr() {
        auto l = ParseAnd();
        while (MatchKw("OR")) {
            auto b = std::make_unique<BinaryExpr>();
            b->op = BinOp::kOr;
            b->left = std::move(l);
            b->right = ParseAnd();
            l = std::move(b);
        }
        return l;
    }
    std::unique_ptr<Expr> ParseAnd() {
        auto l = ParseNot();
        while (MatchKw("AND")) {
            auto b = std::make_unique<BinaryExpr>();
            b->op = BinOp::kAnd;
            b->left = std::move(l);
            b->right = ParseNot();
            l = std::move(b);
        }
        return l;
    }
    std::unique_ptr<Expr> ParseNot() {
        if (MatchKw("NOT")) {
            auto e = std::make_unique<NotExpr>();
            e->child = ParseNot();
            return e;
        }
        return ParseComparison();
    }
    std::unique_ptr<Expr> ParseComparison() {
        auto l = ParseAddSub();
        const Token& t = Peek();
        if (t.kind == TokenKind::kPunct) {
            BinOp op;
            if (t.text == "=") op = BinOp::kEq;
            else if (t.text == "!=") op = BinOp::kNe;
            else if (t.text == "<") op = BinOp::kLt;
            else if (t.text == "<=") op = BinOp::kLe;
            else if (t.text == ">") op = BinOp::kGt;
            else if (t.text == ">=") op = BinOp::kGe;
            else return l;
            ++pos_;
            auto b = std::make_unique<BinaryExpr>();
            b->op = op;
            b->left = std::move(l);
            b->right = ParseAddSub();
            return b;
        }
        if (t.kind == TokenKind::kKeyword && t.text == "BETWEEN") {
            ++pos_;
            auto lo = ParseAddSub();
            ExpectKw("AND");
            auto hi = ParseAddSub();
            // Desugar to (l >= lo AND l <= hi). `l` is duplicated by cloning
            // its textual form is unnecessary: BETWEEN operands are columns,
            // so we re-parse is impossible; instead share via two BinaryExprs
            // referencing copies. We require `l` to be a column ref.
            auto* col = dynamic_cast<ColumnRefExpr*>(l.get());
            if (!col) Fail("BETWEEN requires a column on the left");
            auto mk_col = [&]() {
                auto c = std::make_unique<ColumnRefExpr>();
                c->table_alias = col->table_alias;
                c->column_name = col->column_name;
                return c;
            };
            auto ge = std::make_unique<BinaryExpr>();
            ge->op = BinOp::kGe;
            ge->left = mk_col();
            ge->right = std::move(lo);
            auto le = std::make_unique<BinaryExpr>();
            le->op = BinOp::kLe;
            le->left = mk_col();
            le->right = std::move(hi);
            auto both = std::make_unique<BinaryExpr>();
            both->op = BinOp::kAnd;
            both->left = std::move(ge);
            both->right = std::move(le);
            return both;
        }
        if (t.kind == TokenKind::kKeyword && t.text == "IN") {
            ++pos_;
            ExpectPunct("(");
            auto in = std::make_unique<InExpr>();
            in->column = std::move(l);
            in->values.push_back(ParseLiteral());
            while (MatchPunct(",")) in->values.push_back(ParseLiteral());
            ExpectPunct(")");
            return in;
        }
        return l;
    }
    // Arithmetic, lower precedence: + and -.
    std::unique_ptr<Expr> ParseAddSub() {
        auto l = ParseMulDiv();
        while (Peek().kind == TokenKind::kPunct && (Peek().text == "+" || Peek().text == "-")) {
            const BinOp op = Peek().text == "+" ? BinOp::kAdd : BinOp::kSub;
            ++pos_;
            auto b = std::make_unique<BinaryExpr>();
            b->op = op;
            b->left = std::move(l);
            b->right = ParseMulDiv();
            l = std::move(b);
        }
        return l;
    }
    // Arithmetic, higher precedence: * and /.
    std::unique_ptr<Expr> ParseMulDiv() {
        auto l = ParsePrimary();
        while (Peek().kind == TokenKind::kPunct && (Peek().text == "*" || Peek().text == "/")) {
            const BinOp op = Peek().text == "*" ? BinOp::kMul : BinOp::kDiv;
            ++pos_;
            auto b = std::make_unique<BinaryExpr>();
            b->op = op;
            b->left = std::move(l);
            b->right = ParsePrimary();
            l = std::move(b);
        }
        return l;
    }
    std::unique_ptr<Expr> ParsePrimary() {
        if (MatchPunct("(")) {
            auto e = ParseExpr();
            ExpectPunct(")");
            return e;
        }
        if (PeekAggregate()) return ParseAggregate();  // e.g. inside HAVING
        const Token& t = Peek();
        if (t.kind == TokenKind::kInteger || t.kind == TokenKind::kFloat ||
            t.kind == TokenKind::kString ||
            (t.kind == TokenKind::kKeyword && t.text == "NULL")) {
            auto lit = std::make_unique<LiteralExpr>();
            lit->value = ParseLiteral();
            return lit;
        }
        if (t.kind == TokenKind::kIdentifier) {
            auto ref = std::make_unique<ColumnRefExpr>();
            std::string first = Consume().text;
            if (MatchPunct(".")) {
                ref->table_alias = std::move(first);
                ref->column_name = ExpectIdent();
            } else {
                ref->column_name = std::move(first);
            }
            return ref;
        }
        Fail("expected expression, got '" + t.text + "'");
    }

    // --- aggregate / select item ------------------------------------------
    bool PeekAggregate() const {
        const Token& t = Peek();
        return t.kind == TokenKind::kKeyword &&
               (t.text == "COUNT" || t.text == "SUM" || t.text == "AVG" || t.text == "MIN" ||
                t.text == "MAX");
    }

    std::unique_ptr<Expr> ParseAggregate() {
        const std::string fn = Consume().text;
        ExpectPunct("(");
        auto agg = std::make_unique<AggregateExpr>();
        if (fn == "COUNT") {
            if (MatchPunct("*")) {
                agg->kind = AggKind::kCountStar;
            } else {
                agg->kind = AggKind::kCount;
                agg->argument = ParseAddSub();
            }
        } else {
            if (fn == "SUM") agg->kind = AggKind::kSum;
            else if (fn == "AVG") agg->kind = AggKind::kAvg;
            else if (fn == "MIN") agg->kind = AggKind::kMin;
            else agg->kind = AggKind::kMax;
            agg->argument = ParseAddSub();
        }
        ExpectPunct(")");
        return agg;
    }

    SelectItem ParseSelectItem() {
        SelectItem item;
        if (PeekAggregate()) {
            item.expr = ParseAggregate();
        } else {
            item.expr = ParsePrimary();  // column ref
        }
        if (MatchKw("AS")) {
            item.output_alias = ExpectIdent();
        }
        return item;
    }

    // --- statements --------------------------------------------------------
    std::unique_ptr<Statement> ParseSelect() {
        ExpectKw("SELECT");
        auto s = std::make_unique<SelectStmt>();
        if (MatchPunct("*")) {
            s->select_star = true;
        } else {
            s->items.push_back(ParseSelectItem());
            while (MatchPunct(",")) s->items.push_back(ParseSelectItem());
        }
        ExpectKw("FROM");
        s->tables.push_back(ParseTableRef());
        while (MatchPunct(",")) s->tables.push_back(ParseTableRef());

        if (MatchKw("WHERE")) s->where = ParseExpr();

        if (MatchKw("GROUP")) {
            ExpectKw("BY");
            s->group_by.push_back(ParseColumnRef());
            while (MatchPunct(",")) s->group_by.push_back(ParseColumnRef());
        }
        if (MatchKw("HAVING")) s->having = ParseExpr();

        if (MatchKw("ORDER")) {
            ExpectKw("BY");
            OrderBy ob;
            ob.output_name = ExpectIdent();
            // qualified order key (alias.col) collapses to the column name.
            if (MatchPunct(".")) ob.output_name = ExpectIdent();
            if (MatchKw("DESC")) ob.descending = true;
            else MatchKw("ASC");
            s->order_by = std::move(ob);
        }
        if (MatchKw("LIMIT")) {
            if (Peek().kind != TokenKind::kInteger || Peek().int_value < 0) {
                Fail("LIMIT must be a non-negative integer");
            }
            s->limit = static_cast<std::size_t>(Consume().int_value);
        }
        ExpectEnd();
        return s;
    }

    std::unique_ptr<ColumnRefExpr> ParseColumnRef() {
        auto ref = std::make_unique<ColumnRefExpr>();
        std::string first = ExpectIdent();
        if (MatchPunct(".")) {
            ref->table_alias = std::move(first);
            ref->column_name = ExpectIdent();
        } else {
            ref->column_name = std::move(first);
        }
        return ref;
    }

    TableRef ParseTableRef() {
        TableRef tr;
        tr.name = ExpectIdent();
        if (Peek().kind == TokenKind::kKeyword && Peek().text == "AS") {
            ++pos_;
            tr.alias = ExpectIdent();
        } else if (Peek().kind == TokenKind::kIdentifier) {
            tr.alias = ExpectIdent();
        } else {
            tr.alias = tr.name;
        }
        return tr;
    }

    std::unique_ptr<Statement> ParseInsert() {
        ExpectKw("INSERT");
        ExpectKw("INTO");
        auto s = std::make_unique<InsertStmt>();
        s->table_name = ExpectIdent();
        ExpectKw("VALUES");
        do {
            ExpectPunct("(");
            std::vector<Value> row;
            row.push_back(ParseLiteral());
            while (MatchPunct(",")) row.push_back(ParseLiteral());
            ExpectPunct(")");
            s->rows.push_back(std::move(row));
        } while (MatchPunct(","));
        ExpectEnd();
        return s;
    }

    std::unique_ptr<Statement> ParseCreateTable() {
        ExpectKw("CREATE");
        ExpectKw("TABLE");
        auto s = std::make_unique<CreateTableStmt>();
        s->table_name = ExpectIdent();
        ExpectPunct("(");
        s->columns.push_back(ParseColumnDef());
        while (MatchPunct(",")) s->columns.push_back(ParseColumnDef());
        ExpectPunct(")");
        ExpectEnd();
        return s;
    }

    ColumnDef ParseColumnDef() {
        ColumnDef cd;
        cd.name = ExpectIdent();
        cd.varchar_len = 0;
        if (Peek().kind != TokenKind::kKeyword) Fail("expected column type, got '" + Peek().text + "'");
        const std::string kw = Peek().text;
        if (kw == "INT") {
            cd.type = ColumnType::kInt;
            ++pos_;
        } else if (kw == "BIGINT") {
            cd.type = ColumnType::kBigInt;
            ++pos_;
        } else if (kw == "FLOAT") {
            cd.type = ColumnType::kFloat;
            ++pos_;
        } else if (kw == "VARCHAR") {
            cd.type = ColumnType::kVarchar;
            ++pos_;
            ExpectPunct("(");
            if (Peek().kind != TokenKind::kInteger || Peek().int_value <= 0 ||
                Peek().int_value > 65535) {
                Fail("VARCHAR length must be a positive integer <= 65535");
            }
            cd.varchar_len = static_cast<std::uint16_t>(Consume().int_value);
            ExpectPunct(")");
        } else {
            Fail("unknown column type: " + kw);
        }
        return cd;
    }

    std::unique_ptr<Statement> ParseDrop() {
        ExpectKw("DROP");
        ExpectKw("TABLE");
        auto s = std::make_unique<DropTableStmt>();
        s->table_name = ExpectIdent();
        ExpectEnd();
        return s;
    }

    std::unique_ptr<Statement> ParseTruncate() {
        ExpectKw("TRUNCATE");
        ExpectKw("TABLE");
        auto s = std::make_unique<TruncateStmt>();
        s->table_name = ExpectIdent();
        ExpectEnd();
        return s;
    }
};

}  // namespace

std::unique_ptr<Statement> Parse(std::string_view sql) {
    auto tokens = Tokenize(sql);
    Parser p(std::move(tokens));
    return p.ParseStatement();
}

}  // namespace liteolap::sql
