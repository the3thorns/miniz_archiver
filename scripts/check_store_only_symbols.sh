#!/bin/sh
# Verify that a build contains no deflate/inflate code or symbols.
set -eu

BUILD_DIR=${1:-_build/store}
ROOT=$(cd "$(dirname "$0")/.." && pwd)
LIB=$(find "$BUILD_DIR" -type f \( -name 'libminiz*.a' -o -name 'miniz*.lib' \) | head -n 1)

if [ -z "$LIB" ]; then
    echo "no miniz static library found under $BUILD_DIR" >&2
    exit 1
fi

echo "checking $LIB"

if command -v nm >/dev/null 2>&1; then
    if nm "$LIB" 2>/dev/null | grep -E '(tdefl_|tinfl_|mz_compress|mz_uncompress|mz_deflate|mz_inflate|mz_adler32)'; then
        echo "ERROR: codec symbols found in library" >&2
        exit 1
    fi
fi

if command -v ar >/dev/null 2>&1; then
    members=$(ar t "$LIB")
    if echo "$members" | grep -E '(tdef|tinfl)'; then
        echo "ERROR: codec object files linked into library" >&2
        exit 1
    fi
fi

if grep -RInE '(tdefl_|tinfl_|mz_compress|mz_uncompress|mz_deflate|mz_inflate|mz_adler32)' \
    "$ROOT" --include='*.c' --include='*.h' \
    --exclude-dir=_build --exclude-dir=build --exclude-dir=bin --exclude-dir=.git; then
    echo "ERROR: codec source references found" >&2
    exit 1
fi

echo "codec symbol check passed"
