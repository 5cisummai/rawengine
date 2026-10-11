# Third-party components

The engine processing code is original. No RawTherapee/darktable/GPL application processing pipeline
is copied. Dependency licenses continue to apply when distributing binaries or embedding in an app.

| Component | Tested version | Upstream / license |
|---|---|---|
| LibRaw | 0.22.2 | https://www.libraw.org/ – dual LGPL-2.1 / CDDL-1.0; choose applicable distribution mode and retain notices |
| Little CMS | 2.19 | https://www.littlecms.com/ – MIT |
| Lensfun library | 0.3.4 (vendored unmodified) | https://github.com/lensfun/lensfun – LGPL-3.0; built as a separate shared library |
| Lensfun database | bundled 0.3.4 | CC-BY-SA-3.0, preserve database attribution/license independently of engine code |
| libjpeg-turbo | 3.2.0 | https://libjpeg-turbo.org/ – IJG/BSD-3-Clause/zlib notices depending on components |
| libpng | 1.6.59 | http://www.libpng.org/ – libpng license |
| libtiff | 4.7.2 | https://libtiff.gitlab.io/libtiff/ – libtiff permissive license |
| nlohmann/json | 3.12.0 (vendored header) | https://github.com/nlohmann/json – MIT notice included in header |
| GLib (Lensfun dependency) | 2.90.1 | https://gitlab.gnome.org/GNOME/glib – LGPL-2.1-or-later |

Lensfun's included auxiliary tools/tests have their own notices and are not compiled or shipped as
engine tools. License texts are under `third_party/lensfun/docs`, in the JSON header, and in installed
dependency distributions. Do not strip copyright or license notices when packaging. RAW fixtures are
CC0-1.0, with upstream source and checksum recorded per photograph; photographs stay outside Git.
No Adobe profiles, models, code, or trademarks are redistributed.
