# Native precision calibration

The adapter admits the existing unsigned 8-bit components, one unsigned
9–16-bit grayscale component, or three unsigned 16-bit RGB components. All
components must share full dimensions, origin and unit separation. GDAL exposes
multibyte samples as host-endian UInt16 and preserves actual precision in band
`NBITS` metadata. Matching native reads use the regional C ABI workspace;
conversion, resampling and unsupported layouts remain GDAL operations.

## Calibration and source identities

The question was whether the source-backed C ABI preserves satellite-like
precision in regional grayscale and RGB reads. The smallest authored probe
uses 67×53 samples, 32×32 tiles, two reversible 5/3 decompositions and a 9×11
window at (29,27) crossing four tiles. Its arithmetic oracle includes zero,
maximum values, values above 255 and low-bit detail. The plugin owns adapter
evidence; the codec owns sample, transform and codestream semantics.

| Checkpoint | Observation and decision |
|---|---|
| Codec `aa7090c23cce62437cefe5b441e971b1bd4320b5`, plugin `2b0a8f28434a4fe23869530bc126f3fe98163577` | Baseline grayscale16 C ABI probe passed. Public 9–15-bit Part 1 encode and RGB16 regional MCT rejected; keep those admissions excluded provisionally. |
| Plugin `2f8f4e59009e1316be8c5a010a86f20a2716d676` | Authored grayscale16 GDAL/NITF qualification passed. The real 11-bit input passed C ABI inspection but failed the plugin's 8/16-only precision predicate. |
| Codec `b4c4cffa35cbdac0383c2c0fc46ebe8e729f3bfa`, current plugin candidate | Codec owner supplies genuine 9–15-bit grayscale encoding and RGB16 regional MCT. All authored grayscale9–16 and RGB16 plugin tests pass; the supplied real11 NITF journey passes. Retain the wider validated candidate. |

The final consumed codec is merged revision
`2568f1c40c83a40f527c7ee8f1600af511e046d0`; calibration checkpoints above remain
historical observations. The consumed codec is rebuilt in release mode from exact clean source.
The maintained GDAL fork is
`1af54d99959f3b62ba10451a357a969075374663` (development version 3.14), with its
other JPEG 2000 drivers disabled. The tests additionally isolate those drivers,
so successful outer NITF reads prove actual Emuella delegation.

## Authored verification

[Fixture provenance](../tests/fixtures/precision-PROVENANCE.toml) records the
nine codestream identities, complete arithmetic formula and exact generator
codec revision. These are genuine encoded precisions; no header relabelling
is used to manufacture positive precision evidence. RGB uses reversible MCT.

The native precision suite checks full-image and regional agreement for every
gray precision from 9 to 16, UInt16 type and actual `NBITS`, unaligned and padded
buffers, duplicate-band scatter, Float32 conversion, negative strides, block
reads and one reused workspace. RGB16 checks all full-image samples,
tile-crossing and edge windows, reordered/duplicate bands and a single-band
read retaining its MCT dependencies. Each 9×11 region reserves 198 bytes per
distinct output component. Small grayscale regional coefficient work is lower
than full-image work; no full coefficient plane exceeds the authored tile.
The fork-local NITF suite checks authored grayscale11, grayscale16 and RGB16
full, tile-crossing and edge samples against the independent formula.

The canonical `scripts/check.sh` passed all six suites (native, precision, RGB,
RGB benchmark smoke, NITF and opt-in satellite NITF), plus installation,
registration exports, dynamic-path checks and installed-module checksum on
Linux x86-64 with GCC 16.1.1. All default tests use project-authored inputs.

To regenerate fixtures, build `emuella-j2k-capi` at the recorded revision, then
compile [the recipe](../tests/generate-precision.rs) with
`rustc --edition=2024`, `-L dependency=<codec-target>/release/deps` and
`--extern emuella_j2k=<codec-target>/release/deps/libemuella_j2k-<hash>.rlib`.
Run the executable with an output directory and compare SHA-256 digests to the
provenance record. Normal CTest does not need Rust or regenerate fixtures;
the native test executables link only to GDAL and autoload the plugin.

## Authorised real-source observation

The caller supplied an existing CORE3D Jacksonville WV3 PAN NITF with SHA-256
`61c1ba16ff0c7b1788e912cc143ecf966566cd7c092eb14a111e75a185547e4a`.
The test verifies its identity during configuration and reads it in place.
PAM sidecars are disabled. No source bytes, codestream payload or decoded pixels
are retained outside
its authorised store; all sample comparisons are memory-only.

The source is 43008×43008, one unsigned 11-bit component in UInt16 storage,
with 1024×1024 tiles. The embedded codestream occupies 822,909,368 bytes at
NITF offset 4192. The outer route is NITF delegating to JP2Emuella. A 32×32
window at (20000,20000) produces 1,024 samples with range 160–342. Its pixels
agree exactly across the outer NITF read, direct embedded read, repeated direct
read and crop of the complete containing 1024×1024 tile at (19456,19456).
This is consistency within the same encoded representation, not independent
external-decoder accuracy or source losslessness.

Diagnostic snapshots belong to a separately opened nested JP2Emuella handle.
They do not count outer NITF I/O. Valid non-empty codec callback requests include
inspection and repeated preparation; GDAL identification and physical storage
operations/cache misses are excluded.

| Stage (increment) | Logical reads | Requested bytes | Code-blocks | Tier-1 coefficients | Output samples |
|---|---:|---:|---:|---:|---:|
| Nested open | 84,687 | 731,915 | 0 | 0 | 0 |
| First 32×32 region | 10,977 | 715,481 | 25 | 90,112 | 1,024 |
| Repeated 32×32 region | 10,977 | 715,481 | 25 | 90,112 | 1,024 |
| Complete selected 1024×1024 tile | 12,513 | 1,150,377 | 259 | 1,048,576 | 1,048,576 |

The dataset creates one workspace. Peak C ABI output capacity is 2,048 bytes
after each small read and 2,097,152 bytes after the complete tile. Retained
codec workspace capacity is 178,803 bytes after the small reads and 186,995
bytes after the tile; full coefficient-plane capacity remains zero. A separate
execution of this real-source test recorded 45,920 KiB maximum process RSS
with `/usr/bin/time`, including both open datasets, GDAL, codec and in-memory
comparison buffers. No full-image allocation was observed; the largest caller
sample buffer and codec output correspond to one selected tile. This is one
fixture/build measurement, not a universal process-memory bound.

To run this optional journey, set `JP2EMUELLA_TEST_NITF=ON` and
`JP2EMUELLA_SATELLITE_NITF_FIXTURE=/absolute/path/to/source.NTF` alongside the
normal exact codec and GDAL build settings when invoking `scripts/check.sh`.
The source must already exist under the caller's applicable authority.
`ctest --test-dir <build> -R '^jp2emuella_satellite_nitf$' -V` displays the
aggregate JSON snapshots without saving pixel payloads.

## Remaining gaps

Intermediate-precision RGB, other multispectral layouts and big-endian host
execution remain unqualified. Neither this single real image nor authored
samples prove universal satellite interoperability, independent decoder
agreement, lossy preparation, physical storage behaviour or browser delivery.
The real-source observations above used the legacy source-backed decoder. It
still prepares each region and repeats source-header traversal, reflected by
the identical first/repeat request costs. Those historical numbers do not
measure the opt-in indexed mode described below. The broader viewer and
detection proof requires separate composed qualification.

## Opt-in persistent Part 1 source index

`JP2EMUELLA_REQUIRE_SOURCE_INDEX=YES`, set before open, selects the additive
indexed C ABI constructor for the lifetime of that dataset, including an
embedded NITF image. Although the C ABI constructor is lazy, plugin inspection
constructs the retained index during GDAL open. Header I/O, retained allocation
and construction failures occur then; later regional reads reuse the index.
Explicit construction ceilings are 16 MiB of retained marker bytes, 65,536 markers and 65,536 tile parts. An
unsupported profile or budget failure is an error without legacy fallback. Default opens
keep the existing decoder and admission. The index avoids repeated
whole-source header traversal; it does not cache decoded regions or eliminate
selected packet reads and regional preparation. Retained marker bytes exclude
packet bodies, index descriptors, tile metadata and allocator overhead. The
indexed scanner requires bounded nonzero `Psot` values and a validated complete
tile-part sequence; packet decoding retains the codec's existing profile
requirements. Per-region geometry and sequence bookkeeping can still scale
with the total tile count.

The registered driver advertises
`JP2EMUELLA_SOURCE_INDEX=REQUIRED_SUPPORTED`; preparation callers must check
this capability before relying on the config option. Diagnostic nested datasets
report `SOURCE_INDEX_REQUIRED` and the three `SOURCE_INDEX_MAX_*` ceilings.
These report mode and construction limits, not index heap measurements. NITF
does not forward the nested diagnostic domain, so logical callback counts are
collected from a separately opened embedded handle and exclude outer NITF I/O.

The authored NITF test reads disjoint 7×5 regions at (1,1) and (35,35), then
repeats the second region on one open dataset. Both outer NITF and direct
embedded UInt16 samples must match the independent arithmetic oracle.
Indexed callback requests and bytes for the second region must be lower than
the same operation in legacy mode, and the repeat must retain that cost with
one workspace. Changing the option after open must leave existing decoders in
their original mode. A separate authored input adds valid COM segments beyond
16 MiB: the default path must decode it, and the required path must reject it
without fallback. This protects compatibility and the opt-in budget contract.

Run these checks against the exact codec source and maintained GDAL prefix:

```sh
JP2EMUELLA_TEST_NITF=ON \
GDAL_CONFIG=/path/to/emuella-gdal-prefix/bin/gdal-config \
GDAL_PREFIX=/path/to/emuella-gdal-prefix \
EMUELLA_J2K_SOURCE_DIR=/path/to/emuella-j2k \
./scripts/check.sh
ctest --test-dir build -R '^jp2emuella_nitf$' -V
```

The source-index probe prints aggregate legacy/indexed callback costs. It
uses only project-authored samples and creates no protected-image derivatives.
Real-scene indexed ingestion and browser delivery are not established by this
adapter test.

## Locked independent NITF pixel comparison

The optional [standard-library comparator](../tests/check-independent-nitf.py)
consumes the five small `jpeg-2000/independent-nitf` cases owned by
`emuella-testdata` revision `2d519ddaf019f10b9e409ea3338d395438486647`.
Pinned owner checker, recipe, manifest and provenance hashes bind both the
complete fixture inventory and its genuine precision/coding profiles. The
owner's scalar oracle supplies lossless reference samples at runtime. The
supplied lossy PGM must match the owner's normalised independent OpenJPEG pixel
digest before any comparison. Fixture and oracle semantics remain with the
test-data repository; no fixture payload is included here.

The comparator explicitly registers the supplied GDAL NITF driver and plugin
through public exports and checks codec symbol binding against the supplied
library. Only NITF and JP2Emuella may be registered. It requires
`REQUIRED_SUPPORTED`, sets `JP2EMUELLA_REQUIRE_SOURCE_INDEX=YES` before open,
checks C8/ABPP and native types, and verifies nested NBITS and indexed mode.
Complete outer NITF band reads cover all locked samples, including both tiles
of the 1057×65 PAN cases. The four reversible cases require zero pixel error;
the irreversible U11 case permits peak error at most one against the independent
decoded reference. Source-oracle errors are reported separately from errors
against that lossy reference.

The [README command](../README.md#fork-local-nitf-integration) accepts explicit
binary, owner, pack and reference paths and an exclusively created JSON result.
The result retains exact source and binary SHA-256 identities, the comparator
identity, sample counts, peak/squared errors, mismatch counts and normalised
decoded hashes. It does not prove large-image memory or I/O bounds, independent
autoload, NPJE conformance or original satellite-product qualification.
