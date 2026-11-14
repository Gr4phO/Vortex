/*
 * VORTEX STANDALONE EXECUTABLE PACKAGER & EMBEDDED PAYLOAD RUNNER
 */

#include "vortex.h"
#ifdef __APPLE__
#include <mach-o/dyld.h>
#include <sys/wait.h>
#elif !defined(_WIN32)
#include <sys/wait.h>
#endif

/* --- Standalone Executable Packaging (vortex build) --- */
bool check_and_run_embedded_payload(int argc, char** argv) {
    (void)argc;
    char self_path[1024] = {0};
    vortex_get_self_path(self_path, sizeof(self_path), (argc > 0 && argv) ? argv[0] : NULL);

    if (self_path[0] == '\0') return false;

    FILE* f = fopen(self_path, "rb");
    if (!f) return false;

    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return false;
    }
    long file_size = ftell(f);
    if (file_size < (long)sizeof(VortexPackageFooter)) {
        fclose(f);
        return false;
    }

    if (fseek(f, file_size - (long)sizeof(VortexPackageFooter), SEEK_SET) != 0) {
        fclose(f);
        return false;
    }

    VortexPackageFooter footer;
    if (fread(&footer, sizeof(footer), 1, f) != 1) {
        fclose(f);
        return false;
    }

    if (memcmp(footer.footer_magic, VORTEX_PACK_MAGIC_FOOTER, 8) != 0) {
        fclose(f);
        return false;
    }

    g_is_packaged_app = true;

    if (footer.payload_offset >= (uint64_t)(file_size - sizeof(VortexPackageFooter))) {
        fclose(f);
        fprintf(stderr, "[VORTEX PACKAGE ERROR] Invalid embedded payload offset (%llu)\n",
                (unsigned long long)footer.payload_offset);
        vortex_error("");
    }

    if (fseek(f, (long)footer.payload_offset, SEEK_SET) != 0) {
        fclose(f);
        fprintf(stderr, "[VORTEX PACKAGE ERROR] Failed to seek to embedded payload\n");
        vortex_error("");
    }

    VortexPackageHeader hdr;
    if (fread(&hdr, sizeof(hdr), 1, f) != 1) {
        fclose(f);
        fprintf(stderr, "[VORTEX PACKAGE ERROR] Failed to read embedded package header\n");
        vortex_error("");
    }

    if (memcmp(hdr.header_magic, VORTEX_PACK_MAGIC_HEADER, 4) != 0) {
        fclose(f);
        fprintf(stderr, "[VORTEX PACKAGE ERROR] Corrupted package header magic\n");
        vortex_error("");
    }

    if (hdr.version != VORTEX_PACK_VERSION) {
        fclose(f);
        fprintf(stderr, "[VORTEX PACKAGE ERROR] Unsupported package version (%u)\n", hdr.version);
        vortex_error("");
    }

    if (hdr.source_len == 0 || hdr.source_len > 100 * 1024 * 1024) {
        fclose(f);
        fprintf(stderr, "[VORTEX PACKAGE ERROR] Invalid embedded source size (%u bytes)\n", hdr.source_len);
        vortex_error("");
    }

    char* src_buf = (char*)malloc(hdr.source_len + 1);
    if (!src_buf) {
        fclose(f);
        fprintf(stderr, "[VORTEX PACKAGE ERROR] Out of memory loading embedded payload (%u bytes)\n", hdr.source_len);
        vortex_error("");
    }

    size_t read_bytes = fread(src_buf, 1, hdr.source_len, f);
    src_buf[read_bytes] = '\0';
    fclose(f);

    if (read_bytes != hdr.source_len) {
        free(src_buf);
        fprintf(stderr, "[VORTEX PACKAGE ERROR] Truncated embedded payload (%zu/%u bytes read)\n",
                read_bytes, hdr.source_len);
        vortex_error("");
    }

    g_compile_mode = (hdr.flags & VORTEX_PACK_FLAG_RELEASE) ? VORTEX_MODE_RELEASE : VORTEX_MODE_SAFE;
    g_current_filename = "<packaged>";
    int res = execute_vortex_source(src_buf);
    free(src_buf);
    if (res != 0) {
        fprintf(stderr, "[VORTEX PACKAGE ERROR] Packaged application exited with status %d\n", res);
        vortex_wait_on_standalone_error("Packaged application initialization or execution failed.");
    }
    exit(res);
    return true;
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
} VxElf64Ehdr;

typedef struct {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
} VxElf64Phdr;

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
} VxElf64Shdr;

typedef struct {
    uint32_t magic;
    int32_t  cputype;
    int32_t  cpusubtype;
    uint32_t filetype;
    uint32_t ncmds;
    uint32_t sizeofcmds;
    uint32_t flags;
    uint32_t reserved;
} VxMachHeader64;

typedef struct {
    uint32_t cmd;
    uint32_t cmdsize;
    char     segname[16];
    uint64_t vmaddr;
    uint64_t vmsize;
    uint64_t fileoff;
    uint64_t filesize;
    int32_t  maxprot;
    int32_t  initprot;
    uint32_t nsects;
    uint32_t flags;
} VxMachSegCmd64;

typedef struct {
    char     sectname[16];
    char     segname[16];
    uint64_t addr;
    uint64_t size;
    uint32_t offset;
    uint32_t align;
    uint32_t reloff;
    uint32_t nreloc;
    uint32_t flags;
    uint32_t reserved1;
    uint32_t reserved2;
    uint32_t reserved3;
} VxMachSect64;

typedef struct {
    uint32_t cmd;
    uint32_t cmdsize;
    uint64_t entryoff;
    uint64_t stacksize;
} VxMachMainCmd;
#pragma pack(pop)

static bool write_zero_bytes(FILE* f, size_t count) {
    static const uint8_t zeros[256] = {0};
    while (count > 0) {
        size_t chunk = (count < sizeof(zeros)) ? count : sizeof(zeros);
        if (fwrite(zeros, 1, chunk, f) != chunk) return false;
        count -= chunk;
    }
    return true;
}

static long probe_runtime_clean_size(FILE* f, long file_size) {
    long clean_size = file_size;
    if (file_size >= (long)sizeof(VortexPackageFooter)) {
        if (fseek(f, file_size - (long)sizeof(VortexPackageFooter), SEEK_SET) == 0) {
            VortexPackageFooter prev_footer;
            if (fread(&prev_footer, sizeof(prev_footer), 1, f) == 1 &&
                memcmp(prev_footer.footer_magic, VORTEX_PACK_MAGIC_FOOTER, 8) == 0 &&
                prev_footer.payload_offset < (uint64_t)(file_size - (long)sizeof(VortexPackageFooter))) {
                clean_size = (long)prev_footer.payload_offset;
            }
        }
    }
    fseek(f, 0, SEEK_SET);
    return clean_size;
}

static bool copy_runtime_prefix(FILE* src, FILE* dst, long remaining) {
    char copy_buf[65536];
    while (remaining > 0) {
        size_t chunk = (remaining < (long)sizeof(copy_buf)) ? (size_t)remaining : sizeof(copy_buf);
        size_t n = fread(copy_buf, 1, chunk, src);
        if (n == 0) break;
        if (fwrite(copy_buf, 1, n, dst) != n) return false;
        remaining -= (long)n;
    }
    return remaining == 0;
}

static bool write_package_payload_and_footer(FILE* out_f, const char* src_buf, size_t src_len, uint64_t payload_offset) {
    VortexPackageHeader hdr;
    memcpy(hdr.header_magic, VORTEX_PACK_MAGIC_HEADER, 4);
    hdr.version = VORTEX_PACK_VERSION;
    hdr.flags = (g_compile_mode == VORTEX_MODE_RELEASE) ? VORTEX_PACK_FLAG_RELEASE : 0;
    hdr.num_structs = (uint32_t)g_struct_type_count;
    hdr.num_constants = 0;
    hdr.source_len = (uint32_t)src_len;

    if (fwrite(&hdr, sizeof(hdr), 1, out_f) != 1 ||
        fwrite(src_buf, 1, src_len, out_f) != src_len) {
        return false;
    }

    VortexPackageFooter footer;
    footer.payload_offset = payload_offset;
    memcpy(footer.footer_magic, VORTEX_PACK_MAGIC_FOOTER, 8);
    return fwrite(&footer, sizeof(footer), 1, out_f) == 1;
}

static void ad_hoc_codesign_if_apple(const char* output_path) {
#ifdef __APPLE__
    if (g_target_abi == VORTEX_ABI_DARWIN64) {
        pid_t pid = fork();
        if (pid == 0) {
            const char* cs_argv[] = {"codesign", "--force", "--sign", "-", output_path, NULL};
            execvp("codesign", (char* const*)cs_argv);
            _exit(127);
        } else if (pid > 0) {
            int status = 0;
            waitpid(pid, &status, 0);
        }
    }
#else
    (void)output_path;
#endif
}

static FILE* open_companion_runtime(const char* self_path, const VortexTargetDesc* td, char* out_found, size_t cap) {
    const char* ext = (td->abi == VORTEX_ABI_WIN64) ? ".exe" : "";
    char dir[1024];
    vx_str_copy(dir, sizeof(dir), self_path);
    char* slash1 = strrchr(dir, '/');
    char* slash2 = strrchr(dir, '\\');
    char* sep = (slash1 && slash2) ? ((slash1 > slash2) ? slash1 : slash2) : (slash1 ? slash1 : slash2);
    if (sep) {
        *sep = '\0';
    } else {
        vx_str_copy(dir, sizeof(dir), ".");
    }

    char cand[1280];
    snprintf(cand, sizeof(cand), "%s/vortex-%s%s", dir, td->name, ext);
    vx_str_copy(out_found, cap, cand);
    FILE* f = fopen(out_found, "rb");
    if (f) return f;

    snprintf(cand, sizeof(cand), "%s/targets/vortex-%s%s", dir, td->name, ext);
    vx_str_copy(out_found, cap, cand);
    return fopen(out_found, "rb");
}

static bool emit_elf64_arm64_binary(const char* output_path, const uint8_t* jit_code, size_t jit_size,
                                    const char* src_buf, size_t src_len,
                                    long* out_code_bytes, long* out_total_bytes) {
    static const char shstrtab[] = "\0.text\0.shstrtab\0";
    const uint64_t vaddr_base = 0x400000ULL;
    const uint64_t text_off = 0x1000ULL;
    const uint32_t stub[4] = {
        0x94000004U, /* BL +16 (enter Vortex JIT main) */
        0xD2800000U, /* MOVZ X0, #0 */
        0xD2800BA8U, /* MOVZ X8, #93 (__NR_exit on Linux ARM64) */
        0xD4000001U  /* SVC #0 */
    };
    uint64_t text_size = sizeof(stub) + (uint64_t)jit_size;
    uint64_t shstr_off = text_off + text_size;
    uint64_t shoff = (shstr_off + sizeof(shstrtab) + 7ULL) & ~7ULL;

    VxElf64Ehdr ehdr;
    memset(&ehdr, 0, sizeof(ehdr));
    ehdr.e_ident[0] = 0x7F; ehdr.e_ident[1] = 'E'; ehdr.e_ident[2] = 'L'; ehdr.e_ident[3] = 'F';
    ehdr.e_ident[4] = 2; /* ELFCLASS64 */
    ehdr.e_ident[5] = 1; /* ELFDATA2LSB */
    ehdr.e_ident[6] = 1; /* EV_CURRENT */
    ehdr.e_type = 2;     /* ET_EXEC */
    ehdr.e_machine = 183;/* EM_AARCH64 */
    ehdr.e_version = 1;
    ehdr.e_entry = vaddr_base + text_off;
    ehdr.e_phoff = sizeof(VxElf64Ehdr);
    ehdr.e_shoff = shoff;
    ehdr.e_ehsize = sizeof(VxElf64Ehdr);
    ehdr.e_phentsize = sizeof(VxElf64Phdr);
    ehdr.e_phnum = 1;
    ehdr.e_shentsize = sizeof(VxElf64Shdr);
    ehdr.e_shnum = 3;
    ehdr.e_shstrndx = 2;

    VxElf64Phdr phdr;
    memset(&phdr, 0, sizeof(phdr));
    phdr.p_type = 1; /* PT_LOAD */
    phdr.p_flags = 5;/* PF_R | PF_X */
    phdr.p_offset = 0;
    phdr.p_vaddr = vaddr_base;
    phdr.p_paddr = vaddr_base;
    phdr.p_filesz = text_off + text_size;
    phdr.p_memsz = text_off + text_size;
    phdr.p_align = 0x10000ULL;

    VxElf64Shdr shdrs[3];
    memset(shdrs, 0, sizeof(shdrs));
    shdrs[1].sh_name = 1; /* ".text" */
    shdrs[1].sh_type = 1; /* SHT_PROGBITS */
    shdrs[1].sh_flags = 6;/* SHF_ALLOC | SHF_EXECINSTR */
    shdrs[1].sh_addr = vaddr_base + text_off;
    shdrs[1].sh_offset = text_off;
    shdrs[1].sh_size = text_size;
    shdrs[1].sh_addralign = 4;

    shdrs[2].sh_name = 7; /* ".shstrtab" */
    shdrs[2].sh_type = 3; /* SHT_STRTAB */
    shdrs[2].sh_offset = shstr_off;
    shdrs[2].sh_size = sizeof(shstrtab);
    shdrs[2].sh_addralign = 1;

    FILE* out_f = fopen(output_path, "wb");
    if (!out_f) return false;

    bool ok = (fwrite(&ehdr, sizeof(ehdr), 1, out_f) == 1) &&
              (fwrite(&phdr, sizeof(phdr), 1, out_f) == 1) &&
              write_zero_bytes(out_f, (size_t)(text_off - (sizeof(ehdr) + sizeof(phdr)))) &&
              (fwrite(stub, sizeof(stub), 1, out_f) == 1) &&
              (jit_size == 0 || fwrite(jit_code, 1, jit_size, out_f) == jit_size) &&
              (fwrite(shstrtab, sizeof(shstrtab), 1, out_f) == 1) &&
              write_zero_bytes(out_f, (size_t)(shoff - (shstr_off + sizeof(shstrtab)))) &&
              (fwrite(shdrs, sizeof(shdrs), 1, out_f) == 1);

    if (ok) {
        uint64_t payload_off = (uint64_t)ftell(out_f);
        ok = write_package_payload_and_footer(out_f, src_buf, src_len, payload_off);
    }

    *out_code_bytes = (long)text_size;
    *out_total_bytes = ftell(out_f);
    fclose(out_f);
    return ok;
}

static bool emit_macho64_arm64_binary(const char* output_path, const uint8_t* jit_code, size_t jit_size,
                                      const char* src_buf, size_t src_len,
                                      long* out_code_bytes, long* out_total_bytes) {
    const uint64_t text_vmaddr = 0x100000000ULL;
    const uint64_t text_off = 0x1000ULL;
    const uint32_t stub[4] = {
        0x94000004U, /* BL +16 (enter Vortex JIT main) */
        0xD2800000U, /* MOVZ X0, #0 */
        0xD2800030U, /* MOVZ X16, #1 (SYS_exit on Darwin ARM64) */
        0xD4001001U  /* SVC #0x80 */
    };
    uint64_t text_size = sizeof(stub) + (uint64_t)jit_size;
    uint64_t text_end = text_off + text_size;
    uint64_t seg_size = (text_end + 0x3FFFULL) & ~0x3FFFULL; /* 16 KiB Apple Silicon page */

    uint32_t cmds_size = (uint32_t)(sizeof(VxMachSegCmd64) + sizeof(VxMachSegCmd64) +
                                    sizeof(VxMachSect64) + sizeof(VxMachMainCmd));

    VxMachHeader64 mh;
    memset(&mh, 0, sizeof(mh));
    mh.magic = 0xFEEDFACFU;     /* MH_MAGIC_64 */
    mh.cputype = 0x0100000C;    /* CPU_TYPE_ARM64 */
    mh.cpusubtype = 0;          /* CPU_SUBTYPE_ARM64_ALL */
    mh.filetype = 2;            /* MH_EXECUTE */
    mh.ncmds = 3;
    mh.sizeofcmds = cmds_size;
    mh.flags = 0x85U;           /* MH_NOUNDEFS | MH_DYLDLINK | MH_TWOLEVEL */

    VxMachSegCmd64 pagezero;
    memset(&pagezero, 0, sizeof(pagezero));
    pagezero.cmd = 0x19U;       /* LC_SEGMENT_64 */
    pagezero.cmdsize = sizeof(VxMachSegCmd64);
    memcpy(pagezero.segname, "__PAGEZERO", 10);
    pagezero.vmsize = 0x100000000ULL;

    VxMachSegCmd64 text_seg;
    memset(&text_seg, 0, sizeof(text_seg));
    text_seg.cmd = 0x19U;
    text_seg.cmdsize = sizeof(VxMachSegCmd64) + sizeof(VxMachSect64);
    memcpy(text_seg.segname, "__TEXT", 6);
    text_seg.vmaddr = text_vmaddr;
    text_seg.vmsize = seg_size;
    text_seg.fileoff = 0;
    text_seg.filesize = seg_size;
    text_seg.maxprot = 5;       /* VM_PROT_READ | VM_PROT_EXECUTE */
    text_seg.initprot = 5;
    text_seg.nsects = 1;

    VxMachSect64 text_sect;
    memset(&text_sect, 0, sizeof(text_sect));
    memcpy(text_sect.sectname, "__text", 6);
    memcpy(text_sect.segname, "__TEXT", 6);
    text_sect.addr = text_vmaddr + text_off;
    text_sect.size = text_size;
    text_sect.offset = (uint32_t)text_off;
    text_sect.align = 2;        /* 2^2 = 4-byte alignment */
    text_sect.flags = 0x80000400U; /* S_ATTR_PURE_INSTRUCTIONS | S_ATTR_SOME_INSTRUCTIONS */

    VxMachMainCmd main_cmd;
    memset(&main_cmd, 0, sizeof(main_cmd));
    main_cmd.cmd = 0x80000028U; /* LC_MAIN */
    main_cmd.cmdsize = sizeof(VxMachMainCmd);
    main_cmd.entryoff = text_off;

    size_t hdr_bytes = sizeof(mh) + cmds_size;

    FILE* out_f = fopen(output_path, "wb");
    if (!out_f) return false;

    bool ok = (fwrite(&mh, sizeof(mh), 1, out_f) == 1) &&
              (fwrite(&pagezero, sizeof(pagezero), 1, out_f) == 1) &&
              (fwrite(&text_seg, sizeof(text_seg), 1, out_f) == 1) &&
              (fwrite(&text_sect, sizeof(text_sect), 1, out_f) == 1) &&
              (fwrite(&main_cmd, sizeof(main_cmd), 1, out_f) == 1) &&
              write_zero_bytes(out_f, (size_t)(text_off - hdr_bytes)) &&
              (fwrite(stub, sizeof(stub), 1, out_f) == 1) &&
              (jit_size == 0 || fwrite(jit_code, 1, jit_size, out_f) == jit_size) &&
              write_zero_bytes(out_f, (size_t)(seg_size - text_end));

    if (ok) {
        uint64_t payload_off = (uint64_t)ftell(out_f);
        ok = write_package_payload_and_footer(out_f, src_buf, src_len, payload_off);
    }

    *out_code_bytes = (long)text_size;
    *out_total_bytes = ftell(out_f);
    fclose(out_f);
    return ok;
}

int vortex_cmd_build(int argc, char** argv) {
    const char* input_path = NULL;
    char output_path[1024] = {0};

    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--release") == 0) {
            g_compile_mode = VORTEX_MODE_RELEASE;
        } else if (strcmp(argv[i], "--safe") == 0) {
            g_compile_mode = VORTEX_MODE_SAFE;
        } else if (strcmp(argv[i], "--target") == 0 && i + 1 < argc) {
            const char* tname = argv[++i];
            if (!vortex_set_target_name(tname)) {
                fprintf(stderr, "[VORTEX BUILD ERROR] Unknown target '%s'\n", tname);
                return 1;
            }
        } else if (strncmp(argv[i], "--target=", 9) == 0) {
            if (!vortex_set_target_name(argv[i] + 9)) {
                fprintf(stderr, "[VORTEX BUILD ERROR] Unknown target '%s'\n", argv[i] + 9);
                return 1;
            }
        } else if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
            vx_str_copy(output_path, sizeof(output_path), argv[++i]);
        } else if (argv[i][0] != '-') {
            if (!input_path) input_path = argv[i];
            else if (output_path[0] == '\0') vx_str_copy(output_path, sizeof(output_path), argv[i]);
        }
    }

    if (!input_path) {
        fprintf(stderr, "[VORTEX BUILD ERROR] Missing input script file\n");
        fprintf(stderr, "Usage: vortex build <script.vx> [-o <output>] [--safe|--release] [--target <triple>]\n");
        return 1;
    }

    const VortexTargetDesc* td = vortex_get_target_desc();
    if (output_path[0] == '\0') {
        vx_str_copy(output_path, sizeof(output_path), input_path);
        char* dot = strrchr(output_path, '.');
        if (dot) *dot = '\0';
        if (td->abi == VORTEX_ABI_WIN64) {
            size_t cur = strlen(output_path);
            if (cur + 4 < sizeof(output_path)) memcpy(output_path + cur, ".exe", 5);
        }
    }

    char* src_buf = vortex_resolve_imports(input_path);
    if (!src_buf) {
        return 1;
    }
    size_t r = strlen(src_buf);
    if (r == 0) {
        free(src_buf);
        fprintf(stderr, "[VORTEX BUILD ERROR] Source file '%s' is empty or unreadable\n", input_path);
        return 1;
    }

    char self_path[1024] = {0};
    vortex_get_self_path(self_path, sizeof(self_path), argv ? argv[0] : NULL);

    FILE* rt_f = NULL;
    char companion_path[1024] = {0};
    bool is_cross = vortex_is_cross_target();

    if (!is_cross) {
        rt_f = fopen(self_path, "rb");
    } else {
        rt_f = open_companion_runtime(self_path, td, companion_path, sizeof(companion_path));
    }

    if (rt_f) {
        fseek(rt_f, 0, SEEK_END);
        long rt_size = ftell(rt_f);
        long runtime_clean_size = probe_runtime_clean_size(rt_f, rt_size);
        if (runtime_clean_size <= 0) {
            fprintf(stderr, "[VORTEX BUILD ERROR] Invalid base runtime executable size\n");
            fclose(rt_f);
            free(src_buf);
            return 1;
        }

        FILE* out_f = fopen(output_path, "wb");
        if (!out_f) {
            fprintf(stderr, "[VORTEX BUILD ERROR] Cannot open output executable '%s' for writing\n", output_path);
            fclose(rt_f);
            free(src_buf);
            return 1;
        }

        if (!copy_runtime_prefix(rt_f, out_f, runtime_clean_size)) {
            fprintf(stderr, "[VORTEX BUILD ERROR] Write error while copying base runtime\n");
            fclose(rt_f);
            fclose(out_f);
            free(src_buf);
            return 1;
        }
        fclose(rt_f);

        uint64_t payload_offset = (uint64_t)ftell(out_f);
        bool ok = write_package_payload_and_footer(out_f, src_buf, r, payload_offset);
        free(src_buf);
        if (!ok) {
            fprintf(stderr, "[VORTEX BUILD ERROR] Failed to write embedded payload to '%s'\n", output_path);
            fclose(out_f);
            return 1;
        }

        long final_size = ftell(out_f);
        fclose(out_f);
        ad_hoc_codesign_if_apple(output_path);
#ifndef _WIN32
        chmod(output_path, 0755);
#endif
        printf("[VORTEX BUILD] Executable standalone generated (%s): %s\n", td->name, output_path);
        printf("               Runtime: %ld bytes | Payload: %zu bytes | Total: %ld bytes\n",
               runtime_clean_size, r + sizeof(VortexPackageHeader) + sizeof(VortexPackageFooter), final_size);
        printf("               Payload offset: %llu (0x%llX)\n",
               (unsigned long long)payload_offset, (unsigned long long)payload_offset);
        return 0;
    }

    if (td->arch == VORTEX_ARCH_ARM64) {
        g_current_filename = input_path;
        void* code_mem = alloc_exec_page(MAX_JIT_CODE_SIZE);
        if (!code_mem) {
            free(src_buf);
            fprintf(stderr, "[VORTEX BUILD ERROR] Out of memory compiling ARM64 target\n");
            return 1;
        }
        JITBuffer jit;
        compile_vortex_source_internal(src_buf, code_mem, &jit);

        long code_bytes = 0;
        long total_bytes = 0;
        const char* fmt_name = (td->abi == VORTEX_ABI_DARWIN64) ? "Mach-O 64 ARM64" : "ELF64 AArch64";
        bool ok = (td->abi == VORTEX_ABI_DARWIN64)
                  ? emit_macho64_arm64_binary(output_path, jit.code, jit.size, src_buf, r, &code_bytes, &total_bytes)
                  : emit_elf64_arm64_binary(output_path, jit.code, jit.size, src_buf, r, &code_bytes, &total_bytes);
        free_exec_page(code_mem, MAX_JIT_CODE_SIZE);
        free(src_buf);
        if (!ok) {
            fprintf(stderr, "[VORTEX BUILD ERROR] Failed to write %s output '%s'\n", fmt_name, output_path);
            return 1;
        }
        ad_hoc_codesign_if_apple(output_path);
#ifndef _WIN32
        chmod(output_path, 0755);
#endif
        printf("[VORTEX BUILD] Cross-compiled %s standalone binary: %s\n", fmt_name, output_path);
        printf("               Target: %s | Code (.text): %ld bytes | Total: %ld bytes\n",
               td->name, code_bytes, total_bytes);
        printf("               Note: Place companion runtime 'vortex-%s' beside binary for full dynamic FFI runtime cloning.\n",
               td->name);
        return 0;
    }

    free(src_buf);
    fprintf(stderr, "[VORTEX BUILD ERROR] Cannot open runtime executable '%s' (companion '%s' not found)\n",
            self_path, companion_path[0] ? companion_path : td->name);
    return 1;
}

