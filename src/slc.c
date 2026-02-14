#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "libsl.h"

static int read_file(const char* filename, char** out_data, uint32_t* out_len) {
    FILE* f;
    long size;
    char* data;
    size_t nread;

    *out_data = NULL;
    *out_len = 0;

    f = fopen(filename, "rb");
    if (f == NULL) {
        fprintf(stderr, "failed to open %s\n", filename);
        return -1;
    }

    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        fprintf(stderr, "failed to seek %s\n", filename);
        return -1;
    }

    size = ftell(f);
    if (size < 0 || (unsigned long)size > UINT32_MAX) {
        fclose(f);
        fprintf(stderr, "file too large: %s\n", filename);
        return -1;
    }

    if (fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        fprintf(stderr, "failed to seek %s\n", filename);
        return -1;
    }

    data = (char*)malloc((size_t)size + 1u);
    if (data == NULL) {
        fclose(f);
        fprintf(stderr, "out of memory while reading %s\n", filename);
        return -1;
    }

    nread = fread(data, 1u, (size_t)size, f);
    fclose(f);
    if (nread != (size_t)size) {
        free(data);
        fprintf(stderr, "failed to read %s\n", filename);
        return -1;
    }

    data[size] = '\0';
    *out_data = data;
    *out_len = (uint32_t)size;
    return 0;
}

static void print_escaped(FILE* out, const char* s, uint32_t start, uint32_t end) {
    uint32_t i;

    fputc('"', out);
    for (i = start; i < end; i++) {
        unsigned char c = (unsigned char)s[i];
        switch (c) {
            case '"':
                fputs("\\\"", out);
                break;
            case '\\':
                fputs("\\\\", out);
                break;
            case '\n':
                fputs("\\n", out);
                break;
            case '\r':
                fputs("\\r", out);
                break;
            case '\t':
                fputs("\\t", out);
                break;
            default:
                if (c >= 0x20 && c <= 0x7e) {
                    fputc((int)c, out);
                } else {
                    fprintf(out, "\\x%02x", (unsigned)c);
                }
                break;
        }
    }
    fputc('"', out);
}

static int dump_tokens(const char* filename, const char* source, uint32_t source_len) {
    void* arena_mem;
    uint64_t arena_cap64;
    size_t arena_cap;
    sl_arena arena;
    sl_token_stream stream;
    sl_diag diag;
    uint32_t i;

    arena_cap64 = (uint64_t)(source_len + 16u) * (uint64_t)sizeof(sl_token) + 4096u;
    if (arena_cap64 > (uint64_t)SIZE_MAX) {
        fprintf(stderr, "arena too large\n");
        return -1;
    }

    arena_cap = (size_t)arena_cap64;
    arena_mem = malloc(arena_cap);
    if (arena_mem == NULL) {
        fprintf(stderr, "failed to allocate arena\n");
        return -1;
    }

    sl_arena_init(&arena, arena_mem, (uint32_t)arena_cap);
    if (sl_lex(&arena, (sl_strview){source, source_len}, &stream, &diag) != 0) {
        fprintf(stderr, "%s:%u:%u: error: %s\n", filename, diag.start, diag.end,
                sl_diag_message(diag.code));
        free(arena_mem);
        return -1;
    }

    for (i = 0; i < stream.len; i++) {
        const sl_token* t = &stream.v[i];
        printf("%s %u %u ", sl_token_kind_name(t->kind), t->start, t->end);
        if (t->kind == SL_TOK_EOF) {
            printf("<eof>");
        } else if (t->kind == SL_TOK_SEMICOLON && t->start == t->end) {
            printf("<auto>");
        } else {
            print_escaped(stdout, source, t->start, t->end);
        }
        fputc('\n', stdout);
    }

    free(arena_mem);
    return 0;
}

int main(int argc, char* argv[]) {
    char* source;
    uint32_t source_len;

    if (argc != 2) {
        fprintf(stderr, "usage: %s <file.sl>\n", argv[0]);
        return 2;
    }

    if (read_file(argv[1], &source, &source_len) != 0) {
        return 1;
    }

    if (dump_tokens(argv[1], source, source_len) != 0) {
        free(source);
        return 1;
    }

    free(source);
    return 0;
}
