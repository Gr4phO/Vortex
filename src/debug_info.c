/*
 * VORTEX PROGRAMMING LANGUAGE - NATIVE DEBUGGING, STACKTRACES, DWARF & WIN64 UNWIND
 * Zero-overhead line tables, readable panic/crash stacktraces, GDB/LLDB JIT DWARF,
 * and Windows RtlAddFunctionTable / optional DbgHelp symbols.
 */

#include "vortex.h"
#include <stddef.h>

#ifndef _WIN32
#include <signal.h>
#if defined(__linux__) && !defined(_GNU_SOURCE)
#include <ucontext.h>
#elif defined(__linux__)
#include <ucontext.h>
#elif defined(__APPLE__)
#include <sys/ucontext.h>
#endif
#endif

#define MAX_DEBUG_LINES 8192
#define MAX_DEBUG_FUNCS 512

static VortexDebugLineEntry g_debug_lines[MAX_DEBUG_LINES];
static int g_debug_line_count = 0;

static VortexDebugFuncInfo g_debug_funcs[MAX_DEBUG_FUNCS];
static int g_debug_func_count = 0;

static char g_current_debug_func[64] = "__main__";
static void* g_jit_base = NULL;
static size_t g_jit_size = 0;
static volatile bool g_in_jit_exec = false;

const char* g_dump_debug_path = NULL;
bool g_lsp_capture_errors = false;

/* ========================================================================= */
/* 1. JIT LINE & FUNCTION SOURCE MAP (0 RUNTIME OVERHEAD)                    */
/* ========================================================================= */

void debug_info_reset(void) {
    debug_info_unregister_jit_image();
    g_debug_line_count = 0;
    g_debug_func_count = 0;
    strcpy(g_current_debug_func, "__main__");
    g_jit_base = NULL;
    g_jit_size = 0;
    g_in_jit_exec = false;
}

void debug_info_set_current_func(const char* name) {
    if (name && name[0]) {
        strncpy(g_current_debug_func, name, sizeof(g_current_debug_func) - 1);
        g_current_debug_func[sizeof(g_current_debug_func) - 1] = '\0';
    } else {
        strcpy(g_current_debug_func, "__main__");
    }
}

void debug_info_record_func(const char* name, size_t start_off, size_t end_off,
                            int frame_size, bool is_main, const char* file, int line) {
    if (g_debug_func_count >= MAX_DEBUG_FUNCS || end_off <= start_off) return;
    VortexDebugFuncInfo* fn = &g_debug_funcs[g_debug_func_count++];
    memset(fn, 0, sizeof(*fn));
    strncpy(fn->name, (name && name[0]) ? name : "__main__", sizeof(fn->name) - 1);
    fn->start_offset = start_off;
    fn->end_offset = end_off;
    fn->frame_size = frame_size;
    fn->is_main = is_main;
    fn->file = vortex_intern_cstr(file ? file : "<input>");
    fn->line = (line > 0) ? line : 1;
}

void debug_info_record_line(size_t start_off, size_t end_off, int line, const char* file) {
    if (line <= 0 || end_off <= start_off) return;
    const char* fstr = vortex_intern_cstr(file ? file : "<input>");
    const char* fn_str = vortex_intern_cstr(g_current_debug_func);

    if (g_debug_line_count > 0) {
        VortexDebugLineEntry* prev = &g_debug_lines[g_debug_line_count - 1];
        if (prev->line == line && strcmp(prev->file, fstr) == 0 &&
            strcmp(prev->func_name, fn_str) == 0 && end_off >= prev->start_offset) {
            if (end_off > prev->end_offset) prev->end_offset = end_off;
            return;
        }
    }
    if (g_debug_line_count >= MAX_DEBUG_LINES) return;
    VortexDebugLineEntry* e = &g_debug_lines[g_debug_line_count++];
    e->start_offset = start_off;
    e->end_offset = end_off;
    e->line = line;
    e->file = fstr;
    e->func_name = fn_str;
}

const VortexDebugLineEntry* debug_info_lookup_addr(uintptr_t addr, const char** out_func_name) {
    if (out_func_name) *out_func_name = NULL;
    if (!g_jit_base || g_jit_size == 0) return NULL;
    uintptr_t base = (uintptr_t)g_jit_base;
    if (addr < base || addr >= base + g_jit_size) return NULL;
    size_t off = (size_t)(addr - base);

    if (out_func_name) {
        for (int i = 0; i < g_debug_func_count; i++) {
            if (off >= g_debug_funcs[i].start_offset && off < g_debug_funcs[i].end_offset) {
                *out_func_name = g_debug_funcs[i].name;
                break;
            }
        }
    }

    const VortexDebugLineEntry* best = NULL;
    for (int i = 0; i < g_debug_line_count; i++) {
        const VortexDebugLineEntry* e = &g_debug_lines[i];
        if (off >= e->start_offset && off < e->end_offset) {
            if (out_func_name && !*out_func_name) *out_func_name = e->func_name;
            return e;
        }
        if (off >= e->start_offset) {
            best = e;
        }
    }
    if (best && out_func_name && !*out_func_name) {
        *out_func_name = best->func_name;
    }
    return best;
}

static bool addr_in_jit(uintptr_t addr) {
    if (!g_jit_base || g_jit_size == 0) return false;
    uintptr_t base = (uintptr_t)g_jit_base;
    return (addr >= base && addr < base + g_jit_size);
}

/* ========================================================================= */
/* 2. GDB / LLDB JIT INTERFACE & PURE-C99 DWARF v4 ELF BUILDER               */
/* ========================================================================= */

typedef enum {
    VORTEX_JIT_NOACTION = 0,
    VORTEX_JIT_REGISTER_FN = 1,
    VORTEX_JIT_UNREGISTER_FN = 2
} VortexJitActions;

typedef struct VortexJitCodeEntry {
    struct VortexJitCodeEntry* next_entry;
    struct VortexJitCodeEntry* prev_entry;
    const char* symfile_addr;
    uint64_t symfile_size;
} VortexJitCodeEntry;

typedef struct {
    uint32_t version;
    uint32_t action_flag;
    VortexJitCodeEntry* relevant_entry;
    VortexJitCodeEntry* first_entry;
} VortexJitDescriptor;

VortexJitDescriptor __jit_debug_descriptor = { 1, 0, NULL, NULL };
static VortexJitCodeEntry g_jit_code_entry;
static uint8_t* g_dwarf_elf_buf = NULL;
static size_t g_dwarf_elf_size = 0;

void __attribute__((noinline)) __jit_debug_register_code(void) {
    static volatile uint32_t gdb_hook_barrier = 0;
    gdb_hook_barrier++;
}

typedef struct {
    uint8_t* data;
    size_t len;
    size_t cap;
} DynBuf;

static void dbuf_reserve(DynBuf* b, size_t extra) {
    if (b->len + extra <= b->cap) return;
    size_t ncap = (b->cap == 0) ? 1024 : b->cap * 2;
    while (ncap < b->len + extra) ncap *= 2;
    uint8_t* nb = (uint8_t*)realloc(b->data, ncap);
    if (!nb) return;
    b->data = nb;
    b->cap = ncap;
}

static void dbuf_u8(DynBuf* b, uint8_t v) {
    dbuf_reserve(b, 1);
    if (b->len + 1 <= b->cap) b->data[b->len++] = v;
}

static void dbuf_u16(DynBuf* b, uint16_t v) {
    dbuf_u8(b, (uint8_t)(v & 0xFFU));
    dbuf_u8(b, (uint8_t)((v >> 8) & 0xFFU));
}

static void dbuf_u32(DynBuf* b, uint32_t v) {
    dbuf_u16(b, (uint16_t)(v & 0xFFFFU));
    dbuf_u16(b, (uint16_t)((v >> 16) & 0xFFFFU));
}

static void dbuf_u64(DynBuf* b, uint64_t v) {
    dbuf_u32(b, (uint32_t)(v & 0xFFFFFFFFULL));
    dbuf_u32(b, (uint32_t)((v >> 32) & 0xFFFFFFFFULL));
}

static void dbuf_bytes(DynBuf* b, const void* src, size_t n) {
    if (n == 0) return;
    dbuf_reserve(b, n);
    if (b->len + n <= b->cap) {
        memcpy(b->data + b->len, src, n);
        b->len += n;
    }
}

static void dbuf_cstr(DynBuf* b, const char* s) {
    const char* p = s ? s : "";
    dbuf_bytes(b, p, strlen(p) + 1);
}

static void dbuf_uleb128(DynBuf* b, uint64_t v) {
    do {
        uint8_t byte = (uint8_t)(v & 0x7FU);
        v >>= 7;
        if (v != 0) byte |= 0x80U;
        dbuf_u8(b, byte);
    } while (v != 0);
}

static void dbuf_sleb128(DynBuf* b, int64_t v) {
    bool more = true;
    while (more) {
        uint8_t byte = (uint8_t)(v & 0x7F);
        v >>= 7;
        if ((v == 0 && (byte & 0x40) == 0) || (v == -1 && (byte & 0x40) != 0)) {
            more = false;
        } else {
            byte |= 0x80U;
        }
        dbuf_u8(b, byte);
    }
}

#pragma pack(push, 1)
typedef struct {
    uint8_t  e_ident[16];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;
    uint64_t e_phoff;
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} DbgElf64Ehdr;

typedef struct {
    uint32_t sh_name;
    uint32_t sh_type;
    uint64_t sh_flags;
    uint64_t sh_addr;
    uint64_t sh_offset;
    uint64_t sh_size;
    uint32_t sh_link;
    uint32_t sh_info;
    uint64_t sh_addralign;
    uint64_t sh_entsize;
} DbgElf64Shdr;

typedef struct {
    uint32_t st_name;
    uint8_t  st_info;
    uint8_t  st_other;
    uint16_t st_shndx;
    uint64_t st_value;
    uint64_t st_size;
} DbgElf64Sym;
#pragma pack(pop)

static uint32_t strtab_add(DynBuf* strtab, const char* s) {
    uint32_t off = (uint32_t)strtab->len;
    dbuf_cstr(strtab, s);
    return off;
}

static uint8_t* build_dwarf_elf_image(size_t* out_size) {
    *out_size = 0;
    if (!g_jit_base || g_jit_size == 0) return NULL;
    uint64_t base_addr = (uint64_t)(uintptr_t)g_jit_base;

    /* Collect unique source files for DWARF .debug_line file table */
    const char* unique_files[64];
    int unique_file_count = 0;
    const char* primary_file = (g_current_filename && g_current_filename[0]) ? g_current_filename : "<input>";
    unique_files[unique_file_count++] = primary_file;

    for (int i = 0; i < g_debug_line_count && unique_file_count < 64; i++) {
        const char* f = g_debug_lines[i].file ? g_debug_lines[i].file : primary_file;
        bool seen = false;
        for (int k = 0; k < unique_file_count; k++) {
            if (strcmp(unique_files[k], f) == 0) { seen = true; break; }
        }
        if (!seen) unique_files[unique_file_count++] = f;
    }

    /* 1. .debug_abbrev */
    DynBuf abbrev = {0};
    /* Abbrev 1: DW_TAG_compile_unit (0x11), DW_CHILDREN_yes (1) */
    dbuf_uleb128(&abbrev, 1); dbuf_uleb128(&abbrev, 0x11); dbuf_u8(&abbrev, 1);
    dbuf_uleb128(&abbrev, 0x25); dbuf_uleb128(&abbrev, 0x0E); /* DW_AT_producer : DW_FORM_strp */
    dbuf_uleb128(&abbrev, 0x13); dbuf_uleb128(&abbrev, 0x05); /* DW_AT_language : DW_FORM_data2 (DW_LANG_C99) */
    dbuf_uleb128(&abbrev, 0x03); dbuf_uleb128(&abbrev, 0x0E); /* DW_AT_name : DW_FORM_strp */
    dbuf_uleb128(&abbrev, 0x10); dbuf_uleb128(&abbrev, 0x17); /* DW_AT_stmt_list : DW_FORM_sec_offset */
    dbuf_uleb128(&abbrev, 0x11); dbuf_uleb128(&abbrev, 0x01); /* DW_AT_low_pc : DW_FORM_addr */
    dbuf_uleb128(&abbrev, 0x12); dbuf_uleb128(&abbrev, 0x07); /* DW_AT_high_pc : DW_FORM_data8 */
    dbuf_u8(&abbrev, 0); dbuf_u8(&abbrev, 0);
    /* Abbrev 2: DW_TAG_subprogram (0x2E), DW_CHILDREN_no (0) */
    dbuf_uleb128(&abbrev, 2); dbuf_uleb128(&abbrev, 0x2E); dbuf_u8(&abbrev, 0);
    dbuf_uleb128(&abbrev, 0x03); dbuf_uleb128(&abbrev, 0x0E); /* DW_AT_name : DW_FORM_strp */
    dbuf_uleb128(&abbrev, 0x3A); dbuf_uleb128(&abbrev, 0x0B); /* DW_AT_decl_file : DW_FORM_data1 */
    dbuf_uleb128(&abbrev, 0x3B); dbuf_uleb128(&abbrev, 0x06); /* DW_AT_decl_line : DW_FORM_data4 */
    dbuf_uleb128(&abbrev, 0x11); dbuf_uleb128(&abbrev, 0x01); /* DW_AT_low_pc : DW_FORM_addr */
    dbuf_uleb128(&abbrev, 0x12); dbuf_uleb128(&abbrev, 0x07); /* DW_AT_high_pc : DW_FORM_data8 */
    dbuf_u8(&abbrev, 0); dbuf_u8(&abbrev, 0);
    dbuf_u8(&abbrev, 0); /* end abbrev table */

    /* 2. .debug_str */
    DynBuf dbg_str = {0};
    uint32_t s_prod = strtab_add(&dbg_str, "Vortex JIT " VORTEX_VERSION);
    uint32_t s_cu   = strtab_add(&dbg_str, primary_file);

    /* 3. .debug_info */
    DynBuf dbg_info = {0};
    dbuf_u32(&dbg_info, 0); /* unit_length placeholder */
    dbuf_u16(&dbg_info, 4); /* DWARF v4 */
    dbuf_u32(&dbg_info, 0); /* debug_abbrev_offset */
    dbuf_u8(&dbg_info, 8);  /* address_size = 8 */

    dbuf_uleb128(&dbg_info, 1);
    dbuf_u32(&dbg_info, s_prod);
    dbuf_u16(&dbg_info, 0x000C); /* DW_LANG_C99 */
    dbuf_u32(&dbg_info, s_cu);
    dbuf_u32(&dbg_info, 0);      /* stmt_list = 0 */
    dbuf_u64(&dbg_info, base_addr);
    dbuf_u64(&dbg_info, (uint64_t)g_jit_size);

    /* Symtab + strtab built simultaneously */
    DynBuf strtab = {0};
    dbuf_u8(&strtab, 0);
    DynBuf symtab = {0};
    DbgElf64Sym null_sym;
    memset(&null_sym, 0, sizeof(null_sym));
    dbuf_bytes(&symtab, &null_sym, sizeof(null_sym));

    for (int i = 0; i < g_debug_func_count; i++) {
        const VortexDebugFuncInfo* fn = &g_debug_funcs[i];
        uint32_t fn_dbg_str = strtab_add(&dbg_str, fn->name);
        uint8_t file_id = 1;
        for (int k = 0; k < unique_file_count; k++) {
            if (fn->file && strcmp(unique_files[k], fn->file) == 0) {
                file_id = (uint8_t)(k + 1);
                break;
            }
        }
        dbuf_uleb128(&dbg_info, 2);
        dbuf_u32(&dbg_info, fn_dbg_str);
        dbuf_u8(&dbg_info, file_id);
        dbuf_u32(&dbg_info, (uint32_t)fn->line);
        dbuf_u64(&dbg_info, base_addr + (uint64_t)fn->start_offset);
        dbuf_u64(&dbg_info, (uint64_t)(fn->end_offset - fn->start_offset));

        DbgElf64Sym sym;
        memset(&sym, 0, sizeof(sym));
        sym.st_name = strtab_add(&strtab, fn->name);
        sym.st_info = (uint8_t)((1U << 4) | 2U); /* STB_GLOBAL | STT_FUNC */
        sym.st_shndx = 1; /* .text */
        sym.st_value = base_addr + (uint64_t)fn->start_offset;
        sym.st_size = (uint64_t)(fn->end_offset - fn->start_offset);
        dbuf_bytes(&symtab, &sym, sizeof(sym));
    }
    dbuf_u8(&dbg_info, 0); /* End children of CU */
    uint32_t cu_len = (uint32_t)(dbg_info.len - 4);
    memcpy(dbg_info.data, &cu_len, 4);

    /* 4. .debug_line (DWARF v4 state machine) */
    DynBuf dbg_line = {0};
    dbuf_u32(&dbg_line, 0); /* unit_length placeholder */
    dbuf_u16(&dbg_line, 4); /* DWARF v4 */
    size_t hdr_len_pos = dbg_line.len;
    dbuf_u32(&dbg_line, 0); /* header_length placeholder */
    size_t hdr_body_start = dbg_line.len;
    dbuf_u8(&dbg_line, 1);  /* minimum_instruction_length */
    dbuf_u8(&dbg_line, 1);  /* maximum_operations_per_instruction */
    dbuf_u8(&dbg_line, 1);  /* default_is_stmt */
    dbuf_u8(&dbg_line, (uint8_t)(int8_t)-5); /* line_base */
    dbuf_u8(&dbg_line, 14); /* line_range */
    dbuf_u8(&dbg_line, 13); /* opcode_base */
    static const uint8_t std_op_lens[12] = { 0, 1, 1, 1, 1, 0, 0, 0, 1, 0, 0, 1 };
    dbuf_bytes(&dbg_line, std_op_lens, sizeof(std_op_lens));
    dbuf_u8(&dbg_line, 0); /* include_directories: empty */
    for (int k = 0; k < unique_file_count; k++) {
        dbuf_cstr(&dbg_line, unique_files[k]);
        dbuf_uleb128(&dbg_line, 0); /* dir index */
        dbuf_uleb128(&dbg_line, 0); /* mtime */
        dbuf_uleb128(&dbg_line, 0); /* size */
    }
    dbuf_u8(&dbg_line, 0); /* end file_names */
    uint32_t hdr_len = (uint32_t)(dbg_line.len - hdr_body_start);
    memcpy(dbg_line.data + hdr_len_pos, &hdr_len, 4);

    /* Line program opcodes */
    dbuf_u8(&dbg_line, 0x00); dbuf_uleb128(&dbg_line, 9); dbuf_u8(&dbg_line, 0x02); /* DW_LNE_set_address */
    dbuf_u64(&dbg_line, base_addr);

    uint64_t cur_off = 0;
    int64_t cur_line = 1;
    int cur_file = 1;
    for (int i = 0; i < g_debug_line_count; i++) {
        const VortexDebugLineEntry* e = &g_debug_lines[i];
        int fid = 1;
        for (int k = 0; k < unique_file_count; k++) {
            if (e->file && strcmp(unique_files[k], e->file) == 0) { fid = k + 1; break; }
        }
        if (fid != cur_file) {
            dbuf_u8(&dbg_line, 0x04); /* DW_LNS_set_file */
            dbuf_uleb128(&dbg_line, (uint64_t)fid);
            cur_file = fid;
        }
        if ((uint64_t)e->start_offset >= cur_off) {
            uint64_t d_pc = (uint64_t)e->start_offset - cur_off;
            if (d_pc > 0) {
                dbuf_u8(&dbg_line, 0x02); /* DW_LNS_advance_pc */
                dbuf_uleb128(&dbg_line, d_pc);
                cur_off = (uint64_t)e->start_offset;
            }
        }
        int64_t d_line = (int64_t)e->line - cur_line;
        if (d_line != 0) {
            dbuf_u8(&dbg_line, 0x03); /* DW_LNS_advance_line */
            dbuf_sleb128(&dbg_line, d_line);
            cur_line = e->line;
        }
        dbuf_u8(&dbg_line, 0x01); /* DW_LNS_copy */
    }
    if ((uint64_t)g_jit_size > cur_off) {
        dbuf_u8(&dbg_line, 0x02);
        dbuf_uleb128(&dbg_line, (uint64_t)g_jit_size - cur_off);
    }
    dbuf_u8(&dbg_line, 0x00); dbuf_uleb128(&dbg_line, 1); dbuf_u8(&dbg_line, 0x01); /* DW_LNE_end_sequence */

    uint32_t lp_len = (uint32_t)(dbg_line.len - 4);
    memcpy(dbg_line.data, &lp_len, 4);

    /* 5. Assemble full ELF64 object container */
    static const char shstr[] = "\0.text\0.symtab\0.strtab\0.debug_abbrev\0.debug_info\0.debug_line\0.debug_str\0.shstrtab\0";
    /* Offsets inside shstr:
       1: .text
       7: .symtab
       15: .strtab
       23: .debug_abbrev
       37: .debug_info
       49: .debug_line
       61: .debug_str
       72: .shstrtab */
    DynBuf elf = {0};
    DbgElf64Ehdr ehdr;
    memset(&ehdr, 0, sizeof(ehdr));
    ehdr.e_ident[0] = 0x7F; ehdr.e_ident[1] = 'E'; ehdr.e_ident[2] = 'L'; ehdr.e_ident[3] = 'F';
    ehdr.e_ident[4] = 2; ehdr.e_ident[5] = 1; ehdr.e_ident[6] = 1;
    ehdr.e_type = 1; /* ET_REL / DWARF JIT payload */
    ehdr.e_machine = (g_target_arch == VORTEX_ARCH_ARM64) ? 183 : 62;
    ehdr.e_version = 1;
    ehdr.e_ehsize = sizeof(DbgElf64Ehdr);
    ehdr.e_shentsize = sizeof(DbgElf64Shdr);
    ehdr.e_shnum = 9;
    ehdr.e_shstrndx = 8;
    dbuf_bytes(&elf, &ehdr, sizeof(ehdr));

    uint64_t off_sym = elf.len; dbuf_bytes(&elf, symtab.data, symtab.len);
    uint64_t off_str = elf.len; dbuf_bytes(&elf, strtab.data, strtab.len);
    uint64_t off_abb = elf.len; dbuf_bytes(&elf, abbrev.data, abbrev.len);
    uint64_t off_inf = elf.len; dbuf_bytes(&elf, dbg_info.data, dbg_info.len);
    uint64_t off_lin = elf.len; dbuf_bytes(&elf, dbg_line.data, dbg_line.len);
    uint64_t off_dst = elf.len; dbuf_bytes(&elf, dbg_str.data, dbg_str.len);
    uint64_t off_shs = elf.len; dbuf_bytes(&elf, shstr, sizeof(shstr));

    while ((elf.len & 7U) != 0) dbuf_u8(&elf, 0);
    uint64_t shoff = elf.len;
    memcpy(elf.data + offsetof(DbgElf64Ehdr, e_shoff), &shoff, 8);

    DbgElf64Shdr shdrs[9];
    memset(shdrs, 0, sizeof(shdrs));
    /* [1] .text (SHT_NOBITS referencing runtime JIT range) */
    shdrs[1].sh_name = 1;  shdrs[1].sh_type = 8; shdrs[1].sh_flags = 6;
    shdrs[1].sh_addr = base_addr; shdrs[1].sh_size = (uint64_t)g_jit_size; shdrs[1].sh_addralign = 16;
    /* [2] .symtab */
    shdrs[2].sh_name = 7;  shdrs[2].sh_type = 2; shdrs[2].sh_offset = off_sym; shdrs[2].sh_size = symtab.len;
    shdrs[2].sh_link = 3;  shdrs[2].sh_info = 1; shdrs[2].sh_addralign = 8; shdrs[2].sh_entsize = sizeof(DbgElf64Sym);
    /* [3] .strtab */
    shdrs[3].sh_name = 15; shdrs[3].sh_type = 3; shdrs[3].sh_offset = off_str; shdrs[3].sh_size = strtab.len; shdrs[3].sh_addralign = 1;
    /* [4] .debug_abbrev */
    shdrs[4].sh_name = 23; shdrs[4].sh_type = 1; shdrs[4].sh_offset = off_abb; shdrs[4].sh_size = abbrev.len; shdrs[4].sh_addralign = 1;
    /* [5] .debug_info */
    shdrs[5].sh_name = 37; shdrs[5].sh_type = 1; shdrs[5].sh_offset = off_inf; shdrs[5].sh_size = dbg_info.len; shdrs[5].sh_addralign = 1;
    /* [6] .debug_line */
    shdrs[6].sh_name = 49; shdrs[6].sh_type = 1; shdrs[6].sh_offset = off_lin; shdrs[6].sh_size = dbg_line.len; shdrs[6].sh_addralign = 1;
    /* [7] .debug_str */
    shdrs[7].sh_name = 61; shdrs[7].sh_type = 1; shdrs[7].sh_flags = 0x30; shdrs[7].sh_offset = off_dst; shdrs[7].sh_size = dbg_str.len; shdrs[7].sh_addralign = 1; shdrs[7].sh_entsize = 1;
    /* [8] .shstrtab */
    shdrs[8].sh_name = 72; shdrs[8].sh_type = 3; shdrs[8].sh_offset = off_shs; shdrs[8].sh_size = sizeof(shstr); shdrs[8].sh_addralign = 1;

    dbuf_bytes(&elf, shdrs, sizeof(shdrs));

    free(abbrev.data);
    free(dbg_str.data);
    free(dbg_info.data);
    free(dbg_line.data);
    free(strtab.data);
    free(symtab.data);

    *out_size = elf.len;
    return elf.data;
}

bool debug_write_dwarf_elf(const char* path) {
    if (!path || !path[0]) return false;
    size_t sz = 0;
    uint8_t* buf = build_dwarf_elf_image(&sz);
    if (!buf || sz == 0) {
        free(buf);
        return false;
    }
    FILE* f = fopen(path, "wb");
    if (!f) {
        free(buf);
        return false;
    }
    bool ok = (fwrite(buf, 1, sz, f) == sz);
    fclose(f);
    free(buf);
    return ok;
}

/* ========================================================================= */
/* 3. WINDOWS RUNTIME_FUNCTION + UNWIND_INFO & OPTIONAL DbgHelp SYMBOLS      */
/* ========================================================================= */

#if defined(_WIN32) && (defined(__x86_64__) || defined(_M_X64))
static RUNTIME_FUNCTION g_win_rt_funcs[MAX_DEBUG_FUNCS];
static DWORD g_win_rt_count = 0;
static bool g_win_rt_registered = false;

static void register_win64_unwind_and_symbols(JITBuffer* jit) {
    if (!g_jit_base || !jit || g_debug_func_count == 0) return;
    size_t aligned_uw = (jit->size + 3U) & ~(size_t)3U;
    if (aligned_uw + 8 <= jit->capacity) {
        /* UNWIND_INFO matching push rbp; mov rbp, rsp prologue:
           Version=1, Flags=0, Prolog=4, Codes=2, FrameReg=RBP(5), FrameOff=0
           Code0: offset 4, UWOP_SET_FPREG(3), info 0 -> 0x0304
           Code1: offset 1, UWOP_PUSH_NONVOL(0), RBP(5) -> 0x5001 */
        uint8_t uw[8] = { 0x01, 0x04, 0x02, 0x05, 0x04, 0x03, 0x01, 0x50 };
        memcpy(jit->code + aligned_uw, uw, sizeof(uw));

        g_win_rt_count = 0;
        for (int i = 0; i < g_debug_func_count && g_win_rt_count < MAX_DEBUG_FUNCS; i++) {
            const VortexDebugFuncInfo* fn = &g_debug_funcs[i];
            if (fn->end_offset <= fn->start_offset) continue;
            g_win_rt_funcs[g_win_rt_count].BeginAddress = (DWORD)fn->start_offset;
            g_win_rt_funcs[g_win_rt_count].EndAddress = (DWORD)fn->end_offset;
            g_win_rt_funcs[g_win_rt_count].UnwindData = (DWORD)aligned_uw;
            g_win_rt_count++;
        }
        if (g_win_rt_count > 0) {
            g_win_rt_registered = RtlAddFunctionTable(g_win_rt_funcs, g_win_rt_count, (DWORD64)(uintptr_t)g_jit_base) ? true : false;
        }
    }

    const char* env_dbg = getenv("VORTEX_ENABLE_DBGHELP");
    if (IsDebuggerPresent() || (env_dbg && env_dbg[0] == '1')) {
        HMODULE hDbg = LoadLibraryA("dbghelp.dll");
        if (hDbg) {
            typedef BOOL (WINAPI *PFN_SymInitialize)(HANDLE, PCSTR, BOOL);
            typedef DWORD64 (WINAPI *PFN_SymLoadModuleEx)(HANDLE, HANDLE, PCSTR, PCSTR, DWORD64, DWORD, void*, DWORD);
            typedef BOOL (WINAPI *PFN_SymAddSymbol)(HANDLE, ULONG64, PCSTR, DWORD64, DWORD, DWORD);
            PFN_SymInitialize pSymInit = (PFN_SymInitialize)(void*)GetProcAddress(hDbg, "SymInitialize");
            PFN_SymLoadModuleEx pSymLoad = (PFN_SymLoadModuleEx)(void*)GetProcAddress(hDbg, "SymLoadModuleEx");
            PFN_SymAddSymbol pSymAdd = (PFN_SymAddSymbol)(void*)GetProcAddress(hDbg, "SymAddSymbol");
            if (pSymInit && pSymLoad && pSymAdd) {
                HANDLE proc = GetCurrentProcess();
                pSymInit(proc, NULL, FALSE);
                pSymLoad(proc, NULL, NULL, "vortex_jit", (DWORD64)(uintptr_t)g_jit_base, (DWORD)g_jit_size, NULL, 0);
                for (int i = 0; i < g_debug_func_count; i++) {
                    const VortexDebugFuncInfo* fn = &g_debug_funcs[i];
                    pSymAdd(proc, (ULONG64)(uintptr_t)g_jit_base, fn->name,
                            (DWORD64)((uintptr_t)g_jit_base + fn->start_offset),
                            (DWORD)(fn->end_offset - fn->start_offset), 0);
                }
            }
        }
    }
}
#endif

void debug_info_set_jit_image(void* base, size_t size, JITBuffer* jit_opt) {
    g_jit_base = base;
    g_jit_size = size;

    if (vortex_is_cross_target()) return;

    g_dwarf_elf_buf = build_dwarf_elf_image(&g_dwarf_elf_size);
    if (g_dwarf_elf_buf && g_dwarf_elf_size > 0) {
        memset(&g_jit_code_entry, 0, sizeof(g_jit_code_entry));
        g_jit_code_entry.symfile_addr = (const char*)g_dwarf_elf_buf;
        g_jit_code_entry.symfile_size = (uint64_t)g_dwarf_elf_size;
        __jit_debug_descriptor.relevant_entry = &g_jit_code_entry;
        __jit_debug_descriptor.first_entry = &g_jit_code_entry;
        __jit_debug_descriptor.action_flag = VORTEX_JIT_REGISTER_FN;
        __jit_debug_register_code();
    }

#if defined(_WIN32) && (defined(__x86_64__) || defined(_M_X64))
    register_win64_unwind_and_symbols(jit_opt);
#else
    (void)jit_opt;
#endif
}

void debug_info_unregister_jit_image(void) {
#if defined(_WIN32) && (defined(__x86_64__) || defined(_M_X64))
    if (g_win_rt_registered) {
        RtlDeleteFunctionTable(g_win_rt_funcs);
        g_win_rt_registered = false;
        g_win_rt_count = 0;
    }
#endif
    if (g_dwarf_elf_buf) {
        __jit_debug_descriptor.relevant_entry = &g_jit_code_entry;
        __jit_debug_descriptor.action_flag = VORTEX_JIT_UNREGISTER_FN;
        __jit_debug_register_code();
        __jit_debug_descriptor.first_entry = NULL;
        __jit_debug_descriptor.relevant_entry = NULL;
        free(g_dwarf_elf_buf);
        g_dwarf_elf_buf = NULL;
        g_dwarf_elf_size = 0;
    }
}

/* ========================================================================= */
/* 4. READABLE NATIVE STACKTRACE & CRASH HANDLERS (WINDOWS & POSIX)          */
/* ========================================================================= */

static void print_jit_frame(int idx, uintptr_t lookup_addr, uintptr_t display_addr) {
    const char* fn = NULL;
    const VortexDebugLineEntry* le = debug_info_lookup_addr(lookup_addr, &fn);
    if (le) {
        fprintf(stderr, "  #%d 0x%016llx in %s at %s:%d\n",
                idx, (unsigned long long)display_addr,
                fn ? fn : (le->func_name ? le->func_name : "<jit>"),
                le->file ? le->file : "<input>", le->line);
    } else if (fn) {
        fprintf(stderr, "  #%d 0x%016llx in %s\n",
                idx, (unsigned long long)display_addr, fn);
    } else {
        fprintf(stderr, "  #%d 0x%016llx in <jit+0x%llx>\n",
                idx, (unsigned long long)display_addr,
                (unsigned long long)(display_addr - (uintptr_t)g_jit_base));
    }
}

static void walk_fp_chain(uintptr_t fp, uintptr_t initial_pc, bool has_initial_pc) {
    int frame_idx = 0;
    if (has_initial_pc && addr_in_jit(initial_pc)) {
        print_jit_frame(frame_idx++, initial_pc, initial_pc);
    }
    uintptr_t cur_fp = fp;
    uintptr_t prev_fp = 0;
    for (int depth = 0; depth < 64 && cur_fp > 0x10000ULL && (cur_fp & 7U) == 0; depth++) {
        if (cur_fp <= prev_fp || (cur_fp - prev_fp) > (16ULL * 1024ULL * 1024ULL)) {
            if (prev_fp != 0) break;
        }
        uintptr_t* slots = (uintptr_t*)cur_fp;
        uintptr_t next_fp = slots[0];
        uintptr_t ret_pc = slots[1];
        if (addr_in_jit(ret_pc)) {
            uintptr_t call_site = (ret_pc > (uintptr_t)g_jit_base) ? (ret_pc - 1) : ret_pc;
            print_jit_frame(frame_idx++, call_site, ret_pc);
        }
        prev_fp = cur_fp;
        cur_fp = next_fp;
    }
}

void vortex_print_stacktrace(void* native_ctx) {
    if (!g_jit_base || g_jit_size == 0) return;
    fprintf(stderr, "[VORTEX STACKTRACE]\n");

#ifdef _WIN32
    if (native_ctx != NULL) {
        CONTEXT* ctx = (CONTEXT*)native_ctx;
#if defined(_M_X64) || defined(__x86_64__)
        walk_fp_chain((uintptr_t)ctx->Rbp, (uintptr_t)ctx->Rip, true);
        return;
#elif defined(_M_ARM64) || defined(__aarch64__)
        walk_fp_chain((uintptr_t)ctx->Fp, (uintptr_t)ctx->Pc, true);
        return;
#endif
    }
#if defined(_M_X64) || defined(__x86_64__)
    {
        CONTEXT ctx;
        memset(&ctx, 0, sizeof(ctx));
        RtlCaptureContext(&ctx);
        for (int step = 0; step < 32; step++) {
            if (addr_in_jit((uintptr_t)ctx.Rip)) {
                walk_fp_chain((uintptr_t)ctx.Rbp, (uintptr_t)ctx.Rip, true);
                return;
            }
            DWORD64 img_base = 0;
            PRUNTIME_FUNCTION rf = RtlLookupFunctionEntry(ctx.Rip, &img_base, NULL);
            if (!rf) break;
            PVOID handler_data = NULL;
            DWORD64 estab_frame = 0;
            RtlVirtualUnwind(0, img_base, ctx.Rip, rf, &ctx, &handler_data, &estab_frame, NULL);
            if (ctx.Rip == 0) break;
        }
    }
#endif
    void* bt[64];
    USHORT n = CaptureStackBackTrace(0, 64, bt, NULL);
    int out_idx = 0;
    for (USHORT i = 0; i < n; i++) {
        uintptr_t addr = (uintptr_t)bt[i];
        if (addr_in_jit(addr)) {
            uintptr_t call_site = (addr > (uintptr_t)g_jit_base) ? (addr - 1) : addr;
            print_jit_frame(out_idx++, call_site, addr);
        }
    }
    if (out_idx == 0) {
        void* fp0 = __builtin_frame_address(0);
        walk_fp_chain((uintptr_t)fp0, 0, false);
    }
#else
    if (native_ctx != NULL) {
        ucontext_t* uc = (ucontext_t*)native_ctx;
#if defined(__linux__) && defined(__x86_64__)
        walk_fp_chain((uintptr_t)uc->uc_mcontext.gregs[10], /* REG_RBP */
                      (uintptr_t)uc->uc_mcontext.gregs[16], /* REG_RIP */ true);
        return;
#elif defined(__linux__) && defined(__aarch64__)
        walk_fp_chain((uintptr_t)uc->uc_mcontext.regs[29], (uintptr_t)uc->uc_mcontext.pc, true);
        return;
#elif defined(__APPLE__) && (defined(__aarch64__) || defined(__arm64__))
        walk_fp_chain((uintptr_t)uc->uc_mcontext->__ss.__fp, (uintptr_t)uc->uc_mcontext->__ss.__pc, true);
        return;
#elif defined(__APPLE__) && defined(__x86_64__)
        walk_fp_chain((uintptr_t)uc->uc_mcontext->__ss.__rbp, (uintptr_t)uc->uc_mcontext->__ss.__rip, true);
        return;
#endif
    }
    void* fp0 = __builtin_frame_address(0);
    walk_fp_chain((uintptr_t)fp0, 0, false);
#endif
}

#ifdef _WIN32
static PVOID g_veh_handle = NULL;
static LONG WINAPI vortex_veh_handler(EXCEPTION_POINTERS* info) {
    if (!g_in_jit_exec || !info || !info->ExceptionRecord) return EXCEPTION_CONTINUE_SEARCH;
    DWORD code = info->ExceptionRecord->ExceptionCode;
    if (code != EXCEPTION_ACCESS_VIOLATION &&
        code != EXCEPTION_INT_DIVIDE_BY_ZERO &&
        code != EXCEPTION_ILLEGAL_INSTRUCTION &&
        code != EXCEPTION_STACK_OVERFLOW &&
        code != EXCEPTION_PRIV_INSTRUCTION) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    fprintf(stderr, "\n[VORTEX CRASH] Hardware exception 0x%08lX at address 0x%016llx\n",
            (unsigned long)code, (unsigned long long)(uintptr_t)info->ExceptionRecord->ExceptionAddress);
    vortex_print_stacktrace(info->ContextRecord);
    fflush(stderr);
    ExitProcess(139);
    return EXCEPTION_EXECUTE_HANDLER;
}

void vortex_crash_guard_enter(void) {
    g_in_jit_exec = true;
    if (!g_veh_handle) {
        g_veh_handle = AddVectoredExceptionHandler(1, vortex_veh_handler);
    }
}

void vortex_crash_guard_leave(void) {
    g_in_jit_exec = false;
}
#else
static void vortex_posix_sig_handler(int sig, siginfo_t* si, void* uctx) {
    if (!g_in_jit_exec) _exit(128 + sig);
    fprintf(stderr, "\n[VORTEX CRASH] Signal %d at fault address 0x%016llx\n",
            sig, (unsigned long long)(uintptr_t)(si ? si->si_addr : NULL));
    vortex_print_stacktrace(uctx);
    fflush(stderr);
    _exit(128 + sig);
}

static struct sigaction g_prev_segv, g_prev_bus, g_prev_ill, g_prev_fpe;

void vortex_crash_guard_enter(void) {
    g_in_jit_exec = true;
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = vortex_posix_sig_handler;
    sa.sa_flags = SA_SIGINFO | SA_RESETHAND;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, &g_prev_segv);
    sigaction(SIGBUS,  &sa, &g_prev_bus);
    sigaction(SIGILL,  &sa, &g_prev_ill);
    sigaction(SIGFPE,  &sa, &g_prev_fpe);
}

void vortex_crash_guard_leave(void) {
    g_in_jit_exec = false;
    sigaction(SIGSEGV, &g_prev_segv, NULL);
    sigaction(SIGBUS,  &g_prev_bus,  NULL);
    sigaction(SIGILL,  &g_prev_ill,  NULL);
    sigaction(SIGFPE,  &g_prev_fpe,  NULL);
}
#endif
