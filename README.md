# GDAL JP2Emuella

`gdal-jp2emuella` is an out-of-tree GDAL raster plugin for raw JPEG 2000
Part 1 codestreams decoded by the experimental Emuella C ABI. The plugin is
named `gdal_JP2Emuella.so` and registers the `JP2Emuella` driver through both
`GDALRegister_JP2Emuella` and GDAL's plugin entry point, `GDALRegisterMe`.

This initial driver is deliberately narrow. It opens read-only raw codestreams
whose first markers are exactly SOC and SIZ (`FF4FFF51`). Every component must
be unsigned 8-bit, have unit horizontal and vertical separation, have the full
image dimensions and share an origin. JP2 wrappers and other inputs are not
identified. Accepted components are exposed as GDAL Byte bands.

The plugin uses only public GDAL APIs and `emuella_j2k.h`. Each dataset owns its
VSI handle for its entire decoder lifetime. Codec positioned reads are bounded,
serialised around seek/read pairs and contained by a `noexcept` callback. Each
active region decode owns a separate Emuella workspace, so repeated and
concurrent reads do not share workspace state.

## Requirements

- GDAL 3.13 (development files and runtime)
- CMake 3.20 or later and a C++17 compiler
- Ninja for the documented commands
- `emuella-j2k-capi` built from exact revision
  `75083930b6e533053b7f5c3c4dda7b4a2c0a0c38`

The Emuella ABI is pre-1.0. The CMake revision check is performed when
`EmuellaJ2K_SOURCE_DIR` is supplied; callers providing only installed headers
and a library are responsible for establishing their provenance.

## Build and test

Build the codec first:

```sh
cd /path/to/emuella-j2k
cargo build --locked --release -p emuella-j2k-capi
```

Then configure this plugin. The cache inputs permit non-standard installations:

```sh
cmake -S . -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH=/path/to/gdal-prefix \
  -DEmuellaJ2K_SOURCE_DIR=/path/to/emuella-j2k \
  -DEmuellaJ2K_INCLUDE_DIR=/path/to/emuella-j2k/crates/emuella-j2k-capi/include \
  -DEmuellaJ2K_LIBRARY=/path/to/emuella-j2k/target/release/libemuella_j2k_capi.so
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

The native test executable links to GDAL only. CTest sets `GDAL_DRIVER_PATH` to
the module output directory, so driver registration and every decode exercise
normal GDAL plugin autoloading rather than a test-only static link. Tests cover
metadata, full and windowed pixels, edge reads, `/vsimem/`, `/vsisubfile/`,
malformed and unsupported inputs, concurrent reads and open/close lifecycle.

`scripts/check.sh` requires `EMUELLA_J2K_SOURCE_DIR`. It derives the codec
library from that checkout and the GDAL prefix from `gdal-config` by default:

```sh
EMUELLA_J2K_SOURCE_DIR=/path/to/emuella-j2k ./scripts/check.sh
```

Override `JP2EMUELLA_BUILD_DIR`, `EMUELLA_J2K_LIBRARY`, `GDAL_CONFIG`,
`GDAL_PREFIX`, `GDALINFO_COMMAND`, `CMAKE_COMMAND` or `CTEST_COMMAND` when
required. On Linux the check also verifies the two registration exports,
confirms that the installed module has no host-absolute `NEEDED`, `RPATH` or
`RUNPATH` entry, and loads that installed module through `gdalinfo` to reproduce
the project-authored fixture checksum.

## Install and run

Install into a private directory or GDAL's plugin directory:

```sh
cmake --install build --prefix /path/to/private-prefix
export GDAL_DRIVER_PATH=/path/to/private-prefix/lib/gdalplugins
export LD_LIBRARY_PATH=/path/to/emuella-j2k/target/release:${LD_LIBRARY_PATH:-}
gdalinfo input.j2k
```

Alternatively, place `gdal_JP2Emuella.so` in the directory reported by
`gdal-config --plugindir`. The Emuella shared library must remain discoverable
by the platform dynamic loader. Use `GDAL_DRIVER_PATH` to select an uninstalled
build.

## Fork-local NITF integration

Stock GDAL's NITF driver explicitly allows only selected JPEG 2000 drivers to
inspect an embedded `IC=C8` image segment. The maintained `emuella/gdal` fork
adds `JP2Emuella` to that narrow list; it does not permit arbitrary drivers to
inspect embedded content.

Build and install that fork, then point this project at its exact prefix and
enable the opt-in NITF journey:

```sh
JP2EMUELLA_TEST_NITF=ON \
GDAL_CONFIG=/path/to/emuella-gdal-prefix/bin/gdal-config \
GDAL_PREFIX=/path/to/emuella-gdal-prefix \
GDALINFO_COMMAND=/path/to/emuella-gdal-prefix/bin/gdalinfo \
EMUELLA_J2K_SOURCE_DIR=/path/to/emuella-j2k \
./scripts/check.sh
```

The journey creates an uncompressed 17x19 NITF skeleton through GDAL, replaces
its image payload with the existing project-authored raw codestream, and marks
the image segment as `IC=C8`. It then checks the outer NITF metadata, the nested
`JP2Emuella` identity, complete pixels, a window and an edge read. A negative
probe removes `JP2Emuella` temporarily and proves that a deliberately matching
unlisted driver is not asked to inspect the embedded codestream.

The fork change and this integration test are agent-assisted, fork-local work.
They must not be submitted to OSGeo/GDAL through an agent workflow; GDAL's
adopted LLM policy requires any possible future upstream contribution to be
human-authored, understood and disclosed under that policy.

## Scope and provenance

The test image is generated deterministically by the project-authored
`emuella-j2k-test-support` gradient recipe. Its provenance is recorded beside
the fixture. No protected corpus data or external implementation source is
included.

This project is licensed under the MIT Licence. The generated test codestream
is Apache-2.0 as recorded in its provenance file.
