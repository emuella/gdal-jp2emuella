# GDAL JP2Emuella

`gdal-jp2emuella` is an out-of-tree GDAL raster plugin for raw JPEG 2000
Part 1 codestreams decoded by the experimental Emuella C ABI. The plugin is
named `gdal_JP2Emuella.so` and registers the `JP2Emuella` driver through both
`GDALRegister_JP2Emuella` and GDAL's plugin entry point, `GDALRegisterMe`.

This initial driver is deliberately narrow. It opens read-only raw codestreams
whose first markers are exactly SOC and SIZ (`FF4FFF51`). Every component must
be unsigned 8-bit, or the image must contain exactly one unsigned 9–16-bit
component or three unsigned 16-bit components. Components must have unit
horizontal and vertical separation, have the full image dimensions and share
an origin. JP2 wrappers and other inputs are not identified. Accepted components
are exposed as GDAL Byte or native-endian UInt16 bands.
The band `NBITS` metadata retains actual multibyte precision. Intermediate
9–15-bit RGB, signed components and other component geometries remain excluded.

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
request for matching integer source/buffer windows and output matching the
native Byte or UInt16 type. It preserves requested order, including duplicate bands, with deduplication and output
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

## Required source index

Set the GDAL configuration option `JP2EMUELLA_REQUIRE_SOURCE_INDEX=YES` before
opening a dataset to require the codec's persistent Part 1 source index. This
also reaches the nested plugin when the maintained GDAL fork opens an `IC=C8`
NITF. The option is captured at open and applies for that dataset's lifetime.
The default remains the legacy decoder with its existing format admission.

The required mode uses explicit ceilings of 16 MiB of retained marker bytes,
65,536 markers and 65,536 tile parts. The C ABI constructor is lazy, but the
plugin inspects the decoder during GDAL open, constructing the index then and
retaining it for later windows. Header I/O, retained allocation and construction
failures therefore occur at open. Unsupported indexed profiles or exhausted
index budgets produce an error; this mode never retries
with the legacy decoder. The budgets do not bound total process memory or
decoded sample buffers. Marker bytes omit packet bodies and exclude index
descriptors, tile metadata and allocator overhead. The indexed scanner requires
nonzero bounded `Psot` values and a complete validated tile-part sequence;
construction alone does not establish packet decode support. Regional
preparation and selected packet reads still occur for every decode; the index
avoids repeated whole-source header walks.
Per-region geometry and sequence bookkeeping can still scale with tile count.

Callers requiring this behaviour must check that the registered `JP2Emuella`
driver has metadata `JP2EMUELLA_SOURCE_INDEX=REQUIRED_SUPPORTED` before setting
the option and opening the source. This rejects older plugins that would ignore
the configuration option. With `DIAGNOSTICS=YES`, the nested dataset reports
the selected mode and budgets in `EMUELLA_DIAGNOSTICS`. NITF does not forward
that domain; open its `JPEG2000_DATASET_NAME` from the `DEBUG` domain separately
when collecting nested source-callback observations.

```sh
GDAL_DRIVER_PATH=/path/to/plugin-build \
  gdalinfo --config JP2EMUELLA_REQUIRE_SOURCE_INDEX YES -checksum input.ntf
```

## Requirements

- GDAL 3.13 (development files and runtime)
- CMake 3.20 or later and a C++17 compiler
- Ninja for the documented commands
- `emuella-j2k-capi` built from exact revision
  `2568f1c40c83a40f527c7ee8f1600af511e046d0`

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
workspace reuse. A one-iteration benchmark smoke test exercises all nine cells. The precision
suite checks authored tiled unsigned 9–16-bit grayscale and 16-bit RGB pixels,
unaligned and padded
buffers, duplicate-band scatter, type conversion, negative strides, block reads,
workspace reuse and logical source-read counters. The fork-local NITF suite
also checks exact 11/16-bit grayscale and 16-bit RGB full, tile-crossing and edge
pixels.
See the [precision calibration record](docs/precision-calibration.md) for the
real-source observations and remaining profile gaps.

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
| `SOURCE_READ_REQUESTS` | Valid, non-empty codec source callback requests, with the same inclusion and exclusion rules as `SOURCE_BYTES_REQUESTED`. This is a logical VSI request count, not a physical storage-operation count. |
| `SOURCE_INDEX_REQUIRED` | `1` when the dataset selected the required-index decoder at open; `0` for legacy mode. Successful required-mode open has already constructed the index through decoder inspection. |
| `SOURCE_INDEX_MAX_HEADER_BYTES`, `SOURCE_INDEX_MAX_MARKERS`, `SOURCE_INDEX_MAX_TILE_PARTS` | Required-mode construction ceilings: 16,777,216 bytes, 65,536 markers and 65,536 tile parts. All three are zero in legacy mode. These are not measurements of retained heap capacity. |
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

An optional complete pixel comparison consumes the five locked small NITF
fixtures from `emuella-testdata` revision
`2d519ddaf019f10b9e409ea3338d395438486647`. It needs Python 3.11 or newer, without
NumPy or GDAL Python bindings. Generate the lossy PGM reference using the
[fixture owner's independent OpenJPEG check](https://github.com/emuella/emuella-testdata/blob/2d519ddaf019f10b9e409ea3338d395438486647/recipes/independent-nitf-v1.md#verification-and-independent-pixels),
then pass existing absolute paths and a new JSON output path:

```sh
python3 tests/check-independent-nitf.py \
  --gdal-library /path/to/gdal-prefix/lib/libgdal.so \
  --codec-library /path/to/codec-build/libemuella_j2k_capi.so \
  --plugin-library /path/to/plugin-build/gdal_JP2Emuella.so \
  --testdata-source /path/to/emuella-testdata \
  --input-pack /path/to/emuella-testdata/generated/jpeg-2000/independent-nitf \
  --lossy-reference /path/to/openjpeg-check/pan11-lossy.pgm \
  --output /path/to/scratch/independent-nitf-result.json
```

Put the matching GDAL and codec directories before other dependency prefixes
on the runtime library search path. This comparator explicitly registers the
supplied plugin through its public entry point; the canonical installation
check separately proves autoload. It verifies the owner contract and fixture
hashes, actual SIZ/COD precision and coding parameters, the normalised lossy
reference digest, the required-index capability, native sample types and
outer NITF delegation with only NITF and JP2Emuella registered. Four lossless
cases require exact agreement with the owner's scalar oracle. The lossy case
allows peak error at most one against the locked independent decoded reference.
JSON records binary, input and reference identities and all measured aggregate
errors, including failures. This check accepts only the locked small pack;
it performs no acquisition or large-image qualification and saves no pixels.

An opt-in regional journey uses an existing authorised CORE3D Jacksonville WV3
PAN NITF. Configuration checks SHA-256
`61c1ba16ff0c7b1788e912cc143ecf966566cd7c092eb14a111e75a185547e4a`
in place. Set `JP2EMUELLA_SATELLITE_NITF_FIXTURE=/absolute/path/to/source.NTF`
alongside `JP2EMUELLA_TEST_NITF=ON` when running `scripts/check.sh`. This adds
`jp2emuella_satellite_nitf`; CTest verbose output includes aggregate JSON
observations. It opens the outer NITF with alternative JPEG 2000 drivers
isolated, preserves 11-bit native precision, and compares the 32×32 window at
(20000,20000) with a repeat and its containing complete 1024×1024 tile. All
sample buffers stay in memory and PAM sidecars are disabled. The test does not acquire, copy, save or delete
the source or its pixels. Supply only an existing source whose use you have
authorised; the option does not grant rights or accept external terms.

The real-source diagnostic snapshots belong to a separately opened embedded
`JP2Emuella` dataset. They exclude outer NITF reads and are logical codec/VSI
requests, not physical storage traffic. The comparisons establish consistency
within one representation, not independent decoder accuracy or source
losslessness. See [precision calibration](docs/precision-calibration.md).

The fork change and this integration test are agent-assisted, fork-local work.
They must not be submitted to OSGeo/GDAL through an agent workflow; GDAL's
adopted LLM policy requires any possible future upstream contribution to be
human-authored, understood and disclosed under that policy.

## Scope and provenance

The original grayscale and RGB test images use project-authored
`emuella-j2k-test-support` recipes. Their Apache-2.0 provenance is recorded in
[the grayscale record](tests/fixtures/PROVENANCE.toml) and
[the RGB record](tests/fixtures/rgb-mct-256x192.PROVENANCE.toml).
The RGB codestream comes from `native_planes::reversible_mct_region_fixture`
(`tnsot_one`): 256x192, reversible MCT, five decomposition levels and 19 quality
layers. Tests calculate its authored RGB formulae independently of decoding.

The nine precision fixtures use the local
[arithmetic generator](tests/generate-precision.rs), with MIT licence, formulae,
codec revision and digests recorded in
[their provenance record](tests/fixtures/precision-PROVENANCE.toml).
No protected corpus data or external implementation source is included.

This project is licensed under the MIT Licence. Each generated test codestream
retains the licence recorded in its corresponding provenance file.
