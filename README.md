# RawEngine

A standalone C++20 RAW development library, CLI, and versioned C ABI for native applications.
It uses LibRaw for camera files, original CPU processing code, Little CMS for color management,
and Lensfun for lens corrections. It renders in 32-bit linear Rec.2020 and saves nondestructive JSON
recipes. This is an initial classical engine, not an Adobe Camera Raw replacement with proven parity.

The initial implementation was created with **OpenAI Codex**. See the [validation evidence](docs/VALIDATION.md)
and [feature limitations](docs/SUPPORT.md) for its current capabilities.

## Build

Run the following commands from this repository’s root directory.

On macOS, install the development libraries if they are missing:

```sh
brew install cmake pkg-config libraw little-cms2 libpng libtiff jpeg-turbo glib
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release
cmake --build build-release --parallel 6
ctest --test-dir build-release --output-on-failure
```

On Ubuntu 24.04:

```sh
sudo apt-get install cmake pkg-config libraw-dev liblcms2-dev libpng-dev libtiff-dev libjpeg-turbo8-dev libglib2.0-dev
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DRAWENGINE_STRICT_DEPS=OFF
cmake --build build --parallel 4
ctest --test-dir build --output-on-failure
```

JSON 3.12.0 and Lensfun 0.3.4 sources are vendored. Other dependencies come from pkg-config.
`dependencies.lock.json` records the tested versions. The default requires
that exact development lock; use `-DRAWENGINE_STRICT_DEPS=OFF` for compatible distribution versions.
The GitHub workflow tests distribution dependencies on Linux and macOS; remote CI has not yet run.

For sanitizers:

```sh
cmake -S . -B build-sanitize -DCMAKE_BUILD_TYPE=Debug -DRAWENGINE_SANITIZE=ON
cmake --build build-sanitize --parallel 6
ctest --test-dir build-sanitize --output-on-failure
```

## CLI

```sh
build-release/rawengine inspect /path/to/photo.CR3
build-release/rawengine recipe photographic > /tmp/edit.json
build-release/rawengine render /path/to/photo.CR3 /tmp/preview.png --recipe /tmp/edit.json --max-dimension 1600
build-release/rawengine export /path/to/photo.CR3 /tmp/final.tiff --recipe /tmp/edit.json --space display_p3 --bits 16
build-release/rawengine export /path/to/photo.CR3 /tmp/linear.tiff --space rec2020 --bits 32
build-release/rawengine benchmark /path/to/photo.CR3 --iterations 3
build-release/rawengine capabilities
```

`render` and `export` both write real image files; `--max-dimension` requests a reduced final preview.
Use `--metadata strip` to omit capture metadata. `--width`, `--height`, `--quality`, `--threads`,
`--memory-mb`, and `--progress` control output and resources. Recipes are explicit: the `preset` field
labels a recipe; obtain actual preset values through the `recipe` command or `Recipe::photographic()`.
The default is neutral, including an identity tone curve and no automatic brightness scaling.

## Integrate

```cpp
#include <rawengine/engine.hpp>
rawengine::Engine engine;
auto source = engine.open("photo.NEF");
auto edits = rawengine::Recipe::photographic();
edits.adjustments.exposure = 0.5f;
auto preview = engine.render(*source, edits, {.max_dimension = 1200});
engine.export_image(*source, edits, "photo.png");
```

The C ABI is in `include/rawengine/c_api.h`; the compiled shared library is
`build-release/librawengine.dylib` on macOS. See `examples/swift` for a tested Swift client,
`docs/API.md` for ownership and rendering semantics, and `docs/SUPPORT.md` for feature limits.
Run synchronous processing on a background thread in a native app.

To install, configure with the desired `CMAKE_INSTALL_PREFIX`, then `cmake --install build-release`.
Ship both engine libraries, their linked dependencies, notices, and the Lensfun database. For a macOS
app bundle set the recipe's `geometry.lens_database` to your bundled database directory.

## Validation assets

`tests/synthetic.hpp` creates original analytical DNG/DCP fixtures during tests. Real RAW photographs
are outside Git; `tests/fixtures.json` contains CC0 sources and SHA-256 checksums:

```sh
python3 tools/fetch_fixtures.py /tmp/rawengine-fixtures --ids 2010 3098
python3 tools/check_fixtures.py build-release/rawengine /tmp/rawengine-fixtures /tmp/rawengine-qa
```

The fixture checker records metadata, full-resolution benchmarks, and preview comparisons.
See `docs/VALIDATION.md` for evidence and remaining photographic validation.
