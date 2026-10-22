// Copyright 2025 Google LLC
// Fuzz target for miniz ZIP validation and checksum functions.

#include "miniz.h"
#include <stdint.h>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size < 4) return 0; // Need at least a minimal ZIP header

    mz_zip_error zip_error = MZ_ZIP_NO_ERROR;
    mz_zip_validate_mem_archive(
        data, size, MZ_ZIP_FLAG_DO_NOT_SORT_CENTRAL_DIRECTORY, &zip_error);

    zip_error = MZ_ZIP_NO_ERROR;
    mz_zip_validate_mem_archive(data, size, 0, &zip_error);

    if (size > 1024) {
        zip_error = MZ_ZIP_NO_ERROR;
        mz_zip_validate_mem_archive(data, 1024, 0, &zip_error);
    }

    mz_crc32(MZ_CRC32_INIT, data, size);

    return 0;
}
