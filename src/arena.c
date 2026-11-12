/*
 * VORTEX MEMORY ARCHITECTURE & ZERO-GC DATA STRUCTURES
 * - 2 MB Paged Region Arena (64-byte aligned, Thread-Local zero-lock)
 * - Hierarchical Scope-Arena (scope_push / scope_pop)
 * - Resizable Dynamic Vectors (VortexVec) & Flat Open-Addressing Hash Maps (VortexMap)
 * - UTF-8 String Primitives
 */

#include "vortex.h"

static VortexRegionArena g_arena;

#ifdef _MSC_VER
static __declspec(thread) VortexRegionArena* tl_current_arena = NULL;
#else
static __thread VortexRegionArena* tl_current_arena = NULL;
#endif

static inline VortexRegionArena* get_active_arena(void) {
    if (tl_current_arena) return tl_current_arena;
    return &g_arena;
}

void arena_set_thread_local(VortexRegionArena* ar) {
    tl_current_arena = ar;
}

VortexPage* arena_page_create(size_t min_capacity) {
    size_t cap = VORTEX_PAGE_SIZE;
    if (min_capacity > cap) {
        cap = ((min_capacity + VORTEX_PAGE_SIZE - 1) / VORTEX_PAGE_SIZE) * VORTEX_PAGE_SIZE;
    }

    uint8_t* mem = NULL;
#ifdef _WIN32
    mem = (uint8_t*)VirtualAlloc(NULL, cap, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
#else
    mem = (uint8_t*)mmap(NULL, cap, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mem == MAP_FAILED) mem = NULL;
#endif

    if (!mem) {
        fprintf(stderr, "[VORTEX PANIC] Failed to allocate 2MB region page from OS\n");
        exit(1);
    }

    VortexPage* page = (VortexPage*)malloc(sizeof(VortexPage));
    if (!page) {
        fprintf(stderr, "[VORTEX PANIC] Out of host memory for page header\n");
        exit(1);
    }
    page->base = mem;
    page->cursor = 0;
    page->capacity = cap;
    page->next = NULL;
    return page;
}

void arena_init(void) {
    VortexRegionArena* ar = get_active_arena();
    if (!ar->head) {
        ar->head = arena_page_create(VORTEX_PAGE_SIZE);
        ar->current = ar->head;
        ar->current_page_index = 0;
        ar->total_pages = 1;
    } else {
        VortexPage* p = ar->head;
        while (p) {
            p->cursor = 0;
            p = p->next;
        }
        ar->current = ar->head;
        ar->current_page_index = 0;
    }
}

void* arena_alloc_fast(size_t bytes) {
    size_t aligned = (bytes + (VORTEX_ALIGNMENT - 1)) & ~(size_t)(VORTEX_ALIGNMENT - 1);
    if (aligned == 0) aligned = VORTEX_ALIGNMENT;

    VortexRegionArena* ar = get_active_arena();
    VortexPage* cur = ar->current;
    if (!cur) {
        arena_init();
        cur = ar->current;
    }

    /* Fast path: 1-cycle pointer bump in 2MB page with 64-byte alignment */
    if (cur->cursor + aligned <= cur->capacity) {
        void* ptr = (void*)(cur->base + cur->cursor);
        cur->cursor += aligned;
        return ptr;
    }

    /* Slow path: move to next recycled page or allocate new 2MB page from OS */
    if (cur->next) {
        ar->current = cur->next;
        ar->current_page_index++;
        ar->current->cursor = 0;
    } else {
        VortexPage* new_page = arena_page_create(aligned);
        cur->next = new_page;
        ar->current = new_page;
        ar->current_page_index++;
        ar->total_pages++;
    }

    void* ptr = (void*)(ar->current->base + ar->current->cursor);
    ar->current->cursor += aligned;
    return ptr;
}

void* arena_alloc(size_t bytes) {
    return arena_alloc_fast(bytes);
}

int64_t vortex_alloc_struct(int64_t size) {
    return (int64_t)arena_alloc_fast((size_t)size);
}

int64_t vortex_scope_push(void) {
    VortexRegionArena* ar = get_active_arena();
    uint64_t page_idx = (uint64_t)ar->current_page_index;
    uint64_t cursor = (uint64_t)(ar->current ? ar->current->cursor : 0);
    return (int64_t)((page_idx << 32) | (cursor & 0xFFFFFFFFULL));
}

int64_t vortex_scope_pop(int64_t mark) {
    uint64_t u_mark = (uint64_t)mark;
    size_t target_page_idx = (size_t)(u_mark >> 32);
    size_t target_cursor = (size_t)(u_mark & 0xFFFFFFFFULL);

    VortexRegionArena* ar = get_active_arena();
    VortexPage* p = ar->head;
    size_t idx = 0;
    while (p && idx < target_page_idx) {
        p = p->next;
        idx++;
    }

    if (p) {
        ar->current = p;
        ar->current_page_index = target_page_idx;
        p->cursor = target_cursor;

        VortexPage* next_page = p->next;
        while (next_page) {
            next_page->cursor = 0;
            next_page = next_page->next;
        }
    }
    return 0;
}

/* ========================================================================= */
/* DYNAMIC DATA STRUCTURES: RESIZABLE VECTORS & FLAT HASH MAPS               */
/* ========================================================================= */

int64_t vortex_vec_new(void) {
    VortexVec* v = (VortexVec*)arena_alloc(sizeof(VortexVec));
    v->cap = 8;
    v->len = 0;
    v->data = (int64_t*)arena_alloc(v->cap * sizeof(int64_t));
    return (int64_t)v;
}

int64_t vortex_vec_push(int64_t vec_ptr, int64_t val) {
    VortexVec* v = (VortexVec*)vec_ptr;
    if (!v) return 0;
    if (v->len >= v->cap) {
        int64_t new_cap = v->cap * 2;
        int64_t* new_data = (int64_t*)arena_alloc(new_cap * sizeof(int64_t));
        memcpy(new_data, v->data, v->len * sizeof(int64_t));
        v->data = new_data;
        v->cap = new_cap;
    }
    v->data[v->len++] = val;
    return val;
}

int64_t vortex_vec_get(int64_t vec_ptr, int64_t idx) {
    VortexVec* v = (VortexVec*)vec_ptr;
    if (!v || idx < 0 || idx >= v->len) return 0;
    return v->data[idx];
}

int64_t vortex_vec_set(int64_t vec_ptr, int64_t idx, int64_t val) {
    VortexVec* v = (VortexVec*)vec_ptr;
    if (v && idx >= 0 && idx < v->len) {
        v->data[idx] = val;
    }
    return val;
}

int64_t vortex_vec_len(int64_t vec_ptr) {
    VortexVec* v = (VortexVec*)vec_ptr;
    return v ? v->len : 0;
}

static uint64_t hash_str(const char* str) {
    uint64_t hash = 14695981039346656037ULL; /* FNV-1a 64-bit */
    while (*str) {
        hash ^= (uint8_t)(*str++);
        hash *= 1099511628211ULL;
    }
    return hash;
}

int64_t vortex_map_new(void) {
    VortexMap* m = (VortexMap*)arena_alloc(sizeof(VortexMap));
    m->capacity = 16;
    m->count = 0;
    m->entries = (MapEntry*)arena_alloc(m->capacity * sizeof(MapEntry));
    memset(m->entries, 0, m->capacity * sizeof(MapEntry));
    return (int64_t)m;
}

int64_t vortex_map_set(int64_t map_ptr, int64_t key_ptr, int64_t val) {
    VortexMap* m = (VortexMap*)map_ptr;
    const char* key = (const char*)key_ptr;
    if (!m || !key) return 0;

    if (m->count * 10 >= m->capacity * 7) {
        int64_t old_cap = m->capacity;
        MapEntry* old_entries = m->entries;
        m->capacity *= 2;
        m->entries = (MapEntry*)arena_alloc(m->capacity * sizeof(MapEntry));
        memset(m->entries, 0, m->capacity * sizeof(MapEntry));
        m->count = 0;
        for (int64_t i = 0; i < old_cap; i++) {
            if (old_entries[i].occupied) {
                vortex_map_set((int64_t)m, (int64_t)old_entries[i].key, old_entries[i].value);
            }
        }
    }

    uint64_t h = hash_str(key);
    int64_t idx = h & (m->capacity - 1);
    while (m->entries[idx].occupied) {
        if (strcmp(m->entries[idx].key, key) == 0) {
            m->entries[idx].value = val;
            return val;
        }
        idx = (idx + 1) & (m->capacity - 1);
    }
    m->entries[idx].occupied = true;
    size_t klen = strlen(key);
    char* kcopy = (char*)arena_alloc_fast(klen + 1);
    memcpy(kcopy, key, klen + 1);
    m->entries[idx].key = kcopy;
    m->entries[idx].value = val;
    m->count++;
    return val;
}

int64_t vortex_map_get(int64_t map_ptr, int64_t key_ptr) {
    VortexMap* m = (VortexMap*)map_ptr;
    const char* key = (const char*)key_ptr;
    if (!m || !key || m->count == 0) return 0;

    uint64_t h = hash_str(key);
    int64_t idx = h & (m->capacity - 1);
    while (m->entries[idx].occupied) {
        if (strcmp(m->entries[idx].key, key) == 0) {
            return m->entries[idx].value;
        }
        idx = (idx + 1) & (m->capacity - 1);
    }
    return 0;
}

int64_t vortex_map_has(int64_t map_ptr, int64_t key_ptr) {
    VortexMap* m = (VortexMap*)map_ptr;
    const char* key = (const char*)key_ptr;
    if (!m || !key || m->count == 0) return 0;

    uint64_t h = hash_str(key);
    int64_t idx = h & (m->capacity - 1);
    while (m->entries[idx].occupied) {
        if (strcmp(m->entries[idx].key, key) == 0) {
            return 1;
        }
        idx = (idx + 1) & (m->capacity - 1);
    }
    return 0;
}

int64_t vortex_map_len(int64_t map_ptr) {
    VortexMap* m = (VortexMap*)map_ptr;
    return m ? m->count : 0;
}

/* ========================================================================= */
/* UTF-8 STRINGS                                                             */
/* ========================================================================= */

int64_t vortex_str_concat(int64_t s1_ptr, int64_t s2_ptr) {
    const char* s1 = (const char*)s1_ptr;
    const char* s2 = (const char*)s2_ptr;
    if (!s1) s1 = "";
    if (!s2) s2 = "";
    size_t len1 = strlen(s1);
    size_t len2 = strlen(s2);
    char* res = (char*)arena_alloc(len1 + len2 + 1);
    memcpy(res, s1, len1);
    memcpy(res + len1, s2, len2);
    res[len1 + len2] = '\0';
    return (int64_t)res;
}

int64_t vortex_str_len(int64_t s_ptr) {
    const char* s = (const char*)s_ptr;
    return s ? (int64_t)strlen(s) : 0;
}

int64_t vortex_str_sub(int64_t s_ptr, int64_t start, int64_t len) {
    const char* s = (const char*)s_ptr;
    if (!s || start < 0) return (int64_t)"";
    size_t total = strlen(s);
    if ((size_t)start >= total) return (int64_t)"";
    if ((size_t)(start + len) > total) len = total - start;
    char* res = (char*)arena_alloc(len + 1);
    memcpy(res, s + start, len);
    res[len] = '\0';
    return (int64_t)res;
}

int64_t vortex_str_from_i64(int64_t n) {
    char* buf = (char*)arena_alloc(32);
    snprintf(buf, 32, "%lld", (long long)n);
    return (int64_t)buf;
}

int64_t vortex_str_from_f32(int64_t f_bits) {
    float f;
    uint32_t b = (uint32_t)f_bits;
    memcpy(&f, &b, 4);
    char* buf = (char*)arena_alloc(32);
    snprintf(buf, 32, "%.4f", (double)f);
    return (int64_t)buf;
}
