# API and processing semantics

## Engine, source, and jobs

`Engine::open` creates an immutable source with capture metadata and either normalized sensor samples
or decoded RGB. `Engine::from_image` accepts finite, interleaved **linear Rec.2020 D65** float RGB.
Camera profiles apply to native Bayer sources; input RGB files use their embedded output ICC profile.

`render` returns an owned float buffer and diagnostics. Histogram bins represent clipped-to-0..1
linear working RGB; clipping counters count pixels with any channel <=0 or >=1. They are diagnostics
for a nominal display range, not proof that source sensor data is clipped. Region coordinates refer
to the edited image after crop/geometry, before preview reduction. Previews are area-resampled from
the full processed image, so they agree with downsampled final rendering.

A fixed worker pool per engine bounds CPU workers across simultaneous jobs. Configure 1..64 threads
(0 selects up to eight), an aggregate memory budget, and a per-source cache budget. Sources reserve
storage plus their cache allowance. In-flight jobs reserve conservative working-buffer estimates.
Returned engine buffers retain reservations. An allocation rejected by the budget raises
`memory_limit`; do not assume all camera dimensions fit small budgets. External caller-made image
copies and direct modifications to public vectors are the caller's responsibility. Default memory
budget is 8 GiB, cache allowance 128 MiB. This is an admission budget, not an OS RSS hard limit:
decoder/library internal overhead and allocator bookkeeping vary.

Sensor/develop/profile/detail/global/local/retouch intermediates use bounded immutable caches when they fit.
Changing parameters changes that stage's key and downstream keys. Oversized recipe keys bypass
caching. Geometry/effects are evaluated at render time. Demosaicing is repeated when its sensor-stage entry
cannot fit or has been evicted; larger caches allow sensor and developed stages to coexist.

Jobs are synchronous. Progress callbacks run on the calling job thread and may request cancellation;
callbacks must not throw across the C interface. Worker loops observe cancellation. Do not destroy
handles while another thread is using them. Reuse a source across different jobs and recipes safely.
Cancellation is sticky on a Job; create a new Job for a new operation.

## Recipes and controls

Recipe schema/process versions are both 1. Omitted properties use defaults; unknown keys, wrong
shapes, nonfinite/out-of-range values, unsupported versions, and excessive nesting are errors.
Coordinates for masks, strokes, crop, guides, WB samples, and documented bad pixels are normalized.
Retouch radius uses the shorter image dimension. Brush mask coordinates/radii use normalized axes;
radial masks expose separate x/y radii. Local edits run sequentially. A local edit's masks combine
left to right using union/intersection/subtraction, then opacity blends adjusted pixels.

Exposure is in stops (-20..20); tonal/color/local controls use -100..100. Noise/sharpening/defringe
amounts use 0..100. Grade hue is degrees, grade saturation 0..100. Temperature is 2000..50000 K;
tint is -150..150. Temperature/tint behavior is an approximation based on camera daylight gains,
not Adobe slider semantics. The effective illuminant for DCP interpolation is estimated from the
chosen gains; inactive temperature settings must not affect as-shot/automatic/neutral results.

Processing order:

1. Decode/normalize, optional documented defect correction, demosaic, WB/highlight reconstruction,
   and camera-to-linear-Rec.2020 conversion.
2. Camera ICC/DCP profile; classical noise/detail processing.
3. Texture/clarity/dehaze, exposure/WB shifts, tonal controls, curves, HSL/saturation, calibration,
   grading, and monochrome.
4. Local masked edits and clone/heal/red-eye.
5. Lens/manual geometry and crop, grain/post-crop vignette.
6. Region extraction and preview scaling; optional export scaling/output sharpening and ICC transform.

Negative/above-white values remain representable in float processing. Integer export clips to its
output gamut/range. Float TIFF uses a built-in linear output profile and preserves finite HDR values.
Some creative operations are intentionally nonlinear; highlight reconstruction cannot invent
measured detail when every sensor channel has clipped.

## C ABI

Pass ABI version 1 in optional configuration/options structs. A null configuration selects defaults;
a supplied configuration uses zero threads/memory as defaults and zero cache as disabled. A supplied
export struct must set quality and bits explicitly (e.g. 92 and 16). Null recipes use neutral edits.
All fallible functions return status codes and initialize output handles to null on failure.
`re_last_error()` is thread-local and remains valid until the next fallible call on that thread.

Release with matching functions: engine/source/job/image destroy; strings with `re_string_destroy`.
The caller must not free pixels or diagnostic text separately. `re_image_destroy` frees them together.
The C++ engine keeps no borrowed caller pixel buffers. No C++ exception crosses the C ABI.

## Export and metadata

JPEG is 8-bit, 4:4:4. PNG is 8/16-bit; TIFF is 8/16-bit integer or 32-bit float. Built-in output spaces
are sRGB, Display P3, and Rec.2020; custom RGB output ICC is accepted for integer formats.
Capture make/model, ISO, exposure, aperture, focal length, lens and capture date are preserved where
known and supported. Orientation is normalized to top-left. Maker notes and GPS are not copied.

Exports cannot overwrite their original source path. Writers reserve an exclusive temporary filename
in the destination directory, close the completed file, check cancellation, then atomically rename
it into place. Existing destinations are replaced on supported POSIX filesystems. This provides
atomic visibility, not crash-durable fsync semantics. No cross-filesystem temporary directory is used.
