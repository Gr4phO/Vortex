/*
 * VORTEX PACKAGE MANAGER (vortex pkg)
 * - vortex.toml manifest parser & writer
 * - vortex.lock lockfile with pure-C99 FIPS 180-4 SHA-256 tree hashing
 * - Local path & Git dependency resolution + global content-addressed cache
 */

#include "vortex.h"

#ifdef _WIN32
#include <direct.h>
#define pkg_getcwd _getcwd
#else
#include <dirent.h>
#define pkg_getcwd getcwd
#endif

/* ========================================================================= */
/* 1. PURE C99 SHA-256 (FIPS 180-4) + DETERMINISTIC DIRECTORY TREE HASH      */
/* ========================================================================= */

typedef struct {
    uint32_t state[8];
    uint64_t bitlen;
    uint8_t data[64];
    size_t datalen;
} VortexSha256Ctx;

static const uint32_t g_sha256_k[64] = {
    0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
    0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U, 0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
    0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU, 0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
    0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U, 0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
    0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U, 0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
    0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U, 0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
    0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
    0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U, 0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U
};

static inline uint32_t sha_rotr(uint32_t x, uint32_t n) {
    return (x >> n) | (x << (32U - n));
}

static void sha256_transform(VortexSha256Ctx* ctx, const uint8_t block[64]) {
    uint32_t m[64];
    for (int i = 0; i < 16; i++) {
        m[i] = ((uint32_t)block[i * 4] << 24) |
               ((uint32_t)block[i * 4 + 1] << 16) |
               ((uint32_t)block[i * 4 + 2] << 8) |
               ((uint32_t)block[i * 4 + 3]);
    }
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = sha_rotr(m[i - 15], 7U) ^ sha_rotr(m[i - 15], 18U) ^ (m[i - 15] >> 3U);
        uint32_t s1 = sha_rotr(m[i - 2], 17U) ^ sha_rotr(m[i - 2], 19U) ^ (m[i - 2] >> 10U);
        m[i] = m[i - 16] + s0 + m[i - 7] + s1;
    }

    uint32_t a = ctx->state[0], b = ctx->state[1], c = ctx->state[2], d = ctx->state[3];
    uint32_t e = ctx->state[4], f = ctx->state[5], g = ctx->state[6], h = ctx->state[7];
    for (int i = 0; i < 64; i++) {
        uint32_t S1 = sha_rotr(e, 6U) ^ sha_rotr(e, 11U) ^ sha_rotr(e, 25U);
        uint32_t ch = (e & f) ^ ((~e) & g);
        uint32_t t1 = h + S1 + ch + g_sha256_k[i] + m[i];
        uint32_t S0 = sha_rotr(a, 2U) ^ sha_rotr(a, 13U) ^ sha_rotr(a, 22U);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = S0 + maj;
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }
    ctx->state[0] += a; ctx->state[1] += b; ctx->state[2] += c; ctx->state[3] += d;
    ctx->state[4] += e; ctx->state[5] += f; ctx->state[6] += g; ctx->state[7] += h;
}

static void sha256_init(VortexSha256Ctx* ctx) {
    ctx->datalen = 0;
    ctx->bitlen = 0;
    ctx->state[0] = 0x6a09e667U; ctx->state[1] = 0xbb67ae85U;
    ctx->state[2] = 0x3c6ef372U; ctx->state[3] = 0xa54ff53aU;
    ctx->state[4] = 0x510e527fU; ctx->state[5] = 0x9b05688cU;
    ctx->state[6] = 0x1f83d9abU; ctx->state[7] = 0x5be0cd19U;
}

static void sha256_update(VortexSha256Ctx* ctx, const uint8_t* data, size_t len) {
    for (size_t i = 0; i < len; i++) {
        ctx->data[ctx->datalen++] = data[i];
        if (ctx->datalen == 64) {
            sha256_transform(ctx, ctx->data);
            ctx->bitlen += 512;
            ctx->datalen = 0;
        }
    }
}

static void sha256_final(VortexSha256Ctx* ctx, uint8_t hash[32]) {
    size_t i = ctx->datalen;
    if (ctx->datalen < 56) {
        ctx->data[i++] = 0x80U;
        while (i < 56) ctx->data[i++] = 0x00U;
    } else {
        ctx->data[i++] = 0x80U;
        while (i < 64) ctx->data[i++] = 0x00U;
        sha256_transform(ctx, ctx->data);
        memset(ctx->data, 0, 56);
    }
    ctx->bitlen += (uint64_t)ctx->datalen * 8ULL;
    for (int j = 0; j < 8; j++) {
        ctx->data[63 - j] = (uint8_t)((ctx->bitlen >> (j * 8)) & 0xFFU);
    }
    sha256_transform(ctx, ctx->data);
    for (int j = 0; j < 8; j++) {
        hash[j * 4 + 0] = (uint8_t)((ctx->state[j] >> 24) & 0xFFU);
        hash[j * 4 + 1] = (uint8_t)((ctx->state[j] >> 16) & 0xFFU);
        hash[j * 4 + 2] = (uint8_t)((ctx->state[j] >> 8) & 0xFFU);
        hash[j * 4 + 3] = (uint8_t)(ctx->state[j] & 0xFFU);
    }
}

void vortex_sha256_hex(const uint8_t* data, size_t len, char out_hex[65]) {
    static const char hex[] = "0123456789abcdef";
    VortexSha256Ctx ctx;
    uint8_t digest[32];
    sha256_init(&ctx);
    if (data && len > 0) sha256_update(&ctx, data, len);
    sha256_final(&ctx, digest);
    for (int i = 0; i < 32; i++) {
        out_hex[i * 2] = hex[(digest[i] >> 4) & 0xF];
        out_hex[i * 2 + 1] = hex[digest[i] & 0xF];
    }
    out_hex[64] = '\0';
}

/* ========================================================================= */
/* 2. FILESYSTEM, DIRECTORY TRAVERSAL & TREE COPY / HASH                     */
/* ========================================================================= */

static void pkg_normalize_slashes(char* s) {
    for (size_t i = 0; s && s[i]; i++) {
        if (s[i] == '\\') s[i] = '/';
    }
}

static bool pkg_file_exists(const char* path) {
    if (!path || !path[0]) return false;
#ifdef _WIN32
    DWORD attr = GetFileAttributesA(path);
    return (attr != INVALID_FILE_ATTRIBUTES) && !(attr & FILE_ATTRIBUTE_DIRECTORY);
#else
    struct stat st;
    return (stat(path, &st) == 0) && S_ISREG(st.st_mode);
#endif
}

static bool pkg_dir_exists(const char* path) {
    if (!path || !path[0]) return false;
#ifdef _WIN32
    DWORD attr = GetFileAttributesA(path);
    return (attr != INVALID_FILE_ATTRIBUTES) && ((attr & FILE_ATTRIBUTE_DIRECTORY) != 0);
#else
    struct stat st;
    return (stat(path, &st) == 0) && S_ISDIR(st.st_mode);
#endif
}

static bool pkg_mkdir_one(const char* path) {
    if (pkg_dir_exists(path)) return true;
#ifdef _WIN32
    return CreateDirectoryA(path, NULL) != 0 || GetLastError() == ERROR_ALREADY_EXISTS;
#else
    return mkdir(path, 0755) == 0;
#endif
}

static bool pkg_mkdir_p(const char* path) {
    if (!path || !path[0]) return false;
    char tmp[512];
    strncpy(tmp, path, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';
    pkg_normalize_slashes(tmp);
    size_t len = strlen(tmp);
    if (len == 0) return false;
    if (tmp[len - 1] == '/' && len > 1) tmp[--len] = '\0';

    for (size_t i = 1; i <= len; i++) {
        if (tmp[i] == '/' || tmp[i] == '\0') {
            if (i == 2 && tmp[1] == ':') continue; /* Windows drive letter "C:" */
            char saved = tmp[i];
            tmp[i] = '\0';
            if (tmp[0] && !pkg_dir_exists(tmp)) {
                if (!pkg_mkdir_one(tmp)) return false;
            }
            tmp[i] = saved;
        }
    }
    return pkg_dir_exists(tmp);
}

static bool pkg_is_abs_path(const char* p) {
    if (!p || !p[0]) return false;
    if (p[0] == '/' || p[0] == '\\') return true;
    if (isalpha((unsigned char)p[0]) && p[1] == ':') return true;
    return false;
}

static void pkg_join_path(char* out, size_t cap, const char* a, const char* b) {
    if (!a || !a[0]) {
        strncpy(out, b ? b : "", cap - 1);
        out[cap - 1] = '\0';
        pkg_normalize_slashes(out);
        return;
    }
    size_t alen = strlen(a);
    bool has_sep = (alen > 0 && (a[alen - 1] == '/' || a[alen - 1] == '\\'));
    if (has_sep) {
        snprintf(out, cap, "%s%s", a, b ? b : "");
    } else {
        snprintf(out, cap, "%s/%s", a, b ? b : "");
    }
    pkg_normalize_slashes(out);
}

static bool pkg_copy_file(const char* src, const char* dst) {
    FILE* in = fopen(src, "rb");
    if (!in) return false;
    FILE* out = fopen(dst, "wb");
    if (!out) {
        fclose(in);
        return false;
    }
    uint8_t buf[8192];
    size_t n;
    bool ok = true;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
        if (fwrite(buf, 1, n, out) != n) {
            ok = false;
            break;
        }
    }
    fclose(in);
    fclose(out);
    return ok;
}

static void pkg_rm_rf(const char* path) {
    if (!path || !path[0]) return;
    if (pkg_file_exists(path)) {
        remove(path);
        return;
    }
    if (!pkg_dir_exists(path)) return;
#ifdef _WIN32
    char pattern[512];
    snprintf(pattern, sizeof(pattern), "%s/*", path);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0) continue;
            char child[512];
            pkg_join_path(child, sizeof(child), path, fd.cFileName);
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_READONLY) {
                    SetFileAttributesA(child, FILE_ATTRIBUTE_NORMAL);
                }
                pkg_rm_rf(child);
            } else {
                SetFileAttributesA(child, FILE_ATTRIBUTE_NORMAL);
                DeleteFileA(child);
            }
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryA(path);
#else
    DIR* d = opendir(path);
    if (d) {
        struct dirent* ent;
        while ((ent = readdir(d)) != NULL) {
            if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
            char child[512];
            pkg_join_path(child, sizeof(child), path, ent->d_name);
            pkg_rm_rf(child);
        }
        closedir(d);
    }
    rmdir(path);
#endif
}

typedef struct {
    char** items;
    int count;
    int cap;
} PkgPathList;

static void pkg_pathlist_push(PkgPathList* pl, const char* rel) {
    if (pl->count >= pl->cap) {
        int ncap = (pl->cap == 0) ? 64 : (pl->cap * 2);
        char** ni = (char**)realloc(pl->items, (size_t)ncap * sizeof(char*));
        if (!ni) return;
        pl->items = ni;
        pl->cap = ncap;
    }
    size_t len = strlen(rel);
    char* copy = (char*)malloc(len + 1);
    if (!copy) return;
    memcpy(copy, rel, len + 1);
    pl->items[pl->count++] = copy;
}

static void pkg_pathlist_free(PkgPathList* pl) {
    for (int i = 0; i < pl->count; i++) free(pl->items[i]);
    free(pl->items);
}

static int cmp_cstr_ptr(const void* a, const void* b) {
    const char* sa = *(const char* const*)a;
    const char* sb = *(const char* const*)b;
    return strcmp(sa, sb);
}

static void pkg_collect_files_rec(const char* root, const char* rel_prefix, PkgPathList* out) {
    char cur_dir[512];
    if (rel_prefix[0]) pkg_join_path(cur_dir, sizeof(cur_dir), root, rel_prefix);
    else {
        strncpy(cur_dir, root, sizeof(cur_dir) - 1);
        cur_dir[sizeof(cur_dir) - 1] = '\0';
    }
#ifdef _WIN32
    char pattern[512];
    snprintf(pattern, sizeof(pattern), "%s/*", cur_dir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0) continue;
        if (strcmp(fd.cFileName, ".git") == 0 || strcmp(fd.cFileName, ".vortex") == 0) continue;
        char rel[512];
        if (rel_prefix[0]) snprintf(rel, sizeof(rel), "%s/%s", rel_prefix, fd.cFileName);
        else snprintf(rel, sizeof(rel), "%s", fd.cFileName);
        pkg_normalize_slashes(rel);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            pkg_collect_files_rec(root, rel, out);
        } else {
            pkg_pathlist_push(out, rel);
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    DIR* d = opendir(cur_dir);
    if (!d) return;
    struct dirent* ent;
    while ((ent = readdir(d)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
        if (strcmp(ent->d_name, ".git") == 0 || strcmp(ent->d_name, ".vortex") == 0) continue;
        char rel[512];
        if (rel_prefix[0]) snprintf(rel, sizeof(rel), "%s/%s", rel_prefix, ent->d_name);
        else snprintf(rel, sizeof(rel), "%s", ent->d_name);
        pkg_normalize_slashes(rel);
        char full[512];
        pkg_join_path(full, sizeof(full), root, rel);
        if (pkg_dir_exists(full)) {
            pkg_collect_files_rec(root, rel, out);
        } else if (pkg_file_exists(full)) {
            pkg_pathlist_push(out, rel);
        }
    }
    closedir(d);
#endif
}

static bool sha256_dir_tree(const char* root_dir, char out_hex[65]) {
    if (!pkg_dir_exists(root_dir)) return false;
    PkgPathList files = {0};
    pkg_collect_files_rec(root_dir, "", &files);
    if (files.count > 1) {
        qsort(files.items, (size_t)files.count, sizeof(char*), cmp_cstr_ptr);
    }

    VortexSha256Ctx ctx;
    sha256_init(&ctx);
    const char* hdr = "VORTEX-PKG-TREE-V1\n";
    sha256_update(&ctx, (const uint8_t*)hdr, strlen(hdr));

    for (int i = 0; i < files.count; i++) {
        const char* rel = files.items[i];
        char full[512];
        pkg_join_path(full, sizeof(full), root_dir, rel);
        FILE* f = fopen(full, "rb");
        if (!f) continue;
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        if (sz < 0) sz = 0;
        fseek(f, 0, SEEK_SET);

        sha256_update(&ctx, (const uint8_t*)"F\0", 2);
        sha256_update(&ctx, (const uint8_t*)rel, strlen(rel) + 1);
        uint8_t sz_le[8];
        uint64_t usz = (uint64_t)sz;
        for (int b = 0; b < 8; b++) sz_le[b] = (uint8_t)((usz >> (b * 8)) & 0xFFU);
        sha256_update(&ctx, sz_le, 8);

        uint8_t buf[8192];
        size_t n;
        while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
            sha256_update(&ctx, buf, n);
        }
        fclose(f);
    }

    uint8_t digest[32];
    sha256_final(&ctx, digest);
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < 32; i++) {
        out_hex[i * 2] = hex[(digest[i] >> 4) & 0xF];
        out_hex[i * 2 + 1] = hex[digest[i] & 0xF];
    }
    out_hex[64] = '\0';
    pkg_pathlist_free(&files);
    return true;
}

static bool pkg_copy_dir_tree(const char* src_dir, const char* dst_dir) {
    if (!pkg_dir_exists(src_dir)) return false;
    pkg_rm_rf(dst_dir);
    if (!pkg_mkdir_p(dst_dir)) return false;

    PkgPathList files = {0};
    pkg_collect_files_rec(src_dir, "", &files);
    bool ok = true;
    for (int i = 0; i < files.count; i++) {
        char sfile[512], dfile[512];
        pkg_join_path(sfile, sizeof(sfile), src_dir, files.items[i]);
        pkg_join_path(dfile, sizeof(dfile), dst_dir, files.items[i]);
        char* slash = strrchr(dfile, '/');
        if (slash) {
            *slash = '\0';
            pkg_mkdir_p(dfile);
            *slash = '/';
        }
        if (!pkg_copy_file(sfile, dfile)) {
            ok = false;
            break;
        }
    }
    pkg_pathlist_free(&files);
    return ok;
}

static void pkg_get_cache_dir(char* out, size_t cap) {
    const char* vh = getenv("VORTEX_HOME");
    if (vh && vh[0]) {
        pkg_join_path(out, cap, vh, "cache");
        return;
    }
#ifdef _WIN32
    const char* lad = getenv("LOCALAPPDATA");
    if (lad && lad[0]) {
        snprintf(out, cap, "%s/vortex/cache", lad);
        pkg_normalize_slashes(out);
        return;
    }
    const char* up = getenv("USERPROFILE");
    if (up && up[0]) {
        snprintf(out, cap, "%s/.vortex/cache", up);
        pkg_normalize_slashes(out);
        return;
    }
#else
    const char* hm = getenv("HOME");
    if (hm && hm[0]) {
        snprintf(out, cap, "%s/.vortex/cache", hm);
        pkg_normalize_slashes(out);
        return;
    }
#endif
    strncpy(out, ".vortex/cache", cap - 1);
    out[cap - 1] = '\0';
}

/* ========================================================================= */
/* 3. vortex.toml & vortex.lock PARSERS / SERIALIZERS                        */
/* ========================================================================= */

#define PKG_MAX_DEPS 64

typedef struct {
    char name[64];
    bool is_git;
    char source[512];
    char rev[128];
} VortexDepSpec;

typedef struct {
    char name[64];
    char version[32];
    char entry[256];
    VortexDepSpec deps[PKG_MAX_DEPS];
    int dep_count;
} VortexManifest;

typedef struct {
    char name[64];
    bool is_git;
    char source[512];
    char resolved_rev[128];
    char cache_path[512];
    char sha256[65];
} VortexLockEntry;

static char* trim_ws(char* s) {
    while (*s && (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n')) s++;
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r' || s[n - 1] == '\n')) {
        s[--n] = '\0';
    }
    return s;
}

static void strip_toml_comment(char* line) {
    bool in_q = false;
    for (size_t i = 0; line[i]; i++) {
        if (line[i] == '"' && (i == 0 || line[i - 1] != '\\')) in_q = !in_q;
        if (!in_q && line[i] == '#') {
            line[i] = '\0';
            break;
        }
    }
}

static bool extract_quoted_or_bare(const char* val, char* out, size_t cap) {
    while (*val && (*val == ' ' || *val == '\t')) val++;
    if (*val == '"') {
        val++;
        size_t o = 0;
        while (*val && *val != '"' && o + 1 < cap) {
            if (*val == '\\' && (*(val + 1) == '\\' || *(val + 1) == '"')) val++;
            out[o++] = *val++;
        }
        out[o] = '\0';
        return true;
    }
    strncpy(out, val, cap - 1);
    out[cap - 1] = '\0';
    char* t = trim_ws(out);
    if (t != out) memmove(out, t, strlen(t) + 1);
    return out[0] != '\0';
}

static bool extract_inline_field(const char* table_str, const char* key, char* out, size_t cap) {
    size_t klen = strlen(key);
    const char* p = table_str;
    bool in_q = false;
    while (*p) {
        if (*p == '"') {
            in_q = !in_q;
            p++;
            continue;
        }
        if (!in_q && strncmp(p, key, klen) == 0 &&
            (p == table_str || !isalnum((unsigned char)p[-1]))) {
            const char* q = p + klen;
            while (*q == ' ' || *q == '\t') q++;
            if (*q == '=') {
                q++;
                return extract_quoted_or_bare(q, out, cap);
            }
        }
        p++;
    }
    return false;
}

static bool parse_manifest(const char* path, VortexManifest* m) {
    memset(m, 0, sizeof(*m));
    strncpy(m->name, "app", sizeof(m->name) - 1);
    strncpy(m->version, "0.1.0", sizeof(m->version) - 1);
    strncpy(m->entry, "src/main.vx", sizeof(m->entry) - 1);

    FILE* f = fopen(path, "rb");
    if (!f) return false;

    char line[1024];
    int section = 0; /* 1=package, 2=dependencies */
    while (fgets(line, sizeof(line), f)) {
        strip_toml_comment(line);
        char* t = trim_ws(line);
        if (!t[0]) continue;
        if (t[0] == '[') {
            if (strcmp(t, "[package]") == 0) section = 1;
            else if (strcmp(t, "[dependencies]") == 0) section = 2;
            else section = 0;
            continue;
        }
        char* eq = strchr(t, '=');
        if (!eq) continue;
        *eq = '\0';
        char* key = trim_ws(t);
        char* val = trim_ws(eq + 1);

        if (section == 1) {
            if (strcmp(key, "name") == 0) extract_quoted_or_bare(val, m->name, sizeof(m->name));
            else if (strcmp(key, "version") == 0) extract_quoted_or_bare(val, m->version, sizeof(m->version));
            else if (strcmp(key, "entry") == 0) extract_quoted_or_bare(val, m->entry, sizeof(m->entry));
        } else if (section == 2 && m->dep_count < PKG_MAX_DEPS) {
            VortexDepSpec* d = &m->deps[m->dep_count];
            memset(d, 0, sizeof(*d));
            strncpy(d->name, key, sizeof(d->name) - 1);
            if (val[0] == '{') {
                char git_url[512] = {0}, lpath[512] = {0}, rev[128] = {0};
                if (extract_inline_field(val, "git", git_url, sizeof(git_url))) {
                    d->is_git = true;
                    strncpy(d->source, git_url, sizeof(d->source) - 1);
                    if (extract_inline_field(val, "rev", rev, sizeof(rev)) ||
                        extract_inline_field(val, "tag", rev, sizeof(rev)) ||
                        extract_inline_field(val, "branch", rev, sizeof(rev))) {
                        strncpy(d->rev, rev, sizeof(d->rev) - 1);
                    }
                    m->dep_count++;
                } else if (extract_inline_field(val, "path", lpath, sizeof(lpath))) {
                    d->is_git = false;
                    strncpy(d->source, lpath, sizeof(d->source) - 1);
                    m->dep_count++;
                }
            } else {
                extract_quoted_or_bare(val, d->source, sizeof(d->source));
                d->is_git = false;
                m->dep_count++;
            }
        }
    }
    fclose(f);
    return true;
}

static bool write_manifest(const char* path, const VortexManifest* m) {
    FILE* f = fopen(path, "wb");
    if (!f) return false;
    fprintf(f, "[package]\n");
    fprintf(f, "name = \"%s\"\n", m->name);
    fprintf(f, "version = \"%s\"\n", m->version);
    fprintf(f, "entry = \"%s\"\n\n", m->entry);
    fprintf(f, "[dependencies]\n");
    for (int i = 0; i < m->dep_count; i++) {
        const VortexDepSpec* d = &m->deps[i];
        if (d->is_git) {
            if (d->rev[0]) {
                fprintf(f, "%s = { git = \"%s\", rev = \"%s\" }\n", d->name, d->source, d->rev);
            } else {
                fprintf(f, "%s = { git = \"%s\" }\n", d->name, d->source);
            }
        } else {
            fprintf(f, "%s = { path = \"%s\" }\n", d->name, d->source);
        }
    }
    fclose(f);
    return true;
}

static bool parse_lockfile(const char* path, VortexLockEntry* entries, int max_entries, int* out_count) {
    *out_count = 0;
    FILE* f = fopen(path, "rb");
    if (!f) return false;

    char line[1024];
    int cur = -1;
    while (fgets(line, sizeof(line), f)) {
        strip_toml_comment(line);
        char* t = trim_ws(line);
        if (!t[0]) continue;
        if (strcmp(t, "[[package]]") == 0) {
            if (*out_count < max_entries) {
                cur = (*out_count)++;
                memset(&entries[cur], 0, sizeof(entries[cur]));
            } else {
                cur = -1;
            }
            continue;
        }
        if (cur < 0) continue;
        char* eq = strchr(t, '=');
        if (!eq) continue;
        *eq = '\0';
        char* key = trim_ws(t);
        char* val = trim_ws(eq + 1);
        char parsed[512] = {0};
        extract_quoted_or_bare(val, parsed, sizeof(parsed));
        if (strcmp(key, "name") == 0) strncpy(entries[cur].name, parsed, sizeof(entries[cur].name) - 1);
        else if (strcmp(key, "source_type") == 0) entries[cur].is_git = (strcmp(parsed, "git") == 0);
        else if (strcmp(key, "source") == 0) strncpy(entries[cur].source, parsed, sizeof(entries[cur].source) - 1);
        else if (strcmp(key, "resolved_rev") == 0) strncpy(entries[cur].resolved_rev, parsed, sizeof(entries[cur].resolved_rev) - 1);
        else if (strcmp(key, "cache_path") == 0) strncpy(entries[cur].cache_path, parsed, sizeof(entries[cur].cache_path) - 1);
        else if (strcmp(key, "sha256") == 0) strncpy(entries[cur].sha256, parsed, sizeof(entries[cur].sha256) - 1);
    }
    fclose(f);
    return true;
}

static bool write_lockfile(const char* path, const VortexLockEntry* entries, int count) {
    FILE* f = fopen(path, "wb");
    if (!f) return false;
    fprintf(f, "# Vortex reproducible build lockfile (SHA-256) - auto-generated\n");
    fprintf(f, "version = 1\n");
    for (int i = 0; i < count; i++) {
        const VortexLockEntry* e = &entries[i];
        fprintf(f, "\n[[package]]\n");
        fprintf(f, "name = \"%s\"\n", e->name);
        fprintf(f, "source_type = \"%s\"\n", e->is_git ? "git" : "path");
        fprintf(f, "source = \"%s\"\n", e->source);
        fprintf(f, "resolved_rev = \"%s\"\n", e->resolved_rev);
        fprintf(f, "cache_path = \"%s\"\n", e->cache_path);
        fprintf(f, "sha256 = \"%s\"\n", e->sha256);
    }
    fclose(f);
    return true;
}

/* ========================================================================= */
/* 4. GIT CLONE, CACHE POPULATION & LOCK VERIFICATION                        */
/* ========================================================================= */

static bool pkg_git_fetch_to_staging(const char* url, const char* rev, const char* staging_dir, char out_rev[128]) {
    pkg_rm_rf(staging_dir);
    char cmd[1536];
#ifdef _WIN32
    snprintf(cmd, sizeof(cmd), "git clone --quiet \"%s\" \"%s\" >NUL 2>&1", url, staging_dir);
#else
    snprintf(cmd, sizeof(cmd), "git clone --quiet \"%s\" \"%s\" >/dev/null 2>&1", url, staging_dir);
#endif
    if (system(cmd) != 0) {
        pkg_rm_rf(staging_dir);
        return false;
    }

    if (rev && rev[0]) {
#ifdef _WIN32
        snprintf(cmd, sizeof(cmd), "git -C \"%s\" checkout --quiet \"%s\" >NUL 2>&1", staging_dir, rev);
#else
        snprintf(cmd, sizeof(cmd), "git -C \"%s\" checkout --quiet \"%s\" >/dev/null 2>&1", staging_dir, rev);
#endif
        if (system(cmd) != 0) {
            pkg_rm_rf(staging_dir);
            return false;
        }
    }

    out_rev[0] = '\0';
    char rev_file[512];
    pkg_join_path(rev_file, sizeof(rev_file), staging_dir, ".git/vortex_head_rev");
    snprintf(cmd, sizeof(cmd), "git -C \"%s\" rev-parse HEAD > \"%s\"", staging_dir, rev_file);
    if (system(cmd) == 0) {
        FILE* rf = fopen(rev_file, "rb");
        if (rf) {
            if (fgets(out_rev, 128, rf)) {
                char* tr = trim_ws(out_rev);
                if (tr != out_rev) memmove(out_rev, tr, strlen(tr) + 1);
            }
            fclose(rf);
        }
        remove(rev_file);
    }
    if (!out_rev[0]) strncpy(out_rev, (rev && rev[0]) ? rev : "HEAD", 127);
    return true;
}

static bool pkg_sync_project(const char* proj_dir, bool force_update, bool verify_only, bool quiet) {
    char toml_path[512], lock_path[512];
    pkg_join_path(toml_path, sizeof(toml_path), proj_dir, "vortex.toml");
    pkg_join_path(lock_path, sizeof(lock_path), proj_dir, "vortex.lock");

    VortexManifest m;
    if (!parse_manifest(toml_path, &m)) {
        fprintf(stderr, "[VORTEX PKG ERROR] Cannot find '%s' (run 'vortex pkg init' first)\n", toml_path);
        return false;
    }

    VortexLockEntry prev_locks[PKG_MAX_DEPS];
    int prev_count = 0;
    bool has_lock = parse_lockfile(lock_path, prev_locks, PKG_MAX_DEPS, &prev_count);
    if (verify_only && !has_lock) {
        fprintf(stderr, "[VORTEX PKG ERROR] Missing '%s' to verify\n", lock_path);
        return false;
    }

    char cache_root[512], cache_pkgs[512];
    pkg_get_cache_dir(cache_root, sizeof(cache_root));
    pkg_join_path(cache_pkgs, sizeof(cache_pkgs), cache_root, "packages");
    if (!verify_only) pkg_mkdir_p(cache_pkgs);

    VortexLockEntry new_locks[PKG_MAX_DEPS];
    int new_count = 0;

    for (int i = 0; i < m.dep_count; i++) {
        const VortexDepSpec* d = &m.deps[i];
        const VortexLockEntry* locked = NULL;
        if (has_lock && !force_update) {
            for (int k = 0; k < prev_count; k++) {
                if (strcmp(prev_locks[k].name, d->name) == 0) {
                    locked = &prev_locks[k];
                    break;
                }
            }
        }

        VortexLockEntry ent;
        memset(&ent, 0, sizeof(ent));
        strncpy(ent.name, d->name, sizeof(ent.name) - 1);
        ent.is_git = d->is_git;
        strncpy(ent.source, d->source, sizeof(ent.source) - 1);

        if (!d->is_git) {
            char abs_src[512];
            if (pkg_is_abs_path(d->source)) {
                strncpy(abs_src, d->source, sizeof(abs_src) - 1);
                abs_src[sizeof(abs_src) - 1] = '\0';
                pkg_normalize_slashes(abs_src);
            } else {
                pkg_join_path(abs_src, sizeof(abs_src), proj_dir, d->source);
            }

            char actual_sha[65] = {0};
            if (!sha256_dir_tree(abs_src, actual_sha)) {
                fprintf(stderr, "[VORTEX PKG ERROR] Local dependency '%s' directory not found: %s\n", d->name, abs_src);
                return false;
            }

            if (locked && locked->sha256[0] && strcmp(actual_sha, locked->sha256) != 0) {
                fprintf(stderr,
                        "[VORTEX PKG ERROR] Checksum mismatch for '%s':\n"
                        "  locked sha256: %s\n"
                        "  actual sha256: %s\n"
                        "  Run 'vortex pkg update' if this modification is intentional.\n",
                        d->name, locked->sha256, actual_sha);
                return false;
            }

            char short12[13];
            memcpy(short12, actual_sha, 12);
            short12[12] = '\0';
            char pkg_dir_name[96];
            snprintf(pkg_dir_name, sizeof(pkg_dir_name), "%s-%s", d->name, short12);
            pkg_join_path(ent.cache_path, sizeof(ent.cache_path), cache_pkgs, pkg_dir_name);
            strncpy(ent.resolved_rev, "local", sizeof(ent.resolved_rev) - 1);
            strncpy(ent.sha256, actual_sha, sizeof(ent.sha256) - 1);

            if (verify_only) {
                if (pkg_dir_exists(ent.cache_path)) {
                    char cache_sha[65] = {0};
                    if (!sha256_dir_tree(ent.cache_path, cache_sha) || strcmp(cache_sha, ent.sha256) != 0) {
                        fprintf(stderr, "[VORTEX PKG ERROR] Corrupted cache entry for '%s' at %s\n",
                                d->name, ent.cache_path);
                        return false;
                    }
                }
            } else {
                bool cache_ok = false;
                if (pkg_dir_exists(ent.cache_path)) {
                    char cache_sha[65] = {0};
                    if (sha256_dir_tree(ent.cache_path, cache_sha) && strcmp(cache_sha, actual_sha) == 0) {
                        cache_ok = true;
                    }
                }
                if (!cache_ok) {
                    if (!pkg_copy_dir_tree(abs_src, ent.cache_path)) {
                        fprintf(stderr, "[VORTEX PKG ERROR] Failed to populate cache for '%s'\n", d->name);
                        return false;
                    }
                }
            }
            if (!quiet) {
                printf("[PKG] %-14s local %-24s sha256:%.12s%s\n",
                       d->name, d->source, ent.sha256, locked ? " (verified)" : " (cached)");
            }
        } else {
            bool need_clone = true;
            if (locked && locked->cache_path[0] && pkg_dir_exists(locked->cache_path)) {
                char csha[65] = {0};
                if (sha256_dir_tree(locked->cache_path, csha)) {
                    if (strcmp(csha, locked->sha256) != 0) {
                        fprintf(stderr,
                                "[VORTEX PKG ERROR] Cached Git package '%s' tampered/corrupted!\n"
                                "  expected: %s\n"
                                "  got:      %s\n",
                                d->name, locked->sha256, csha);
                        return false;
                    }
                    ent = *locked;
                    need_clone = false;
                    if (!quiet) {
                        printf("[PKG] %-14s git   %-24s sha256:%.12s (cache hit)\n",
                               d->name, ent.resolved_rev, ent.sha256);
                    }
                }
            }

            if (need_clone) {
                if (verify_only) {
                    fprintf(stderr, "[VORTEX PKG ERROR] Git dependency '%s' missing from cache (%s)\n",
                            d->name, locked ? locked->cache_path : "<uninstalled>");
                    return false;
                }
                char staging[512];
                snprintf(staging, sizeof(staging), "%s/_staging_%s", cache_root, d->name);
                pkg_normalize_slashes(staging);
                const char* target_rev = (locked && locked->resolved_rev[0]) ? locked->resolved_rev : d->rev;
                char resolved_rev[128] = {0};
                if (!pkg_git_fetch_to_staging(d->source, target_rev, staging, resolved_rev)) {
                    fprintf(stderr, "[VORTEX PKG ERROR] Failed to clone git dependency '%s' from '%s'\n",
                            d->name, d->source);
                    return false;
                }
                char actual_sha[65] = {0};
                if (!sha256_dir_tree(staging, actual_sha)) {
                    pkg_rm_rf(staging);
                    fprintf(stderr, "[VORTEX PKG ERROR] Failed to hash git tree for '%s'\n", d->name);
                    return false;
                }
                if (locked && locked->sha256[0] && strcmp(actual_sha, locked->sha256) != 0) {
                    pkg_rm_rf(staging);
                    fprintf(stderr,
                            "[VORTEX PKG ERROR] Git dependency '%s' checksum mismatch!\n"
                            "  locked sha256: %s\n"
                            "  actual sha256: %s\n",
                            d->name, locked->sha256, actual_sha);
                    return false;
                }
                char short12[13];
                memcpy(short12, actual_sha, 12);
                short12[12] = '\0';
                char pkg_dir_name[96];
                snprintf(pkg_dir_name, sizeof(pkg_dir_name), "%s-%s", d->name, short12);
                pkg_join_path(ent.cache_path, sizeof(ent.cache_path), cache_pkgs, pkg_dir_name);
                if (!pkg_copy_dir_tree(staging, ent.cache_path)) {
                    pkg_rm_rf(staging);
                    fprintf(stderr, "[VORTEX PKG ERROR] Failed copying '%s' into cache\n", d->name);
                    return false;
                }
                pkg_rm_rf(staging);
                strncpy(ent.resolved_rev, resolved_rev, sizeof(ent.resolved_rev) - 1);
                strncpy(ent.sha256, actual_sha, sizeof(ent.sha256) - 1);
                if (!quiet) {
                    printf("[PKG] %-14s git   %.12s sha256:%.12s (cloned+cached)\n",
                           d->name, ent.resolved_rev, ent.sha256);
                }
            }
        }
        new_locks[new_count++] = ent;
    }

    if (!verify_only) {
        if (!write_lockfile(lock_path, new_locks, new_count)) {
            fprintf(stderr, "[VORTEX PKG ERROR] Failed writing '%s'\n", lock_path);
            return false;
        }
    }
    return true;
}

/* ========================================================================= */
/* 5. MODULE IMPORT HOOK FOR PACKAGES                                        */
/* ========================================================================= */

static bool find_project_root(const char* start_file, char* out_dir, size_t cap) {
    char dir[512] = {0};
    char cwd[512] = {0};
    if (pkg_getcwd(cwd, sizeof(cwd))) pkg_normalize_slashes(cwd);

    if (start_file && start_file[0] && start_file[0] != '<') {
        char rel[512] = {0};
        strncpy(rel, start_file, sizeof(rel) - 1);
        pkg_normalize_slashes(rel);
        char* s = strrchr(rel, '/');
        if (s) *s = '\0';
        else rel[0] = '\0';
        if (rel[0]) {
            if (pkg_is_abs_path(rel)) {
                strncpy(dir, rel, sizeof(dir) - 1);
            } else if (cwd[0]) {
                pkg_join_path(dir, sizeof(dir), cwd, rel);
            } else {
                strncpy(dir, rel, sizeof(dir) - 1);
            }
        }
    }
    if (!dir[0]) {
        if (!cwd[0]) return false;
        strncpy(dir, cwd, sizeof(dir) - 1);
    }

    for (int up = 0; up < 8; up++) {
        char probe[512];
        pkg_join_path(probe, sizeof(probe), dir, "vortex.toml");
        if (pkg_file_exists(probe)) {
            strncpy(out_dir, dir, cap - 1);
            out_dir[cap - 1] = '\0';
            return true;
        }
        char* s = strrchr(dir, '/');
        if (!s || s == dir || (s == dir + 2 && dir[1] == ':')) break;
        *s = '\0';
    }
    return false;
}

static bool probe_pkg_entry(const char* pkg_root, const char* pkg_name, const char* sub, char* out, size_t cap) {
    char cands[6][512];
    int n = 0;
    if (sub && sub[0]) {
        snprintf(cands[n++], 512, "%s/src/%s.vx", pkg_root, sub);
        snprintf(cands[n++], 512, "%s/src/%s", pkg_root, sub);
        snprintf(cands[n++], 512, "%s/%s.vx", pkg_root, sub);
        snprintf(cands[n++], 512, "%s/%s", pkg_root, sub);
    } else {
        snprintf(cands[n++], 512, "%s/src/lib.vx", pkg_root);
        snprintf(cands[n++], 512, "%s/lib.vx", pkg_root);
        snprintf(cands[n++], 512, "%s/src/%s.vx", pkg_root, pkg_name);
        snprintf(cands[n++], 512, "%s/%s.vx", pkg_root, pkg_name);
        snprintf(cands[n++], 512, "%s/src/main.vx", pkg_root);
    }
    for (int i = 0; i < n; i++) {
        pkg_normalize_slashes(cands[i]);
        if (pkg_file_exists(cands[i])) {
            strncpy(out, cands[i], cap - 1);
            out[cap - 1] = '\0';
            return true;
        }
    }
    return false;
}

bool pkg_resolve_import(const char* spec, const char* importer_file, char* out_path, size_t cap) {
    if (!spec || !spec[0]) return false;
    char pkg_name[64] = {0};
    const char* sub = "";
    const char* slash = strchr(spec, '/');
    if (slash) {
        size_t plen = (size_t)(slash - spec);
        if (plen == 0 || plen >= sizeof(pkg_name)) return false;
        memcpy(pkg_name, spec, plen);
        pkg_name[plen] = '\0';
        sub = slash + 1;
    } else {
        strncpy(pkg_name, spec, sizeof(pkg_name) - 1);
    }

    char proj_dir[512];
    if (!find_project_root(importer_file, proj_dir, sizeof(proj_dir))) return false;

    char lock_path[512], toml_path[512];
    pkg_join_path(lock_path, sizeof(lock_path), proj_dir, "vortex.lock");
    pkg_join_path(toml_path, sizeof(toml_path), proj_dir, "vortex.toml");

    VortexLockEntry locks[PKG_MAX_DEPS];
    int lcount = 0;
    if (parse_lockfile(lock_path, locks, PKG_MAX_DEPS, &lcount)) {
        for (int i = 0; i < lcount; i++) {
            if (strcmp(locks[i].name, pkg_name) == 0 && locks[i].cache_path[0] && pkg_dir_exists(locks[i].cache_path)) {
                if (probe_pkg_entry(locks[i].cache_path, pkg_name, sub, out_path, cap)) {
                    return true;
                }
            }
        }
    }

    VortexManifest m;
    if (parse_manifest(toml_path, &m)) {
        for (int i = 0; i < m.dep_count; i++) {
            if (strcmp(m.deps[i].name, pkg_name) == 0 && !m.deps[i].is_git) {
                char dep_dir[512];
                if (pkg_is_abs_path(m.deps[i].source)) {
                    strncpy(dep_dir, m.deps[i].source, sizeof(dep_dir) - 1);
                    dep_dir[sizeof(dep_dir) - 1] = '\0';
                } else {
                    pkg_join_path(dep_dir, sizeof(dep_dir), proj_dir, m.deps[i].source);
                }
                if (probe_pkg_entry(dep_dir, pkg_name, sub, out_path, cap)) {
                    return true;
                }
            }
        }
    }
    return false;
}

/* ========================================================================= */
/* 6. CLI DISPATCHER: vortex pkg <subcommand>                                */
/* ========================================================================= */

static void pkg_print_usage(void) {
    printf("Vortex Package Manager (vortex pkg)\n\n");
    printf("SUBCOMMANDS:\n");
    printf("  vortex pkg init [name]                     Create vortex.toml & src/main.vx\n");
    printf("  vortex pkg add <name> [--path] <src> [--rev r] Add dependency and lock SHA-256\n");
    printf("  vortex pkg install                         Install & verify deps from lock/manifest\n");
    printf("  vortex pkg update                          Re-resolve deps and update vortex.lock\n");
    printf("  vortex pkg verify                          Verify vortex.lock SHA-256 hashes\n");
    printf("  vortex pkg list                            List dependencies in manifest/lock\n");
    printf("  vortex pkg cache-dir                       Print global package cache path\n");
    printf("  vortex pkg run [--safe|--ultrasafe N|--release] Verify lock and run manifest entry\n");
    printf("  vortex pkg build [build flags]             Verify lock and build standalone entry\n");
}

int vortex_cmd_pkg(int argc, char** argv) {
    if (argc < 3 || strcmp(argv[2], "--help") == 0 || strcmp(argv[2], "-h") == 0) {
        pkg_print_usage();
        return (argc < 3) ? 1 : 0;
    }

    const char* sub = argv[2];
    if (strcmp(sub, "cache-dir") == 0) {
        char cache[512];
        pkg_get_cache_dir(cache, sizeof(cache));
        printf("%s\n", cache);
        return 0;
    }

    if (strcmp(sub, "init") == 0) {
        VortexManifest m;
        memset(&m, 0, sizeof(m));
        const char* name = (argc >= 4 && argv[3][0] != '-') ? argv[3] : "vortex_app";
        strncpy(m.name, name, sizeof(m.name) - 1);
        strncpy(m.version, "0.1.0", sizeof(m.version) - 1);
        strncpy(m.entry, "src/main.vx", sizeof(m.entry) - 1);
        if (!write_manifest("vortex.toml", &m)) {
            fprintf(stderr, "[VORTEX PKG ERROR] Cannot write vortex.toml\n");
            return 1;
        }
        pkg_mkdir_p("src");
        if (!pkg_file_exists("src/main.vx")) {
            FILE* mf = fopen("src/main.vx", "wb");
            if (mf) {
                fprintf(mf, "print_str(\"Hello from %s!\");\n", m.name);
                fclose(mf);
            }
        }
        printf("[PKG] Initialized package '%s' (vortex.toml, src/main.vx)\n", m.name);
        return 0;
    }

    if (strcmp(sub, "add") == 0) {
        if (argc < 5) {
            fprintf(stderr, "[VORTEX PKG ERROR] Usage: vortex pkg add <name> <path|git_url> [--rev <rev>]\n");
            return 1;
        }
        const char* dep_name = argv[3];
        int arg_idx = 4;
        if (strcmp(argv[arg_idx], "--path") == 0) {
            arg_idx++;
            if (arg_idx >= argc) {
                fprintf(stderr, "[VORTEX PKG ERROR] Missing path after --path\n");
                return 1;
            }
        }
        const char* dep_src = argv[arg_idx++];
        const char* rev = "";
        for (int i = arg_idx; i < argc; i++) {
            if (strcmp(argv[i], "--rev") == 0 && i + 1 < argc) rev = argv[++i];
        }
        VortexManifest m;
        if (!parse_manifest("vortex.toml", &m)) {
            memset(&m, 0, sizeof(m));
            strncpy(m.name, "vortex_app", sizeof(m.name) - 1);
            strncpy(m.version, "0.1.0", sizeof(m.version) - 1);
            strncpy(m.entry, "src/main.vx", sizeof(m.entry) - 1);
        }
        bool is_git = (strncmp(dep_src, "http://", 7) == 0 ||
                       strncmp(dep_src, "https://", 8) == 0 ||
                       strncmp(dep_src, "ssh://", 6) == 0 ||
                       strncmp(dep_src, "git://", 6) == 0 ||
                       strncmp(dep_src, "file://", 7) == 0 ||
                       strstr(dep_src, ".git") != NULL ||
                       rev[0] != '\0');
        VortexDepSpec* d = NULL;
        for (int i = 0; i < m.dep_count; i++) {
            if (strcmp(m.deps[i].name, dep_name) == 0) {
                d = &m.deps[i];
                break;
            }
        }
        if (!d) {
            if (m.dep_count >= PKG_MAX_DEPS) {
                fprintf(stderr, "[VORTEX PKG ERROR] Too many dependencies (max %d)\n", PKG_MAX_DEPS);
                return 1;
            }
            d = &m.deps[m.dep_count++];
        }
        memset(d, 0, sizeof(*d));
        strncpy(d->name, dep_name, sizeof(d->name) - 1);
        d->is_git = is_git;
        strncpy(d->source, dep_src, sizeof(d->source) - 1);
        if (!strstr(d->source, "://")) pkg_normalize_slashes(d->source);
        if (rev[0]) strncpy(d->rev, rev, sizeof(d->rev) - 1);
        if (!write_manifest("vortex.toml", &m)) {
            fprintf(stderr, "[VORTEX PKG ERROR] Failed updating vortex.toml\n");
            return 1;
        }
        char cwd[512] = ".";
        if (pkg_getcwd(cwd, sizeof(cwd))) pkg_normalize_slashes(cwd);
        return pkg_sync_project(cwd, true, false, false) ? 0 : 1;
    }

    char cwd[512] = ".";
    if (pkg_getcwd(cwd, sizeof(cwd))) pkg_normalize_slashes(cwd);

    if (strcmp(sub, "install") == 0) {
        return pkg_sync_project(cwd, false, false, false) ? 0 : 1;
    }
    if (strcmp(sub, "update") == 0) {
        return pkg_sync_project(cwd, true, false, false) ? 0 : 1;
    }
    if (strcmp(sub, "verify") == 0) {
        return pkg_sync_project(cwd, false, true, false) ? 0 : 1;
    }
    if (strcmp(sub, "list") == 0) {
        VortexManifest m;
        if (!parse_manifest("vortex.toml", &m)) {
            fprintf(stderr, "[VORTEX PKG ERROR] Missing vortex.toml\n");
            return 1;
        }
        printf("Package: %s %s (entry: %s)\n", m.name, m.version, m.entry);
        VortexLockEntry locks[PKG_MAX_DEPS];
        int lc = 0;
        parse_lockfile("vortex.lock", locks, PKG_MAX_DEPS, &lc);
        for (int i = 0; i < m.dep_count; i++) {
            const char* sha = "<unlocked>";
            for (int k = 0; k < lc; k++) {
                if (strcmp(locks[k].name, m.deps[i].name) == 0 && locks[k].sha256[0]) {
                    sha = locks[k].sha256;
                    break;
                }
            }
            printf("  - %-14s %-6s %-28s sha256:%.16s\n",
                   m.deps[i].name, m.deps[i].is_git ? "git" : "path", m.deps[i].source, sha);
        }
        return 0;
    }
    if (strcmp(sub, "run") == 0) {
        VortexManifest m;
        if (!parse_manifest("vortex.toml", &m)) {
            fprintf(stderr, "[VORTEX PKG ERROR] Missing vortex.toml\n");
            return 1;
        }
        if (m.dep_count > 0) {
            if (!pkg_sync_project(cwd, false, false, true)) return 1;
        }
        return execute_vortex_file(m.entry);
    }
    if (strcmp(sub, "build") == 0) {
        VortexManifest m;
        if (!parse_manifest("vortex.toml", &m)) {
            fprintf(stderr, "[VORTEX PKG ERROR] Missing vortex.toml\n");
            return 1;
        }
        if (m.dep_count > 0) {
            if (!pkg_sync_project(cwd, false, false, true)) return 1;
        }
        char* bargv[32];
        int bargc = 0;
        bargv[bargc++] = argv[0];
        bargv[bargc++] = "build";
        bargv[bargc++] = m.entry;
        for (int i = 3; i < argc && bargc < 31; i++) {
            bargv[bargc++] = argv[i];
        }
        return vortex_cmd_build(bargc, bargv);
    }

    fprintf(stderr, "[VORTEX PKG ERROR] Unknown pkg subcommand '%s'\n", sub);
    pkg_print_usage();
    return 1;
}
