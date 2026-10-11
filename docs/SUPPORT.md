# Feature/support matrix

| Area | Implemented | Limits |
|---|---|---|
| RAW decoding | LibRaw-supported camera formats; native Bayer/monochrome, X-Trans/linear RGB adapters | Actual tested cameras listed in VALIDATION.md; availability depends on decoder build |
| Sensor normalization | Active crop, orientation, global/channel/repeating black levels, white level, documented defect points | DNG opcode gain maps, delta-H/V black fields and specialty sensor calibration are not independently implemented; decoder limitations can remain |
| Demosaicing | Original float edge-directed green/color-difference Bayer; bilinear-style reference; LibRaw X-Trans | No claim of AMaZE/RCD or Adobe detail parity; alias/false-color tuning remains |
| RAW precision | Native Bayer float after sensor unpack | X-Trans/specialty adapter uses 16-bit linear sRGB after as-shot WB, limiting headroom and later highlight recovery |
| White balance | As-shot, gray-world auto, neutral patch, temperature/tint | Temperature and inferred illuminant are approximate; no spectrally measured camera temperature model |
| Highlights | Conservative partial-channel reconstruction | Cannot recover all-channel-clipped detail; no spatial highlight inpainting |
| Calibration | Embedded/LibRaw camera matrices; explicit or embedded RGB ICC | ICC custom camera transforms currently native Bayer only; unusual embedded ICC color spaces fail explicitly |
| DCP subset | ColorMatrix1/2, supported calibration illuminants, linear hue/saturation tables, tone curves | ForwardMatrix, LookTable, encoded tables, baseline exposure/advanced profile tags fail explicitly; not full Adobe profile compatibility |
| Global editing | Exposure/contrast, highlights/shadows/whites/blacks, point/parametric curves, vibrance/saturation, 8-band HSL, mono mix, grading, calibration | Engine-defined semantics in linear working RGB; tuning and advanced gamut mapping remain |
| Local contrast | Multiscale texture/clarity, conservative dehaze, grain/vignette | Box-filter scales rather than a calibrated perceptual model; dehaze is a simple estimate |
| Detail | Bilateral luminance/chroma reduction, masked unsharp detail/output sharpening, defringe | Classical CPU algorithms; slow at high resolutions/large noise amounts; defringe heuristic can desaturate real purple/green |
| Lens correction | Lensfun distortion/TCA/vignetting, manual alternatives | Bundled database 0.3.4; requires identifiable lens/focal metadata; missing calibration warns; DNG lens opcodes not independently applied |
| Geometry | Crop, rotation, vertical/horizontal homography, guided vertical roll/convergence | Manual/guided controls; no automatic scene/line detection; unsupported borders become black |
| Masks | Brush, linear/radial gradient, luminance/color range, feather/invert/union/intersect/subtract | Manual masks only; no semantic segmentation |
| Retouch | Clone, low-frequency-matched heal, red-eye | Not content-aware or generative; sources use pre-stroke pixels; edge samples clamp |
| Rendering | Full image, exact final-image regions, matching area-downsampled previews, histograms/clipping | Full-frame neighborhood processing; tile_rows controls point-operation scheduling, not out-of-core tiles |
| Input RGB | JPEG/PNG and contiguous stripped RGB TIFF 8/16 or 32f, embedded ICC | No CMYK/tiled/planar TIFF; unprofiled images assume sRGB (float TIFF linear sRGB); PNG custom gAMA/cHRM alone not interpreted |
| Export | JPEG, PNG8/16, TIFF8/16/32f; ICC, metadata policy, size/output sharpening, atomic publish | Integer output clips out-of-gamut/HDR values; no DNG writing; custom ICC float export rejected |
| Integration | C++20, C ABI v1, compiled Swift example, CMake installation and CI definitions | No GUI/catalog; Linux workflow not yet executed remotely; bundle/database packaging is caller-controlled |

AI denoise/super-resolution/masks/depth/generative removal, HDR/panorama merge, Metal, and Adobe XMP/
preset translation are intentionally deferred. Existing RAW sensor CFA flavors outside native Bayer
are delegated to LibRaw or rejected. This matrix distinguishes working implementations from remaining
quality work; real-world photographic accuracy is not established by synthetic tests alone.
