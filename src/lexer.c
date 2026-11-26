/*
 * VORTEX PROGRAMMING LANGUAGE - LEXER (src/lexer.c)
 * Tokenizes Vortex source code into keywords, identifiers, numbers, strings, and operators.
 */

#include "parser_internal.h"

static void unescape_string(char* dst, size_t dst_cap, const char* src, size_t src_len) {
    size_t di = 0;
    size_t si = 0;
    if (!dst || dst_cap == 0) return;
    while (si < src_len && di + 1 < dst_cap) {
        if (src[si] == '\\' && si + 1 < src_len) {
            si++;
            switch (src[si]) {
                case 'n':  dst[di++] = '\n'; break;
                case 't':  dst[di++] = '\t'; break;
                case 'r':  dst[di++] = '\r'; break;
                case '0':  dst[di++] = '\0'; break;
                case '\\': dst[di++] = '\\'; break;
                case '\"': dst[di++] = '\"'; break;
                default:   dst[di++] = src[si]; break;
            }
        } else {
            dst[di++] = src[si];
        }
        si++;
    }
    dst[di] = '\0';
}

void lexer_init(Lexer* lex, const char* source) {
    lex->src = source ? source : "";
    lex->pos = 0;
    lex->len = strlen(lex->src);
    lex->line = 1;
    lex->col = 1;
    if (lex->len >= 3 &&
        (unsigned char)lex->src[0] == 0xEF &&
        (unsigned char)lex->src[1] == 0xBB &&
        (unsigned char)lex->src[2] == 0xBF) {
        lex->pos = 3;
    }
    next_token(lex);
}

void next_token(Lexer* lex) {
    if (lex->pos == 0 && lex->len >= 3 &&
        (unsigned char)lex->src[0] == 0xEF &&
        (unsigned char)lex->src[1] == 0xBB &&
        (unsigned char)lex->src[2] == 0xBF) {
        lex->pos = 3;
    }
    while (lex->pos < lex->len) {
        char c = lex->src[lex->pos];
        if (c == ' ' || c == '\t' || c == '\r') {
            lex->pos++; lex->col++; continue;
        }
        if (c == '\n') {
            lex->pos++; lex->line++; lex->col = 1; continue;
        }
        if (c == '/' && lex->pos + 1 < lex->len && lex->src[lex->pos + 1] == '/') {
            lex->pos += 2;
            while (lex->pos < lex->len && lex->src[lex->pos] != '\n') lex->pos++;
            continue;
        }
        if (c == '/' && lex->pos + 1 < lex->len && lex->src[lex->pos + 1] == '*') {
            lex->pos += 2;
            lex->col += 2;
            while (lex->pos < lex->len) {
                if (lex->src[lex->pos] == '*' && lex->pos + 1 < lex->len && lex->src[lex->pos + 1] == '/') {
                    lex->pos += 2;
                    lex->col += 2;
                    break;
                }
                if (lex->src[lex->pos] == '\n') {
                    lex->pos++; lex->line++; lex->col = 1;
                } else {
                    lex->pos++; lex->col++;
                }
            }
            continue;
        }
        break;
    }

    if (lex->pos >= lex->len) {
        lex->cur.type = TOK_EOF;
        lex->cur.text[0] = '\0';
        lex->cur.line = lex->line;
        lex->cur.col = lex->col;
        return;
    }

    lex->cur.line = lex->line;
    lex->cur.col = lex->col;
    char c = lex->src[lex->pos];

    if (isdigit((unsigned char)c)) {
        size_t start = lex->pos;
        if (c == '0' && lex->pos + 1 < lex->len &&
            (lex->src[lex->pos + 1] == 'x' || lex->src[lex->pos + 1] == 'X')) {
            lex->pos += 2; lex->col += 2;
            while (lex->pos < lex->len && isxdigit((unsigned char)lex->src[lex->pos])) {
                lex->pos++; lex->col++;
            }
            size_t len = lex->pos - start;
            if (len >= sizeof(lex->cur.text)) len = sizeof(lex->cur.text) - 1;
            memcpy(lex->cur.text, &lex->src[start], len);
            lex->cur.text[len] = '\0';
            lex->cur.type = TOK_INT;
            lex->cur.int_val = (int64_t)strtoull(lex->cur.text, NULL, 16);
            return;
        }

        bool is_float = false;
        while (lex->pos < lex->len && (isdigit((unsigned char)lex->src[lex->pos]) || lex->src[lex->pos] == '.')) {
            if (lex->src[lex->pos] == '.') {
                if (lex->pos + 1 < lex->len && lex->src[lex->pos + 1] == '.') break;
                is_float = true;
            }
            lex->pos++; lex->col++;
        }
        size_t len = lex->pos - start;
        if (len >= sizeof(lex->cur.text)) len = sizeof(lex->cur.text) - 1;
        memcpy(lex->cur.text, &lex->src[start], len);
        lex->cur.text[len] = '\0';

        if (is_float) {
            lex->cur.type = TOK_FLOAT;
            lex->cur.float_val = (float)strtod(lex->cur.text, NULL);
        } else {
            lex->cur.type = TOK_INT;
            lex->cur.int_val = strtoll(lex->cur.text, NULL, 10);
        }
        return;
    }

    if (isalpha((unsigned char)c) || c == '_') {
        size_t start = lex->pos;
        while (lex->pos < lex->len && (isalnum((unsigned char)lex->src[lex->pos]) || lex->src[lex->pos] == '_')) {
            lex->pos++; lex->col++;
        }
        size_t len = lex->pos - start;
        if (len >= sizeof(lex->cur.text)) len = sizeof(lex->cur.text) - 1;
        memcpy(lex->cur.text, &lex->src[start], len);
        lex->cur.text[len] = '\0';

        if (strcmp(lex->cur.text, "let") == 0) lex->cur.type = TOK_LET;
        else if (strcmp(lex->cur.text, "mut") == 0) lex->cur.type = TOK_MUT;
        else if (strcmp(lex->cur.text, "for") == 0) lex->cur.type = TOK_FOR;
        else if (strcmp(lex->cur.text, "parallel_for") == 0) lex->cur.type = TOK_PARALLEL_FOR;
        else if (strcmp(lex->cur.text, "in") == 0) lex->cur.type = TOK_IN;
        else if (strcmp(lex->cur.text, "while") == 0) lex->cur.type = TOK_WHILE;
        else if (strcmp(lex->cur.text, "if") == 0) lex->cur.type = TOK_IF;
        else if (strcmp(lex->cur.text, "else") == 0) lex->cur.type = TOK_ELSE;
        else if (strcmp(lex->cur.text, "fn") == 0) lex->cur.type = TOK_FN;
        else if (strcmp(lex->cur.text, "return") == 0) lex->cur.type = TOK_RETURN;
        else if (strcmp(lex->cur.text, "struct") == 0) lex->cur.type = TOK_STRUCT;
        else if (strcmp(lex->cur.text, "enum") == 0) lex->cur.type = TOK_ENUM;
        else if (strcmp(lex->cur.text, "extern") == 0) lex->cur.type = TOK_EXTERN;
        else if (strcmp(lex->cur.text, "unsafe") == 0) lex->cur.type = TOK_UNSAFE;
        else if (strcmp(lex->cur.text, "as") == 0) lex->cur.type = TOK_AS;
        else if (strcmp(lex->cur.text, "import") == 0) lex->cur.type = TOK_IMPORT;
        else if (strcmp(lex->cur.text, "break") == 0) lex->cur.type = TOK_BREAK;
        else if (strcmp(lex->cur.text, "continue") == 0) lex->cur.type = TOK_CONTINUE;
        else if (strcmp(lex->cur.text, "true") == 0) lex->cur.type = TOK_TRUE;
        else if (strcmp(lex->cur.text, "false") == 0) lex->cur.type = TOK_FALSE;
        else if (strcmp(lex->cur.text, "i64") == 0) lex->cur.type = TOK_I64_KW;
        else if (strcmp(lex->cur.text, "f32") == 0) lex->cur.type = TOK_F32_KW;
        else lex->cur.type = TOK_IDENT;
        return;
    }

    if (c == '"') {
        lex->pos++; lex->col++;
        size_t start = lex->pos;
        while (lex->pos < lex->len && lex->src[lex->pos] != '"') {
            if (lex->src[lex->pos] == '\\' && lex->pos + 1 < lex->len) {
                lex->pos += 2; lex->col += 2;
            } else {
                if (lex->src[lex->pos] == '\n') {
                    lex->pos++; lex->line++; lex->col = 1;
                } else {
                    lex->pos++; lex->col++;
                }
            }
        }
        size_t raw_len = lex->pos - start;
        unescape_string(lex->cur.text, sizeof(lex->cur.text), &lex->src[start], raw_len);

        if (lex->pos < lex->len && lex->src[lex->pos] == '"') {
            lex->pos++; lex->col++;
        }
        lex->cur.type = TOK_STRING;
        return;
    }

    /* 3-char shift-assign operators: <<= and >>= */
    if (lex->pos + 2 < lex->len) {
        char c2 = lex->src[lex->pos + 1];
        char c3 = lex->src[lex->pos + 2];
        if (c == '<' && c2 == '<' && c3 == '=') {
            lex->pos += 3; lex->col += 3;
            lex->cur.type = TOK_SHL_EQ; strcpy(lex->cur.text, "<<="); return;
        }
        if (c == '>' && c2 == '>' && c3 == '=') {
            lex->pos += 3; lex->col += 3;
            lex->cur.type = TOK_SHR_EQ; strcpy(lex->cur.text, ">>="); return;
        }
    }

    if (lex->pos + 1 < lex->len) {
        char c2 = lex->src[lex->pos + 1];
        if (c == '-' && c2 == '>') { lex->pos += 2; lex->col += 2; lex->cur.type = TOK_ARROW; strcpy(lex->cur.text, "->"); return; }
        if (c == '.' && c2 == '.') { lex->pos += 2; lex->col += 2; lex->cur.type = TOK_DOTDOT; strcpy(lex->cur.text, ".."); return; }
        if (c == '=' && c2 == '=') { lex->pos += 2; lex->col += 2; lex->cur.type = TOK_EQEQ; strcpy(lex->cur.text, "=="); return; }
        if (c == '!' && c2 == '=') { lex->pos += 2; lex->col += 2; lex->cur.type = TOK_NEQ; strcpy(lex->cur.text, "!="); return; }
        if (c == '<' && c2 == '=') { lex->pos += 2; lex->col += 2; lex->cur.type = TOK_LE; strcpy(lex->cur.text, "<="); return; }
        if (c == '>' && c2 == '=') { lex->pos += 2; lex->col += 2; lex->cur.type = TOK_GE; strcpy(lex->cur.text, ">="); return; }
        if (c == '&' && c2 == '&') { lex->pos += 2; lex->col += 2; lex->cur.type = TOK_AND; strcpy(lex->cur.text, "&&"); return; }
        if (c == '|' && c2 == '|') { lex->pos += 2; lex->col += 2; lex->cur.type = TOK_OR; strcpy(lex->cur.text, "||"); return; }
        if (c == '<' && c2 == '<') { lex->pos += 2; lex->col += 2; lex->cur.type = TOK_SHL; strcpy(lex->cur.text, "<<"); return; }
        if (c == '>' && c2 == '>') { lex->pos += 2; lex->col += 2; lex->cur.type = TOK_SHR; strcpy(lex->cur.text, ">>"); return; }
        if (c == ':' && c2 == ':') { lex->pos += 2; lex->col += 2; lex->cur.type = TOK_COLONCOLON; strcpy(lex->cur.text, "::"); return; }
        if (c == '+' && c2 == '=') { lex->pos += 2; lex->col += 2; lex->cur.type = TOK_PLUS_EQ; strcpy(lex->cur.text, "+="); return; }
        if (c == '-' && c2 == '=') { lex->pos += 2; lex->col += 2; lex->cur.type = TOK_MINUS_EQ; strcpy(lex->cur.text, "-="); return; }
        if (c == '*' && c2 == '=') { lex->pos += 2; lex->col += 2; lex->cur.type = TOK_STAR_EQ; strcpy(lex->cur.text, "*="); return; }
        if (c == '/' && c2 == '=') { lex->pos += 2; lex->col += 2; lex->cur.type = TOK_SLASH_EQ; strcpy(lex->cur.text, "/="); return; }
        if (c == '%' && c2 == '=') { lex->pos += 2; lex->col += 2; lex->cur.type = TOK_PERCENT_EQ; strcpy(lex->cur.text, "%="); return; }
        if (c == '&' && c2 == '=') { lex->pos += 2; lex->col += 2; lex->cur.type = TOK_AMP_EQ; strcpy(lex->cur.text, "&="); return; }
        if (c == '|' && c2 == '=') { lex->pos += 2; lex->col += 2; lex->cur.type = TOK_PIPE_EQ; strcpy(lex->cur.text, "|="); return; }
        if (c == '^' && c2 == '=') { lex->pos += 2; lex->col += 2; lex->cur.type = TOK_CARET_EQ; strcpy(lex->cur.text, "^="); return; }
    }

    lex->pos++; lex->col++;
    lex->cur.text[0] = c; lex->cur.text[1] = '\0';

    switch (c) {
        case '+': lex->cur.type = TOK_PLUS; break;
        case '-': lex->cur.type = TOK_MINUS; break;
        case '*': lex->cur.type = TOK_STAR; break;
        case '/': lex->cur.type = TOK_SLASH; break;
        case '%': lex->cur.type = TOK_PERCENT; break;
        case '=': lex->cur.type = TOK_EQ; break;
        case '<': lex->cur.type = TOK_LT; break;
        case '>': lex->cur.type = TOK_GT; break;
        case '!': lex->cur.type = TOK_BANG; break;
        case '&': lex->cur.type = TOK_AMP; break;
        case '|': lex->cur.type = TOK_PIPE; break;
        case '^': lex->cur.type = TOK_CARET; break;
        case '~': lex->cur.type = TOK_TILDE; break;
        case '(': lex->cur.type = TOK_LPAREN; break;
        case ')': lex->cur.type = TOK_RPAREN; break;
        case '{': lex->cur.type = TOK_LBRACE; break;
        case '}': lex->cur.type = TOK_RBRACE; break;
        case '[': lex->cur.type = TOK_LBRACKET; break;
        case ']': lex->cur.type = TOK_RBRACKET; break;
        case ';': lex->cur.type = TOK_SEMI; break;
        case ',': lex->cur.type = TOK_COMMA; break;
        case ':': lex->cur.type = TOK_COLON; break;
        case '.': lex->cur.type = TOK_DOT; break;
        case '?': lex->cur.type = TOK_QUESTION; break;
        case '#': lex->cur.type = TOK_HASH; break;
        default:
            fprintf(stderr, "[VORTEX ERROR] Unknown character '%c' at %s:%d:%d\n",
                    c, g_current_filename ? g_current_filename : "<input>", lex->line, lex->col);
            vortex_error("");
    }
}

void vortex_lexer_sync(Lexer* l, bool in_function) {
    if (!l || l->cur.type == TOK_EOF) return;
    int brace_depth = 0;
    if (l->cur.type == TOK_SEMI) {
        next_token(l);
        return;
    }
    if (l->cur.type == TOK_LBRACE) brace_depth = 1;
    next_token(l);

    while (l->cur.type != TOK_EOF) {
        VortexTokenType t = l->cur.type;
        if (t == TOK_LBRACE) {
            brace_depth++;
            next_token(l);
            continue;
        }
        if (t == TOK_RBRACE) {
            if (brace_depth > 0) {
                brace_depth--;
                next_token(l);
                if (brace_depth == 0) return;
                continue;
            }
            return;
        }
        if (brace_depth == 0) {
            if (t == TOK_SEMI) {
                next_token(l);
                return;
            }
            if (!in_function && (t == TOK_FN || t == TOK_STRUCT || t == TOK_ENUM ||
                                 t == TOK_EXTERN || t == TOK_IMPORT || t == TOK_UNSAFE || t == TOK_LET)) {
                return;
            }
            if (in_function && (t == TOK_LET || t == TOK_IF || t == TOK_FOR ||
                                t == TOK_WHILE || t == TOK_RETURN || t == TOK_BREAK || t == TOK_CONTINUE)) {
                return;
            }
        }
        next_token(l);
    }
}
