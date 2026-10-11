# Independent implementation review

Two read-only reviews examined the initially empty repository's new engine against the supplied
plan and correctness/resource-safety standards. They identified concrete issues, which were fixed:

- Persistent render/C ABI outputs and export/import working allocations now retain or reserve memory
  ledger tickets. A constrained-budget retained-buffer test verifies rejection and budget recovery.
- Default lens database lookup checks the configured install-prefix database, with a checkout fallback.
  Native bundle clients can override the database location explicitly.
- JPEG marker buffers are constructed outside libjpeg setjmp regions; no marker-write jump skips their
  destructors.
- DCP dual-illuminant interpolation uses the effective selected WB illuminant, rather than an inactive
  recipe temperature. Tests cover inactive-field invariance and explicit tungsten behavior.
- DCP analytical neutral/tone, controlled noise/clipping, and synthetic Lensfun calibration tests were
  added. Missing measured photographic references remain clearly recorded in VALIDATION.md.

No unsupported features are implemented as success-returning stubs. Accuracy/adapter limitations are
listed in SUPPORT.md. Reviews do not establish Adobe equivalence or production readiness.
