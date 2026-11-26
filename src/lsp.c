/*
 * VORTEX LANGUAGE SERVER PROTOCOL (vortex lsp)
 * JSON-RPC 2.0 over stdio reusing Vortex lexer + parser modules + ir_region_verify:
 * - Real-time diagnostics (syntax, type, unsafe rules, region escape, imports)
 * - Go-to-definition (local symbols + cross-file imported/package modules)
 * - Hover documentation (functions, structs, fields, variables, builtins, keywords)
 * - Auto-completion (identifiers, dot-fields, keywords, builtins, stdlib imports)
 */

#include "vortex.h"

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <direct.h>
#define lsp_getcwd _getcwd
#else
#define lsp_getcwd getcwd
#endif

/* ========================================================================= */
/* 1. DYNAMIC STRING BUFFER & JSON PRIMITIVES                                */
/* ========================================================================= */

typedef struct {
    char* data;
    size_t len;
    size_t cap;
} LspStr;

static void lstr_reserve(LspStr* b, size_t extra) {
    if (b->len + extra + 1 <= b->cap) return;
    size_t ncap = (b->cap == 0) ? 1024 : (b->cap * 2);
    while (ncap < b->len + extra + 1) ncap *= 2;
    char* nb = (char*)realloc(b->data, ncap);
    if (!nb) return;
    b->data = nb;
    b->cap = ncap;
}

static void lstr_addn(LspStr* b, const char* s, size_t n) {
    if (!s || n == 0) return;
    lstr_reserve(b, n);
    if (b->len + n + 1 <= b->cap) {
        memcpy(b->data + b->len, s, n);
        b->len += n;
        b->data[b->len] = '\0';
    }
}

static void lstr_adds(LspStr* b, const char* s) {
    if (s) lstr_addn(b, s, strlen(s));
}

static void lstr_addc(LspStr* b, char c) {
    lstr_addn(b, &c, 1);
}

static void lstr_add_json_str(LspStr* b, const char* s) {
    lstr_addc(b, '"');
    for (size_t i = 0; s && s[i]; i++) {
        unsigned char c = (unsigned char)s[i];
        switch (c) {
            case '"':  lstr_adds(b, "\\\""); break;
            case '\\': lstr_adds(b, "\\\\"); break;
            case '\b': lstr_adds(b, "\\b"); break;
            case '\f': lstr_adds(b, "\\f"); break;
            case '\n': lstr_adds(b, "\\n"); break;
            case '\r': lstr_adds(b, "\\r"); break;
            case '\t': lstr_adds(b, "\\t"); break;
            default:
                if (c < 0x20) {
                    char esc[8];
                    snprintf(esc, sizeof(esc), "\\u%04x", (unsigned int)c);
                    lstr_adds(b, esc);
                } else {
                    lstr_addc(b, (char)c);
                }
                break;
        }
    }
    lstr_addc(b, '"');
}

/* --- Minimal JSON navigation over received JSON-RPC frames --- */
static const char* json_skip_ws(const char* p) {
    while (p && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) p++;
    return p;
}

static const char* json_skip_string(const char* p) {
    if (!p || *p != '"') return p;
    p++;
    while (*p && *p != '"') {
        if (*p == '\\' && *(p + 1)) p += 2;
        else p++;
    }
    if (*p == '"') p++;
    return p;
}

static const char* json_skip_value(const char* p) {
    p = json_skip_ws(p);
    if (!p || !*p) return p;
    if (*p == '"') return json_skip_string(p);
    if (*p == '{' || *p == '[') {
        char open_ch = *p;
        char close_ch = (open_ch == '{') ? '}' : ']';
        int depth = 1;
        p++;
        while (*p && depth > 0) {
            if (*p == '"') {
                p = json_skip_string(p);
                continue;
            }
            if (*p == open_ch) depth++;
            else if (*p == close_ch) depth--;
            p++;
        }
        return p;
    }
    while (*p && *p != ',' && *p != '}' && *p != ']' &&
           *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n') {
        p++;
    }
    return p;
}

static bool json_eq_string_token(const char* p, const char* target) {
    if (!p || *p != '"') return false;
    p++;
    size_t tlen = strlen(target);
    size_t idx = 0;
    while (*p && *p != '"') {
        char ch = *p;
        if (ch == '\\' && *(p + 1)) {
            p++;
            ch = *p;
        }
        if (idx >= tlen || ch != target[idx]) return false;
        idx++;
        p++;
    }
    return (*p == '"' && idx == tlen);
}

static const char* json_find_key(const char* obj, const char* key) {
    obj = json_skip_ws(obj);
    if (!obj || *obj != '{') return NULL;
    const char* p = json_skip_ws(obj + 1);
    while (*p && *p != '}') {
        if (*p != '"') break;
        const char* kstart = p;
        const char* kend = json_skip_string(kstart);
        p = json_skip_ws(kend);
        if (*p != ':') break;
        p = json_skip_ws(p + 1);
        if (json_eq_string_token(kstart, key)) {
            return p;
        }
        p = json_skip_value(p);
        p = json_skip_ws(p);
        if (*p == ',') p = json_skip_ws(p + 1);
        else break;
    }
    return NULL;
}

static const char* json_array_first(const char* arr) {
    arr = json_skip_ws(arr);
    if (!arr || *arr != '[') return NULL;
    const char* p = json_skip_ws(arr + 1);
    if (*p == ']' || !*p) return NULL;
    return p;
}

static char* json_dup_string(const char* val) {
    val = json_skip_ws(val);
    if (!val || *val != '"') return NULL;
    val++;
    LspStr out = {0};
    lstr_reserve(&out, 64);
    out.data[0] = '\0';
    while (*val && *val != '"') {
        if (*val == '\\' && *(val + 1)) {
            val++;
            switch (*val) {
                case 'n':  lstr_addc(&out, '\n'); break;
                case 'r':  lstr_addc(&out, '\r'); break;
                case 't':  lstr_addc(&out, '\t'); break;
                case 'b':  lstr_addc(&out, '\b'); break;
                case 'f':  lstr_addc(&out, '\f'); break;
                case '\\': lstr_addc(&out, '\\'); break;
                case '"':  lstr_addc(&out, '"'); break;
                case '/':  lstr_addc(&out, '/'); break;
                case 'u': {
                    unsigned int cp = 0;
                    if (isxdigit((unsigned char)val[1]) && isxdigit((unsigned char)val[2]) &&
                        isxdigit((unsigned char)val[3]) && isxdigit((unsigned char)val[4])) {
                        char hex4[5] = { val[1], val[2], val[3], val[4], '\0' };
                        cp = (unsigned int)strtoul(hex4, NULL, 16);
                        val += 4;
                    }
                    if (cp < 0x80U) lstr_addc(&out, (char)cp);
                    else if (cp < 0x800U) {
                        lstr_addc(&out, (char)(0xC0U | (cp >> 6)));
                        lstr_addc(&out, (char)(0x80U | (cp & 0x3FU)));
                    } else {
                        lstr_addc(&out, (char)(0xE0U | (cp >> 12)));
                        lstr_addc(&out, (char)(0x80U | ((cp >> 6) & 0x3FU)));
                        lstr_addc(&out, (char)(0x80U | (cp & 0x3FU)));
                    }
                    break;
                }
                default: lstr_addc(&out, *val); break;
            }
            val++;
        } else {
            lstr_addc(&out, *val++);
        }
    }
    return out.data;
}

static bool json_get_int(const char* val, int64_t* out) {
    val = json_skip_ws(val);
    if (!val || (!isdigit((unsigned char)*val) && *val != '-')) return false;
    char* endp = NULL;
    int64_t v = strtoll(val, &endp, 10);
    if (endp == val) return false;
    *out = v;
    return true;
}

static bool json_extract_raw_id(const char* root, char* out, size_t cap) {
    const char* id_pos = json_find_key(root, "id");
    if (!id_pos) return false;
    const char* end = json_skip_value(id_pos);
    size_t n = (size_t)(end - id_pos);
    if (n == 0 || n >= cap) return false;
    memcpy(out, id_pos, n);
    out[n] = '\0';
    return true;
}

/* ========================================================================= */
/* 2. CONTENT-LENGTH FRAMED STDIO TRANSPORT                                  */
/* ========================================================================= */

static int lsp_fd_read(void* buf, size_t len) {
#ifdef _WIN32
    HANDLE hIn = GetStdHandle(STD_INPUT_HANDLE);
    if (hIn == INVALID_HANDLE_VALUE || hIn == NULL) return -1;
    DWORD nr = 0;
    if (!ReadFile(hIn, buf, (DWORD)len, &nr, NULL)) return -1;
    return (int)nr;
#else
    return (int)read(STDIN_FILENO, buf, len);
#endif
}

static bool lsp_fd_write_all(const char* buf, size_t len) {
    size_t off = 0;
    while (off < len) {
#ifdef _WIN32
        HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
        if (hOut == INVALID_HANDLE_VALUE || hOut == NULL) return false;
        DWORD nw = 0;
        if (!WriteFile(hOut, buf + off, (DWORD)(len - off), &nw, NULL) || nw == 0) return false;
        off += (size_t)nw;
#else
        ssize_t w = write(STDOUT_FILENO, buf + off, len - off);
        if (w <= 0) return false;
        off += (size_t)w;
#endif
    }
    return true;
}

static bool lsp_read_line(char* line, size_t cap) {
    size_t pos = 0;
    while (pos + 1 < cap) {
        char ch = 0;
        int r = lsp_fd_read(&ch, 1);
        if (r <= 0) {
            line[pos] = '\0';
            return pos > 0;
        }
        if (ch == '\n') {
            break;
        }
        if (ch != '\r') {
            line[pos++] = ch;
        }
    }
    line[pos] = '\0';
    return true;
}

static bool lsp_read_message(char** out_json, size_t* out_len) {
    *out_json = NULL;
    *out_len = 0;

    char line[512];
    size_t content_len = 0;
    bool got_header = false;

    while (lsp_read_line(line, sizeof(line))) {
        size_t l = strlen(line);
        if (l == 0) {
            if (got_header) break;
            continue;
        }
        got_header = true;
        /* Case-insensitive Content-Length prefix */
        if (l > 15) {
            char hdr_low[16];
            for (int i = 0; i < 15; i++) hdr_low[i] = (char)tolower((unsigned char)line[i]);
            hdr_low[15] = '\0';
            if (strcmp(hdr_low, "content-length:") == 0) {
                content_len = (size_t)strtoull(line + 15, NULL, 10);
            }
        }
    }

    if (!got_header || content_len == 0 || content_len > (16U * 1024U * 1024U)) {
        if (getenv("VORTEX_LSP_DEBUG")) {
            fprintf(stderr, "[LSP_DBG] read_message abort: got_header=%d content_len=%zu\n",
                    got_header ? 1 : 0, content_len);
            fflush(stderr);
        }
        return false;
    }

    char* buf = (char*)malloc(content_len + 1);
    if (!buf) return false;

    size_t total = 0;
    while (total < content_len) {
        size_t want = content_len - total;
        if (want > 65536U) want = 65536U;
        int r = lsp_fd_read(buf + total, want);
        if (r <= 0) {
            free(buf);
            return false;
        }
        total += (size_t)r;
    }
    buf[content_len] = '\0';
    *out_json = buf;
    *out_len = content_len;
    return true;
}

static void lsp_send_json(const char* json) {
    if (!json) return;
    size_t len = strlen(json);
    char hdr[64];
    int hlen = snprintf(hdr, sizeof(hdr), "Content-Length: %zu\r\n\r\n", len);
    if (hlen > 0) {
        lsp_fd_write_all(hdr, (size_t)hlen);
    }
    lsp_fd_write_all(json, len);
}

/* ========================================================================= */
/* 3. URI <-> PATH HELPERS                                                   */
/* ========================================================================= */

static int hex_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void lsp_uri_to_path(const char* uri, char* out, size_t cap) {
    out[0] = '\0';
    if (!uri || !uri[0]) return;
    const char* p = uri;
    if (strncmp(p, "file://", 7) == 0) {
        p += 7;
        if (strncmp(p, "localhost", 9) == 0) p += 9;
        /* Windows: file:///C:/... -> strip leading slash before C: */
        if (p[0] == '/' && isalpha((unsigned char)p[1]) &&
            (p[2] == ':' || (p[2] == '%' && p[3] == '3' && (p[4] == 'A' || p[4] == 'a')))) {
            p++;
        }
    }
    size_t o = 0;
    while (*p && o + 1 < cap) {
        if (*p == '%' && hex_val(p[1]) >= 0 && hex_val(p[2]) >= 0) {
            out[o++] = (char)((hex_val(p[1]) << 4) | hex_val(p[2]));
            p += 3;
        } else {
            char c = *p++;
            if (c == '\\') c = '/';
            out[o++] = c;
        }
    }
    out[o] = '\0';
}

static void lsp_path_to_uri(const char* path, char* out, size_t cap) {
    char full[512] = {0};
    if (!path || !path[0]) {
        strncpy(out, "file:///unknown.vx", cap - 1);
        out[cap - 1] = '\0';
        return;
    }
    bool is_abs = (path[0] == '/' || path[0] == '\\' || (isalpha((unsigned char)path[0]) && path[1] == ':'));
    if (is_abs) {
        strncpy(full, path, sizeof(full) - 1);
    } else {
        char cwd[512] = {0};
        if (lsp_getcwd(cwd, sizeof(cwd))) {
            snprintf(full, sizeof(full), "%s/%s", cwd, path);
        } else {
            strncpy(full, path, sizeof(full) - 1);
        }
    }
    for (size_t i = 0; full[i]; i++) {
        if (full[i] == '\\') full[i] = '/';
    }

    LspStr s = {0};
    lstr_adds(&s, (full[0] == '/') ? "file://" : "file:///");
    for (size_t i = 0; full[i]; i++) {
        char c = full[i];
        if (c == ' ') lstr_adds(&s, "%20");
        else lstr_addc(&s, c);
    }
    strncpy(out, s.data ? s.data : "file:///", cap - 1);
    out[cap - 1] = '\0';
    free(s.data);
}

/* ========================================================================= */
/* 4. RESILIENT SYMBOL INDEXER, HOVER, DEFINITION & COMPLETION ENGINE        */
/* ========================================================================= */

typedef enum {
    LSYM_FN = 3,
    LSYM_FIELD = 5,
    LSYM_VAR = 6,
    LSYM_STRUCT = 22
} LspSymKind;

typedef struct {
    char name[64];
    LspSymKind kind;
    int line;       /* 1-based */
    int col;        /* 1-based */
    char file[512];
    char uri[600];
    char detail[256];
    char parent[64];
    char scope_fn[64];
    char type_name[64];
} LspSymbol;

typedef struct {
    char* uri;
    char path[512];
    char* text;
} LspDocument;

#define LSP_MAX_DOCS 32
#define LSP_MAX_SYMS 1024
#define LSP_MAX_INDEXED_FILES 64

static LspDocument g_docs[LSP_MAX_DOCS];
static int g_doc_count = 0;

static LspSymbol g_syms[LSP_MAX_SYMS];
static int g_sym_count = 0;
static char g_indexed_files[LSP_MAX_INDEXED_FILES][512];
static int g_indexed_file_count = 0;

static void lsp_add_sym(const char* name, LspSymKind kind, int line, int col,
                        const char* file, const char* uri, const char* detail,
                        const char* parent, const char* scope_fn, const char* type_name) {
    if (!name || !name[0] || g_sym_count >= LSP_MAX_SYMS) return;
    LspSymbol* s = &g_syms[g_sym_count++];
    memset(s, 0, sizeof(*s));
    strncpy(s->name, name, sizeof(s->name) - 1);
    s->kind = kind;
    s->line = (line > 0) ? line : 1;
    s->col = (col > 0) ? col : 1;
    if (file) strncpy(s->file, file, sizeof(s->file) - 1);
    if (uri) strncpy(s->uri, uri, sizeof(s->uri) - 1);
    if (detail) strncpy(s->detail, detail, sizeof(s->detail) - 1);
    if (parent) strncpy(s->parent, parent, sizeof(s->parent) - 1);
    if (scope_fn) strncpy(s->scope_fn, scope_fn, sizeof(s->scope_fn) - 1);
    if (type_name) strncpy(s->type_name, type_name, sizeof(s->type_name) - 1);
}

typedef struct {
    char text[128];
    int line;
    int col;
    int kind; /* 0=eof, 1=ident, 2=str, 3=num, 4=punct */
} ScanTok;

typedef struct {
    const char* src;
    size_t pos;
    size_t len;
    int line;
    int col;
    ScanTok cur;
} SafeScan;

static void scan_next(SafeScan* s) {
    while (s->pos < s->len) {
        char c = s->src[s->pos];
        if (c == ' ' || c == '\t' || c == '\r') {
            s->pos++; s->col++; continue;
        }
        if (c == '\n') {
            s->pos++; s->line++; s->col = 1; continue;
        }
        if (c == '/' && s->pos + 1 < s->len && s->src[s->pos + 1] == '/') {
            while (s->pos < s->len && s->src[s->pos] != '\n') s->pos++;
            continue;
        }
        break;
    }
    if (s->pos >= s->len) {
        s->cur.kind = 0;
        s->cur.text[0] = '\0';
        s->cur.line = s->line;
        s->cur.col = s->col;
        return;
    }
    s->cur.line = s->line;
    s->cur.col = s->col;
    char c = s->src[s->pos];

    if (isalpha((unsigned char)c) || c == '_') {
        size_t st = s->pos;
        while (s->pos < s->len && (isalnum((unsigned char)s->src[s->pos]) || s->src[s->pos] == '_')) {
            s->pos++; s->col++;
        }
        size_t n = s->pos - st;
        if (n >= sizeof(s->cur.text)) n = sizeof(s->cur.text) - 1;
        memcpy(s->cur.text, s->src + st, n);
        s->cur.text[n] = '\0';
        s->cur.kind = 1;
        return;
    }
    if (isdigit((unsigned char)c)) {
        size_t st = s->pos;
        while (s->pos < s->len && (isxdigit((unsigned char)s->src[s->pos]) || s->src[s->pos] == '.' ||
               s->src[s->pos] == 'x' || s->src[s->pos] == 'X')) {
            s->pos++; s->col++;
        }
        size_t n = s->pos - st;
        if (n >= sizeof(s->cur.text)) n = sizeof(s->cur.text) - 1;
        memcpy(s->cur.text, s->src + st, n);
        s->cur.text[n] = '\0';
        s->cur.kind = 3;
        return;
    }
    if (c == '"') {
        s->pos++; s->col++;
        size_t o = 0;
        while (s->pos < s->len && s->src[s->pos] != '"') {
            if (s->src[s->pos] == '\\' && s->pos + 1 < s->len) s->pos++;
            if (o + 1 < sizeof(s->cur.text)) s->cur.text[o++] = s->src[s->pos];
            s->pos++; s->col++;
        }
        if (s->pos < s->len && s->src[s->pos] == '"') { s->pos++; s->col++; }
        s->cur.text[o] = '\0';
        s->cur.kind = 2;
        return;
    }
    if (c == '-' && s->pos + 1 < s->len && s->src[s->pos + 1] == '>') {
        strcpy(s->cur.text, "->");
        s->pos += 2; s->col += 2;
        s->cur.kind = 4;
        return;
    }
    s->cur.text[0] = c;
    s->cur.text[1] = '\0';
    s->pos++; s->col++;
    s->cur.kind = 4;
}

static void scan_type_text(SafeScan* sc, char* out, size_t cap) {
    out[0] = '\0';
    if (strcmp(sc->cur.text, "*") == 0) {
        strncat(out, "*", cap - strlen(out) - 1);
        scan_next(sc);
    }
    if (strcmp(sc->cur.text, "struct") == 0) {
        strncat(out, "struct ", cap - strlen(out) - 1);
        scan_next(sc);
    }
    if (sc->cur.kind == 1) {
        strncat(out, sc->cur.text, cap - strlen(out) - 1);
        scan_next(sc);
    }
    if (strcmp(sc->cur.text, "?") == 0) {
        strncat(out, "?", cap - strlen(out) - 1);
        scan_next(sc);
    }
}

static void lsp_index_source_rec(const char* text, const char* file_path, const char* file_uri, int depth) {
    if (!text || depth > 8) return;
    if (file_path && file_path[0]) {
        for (int i = 0; i < g_indexed_file_count; i++) {
            if (strcmp(g_indexed_files[i], file_path) == 0) return;
        }
        if (g_indexed_file_count < LSP_MAX_INDEXED_FILES) {
            strncpy(g_indexed_files[g_indexed_file_count++], file_path, 511);
        }
    }

    SafeScan sc;
    memset(&sc, 0, sizeof(sc));
    sc.src = text;
    sc.len = strlen(text);
    sc.line = 1;
    sc.col = 1;
    scan_next(&sc);

    int brace_depth = 0;
    char current_fn[64] = "__main__";
    int fn_brace_depth = -1;

    while (sc.cur.kind != 0) {
        if (sc.cur.kind == 4 && strcmp(sc.cur.text, "{") == 0) {
            brace_depth++;
            scan_next(&sc);
            continue;
        }
        if (sc.cur.kind == 4 && strcmp(sc.cur.text, "}") == 0) {
            if (brace_depth > 0) brace_depth--;
            if (fn_brace_depth >= 0 && brace_depth < fn_brace_depth) {
                strcpy(current_fn, "__main__");
                fn_brace_depth = -1;
            }
            scan_next(&sc);
            continue;
        }

        if (sc.cur.kind == 1 && strcmp(sc.cur.text, "import") == 0 && brace_depth == 0) {
            scan_next(&sc);
            char spec[256] = {0};
            if (sc.cur.kind == 2) {
                strncpy(spec, sc.cur.text, sizeof(spec) - 1);
                scan_next(&sc);
            } else if (sc.cur.kind == 1) {
                strncpy(spec, sc.cur.text, sizeof(spec) - 1);
                scan_next(&sc);
                while (strcmp(sc.cur.text, "/") == 0 || strcmp(sc.cur.text, ".") == 0 || strcmp(sc.cur.text, "-") == 0) {
                    strncat(spec, sc.cur.text, sizeof(spec) - strlen(spec) - 1);
                    scan_next(&sc);
                    if (sc.cur.kind == 1 || sc.cur.kind == 3) {
                        strncat(spec, sc.cur.text, sizeof(spec) - strlen(spec) - 1);
                        scan_next(&sc);
                    }
                }
            }
            char resolved[512] = {0};
            if (spec[0] && vortex_resolve_import(spec, file_path, resolved, sizeof(resolved))) {
                FILE* rf = fopen(resolved, "rb");
                if (rf) {
                    fseek(rf, 0, SEEK_END);
                    long rsz = ftell(rf);
                    fseek(rf, 0, SEEK_SET);
                    if (rsz > 0 && rsz < (4L * 1024L * 1024L)) {
                        char* mbuf = (char*)malloc((size_t)rsz + 1);
                        if (mbuf) {
                            size_t rn = fread(mbuf, 1, (size_t)rsz, rf);
                            mbuf[rn] = '\0';
                            char imp_uri[600];
                            lsp_path_to_uri(resolved, imp_uri, sizeof(imp_uri));
                            lsp_index_source_rec(mbuf, resolved, imp_uri, depth + 1);
                            free(mbuf);
                        }
                    }
                    fclose(rf);
                }
            }
            continue;
        }

        bool is_unsafe = false;
        bool is_extern = false;
        char extern_lib[64] = {0};
        if (sc.cur.kind == 1 && strcmp(sc.cur.text, "unsafe") == 0) {
            scan_next(&sc);
            if (sc.cur.kind == 1 && strcmp(sc.cur.text, "fn") == 0) {
                is_unsafe = true;
            } else {
                continue;
            }
        }
        if (sc.cur.kind == 1 && strcmp(sc.cur.text, "extern") == 0) {
            scan_next(&sc);
            if (sc.cur.kind == 2) {
                strncpy(extern_lib, sc.cur.text, sizeof(extern_lib) - 1);
                scan_next(&sc);
            }
            if (sc.cur.kind == 1 && strcmp(sc.cur.text, "fn") == 0) {
                is_extern = true;
            } else {
                continue;
            }
        }

        if (sc.cur.kind == 1 && strcmp(sc.cur.text, "fn") == 0) {
            scan_next(&sc);
            if (sc.cur.kind == 1) {
                char fn_name[64];
                strncpy(fn_name, sc.cur.text, sizeof(fn_name) - 1);
                fn_name[sizeof(fn_name) - 1] = '\0';
                int fn_line = sc.cur.line;
                int fn_col = sc.cur.col;
                scan_next(&sc);

                char sig[256];
                if (is_extern) snprintf(sig, sizeof(sig), "extern \"%s\" fn %s(", extern_lib, fn_name);
                else if (is_unsafe) snprintf(sig, sizeof(sig), "unsafe fn %s(", fn_name);
                else snprintf(sig, sizeof(sig), "fn %s(", fn_name);

                char pnames[16][64], ptypes[16][64];
                int plines[16], pcols[16];
                int pcnt = 0;

                if (strcmp(sc.cur.text, "(") == 0) {
                    scan_next(&sc);
                    bool first_p = true;
                    while (sc.cur.kind != 0 && strcmp(sc.cur.text, ")") != 0 && strcmp(sc.cur.text, "{") != 0) {
                        if (strcmp(sc.cur.text, ",") == 0) { scan_next(&sc); continue; }
                        if (strcmp(sc.cur.text, ".") == 0) {
                            if (!first_p) strncat(sig, ", ", sizeof(sig) - strlen(sig) - 1);
                            strncat(sig, "...", sizeof(sig) - strlen(sig) - 1);
                            while (strcmp(sc.cur.text, ".") == 0) scan_next(&sc);
                            first_p = false;
                            continue;
                        }
                        if (sc.cur.kind != 1) { scan_next(&sc); continue; }
                        char pn[64] = {0}, pt[64] = "i64";
                        strncpy(pn, sc.cur.text, sizeof(pn) - 1);
                        int pl = sc.cur.line, pc = sc.cur.col;
                        scan_next(&sc);
                        if (strcmp(sc.cur.text, ":") == 0) {
                            scan_next(&sc);
                            scan_type_text(&sc, pt, sizeof(pt));
                        }
                        if (!first_p) strncat(sig, ", ", sizeof(sig) - strlen(sig) - 1);
                        strncat(sig, pn, sizeof(sig) - strlen(sig) - 1);
                        strncat(sig, ": ", sizeof(sig) - strlen(sig) - 1);
                        strncat(sig, pt, sizeof(sig) - strlen(sig) - 1);
                        first_p = false;
                        if (pcnt < 16) {
                            strncpy(pnames[pcnt], pn, 63);
                            strncpy(ptypes[pcnt], pt, 63);
                            plines[pcnt] = pl;
                            pcols[pcnt] = pc;
                            pcnt++;
                        }
                    }
                    if (strcmp(sc.cur.text, ")") == 0) scan_next(&sc);
                }
                strncat(sig, ")", sizeof(sig) - strlen(sig) - 1);
                char ret_t[64] = "i64";
                if (strcmp(sc.cur.text, "->") == 0) {
                    scan_next(&sc);
                    scan_type_text(&sc, ret_t, sizeof(ret_t));
                    strncat(sig, " -> ", sizeof(sig) - strlen(sig) - 1);
                    strncat(sig, ret_t, sizeof(sig) - strlen(sig) - 1);
                } else if (is_extern) {
                    strcpy(ret_t, "void");
                }
                lsp_add_sym(fn_name, LSYM_FN, fn_line, fn_col, file_path, file_uri, sig, "", "", ret_t);
                for (int p = 0; p < pcnt; p++) {
                    char pdet[256];
                    snprintf(pdet, sizeof(pdet), "(parameter) %.63s: %.63s", pnames[p], ptypes[p]);
                    lsp_add_sym(pnames[p], LSYM_VAR, plines[p], pcols[p], file_path, file_uri,
                                pdet, "", fn_name, ptypes[p]);
                }
                if (!is_extern && strcmp(sc.cur.text, "{") == 0) {
                    vx_str_copy(current_fn, sizeof(current_fn), fn_name);
                    fn_brace_depth = brace_depth + 1;
                    brace_depth++;
                    scan_next(&sc);
                }
            }
            continue;
        }

        if (sc.cur.kind == 1 && strcmp(sc.cur.text, "struct") == 0 && brace_depth == 0) {
            scan_next(&sc);
            if (sc.cur.kind == 1) {
                char sname[64];
                strncpy(sname, sc.cur.text, sizeof(sname) - 1);
                sname[sizeof(sname) - 1] = '\0';
                int sl = sc.cur.line, scol = sc.cur.col;
                scan_next(&sc);

                char sdet[256];
                snprintf(sdet, sizeof(sdet), "struct %s { ", sname);
                bool first_f = true;
                if (strcmp(sc.cur.text, "{") == 0) {
                    scan_next(&sc);
                    while (sc.cur.kind != 0 && strcmp(sc.cur.text, "}") != 0) {
                        if (strcmp(sc.cur.text, ",") == 0 || strcmp(sc.cur.text, ";") == 0) {
                            scan_next(&sc);
                            continue;
                        }
                        if (sc.cur.kind != 1) { scan_next(&sc); continue; }
                        char fn[64] = {0}, ft[64] = "i64";
                        strncpy(fn, sc.cur.text, sizeof(fn) - 1);
                        int fl = sc.cur.line, fc = sc.cur.col;
                        scan_next(&sc);
                        if (strcmp(sc.cur.text, ":") == 0) {
                            scan_next(&sc);
                            scan_type_text(&sc, ft, sizeof(ft));
                        }
                        char fdet[256];
                        snprintf(fdet, sizeof(fdet), "%s.%s: %s", sname, fn, ft);
                        lsp_add_sym(fn, LSYM_FIELD, fl, fc, file_path, file_uri, fdet, sname, "", ft);
                        if (!first_f) strncat(sdet, ", ", sizeof(sdet) - strlen(sdet) - 1);
                        strncat(sdet, fn, sizeof(sdet) - strlen(sdet) - 1);
                        strncat(sdet, ": ", sizeof(sdet) - strlen(sdet) - 1);
                        strncat(sdet, ft, sizeof(sdet) - strlen(sdet) - 1);
                        first_f = false;
                    }
                    if (strcmp(sc.cur.text, "}") == 0) scan_next(&sc);
                }
                strncat(sdet, " }", sizeof(sdet) - strlen(sdet) - 1);
                lsp_add_sym(sname, LSYM_STRUCT, sl, scol, file_path, file_uri, sdet, "", "", sname);
            }
            continue;
        }

        if (sc.cur.kind == 1 && strcmp(sc.cur.text, "let") == 0) {
            scan_next(&sc);
            bool is_mut = false;
            if (sc.cur.kind == 1 && strcmp(sc.cur.text, "mut") == 0) {
                is_mut = true;
                scan_next(&sc);
            }
            if (sc.cur.kind == 1) {
                char vname[64], vtype[64] = "inferred";
                strncpy(vname, sc.cur.text, sizeof(vname) - 1);
                vname[sizeof(vname) - 1] = '\0';
                int vl = sc.cur.line, vc = sc.cur.col;
                scan_next(&sc);
                if (strcmp(sc.cur.text, ":") == 0) {
                    scan_next(&sc);
                    scan_type_text(&sc, vtype, sizeof(vtype));
                } else if (strcmp(sc.cur.text, "=") == 0) {
                    scan_next(&sc);
                    if (sc.cur.kind == 2) strcpy(vtype, "str");
                    else if (sc.cur.kind == 3) strcpy(vtype, strchr(sc.cur.text, '.') ? "f32" : "i64");
                    else if (sc.cur.kind == 1) {
                        if (strcmp(sc.cur.text, "slice_i64") == 0) strcpy(vtype, "*i64");
                        else if (strcmp(sc.cur.text, "slice_f32") == 0) strcpy(vtype, "*f32");
                        else if (strcmp(sc.cur.text, "slice_i32") == 0) strcpy(vtype, "*i32");
                        else if (strcmp(sc.cur.text, "str_concat") == 0 || strcmp(sc.cur.text, "str_sub") == 0 ||
                                 strcmp(sc.cur.text, "str_from_byte") == 0 || strcmp(sc.cur.text, "str_from_i64") == 0 ||
                                 strcmp(sc.cur.text, "str_join") == 0) {
                            strcpy(vtype, "str");
                        } else {
                            char candidate[64];
                            strncpy(candidate, sc.cur.text, sizeof(candidate) - 1);
                            scan_next(&sc);
                            if (strcmp(sc.cur.text, "{") == 0) strncpy(vtype, candidate, sizeof(vtype) - 1);
                        }
                    }
                }
                char vdet[256];
                snprintf(vdet, sizeof(vdet), "%s%s: %s", is_mut ? "let mut " : "let ", vname, vtype);
                lsp_add_sym(vname, LSYM_VAR, vl, vc, file_path, file_uri, vdet, "", current_fn, vtype);
            }
            continue;
        }

        scan_next(&sc);
    }
}

static void lsp_rebuild_index_for_doc(const LspDocument* doc) {
    g_sym_count = 0;
    g_indexed_file_count = 0;
    if (doc) {
        lsp_index_source_rec(doc->text, doc->path, doc->uri, 0);
    }
}

/* ========================================================================= */
/* 5. BUILTINS CATALOG & WORD EXTRACTION                                     */
/* ========================================================================= */

typedef struct {
    const char* name;
    const char* sig;
    const char* doc;
    int lsp_kind;
} LspBuiltinDoc;

static const LspBuiltinDoc g_builtin_docs[] = {
    {"print",        "fn print(val: i64|f32|ptr)",              "Print a numeric value or pointer followed by a newline.", 3},
    {"print_str",    "fn print_str(s: str)",                    "Print a string followed by a newline.", 3},
    {"scope_push",   "fn scope_push() -> i64",                  "Push an arena scope mark (`2 MiB` paged bump arena).", 3},
    {"scope_pop",    "fn scope_pop(mark: i64) -> i64",          "Rewind active arena back to `mark` in O(1) without GC.", 3},
    {"alloc",        "fn alloc(bytes: i64) -> ptr",             "Allocate 64-byte aligned memory on the active thread arena.", 3},
    {"slice_i64",    "fn slice_i64(len: i64) -> *i64",          "Allocate a bounds-checked zeroed `i64` slice on the arena.", 3},
    {"slice_i32",    "fn slice_i32(len: i64) -> *i32",          "Allocate a bounds-checked zeroed `i32` slice on the arena.", 3},
    {"slice_f32",    "fn slice_f32(len: i64) -> *f32",          "Allocate a bounds-checked zeroed `f32` slice on the arena.", 3},
    {"slice_len",    "fn slice_len(s: *T) -> i64",              "Return element count of a typed Vortex slice.", 3},
    {"vec_new",      "fn vec_new() -> i64",                     "Create a resizable dynamic vector in the active arena.", 3},
    {"vec_push",     "fn vec_push(v: i64, val: i64) -> i64",    "Append a 64-bit value or reference to a vector.", 3},
    {"vec_get",      "fn vec_get(v: i64, idx: i64) -> i64",     "Read element `idx` (bounds-checked in `--safe` mode).", 3},
    {"vec_set",      "fn vec_set(v: i64, idx: i64, val: i64)",  "Write element `idx` (bounds-checked in `--safe` mode).", 3},
    {"vec_len",      "fn vec_len(v: i64) -> i64",               "Return current length of a vector.", 3},
    {"map_new",      "fn map_new() -> i64",                     "Create an open-addressing string-keyed hash map on the arena.", 3},
    {"map_set",      "fn map_set(m: i64, k: str, v: i64)",      "Insert or update key `k` in map `m`.", 3},
    {"map_get",      "fn map_get(m: i64, k: str) -> i64",       "Look up value for key `k` in map `m`.", 3},
    {"map_has",      "fn map_has(m: i64, k: str) -> i64",       "Return `1` if key `k` exists in map `m`, else `0`.", 3},
    {"map_len",      "fn map_len(m: i64) -> i64",               "Return number of entries in map `m`.", 3},
    {"str_concat",   "fn str_concat(a: str, b: str) -> str",    "Concatenate two strings on the active arena.", 3},
    {"str_join",     "fn str_join(chunks: i64, sep: str) -> str", "Join a `vec` of strings with separator `sep` in O(N).", 3},
    {"str_len",      "fn str_len(s: str) -> i64",               "Return byte length of string `s`.", 3},
    {"str_sub",      "fn str_sub(s: str, st: i64, len: i64) -> str", "Extract substring allocated on the active arena.", 3},
    {"str_byte",     "fn str_byte(s: str, idx: i64) -> i64",    "Safe O(1)-cached byte read (`0..255`, or `-1` if OOB).", 3},
    {"str_from_byte","fn str_from_byte(ch: i64) -> str",        "Create 1-byte string on the arena.", 3},
    {"str_eq",       "fn str_eq(a: str, b: str) -> i64",        "Return `1` if strings `a` and `b` have identical content.", 3},
    {"str_find",     "fn str_find(h: str, n: str, st: i64) -> i64", "Find first index of `n` in `h` from `st`, or `-1`.", 3},
    {"str_find_any2","fn str_find_any2(s: str, c1: i64, c2: i64, st: i64) -> i64", "Find first byte `c1` or `c2` in `s` from `st`, or `-1`.", 3},
    {"str_to_i64",   "fn str_to_i64(s: str) -> i64",            "Parse decimal integer from string `s`.", 3},
    {"str_from_i64", "fn str_from_i64(n: i64) -> str",          "Format integer `n` to arena string.", 3},
    {"str_from_f32", "fn str_from_f32(f_bits: i64) -> str",     "Format `f32` to arena string.", 3},
    {"ffi_load",     "unsafe fn ffi_load(lib: str) -> ptr",     "Dynamically load a shared library (`LoadLibraryA` / `dlopen`).", 3},
    {"ffi_symbol",   "unsafe fn ffi_symbol(h: ptr, sym: str) -> ptr", "Resolve symbol pointer from shared library handle.", 3},
    {"ffi_call0",    "unsafe fn ffi_call0(fp: ptr) -> i64",     "Invoke dynamic C function pointer (0 args).", 3},
    {"ffi_call1",    "unsafe fn ffi_call1(fp: ptr, a1) -> i64", "Invoke dynamic C function pointer (1 arg).", 3},
    {"ffi_call2",    "unsafe fn ffi_call2(fp: ptr, a1, a2) -> i64", "Invoke dynamic C function pointer (2 args).", 3},
    {"ffi_call3",    "unsafe fn ffi_call3(fp: ptr, a1..a3) -> i64", "Invoke dynamic C function pointer (3 args).", 3},
    {"ffi_call4",    "unsafe fn ffi_call4(fp: ptr, a1..a4) -> i64", "Invoke dynamic C function pointer (4 args).", 3},
    {"ffi_call5",    "unsafe fn ffi_call5(fp: ptr, a1..a5) -> i64", "Invoke dynamic C function pointer (5 args).", 3},
    {"ffi_call6",    "unsafe fn ffi_call6(fp: ptr, a1..a6) -> i64", "Invoke dynamic C function pointer (6 args).", 3},
    {"ptr_to_str",   "unsafe fn ptr_to_str(p: ptr) -> str",     "Copy C string pointer `p` into the Vortex arena.", 3},
    {"ptr_read8",    "unsafe fn ptr_read8(p: ptr) -> i64",      "Read 1 byte at raw address `p`.", 3},
    {"ptr_write8",   "unsafe fn ptr_write8(p: ptr, v: i64)",    "Write 1 byte at raw address `p`.", 3},
    {"ptr_read32",   "unsafe fn ptr_read32(p: ptr) -> i64",     "Read 32-bit word at raw address `p`.", 3},
    {"ptr_write32",  "unsafe fn ptr_write32(p: ptr, v: i64)",   "Write 32-bit word at raw address `p`.", 3},
    {"ptr_read64",   "unsafe fn ptr_read64(p: ptr) -> i64",     "Read 64-bit word at raw address `p`.", 3},
    {"ptr_write64",  "unsafe fn ptr_write64(p: ptr, v: i64)",   "Write 64-bit word at raw address `p`.", 3},
    {"time_us",      "fn time_us() -> i64",                     "High-resolution monotonic microsecond timestamp.", 3},
    {"num_workers",  "fn num_workers() -> i64",                 "Number of hardware worker threads in Vortex lock-free pool.", 3},
    {"parallel_for", "parallel_for idx in start..end { ... }",  "Lock-free parallel worker loop with private thread arenas & SIMD.", 14},
    {"unsafe",       "unsafe { ... } / unsafe fn ...",          "Explicit block allowing raw pointers, pointer casts & FFI calls.", 14},
    {"import",       "import \"std/json\";",                    "Import a standard library, local file, or `vortex.toml` package.", 14},
    {NULL, NULL, NULL, 0}
};

static LspDocument* lsp_find_doc(const char* uri) {
    if (!uri) return NULL;
    for (int i = 0; i < g_doc_count; i++) {
        if (strcmp(g_docs[i].uri, uri) == 0) return &g_docs[i];
    }
    return NULL;
}

static LspDocument* lsp_upsert_doc(const char* uri, const char* text) {
    LspDocument* d = lsp_find_doc(uri);
    if (!d) {
        if (g_doc_count >= LSP_MAX_DOCS) {
            free(g_docs[0].uri);
            free(g_docs[0].text);
            memmove(&g_docs[0], &g_docs[1], (size_t)(g_doc_count - 1) * sizeof(LspDocument));
            g_doc_count--;
        }
        d = &g_docs[g_doc_count++];
        memset(d, 0, sizeof(*d));
        d->uri = (char*)malloc(strlen(uri) + 1);
        strcpy(d->uri, uri);
        lsp_uri_to_path(uri, d->path, sizeof(d->path));
    }
    free(d->text);
    d->text = (char*)malloc(strlen(text) + 1);
    strcpy(d->text, text);
    return d;
}

static void lsp_word_at(const char* text, int line0, int char0,
                        char* out_word, size_t wcap,
                        bool* out_after_dot, char* out_recv, size_t rcap) {
    out_word[0] = '\0';
    out_recv[0] = '\0';
    *out_after_dot = false;
    if (!text) return;

    int cur_l = 0;
    size_t i = 0, len = strlen(text);
    while (i < len && cur_l < line0) {
        if (text[i++] == '\n') cur_l++;
    }
    size_t line_start = i;
    size_t pos = line_start + (size_t)((char0 > 0) ? char0 : 0);
    if (pos > len) pos = len;
    while (pos > line_start && text[pos - 1] == '\n') pos--;

    size_t st = pos;
    while (st > line_start && (isalnum((unsigned char)text[st - 1]) || text[st - 1] == '_')) st--;
    size_t en = pos;
    while (en < len && (isalnum((unsigned char)text[en]) || text[en] == '_')) en++;

    size_t wn = en - st;
    if (wn >= wcap) wn = wcap - 1;
    memcpy(out_word, text + st, wn);
    out_word[wn] = '\0';

    if (st > line_start && text[st - 1] == '.') {
        *out_after_dot = true;
        size_t rs = st - 1;
        while (rs > line_start && (isalnum((unsigned char)text[rs - 1]) || text[rs - 1] == '_')) rs--;
        size_t rn = (st - 1) - rs;
        if (rn >= rcap) rn = rcap - 1;
        memcpy(out_recv, text + rs, rn);
        out_recv[rn] = '\0';
    }
}

/* ========================================================================= */
/* 6. DIAGNOSTIC PUBLISHING (REUSES REAL COMPILER FRONTEND)                  */
/* ========================================================================= */

static void lsp_parse_diag_loc(const char* msg, const char* fallback_path,
                               int* out_line1, int* out_col1, char* out_clean, size_t clean_cap) {
    *out_line1 = 1;
    *out_col1 = 1;
    vx_str_copy(out_clean, clean_cap, msg ? msg : "Compilation error");
    size_t n = strlen(out_clean);
    while (n > 0 && (out_clean[n - 1] == '\n' || out_clean[n - 1] == '\r')) out_clean[--n] = '\0';

    const char* at = strstr(out_clean, " at ");
    if (!at) return;
    const char* fstart = at + 4;
    const char* c1 = strchr(fstart, ':');
    /* Handle Windows drive letter "C:\..." */
    if (c1 && (c1 == fstart + 1) && isalpha((unsigned char)fstart[0])) {
        c1 = strchr(c1 + 1, ':');
    }
    if (!c1) return;

    char err_file[512];
    size_t flen = (size_t)(c1 - fstart);
    if (flen >= sizeof(err_file)) flen = sizeof(err_file) - 1;
    memcpy(err_file, fstart, flen);
    err_file[flen] = '\0';

    int ln = 1, cl = 1;
    if (sscanf(c1 + 1, "%d:%d", &ln, &cl) >= 1) {
        char norm_err[512], norm_fb[512];
        size_t k;
        for (k = 0; k + 1 < sizeof(norm_err) && err_file[k]; k++) {
            norm_err[k] = (err_file[k] == '\\') ? '/' : (char)tolower((unsigned char)err_file[k]);
        }
        norm_err[k] = '\0';
        for (k = 0; k + 1 < sizeof(norm_fb) && fallback_path && fallback_path[k]; k++) {
            norm_fb[k] = (fallback_path[k] == '\\') ? '/' : (char)tolower((unsigned char)fallback_path[k]);
        }
        norm_fb[k] = '\0';
        if (!fallback_path || strcmp(norm_err, norm_fb) == 0 || strstr(norm_err, "<") == norm_err) {
            *out_line1 = (ln > 0) ? ln : 1;
            *out_col1 = (cl > 0) ? cl : 1;
        }
    }
}

static void lsp_publish_diagnostics(const LspDocument* doc) {
    char diag_raw[4096] = {0};
    bool ok = vortex_frontend_check(doc->text, doc->path, diag_raw, sizeof(diag_raw));

    LspStr j = {0};
    lstr_adds(&j, "{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/publishDiagnostics\",\"params\":{\"uri\":");
    lstr_add_json_str(&j, doc->uri);
    lstr_adds(&j, ",\"diagnostics\":[");

    if (!ok && diag_raw[0]) {
        int ln1 = 1, col1 = 1;
        char clean[2048];
        lsp_parse_diag_loc(diag_raw, doc->path, &ln1, &col1, clean, sizeof(clean));
        int l0 = (ln1 > 0) ? (ln1 - 1) : 0;
        int c0 = (col1 > 0) ? (col1 - 1) : 0;
        char buf[256];
        snprintf(buf, sizeof(buf),
                 "{\"range\":{\"start\":{\"line\":%d,\"character\":%d},"
                 "\"end\":{\"line\":%d,\"character\":%d}},\"severity\":1,\"source\":\"vortex\",\"message\":",
                 l0, c0, l0, c0 + 1);
        lstr_adds(&j, buf);
        lstr_add_json_str(&j, clean);
        lstr_adds(&j, "}");
    }

    lstr_adds(&j, "]}}");
    lsp_send_json(j.data);
    free(j.data);
}

static void lsp_publish_empty_diagnostics(const char* uri) {
    LspStr j = {0};
    lstr_adds(&j, "{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/publishDiagnostics\",\"params\":{\"uri\":");
    lstr_add_json_str(&j, uri);
    lstr_adds(&j, ",\"diagnostics\":[]}}");
    lsp_send_json(j.data);
    free(j.data);
}

/* ========================================================================= */
/* 7. REQUEST HANDLERS: INITIALIZE, DEFINITION, HOVER, COMPLETION            */
/* ========================================================================= */

static const LspSymbol* lsp_lookup_symbol(const char* word, const char* doc_uri,
                                          bool after_dot, const char* recv) {
    const char* recv_type = NULL;
    if (after_dot && recv && recv[0]) {
        for (int i = 0; i < g_sym_count; i++) {
            if (strcmp(g_syms[i].name, recv) == 0 && g_syms[i].type_name[0]) {
                recv_type = g_syms[i].type_name;
                break;
            }
        }
    }
    if (after_dot && recv_type) {
        for (int i = 0; i < g_sym_count; i++) {
            if (g_syms[i].kind == LSYM_FIELD && strcmp(g_syms[i].parent, recv_type) == 0 &&
                strcmp(g_syms[i].name, word) == 0) {
                return &g_syms[i];
            }
        }
    }
    /* Prefer same-file definitions first */
    for (int i = 0; i < g_sym_count; i++) {
        if (strcmp(g_syms[i].name, word) == 0 && doc_uri && strcmp(g_syms[i].uri, doc_uri) == 0) {
            return &g_syms[i];
        }
    }
    for (int i = 0; i < g_sym_count; i++) {
        if (strcmp(g_syms[i].name, word) == 0) {
            return &g_syms[i];
        }
    }
    return NULL;
}

static void lsp_handle_definition(const char* id_json, const char* params) {
    const char* td = json_find_key(params, "textDocument");
    const char* pos = json_find_key(params, "position");
    char* uri = td ? json_dup_string(json_find_key(td, "uri")) : NULL;
    int64_t l0 = 0, c0 = 0;
    if (pos) {
        json_get_int(json_find_key(pos, "line"), &l0);
        json_get_int(json_find_key(pos, "character"), &c0);
    }

    LspDocument* doc = uri ? lsp_find_doc(uri) : NULL;
    const LspSymbol* sym = NULL;
    if (doc) {
        lsp_rebuild_index_for_doc(doc);
        char word[128], recv[128];
        bool after_dot = false;
        lsp_word_at(doc->text, (int)l0, (int)c0, word, sizeof(word), &after_dot, recv, sizeof(recv));
        if (word[0]) {
            sym = lsp_lookup_symbol(word, doc->uri, after_dot, recv);
        }
    }

    LspStr res = {0};
    lstr_adds(&res, "{\"jsonrpc\":\"2.0\",\"id\":");
    lstr_adds(&res, id_json);
    lstr_adds(&res, ",\"result\":");
    if (sym) {
        int sl = sym->line - 1;
        int sc = sym->col - 1;
        int nlen = (int)strlen(sym->name);
        lstr_adds(&res, "{\"uri\":");
        lstr_add_json_str(&res, sym->uri);
        char rbuf[160];
        snprintf(rbuf, sizeof(rbuf),
                 ",\"range\":{\"start\":{\"line\":%d,\"character\":%d},\"end\":{\"line\":%d,\"character\":%d}}}",
                 sl, sc, sl, sc + nlen);
        lstr_adds(&res, rbuf);
    } else {
        lstr_adds(&res, "null");
    }
    lstr_adds(&res, "}");
    lsp_send_json(res.data);
    free(res.data);
    free(uri);
}

static void lsp_handle_hover(const char* id_json, const char* params) {
    const char* td = json_find_key(params, "textDocument");
    const char* pos = json_find_key(params, "position");
    char* uri = td ? json_dup_string(json_find_key(td, "uri")) : NULL;
    int64_t l0 = 0, c0 = 0;
    if (pos) {
        json_get_int(json_find_key(pos, "line"), &l0);
        json_get_int(json_find_key(pos, "character"), &c0);
    }

    LspDocument* doc = uri ? lsp_find_doc(uri) : NULL;
    char md[1024] = {0};
    if (doc) {
        lsp_rebuild_index_for_doc(doc);
        char word[128], recv[128];
        bool after_dot = false;
        lsp_word_at(doc->text, (int)l0, (int)c0, word, sizeof(word), &after_dot, recv, sizeof(recv));
        if (word[0]) {
            const LspSymbol* sym = lsp_lookup_symbol(word, doc->uri, after_dot, recv);
            if (sym) {
                snprintf(md, sizeof(md), "```vortex\n%s\n```\nDefined at `%s:%d`",
                         sym->detail[0] ? sym->detail : sym->name, sym->file, sym->line);
            } else {
                for (int b = 0; g_builtin_docs[b].name != NULL; b++) {
                    if (strcmp(g_builtin_docs[b].name, word) == 0) {
                        snprintf(md, sizeof(md), "```vortex\n%s\n```\n%s",
                                 g_builtin_docs[b].sig, g_builtin_docs[b].doc);
                        break;
                    }
                }
            }
        }
    }

    LspStr res = {0};
    lstr_adds(&res, "{\"jsonrpc\":\"2.0\",\"id\":");
    lstr_adds(&res, id_json);
    lstr_adds(&res, ",\"result\":");
    if (md[0]) {
        lstr_adds(&res, "{\"contents\":{\"kind\":\"markdown\",\"value\":");
        lstr_add_json_str(&res, md);
        lstr_adds(&res, "}}");
    } else {
        lstr_adds(&res, "null");
    }
    lstr_adds(&res, "}");
    lsp_send_json(res.data);
    free(res.data);
    free(uri);
}

static bool lsp_prefix_match(const char* cand, const char* prefix) {
    if (!prefix || !prefix[0]) return true;
    for (size_t i = 0; prefix[i]; i++) {
        if (tolower((unsigned char)cand[i]) != tolower((unsigned char)prefix[i])) return false;
    }
    return true;
}

static void lsp_handle_completion(const char* id_json, const char* params) {
    const char* td = json_find_key(params, "textDocument");
    const char* pos = json_find_key(params, "position");
    char* uri = td ? json_dup_string(json_find_key(td, "uri")) : NULL;
    int64_t l0 = 0, c0 = 0;
    if (pos) {
        json_get_int(json_find_key(pos, "line"), &l0);
        json_get_int(json_find_key(pos, "character"), &c0);
    }

    LspDocument* doc = uri ? lsp_find_doc(uri) : NULL;
    LspStr res = {0};
    lstr_adds(&res, "{\"jsonrpc\":\"2.0\",\"id\":");
    lstr_adds(&res, id_json);
    lstr_adds(&res, ",\"result\":{\"isIncomplete\":false,\"items\":[");

    if (doc) {
        lsp_rebuild_index_for_doc(doc);
        char word[128], recv[128];
        bool after_dot = false;
        lsp_word_at(doc->text, (int)l0, (int)c0, word, sizeof(word), &after_dot, recv, sizeof(recv));

        const char* recv_type = NULL;
        if (after_dot && recv[0]) {
            for (int i = 0; i < g_sym_count; i++) {
                if (strcmp(g_syms[i].name, recv) == 0 && g_syms[i].type_name[0]) {
                    recv_type = g_syms[i].type_name;
                    break;
                }
            }
        }

        int emitted = 0;
        for (int i = 0; i < g_sym_count; i++) {
            const LspSymbol* s = &g_syms[i];
            if (after_dot) {
                if (s->kind != LSYM_FIELD) continue;
                if (recv_type && strcmp(s->parent, recv_type) != 0) continue;
            }
            if (!lsp_prefix_match(s->name, word)) continue;

            /* Deduplicate by name in completion list */
            bool dup = false;
            for (int j = 0; j < i; j++) {
                if (strcmp(g_syms[j].name, s->name) == 0 && (!after_dot || g_syms[j].kind == LSYM_FIELD)) {
                    dup = true;
                    break;
                }
            }
            if (dup) continue;

            if (emitted > 0) lstr_addc(&res, ',');
            lstr_adds(&res, "{\"label\":");
            lstr_add_json_str(&res, s->name);
            char kbuf[32];
            snprintf(kbuf, sizeof(kbuf), ",\"kind\":%d,\"detail\":", (int)s->kind);
            lstr_adds(&res, kbuf);
            lstr_add_json_str(&res, s->detail);
            lstr_addc(&res, '}');
            emitted++;
        }

        if (!after_dot) {
            for (int b = 0; g_builtin_docs[b].name != NULL; b++) {
                if (!lsp_prefix_match(g_builtin_docs[b].name, word)) continue;
                if (emitted > 0) lstr_addc(&res, ',');
                lstr_adds(&res, "{\"label\":");
                lstr_add_json_str(&res, g_builtin_docs[b].name);
                char kbuf[32];
                snprintf(kbuf, sizeof(kbuf), ",\"kind\":%d,\"detail\":", g_builtin_docs[b].lsp_kind);
                lstr_adds(&res, kbuf);
                lstr_add_json_str(&res, g_builtin_docs[b].sig);
                lstr_adds(&res, ",\"documentation\":");
                lstr_add_json_str(&res, g_builtin_docs[b].doc);
                lstr_addc(&res, '}');
                emitted++;
            }
            static const char* kws[] = {
                "fn", "struct", "let", "mut", "if", "else", "for", "while", "return",
                "unsafe", "extern", "import", "as", "i64", "i32", "f32", "f64", "str", "ptr", NULL
            };
            for (int k = 0; kws[k] != NULL; k++) {
                if (!lsp_prefix_match(kws[k], word)) continue;
                if (emitted > 0) lstr_addc(&res, ',');
                lstr_adds(&res, "{\"label\":");
                lstr_add_json_str(&res, kws[k]);
                lstr_adds(&res, ",\"kind\":14,\"detail\":\"Vortex keyword\"}");
                emitted++;
            }
        }
    }

    lstr_adds(&res, "]}}");
    lsp_send_json(res.data);
    free(res.data);
    free(uri);
}

/* ========================================================================= */
/* 8. LSP MAIN JSON-RPC LOOP                                                 */
/* ========================================================================= */

int vortex_cmd_lsp(int argc, char** argv) {
    (void)argc;
    (void)argv;
    bool dbg = (getenv("VORTEX_LSP_DEBUG") != NULL);
    if (dbg) {
        fprintf(stderr, "[LSP_DBG] vortex_cmd_lsp enter\n");
        fflush(stderr);
    }
#ifdef _WIN32
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif

    bool shutdown_received = false;
    char* msg = NULL;
    size_t msg_len = 0;

    while (lsp_read_message(&msg, &msg_len)) {
        if (dbg) {
            fprintf(stderr, "[LSP_DBG] recv %zu bytes\n", msg_len);
            fflush(stderr);
        }
        char id_json[128] = "null";
        bool has_id = json_extract_raw_id(msg, id_json, sizeof(id_json));
        const char* mpos = json_find_key(msg, "method");
        char* method = mpos ? json_dup_string(mpos) : NULL;
        const char* params = json_find_key(msg, "params");

        if (method) {
            if (strcmp(method, "initialize") == 0 && has_id) {
                char resp[768];
                snprintf(resp, sizeof(resp),
                         "{\"jsonrpc\":\"2.0\",\"id\":%s,\"result\":{"
                         "\"capabilities\":{"
                         "\"textDocumentSync\":{\"openClose\":true,\"change\":1,\"save\":{\"includeText\":true}},"
                         "\"hoverProvider\":true,"
                         "\"definitionProvider\":true,"
                         "\"completionProvider\":{\"triggerCharacters\":[\".\",\":\"]}"
                         "},\"serverInfo\":{\"name\":\"vortex-lsp\",\"version\":\"%s\"}}}",
                         id_json, VORTEX_VERSION);
                lsp_send_json(resp);
            } else if (strcmp(method, "shutdown") == 0 && has_id) {
                shutdown_received = true;
                char resp[192];
                snprintf(resp, sizeof(resp), "{\"jsonrpc\":\"2.0\",\"id\":%s,\"result\":null}", id_json);
                lsp_send_json(resp);
            } else if (strcmp(method, "exit") == 0) {
                free(method);
                free(msg);
                return shutdown_received ? 0 : 1;
            } else if (strcmp(method, "textDocument/didOpen") == 0 && params) {
                const char* td = json_find_key(params, "textDocument");
                char* uri = td ? json_dup_string(json_find_key(td, "uri")) : NULL;
                char* text = td ? json_dup_string(json_find_key(td, "text")) : NULL;
                if (uri && text) {
                    LspDocument* doc = lsp_upsert_doc(uri, text);
                    lsp_publish_diagnostics(doc);
                }
                free(uri);
                free(text);
            } else if (strcmp(method, "textDocument/didChange") == 0 && params) {
                const char* td = json_find_key(params, "textDocument");
                char* uri = td ? json_dup_string(json_find_key(td, "uri")) : NULL;
                const char* cc = json_find_key(params, "contentChanges");
                const char* first_ch = cc ? json_array_first(cc) : NULL;
                char* text = first_ch ? json_dup_string(json_find_key(first_ch, "text")) : NULL;
                if (uri && text) {
                    LspDocument* doc = lsp_upsert_doc(uri, text);
                    lsp_publish_diagnostics(doc);
                }
                free(uri);
                free(text);
            } else if (strcmp(method, "textDocument/didSave") == 0 && params) {
                const char* td = json_find_key(params, "textDocument");
                char* uri = td ? json_dup_string(json_find_key(td, "uri")) : NULL;
                char* text = json_dup_string(json_find_key(params, "text"));
                if (uri) {
                    LspDocument* doc = text ? lsp_upsert_doc(uri, text) : lsp_find_doc(uri);
                    if (doc) lsp_publish_diagnostics(doc);
                }
                free(uri);
                free(text);
            } else if (strcmp(method, "textDocument/didClose") == 0 && params) {
                const char* td = json_find_key(params, "textDocument");
                char* uri = td ? json_dup_string(json_find_key(td, "uri")) : NULL;
                if (uri) {
                    lsp_publish_empty_diagnostics(uri);
                }
                free(uri);
            } else if (strcmp(method, "textDocument/definition") == 0 && has_id && params) {
                lsp_handle_definition(id_json, params);
            } else if (strcmp(method, "textDocument/hover") == 0 && has_id && params) {
                lsp_handle_hover(id_json, params);
            } else if (strcmp(method, "textDocument/completion") == 0 && has_id && params) {
                lsp_handle_completion(id_json, params);
            } else if (has_id) {
                char resp[256];
                snprintf(resp, sizeof(resp),
                         "{\"jsonrpc\":\"2.0\",\"id\":%s,\"error\":{\"code\":-32601,\"message\":\"Method not found\"}}",
                         id_json);
                lsp_send_json(resp);
            }
        }

        free(method);
        free(msg);
        msg = NULL;
    }

    for (int i = 0; i < g_doc_count; i++) {
        free(g_docs[i].uri);
        free(g_docs[i].text);
    }
    return 0;
}
