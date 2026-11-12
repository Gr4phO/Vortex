/*
 * VORTEX PROGRAMMING LANGUAGE - CLI & INTERACTIVE REPL ENTRYPOINT
 */

#include "vortex.h"

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
    printf("Vortex v%s (x86-64 AVX2 JIT Compiler & Native Paged-Arena Runtime)\n", VORTEX_VERSION);
    printf("Targets : Windows x64 / Linux System V AMD64 | Backend: Linear-Scan + AVX2 SIMD\n");
}

static void print_usage(const char* exe_name) {
    printf("========================================================================\n");
    print_version();
    printf("========================================================================\n\n");
    printf("USAGE:\n");
    printf("  %s                          Open interactive Vortex REPL terminal\n", exe_name);
    printf("  %s repl | -i                Open interactive Vortex REPL terminal\n", exe_name);
    printf("  %s <script.vx>              JIT-compile and run a Vortex file\n", exe_name);
    printf("  %s run <script.vx>          JIT-compile and run a Vortex file\n", exe_name);
    printf("  %s eval \"<code>\"            JIT-compile and evaluate inline code\n", exe_name);
    printf("  %s build <in.vx> [-o out]   Package standalone single-file executable\n", exe_name);
    printf("  %s --version | -v           Display version and architecture info\n", exe_name);
    printf("  %s --help | -h              Display this help message\n\n", exe_name);
    printf("EXAMPLES:\n");
    printf("  %s run examples/app_3d.vx\n", exe_name);
    printf("  %s eval \"let x = 21; print(x * 2);\"\n", exe_name);
    printf("  %s build examples/app_3d.vx -o app_3d.exe\n\n", exe_name);
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
                    strncmp(snippet, "struct ", 7) == 0 ||
                    strncmp(snippet, "extern ", 7) == 0);
    bool is_stmt = (is_decl ||
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

    bool persist = (is_decl || (is_stmt && has_top_level_assign(snippet)));
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
            system("cls");
#else
            system("clear");
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
    if (check_and_run_embedded_payload(argc, argv)) {
        return 0;
    }

    if (argc == 1) {
        vortex_repl();
        return 0;
    }

    if (strcmp(argv[1], "repl") == 0 || strcmp(argv[1], "-i") == 0) {
        vortex_repl();
        return 0;
    }
    if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0) {
        print_usage(argv[0]);
        return 0;
    }
    if (strcmp(argv[1], "--version") == 0 || strcmp(argv[1], "-v") == 0) {
        print_version();
        return 0;
    }
    if (strcmp(argv[1], "build") == 0) {
        return vortex_cmd_build(argc, argv);
    }
    if (strcmp(argv[1], "eval") == 0) {
        if (argc < 3) {
            fprintf(stderr, "[VORTEX ERROR] Missing code string after 'eval'.\n\n");
            print_usage(argv[0]);
            return 1;
        }
        return execute_vortex_source(argv[2]);
    }
    if (strcmp(argv[1], "run") == 0) {
        if (argc < 3) {
            fprintf(stderr, "[VORTEX ERROR] Missing script path after 'run'.\n\n");
            print_usage(argv[0]);
            return 1;
        }
        return execute_vortex_file(argv[2]);
    }

    FILE* f = fopen(argv[1], "rb");
    if (f) {
        fclose(f);
        return execute_vortex_file(argv[1]);
    }

    fprintf(stderr, "[VORTEX ERROR] Unknown command or missing file: '%s'\n\n", argv[1]);
    print_usage(argv[0]);
    return 1;
}
