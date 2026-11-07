#include "libpieg.h"

/*
 * IYS - Interpret Your Script
 * Small Python-compatible interpreter for PIEG.
 *
 * Supported subset:
 *   - integers, strings, True, False
 *   - variables and assignment
 *   - print(value, value, ...)
 *   - + - * / % and parentheses
 *   - == != < <= > >=
 *   - if / else blocks using Python indentation and colons
 *   - # comments
 *
 * This is deliberately a small Python-like subset, not CPython.
 */

#define SOURCE_MAX 32768
#define LINE_MAX   512
#define VAR_MAX    64
#define NAME_MAX   32
#define STR_MAX    256
#define TOK_MAX    128

enum { V_INT, V_STR, V_BOOL };
typedef struct {
    int type;
    int64_t n;
    char s[STR_MAX];
} value_t;

typedef struct {
    char name[NAME_MAX];
    value_t value;
} variable_t;

enum {
    T_END = 0, T_NUM = 256, T_STR, T_ID, T_EQ, T_NE, T_LE, T_GE,
    T_AND, T_OR, T_NOT
};

typedef struct {
    int kind;
    int64_t number;
    char text[STR_MAX];
} token_t;

static variable_t vars[VAR_MAX];
static unsigned int var_count;
static token_t toks[TOK_MAX];
static unsigned int ntok, pos;
static const char *expr_src;
static int expr_error;
static unsigned int current_line;

static void copystr(char *d, const char *s, size_t cap)
{
    size_t i = 0;
    if (!cap) return;
    while (s[i] && i + 1 < cap) { d[i] = s[i]; i++; }
    d[i] = 0;
}

static void say_error(const char *s)
{
    p_puts("IYS: line ");
    p_u64(current_line);
    p_puts(": ");
    p_putln(s);
}

static value_t vint(int64_t n)
{
    value_t v;
    memset(&v, 0, sizeof(v));
    v.type = V_INT;
    v.n = n;
    return v;
}

static value_t vbool(int b)
{
    value_t v = vint(!!b);
    v.type = V_BOOL;
    return v;
}

static value_t vstr(const char *s)
{
    value_t v;
    memset(&v, 0, sizeof(v));
    v.type = V_STR;
    copystr(v.s, s, sizeof(v.s));
    return v;
}

static int truth(value_t v)
{
    if (v.type == V_STR) return v.s[0] != 0;
    return v.n != 0;
}

static void value_text(value_t v, char *out, size_t cap)
{
    if (v.type == V_STR) { copystr(out, v.s, cap); return; }
    if (v.type == V_BOOL) { copystr(out, v.n ? "True" : "False", cap); return; }

    char rev[32];
    unsigned int n = 0, p = 0;
    uint64_t x;
    if (!cap) return;
    if (v.n < 0) x = (uint64_t)(-(v.n + 1)) + 1;
    else x = (uint64_t)v.n;
    do { rev[n++] = (char)('0' + x % 10); x /= 10; } while (x && n < sizeof(rev));
    if (v.n < 0 && p + 1 < cap) out[p++] = '-';
    while (n && p + 1 < cap) out[p++] = rev[--n];
    out[p] = 0;
}

static int find_var(const char *name)
{
    for (unsigned int i = 0; i < var_count; i++)
        if (p_streq(vars[i].name, name)) return (int)i;
    return -1;
}

static int set_var(const char *name, value_t v)
{
    int i = find_var(name);
    if (i < 0) {
        if (var_count >= VAR_MAX) return 0;
        i = (int)var_count++;
        copystr(vars[i].name, name, sizeof(vars[i].name));
    }
    vars[i].value = v;
    return 1;
}

/* ---------------- expression lexer ---------------- */

static int alpha_(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}
static int digit_(char c) { return c >= '0' && c <= '9'; }

static int tokenize(const char *s)
{
    ntok = pos = 0;
    expr_src = s;
    expr_error = 0;

    while (*s) {
        while (*s == ' ' || *s == '\t' || *s == '\r') s++;
        if (!*s) break;
        if (ntok + 1 >= TOK_MAX) { expr_error = 1; break; }

        token_t *t = &toks[ntok++];
        memset(t, 0, sizeof(*t));

        if (digit_(*s)) {
            uint64_t n = 0;
            while (digit_(*s)) {
                n = n * 10 + (unsigned)(*s - '0');
                if (n > 9223372036854775807ull) { expr_error = 1; break; }
                s++;
            }
            t->kind = T_NUM;
            t->number = (int64_t)n;
            continue;
        }

        if (alpha_(*s)) {
            unsigned int n = 0;
            while (alpha_(*s) || digit_(*s)) {
                if (n + 1 < sizeof(t->text)) t->text[n++] = *s;
                s++;
            }
            t->text[n] = 0;
            t->kind = T_ID;
            if (p_streq(t->text, "and")) t->kind = T_AND;
            if (p_streq(t->text, "or")) t->kind = T_OR;
            if (p_streq(t->text, "not")) t->kind = T_NOT;
            continue;
        }

        if (*s == '"') {
            unsigned int n = 0;
            s++;
            while (*s && *s != '"') {
                char c = *s++;
                if (c == '\\' && *s) {
                    c = *s++;
                    if (c == 'n') c = '\n';
                    else if (c == 't') c = '\t';
                    else if (c != '\\' && c != '"') { expr_error = 1; break; }
                }
                if (n + 1 < sizeof(t->text)) t->text[n++] = c;
            }
            if (*s != '"') expr_error = 1;
            else s++;
            t->text[n] = 0;
            t->kind = T_STR;
            continue;
        }

        if (s[0] == '=' && s[1] == '=') { t->kind = T_EQ; s += 2; continue; }
        if (s[0] == '!' && s[1] == '=') { t->kind = T_NE; s += 2; continue; }
        if (s[0] == '<' && s[1] == '=') { t->kind = T_LE; s += 2; continue; }
        if (s[0] == '>' && s[1] == '=') { t->kind = T_GE; s += 2; continue; }
        t->kind = (unsigned char)*s++;
    }
    toks[ntok].kind = T_END;
    return !expr_error;
}

static token_t *peek(void) { return &toks[pos]; }
static int accept(int k) { if (peek()->kind == k) { pos++; return 1; } return 0; }

static value_t parse_or(void);

static value_t primary(void)
{
    token_t t = *peek();
    if (accept(T_NUM)) return vint(t.number);
    if (accept(T_STR)) return vstr(t.text);

    if (accept(T_ID)) {
        if (p_streq(t.text, "True")) return vbool(1);
        if (p_streq(t.text, "False")) return vbool(0);
        if (p_streq(t.text, "None")) return vint(0);
        int i = find_var(t.text);
        if (i < 0) { expr_error = 1; return vint(0); }
        return vars[i].value;
    }

    if (accept('(')) {
        value_t v = parse_or();
        if (!accept(')')) expr_error = 1;
        return v;
    }

    expr_error = 1;
    return vint(0);
}

static value_t unary(void)
{
    if (accept('-')) {
        value_t v = unary();
        if (v.type == V_STR) expr_error = 1;
        else if (v.n == (-9223372036854775807ll - 1)) expr_error = 1;
        else v.n = -v.n;
        return v;
    }
    if (accept(T_NOT)) return vbool(!truth(unary()));
    return primary();
}

static value_t mul(void)
{
    value_t a = unary();
    while (peek()->kind == '*' || peek()->kind == '/' || peek()->kind == '%') {
        int op = peek()->kind; pos++;
        value_t b = unary();
        if (a.type == V_STR || b.type == V_STR || ((op == '/' || op == '%') && b.n == 0)) {
            expr_error = 1; return vint(0);
        }
        if (op == '*') a.n *= b.n;
        else if (op == '/') a.n /= b.n;
        else a.n %= b.n;
        a.type = V_INT;
    }
    return a;
}

static value_t add(void)
{
    value_t a = mul();
    while (peek()->kind == '+' || peek()->kind == '-') {
        int op = peek()->kind; pos++;
        value_t b = mul();
        if (op == '+' && (a.type == V_STR || b.type == V_STR)) {
            char x[STR_MAX], y[STR_MAX], z[STR_MAX];
            value_text(a, x, sizeof(x)); value_text(b, y, sizeof(y));
            copystr(z, x, sizeof(z));
            size_t n = p_strlen(z), j = 0;
            while (y[j] && n + 1 < sizeof(z)) z[n++] = y[j++];
            z[n] = 0;
            a = vstr(z);
        } else {
            if (a.type == V_STR || b.type == V_STR) { expr_error = 1; return vint(0); }
            if (op == '+') a.n += b.n; else a.n -= b.n;
            a.type = V_INT;
        }
    }
    return a;
}

static int compare(value_t a, value_t b)
{
    if (a.type == V_STR || b.type == V_STR) {
        char x[STR_MAX], y[STR_MAX];
        value_text(a, x, sizeof(x)); value_text(b, y, sizeof(y));
        int c = 0;
        while (x[c] && y[c] && x[c] == y[c]) c++;
        return (unsigned char)x[c] - (unsigned char)y[c];
    }
    return a.n < b.n ? -1 : a.n > b.n ? 1 : 0;
}

static value_t comparison(void)
{
    value_t a = add();
    int op = peek()->kind;
    if (op == T_EQ || op == T_NE || op == '<' || op == '>' || op == T_LE || op == T_GE) {
        pos++;
        value_t b = add();
        int c = compare(a, b);
        return vbool(op == T_EQ ? c == 0 : op == T_NE ? c != 0 :
                     op == '<' ? c < 0 : op == '>' ? c > 0 :
                     op == T_LE ? c <= 0 : c >= 0);
    }
    return a;
}

static value_t parse_and(void)
{
    value_t a = comparison();
    while (accept(T_AND)) { value_t b = comparison(); a = vbool(truth(a) && truth(b)); }
    return a;
}

static value_t parse_or(void)
{
    value_t a = parse_and();
    while (accept(T_OR)) { value_t b = parse_and(); a = vbool(truth(a) || truth(b)); }
    return a;
}

static int eval(const char *s, value_t *out)
{
    if (!tokenize(s)) return 0;
    *out = parse_or();
    return !expr_error && peek()->kind == T_END;
}

/* ---------------- statements / Python indentation ---------------- */

typedef struct {
    char text[LINE_MAX];
    unsigned int indent;
    unsigned int number;
} line_t;

static line_t lines[256];
static unsigned int line_count;

static int prepare_lines(const char *src)
{
    line_count = 0;
    unsigned int line = 1;
    while (*src) {
        if (line_count >= 256) return 0;
        line_t *l = &lines[line_count];
        memset(l, 0, sizeof(*l));
        l->number = line;
        while (*src == ' ' || *src == '\t') {
            l->indent += (*src == '\t') ? 4 : 1;
            src++;
        }
        unsigned int n = 0;
        int quoted = 0;
        while (*src && *src != '\n' && *src != '\r') {
            char c = *src++;
            if (c == '"' && (n == 0 || l->text[n-1] != '\\')) quoted = !quoted;
            if (c == '#' && !quoted) {
                while (*src && *src != '\n') src++;
                break;
            }
            if (n + 1 < sizeof(l->text)) l->text[n++] = c;
        }
        l->text[n] = 0;
        if (*src == '\r') src++;
        if (*src == '\n') { src++; line++; }
        line_count++;
    }
    return 1;
}

static int starts(const char *s, const char *prefix)
{
    while (*prefix) if (*s++ != *prefix++) return 0;
    return 1;
}

static char *trim(char *s)
{
    while (*s == ' ' || *s == '\t') s++;
    size_t n = p_strlen(s);
    while (n && (s[n-1] == ' ' || s[n-1] == '\t')) s[--n] = 0;
    return s;
}

static int valid_name(const char *s)
{
    if (!alpha_(*s)) return 0;
    while (*s) { if (!alpha_(*s) && !digit_(*s)) return 0; s++; }
    return 1;
}

static int exec_simple(char *s)
{
    s = trim(s);
    if (!*s) return 1;

    if (starts(s, "print") && s[5] == '(') {
        char *p = s + 6;
        char arg[STR_MAX];
        int depth = 0, quote = 0, first = 1;
        while (*p && *p != ')') {
            char *start = p;
            depth = 0; quote = 0;
            while (*p) {
                if (*p == '"' && (p == start || p[-1] != '\\')) quote = !quote;
                if (!quote && *p == '(') depth++;
                if (!quote && *p == ')') { if (!depth) break; depth--; }
                if (!quote && !depth && *p == ',') break;
                p++;
            }
            size_t n = (size_t)(p - start);
            if (n >= sizeof(arg)) return 0;
            memcpy(arg, start, n); arg[n] = 0;
            value_t v;
            if (!eval(trim(arg), &v)) return 0;
            if (!first) p_putc(' ');
            char out[STR_MAX]; value_text(v, out, sizeof(out)); p_puts(out);
            first = 0;
            if (*p == ',') p++;
            else break;
        }
        if (*p != ')') return 0;
        p_putc('\n');
        return 1;
    }

    /* Assignment: identifier = expression (not ==). */
    char *eq = 0;
    int quote = 0, depth = 0;
    for (char *p = s; *p; p++) {
        if (*p == '"' && (p == s || p[-1] != '\\')) quote = !quote;
        if (!quote && *p == '(') depth++;
        if (!quote && *p == ')') depth--;
        if (!quote && !depth && *p == '=' && p[1] != '=' && (p == s || p[-1] != '=')) { eq = p; break; }
    }
    if (!eq) return 0;
    *eq = 0;
    char *name = trim(s);
    if (!valid_name(name)) return 0;
    value_t v;
    if (!eval(trim(eq + 1), &v)) return 0;
    return set_var(name, v);
}

static int run_lines(unsigned int *index, unsigned int parent_indent);

static int run_if(unsigned int *index, unsigned int indent)
{
    line_t *l = &lines[*index];
    char cond[LINE_MAX];
    copystr(cond, trim(l->text) + 3, sizeof(cond));
    char *colon = cond + p_strlen(cond);
    while (colon > cond && colon[-1] == ' ') colon--;
    if (colon == cond || colon[-1] != ':') return 0;
    colon[-1] = 0;

    value_t v;
    if (!eval(trim(cond), &v)) return 0;
    int take_then = truth(v);
    (*index)++;

    if (*index >= line_count || lines[*index].indent <= indent) return 0;
    unsigned int child_indent = lines[*index].indent;

    if (take_then) {
        if (!run_lines(index, child_indent)) return 0;
    } else {
        while (*index < line_count && (lines[*index].text[0] == 0 ||
               lines[*index].indent >= child_indent)) (*index)++;
    }

    /* Optional else at the same indentation. */
    while (*index < line_count && !lines[*index].text[0]) (*index)++;
    if (*index < line_count && lines[*index].indent == indent &&
        starts(trim(lines[*index].text), "else:")) {
        (*index)++;
        if (*index >= line_count || lines[*index].indent <= indent) return 0;
        unsigned int else_indent = lines[*index].indent;
        if (!take_then) {
            if (!run_lines(index, else_indent)) return 0;
        } else {
            while (*index < line_count && (lines[*index].text[0] == 0 ||
                   lines[*index].indent >= else_indent)) (*index)++;
        }
    }
    return 1;
}

static int run_lines(unsigned int *index, unsigned int parent_indent)
{
    while (*index < line_count) {
        line_t *l = &lines[*index];
        if (!l->text[0]) { (*index)++; continue; }
        if (l->indent < parent_indent) return 1;
        if (l->indent > parent_indent) return 0;
        current_line = l->number;
        char *s = trim(l->text);

        if (starts(s, "if ") && p_strlen(s) > 4) {
            if (!run_if(index, parent_indent)) return 0;
            continue;
        }
        if (starts(s, "else:")) return 1;
        if (!exec_simple(s)) return 0;
        (*index)++;
    }
    return 1;
}

static int execute_source(const char *src)
{
    if (!prepare_lines(src)) { say_error("too many lines."); return 1; }
    var_count = 0;
    memset(vars, 0, sizeof(vars));
    unsigned int i = 0;
    if (!run_lines(&i, 0)) {
        say_error("unsupported syntax or invalid expression.");
        return 1;
    }
    return 0;
}

/* ---------------- file loading and REPL ---------------- */

static int run_file(const char *filename)
{
    char path[256];
    int is_dir = 0;
    uint32_t size = 0;
    pieg->resolve(filename, path, sizeof(path));
    int err = pieg->fs_stat(path, &is_dir, &size);
    if (err) {
        p_puts("IYS: cannot access file: ");
        p_putln(pieg->fs_error(err));
        return 1;
    }
    if (is_dir) { p_putln("IYS: expected a file."); return 1; }
    if (size > SOURCE_MAX) { p_putln("IYS: script exceeds 32 KiB."); return 1; }

    static char source[SOURCE_MAX + 1];
    uint32_t off = 0;
    while (off < size) {
        uint32_t done = 0, amount = size - off;
        if (amount > 512) amount = 512;
        err = pieg->fs_read(path, off, source + off, amount, &done);
        if (err || !done || done > amount) {
            p_puts("IYS: file read failed: ");
            p_putln(err ? pieg->fs_error(err) : "invalid read length");
            return 1;
        }
        off += done;
    }
    source[size] = 0;
    p_puts("IYS executing ");
    p_putln(filename);
    return execute_source(source);
}

static int repl(void)
{
    char line[LINE_MAX];
    p_putln("IYS - Interpret Your Script");
    p_putln("Small Python-compatible interpreter for PIEG");
    p_putln("Subset: variables, print, arithmetic, comparisons, if/else");
    p_putln("Type exit to leave.");
    for (;;) {
        p_puts(">>> ");
        unsigned int n = 0;
        for (;;) {
            char c = p_getc();
            if (c == '\r' || c == '\n') { p_putc('\n'); break; }
            if ((c == '\b' || (unsigned char)c == 127) && n) {
                n--; p_putc('\b'); p_putc(' '); p_putc('\b');
            } else if (c >= 32 && c < 127 && n + 1 < sizeof(line)) {
                line[n++] = c; p_putc(c);
            }
        }
        line[n] = 0;
        char *s = trim(line);
        if (p_streq(s, "exit()") || p_streq(s, "quit()")) return 0;
        if (!*s) continue;
        current_line = 1;
        if (!exec_simple(s)) say_error("unsupported REPL statement.");
    }
}

int main(int argc, char **argv)
{
    if (!pieg || pieg->version < 4) {
        p_putln("IYS: PIEG API version 4 required.");
        return 1;
    }
    if (argc > 2) {
        p_putln("Usage: iys [script.py]");
        return 2;
    }
    if (argc == 1) return repl();
    if (p_streq(argv[1], "--help") || p_streq(argv[1], "-h")) {
        p_putln("IYS - small Python-compatible interpreter");
        p_putln("Usage: iys [script.py]");
        p_putln("Supports: variables, integers, strings, print(), arithmetic,");
        p_putln("comparisons, and indented if/else blocks.");
        return 0;
    }
    return run_file(argv[1]);
}
