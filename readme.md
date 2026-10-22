# Miniz_archiver

This repo contains a modified version of minz.

## Miniz (store-only fork)

This is a destructive, store-only fork of [miniz](https://github.com/richgel999/miniz),
a small C library for ZIP archive reading and writing. The Deflate/Inflate
codecs and the zlib-style API have been removed from the source tree entirely.
The library stores and extracts uncompressed ZIP entries and rejects entries
that use any compression method other than `stored` (`0`).

## Usage

The build can generate a standalone pair of `miniz.c`/`miniz.h` files in
`amalgamation/` via `./amalgamate.sh` or `-DAMALGAMATE_SOURCES=ON`. The
library can also be used as a CMake or Meson module. All declarations live in
the single public header `miniz.h`, and the whole implementation lives in
`miniz.c`.

## Features

* MIT licensed.
* Portable C sources, tested with GCC, clang and Visual Studio.
* ZIP64-capable reader, writer, appender, iterator, clone, validation and
  in-place update paths.
* Stored entries only. There is no compressor, decompressor or zlib-style API,
  and no build option can add one.
* CRC-32 (`mz_crc32`), allocator helpers and `mz_version()` remain available.
* Compression levels are accepted by writer calls for source compatibility but
  are ignored; output is always stored.

## Known limitations

* Reading or validating a Deflate-compressed entry fails with
  `MZ_ZIP_UNSUPPORTED_METHOD`.
* `MZ_ZIP_FLAG_COMPRESSED_DATA` can raw-copy caller-supplied bytes and tags
  them as method `8`, but this fork cannot read such entries back as
  uncompressed data.
* No encrypted archives and no PNG writer.
* Minimal documentation; see `PATCHES.md` and `examples/example2.c`.

## Building

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

See `PATCHES.md` for the complete list of changes, the test commands and the
maintenance checks.

## Special Thanks

Thanks to Rich Geldreich and the miniz contributors. This fork only removes
code; the ZIP implementation is theirs.

## License

See `LICENSE`. The original miniz code is public domain / MIT licensed as
described there.
