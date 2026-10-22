/* Characterization tests for the store-only archiver.
 *
 * These tests exercise the ZIP reader, writer, iterator, clone, ZIP64 and
 * validation paths. Deflate-compressed inputs must be rejected.
 */
#include "miniz.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

#define CHECK(cond, msg)                                              \
    do                                                                \
    {                                                                 \
        if (!(cond))                                                  \
        {                                                             \
            fprintf(stderr, "FAIL: %s (%s:%d)\n", (msg), __FILE__, __LINE__); \
            failures++;                                               \
        }                                                             \
    } while (0)

static void fill_pattern(unsigned char *p, size_t n, unsigned seed)
{
    size_t i;
    for (i = 0; i < n; i++)
        p[i] = (unsigned char)((i * 31u + seed * 17u) & 0xFFu);
}

struct mem_read_ctx
{
    const unsigned char *data;
    size_t size;
};

static size_t mem_read_cb(void *opaque, mz_uint64 ofs, void *buf, size_t n)
{
    struct mem_read_ctx *ctx = (struct mem_read_ctx *)opaque;
    size_t offset = (size_t)ofs;
    if (offset >= ctx->size)
        return 0;
    if (n > ctx->size - offset)
        n = ctx->size - offset;
    memcpy(buf, ctx->data + offset, n);
    return n;
}

struct mem_write_ctx
{
    unsigned char *data;
    size_t size;
    size_t cap;
};

static size_t mem_write_cb(void *opaque, mz_uint64 ofs, const void *buf, size_t n)
{
    struct mem_write_ctx *ctx = (struct mem_write_ctx *)opaque;
    size_t offset = (size_t)ofs;
    if (offset + n > ctx->cap)
    {
        size_t new_cap = ctx->cap ? ctx->cap : 4096;
        unsigned char *p;
        while (new_cap < offset + n)
            new_cap *= 2;
        p = (unsigned char *)realloc(ctx->data, new_cap);
        if (!p)
            return 0;
        ctx->data = p;
        ctx->cap = new_cap;
    }
    memcpy(ctx->data + offset, buf, n);
    if (offset + n > ctx->size)
        ctx->size = offset + n;
    return n;
}

static int check_extracted(unsigned char *got, size_t got_size,
                           const unsigned char *want, size_t want_size,
                           const char *what)
{
    if (got_size != want_size || memcmp(got, want, want_size) != 0)
    {
        fprintf(stderr, "FAIL: %s (size %lu vs %lu)\n", what,
                (unsigned long)got_size, (unsigned long)want_size);
        failures++;
        return 0;
    }
    return 1;
}

static void test_mem_roundtrip(void)
{
    mz_zip_archive zip;
    mz_zip_archive rd;
    void *archive = NULL;
    size_t archive_size = 0;
    unsigned char *pattern;
    unsigned char *cb_pattern;
    unsigned char out[8192];
    mz_zip_archive_file_stat st;
    static const char text[] = "store-only characterization payload";

    pattern = (unsigned char *)malloc(200000);
    cb_pattern = (unsigned char *)malloc(12345);
    CHECK(pattern && cb_pattern, "alloc pattern");
    if (!pattern || !cb_pattern)
        return;
    fill_pattern(pattern, 200000, 1);
    fill_pattern(cb_pattern, 12345, 2);

    memset(&zip, 0, sizeof(zip));
    CHECK(mz_zip_writer_init_heap(&zip, 0, 0), "writer_init_heap");
    CHECK(mz_zip_writer_add_mem(&zip, "data.bin", pattern, 200000, 0),
          "add_mem stored level 0");
    CHECK(mz_zip_writer_add_mem(&zip, "text.txt", text, sizeof(text) - 1, MZ_DEFAULT_COMPRESSION),
          "add_mem default level");
    CHECK(mz_zip_writer_add_mem(&zip, "dir/", NULL, 0, 0), "add dir");
    CHECK(mz_zip_writer_add_mem(&zip, "empty.txt", NULL, 0, 0), "add empty");
    {
        struct mem_read_ctx cb_ctx;
        cb_ctx.data = cb_pattern;
        cb_ctx.size = 12345;
        CHECK(mz_zip_writer_add_read_buf_callback(&zip, "callback.bin", mem_read_cb, &cb_ctx, cb_ctx.size,
                                                  NULL, NULL, 0, MZ_BEST_COMPRESSION, NULL, 0, NULL, 0),
              "add_read_buf_callback");
    }
    CHECK(mz_zip_writer_finalize_heap_archive(&zip, &archive, &archive_size), "finalize_heap");
    CHECK(mz_zip_writer_end(&zip), "writer_end");

    memset(&rd, 0, sizeof(rd));
    CHECK(mz_zip_reader_init_mem(&rd, archive, archive_size, 0), "reader_init_mem");
    CHECK(mz_zip_reader_get_num_files(&rd) == 5, "num files");

    CHECK(mz_zip_reader_file_stat(&rd, 0, &st), "stat data.bin");
    CHECK(st.m_method == 0, "method is stored");
    CHECK(st.m_uncomp_size == 200000, "uncompressed size");
    CHECK(st.m_comp_size == st.m_uncomp_size, "compressed size equals uncompressed");
    /* Levels requested for these entries must have been ignored. */
    CHECK(mz_zip_reader_file_stat(&rd, 1, &st), "stat text.txt");
    CHECK(st.m_method == 0 && st.m_comp_size == st.m_uncomp_size, "level ignored for text.txt");
    CHECK(mz_zip_reader_file_stat(&rd, 4, &st), "stat callback.bin");
    CHECK(st.m_method == 0 && st.m_comp_size == st.m_uncomp_size, "level ignored for callback.bin");

    /* extract_to_mem_no_alloc1 path */
    memset(out, 0, sizeof(out));
    {
        unsigned char *big = (unsigned char *)malloc(200000);
        CHECK(mz_zip_reader_extract_to_mem(&rd, 0, big, 200000, 0), "extract_to_mem");
        check_extracted(big, 200000, pattern, 200000, "extract_to_mem bytes");
        free(big);
    }

    /* extract_to_heap path */
    {
        size_t sz = 0;
        void *p = mz_zip_reader_extract_file_to_heap(&rd, "text.txt", &sz, 0);
        check_extracted((unsigned char *)p, sz, (const unsigned char *)text, sizeof(text) - 1,
                        "extract_file_to_heap bytes");
        free(p);
    }

    /* extract_to_callback path, in small chunks */
    {
        struct mem_write_ctx wctx;
        memset(&wctx, 0, sizeof(wctx));
        CHECK(mz_zip_reader_extract_to_callback(&rd, 4, mem_write_cb, &wctx, 0),
              "extract_to_callback");
        check_extracted(wctx.data, wctx.size, cb_pattern, 12345, "extract_to_callback bytes");
        free(wctx.data);
    }

    /* iterator path */
    {
        mz_zip_reader_extract_iter_state *it = mz_zip_reader_extract_iter_new(&rd, 0, 0);
        unsigned char *buf = (unsigned char *)malloc(200000);
        size_t total = 0;
        CHECK(it != NULL, "iter_new");
        while (it && total < 200000)
        {
            size_t n = mz_zip_reader_extract_iter_read(it, buf + total, 777);
            if (!n)
                break;
            total += n;
        }
        CHECK(it && mz_zip_reader_extract_iter_free(it), "iter_free");
        check_extracted(buf, total, pattern, 200000, "iterator bytes");
        free(buf);
    }

    CHECK(mz_zip_validate_archive(&rd, 0), "validate_archive");
    CHECK(mz_zip_reader_end(&rd), "reader_end");

    /* Clone one entry into a fresh archive without any codec. */
    {
        mz_zip_archive src;
        mz_zip_archive dst;
        void *dst_archive = NULL;
        size_t dst_size = 0;
        memset(&src, 0, sizeof(src));
        memset(&dst, 0, sizeof(dst));
        CHECK(mz_zip_reader_init_mem(&src, archive, archive_size, 0), "clone reader init");
        CHECK(mz_zip_writer_init_heap(&dst, 0, 0), "clone writer init");
        CHECK(mz_zip_writer_add_from_zip_reader(&dst, &src, 0), "add_from_zip_reader");
        CHECK(mz_zip_writer_finalize_heap_archive(&dst, &dst_archive, &dst_size), "clone finalize");
        CHECK(mz_zip_writer_end(&dst), "clone writer end");
        CHECK(mz_zip_reader_end(&src), "clone reader end");
        {
            mz_zip_archive rd2;
            unsigned char *big = (unsigned char *)malloc(200000);
            memset(&rd2, 0, sizeof(rd2));
            CHECK(mz_zip_reader_init_mem(&rd2, dst_archive, dst_size, 0), "clone reader init 2");
            CHECK(mz_zip_reader_extract_to_mem(&rd2, 0, big, 200000, 0), "clone extract");
            check_extracted(big, 200000, pattern, 200000, "clone bytes");
            free(big);
            mz_zip_reader_end(&rd2);
        }
        free(dst_archive);
    }

    free(archive);
    free(pattern);
    free(cb_pattern);
}

static void test_file_roundtrip(const char *scratch)
{
    char name[1024];
    mz_zip_archive zip;
    mz_zip_archive rd;
    unsigned char pattern[4096];
    unsigned char got[4096];
    FILE *f;
    size_t sz = 0;
    void *p;

    sprintf(name, "%s/file.zip", scratch);
    fill_pattern(pattern, sizeof(pattern), 3);

    memset(&zip, 0, sizeof(zip));
    CHECK(mz_zip_writer_init_file(&zip, name, 0), "writer_init_file");
    CHECK(mz_zip_writer_add_mem(&zip, "f.bin", pattern, sizeof(pattern), MZ_BEST_SPEED), "add file.bin");
    CHECK(mz_zip_writer_finalize_archive(&zip), "finalize file");
    CHECK(mz_zip_writer_end(&zip), "end file writer");

    memset(&rd, 0, sizeof(rd));
    CHECK(mz_zip_reader_init_file(&rd, name, 0), "reader_init_file");
    p = mz_zip_reader_extract_to_heap(&rd, 0, &sz, 0);
    check_extracted((unsigned char *)p, sz, pattern, sizeof(pattern), "file roundtrip bytes");
    free(p);
    CHECK(mz_zip_reader_end(&rd), "end file reader");

    /* FILE* writer/reader. */
    sprintf(name, "%s/cfile.zip", scratch);
    f = fopen(name, "wb");
    CHECK(f != NULL, "fopen cfile wb");
    if (f)
    {
        memset(&zip, 0, sizeof(zip));
        CHECK(mz_zip_writer_init_cfile(&zip, f, 0), "writer_init_cfile");
        CHECK(mz_zip_writer_add_mem(&zip, "c.bin", pattern, sizeof(pattern), 0), "add c.bin");
        CHECK(mz_zip_writer_finalize_archive(&zip), "finalize cfile");
        CHECK(mz_zip_writer_end(&zip), "end cfile writer");
        fclose(f);
    }
    f = fopen(name, "rb");
    CHECK(f != NULL, "fopen cfile rb");
    if (f)
    {
        memset(&rd, 0, sizeof(rd));
        CHECK(mz_zip_reader_init_cfile(&rd, f, 0, 0), "reader_init_cfile");
        memset(got, 0, sizeof(got));
        CHECK(mz_zip_reader_extract_to_mem(&rd, 0, got, sizeof(got), 0), "extract cfile");
        check_extracted(got, sizeof(got), pattern, sizeof(pattern), "cfile roundtrip bytes");
        CHECK(mz_zip_reader_end(&rd), "end cfile reader");
        fclose(f);
    }
}

static void test_zip64_flag(void)
{
    mz_zip_archive zip;
    mz_zip_archive rd;
    void *archive = NULL;
    size_t archive_size = 0;
    unsigned char data[300];
    unsigned char got[300];
    mz_zip_archive_file_stat st;

    fill_pattern(data, sizeof(data), 4);
    memset(&zip, 0, sizeof(zip));
    CHECK(mz_zip_writer_init_heap_v2(&zip, 0, 0, MZ_ZIP_FLAG_WRITE_ZIP64), "writer_init_heap_v2 zip64");
    CHECK(mz_zip_writer_add_mem(&zip, "z.bin", data, sizeof(data), 0), "add zip64 entry");
    CHECK(mz_zip_writer_finalize_heap_archive(&zip, &archive, &archive_size), "finalize zip64");
    CHECK(mz_zip_writer_end(&zip), "end zip64 writer");

    memset(&rd, 0, sizeof(rd));
    CHECK(mz_zip_reader_init_mem(&rd, archive, archive_size, 0), "reader_init_mem zip64");
    CHECK(mz_zip_reader_file_stat(&rd, 0, &st), "stat zip64");
    CHECK(st.m_method == 0, "zip64 method stored");
    memset(got, 0, sizeof(got));
    CHECK(mz_zip_reader_extract_to_mem(&rd, 0, got, sizeof(got), 0), "extract zip64");
    check_extracted(got, sizeof(got), data, sizeof(data), "zip64 bytes");
    CHECK(mz_zip_reader_end(&rd), "end zip64 reader");
    free(archive);
}

static void put16(unsigned char *p, unsigned v)
{
    p[0] = (unsigned char)(v & 0xFF);
    p[1] = (unsigned char)((v >> 8) & 0xFF);
}

static void put32(unsigned char *p, unsigned long v)
{
    p[0] = (unsigned char)(v & 0xFF);
    p[1] = (unsigned char)((v >> 8) & 0xFF);
    p[2] = (unsigned char)((v >> 16) & 0xFF);
    p[3] = (unsigned char)((v >> 24) & 0xFF);
}

static size_t make_deflate_zip(unsigned char *out)
{
    static const char name[] = "d.bin";
    static const unsigned char payload[5] = {0x4b, 0x04, 0x00, 0x00, 0x00};
    unsigned char *p = out;
    unsigned long crc = 0x12345678UL;

    /* local file header */
    put32(p, 0x04034b50UL); p += 4;
    put16(p, 20); p += 2;
    put16(p, 0); p += 2;
    put16(p, 8); p += 2;
    put16(p, 0); p += 2;
    put16(p, 0); p += 2;
    put32(p, crc); p += 4;
    put32(p, sizeof(payload)); p += 4;
    put32(p, 3); p += 4;
    put16(p, sizeof(name) - 1); p += 2;
    put16(p, 0); p += 2;
    memcpy(p, name, sizeof(name) - 1); p += sizeof(name) - 1;
    memcpy(p, payload, sizeof(payload)); p += sizeof(payload);

    /* central directory header */
    put32(p, 0x02014b50UL); p += 4;
    put16(p, 20); p += 2;
    put16(p, 20); p += 2;
    put16(p, 0); p += 2;
    put16(p, 8); p += 2;
    put16(p, 0); p += 2;
    put16(p, 0); p += 2;
    put32(p, crc); p += 4;
    put32(p, sizeof(payload)); p += 4;
    put32(p, 3); p += 4;
    put16(p, sizeof(name) - 1); p += 2;
    put16(p, 0); p += 2;
    put16(p, 0); p += 2;
    put16(p, 0); p += 2;
    put16(p, 0); p += 2;
    put32(p, 0); p += 4;
    put32(p, 0); p += 4;
    memcpy(p, name, sizeof(name) - 1); p += sizeof(name) - 1;

    /* end of central directory */
    put32(p, 0x06054b50UL); p += 4;
    put16(p, 0); p += 2;
    put16(p, 0); p += 2;
    put16(p, 1); p += 2;
    put16(p, 1); p += 2;
    put32(p, (unsigned long)(sizeof(name) - 1 + 46)); p += 4;
    put32(p, (unsigned long)(sizeof(name) - 1 + 30 + sizeof(payload))); p += 4;
    put16(p, 0); p += 2;

    return (size_t)(p - out);
}

static void test_deflate_rejected(void)
{
    unsigned char zipbuf[512];
    size_t zip_size = make_deflate_zip(zipbuf);
    mz_zip_archive rd;
    unsigned char out[16];

    memset(&rd, 0, sizeof(rd));
    CHECK(mz_zip_reader_init_mem(&rd, zipbuf, zip_size, 0), "deflate archive opens");
    CHECK(!mz_zip_reader_is_file_supported(&rd, 0), "method 8 reported unsupported");
    CHECK(!mz_zip_reader_extract_to_mem(&rd, 0, out, sizeof(out), 0), "method 8 extract rejected");
    CHECK(mz_zip_get_last_error(&rd) == MZ_ZIP_UNSUPPORTED_METHOD, "method 8 yields UNSUPPORTED_METHOD");
    CHECK(mz_zip_reader_end(&rd), "end deflate reader");
}

static void test_interop_writer(const char *scratch)
{
    char name[1024];
    mz_zip_archive zip;
    mz_zip_archive rd;
    unsigned char *data = (unsigned char *)malloc(1 << 20);
    static const char hello[] = "hello from store-only\n";

    CHECK(data != NULL, "interop alloc");
    if (!data)
        return;
    fill_pattern(data, 1 << 20, 5);

    sprintf(name, "%s/interop.zip", scratch);
    memset(&zip, 0, sizeof(zip));
    CHECK(mz_zip_writer_init_file(&zip, name, 0), "interop writer init");
    CHECK(mz_zip_writer_add_mem(&zip, "level9.bin", data, 1 << 20, MZ_BEST_COMPRESSION), "interop add level9");
    CHECK(mz_zip_writer_add_mem(&zip, "hello.txt", hello, sizeof(hello) - 1, 0), "interop add hello");
    CHECK(mz_zip_writer_add_mem(&zip, "dir/", NULL, 0, 0), "interop add dir");
    CHECK(mz_zip_writer_finalize_archive(&zip), "interop finalize");
    CHECK(mz_zip_writer_end(&zip), "interop writer end");

    memset(&rd, 0, sizeof(rd));
    CHECK(mz_zip_reader_init_file(&rd, name, 0), "interop reopen");
    CHECK(mz_zip_validate_archive(&rd, 0), "interop validate");
    CHECK(mz_zip_reader_end(&rd), "interop reader end");
    free(data);
}

int main(int argc, char **argv)
{
    const char *scratch = (argc > 1) ? argv[1] : ".";

    test_mem_roundtrip();
    test_file_roundtrip(scratch);
    test_zip64_flag();
    test_deflate_rejected();
    test_interop_writer(scratch);

    if (failures)
    {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("store-only characterization tests passed\n");
    return 0;
}
