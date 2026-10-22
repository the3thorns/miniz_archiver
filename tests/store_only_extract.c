/* Small CLI used by the Python characterization driver.
 *
 * Usage: store_only_extract <archive.zip> <entry name> <output file>
 *
 * Extracts one entry through mz_zip_reader_extract_to_callback(), so the
 * comparison works for entries larger than memory as well.
 */
#include "miniz.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static size_t file_write_cb(void *opaque, mz_uint64 ofs, const void *buf, size_t n)
{
    FILE *f = (FILE *)opaque;
    (void)ofs;
    return fwrite(buf, 1, n, f);
}

int main(int argc, char **argv)
{
    mz_zip_archive zip;
    FILE *out;
    int ok;

    if (argc != 4)
    {
        fprintf(stderr, "usage: %s <archive.zip> <entry|--validate-all> <output|->\n", argv[0]);
        return 2;
    }

    memset(&zip, 0, sizeof(zip));
    if (!mz_zip_reader_init_file(&zip, argv[1], 0))
    {
        fprintf(stderr, "cannot open archive: %s\n", mz_zip_get_error_string(mz_zip_get_last_error(&zip)));
        return 1;
    }

    if (strcmp(argv[2], "--validate-all") == 0)
    {
        ok = mz_zip_validate_archive(&zip, 0);
        if (!ok)
            fprintf(stderr, "validate failed: %s\n", mz_zip_get_error_string(mz_zip_get_last_error(&zip)));
        mz_zip_reader_end(&zip);
        return ok ? 0 : 1;
    }

    out = fopen(argv[3], "wb");
    if (!out)
    {
        fprintf(stderr, "cannot open output\n");
        mz_zip_reader_end(&zip);
        return 1;
    }

    ok = mz_zip_reader_extract_file_to_callback(&zip, argv[2], file_write_cb, out, 0);
    fclose(out);
    if (!ok)
    {
        fprintf(stderr, "extract failed: %s\n", mz_zip_get_error_string(mz_zip_get_last_error(&zip)));
        mz_zip_reader_end(&zip);
        return 1;
    }

    mz_zip_reader_end(&zip);
    return 0;
}
