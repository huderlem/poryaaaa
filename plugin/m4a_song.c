/*
 * mid2agb `.s` song parser.  See m4a_song.h.
 */

#include "m4a_song.h"

#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------------
 * Built-in vocabulary (sound/MPlayDef.s)
 * ------------------------------------------------------------------------ */

/* gClockTable: the note / wait lengths that have a single-byte encoding. */
static const uint8_t sClockTable[49] = {
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20,
    21, 22, 23, 24, 28, 30, 32, 36, 40, 42, 44, 48, 52, 54, 56, 60, 64, 66, 68,
    72, 76, 78, 80, 84, 88, 90, 92, 96
};

static int clock_index(int length)
{
    for (int i = 0; i < 49; i++)
        if (sClockTable[i] == length)
            return i;
    return -1;
}

typedef struct { const char *name; int32_t value; } NamedValue;

static const NamedValue sKeywords[] = {
    { "FINE", 0xB1 }, { "GOTO", 0xB2 }, { "PATT", 0xB3 }, { "PEND", 0xB4 },
    { "REPT", 0xB5 }, { "MEMACC", 0xB9 }, { "PRIO", 0xBA }, { "TEMPO", 0xBB },
    { "KEYSH", 0xBC }, { "VOICE", 0xBD }, { "VOL", 0xBE }, { "PAN", 0xBF },
    { "PAM", 0xBF }, { "BEND", 0xC0 }, { "BENDR", 0xC1 }, { "LFOS", 0xC2 },
    { "LFODL", 0xC3 }, { "MOD", 0xC4 }, { "MODT", 0xC5 }, { "TUNE", 0xC8 },
    { "XCMD", 0xCD }, { "xXXX", 0x00 }, { "xWAVE", 0x01 }, { "xTYPE", 0x02 }, { "xATTA", 0x04 },
    { "xDECA", 0x05 }, { "xSUST", 0x06 }, { "xRELE", 0x07 }, { "xIECV", 0x08 }, { "xIECL", 0x09 },
    { "xLENG", 0x0A }, { "xSWEE", 0x0B }, { "xWAIT", 0x0C },
    { "EOT", 0xCE }, { "TIE", 0xCF },
    { "mxv", 0x7F }, { "c_v", 0x40 }, { "reverb_set", 0x80 },
    { "gtp1", 1 }, { "gtp2", 2 }, { "gtp3", 3 },
    { "mod_vib", 0 }, { "mod_tre", 1 }, { "mod_pan", 2 },
    { "mem_set", 0 }, { "mem_add", 1 }, { "mem_sub", 2 },
    { "mem_mem_set", 3 }, { "mem_mem_add", 4 }, { "mem_mem_sub", 5 },
    { "mem_beq", 6 }, { "mem_bne", 7 }, { "mem_bhi", 8 }, { "mem_bhs", 9 },
    { "mem_bls", 10 }, { "mem_blo", 11 },
    { "mem_mem_beq", 12 }, { "mem_mem_bne", 13 }, { "mem_mem_bhi", 14 },
    { "mem_mem_bhs", 15 }, { "mem_mem_bls", 16 }, { "mem_mem_blo", 17 },
};

static bool all_digits(const char *s, int n)
{
    for (int i = 0; i < n; i++)
        if (!isdigit((unsigned char)s[i]))
            return false;
    return true;
}

static bool builtin_symbol(const char *name, int32_t *value)
{
    size_t n = strlen(name);

    for (size_t i = 0; i < sizeof(sKeywords) / sizeof(sKeywords[0]); i++) {
        if (strcmp(name, sKeywords[i].name) == 0) {
            *value = sKeywords[i].value;
            return true;
        }
    }

    /* W00..W96 (0x80 + clock index) and N01..N96 (TIE + clock index) */
    if (n == 3 && (name[0] == 'W' || name[0] == 'N') && all_digits(name + 1, 2)) {
        int idx = clock_index(atoi(name + 1));
        if (idx < 0 || (name[0] == 'N' && idx == 0))
            return false;
        *value = (name[0] == 'W' ? 0x80 : 0xCF) + idx;
        return true;
    }

    /* Velocities v000..v127 */
    if (n == 4 && name[0] == 'v' && all_digits(name + 1, 3)) {
        int v = atoi(name + 1);
        if (v > 127)
            return false;
        *value = v;
        return true;
    }

    /* Note names: [A-G][ns](M2|M1|0..8), CnM2 = 0 ... Gn8 = 127 */
    if ((n == 3 || n == 4) && strchr("ABCDEFG", name[0]) && (name[1] == 'n' || name[1] == 's')) {
        static const int semis[7] = { 9, 11, 0, 2, 4, 5, 7 }; /* A B C D E F G */
        int octave;
        if (name[1] == 's' && (name[0] == 'E' || name[0] == 'B'))
            return false; /* MPlayDef.s defines no Es / Bs */
        if (n == 4 && name[2] == 'M' && (name[3] == '1' || name[3] == '2'))
            octave = -(name[3] - '0');
        else if (n == 3 && name[2] >= '0' && name[2] <= '8')
            octave = name[2] - '0';
        else
            return false;
        int v = (octave + 2) * 12 + semis[name[0] - 'A'] + (name[1] == 's');
        if (v < 0 || v > 127)
            return false;
        *value = v;
        return true;
    }

    return false;
}

/* ------------------------------------------------------------------------
 * Parser state
 * ------------------------------------------------------------------------ */

typedef struct {
    char *name;
    char *expr;             /* .equ: expression text (evaluated lazily) */
    int32_t value;          /* label: offset */
    bool isLabel;
} Symbol;

typedef struct {
    uint32_t offset;
    char *expr;
    int line;
} PendingWord;

typedef struct {
    M4ASong *song;
    uint32_t cap;

    Symbol *syms;
    int symCount, symCap;

    PendingWord *words;
    int wordCount, wordCap;

    char *globalName;
    bool sawOtherInclude;

    char *err;
    size_t errLen;
    int line;
    bool failed;
} Parser;

static void fail(Parser *p, const char *fmt, ...)
{
    if (p->failed)
        return;
    p->failed = true;
    if (!p->err || p->errLen == 0)
        return;
    char msg[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    if (p->line > 0)
        snprintf(p->err, p->errLen, "line %d: %s", p->line, msg);
    else
        snprintf(p->err, p->errLen, "%s", msg);
}

static char *dupn(const char *s, size_t n)
{
    char *d = malloc(n + 1);
    if (!d)
        return NULL;
    memcpy(d, s, n);
    d[n] = 0;
    return d;
}

static Symbol *find_symbol(Parser *p, const char *name)
{
    for (int i = 0; i < p->symCount; i++)
        if (strcmp(p->syms[i].name, name) == 0)
            return &p->syms[i];
    return NULL;
}

static Symbol *add_symbol(Parser *p, const char *name)
{
    if (p->symCount == p->symCap) {
        int cap = p->symCap ? p->symCap * 2 : 64;
        Symbol *s = realloc(p->syms, (size_t)cap * sizeof(Symbol));
        if (!s) { fail(p, "out of memory"); return NULL; }
        p->syms = s;
        p->symCap = cap;
    }
    Symbol *s = &p->syms[p->symCount++];
    memset(s, 0, sizeof(*s));
    s->name = dupn(name, strlen(name));
    return s;
}

static bool emit_byte(Parser *p, uint8_t b)
{
    M4ASong *song = p->song;
    if (song->size == p->cap) {
        uint32_t cap = p->cap ? p->cap * 2 : 4096;
        uint8_t *d = realloc(song->data, cap);
        if (!d) { fail(p, "out of memory"); return false; }
        song->data = d;
        p->cap = cap;
    }
    song->data[song->size++] = b;
    return true;
}

/* ------------------------------------------------------------------------
 * Expressions
 * ------------------------------------------------------------------------ */

typedef struct {
    Parser *p;
    const char *s;
    /* Symbol bookkeeping for relocations: how many label references and
     * which external (undefined) symbol the expression used. */
    int labelRefs;
    int32_t labelValue;
    const char *external;
    int depth;      /* .equ evaluation nesting */
    int nest;       /* parenthesis / unary nesting */
} Expr;

static int64_t parse_sum(Expr *e);

static void skip_ws(Expr *e)
{
    while (*e->s == ' ' || *e->s == '\t')
        e->s++;
}

static int64_t eval_identifier(Expr *e, const char *name)
{
    Parser *p = e->p;
    int32_t v;

    Symbol *sym = find_symbol(p, name);
    if (sym) {
        if (sym->isLabel) {
            e->labelRefs++;
            e->labelValue = sym->value;
            return sym->value;
        }
        if (e->depth > 16) {
            fail(p, "symbol '%s' is defined recursively", name);
            return 0;
        }
        Expr sub = { p, sym->expr, 0, 0, NULL, e->depth + 1, 0 };
        int64_t r = parse_sum(&sub);
        skip_ws(&sub);
        if (*sub.s != 0)
            fail(p, "bad expression in .equ %s", name);
        e->labelRefs += sub.labelRefs;
        if (sub.labelRefs)
            e->labelValue = sub.labelValue;
        if (sub.external)
            e->external = sub.external;
        return r;
    }
    if (builtin_symbol(name, &v))
        return v;
    fail(p, p->sawOtherInclude ? "undefined symbol '%s' (.include files other than MPlayDef.s are not read)"
                               : "undefined symbol '%s'", name);
    return 0;
}

/* An identifier that is neither defined in the file nor built in: an
 * external symbol, which can only be the voicegroup (mid2agb names it
 * voicegroup<...>; hand-written songs follow suit).  It evaluates to 0; the
 * caller decides whether an external reference is acceptable in its context.
 * Anything else undefined is a typo and is reported as such. */
static int64_t eval_external(Expr *e, Symbol *sym)
{
    if (strncmp(sym->name, "voicegroup", 10) != 0) {
        fail(e->p, e->p->sawOtherInclude ? "undefined symbol '%s' (.include files other than MPlayDef.s are not read)"
                                         : "undefined symbol '%s'", sym->name);
        return 0;
    }
    if (e->external && strcmp(e->external, sym->name) != 0)
        fail(e->p, "expression references two undefined symbols ('%s', '%s')", e->external, sym->name);
    e->external = sym->name;
    return 0;
}

static int64_t parse_primary(Expr *e)
{
    skip_ws(e);
    const char *s = e->s;

    if (*s == '(' || *s == '-' || *s == '+' || *s == '~') {
        if (++e->nest > 64) { fail(e->p, "expression nested too deeply"); return 0; }
        int64_t v;
        if (*s == '(') {
            e->s++;
            v = parse_sum(e);
            skip_ws(e);
            if (*e->s != ')') { fail(e->p, "missing ')'"); return 0; }
            e->s++;
        } else {
            e->s++;
            v = parse_primary(e);
            if (*s == '-') v = (int64_t)(0u - (uint64_t)v);
            else if (*s == '~') v = ~v;
        }
        e->nest--;
        return v;
    }

    if (isdigit((unsigned char)*s)) {
        char *end;
        errno = 0;
        unsigned long long u = strtoull(s, &end, 0);
        if (errno == ERANGE || u > 0xFFFFFFFFull) {
            fail(e->p, "integer constant too large");
            return 0;
        }
        e->s = end;
        return (int64_t)u;
    }
    if (isalpha((unsigned char)*s) || *s == '_' || *s == '.' || *s == '$') {
        const char *start = s;
        while (isalnum((unsigned char)*s) || *s == '_' || *s == '.' || *s == '$')
            s++;
        char name[128];
        size_t n = (size_t)(s - start);
        if (n >= sizeof(name)) { fail(e->p, "identifier too long"); return 0; }
        memcpy(name, start, n);
        name[n] = 0;
        e->s = s;
        Symbol *sym = find_symbol(e->p, name);
        int32_t bv;
        if (!sym && !builtin_symbol(name, &bv)) {
            /* Intern it so the name outlives this frame (e->external points
             * at it).  A symbol with neither a label value nor an .equ
             * expression is an external. */
            sym = add_symbol(e->p, name);
            if (!sym) return 0;
        }
        if (sym && !sym->isLabel && sym->expr == NULL)
            return eval_external(e, sym);
        return eval_identifier(e, name);
    }
    fail(e->p, "unexpected character '%c' in expression", *s ? *s : '?');
    return 0;
}

static int64_t parse_product(Expr *e)
{
    int64_t v = parse_primary(e);
    for (;;) {
        skip_ws(e);
        char op = *e->s;
        if (op != '*' && op != '/' && op != '%')
            return v;
        e->s++;
        int64_t r = parse_primary(e);
        if (op == '*') v = (int64_t)((uint64_t)v * (uint64_t)r);
        else if (r == 0) { fail(e->p, "division by zero"); return 0; }
        else if (r == -1) v = (op == '/') ? (int64_t)(0u - (uint64_t)v) : 0;
        else if (op == '/') v /= r;
        else v %= r;
    }
}

static int64_t parse_sum(Expr *e)
{
    int64_t v = parse_product(e);
    for (;;) {
        skip_ws(e);
        char op = *e->s;
        if (op != '+' && op != '-')
            return v;
        e->s++;
        int64_t r = parse_product(e);
        v = (int64_t)((op == '+') ? (uint64_t)v + (uint64_t)r : (uint64_t)v - (uint64_t)r);
    }
}

/* Evaluate a complete expression string. */
static int64_t eval_expr(Parser *p, const char *text, Expr *info)
{
    Expr e = { p, text, 0, 0, NULL, 0, 0 };
    int64_t v = parse_sum(&e);
    skip_ws(&e);
    if (*e.s != 0 && !p->failed) {
        if (strchr("<>&|^!=", *e.s))
            fail(p, "operator '%c' is not supported in expression '%s'", *e.s, text);
        else
            fail(p, "trailing characters in expression '%s'", text);
    }
    if (info)
        *info = e;
    return v;
}

/* ------------------------------------------------------------------------
 * Lines
 * ------------------------------------------------------------------------ */

static char *trim(char *s)
{
    while (*s == ' ' || *s == '\t')
        s++;
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r' || s[n - 1] == '\n'))
        s[--n] = 0;
    return s;
}

/* Split a comma-separated operand list in place; returns count. */
static int split_operands(char *s, char **out, int max)
{
    int n = 0;
    char *tok = s;
    int depth = 0;
    for (char *c = s;; c++) {
        if (*c == '(') depth++;
        else if (*c == ')') depth--;
        if ((*c == ',' && depth == 0) || *c == 0) {
            bool end = (*c == 0);
            *c = 0;
            char *t = trim(tok);
            if (*t != 0) {
                if (n == max)
                    return -1;
                out[n++] = t;
            }
            if (end)
                break;
            tok = c + 1;
        }
    }
    return n;
}

static bool add_pending_word(Parser *p, const char *expr)
{
    if (p->wordCount == p->wordCap) {
        int cap = p->wordCap ? p->wordCap * 2 : 64;
        PendingWord *w = realloc(p->words, (size_t)cap * sizeof(PendingWord));
        if (!w) { fail(p, "out of memory"); return false; }
        p->words = w;
        p->wordCap = cap;
    }
    PendingWord *w = &p->words[p->wordCount++];
    w->offset = p->song->size;
    w->expr = dupn(expr, strlen(expr));
    w->line = p->line;
    for (int i = 0; i < 4; i++)
        if (!emit_byte(p, 0))
            return false;
    return true;
}

static void parse_directive(Parser *p, char *name, char *args)
{
    if (strcmp(name, ".byte") == 0) {
        char *ops[64];
        int n = split_operands(args, ops, 64);
        if (n < 0) { fail(p, "too many operands"); return; }
        for (int i = 0; i < n; i++) {
            Expr info;
            int64_t v = eval_expr(p, ops[i], &info);
            if (p->failed) return;
            if (info.external) { fail(p, "undefined symbol '%s' in .byte", info.external); return; }
            if (v < -128 || v > 255) { fail(p, "value %lld out of range for .byte", (long long)v); return; }
            if (!emit_byte(p, (uint8_t)(v & 0xFF))) return;
        }
    } else if (strcmp(name, ".word") == 0 || strcmp(name, ".4byte") == 0) {
        char *ops[16];
        int n = split_operands(args, ops, 16);
        if (n < 0) { fail(p, "too many operands"); return; }
        for (int i = 0; i < n; i++)
            if (!add_pending_word(p, ops[i])) return;
    } else if (strcmp(name, ".equ") == 0 || strcmp(name, ".set") == 0) {
        char *ops[2];
        int n = split_operands(args, ops, 2);
        if (n != 2) { fail(p, ".equ needs a name and a value"); return; }
        Symbol *s = find_symbol(p, ops[0]);
        if (!s) {
            s = add_symbol(p, ops[0]);
            if (!s) return;
        } else if (s->isLabel) {
            fail(p, "'%s' is already a label", ops[0]);
            return;
        } else if (s->expr) {
            fail(p, "'%s' is already defined (.equ redefinition is not supported)", ops[0]);
            return;
        }
        s->expr = dupn(ops[1], strlen(ops[1]));
    } else if (strcmp(name, ".align") == 0 || strcmp(name, ".balign") == 0) {
        Expr info;
        int64_t v = eval_expr(p, args, &info);
        if (p->failed) return;
        if (info.external || info.labelRefs) { fail(p, "%s needs a constant", name); return; }
        if (strcmp(name, ".align") == 0) {
            if (v < 0 || v > 12) { fail(p, "bad alignment %lld", (long long)v); return; }
            v = (int64_t)1 << v;
        }
        if (v <= 0 || v > 4096 || (v & (v - 1))) { fail(p, "alignment must be a power of two"); return; }
        uint32_t align = (uint32_t)v;
        /* .rodata is zero-filled by the linker/assembler. */
        while (p->song->size % align != 0)
            if (!emit_byte(p, 0)) return;
    } else if (strcmp(name, ".global") == 0 || strcmp(name, ".globl") == 0) {
        char *ops[4];
        int n = split_operands(args, ops, 4);
        if (n >= 1 && !p->globalName)
            p->globalName = dupn(ops[0], strlen(ops[0]));
    } else if (strcmp(name, ".include") == 0) {
        /* The MPlayDef.s vocabulary is built in; other includes are not
         * read, so a song that needs their symbols fails on the first use. */
        if (!strstr(args, "MPlayDef.s"))
            p->sawOtherInclude = true;
    } else if (strcmp(name, ".section") == 0
               || strcmp(name, ".text") == 0 || strcmp(name, ".end") == 0
               || strcmp(name, ".syntax") == 0 || strcmp(name, ".arm") == 0
               || strcmp(name, ".thumb") == 0 || strcmp(name, ".size") == 0
               || strcmp(name, ".type") == 0) {
        /* Nothing to emit. */
    } else {
        fail(p, "unsupported directive '%s'", name);
    }
}

static void parse_line(Parser *p, char *line);

static void parse_line(Parser *p, char *line)
{
    /* Strip comments. */
    char *at = strchr(line, '@');
    if (at)
        *at = 0;

    /* On ARM, GAS uses ';' as a statement separator. */
    char *semi = strchr(line, ';');
    if (semi) {
        *semi = 0;
        parse_line(p, line);
        if (!p->failed)
            parse_line(p, semi + 1);
        return;
    }

    char *s = trim(line);
    if (*s == 0)
        return;

    /* Labels: `name:` possibly followed by more on the same line. */
    while (isalpha((unsigned char)*s) || *s == '_' || *s == '.' || *s == '$') {
        char *c = s;
        while (isalnum((unsigned char)*c) || *c == '_' || *c == '.' || *c == '$')
            c++;
        if (*c != ':')
            break;
        *c = 0;
        Symbol *sym = find_symbol(p, s);
        if (sym && (sym->isLabel || sym->expr)) {
            fail(p, "label '%s' defined twice", s);
            return;
        }
        if (!sym) {
            sym = add_symbol(p, s);
            if (!sym) return;
        }
        sym->isLabel = true;
        sym->value = (int32_t)p->song->size;
        s = trim(c + 1);
        if (*s == 0)
            return;
    }

    if (*s == '.') {
        char *c = s;
        while (*c && *c != ' ' && *c != '\t')
            c++;
        char *args = *c ? trim(c + 1) : c;
        *c = 0;
        parse_directive(p, s, args);
        return;
    }

    fail(p, "unexpected text '%s'", s);
}

static uint32_t read_u32(const uint8_t *d)
{
    return (uint32_t)d[0] | ((uint32_t)d[1] << 8) | ((uint32_t)d[2] << 16) | ((uint32_t)d[3] << 24);
}

static void write_u32(uint8_t *d, uint32_t v)
{
    d[0] = (uint8_t)v;
    d[1] = (uint8_t)(v >> 8);
    d[2] = (uint8_t)(v >> 16);
    d[3] = (uint8_t)(v >> 24);
}

static void resolve_words(Parser *p)
{
    M4ASong *song = p->song;
    song->relocs = calloc((size_t)(p->wordCount ? p->wordCount : 1), sizeof(M4ASongReloc));
    if (!song->relocs) { fail(p, "out of memory"); return; }

    for (int i = 0; i < p->wordCount && !p->failed; i++) {
        PendingWord *w = &p->words[i];
        p->line = w->line;
        Expr info;
        int64_t v = eval_expr(p, w->expr, &info);
        if (p->failed) return;

        M4ASongReloc *r = &song->relocs[song->relocCount];
        if (info.external) {
            if (info.labelRefs) { fail(p, "cannot mix a label and '%s' in .word", info.external); return; }
            r->offset = w->offset;
            r->target = M4A_SONG_RELOC_VOICEGROUP;
            r->addend = (int32_t)v;
            song->relocCount++;
            /* The external symbol names the voicegroup. */
            const char *vg = info.external;
            if (strncmp(vg, "voicegroup_", 11) == 0)
                vg += 11;
            if (song->voicegroup[0] && strcmp(song->voicegroup, vg) != 0) {
                fail(p, "song references two external symbols ('%s', '%s')", song->voicegroup, vg);
                return;
            }
            snprintf(song->voicegroup, sizeof(song->voicegroup), "%s", vg);
            write_u32(song->data + w->offset, 0);
        } else if (info.labelRefs == 1) {
            int64_t addend = v - info.labelValue;
            if (addend < -0x8000 || addend > 0x8000) {
                fail(p, ".word expression '%s' is not label + small constant", w->expr);
                return;
            }
            r->offset = w->offset;
            r->target = info.labelValue;
            r->addend = (int32_t)addend;
            song->relocCount++;
            write_u32(song->data + w->offset, (uint32_t)v);
        } else if (info.labelRefs == 0) {
            if (v < -0x80000000ll || v > 0xFFFFFFFFll) {
                fail(p, "value %lld out of range for .word", (long long)v);
                return;
            }
            write_u32(song->data + w->offset, (uint32_t)v);
        } else {
            fail(p, ".word expression references more than one label");
            return;
        }
    }
}

static void read_header(Parser *p)
{
    M4ASong *song = p->song;
    p->line = 0; /* header problems are not tied to a source line */
    if (!p->globalName) { fail(p, "no .global song label"); return; }
    Symbol *h = find_symbol(p, p->globalName);
    if (!h || !h->isLabel) { fail(p, "song label '%s' is not defined", p->globalName); return; }

    song->headerOffset = (uint32_t)h->value;
    snprintf(song->name, sizeof(song->name), "%s", p->globalName);
    if (song->headerOffset + 8 > song->size) { fail(p, "song header is truncated"); return; }

    const uint8_t *d = song->data + song->headerOffset;
    song->trackCount = d[0];
    song->blockCount = d[1];
    song->priority = d[2];
    song->reverb = d[3];
    if (song->trackCount > M4A_SONG_MAX_TRACKS) {
        fail(p, "song has %u tracks (max %d)", song->trackCount, M4A_SONG_MAX_TRACKS);
        return;
    }
    if (song->headerOffset + 8 + 4u * song->trackCount > song->size) {
        fail(p, "song header is truncated");
        return;
    }
    for (int i = 0; i < song->trackCount; i++) {
        uint32_t wordOff = song->headerOffset + 8 + 4 * i;
        uint32_t off = read_u32(song->data + wordOff);
        bool isLabel = false;
        for (int r = 0; r < song->relocCount; r++)
            if (song->relocs[r].offset == wordOff && song->relocs[r].target != M4A_SONG_RELOC_VOICEGROUP)
                isLabel = true;
        if (!isLabel) { fail(p, "track %d pointer is not a label", i + 1); return; }
        if (off >= song->size) { fail(p, "track %d points outside the song", i + 1); return; }
        song->trackOffsets[i] = off;
    }
}

static void parser_free(Parser *p)
{
    for (int i = 0; i < p->symCount; i++) {
        free(p->syms[i].name);
        free(p->syms[i].expr);
    }
    free(p->syms);
    for (int i = 0; i < p->wordCount; i++)
        free(p->words[i].expr);
    free(p->words);
    free(p->globalName);
}

M4ASong *m4a_song_parse(const char *text, size_t len, char *err, size_t errLen)
{
    if (err && errLen)
        err[0] = 0;

    Parser p;
    memset(&p, 0, sizeof(p));
    p.err = err;
    p.errLen = errLen;
    p.song = calloc(1, sizeof(M4ASong));
    if (!p.song) {
        if (err && errLen) snprintf(err, errLen, "out of memory");
        return NULL;
    }

    /* Copy so lines can be split in place. */
    char *buf = dupn(text, len);
    if (!buf) {
        fail(&p, "out of memory");
    } else if (memchr(buf, 0, len)) {
        p.line = 0;
        fail(&p, "file contains a NUL byte; not a text song file");
    } else {
        char *cur = buf;
        p.line = 0;
        while (cur < buf + len && !p.failed) {
            char *nl = memchr(cur, '\n', (size_t)(buf + len - cur));
            if (nl) *nl = 0;
            p.line++;
            parse_line(&p, cur);
            if (!nl) break;
            cur = nl + 1;
        }
    }
    if (!p.failed) resolve_words(&p);
    if (!p.failed) read_header(&p);

    free(buf);
    parser_free(&p);

    if (p.failed) {
        m4a_song_free(p.song);
        return NULL;
    }
    return p.song;
}

M4ASong *m4a_song_load(const char *path, char *err, size_t errLen)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        if (err && errLen) snprintf(err, errLen, "cannot open %s", path);
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0) { fclose(f); if (err && errLen) snprintf(err, errLen, "cannot read %s", path); return NULL; }
    char *buf = malloc((size_t)n + 1);
    if (!buf) { fclose(f); if (err && errLen) snprintf(err, errLen, "out of memory"); return NULL; }
    size_t got = fread(buf, 1, (size_t)n, f);
    fclose(f);
    buf[got] = 0;
    M4ASong *song = m4a_song_parse(buf, got, err, errLen);
    free(buf);
    return song;
}

void m4a_song_free(M4ASong *song)
{
    if (!song)
        return;
    free(song->data);
    free(song->relocs);
    free(song);
}

void m4a_song_relocate(const M4ASong *song, uint32_t baseAddr, uint32_t voicegroupAddr,
                       uint8_t *out)
{
    memcpy(out, song->data, song->size);
    for (int i = 0; i < song->relocCount; i++) {
        const M4ASongReloc *r = &song->relocs[i];
        uint32_t v;
        if (r->target == M4A_SONG_RELOC_VOICEGROUP)
            v = voicegroupAddr + (uint32_t)r->addend;
        else
            v = baseAddr + (uint32_t)r->target + (uint32_t)r->addend;
        write_u32(out + r->offset, v);
    }
}
