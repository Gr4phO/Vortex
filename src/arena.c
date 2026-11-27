/*
 * VORTEX MEMORY ARCHITECTURE & ZERO-GC DATA STRUCTURES
 * - 2 MB Paged Region Arena (64-byte aligned, Thread-Local zero-lock)
 * - Hierarchical Scope-Arena (scope_push / scope_pop) with Epoch Safety Verification
 * - Safe Typed Slices, Bounds/Epoch/FFI Boundary Validators
 * - Resizable Dynamic Vectors (VortexVec) & Flat Open-Addressing Hash Maps (VortexMap)
 * - UTF-8 String Primitives
 */

#include "vortex.h"

static VortexRegionArena g_arena;
static VortexRegionArena* g_registered_arenas[VORTEX_MAX_THREADS + 4];
static int g_registered_arena_count = 0;

#ifdef _MSC_VER
static __declspec(thread) VortexRegionArena* tl_current_arena = NULL;
static __declspec(thread) uint64_t tl_rewind_seq = 1;
static __declspec(thread) const char* tl_last_str_s = NULL;
static __declspec(thread) size_t tl_last_str_len = 0;
static __declspec(thread) uint64_t tl_last_str_seq = 0;
#else
static __thread VortexRegionArena* tl_current_arena = NULL;
static __thread uint64_t tl_rewind_seq = 1;
static __thread const char* tl_last_str_s = NULL;
static __thread size_t tl_last_str_len = 0;
static __thread uint64_t tl_last_str_seq = 0;
#endif

static inline VortexRegionArena* get_active_arena(void) {
    if (tl_current_arena) return tl_current_arena;
    return &g_arena;
}

void arena_register(VortexRegionArena* ar) {
    if (!ar) return;
    for (int i = 0; i < g_registered_arena_count; i++) {
        if (g_registered_arenas[i] == ar) return;
    }
    if (g_registered_arena_count < VORTEX_MAX_THREADS + 4) {
        g_registered_arenas[g_registered_arena_count++] = ar;
    }
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
    page->high_water = 0;
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
            if (p->cursor > p->high_water) p->high_water = p->cursor;
            p->cursor = 0;
            p = p->next;
        }
        ar->current = ar->head;
        ar->current_page_index = 0;
    }
    ar->scope_depth = 0;
    ar->scope_stack[0] = 1;
    ar->next_epoch = 2;
    tl_rewind_seq++;
    arena_register(ar);
    if (ar == &g_arena) {
        vortex_hw_release_all_bufs();
    }
}

void arena_get_scope_info(void** out_arena, uint32_t* out_depth, uint64_t* out_epoch) {
    VortexRegionArena* ar = get_active_arena();
    if (out_arena) *out_arena = (void*)ar;
    if (out_depth) *out_depth = ar ? ar->scope_depth : 0;
    if (out_epoch) {
        *out_epoch = (ar && ar->scope_depth < VORTEX_MAX_SCOPE_DEPTH)
                   ? ar->scope_stack[ar->scope_depth]
                   : 1;
    }
}

static void* arena_bump_raw(VortexRegionArena* ar, size_t total_aligned) {
    VortexPage* cur = ar->current;
    if (!cur) {
        arena_init();
        cur = ar->current;
    }

    /* Fast path: 1-cycle pointer bump in 2MB page with 64-byte alignment */
    if (cur->cursor + total_aligned <= cur->capacity) {
        void* ptr = (void*)(cur->base + cur->cursor);
        cur->cursor += total_aligned;
        return ptr;
    }

    /* Slow path: move to next recycled page (with enough capacity) or allocate new OS page */
    if (cur->cursor > cur->high_water) cur->high_water = cur->cursor;
    while (cur->next && cur->next->capacity < total_aligned) {
        cur = cur->next;
        ar->current_page_index++;
        if (cur->cursor > cur->high_water) cur->high_water = cur->cursor;
        cur->cursor = 0;
    }
    if (cur->next) {
        ar->current = cur->next;
        ar->current_page_index++;
        if (ar->current->cursor > ar->current->high_water) ar->current->high_water = ar->current->cursor;
        ar->current->cursor = 0;
    } else {
        VortexPage* new_page = arena_page_create(total_aligned);
        cur->next = new_page;
        ar->current = new_page;
        ar->current_page_index++;
        ar->total_pages++;
    }

    void* ptr = (void*)(ar->current->base + ar->current->cursor);
    ar->current->cursor += total_aligned;
    return ptr;
}

static void* arena_alloc_with_header(size_t bytes, uint64_t elem_count, uint32_t elem_size) {
    size_t aligned = (bytes + (VORTEX_ALIGNMENT - 1)) & ~(size_t)(VORTEX_ALIGNMENT - 1);
    if (aligned == 0) aligned = VORTEX_ALIGNMENT;

    VortexRegionArena* ar = get_active_arena();
    if (!ar->current) {
        arena_init();
    }

    uint8_t* raw = (uint8_t*)arena_bump_raw(ar, sizeof(VortexSafeHeader) + aligned);
    uint8_t* user_ptr = raw + sizeof(VortexSafeHeader);

    VortexSafeHeader* hdr = (VortexSafeHeader*)raw;
    hdr->magic = VORTEX_SAFE_MAGIC;
    hdr->owner_arena = ar;
    hdr->epoch = (ar->scope_depth < VORTEX_MAX_SCOPE_DEPTH) ? ar->scope_stack[ar->scope_depth] : 1;
    hdr->byte_size = (uint64_t)bytes;
    hdr->elem_count = elem_count;
    hdr->scope_depth = ar->scope_depth;
    hdr->elem_size = elem_size;
    hdr->page_idx = (uint32_t)ar->current_page_index;
    hdr->page_offset = (uint32_t)(user_ptr - ar->current->base);
    hdr->reserved = 0;

    return (void*)user_ptr;
}

void* arena_alloc_fast(size_t bytes) {
    if (g_compile_mode == VORTEX_MODE_SAFE) {
        return arena_alloc_with_header(bytes, 0, 0);
    }

    size_t aligned = (bytes + (VORTEX_ALIGNMENT - 1)) & ~(size_t)(VORTEX_ALIGNMENT - 1);
    if (aligned == 0) aligned = VORTEX_ALIGNMENT;

    VortexRegionArena* ar = get_active_arena();
    return arena_bump_raw(ar, aligned);
}

void* arena_alloc_compact(size_t bytes) {
    if (g_compile_mode == VORTEX_MODE_SAFE) {
        return arena_alloc_with_header(bytes, 0, 0);
    }
    size_t aligned = (bytes + 7U) & ~(size_t)7U;
    if (aligned == 0) aligned = 8;
    return arena_bump_raw(get_active_arena(), aligned);
}

void* arena_alloc(size_t bytes) {
    return arena_alloc_fast(bytes);
}

int64_t vortex_alloc_struct(int64_t size) {
    if (g_compile_mode == VORTEX_MODE_RELEASE) {
        size_t aligned = ((size_t)size + 7U) & ~(size_t)7U;
        if (aligned == 0) aligned = 8;
        return (int64_t)arena_bump_raw(get_active_arena(), aligned);
    }
    return (int64_t)arena_alloc_with_header((size_t)size, 0, 0);
}

static int64_t vortex_alloc_slice_common(int64_t len, uint32_t elem_size) {
    if (len < 0 || len > 1000000000LL || elem_size == 0 ||
        (uint64_t)len > (uint64_t)(SIZE_MAX - sizeof(VortexSafeHeader) - VORTEX_ALIGNMENT) / elem_size) {
        fprintf(stderr, "[VORTEX SAFETY ERROR] Invalid slice length %lld\n", (long long)len);
        vortex_error("");
    }
    size_t bytes = (size_t)len * (size_t)elem_size;
    void* ptr = arena_alloc_with_header(bytes, (uint64_t)len, elem_size);
    if (bytes > 0) {
        VortexRegionArena* ar = get_active_arena();
        VortexPage* page = ar->current;
        if (page && page->high_water > 0) {
            size_t user_off = (size_t)((uint8_t*)ptr - page->base);
            if (user_off < page->high_water) {
                size_t dirty_bytes = page->high_water - user_off;
                if (dirty_bytes > bytes) dirty_bytes = bytes;
                memset(ptr, 0, dirty_bytes);
            }
        }
    }
    return (int64_t)ptr;
}

int64_t vortex_alloc_slice_i64(int64_t len) {
    return vortex_alloc_slice_common(len, 8);
}

int64_t vortex_alloc_slice_f32(int64_t len) {
    return vortex_alloc_slice_common(len, 4);
}

int64_t vortex_alloc_slice_f64(int64_t len) {
    return vortex_alloc_slice_common(len, 8);
}

int64_t vortex_alloc_slice_i32(int64_t len) {
    return vortex_alloc_slice_common(len, 4);
}

static inline VortexSafeHeader* arena_match_page_header(VortexRegionArena* ar,
                                                        VortexPage* page,
                                                        size_t p_idx,
                                                        const uint8_t* p_addr,
                                                        VortexRegionArena** out_arena,
                                                        VortexPage** out_page,
                                                        size_t* out_page_idx,
                                                        size_t* out_offset,
                                                        bool* out_in_arena,
                                                        bool* matched) {
    if (page && p_addr >= page->base && p_addr < page->base + page->capacity) {
        size_t off = (size_t)(p_addr - page->base);
        if (out_arena) *out_arena = ar;
        if (out_page) *out_page = page;
        if (out_page_idx) *out_page_idx = p_idx;
        if (out_offset) *out_offset = off;
        if (out_in_arena) *out_in_arena = true;
        *matched = true;
        if (off >= sizeof(VortexSafeHeader)) {
            VortexSafeHeader* hdr = (VortexSafeHeader*)(p_addr - sizeof(VortexSafeHeader));
            if (hdr->magic == VORTEX_SAFE_MAGIC) {
                return hdr;
            }
        }
        return NULL;
    }
    *matched = false;
    return NULL;
}

static VortexSafeHeader* find_arena_header_internal(const void* ptr,
                                                    VortexRegionArena** out_arena,
                                                    VortexPage** out_page,
                                                    size_t* out_page_idx,
                                                    size_t* out_offset,
                                                    bool* out_in_arena) {
    const uint8_t* p_addr = (const uint8_t*)ptr;
    if (out_in_arena) *out_in_arena = false;
    if (!p_addr) return NULL;

    /* Fast path: check active thread-local arena's current page first, then its page list */
    VortexRegionArena* act = get_active_arena();
    bool matched = false;
    if (act && act->current) {
        VortexSafeHeader* h = arena_match_page_header(
            act, act->current, act->current_page_index, p_addr,
            out_arena, out_page, out_page_idx, out_offset, out_in_arena, &matched);
        if (matched) return h;
    }
    if (act) {
        VortexPage* page = act->head;
        size_t p_idx = 0;
        while (page) {
            if (page != act->current) {
                VortexSafeHeader* h = arena_match_page_header(
                    act, page, p_idx, p_addr,
                    out_arena, out_page, out_page_idx, out_offset, out_in_arena, &matched);
                if (matched) return h;
            }
            page = page->next;
            p_idx++;
        }
    }

    for (int a = 0; a < g_registered_arena_count; a++) {
        VortexRegionArena* ar = g_registered_arenas[a];
        if (!ar || ar == act) continue;
        VortexPage* page = ar->head;
        size_t p_idx = 0;
        while (page) {
            VortexSafeHeader* h = arena_match_page_header(
                ar, page, p_idx, p_addr,
                out_arena, out_page, out_page_idx, out_offset, out_in_arena, &matched);
            if (matched) return h;
            page = page->next;
            p_idx++;
        }
    }
    return NULL;
}

static bool is_arena_pointer_alive(const void* ptr,
                                   VortexSafeHeader** out_hdr,
                                   bool* out_in_arena,
                                   const char** out_reason) {
    VortexRegionArena* ar = NULL;
    VortexPage* page = NULL;
    size_t p_idx = 0;
    size_t off = 0;
    bool in_arena = false;

    VortexSafeHeader* hdr = find_arena_header_internal(ptr, &ar, &page, &p_idx, &off, &in_arena);
    if (out_hdr) *out_hdr = hdr;
    if (out_in_arena) *out_in_arena = in_arena;

    if (!in_arena) {
        return true;
    }

    if (p_idx > ar->current_page_index ||
        (p_idx == ar->current_page_index && off >= ar->current->cursor)) {
        if (out_reason) *out_reason = "Use-after-pop: pointer refers to popped arena region beyond active cursor";
        return false;
    }

    if (hdr) {
        if (hdr->owner_arena != ar) {
            if (out_reason) *out_reason = "Invalid arena ownership metadata";
            return false;
        }
        if (hdr->scope_depth > ar->scope_depth ||
            hdr->scope_depth >= VORTEX_MAX_SCOPE_DEPTH ||
            ar->scope_stack[hdr->scope_depth] != hdr->epoch) {
            if (out_reason) *out_reason = "Use-after-pop: arena scope epoch mismatch (dangling reference)";
            return false;
        }
    }
    return true;
}

int64_t vortex_slice_len(int64_t ptr_val) {
    if (!ptr_val) return 0;
    VortexSafeHeader* hdr = NULL;
    bool in_arena = false;
    const char* reason = NULL;
    if (!is_arena_pointer_alive((const void*)ptr_val, &hdr, &in_arena, &reason)) {
        fprintf(stderr, "[VORTEX MEMORY ERROR] %s in slice_len()\n", reason ? reason : "Invalid slice");
        vortex_error("");
    }
    if (hdr) {
        if (hdr->elem_count > 0) return (int64_t)hdr->elem_count;
        if (hdr->elem_size > 0) return (int64_t)(hdr->byte_size / hdr->elem_size);
        return (int64_t)(hdr->byte_size / 8);
    }
    return 0;
}

int64_t vortex_scope_push(void) {
    VortexRegionArena* ar = get_active_arena();
    uint64_t prev_depth = (uint64_t)ar->scope_depth;
    uint64_t page_idx = (uint64_t)ar->current_page_index;
    uint64_t cursor = (uint64_t)(ar->current ? ar->current->cursor : 0);

    if (ar->scope_depth + 1 < VORTEX_MAX_SCOPE_DEPTH) {
        ar->scope_depth++;
        ar->scope_stack[ar->scope_depth] = ar->next_epoch++;
    }

    return (int64_t)(((prev_depth & 0xFFFULL) << 52) |
                     ((page_idx & 0xFFFFFULL) << 32) |
                     (cursor & 0xFFFFFFFFULL));
}

int64_t vortex_scope_pop(int64_t mark) {
    uint64_t u_mark = (uint64_t)mark;
    uint32_t target_depth = (uint32_t)((u_mark >> 52) & 0xFFFULL);
    size_t target_page_idx = (size_t)((u_mark >> 32) & 0xFFFFFULL);
    size_t target_cursor = (size_t)(u_mark & 0xFFFFFFFFULL);

    VortexRegionArena* ar = get_active_arena();
    if (target_depth < VORTEX_MAX_SCOPE_DEPTH) {
        ar->scope_depth = target_depth;
    }

    VortexPage* p = ar->head;
    size_t idx = 0;
    while (p && idx < target_page_idx) {
        p = p->next;
        idx++;
    }

    if (p) {
        if (p->cursor > p->high_water) p->high_water = p->cursor;
        ar->current = p;
        ar->current_page_index = target_page_idx;
        p->cursor = target_cursor;

        VortexPage* next_page = p->next;
        while (next_page) {
            if (next_page->cursor > next_page->high_water) next_page->high_water = next_page->cursor;
            next_page->cursor = 0;
            next_page = next_page->next;
        }
    }
    vortex_hw_release_scope(ar, target_depth);
    tl_rewind_seq++;
    return 0;
}

/* ========================================================================= */
/* SAFE RUNTIME VALIDATION HELPERS (--safe MODE & HOISTED CHECKS)            */
/* ========================================================================= */

void vortex_safe_check_ptr(int64_t ptr_val, int64_t file_ptr, int64_t line) {
    const char* file = file_ptr ? (const char*)file_ptr : "<unknown>";
    if (ptr_val == 0) {
        fprintf(stderr, "[VORTEX SAFETY ERROR] Null pointer dereference at %s:%lld\n",
                file, (long long)line);
        vortex_error("");
    }
    VortexSafeHeader* hdr = NULL;
    bool in_arena = false;
    const char* reason = NULL;
    if (!is_arena_pointer_alive((const void*)ptr_val, &hdr, &in_arena, &reason)) {
        fprintf(stderr, "[VORTEX MEMORY ERROR] %s at %s:%lld\n",
                reason ? reason : "Use-after-pop detected", file, (long long)line);
        vortex_error("");
    }
    if (!in_arena) {
        uintptr_t addr = (uintptr_t)ptr_val;
        if (addr < 4096 || (addr & 3) != 0) {
            fprintf(stderr, "[VORTEX MEMORY ERROR] Invalid or unaligned pointer 0x%llx at %s:%lld\n",
                    (unsigned long long)addr, file, (long long)line);
            vortex_error("");
        }
    }
}

void vortex_safe_check_bounds(int64_t ptr_val, int64_t idx, int64_t len_override,
                              int64_t elem_size, int64_t file_ptr, int64_t line) {
    const char* file = file_ptr ? (const char*)file_ptr : "<unknown>";
    if (ptr_val == 0) {
        fprintf(stderr, "[VORTEX BOUNDS ERROR] Null slice/array pointer at %s:%lld\n",
                file, (long long)line);
        vortex_error("");
    }

    VortexSafeHeader* hdr = NULL;
    bool in_arena = false;
    const char* reason = NULL;
    if (!is_arena_pointer_alive((const void*)ptr_val, &hdr, &in_arena, &reason)) {
        fprintf(stderr, "[VORTEX MEMORY ERROR] %s at %s:%lld\n",
                reason ? reason : "Use-after-pop on array/slice", file, (long long)line);
        vortex_error("");
    }

    int64_t len = len_override;
    if (len < 0 && hdr) {
        if (hdr->elem_count > 0) {
            len = (int64_t)hdr->elem_count;
        } else if (hdr->byte_size > 0) {
            int64_t esz = (elem_size > 0) ? elem_size : 8;
            len = (int64_t)(hdr->byte_size / (size_t)esz);
        }
    }

    if (len >= 0) {
        if (idx < 0 || idx >= len) {
            fprintf(stderr, "[VORTEX BOUNDS ERROR] Index %lld out of bounds [0..%lld) at %s:%lld\n",
                    (long long)idx, (long long)len, file, (long long)line);
            vortex_error("");
        }
    } else if (g_compile_mode == VORTEX_MODE_SAFE) {
        fprintf(stderr, "[VORTEX BOUNDS ERROR] Untracked raw pointer indexing outside 'unsafe' at %s:%lld\n",
                file, (long long)line);
        vortex_error("");
    }
}

void vortex_safe_check_range(int64_t ptr_val, int64_t start, int64_t lim,
                             int64_t len_override, int64_t elem_size,
                             int64_t file_ptr, int64_t line) {
    if (start >= lim) {
        return; /* Loop executes 0 times; no element accessed */
    }
    const char* file = file_ptr ? (const char*)file_ptr : "<unknown>";
    vortex_safe_check_bounds(ptr_val, start, len_override, elem_size, file_ptr, line);
    vortex_safe_check_bounds(ptr_val, lim - 1, len_override, elem_size, file_ptr, line);
    (void)file;
}

void vortex_safe_check_ffi_arg(int64_t val, int64_t type_code, int64_t align_and_flags,
                               int64_t fn_name_ptr, int64_t arg_idx,
                               int64_t file_ptr, int64_t line) {
    const char* file = file_ptr ? (const char*)file_ptr : "<unknown>";
    const char* fn_name = fn_name_ptr ? (const char*)fn_name_ptr : "<ffi>";
    bool nullable = ((align_and_flags >> 16) & 1) != 0;
    int64_t req_align = align_and_flags & 0xFFFF;
    (void)type_code;

    if (val == 0) {
        if (!nullable) {
            fprintf(stderr,
                    "[VORTEX FFI ERROR] Null pointer passed to argument #%lld of extern fn '%s' at %s:%lld\n",
                    (long long)(arg_idx + 1), fn_name, file, (long long)line);
            vortex_error("");
        }
        return;
    }

    if (req_align > 1 && ((uintptr_t)val & (uintptr_t)(req_align - 1)) != 0) {
        fprintf(stderr,
                "[VORTEX FFI ERROR] Unaligned pointer 0x%llx (requires %lld-byte alignment) for argument #%lld of extern fn '%s' at %s:%lld\n",
                (unsigned long long)(uintptr_t)val, (long long)req_align,
                (long long)(arg_idx + 1), fn_name, file, (long long)line);
        vortex_error("");
    }

    VortexSafeHeader* hdr = NULL;
    bool in_arena = false;
    const char* reason = NULL;
    if (!is_arena_pointer_alive((const void*)val, &hdr, &in_arena, &reason)) {
        fprintf(stderr,
                "[VORTEX FFI ERROR] %s passed to argument #%lld of extern fn '%s' at %s:%lld\n",
                reason ? reason : "Dangling arena pointer",
                (long long)(arg_idx + 1), fn_name, file, (long long)line);
        vortex_error("");
    }
}

/* --- Low-level unsafe pointer access helpers --- */
int64_t vortex_ptr_read8(int64_t ptr_val) {
    return (int64_t)(*(const volatile uint8_t*)(uintptr_t)ptr_val);
}

int64_t vortex_ptr_write8(int64_t ptr_val, int64_t val) {
    *(volatile uint8_t*)(uintptr_t)ptr_val = (uint8_t)val;
    return val;
}

int64_t vortex_ptr_read32(int64_t ptr_val) {
    return (int64_t)(*(const volatile int32_t*)(uintptr_t)ptr_val);
}

int64_t vortex_ptr_write32(int64_t ptr_val, int64_t val) {
    *(volatile int32_t*)(uintptr_t)ptr_val = (int32_t)val;
    return val;
}

int64_t vortex_ptr_read64(int64_t ptr_val) {
    return *(const volatile int64_t*)(uintptr_t)ptr_val;
}

int64_t vortex_ptr_write64(int64_t ptr_val, int64_t val) {
    *(volatile int64_t*)(uintptr_t)ptr_val = val;
    return val;
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

int64_t vortex_vec_get_safe(int64_t vec_ptr, int64_t idx, int64_t file_ptr, int64_t line) {
    const char* file = file_ptr ? (const char*)file_ptr : "<unknown>";
    vortex_safe_check_ptr(vec_ptr, file_ptr, line);
    VortexVec* v = (VortexVec*)vec_ptr;
    if (idx < 0 || idx >= v->len) {
        fprintf(stderr, "[VORTEX BOUNDS ERROR] vec_get index %lld out of bounds [0..%lld) at %s:%lld\n",
                (long long)idx, (long long)v->len, file, (long long)line);
        vortex_error("");
    }
    return v->data[idx];
}

int64_t vortex_vec_set_safe(int64_t vec_ptr, int64_t idx, int64_t val, int64_t file_ptr, int64_t line) {
    const char* file = file_ptr ? (const char*)file_ptr : "<unknown>";
    vortex_safe_check_ptr(vec_ptr, file_ptr, line);
    VortexVec* v = (VortexVec*)vec_ptr;
    if (idx < 0 || idx >= v->len) {
        fprintf(stderr, "[VORTEX BOUNDS ERROR] vec_set index %lld out of bounds [0..%lld) at %s:%lld\n",
                (long long)idx, (long long)v->len, file, (long long)line);
        vortex_error("");
    }
    v->data[idx] = val;
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
        int64_t mask = m->capacity - 1;
        for (int64_t i = 0; i < old_cap; i++) {
            if (old_entries[i].occupied) {
                int64_t nidx = (int64_t)(old_entries[i].hash & (uint64_t)mask);
                while (m->entries[nidx].occupied) {
                    nidx = (nidx + 1) & mask;
                }
                m->entries[nidx] = old_entries[i];
            }
        }
    }

    uint64_t h = hash_str(key);
    int64_t mask = m->capacity - 1;
    int64_t idx = (int64_t)(h & (uint64_t)mask);
    while (m->entries[idx].occupied) {
        if (m->entries[idx].hash == h && strcmp(m->entries[idx].key, key) == 0) {
            m->entries[idx].value = val;
            return val;
        }
        idx = (idx + 1) & mask;
    }
    m->entries[idx].occupied = true;
    m->entries[idx].hash = h;
    size_t klen = strlen(key);
    char* kcopy = (char*)arena_alloc_compact(klen + 1);
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
    int64_t mask = m->capacity - 1;
    int64_t idx = (int64_t)(h & (uint64_t)mask);
    while (m->entries[idx].occupied) {
        if (m->entries[idx].hash == h && strcmp(m->entries[idx].key, key) == 0) {
            return m->entries[idx].value;
        }
        idx = (idx + 1) & mask;
    }
    return 0;
}

int64_t vortex_map_has(int64_t map_ptr, int64_t key_ptr) {
    VortexMap* m = (VortexMap*)map_ptr;
    const char* key = (const char*)key_ptr;
    if (!m || !key || m->count == 0) return 0;

    uint64_t h = hash_str(key);
    int64_t mask = m->capacity - 1;
    int64_t idx = (int64_t)(h & (uint64_t)mask);
    while (m->entries[idx].occupied) {
        if (m->entries[idx].hash == h && strcmp(m->entries[idx].key, key) == 0) {
            return 1;
        }
        idx = (idx + 1) & mask;
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

static inline size_t vortex_cached_strlen(const char* s) {
    if (!s) return 0;
    if (s != tl_last_str_s || tl_last_str_seq != tl_rewind_seq) {
        tl_last_str_s = s;
        tl_last_str_len = strlen(s);
        tl_last_str_seq = tl_rewind_seq;
    }
    return tl_last_str_len;
}

int64_t vortex_str_concat(int64_t s1_ptr, int64_t s2_ptr) {
    const char* s1 = (const char*)s1_ptr;
    const char* s2 = (const char*)s2_ptr;
    if (!s1) s1 = "";
    if (!s2) s2 = "";
    size_t len1 = strlen(s1);
    size_t len2 = strlen(s2);
    char* res = (char*)arena_alloc_compact(len1 + len2 + 1);
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
    if (!s || start < 0 || len <= 0) return (int64_t)"";
    size_t total = vortex_cached_strlen(s);
    size_t ustart = (size_t)start;
    if (ustart >= total) return (int64_t)"";
    size_t avail = total - ustart;
    size_t ulen = ((uint64_t)len > (uint64_t)avail) ? avail : (size_t)len;
    char* res = (char*)arena_alloc_compact(ulen + 1);
    memcpy(res, s + ustart, ulen);
    res[ulen] = '\0';
    return (int64_t)res;
}

int64_t vortex_str_from_i64(int64_t n) {
    char tmp[24];
    int pos = 23;
    tmp[pos] = '\0';
    bool neg = (n < 0);
    uint64_t uv = neg ? ((uint64_t)(-(n + 1)) + 1ULL) : (uint64_t)n;
    do {
        tmp[--pos] = (char)('0' + (uv % 10ULL));
        uv /= 10ULL;
    } while (uv > 0);
    if (neg) tmp[--pos] = '-';
    size_t len = (size_t)(23 - pos);
    char* buf = (char*)arena_alloc_compact(len + 1);
    memcpy(buf, tmp + pos, len + 1);
    return (int64_t)buf;
}

int64_t vortex_str_from_f32(int64_t f_bits) {
    float f;
    uint32_t b = (uint32_t)f_bits;
    memcpy(&f, &b, 4);
    char tmp[32];
    int n = snprintf(tmp, sizeof(tmp), "%.4f", (double)f);
    size_t len = (n > 0 && n < (int)sizeof(tmp)) ? (size_t)n : strlen(tmp);
    char* buf = (char*)arena_alloc_compact(len + 1);
    memcpy(buf, tmp, len + 1);
    return (int64_t)buf;
}

int64_t vortex_str_from_f64(int64_t f64_bits) {
    double v;
    memcpy(&v, &f64_bits, 8);
    char tmp[64];
    int n = snprintf(tmp, sizeof(tmp), "%.15g", v);
    size_t len = (n > 0 && n < (int)sizeof(tmp)) ? (size_t)n : strlen(tmp);
    char* buf = (char*)arena_alloc_compact(len + 1);
    memcpy(buf, tmp, len + 1);
    return (int64_t)buf;
}

int64_t vortex_str_byte(int64_t s_ptr, int64_t idx) {
    const char* s = (const char*)s_ptr;
    if (!s || idx < 0) return -1;
    size_t len = vortex_cached_strlen(s);
    if ((size_t)idx >= len) return -1;
    return (int64_t)(unsigned char)s[idx];
}

int64_t vortex_str_from_byte(int64_t ch) {
    char* buf = (char*)arena_alloc_compact(2);
    buf[0] = (char)(ch & 0xFF);
    buf[1] = '\0';
    return (int64_t)buf;
}

int64_t vortex_str_eq(int64_t s1_ptr, int64_t s2_ptr) {
    const char* a = (const char*)s1_ptr;
    const char* b = (const char*)s2_ptr;
    if (a == b) return 1;
    if (!a || !b) return 0;
    return (strcmp(a, b) == 0) ? 1 : 0;
}

int64_t vortex_str_find(int64_t haystack_ptr, int64_t needle_ptr, int64_t start_idx) {
    const char* h = (const char*)haystack_ptr;
    const char* n = (const char*)needle_ptr;
    if (!h || !n) return -1;
    if (start_idx < 0) start_idx = 0;
    size_t hlen = vortex_cached_strlen(h);
    if ((size_t)start_idx > hlen) return -1;
    if (n[0] == '\0') return start_idx;
    if (n[1] == '\0') {
        const char* found1 = strchr(h + start_idx, (unsigned char)n[0]);
        return found1 ? (int64_t)(found1 - h) : -1;
    }
    const char* found = strstr(h + start_idx, n);
    return found ? (int64_t)(found - h) : -1;
}

int64_t vortex_str_find_any2(int64_t s_ptr, int64_t c1, int64_t c2, int64_t start_idx) {
    const char* s = (const char*)s_ptr;
    if (!s) return -1;
    if (start_idx < 0) start_idx = 0;
    size_t len = vortex_cached_strlen(s);
    if ((size_t)start_idx >= len) return -1;
    unsigned char b1 = (unsigned char)c1;
    unsigned char b2 = (unsigned char)c2;
    const unsigned char* p = (const unsigned char*)(s + start_idx);
    const unsigned char* end = (const unsigned char*)(s + len);
    while (p < end) {
        unsigned char ch = *p;
        if (ch == b1 || ch == b2) return (int64_t)(p - (const unsigned char*)s);
        p++;
    }
    return -1;
}

int64_t vortex_str_join(int64_t vec_ptr, int64_t sep_ptr) {
    VortexVec* v = (VortexVec*)vec_ptr;
    if (!v || v->len <= 0) return (int64_t)"";
    const char* sep = (const char*)sep_ptr;
    if (!sep) sep = "";
    size_t sep_len = strlen(sep);

    int64_t n = v->len;
    size_t small_lens[128];
    size_t* lens = (n <= 128) ? small_lens : (size_t*)malloc((size_t)n * sizeof(size_t));
    if (!lens) {
        fprintf(stderr, "[VORTEX MEMORY ERROR] Out of memory in str_join\n");
        vortex_error("");
    }

    size_t total = (sep_len > 0 && n > 1) ? sep_len * (size_t)(n - 1) : 0;
    for (int64_t i = 0; i < n; i++) {
        const char* s = (const char*)v->data[i];
        size_t sl = s ? strlen(s) : 0;
        lens[i] = sl;
        total += sl;
    }

    char* res = (char*)arena_alloc_compact(total + 1);
    char* out = res;
    for (int64_t i = 0; i < n; i++) {
        if (i > 0 && sep_len > 0) {
            memcpy(out, sep, sep_len);
            out += sep_len;
        }
        size_t sl = lens[i];
        if (sl > 0) {
            memcpy(out, (const char*)v->data[i], sl);
            out += sl;
        }
    }
    *out = '\0';
    if (lens != small_lens) free(lens);
    return (int64_t)res;
}

int64_t vortex_str_to_i64(int64_t s_ptr) {
    const char* s = (const char*)s_ptr;
    if (!s) return 0;
    return (int64_t)strtoll(s, NULL, 10);
}

int64_t vortex_ptr_to_str(int64_t ptr_val) {
    const char* s = (const char*)ptr_val;
    if (!s) {
        char* empty = (char*)arena_alloc_compact(1);
        empty[0] = '\0';
        return (int64_t)empty;
    }
    size_t len = strlen(s);
    char* copy = (char*)arena_alloc_compact(len + 1);
    memcpy(copy, s, len + 1);
    return (int64_t)copy;
}
