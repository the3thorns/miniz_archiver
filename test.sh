#!/bin/bash

set -e

. amalgamate.sh

cat << "EOF" > miniz_export.h
#ifndef MINIZ_EXPORT
#define MINIZ_EXPORT
#endif
EOF

gcc tests/store_only_tests.c amalgamation/miniz.c -o store_only_tests \
    -Iamalgamation -ggdb -O2
gcc tests/store_only_extract.c amalgamation/miniz.c -o store_only_extract \
    -Iamalgamation -ggdb -O2
mkdir -p test_scratch
./store_only_tests test_scratch
python3 tests/characterize.py --extractor ./store_only_extract \
    --interop test_scratch/interop.zip --expect-stored --reject-deflate \
    --entries-70k --sparse-zip64 --workdir test_scratch/corpus
