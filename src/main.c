/*
 * VORTEX PROGRAMMING LANGUAGE - CLI & INTERACTIVE REPL ENTRYPOINT
 */

#include "vortex.h"

#ifdef VORTEX_SHARED_LIB

extern void* g_vortex_dll_handle;

#ifdef _WIN32
BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved) {
    (void)lpvReserved;
    if (fdwReason == DLL_PROCESS_ATTACH) {
        g_vortex_dll_handle = (void*)hinstDLL;
        DisableThreadLibraryCalls(hinstDLL);
    } else if (fdwReason == DLL_PROCESS_DETACH) {
        vortex_host_shutdown();
    }
    return TRUE;
}
#endif

#else /* !VORTEX_SHARED_LIB */

static const char* vortex_type_name(VortexType t, int struct_id) {
    static char buf[96];
    switch (t) {
        case TYPE_VOID:   return "void";
        case TYPE_I64:    return "i64";
        case TYPE_I32:    return "i32";
        case TYPE_F32:    return "f32";
        case TYPE_F64:    return "f64";
        case TYPE_STR:    return "str";
        case TYPE_PTR:    return "ptr";
        case TYPE_PTR_I64: return "*i64";
        case TYPE_PTR_F32: return "*f32";
        case TYPE_PTR_I32: return "*i32";
        case TYPE_STRUCT: {
            VortexStructType* st = (struct_id >= 0) ? struct_type_find_by_id(struct_id) : NULL;
            if (st) {
                snprintf(buf, sizeof(buf), "struct %s", st->name);
                return buf;
            }
            return "struct";
        }
        case TYPE_PTR_STRUCT: {
            VortexStructType* st = (struct_id >= 0) ? struct_type_find_by_id(struct_id) : NULL;
            if (st) {
                snprintf(buf, sizeof(buf), "*struct %s", st->name);
                return buf;
            }
            return "*struct";
        }
        default: return "unknown";
    }
}

static void print_version(void) {
    const VortexTargetDesc* td = vortex_get_target_desc();
    const VortexJITBackend* bk = vortex_get_backend();
    const char* simd_str = (td->simd_caps & VORTEX_SIMD_AVX2) ? "AVX2 256-bit"
                         : ((td->simd_caps & VORTEX_SIMD_NEON) ? "NEON 128-bit" : "Scalar");
    printf("Vortex v%s (%s JIT Compiler & Native Paged-Arena Runtime)\n", VORTEX_VERSION, bk->backend_name);
    printf("Active  : %s | SIMD: %s (%d x f32) | Regs: %d GPR / %d FPR\n",
           td->name, simd_str, td->simd_lanes_f32,
           td->int_callee_count + td->int_scratch_count,
           td->float_callee_count + td->float_scratch_count);
    printf("Targets : x86_64-windows, x86_64-linux, x86_64-darwin, arm64-linux, arm64-darwin\n");
}

static void print_usage(const char* exe_name) {
    printf("========================================================================\n");
    print_version();
    printf("========================================================================\n\n");
    printf("USAGE:\n");
    printf("  %s                          Open interactive Vortex REPL terminal\n", exe_name);
    printf("  %s repl | -i                Open interactive Vortex REPL terminal\n", exe_name);
    printf("  %s [flags] <script.vx>      JIT-compile and run a Vortex file\n", exe_name);
    printf("  %s run [flags] <script.vx>  JIT-compile and run a Vortex file\n", exe_name);
    printf("  %s eval [flags] \"<code>\"    JIT-compile and evaluate inline code\n", exe_name);
    printf("  %s build [flags] <in.vx> [-o out] Package standalone executable\n", exe_name);
    printf("  %s --version | -v           Display version and architecture info\n", exe_name);
    printf("  %s --help | -h              Display this help message\n", exe_name);
    printf("  %s pkg <subcmd>             Package manager (init, add, install, verify, run, build)\n", exe_name);
    printf("  %s lsp                      Start Language Server Protocol (JSON-RPC over stdio)\n", exe_name);
    printf("  %s test [path] [--json]     Run native Vortex unit tests (assert / assert_eq / test_*)\n", exe_name);
    printf("  %s spec [--json]            Emit live compiler specification (Markdown or JSON)\n\n", exe_name);
    printf("FLAGS / MODES:\n");
    printf("  --safe             (Default) Bounds, arena epoch & FFI boundary validation\n");
    printf("  --ultrasafe <1|2|3> Strict safety & static checks (1: runtime div0 + warnings,\n");
    printf("                     2: static audits + recursion guard, 3: multi-error recovery)\n");
    printf("  --check            Type/region/safety-check script without running JIT code\n");
    printf("  --json             Emit structured JSON diagnostics / test / spec output\n");
    printf("  --error-format=json Alias for --json\n");
    printf("  --release          Range check elimination / LICM + max SIMD throughput\n");
    printf("  --headless         Disable GUI/X11 window creation (or set VORTEX_HEADLESS=1)\n");
    printf("  --target <triple>  Target ABI/arch (x86_64-windows, x86_64-linux, arm64-linux, arm64-darwin)\n");
    printf("  --dump-jit <file>  Write raw JIT machine code bytes to <file>\n");
    printf("  --dump-debug <f>   Write DWARF v4 ELF debug symbols to <f>\n\n");
    printf("EXAMPLES:\n");
    printf("  %s run examples/app_3d.vx\n", exe_name);
    printf("  %s eval \"let x = 21; print(x * 2);\"\n", exe_name);
    printf("  %s build examples/app_3d.vx -o app_3d.exe\n", exe_name);
    printf("  %s build benchmarks/benchmark.vx --release --target arm64-darwin -o bench_arm64\n\n", exe_name);
}

static bool has_top_level_assign(const char* s) {
    int depth_paren = 0;
    int depth_brace = 0;
    bool in_str = false;
    for (int i = 0; s[i]; i++) {
        char ch = s[i];
        if (ch == '"' && (i == 0 || s[i - 1] != '\\')) {
            in_str = !in_str;
            continue;
        }
        if (in_str) continue;
        if (ch == '(') depth_paren++;
        else if (ch == ')' && depth_paren > 0) depth_paren--;
        else if (ch == '{') depth_brace++;
        else if (ch == '}' && depth_brace > 0) depth_brace--;
        else if (ch == '=' && depth_paren == 0 && depth_brace == 0) {
            char prev = (i > 0) ? s[i - 1] : '\0';
            char next = s[i + 1];
            if (prev != '!' && prev != '<' && prev != '>' && prev != '=' && next != '=') {
                return true;
            }
        }
    }
    return false;
}

static void repl_print_vars(void) {
    if (!g_has_last_symtab || g_last_symtab.count == 0) {
        printf("[VORTEX] No active variables in current session.\n");
        return;
    }
    printf("\n--- ACTIVE SESSION VARIABLES (%d) ---\n", g_last_symtab.count);
    for (int i = 0; i < g_last_symtab.count; i++) {
        Symbol* s = &g_last_symtab.symbols[i];
        printf("  %-18s : %-18s (%s)\n",
               s->name,
               vortex_type_name(s->type, s->struct_id),
               s->is_mut ? "mut" : "let");
    }
    printf("--------------------------------------\n\n");
}

static void repl_print_structs(void) {
    if (g_struct_type_count == 0) {
        printf("[VORTEX] No structs defined in current session.\n");
        return;
    }
    printf("\n--- DEFINED STRUCTS (%d) ---\n", g_struct_type_count);
    for (int i = 0; i < g_struct_type_count; i++) {
        VortexStructType* st = &g_struct_types[i];
        printf("  struct %s (size=%d bytes, align=%d) {\n", st->name, st->size, st->align);
        for (int f = 0; f < st->field_count; f++) {
            VortexField* fld = &st->fields[f];
            printf("    %-14s : %-14s @ offset +%d (%d bytes)\n",
                   fld->name, vortex_type_name(fld->type, fld->struct_id), fld->offset, fld->size);
        }
        printf("  }\n");
    }
    printf("---------------------------\n\n");
}

static bool repl_eval_snippet(const char* snippet, char* session_code, size_t session_cap, bool* out_persisted) {
    size_t len = strlen(snippet);
    char to_eval[65536];
    bool is_decl = (strncmp(snippet, "let ", 4) == 0 ||
                    strncmp(snippet, "fn ", 3) == 0 ||
                    strncmp(snippet, "unsafe fn ", 10) == 0 ||
                    strncmp(snippet, "struct ", 7) == 0 ||
                    strncmp(snippet, "extern ", 7) == 0);
    bool is_stmt = (is_decl ||
                    strncmp(snippet, "unsafe", 6) == 0 ||
                    strncmp(snippet, "print", 5) == 0 ||
                    strncmp(snippet, "for ", 4) == 0 ||
                    strncmp(snippet, "parallel_for ", 13) == 0 ||
                    strncmp(snippet, "while ", 6) == 0 ||
                    strncmp(snippet, "if ", 3) == 0 ||
                    has_top_level_assign(snippet));

    if (!is_stmt) {
        char temp_expr[2048];
        strncpy(temp_expr, snippet, sizeof(temp_expr) - 1);
        temp_expr[sizeof(temp_expr) - 1] = '\0';
        size_t tlen = strlen(temp_expr);
        if (tlen > 0 && temp_expr[tlen - 1] == ';') temp_expr[tlen - 1] = '\0';
        snprintf(to_eval, sizeof(to_eval), "%s\nprint(%s);", session_code, temp_expr);
    } else {
        const char* maybe_semi = "";
        if (len > 0 && snippet[len - 1] != ';' &&
            (strncmp(snippet, "let ", 4) == 0 || snippet[len - 1] != '}')) {
            maybe_semi = ";";
        }
        snprintf(to_eval, sizeof(to_eval), "%s\n%s%s", session_code, snippet, maybe_semi);
    }

    volatile bool persist = (is_decl || (is_stmt && has_top_level_assign(snippet)));
    if (out_persisted) *out_persisted = false;

    if (setjmp(g_error_jmp) == 0) {
        execute_vortex_source(to_eval);
        if (persist) {
            size_t cur_len = strlen(session_code);
            const char* maybe_semi = "";
            if (len > 0 && snippet[len - 1] != ';' &&
                (strncmp(snippet, "let ", 4) == 0 || snippet[len - 1] != '}')) {
                maybe_semi = ";";
            }
            snprintf(session_code + cur_len, session_cap - cur_len, "%s%s\n", snippet, maybe_semi);
            if (out_persisted) *out_persisted = true;
        }
        return true;
    }
    return false;
}

static void vortex_repl(void) {
    char session_code[65536] = {0};
    char line_buf[2048];

    printf("\n========================================================================\n");
    printf("   __     ______  ___ _____ ___  __  __\n");
    printf("  / /    / __/ / / _ \\_   _/ _ \\ \\ \\/ /\n");
    printf(" / /__  / _// /_/ // // / / // /  \\  /\n");
    printf("/____/ /___/____/____//_/ /____/  /_/    v%s\n\n", VORTEX_VERSION);
    printf(" Vortex Programming Language - Interactive x86-64 AVX2 JIT Terminal\n");
    printf(" 2MB Paged Arena | Linear-Scan IR | AVX2 Vectorizer | Dynamic C FFI\n");
    printf(" Commands: ':help', ':vars', ':structs', ':time <code|expr>', ':exit'\n");
    printf("========================================================================\n\n");

    g_in_repl = true;

    while (1) {
        printf("vortex> ");
        fflush(stdout);

        if (!fgets(line_buf, sizeof(line_buf), stdin)) {
            break;
        }

        char* trimmed = line_buf;
        while (*trimmed == ' ' || *trimmed == '\t' || *trimmed == '\r' || *trimmed == '\n') trimmed++;
        int len = (int)strlen(trimmed);
        while (len > 0 && (trimmed[len - 1] == ' ' || trimmed[len - 1] == '\t' || trimmed[len - 1] == '\r' || trimmed[len - 1] == '\n')) {
            trimmed[--len] = '\0';
        }

        if (len == 0) continue;

        if (strcmp(trimmed, ":exit") == 0 || strcmp(trimmed, ":quit") == 0 ||
            strcmp(trimmed, "exit") == 0 || strcmp(trimmed, "quit") == 0) {
            printf("[VORTEX] Session ended.\n");
            break;
        }

        if (strcmp(trimmed, ":clear") == 0 || strcmp(trimmed, ":cls") == 0) {
#ifdef _WIN32
            int _cls_rc = system("cls"); (void)_cls_rc;
#else
            int _cls_rc = system("clear"); (void)_cls_rc;
#endif
            continue;
        }

        if (strcmp(trimmed, ":vars") == 0) {
            repl_print_vars();
            continue;
        }

        if (strcmp(trimmed, ":structs") == 0) {
            repl_print_structs();
            continue;
        }

        if (strcmp(trimmed, ":reset") == 0) {
            session_code[0] = '\0';
            g_struct_type_count = 0;
            g_ffi_func_count = 0;
            g_has_last_symtab = false;
            g_last_symtab.count = 0;
            arena_init();
            printf("[VORTEX] Session state, structs, FFI bindings, and arena reset.\n");
            continue;
        }

        if (strcmp(trimmed, ":help") == 0) {
            printf("\n--- VORTEX REPL COMMANDS ---\n");
            printf("  :help               Show this command reference\n");
            printf("  :vars               List active variables and inferred types\n");
            printf("  :structs            Inspect registered structs, sizes, and field offsets\n");
            printf("  :time <code/expr>   Measure JIT compile + execution time in us/ms\n");
            printf("  :run <file.vx>      Execute a Vortex script from disk\n");
            printf("  :cls / :clear       Clear terminal screen\n");
            printf("  :reset              Clear session variables, structs, FFI, and arena\n");
            printf("  :exit / :quit       Exit the Vortex terminal\n\n");
            printf("Quick examples:\n");
            printf("  vortex> 10 + 25\n");
            printf("  vortex> let mut x = 40; x = x + 2; x\n");
            printf("  vortex> struct Vec3 { x: f32, y: f32, z: f32 }\n");
            printf("  vortex> fn fib(n) { if n <= 1 { return n; } return fib(n-1) + fib(n-2); }\n");
            printf("  vortex> :time fib(30)\n");
            printf("----------------------------\n\n");
            continue;
        }

        if (strncmp(trimmed, ":time ", 6) == 0) {
            const char* sub = trimmed + 6;
            while (*sub == ' ') sub++;
            if (*sub == '\0') {
                printf("[VORTEX] Usage: :time <expression or statement>\n");
                continue;
            }
            int64_t t0 = helper_time_us();
            bool ok = repl_eval_snippet(sub, session_code, sizeof(session_code), NULL);
            int64_t dt = helper_time_us() - t0;
            if (ok) {
                printf("[TIME] %lld us (%.3f ms)\n", (long long)dt, (double)dt / 1000.0);
            }
            continue;
        }

        if (strncmp(trimmed, ":run ", 5) == 0) {
            const char* file_to_run = trimmed + 5;
            while (*file_to_run == ' ') file_to_run++;
            execute_vortex_file(file_to_run);
            continue;
        }

        repl_eval_snippet(trimmed, session_code, sizeof(session_code), NULL);
    }

    g_in_repl = false;
}

int main(int argc, char** argv) {
    g_pkg_import_hook = pkg_resolve_import;
    if (check_and_run_embedded_payload(argc, argv)) {
        return 0;
    }

    char* filtered_argv[128];
    int filtered_argc = 0;
    filtered_argv[filtered_argc++] = argv[0];
    for (int i = 1; i < argc && filtered_argc < 127; i++) {
        if (strcmp(argv[i], "--release") == 0) {
            g_compile_mode = VORTEX_MODE_RELEASE;
            g_ultrasafe_level = 0;
        } else if (strcmp(argv[i], "--safe") == 0) {
            g_compile_mode = VORTEX_MODE_SAFE;
        } else if (strcmp(argv[i], "--ultrasafe") == 0) {
            g_compile_mode = VORTEX_MODE_SAFE;
            if (i + 1 < argc && isdigit((unsigned char)argv[i + 1][0]) && argv[i + 1][1] == '\0') {
                int lvl = argv[++i][0] - '0';
                if (lvl < 1 || lvl > 3) {
                    fprintf(stderr, "[VORTEX ERROR] --ultrasafe level must be 1, 2, or 3 (got %d)\n", lvl);
                    return 1;
                }
                g_ultrasafe_level = lvl;
            } else {
                g_ultrasafe_level = 3;
            }
        } else if (strncmp(argv[i], "--ultrasafe=", 12) == 0) {
            g_compile_mode = VORTEX_MODE_SAFE;
            int lvl = atoi(argv[i] + 12);
            if (lvl < 1 || lvl > 3) {
                fprintf(stderr, "[VORTEX ERROR] --ultrasafe level must be 1, 2, or 3 (got '%s')\n", argv[i] + 12);
                return 1;
            }
            g_ultrasafe_level = lvl;
        } else if (strcmp(argv[i], "--json") == 0 || strcmp(argv[i], "--error-format=json") == 0) {
            g_json_errors = true;
        } else if (strcmp(argv[i], "--check") == 0) {
            g_check_only = true;
        } else if (strcmp(argv[i], "--headless") == 0) {
            g_headless_mode = true;
        } else if (strcmp(argv[i], "--target") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "[VORTEX ERROR] Missing target triple after --target\n");
                return 1;
            }
            const char* tname = argv[++i];
            if (!vortex_set_target_name(tname)) {
                fprintf(stderr, "[VORTEX ERROR] Unknown target '%s' (supported: x86_64-windows, x86_64-linux, arm64-linux, arm64-darwin)\n", tname);
                return 1;
            }
        } else if (strncmp(argv[i], "--target=", 9) == 0) {
            const char* tname = argv[i] + 9;
            if (!vortex_set_target_name(tname)) {
                fprintf(stderr, "[VORTEX ERROR] Unknown target '%s' (supported: x86_64-windows, x86_64-linux, arm64-linux, arm64-darwin)\n", tname);
                return 1;
            }
        } else if (strcmp(argv[i], "--dump-jit") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "[VORTEX ERROR] Missing file path after --dump-jit\n");
                return 1;
            }
            g_dump_jit_path = argv[++i];
        } else if (strncmp(argv[i], "--dump-jit=", 11) == 0) {
            g_dump_jit_path = argv[i] + 11;
        } else if (strcmp(argv[i], "--dump-debug") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "[VORTEX ERROR] Missing file path after --dump-debug\n");
                return 1;
            }
            g_dump_debug_path = argv[++i];
        } else if (strncmp(argv[i], "--dump-debug=", 13) == 0) {
            g_dump_debug_path = argv[i] + 13;
        } else {
            filtered_argv[filtered_argc++] = argv[i];
        }
    }
    filtered_argv[filtered_argc] = NULL;

    if (filtered_argc == 1) {
        vortex_repl();
        return 0;
    }

    const char* arg1 = filtered_argv[1];
    if (strcmp(arg1, "repl") == 0 || strcmp(arg1, "-i") == 0) {
        vortex_repl();
        return 0;
    }
    if (strcmp(arg1, "--help") == 0 || strcmp(arg1, "-h") == 0) {
        print_usage(filtered_argv[0]);
        return 0;
    }
    if (strcmp(arg1, "--version") == 0 || strcmp(arg1, "-v") == 0) {
        print_version();
        return 0;
    }
    if (strcmp(arg1, "build") == 0) {
        return vortex_cmd_build(filtered_argc, filtered_argv);
    }
    if (strcmp(arg1, "pkg") == 0) {
        return vortex_cmd_pkg(filtered_argc, filtered_argv);
    }
    if (strcmp(arg1, "lsp") == 0) {
        return vortex_cmd_lsp(filtered_argc, filtered_argv);
    }
    if (strcmp(arg1, "test") == 0) {
        return vortex_cmd_test(filtered_argc, filtered_argv);
    }
    if (strcmp(arg1, "spec") == 0) {
        return vortex_cmd_spec(filtered_argc, filtered_argv);
    }
    if (strcmp(arg1, "eval") == 0) {
        if (filtered_argc < 3) {
            fprintf(stderr, "[VORTEX ERROR] Missing code string after 'eval'.\n\n");
            print_usage(filtered_argv[0]);
            return 1;
        }
        return execute_vortex_source(filtered_argv[2]);
    }
    if (strcmp(arg1, "run") == 0) {
        if (filtered_argc < 3) {
            fprintf(stderr, "[VORTEX ERROR] Missing script path after 'run'.\n\n");
            print_usage(filtered_argv[0]);
            return 1;
        }
        return execute_vortex_file(filtered_argv[2]);
    }

    FILE* f = fopen(arg1, "rb");
    if (f) {
        fclose(f);
        return execute_vortex_file(arg1);
    }

    fprintf(stderr, "[VORTEX ERROR] Unknown command or missing file: '%s'\n\n", arg1);
    print_usage(filtered_argv[0]);
    return 1;
}

#endif /* !VORTEX_SHARED_LIB */
