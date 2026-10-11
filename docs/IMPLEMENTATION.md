# Implementation contract

The requested first release is a portable C++20 classical RAW engine using LibRaw, Little CMS,
Lensfun, libjpeg-turbo, libpng, libtiff, and nlohmann/json. It exposes immutable sources, versioned
recipes, C++/C APIs, CLI inspect/render/export/benchmark, cancellable bounded workers, previews,
regions, metadata, profiles, classical global/detail/lens/local/retouch edits, and atomic export.
The supplied plan defers AI, HDR/panorama merging, Metal, and Adobe preset/XMP compatibility.
Photographic quality takes priority over matching Adobe rendering. Integration in a closed-source
macOS application must remain possible; avoid GPL application processing code.

Acceptance requires deterministic processing, JSON round trips, observable controls, meaningful
numerical/API/malformed-input tests, sanitizer runs, actual Bayer and X-Trans fixture checks, and
honest documentation of quality limits. Camera-specific color accuracy requires measured references.

See SUPPORT.md for the actual supported subset and limitations; these are not claims of Adobe parity.
