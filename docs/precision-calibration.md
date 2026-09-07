# Native precision calibration

The selected adapter profile adds one unsigned 16-bit grayscale component to
the existing unsigned 8-bit component admission. GDAL exposes UInt16 in host
byte order and `NBITS=16`. Matching native reads retain the regional C ABI
workspace; type conversion, resampling and unsupported layouts remain GDAL
operations. The source callback now counts logical requests as well as bytes.
These counters include codec inspection and repeated reads, and exclude GDAL
identification, physical storage operations and cache misses.

## Question, probe and selection

The question was whether the existing source-backed C ABI can preserve
satellite-like precision in regional grayscale and RGB reads. The smallest
probe used authored 67×53 unsigned 16-bit images, 32×32 tiles, two reversible
5/3 decompositions and a 9×11 window at (29,27) crossing four tiles. The
arithmetic oracle includes zero, 65535, values above 255 and low-bit detail.
The plugin owns adapter evidence; the codec owns transform and sample semantics.
Exit requires native sample metadata and exact arithmetic agreement before
admission is widened.

Calibration used codec revision
`aa7090c23cce62437cefe5b441e971b1bd4320b5` and plugin baseline
`2b0a8f28434a4fe23869530bc126f3fe98163577`. The C ABI was rebuilt in release
mode from that exact clean codec checkout. Fixture identities and the complete
formula are in [the provenance record](../tests/fixtures/precision-PROVENANCE.toml).
The fixture encoder consumes only project-authored samples.

| Probe | Observation | Decision |
|---|---|---|
| 9–15-bit Part 1 public encode | Rejects with `baseline encode currently supports 8-bit and 16-bit unsigned samples only` | No fixture qualification; keep admission excluded |
| 16-bit grayscale C ABI tile-crossing decode | Status OK; unsigned precision 16, little-endian representation; all 99 samples agree exactly | Retain grayscale adapter candidate |
| 16-bit RGB C ABI tile-crossing decode | Status unsupported: `selective reversible MCT regions require three matching unsigned 8-bit unit-sampled components` | Reject RGB admission pending codec-owned work |

The plugin does not reinterpret or patch encoded headers to manufacture lower
precision support. The RGB codestream is retained as a deterministic negative
admission fixture, not a successful RGB qualification claim.

## Verification and reproducibility

Build the exact codec and run the ordinary plugin check documented in the
README, with the maintained GDAL fork and `JP2EMUELLA_TEST_NITF=ON`. The
calibration uses GDAL fork revision
`1af54d99959f3b62ba10451a357a969075374663` (development version 3.14), with its
other JPEG 2000 drivers disabled. The project native tests also isolate those
drivers, so NITF pixel success exercises Emuella delegation. No external NITF
fixture is needed for the UInt16 tests.

The canonical `scripts/check.sh` run passed all five CTest suites (native,
precision, RGB, RGB benchmark smoke and NITF), plus installation, export,
dynamic-path and installed-module checksum checks, on Linux x86-64 with GCC
16.1.1. The precision test reports 552 logical source requests and 16,511
requested bytes across its complete open/read sequence. These totals include
repeat preparation and generic GDAL block reads.

The authored tests check all full-image pixels, tile-crossing and edge windows,
unaligned UInt16 destinations, planar and pixel-interleaved padding guards,
duplicate bands in one codec call, Float32 conversion and negative strides,
GDAL block reads and one reused workspace. The first small region reserves
exactly 198 output bytes and retains no full coefficient plane larger than
one 32×32 fixture tile; its coefficient work is lower than a full-image read.
These are fixture-specific work and capacity assertions, not a process-memory
bound or a claim that the decoder avoids repeated codestream preparation.

To regenerate the two fixtures, build `emuella-j2k-capi` at the recorded
revision, then compile [the recipe](../tests/generate-precision.rs) with
`rustc --edition=2024`, `-L dependency=<codec-target>/release/deps` and
`--extern emuella_j2k=<codec-target>/release/deps/libemuella_j2k-<hash>.rlib`.
Run the resulting executable with an output directory and compare both SHA-256
digests to the provenance record. Generation is separate from normal CTest;
plugin tests link only to GDAL and load the module normally.

## Remaining gaps

An authorised integration input inspected in place reports one unsigned 11-bit
component. Its embedded source passes C ABI inspection but this candidate
rejects it at the plugin image-admission predicate: `bits_per_sample != 8 &&
bits_per_sample != 16`. The observed error is `JP2Emuella does not support this
image geometry`; no decode or pixel output was requested. Genuine 11-bit
fixtures and precise `NBITS` preservation are the immediate consumer follow-up.

Unsigned 9–15-bit and 16-bit RGB admission need genuine codec-owned positive
fixtures and regional qualification. Big-endian output conversion is present
but has not run on a big-endian host. Small synthetic NITF precision success
does not qualify large satellite inputs, multispectral interpretation, lossy
preparation, indexed reuse, physical source I/O, peak process memory or browser
presentation. Repeated C ABI region calls still prepare again; persistent
indexes and avoiding repeated scans remain codec work. The broader viewer and
detection proof is not complete.
