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
dataset lazily creates one Emuella workspace and serialises all of its
codec decodes and output copies with a dataset mutex. Repeated band, block and
combined reads reuse that workspace. Different datasets have independent
workspaces. The decode mutex is acquired before the VSI mutex; the source
callback acquires only the VSI mutex. The workspace and decoder are destroyed
before the source context and file.

Dataset RasterIO combines up to four distinct bands into one regional codec
request for matching integer source/buffer windows and Byte output. It preserves
requested order, including duplicate bands, with deduplication and output
scatter. Supported positive layouts include planar and pixel-interleaved
buffers with pixel, row and band padding. Offset arithmetic is bounded before
pointer arithmetic. A reusable dataset scratch plane supports scatter; a
contiguous planar destination receives each component directly.

Requests with more than four distinct bands, unsupported or overlapping layouts,
other output types, resampling, a valid floating-point source window or a
progress callback use GDAL's generic path. Band reads use the same workspace;
block reads reached through GDAL's generic path also reuse it. Cancellation is
observed at GDAL progress checkpoints, not within a codec decode. Concurrent
matching direct reads are serialised and tested. This does not establish general
thread safety for GDAL metadata, block-cache operations or dataset destruction;
close a dataset only after its readers have finished.

## Requirements

- GDAL 3.13 (development files and runtime)
- CMake 3.20 or later and a C++17 compiler
- Ninja for the documented commands
- `emuella-j2k-capi` built from exact revision
  `553b941911e35863c1eed7f23be8a41e1c4caa84`

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
metadata, full and windowed pixels, fractional-window nearest-neighbour and
bilinear reads against the generic GDAL path, progress and cancellation, edge reads, `/vsimem/`, `/vsisubfile/`,
malformed and unsupported inputs, concurrent reads and open/close lifecycle.
The RGB suite additionally checks reversible-MCT pixels, full images, band order,
duplicates, padding guards, negative-stride and type-conversion fallback,
fractional resampling, dataset progress/cancellation, shared MCT work and
workspace reuse. A one-iteration benchmark smoke test exercises all nine cells.

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

## RGB benchmark and diagnostics

The native benchmark links only to GDAL and uses normal plugin autoloading. It
reads the project-authored RGB fixture as one band, three separate band calls,
and one combined RGB call. Each mode covers repeated 64x64 windows at (0,0),
adjacent 64x64 windows cycling through x=0,64,128,192 at y=0, and repeated full
images. Run it after building:

```sh
GDAL_DRIVER_PATH=/path/to/plugin-build \
  /path/to/plugin-build/jp2emuella_rgb_benchmark 9 > candidate.jsonl
```

The optional iteration count is 1–1000 (default 9). Output is JSON Lines with
one record per mode/scenario. `read_ns` contains each complete RasterIO operation
in nanoseconds (all three calls for `separate_rgb`); `open_ns` measures its
dataset open. A dataset stays open throughout a cell. The first sample starts
with a fresh dataset/workspace; later samples reuse it. Buffers are allocated
before timing and each output is retained and checked against the independent
RGB oracle after timing. There is no explicit warm-up or filesystem cache flush.
A fresh dataset does not imply a cold OS, VSI or storage cache.

Timing runs leave diagnostics disabled. An additional untimed run performs the
same operations with `DIAGNOSTICS=YES` and records snapshots after open, after
the first operation and after all operations. To measure a previous plugin,
point `GDAL_DRIVER_PATH` at its build while running this same executable, and
ensure its matching codec library is selected by the dynamic loader. Older
plugins report `metrics_available:false` and omit snapshots. Capture the plugin,
codec and GDAL revisions, build configuration, CPU, iteration count and library
selection alongside benchmark output when reporting a comparison. Timing claims
apply only to the measured fixture and environment.

Applications can opt in with the `DIAGNOSTICS=YES` open option and read
`GetMetadata("EMUELLA_DIAGNOSTICS")` after RasterIO. Values are decimal unsigned
integers; `SCHEMA_VERSION=1` identifies this schema. Snapshots are cumulative for
one dataset lifetime. Copy the returned metadata before requesting another
snapshot, and query it after concurrent readers have joined. The domain is
absent when diagnostics are disabled. No counters or codec work statistics are
collected by default.

| Field | Meaning and scope |
|---|---|
| `SOURCE_BYTES_REQUESTED` | Bytes requested by valid, non-empty codec source callbacks, including inspection during open and repeated reads; includes requests that subsequently fail VSI I/O. It excludes GDAL's initial identification reads and is not disk traffic or cache misses. |
| `DECODE_COUNT`, `preparation_count` | Successful codec region calls and their preparations. Repeated windows prepare again; workspace reuse is not region-plan or decoded-image caching. |
| `WORKSPACE_CREATIONS` | Successful dataset workspace creations (normally zero before the first decode, then one). |
| `code_blocks_decoded`, `tier1_coefficients`, `dwt_samples` | Accumulated codec code-block, Tier-1 coefficient and DWT work counters for successful decodes. |
| `synthesis_coefficients_loaded`, `synthesis_horizontal_values`, `synthesis_vertical_values`, `synthesis_lifting_updates`, `synthesis_output_samples` | Accumulated codec synthesis work counts. These count actual codec operations; they are not elapsed time. |
| `windowed_synthesis_component_tiles`, `full_synthesis_component_tiles` | Accumulated component-tile synthesis paths selected by the codec. |
| `output_allocation_count`, `output_allocation_bytes` | Accumulated successful C ABI output-plane reservation requests and their logical requested bytes. Excludes descriptors, plans, workspace, plugin scatter, GDAL, allocator metadata and copies. |
| `PEAK_output_capacity_bytes` | Largest combined output-plane capacity in bytes observed at a successful decode. |
| `PEAK_workspace_retained_heap_bytes` | Largest codec-reported retained workspace heap capacity, including worker buffers. This is capacity, not all allocations or a process memory peak. |
| `PEAK_coefficient_capacity`, `PEAK_segment_capacity`, `PEAK_transform_capacity`, `PEAK_full_coefficient_plane_capacity`, `PEAK_full_transform_scratch_capacity` | Largest codec-reported workspace capacities; coefficient/transform capacities are sample slots and segment capacity is bytes. See the C header for exact buffer scope. |
| `SCATTER_GROWTH_REQUESTS`, `PEAK_SCATTER_CAPACITY_BYTES` | Plugin scratch-plane growth requests and its retained byte capacity. Contiguous planar reads need no scatter plane. These exclude codec allocations. |

Subtract cumulative work snapshots to obtain per-operation or interval counts;
capacity fields are lifetime maxima and must not be treated as allocation
counts or subtracted to estimate total allocation. No field claims to measure
all allocator calls, total allocated bytes, process RSS, or peak live memory.
The benchmark exposes the initial snapshot so source inspection is not silently
attributed to the first region read.

The [recorded RGB benchmark](docs/rgb-region-benchmark.md) contains a complete
comparison against the previous plugin, with raw timing and work observations.

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

An additional qualification journey can use the caller's existing copy of
GDAL's `test_jp2_ecw33.ntf`. Supply its absolute path; configuration verifies
the expected SHA-256 digest before the test reads it in place:

```sh
JP2EMUELLA_TEST_NITF=ON \
JP2EMUELLA_GDAL_NITF_FIXTURE=/absolute/path/to/test_jp2_ecw33.ntf \
GDAL_CONFIG=/path/to/emuella-gdal-prefix/bin/gdal-config \
GDAL_PREFIX=/path/to/emuella-gdal-prefix \
GDALINFO_COMMAND=/path/to/emuella-gdal-prefix/bin/gdalinfo \
EMUELLA_J2K_SOURCE_DIR=/path/to/emuella-j2k \
./scripts/check.sh
```

The external fixture is optional and is never copied into this project or its
build tree. Without the path, all default and fork-local checks continue to use
only project-authored inputs.

The fork change and this integration test are agent-assisted, fork-local work.
They must not be submitted to OSGeo/GDAL through an agent workflow; GDAL's
adopted LLM policy requires any possible future upstream contribution to be
human-authored, understood and disclosed under that policy.

## Scope and provenance

The test images are generated deterministically by project-authored
`emuella-j2k-test-support` recipes. Their provenance is recorded beside the
fixtures. The RGB codestream comes from `native_planes::reversible_mct_region_fixture`
(`tnsot_one`): 256x192, reversible MCT, five decomposition levels and 19 quality
layers. Tests calculate its authored RGB formulae independently of decoding. No
protected corpus data or external implementation source is included.

This project is licensed under the MIT Licence. The generated test codestreams
are Apache-2.0 as recorded in their provenance files.
