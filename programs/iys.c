
#include "libpieg.h"

/*
 * IYS - Interpret Your Script
 * Native PIEG scripting interpreter
 *
 * Usage:
 *   iys
 *   iys FILE.YS
 *   iys --version
 *   iys --help
 */

#define IYS_VERSION       "1.1"
#define SOURCE_MAX        32768
#define TOKEN_MAX         256
#define VARIABLE_MAX      64
#define VARIABLE_NAME_MAX 32
#define STRING_MAX        256

enum {
    TOK_EOF = 256,
    TOK_ID,
    TOK_NUMBER,
    TOK_STRING,
    TOK_NEWLINE,
    TOK_EQEQ,
    TOK_NEQ,
    TOK_LTE,
    TOK_GTE
};

typedef struct {
    int kind;
    char text[TOKEN_MAX];
    int64_t number;
    unsigned int line;
} token_t;

typedef struct {
    char name[VARIABLE_NAME_MAX];
    int type;
    int64_t number;
    char string[STRING_MAX];
} variable_t;

typedef struct {
    int type;
    int64_t number;
    char string[STRING_MAX];
} value_t;

static char source[SOURCE_MAX + 1];
static const char *cursor;
static token_t token;

static variable_t variables[VARIABLE_MAX];
static unsigned int variable_count;

static unsigned int line_number;
static int failed;

/* ---------------------------------------------------------
 * STRING HELPERS
 * --------------------------------------------------------- */

static void copy_string(char *dst, const char *src, size_t cap)
{
    size_t i = 0;

    if (!cap)
        return;

    while (src[i] && i + 1 < cap) {
        dst[i] = src[i];
        i++;
    }

    dst[i] = '\0';
}

static int string_equal(const char *a, const char *b)
{
    return p_streq(a, b);
}

static void value_string(value_t *v, const char *s)
{
    v->type = 1;
    v->number = 0;
    copy_string(v->string, s, sizeof(v->string));
}

static void value_number(value_t *v, int64_t n)
{
    v->type = 0;
    v->number = n;
    v->string[0] = '\0';
}

static void number_to_string(int64_t n, char *out, size_t cap)
{
    char temp[32];
    unsigned int count = 0;
    unsigned int pos = 0;
    uint64_t value;

    if (!cap)
        return;

    if (n < 0)
        value = (uint64_t)(-(n + 1)) + 1;
    else
        value = (uint64_t)n;

    do {
        temp[count++] = (char)('0' + value % 10);
        value /= 10;
    } while (value && count < sizeof(temp));

    if (n < 0 && pos + 1 < cap)
        out[pos++] = '-';

    while (count && pos + 1 < cap)
        out[pos++] = temp[--count];

    out[pos] = '\0';
}

static void value_to_string(value_t *v, char *out, size_t cap)
{
    if (v->type)
        copy_string(out, v->string, cap);
    else
        number_to_string(v->number, out, cap);
}

static int value_truth(value_t *v)
{
    if (v->type)
        return v->string[0] != '\0';

    return v->number != 0;
}

static void print_value(value_t *v)
{
    if (v->type) {
        p_puts(v->string);
    } else if (v->number < 0) {
        char buffer[32];
        number_to_string(v->number, buffer, sizeof(buffer));
        p_puts(buffer);
    } else {
        p_u64((uint64_t)v->number);
    }

    p_putc('\n');
}

/* ---------------------------------------------------------
 * ERROR HANDLING
 * --------------------------------------------------------- */

static void error(const char *message)
{
    if (failed)
        return;

    failed = 1;

    p_puts("IYS error at line ");
    p_u64(token.line);
    p_puts(": ");
    p_putln(message);
}

/* ---------------------------------------------------------
 * LEXER
 * --------------------------------------------------------- */

static int is_alpha(char c)
{
    return (c >= 'a' && c <= 'z') ||
           (c >= 'A' && c <= 'Z') ||
           c == '_';
}

static int is_digit(char c)
{
    return c >= '0' && c <= '9';
}

static int is_alnum(char c)
{
    return is_alpha(c) || is_digit(c);
}

static void next_token(void)
{
    token.text[0] = '\0';
    token.number = 0;

    for (;;) {
        char c = *cursor;

        if (c == ' ' || c == '\t' || c == '\r') {
            cursor++;
            continue;
        }

        if (c == '#') {
            while (*cursor && *cursor != '\n')
                cursor++;

            continue;
        }

        break;
    }

    token.line = line_number;

    char c = *cursor;

    if (!c) {
        token.kind = TOK_EOF;
        return;
    }

    if (c == '\n') {
        cursor++;
        line_number++;
        token.kind = TOK_NEWLINE;
        return;
    }

    if (is_alpha(c)) {
        unsigned int n = 0;

        while (is_alnum(*cursor)) {
            if (n + 1 >= TOKEN_MAX) {
                error("Identifier too long.");
                return;
            }

            token.text[n++] = *cursor++;
        }

        token.text[n] = '\0';
        token.kind = TOK_ID;
        return;
    }

    if (is_digit(c)) {
        uint64_t value = 0;

        while (is_digit(*cursor)) {
            unsigned int digit = (unsigned int)(*cursor - '0');

            if (value > 922337203685477580ULL ||
                (value == 922337203685477580ULL && digit > 7)) {
                error("Integer overflow.");
                return;
            }

            value = value * 10 + digit;
            cursor++;
        }

        token.number = (int64_t)value;
        token.kind = TOK_NUMBER;
        return;
    }

    if (c == '"') {
        cursor++;

        unsigned int n = 0;

        while (*cursor && *cursor != '"') {
            char ch = *cursor++;

            if (ch == '\n') {
                error("Unterminated string.");
                return;
            }

            if (ch == '\\') {
                ch = *cursor++;

                switch (ch) {
                case 'n': ch = '\n'; break;
                case 't': ch = '\t'; break;
                case 'r': ch = '\r'; break;
                case '\\': break;
                case '"': break;

                default:
                    error("Unknown string escape.");
                    return;
                }
            }

            if (n + 1 >= TOKEN_MAX) {
                error("String too long.");
                return;
            }

            token.text[n++] = ch;
        }

        if (*cursor != '"') {
            error("Unterminated string.");
            return;
        }

        cursor++;
        token.text[n] = '\0';
        token.kind = TOK_STRING;
        return;
    }

    cursor++;

    if (c == '=' && *cursor == '=') {
        cursor++;
        token.kind = TOK_EQEQ;
        return;
    }

    if (c == '!' && *cursor == '=') {
        cursor++;
        token.kind = TOK_NEQ;
        return;
    }

    if (c == '<' && *cursor == '=') {
        cursor++;
        token.kind = TOK_LTE;
        return;
    }

    if (c == '>' && *cursor == '=') {
        cursor++;
        token.kind = TOK_GTE;
        return;
    }

    token.kind = (unsigned char)c;
}

static void advance(void)
{
    if (!failed)
        next_token();
}

static int accept(int kind)
{
    if (token.kind != kind)
        return 0;

    advance();
    return 1;
}

static int expect(int kind, const char *message)
{
    if (token.kind != kind) {
        error(message);
        return 0;
    }

    advance();
    return 1;
}

static void skip_lines(void)
{
    while (!failed &&
           (token.kind == TOK_NEWLINE || token.kind == ';'))
        advance();
}

/* ---------------------------------------------------------
 * VARIABLES
 * --------------------------------------------------------- */

static variable_t *find_variable(const char *name)
{
    for (unsigned int i = 0; i < variable_count; i++) {
        if (string_equal(variables[i].name, name))
            return &variables[i];
    }

    return 0;
}

static void set_variable(const char *name, value_t *value)
{
    variable_t *v = find_variable(name);

    if (!v) {
        if (variable_count >= VARIABLE_MAX) {
            error("Too many variables.");
            return;
        }

        v = &variables[variable_count++];
        copy_string(v->name, name, sizeof(v->name));
    }

    v->type = value->type;
    v->number = value->number;

    copy_string(v->string, value->string, sizeof(v->string));
}

static void get_variable(const char *name, value_t *out)
{
    variable_t *v = find_variable(name);

    if (!v) {
        error("Undefined variable.");
        return;
    }

    out->type = v->type;
    out->number = v->number;

    copy_string(out->string, v->string, sizeof(out->string));
}

/* ---------------------------------------------------------
 * EXPRESSION PARSER
 * --------------------------------------------------------- */

static value_t expression(int execute);

static value_t primary(int execute)
{
    value_t result;
    value_number(&result, 0);

    if (token.kind == TOK_NUMBER) {
        result.number = token.number;
        advance();
        return result;
    }

    if (token.kind == TOK_STRING) {
        value_string(&result, token.text);
        advance();
        return result;
    }

    if (token.kind == TOK_ID) {
        char name[VARIABLE_NAME_MAX];

        copy_string(name, token.text, sizeof(name));
        advance();

        if (execute)
            get_variable(name, &result);

        return result;
    }

    if (accept('(')) {
        result = expression(execute);
        expect(')', "Expected closing parenthesis.");
        return result;
    }

    error("Expected a value.");
    return result;
}

static value_t unary(int execute)
{
    if (accept('-')) {
        value_t v = unary(execute);

        if (execute) {
            if (v.type) {
                error("Cannot negate a string.");
            } else if (v.number == INT64_MIN) {
                error("Integer overflow.");
            } else {
                v.number = -v.number;
            }
        }

        return v;
    }

    return primary(execute);
}

static int checked_add(int64_t a, int64_t b, int64_t *out)
{
    if (b > 0 && a > INT64_MAX - b)
        return 0;

    if (b < 0 && a < INT64_MIN - b)
        return 0;

    *out = a + b;
    return 1;
}

static int checked_subtract(int64_t a, int64_t b, int64_t *out)
{
    if (b < 0 && a > INT64_MAX + b)
        return 0;

    if (b > 0 && a < INT64_MIN + b)
        return 0;

    *out = a - b;
    return 1;
}

static int checked_multiply(int64_t a, int64_t b, int64_t *out)
{
    if (!a || !b) {
        *out = 0;
        return 1;
    }

    if ((a == -1 && b == INT64_MIN) ||
        (b == -1 && a == INT64_MIN))
        return 0;

    if (a > 0) {
        if (b > 0) {
            if (a > INT64_MAX / b)
                return 0;
        } else if (b < INT64_MIN / a) {
            return 0;
        }
    } else {
        if (b > 0) {
            if (a < INT64_MIN / b)
                return 0;
        } else if (a < INT64_MAX / b) {
            return 0;
        }
    }

    *out = a * b;
    return 1;
}

static value_t multiply(int execute)
{
    value_t left = unary(execute);

    while (token.kind == '*' || token.kind == '/' || token.kind == '%') {
        int op = token.kind;
        advance();

        value_t right = unary(execute);

        if (!execute)
            continue;

        if (left.type || right.type) {
            error("Arithmetic requires numbers.");
            continue;
        }

        if ((op == '/' || op == '%') && right.number == 0) {
            error("Division by zero.");
            continue;
        }

        if ((op == '/' || op == '%') &&
            left.number == INT64_MIN && right.number == -1) {
            error("Integer overflow.");
            continue;
        }

        if (op == '*') {
            int64_t result;

            if (!checked_multiply(left.number, right.number, &result)) {
                error("Integer overflow.");
                continue;
            }

            left.number = result;
        } else if (op == '/') {
            left.number /= right.number;
        } else {
            left.number %= right.number;
        }
    }

    return left;
}

static value_t additive(int execute)
{
    value_t left = multiply(execute);

    while (token.kind == '+' || token.kind == '-') {
        int op = token.kind;
        advance();

        value_t right = multiply(execute);

        if (!execute)
            continue;

        if (op == '-' && (left.type || right.type)) {
            error("Subtraction requires numbers.");
            continue;
        }

        if (op == '+') {
            if (!left.type && !right.type) {
                int64_t result;

                if (!checked_add(left.number, right.number, &result)) {
                    error("Integer overflow.");
                    continue;
                }

                left.number = result;
            } else {
                char a[STRING_MAX];
                char b[STRING_MAX];
                char combined[STRING_MAX];

                value_to_string(&left, a, sizeof(a));
                value_to_string(&right, b, sizeof(b));

                copy_string(combined, a, sizeof(combined));

                size_t n = p_strlen(combined);
                size_t i = 0;

                while (b[i] && n + 1 < sizeof(combined))
                    combined[n++] = b[i++];

                combined[n] = '\0';

                value_string(&left, combined);
            }
        } else {
            int64_t result;

            if (!checked_subtract(left.number, right.number, &result)) {
                error("Integer overflow.");
                continue;
            }

            left.number = result;
        }
    }

    return left;
}

/* ---------------------------------------------------------
 * COMPARISON
 * --------------------------------------------------------- */

static int compare_values(value_t *a, value_t *b)
{
    if (a->type || b->type) {
        char sa[STRING_MAX];
        char sb[STRING_MAX];

        value_to_string(a, sa, sizeof(sa));
        value_to_string(b, sb, sizeof(sb));

        const char *pa = sa;
        const char *pb = sb;

        while (*pa && *pb && *pa == *pb) {
            pa++;
            pb++;
        }

        if ((unsigned char)*pa < (unsigned char)*pb)
            return -1;

        if ((unsigned char)*pa > (unsigned char)*pb)
            return 1;

        return 0;
    }

    if (a->number < b->number)
        return -1;

    if (a->number > b->number)
        return 1;

    return 0;
}

static value_t expression(int execute)
{
    value_t left = additive(execute);

    while (token.kind == TOK_EQEQ ||
           token.kind == TOK_NEQ ||
           token.kind == '<' ||
           token.kind == '>' ||
           token.kind == TOK_LTE ||
           token.kind == TOK_GTE) {

        int op = token.kind;
        advance();

        value_t right = additive(execute);

        if (!execute)
            continue;

        int cmp = compare_values(&left, &right);
        int result = 0;

        switch (op) {
        case TOK_EQEQ: result = cmp == 0; break;
        case TOK_NEQ:  result = cmp != 0; break;
        case '<':      result = cmp < 0; break;
        case '>':      result = cmp > 0; break;
        case TOK_LTE:  result = cmp <= 0; break;
        case TOK_GTE:  result = cmp >= 0; break;
        }

        value_number(&left, result);
    }

    return left;
}

/* ---------------------------------------------------------
 * STATEMENT PARSER
 * --------------------------------------------------------- */

static void statement(int execute);

static void block(int execute)
{
    if (!expect('{', "Expected opening brace."))
        return;

    skip_lines();

    while (!failed && token.kind != TOK_EOF && token.kind != '}') {
        statement(execute);

        if (failed)
            return;

        skip_lines();
    }

    expect('}', "Expected closing brace.");
}

static void statement_if(int execute)
{
    advance();

    value_t condition = expression(execute);
    int condition_true = execute && value_truth(&condition);

    if (!expect('{', "Expected '{' after condition."))
        return;

    block(condition_true);

    if (failed)
        return;

    skip_lines();

    if (token.kind == TOK_ID && string_equal(token.text, "else")) {
        advance();

        if (!expect('{', "Expected '{' after else."))
            return;

        block(execute && !value_truth(&condition));
    }
}

static void statement_let(int execute)
{
    advance();

    if (token.kind != TOK_ID) {
        error("Expected variable name.");
        return;
    }

    char name[VARIABLE_NAME_MAX];
    copy_string(name, token.text, sizeof(name));

    advance();

    if (!expect('=', "Expected '=' after variable name."))
        return;

    value_t value = expression(execute);

    if (execute && !failed)
        set_variable(name, &value);
}

static void statement_print(int execute)
{
    advance();

    if (!expect('(', "Expected '(' after print."))
        return;

    value_t value = expression(execute);

    if (!expect(')', "Expected ')' after print expression."))
        return;

    if (execute && !failed)
        print_value(&value);
}

static void statement_assignment(int execute)
{
    char name[VARIABLE_NAME_MAX];

    copy_string(name, token.text, sizeof(name));
    advance();

    if (!expect('=', "Expected '=' in assignment."))
        return;

    value_t value = expression(execute);

    if (execute && !failed)
        set_variable(name, &value);
}

static void statement(int execute)
{
    if (failed || token.kind == TOK_EOF)
        return;

    if (token.kind == TOK_NEWLINE || token.kind == ';') {
        advance();
        return;
    }

    if (token.kind != TOK_ID) {
        error("Expected a statement.");
        return;
    }

    if (string_equal(token.text, "let")) {
        statement_let(execute);
        return;
    }

    if (string_equal(token.text, "print")) {
        statement_print(execute);
        return;
    }

    if (string_equal(token.text, "if")) {
        statement_if(execute);
        return;
    }

    statement_assignment(execute);
}

/* ---------------------------------------------------------
 * INTERPRETER
 * --------------------------------------------------------- */

static int execute_source(const char *text)
{
    cursor = text;
    line_number = 1;
    failed = 0;

    advance();
    skip_lines();

    while (token.kind != TOK_EOF && !failed) {
        statement(1);
        skip_lines();
    }

    return failed ? 1 : 0;
}

/* ---------------------------------------------------------
 * FILE LOADING
 * --------------------------------------------------------- */

static int run_file(const char *filename)
{
    char path[256];
    int is_directory = 0;
    uint32_t size = 0;

    pieg->resolve(filename, path, sizeof(path));

    int err = pieg->fs_stat(path, &is_directory, &size);

    if (err) {
        p_puts("IYS: cannot access file: ");
        p_putln(pieg->fs_error(err));
        return 1;
    }

    if (is_directory) {
        p_putln("IYS: expected a file, not a directory.");
        return 1;
    }

    if (size > SOURCE_MAX) {
        p_putln("IYS: script exceeds maximum size of 32 KiB.");
        return 1;
    }

    uint32_t offset = 0;

    while (offset < size) {
        uint32_t done = 0;
        uint32_t amount = size - offset;

        if (amount > 512)
            amount = 512;

        err = pieg->fs_read(path, offset, source + offset, amount, &done);

        if (err) {
            p_puts("IYS: read error: ");
            p_putln(pieg->fs_error(err));
            return 1;
        }

        if (!done) {
            p_putln("IYS: unexpected end of file.");
            return 1;
        }

        if (done > amount) {
            p_putln("IYS: filesystem returned an invalid read length.");
            return 1;
        }

        offset += done;
    }

    source[size] = '\0';

    p_puts("IYS ");
    p_puts(IYS_VERSION);
    p_puts(" executing ");
    p_putln(filename);

    variable_count = 0;
    memset(variables, 0, sizeof(variables));

    return execute_source(source);
}

/* ---------------------------------------------------------
 * INTERACTIVE REPL
 * --------------------------------------------------------- */

static int read_line(char *buffer, size_t capacity)
{
    size_t n = 0;

    for (;;) {
        char c = p_getc();

        if (c == '\r' || c == '\n') {
            p_putc('\n');
            break;
        }

        if ((c == '\b' || (unsigned char)c == 127) && n) {
            n--;
            p_putc('\b');
            p_putc(' ');
            p_putc('\b');
            continue;
        }

        if (c >= 32 && c < 127 && n + 1 < capacity) {
            buffer[n++] = c;
            p_putc(c);
        }
    }

    buffer[n] = '\0';
    return (int)n;
}

static void repl(void)
{
    p_putln("IYS - Interpret Your Script");
    p_putln("PIEG scripting interpreter");
    p_putln("Type 'exit' to leave.");
    p_putln("");

    for (;;) {
        p_puts("iys> ");

        read_line(source, SOURCE_MAX + 1);

        if (string_equal(source, "exit") ||
            string_equal(source, "quit")) {
            p_putln("Leaving IYS.");
            return;
        }

        if (!source[0])
            continue;

        execute_source(source);
    }
}

/* ---------------------------------------------------------
 * HELP
 * --------------------------------------------------------- */

static void help(void)
{
    p_putln("IYS - Interpret Your Script");
    p_putln("");
    p_putln("Usage:");
    p_putln("  iys");
    p_putln("  iys FILE.YS");
    p_putln("  iys --version");
    p_putln("  iys --help");
    p_putln("");
    p_putln("Language:");
    p_putln("  let name = \"PIEG\"");
    p_putln("  let number = 10 + 5");
    p_putln("  print(name)");
    p_putln("  number = number * 2");
    p_putln("");
    p_putln("  if number > 10 {");
    p_putln("      print(\"Greater than 10\")");
    p_putln("  } else {");
    p_putln("      print(\"Smaller or equal\")");
    p_putln("  }");
    p_putln("");
    p_putln("Arithmetic:");
    p_putln("  +  -  *  /  %");
    p_putln("");
    p_putln("Comparisons:");
    p_putln("  ==  !=  <  >  <=  >=");
    p_putln("");
    p_putln("Comments begin with #.");
}

/* ---------------------------------------------------------
 * ENTRY POINT
 * --------------------------------------------------------- */

int main(int argc, char **argv)
{
    if (!pieg || pieg->version < 4) {
        p_putln("IYS requires PIEG API version 4.");
        return 1;
    }

    variable_count = 0;
    memset(variables, 0, sizeof(variables));

    if (argc < 2) {
        repl();
        return 0;
    }

    if (string_equal(argv[1], "--version")) {
        p_putln("IYS " IYS_VERSION);
        return 0;
    }

    if (string_equal(argv[1], "--help") ||
        string_equal(argv[1], "help")) {
        help();
        return 0;
    }

    if (argc > 2) {
        p_putln("IYS currently accepts one script at a time.");
        return 1;
    }

    return run_file(argv[1]);
}
