/*
 * VORTEX PROGRAMMING LANGUAGE - MODULE LOADER, #[cfg(...)] & BUNDLER (src/modules.c)
 */

#include "parser_internal.h"

VortexImportHookFn g_pkg_import_hook = NULL;

#define MAX_IMPORTED_PATHS 256
static char g_imported_paths[MAX_IMPORTED_PATHS][512];
static int g_imported_path_count = 0;

void lexer_reset_imports(void) {
    g_imported_path_count = 0;
}

static void normalize_mod_path(const char* in, char* out, size_t cap) {
    size_t i = 0, o = 0;
    while (in && in[i] && o + 1 < cap) {
        char c = in[i++];
        if (c == '\\') c = '/';
#ifdef _WIN32
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
#endif
        if (c == '/' && o > 0 && out[o - 1] == '/') continue;
        if (c == '.' && i < strlen(in) && (in[i] == '/' || in[i] == '\\') && (o == 0 || out[o - 1] == '/')) {
            i++;
            continue;
        }
        out[o++] = c;
    }
    out[o] = '\0';
}

bool vortex_has_imported_path(const char* path) {
    char norm[512];
    normalize_mod_path(path, norm, sizeof(norm));
    for (int i = 0; i < g_imported_path_count; i++) {
        if (strcmp(g_imported_paths[i], norm) == 0) return true;
    }
    return false;
}

void vortex_mark_imported_path(const char* path) {
    char norm[512];
    normalize_mod_path(path, norm, sizeof(norm));
    for (int i = 0; i < g_imported_path_count; i++) {
        if (strcmp(g_imported_paths[i], norm) == 0) return;
    }
    if (g_imported_path_count < MAX_IMPORTED_PATHS) {
        strncpy(g_imported_paths[g_imported_path_count++], norm, 511);
        g_imported_paths[g_imported_path_count - 1][511] = '\0';
    }
}

static bool is_readable_regular_file(const char* path) {
    if (!path || !path[0]) return false;
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    /* Reject directories on POSIX where fopen("dir", "rb") can succeed */
    int ch = fgetc(f);
    (void)ch;
    bool err = (ferror(f) != 0);
    fclose(f);
    return !err;
}

static bool try_probe_vx_file(const char* base, char* out, size_t cap) {
    if (!base || !base[0]) return false;
    if (is_readable_regular_file(base)) {
        strncpy(out, base, cap - 1);
        out[cap - 1] = '\0';
        return true;
    }
    size_t blen = strlen(base);
    if (blen < 3 || strcmp(base + blen - 3, ".vx") != 0) {
        char cand[512];
        snprintf(cand, sizeof(cand), "%s.vx", base);
        if (is_readable_regular_file(cand)) {
            strncpy(out, cand, cap - 1);
            out[cap - 1] = '\0';
            return true;
        }
        snprintf(cand, sizeof(cand), "%s/src/lib.vx", base);
        if (is_readable_regular_file(cand)) {
            strncpy(out, cand, cap - 1);
            out[cap - 1] = '\0';
            return true;
        }
        snprintf(cand, sizeof(cand), "%s/lib.vx", base);
        if (is_readable_regular_file(cand)) {
            strncpy(out, cand, cap - 1);
            out[cap - 1] = '\0';
            return true;
        }
    }
    return false;
}

static void extract_dir_part(const char* file_path, char* out_dir, size_t cap) {
    if (!file_path || !file_path[0] || file_path[0] == '<') {
        out_dir[0] = '\0';
        return;
    }
    strncpy(out_dir, file_path, cap - 1);
    out_dir[cap - 1] = '\0';
    char* s1 = strrchr(out_dir, '/');
    char* s2 = strrchr(out_dir, '\\');
    char* sep = (s1 > s2) ? s1 : s2;
    if (sep) *sep = '\0';
    else out_dir[0] = '\0';
}

bool vortex_resolve_import(const char* spec, const char* importer_file, char* out_path, size_t cap) {
    if (!spec || !spec[0]) return false;
    char imp_dir[512] = {0};
    extract_dir_part(importer_file, imp_dir, sizeof(imp_dir));

    /* 1. Built-in stdlib prefix: "std/..." */
    if (strncmp(spec, "std/", 4) == 0 || strncmp(spec, "std\\", 4) == 0) {
        const char* sub = spec + 4;
        char cand[512];
        const char* env_std = getenv("VORTEX_STD");
        if (env_std && env_std[0]) {
            snprintf(cand, sizeof(cand), "%s/%s", env_std, sub);
            if (try_probe_vx_file(cand, out_path, cap)) return true;
        }
        char self_path[512] = {0};
        char self_dir[512] = {0};
        vortex_get_self_path(self_path, sizeof(self_path), NULL);
        extract_dir_part(self_path, self_dir, sizeof(self_dir));
        if (self_dir[0]) {
            snprintf(cand, sizeof(cand), "%s/std/%s", self_dir, sub);
            if (try_probe_vx_file(cand, out_path, cap)) return true;
            snprintf(cand, sizeof(cand), "%s/../std/%s", self_dir, sub);
            if (try_probe_vx_file(cand, out_path, cap)) return true;
        }
        if (imp_dir[0]) {
            snprintf(cand, sizeof(cand), "%s/std/%s", imp_dir, sub);
            if (try_probe_vx_file(cand, out_path, cap)) return true;
            snprintf(cand, sizeof(cand), "%s/../std/%s", imp_dir, sub);
            if (try_probe_vx_file(cand, out_path, cap)) return true;
        }
        snprintf(cand, sizeof(cand), "std/%s", sub);
        if (try_probe_vx_file(cand, out_path, cap)) return true;
    }

    /* 2. Relative to importer file */
    if (imp_dir[0]) {
        char cand[512];
        snprintf(cand, sizeof(cand), "%s/%s", imp_dir, spec);
        if (try_probe_vx_file(cand, out_path, cap)) return true;
    }

    /* 3. Relative to current working directory / direct path */
    if (try_probe_vx_file(spec, out_path, cap)) return true;

    /* 4. Package manager dependency lookup (vortex.toml / vortex.lock / cache) */
    if (g_pkg_import_hook && g_pkg_import_hook(spec, importer_file, out_path, cap)) {
        return true;
    }
    return false;
}

char* vortex_read_file_alloc(const char* path, size_t* out_len) {
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    if (sz < 0) {
        fclose(f);
        return NULL;
    }
    fseek(f, 0, SEEK_SET);
    char* buf = (char*)malloc((size_t)sz + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    size_t n = fread(buf, 1, (size_t)sz, f);
    buf[n] = '\0';
    fclose(f);
    if (out_len) *out_len = n;
    return buf;
}

void vortex_parse_import_spec(Lexer* l, char* spec, size_t cap) {
    spec[0] = '\0';
    if (l->cur.type == TOK_STRING) {
        strncpy(spec, l->cur.text, cap - 1);
        spec[cap - 1] = '\0';
        next_token(l);
    } else if (l->cur.type == TOK_IDENT) {
        strncpy(spec, l->cur.text, cap - 1);
        spec[cap - 1] = '\0';
        next_token(l);
        while (l->cur.type == TOK_SLASH || l->cur.type == TOK_DOT || l->cur.type == TOK_MINUS) {
            strncat(spec, l->cur.text, cap - strlen(spec) - 1);
            next_token(l);
            if (l->cur.type == TOK_IDENT || l->cur.type == TOK_INT) {
                strncat(spec, l->cur.text, cap - strlen(spec) - 1);
                next_token(l);
            }
        }
    } else {
        fprintf(stderr, "[VORTEX SYNTAX ERROR] Expected module path string or identifier after 'import' at %s:%d:%d\n",
                g_current_filename ? g_current_filename : "<input>", l->cur.line, l->cur.col);
        vortex_error("");
    }
    if (l->cur.type == TOK_SEMI) next_token(l);
}

bool vortex_eval_cfg_predicate(const char* pred) {
    const VortexTargetDesc* td = vortex_get_target_desc();
    if (!pred || !td) return false;
    if (strcmp(pred, "windows") == 0) return td->abi == VORTEX_ABI_WIN64;
    if (strcmp(pred, "linux") == 0) return strstr(td->name, "linux") != NULL;
    if (strcmp(pred, "macos") == 0 || strcmp(pred, "darwin") == 0) return td->abi == VORTEX_ABI_DARWIN64;
    if (strcmp(pred, "unix") == 0) return td->abi != VORTEX_ABI_WIN64;
    if (strcmp(pred, "x86_64") == 0 || strcmp(pred, "x64") == 0) return td->arch == VORTEX_ARCH_X86_64;
    if (strcmp(pred, "arm64") == 0 || strcmp(pred, "aarch64") == 0) return td->arch == VORTEX_ARCH_ARM64;
    return false;
}

void vortex_skip_cfg_item(Compiler* c) {
    Lexer* l = c->lex;
    while (l->cur.type == TOK_HASH) {
        next_token(l);
        if (l->cur.type == TOK_LBRACKET) {
            int adepth = 1;
            next_token(l);
            while (adepth > 0 && l->cur.type != TOK_EOF) {
                if (l->cur.type == TOK_LBRACKET) adepth++;
                else if (l->cur.type == TOK_RBRACKET) adepth--;
                next_token(l);
            }
        }
    }
    if (l->cur.type == TOK_IMPORT) {
        next_token(l);
        char dummy[256];
        if (l->cur.type == TOK_STRING || l->cur.type == TOK_IDENT) {
            vortex_parse_import_spec(l, dummy, sizeof(dummy));
        }
        return;
    }
    int brace_depth = 0;
    bool saw_brace = false;
    while (l->cur.type != TOK_EOF) {
        if (l->cur.type == TOK_LBRACE) {
            brace_depth++;
            saw_brace = true;
        } else if (l->cur.type == TOK_RBRACE) {
            if (brace_depth > 0) brace_depth--;
            next_token(l);
            if (brace_depth == 0 && saw_brace) return;
            continue;
        } else if (l->cur.type == TOK_SEMI && brace_depth == 0) {
            next_token(l);
            return;
        }
        next_token(l);
    }
}

static void append_str_buf(char** dst, size_t* len, size_t* cap, const char* s, size_t slen) {
    if (*len + slen + 1 > *cap) {
        size_t ncap = (*cap == 0) ? 4096 : (*cap * 2);
        while (ncap < *len + slen + 1) ncap *= 2;
        char* nb = (char*)realloc(*dst, ncap);
        if (!nb) return;
        *dst = nb;
        *cap = ncap;
    }
    if (slen > 0) memcpy(*dst + *len, s, slen);
    *len += slen;
    (*dst)[*len] = '\0';
}

static void bundle_recursive(const char* file_path, const char* src,
                             char** out_buf, size_t* out_len, size_t* out_cap) {
    if (!src) return;
    if (file_path && file_path[0]) {
        if (vortex_has_imported_path(file_path)) return;
        vortex_mark_imported_path(file_path);
    }
    size_t i = 0;
    size_t n = strlen(src);
    int brace_depth = 0;
    while (i < n) {
        if (src[i] == '"') {
            size_t st = i++;
            while (i < n && src[i] != '"') {
                if (src[i] == '\\' && i + 1 < n) i += 2;
                else i++;
            }
            if (i < n && src[i] == '"') i++;
            append_str_buf(out_buf, out_len, out_cap, src + st, i - st);
            continue;
        }
        if (src[i] == '/' && i + 1 < n && src[i + 1] == '/') {
            size_t st = i;
            while (i < n && src[i] != '\n') i++;
            append_str_buf(out_buf, out_len, out_cap, src + st, i - st);
            continue;
        }
        if (src[i] == '{') brace_depth++;
        else if (src[i] == '}' && brace_depth > 0) brace_depth--;

        if (brace_depth == 0 && i + 6 <= n && strncmp(src + i, "import", 6) == 0 &&
            (i == 0 || (!isalnum((unsigned char)src[i - 1]) && src[i - 1] != '_')) &&
            (i + 6 == n || isspace((unsigned char)src[i + 6]) || src[i + 6] == '"')) {
            size_t j = i + 6;
            while (j < n && (src[j] == ' ' || src[j] == '\t')) j++;
            char spec[256] = {0};
            if (j < n && src[j] == '"') {
                j++;
                size_t k = 0;
                while (j < n && src[j] != '"' && src[j] != '\n' && k + 1 < sizeof(spec)) {
                    spec[k++] = src[j++];
                }
                if (j < n && src[j] == '"') j++;
            } else {
                size_t k = 0;
                while (j < n && (isalnum((unsigned char)src[j]) || src[j] == '_' || src[j] == '/' || src[j] == '.' || src[j] == '-') && k + 1 < sizeof(spec)) {
                    spec[k++] = src[j++];
                }
            }
            while (j < n && (src[j] == ' ' || src[j] == '\t')) j++;
            if (j < n && src[j] == ';') j++;

            char resolved[512] = {0};
            if (spec[0] && vortex_resolve_import(spec, file_path, resolved, sizeof(resolved))) {
                size_t mlen = 0;
                char* msrc = vortex_read_file_alloc(resolved, &mlen);
                if (msrc) {
                    bundle_recursive(resolved, msrc, out_buf, out_len, out_cap);
                    free(msrc);
                    append_str_buf(out_buf, out_len, out_cap, "\n", 1);
                    i = j;
                    continue;
                }
            }
        }
        append_str_buf(out_buf, out_len, out_cap, &src[i], 1);
        i++;
    }
}

char* vortex_bundle_source(const char* entry_path, const char* entry_src, size_t* out_len) {
    lexer_reset_imports();
    char* buf = NULL;
    size_t len = 0;
    size_t cap = 0;
    bundle_recursive(entry_path, entry_src, &buf, &len, &cap);
    lexer_reset_imports();
    if (out_len) *out_len = len;
    return buf;
}
