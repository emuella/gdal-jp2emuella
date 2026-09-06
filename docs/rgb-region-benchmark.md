# GDAL RGB region benchmark, 6 September 2026

The combined dataset RasterIO path removes two repeated preparations and two
reconstructions of every reversible-MCT dependency for one RGB request. The
measurements below use the project-authored 256×192 RGB fixture; they do not
establish a universal wall-clock speedup or physical-storage savings.

## Reproduction and identities

[Raw samples and counter snapshots](benchmarks/rgb-region-2026-09-06.json) record
the complete nine-cell matrix, environment and exact source revisions. The
baseline plugin is `29ff028ca3ef58f5f77c3b42f326d15bd6b750d3`, built against
MCT-capable codec `5e090873fd82c6761eacbe5e5c2fcf8c28169a71` using the CMake
required-revision override. The candidate plugin is
`0a264bf10d07c1b1ea1443bd77cd4048d51efa89`, built against codec
`2afed91c081759e1f9af9c6f831aed39692abacd`. Final dependency pins do not alter
the measured implementation.

Run the README benchmark command with 31 iterations for each plugin build.
The two complete runs executed sequentially after builds finished, using GDAL
3.13.0, release builds and an AMD Ryzen 9 9950X3D on Linux x86-64. The source
fixture digest is
`6e0b2c084490ea9b3898ee7d78602243de850212c62a35a2340ddf1428e4b140`.
No external codec or protected fixture was used.

Each cell retains one dataset and preallocates its output buffers. Sample one
starts with a fresh dataset workspace; the table reports the median of the
remaining 30 samples. Raw data retains all 31 samples and open time. No cache
flush is performed; a fresh dataset does not mean a cold filesystem cache.
Timing disables diagnostics. A separate untimed run records after-open,
after-first-read and after-all-reads observations and validates every output.

## Elapsed time

| Region pattern | Read | Baseline median ms | Candidate median ms | Baseline/candidate |
|---|---|---:|---:|---:|
| same_window | one_band | 8.794 | 8.664 | 1.02 |
| same_window | separate_rgb | 25.780 | 25.524 | 1.01 |
| same_window | combined_rgb | 25.456 | 8.526 | 2.99 |
| adjacent_windows | one_band | 12.546 | 12.612 | 0.99 |
| adjacent_windows | separate_rgb | 37.869 | 37.975 | 1.00 |
| adjacent_windows | combined_rgb | 37.642 | 12.670 | 2.97 |
| full_image | one_band | 17.092 | 17.095 | 1.00 |
| full_image | separate_rgb | 51.231 | 51.078 | 1.00 |
| full_image | combined_rgb | 52.066 | 17.316 | 3.01 |

The small one-band and separate-RGB differences do not support a material
workspace-only latency improvement on this fixture. Combined RGB is about
three times faster in these runs. The work counters below establish the removed
computation independently of filesystem cache state.

## Observed work and allocations

For the first 64×64 region at (0, 0), subtract the after-open snapshot:

| Observation | One band | Three separate bands | Combined RGB |
|---|---:|---:|---:|
| Source callback bytes requested | 228,009 | 684,027 | 228,009 |
| Preparations | 1 | 3 | 1 |
| Tier-1 blocks decoded | 48 | 144 | 48 |
| Tier-1 coefficient slots | 73,728 | 221,184 | 73,728 |
| Synthesis component-tiles | 3 | 9 | 3 |
| Synthesis output samples | 12,288 | 36,864 | 12,288 |
| Output-plane reservation requests | 1 | 3 | 3 |
| Output-plane requested bytes | 4,096 | 12,288 | 12,288 |

The full-image separate/combined comparison is 225/75 Tier-1 blocks, 9/3
synthesis component-tiles and 906,243/302,081 callback bytes. Adjacent windows
cycle through x=0,64,128,192 and have position-dependent work; all cumulative
snapshots are retained in the raw record.

Every candidate cell creates one workspace across all 31 operations (93 decode
calls for separate RGB). Contiguous planar output needs no plugin scatter
growth. RGB output reservations remain three planes per operation: combining
removes dependency computation, not the required RGB result storage.

Workspace capacity observations show retained storage, not allocation calls or
per-operation allocation growth. Output reservation counters exclude plans,
descriptors, transform internals, GDAL and allocator overhead; total process
allocation traffic and RSS were not measured. Source bytes count codec callback
requests, including repeated preparation reads, not disk bytes or cache misses.

Repeated same-region reads still prepare on every call. This work reuses the
dataset workspace; it introduces neither source/index reuse beyond the existing
decoder lifetime nor a region-plan or decoded-image cache. The measurements
qualify the authored RGB fixture and direct planar path. Layout/fallback
correctness has separate native tests; no timing claim is made for resampling,
remote VSI sources, general GDAL concurrency or other codec profiles.
