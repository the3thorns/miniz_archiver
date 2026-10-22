#!/bin/sh
# Merge a new upstream miniz release and verify the build.
#
# Usage: scripts/sync-upstream.sh [remote] [branch]
# Defaults: remote=upstream, branch=master
#
# The merge is intentionally left with conflicts for the maintainer to resolve;
# nothing is discarded or rewritten automatically. Upstream codec sources must
# be removed again after resolving, because this fork is store-only.
set -eu

REMOTE=${1:-upstream}
BRANCH=${2:-master}
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"

git fetch "$REMOTE"
BASE=$(git rev-parse --short "$REMOTE/$BRANCH")
NAME="sync-upstream-$BASE"

git checkout -b "$NAME"
if ! git merge --no-edit "$REMOTE/$BRANCH"; then
    echo "Merge conflicts in $NAME. Resolve them, remove any codec files, fold the ZIP implementation into miniz.c, then run:" >&2
    echo "  scripts/check_store_only_symbols.sh _build/sync-check" >&2
    exit 1
fi

cmake -S . -B _build/sync-check -DBUILD_TESTS=ON -DBUILD_EXAMPLES=OFF
cmake --build _build/sync-check -j
ctest --test-dir _build/sync-check --output-on-failure
scripts/check_store_only_symbols.sh _build/sync-check

echo "Upstream $REMOTE/$BRANCH ($BASE) merged cleanly into $NAME."
