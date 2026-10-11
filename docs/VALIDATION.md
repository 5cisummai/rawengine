# Validation evidence

Verified locally on Apple Silicon with AppleClang 21, C++20, dependency versions in the lock file.

## Automated checks

- Release CTest: native C++ numerical/integration suite and a separately compiled C ABI client pass.
- AddressSanitizer + UndefinedBehaviorSanitizer: the same suites pass; vendored Lensfun is instrumented.
- Swift client compiles and runs against the shared library on a synthetic DNG, renders float preview
  pixels, exports a PNG, and releases owned handles.
- Installed CLI loads its installed engine/Lensfun shared libraries and reports capabilities.
- Exact development dependency lock configuration succeeds.

Tests cover sensor black/white normalization, native DNG decoding, flat CFA/reference demosaic,
documented bad pixels, exposure scaling, WB modes, partial/all-channel clipping, every classical
editing family, synthetic Lensfun calibration, DCP analytical neutral/tone behavior, explicit
illuminant interpolation/inactive-field invariance, recipe serialization/validation, deterministic
output, ROI/preview/tile equivalence, concurrency, retained-output memory limits, cancellation,
atomic preservation of existing destinations, capture metadata, ICC round trips and HDR float TIFF.
PNG/TIFF integer round-trip maximum error on an in-gamut Rec.2020 fixture: about 1.5e-5; JPEG is lossy
and tested with an explicit tolerance at quality 100. These are controlled numerical checks, not
measured accuracy against a commercial RAW converter.

`tools/color_error.py REFERENCE.csv RENDERED.csv` reports Delta E 76 and CIEDE2000 for matching
`patch,L,a,b` rows. Both files must use the same CIELab illuminant/observer. Reference-pair tests
check the CIEDE2000 implementation against published Sharma/Wu/Dalal examples. No physical camera
chart reference is currently available, so no camera Delta E score is claimed.

## Real camera fixtures

CC0 photographs were fetched from raw.pixls.us, and SHA-256 matched the manifest. Each completed
metadata inspection, full-resolution benchmark rendering, neutral preview and explicit tonal/color
edit preview. Generated evidence lives in `out/qa/report.json` and `out/qa/index.html` (ignored by Git).

| Camera | Adapter | Render dimensions | Neutral render observations |
|---|---|---|---|
| Canon EOS D60 | Native Bayer | 3088 × 2056 | Outdoor foliage/sky/buildings; readable detail and ordinary color |
| Nikon D200 | Native Bayer | 3899 × 2616 | Actual NEF decode/render completed |
| Sony ILCE-7M3 | Native Bayer | 6024 × 4024 | Outdoor fine branches, muted colors, bright sky |
| Fujifilm X-E1 | LibRaw X-Trans adapter | 4952 × 3288 | Indoor scene; adapter headroom warning surfaced |

Initial eight-worker Release render timings, excluding RAW open/unpack: roughly 0.6–0.8 s Canon,
0.9–1.0 s Nikon, 3.0–3.5 s Sony, 1.2–1.5 s Fuji on this machine. Timings vary with contention/cache;
the exact runs and metadata are in the JSON report. These are neutral full-frame benchmarks, not
latency guarantees for bilateral denoise or every editing combination.

## Remaining photographic validation

Measured ColorChecker/spectral references, controlled high-ISO camera scenes, spatial clipped-light
reconstruction targets, and measured physical lens targets remain unavailable. Synthetic tests cover
known noise reduction, clipping behavior and lens-model effects, but do not replace those photographs.
The fixture manifest records these gaps. X-Trans/ICC/DCP color and fine-detail behavior need additional
camera-specific evaluation; the initial algorithms should be tuned before claiming production parity.

The Linux/macOS CI workflows are included as source definitions but have not run remotely here.
