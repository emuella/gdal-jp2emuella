# Contributing

Contributions should keep this driver out of the GDAL source tree and use only
public GDAL interfaces. Keep codec interaction behind `emuella_j2k.h`, preserve
the decoder/source lifetime relationship, and give every simultaneous decode
its own workspace.

Use Australian English in prose and comments while preserving established API,
standards and product names. Do not add protected corpus material or source
copied from another JPEG 2000 implementation. New format admissions need small,
project-authored fixtures and native tests that load the module through GDAL.

Before proposing a change, build the required exact Emuella codec revision and
run:

```sh
./scripts/check.sh
```

Please keep changes focused and describe any acceptance condition that remains
unproved.
