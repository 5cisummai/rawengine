#include "internal.hpp"
#include "rawengine/c_api.h"
#include "synthetic.hpp"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <future>
#include <iostream>
using namespace rawengine;
static int checks = 0;
static void require(bool ok, const char *message) {
    ++checks;
    if (!ok)
        throw std::runtime_error(message);
}
static Image fixture(uint32_t w = 48, uint32_t h = 40) {
    Image i(w, h);
    for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < w; ++x) {
            auto *p = i.at(x, y);
            p[0] = .05f + .65f * x / w;
            p[1] = .1f + .45f * y / h;
            p[2] = .15f + ((x + y) % 7) * .025f;
        }
    return i;
}
static float difference(const Image &a, const Image &b) {
    require(a.pixels.size() == b.pixels.size(), "same size");
    float result = 0;
    for (size_t n = 0; n < a.pixels.size(); ++n)
        result = std::max(result, std::abs(a.pixels[n] - b.pixels[n]));
    return result;
}
static void invalid(const std::string &s) {
    try {
        Recipe::from_json(s);
    } catch (const Error &) {
        return;
    }
    throw std::runtime_error("invalid recipe accepted");
}
int main() {
    try {
        Engine engine(Config{256 * 1024 * 1024, 16 * 1024 * 1024, 4});
        auto input = fixture();
        Metadata capture;
        capture.make = "Test";
        capture.model = "Synthetic";
        capture.iso = 400;
        capture.shutter = .01;
        capture.aperture = 2.8;
        capture.focal_length = 35;
        capture.lens = "Synthetic lens";
        capture.timestamp = 1700000000;
        auto source = engine.from_image(input, capture);
        auto neutral = engine.render(*source);
        require(difference(neutral.image, input) < 1e-6, "neutral identity");
        Recipe exposure;
        exposure.adjustments.exposure = 1;
        auto twice = engine.render(*source, exposure);
        for (size_t n = 0; n < input.pixels.size(); ++n)
            require(std::abs(twice.image.pixels[n] - 2 * input.pixels[n]) < 1e-5, "exposure scaling");
        auto control_input = input;
        for (auto &v : control_input.pixels)
            v *= 3;
        auto control_source = engine.from_image(control_input);
        for (auto member :
             {&Adjustments::contrast, &Adjustments::highlights, &Adjustments::shadows, &Adjustments::whites,
              &Adjustments::blacks, &Adjustments::saturation, &Adjustments::vibrance, &Adjustments::texture,
              &Adjustments::clarity, &Adjustments::dehaze, &Adjustments::temperature_shift,
              &Adjustments::tint_shift}) {
            Recipe r;
            r.adjustments.*member = 20;
            require(difference(engine.render(*control_source, r).image, control_input) > 1e-6,
                    "global control observable");
            require(Recipe::from_json(r.json()).json() == r.json(), "control serialized");
        }
        for (auto member :
             {&Detail::luminance_noise, &Detail::chroma_noise, &Detail::sharpen, &Detail::defringe}) {
            Recipe r;
            r.detail.*member = 30;
            require(difference(engine.render(*source, r).image, input) > 1e-6, "detail control observable");
        }
        for (auto member :
             {&Geometry::rotation, &Geometry::vertical, &Geometry::horizontal, &Geometry::distortion,
              &Geometry::chromatic_aberration, &Geometry::vignette_correction}) {
            Recipe r;
            r.geometry.*member = 10;
            require(difference(engine.render(*source, r).image, input) > 1e-6, "geometry control observable");
        }
        for (auto kind : {"brush", "linear", "radial", "luminance", "color"}) {
            Recipe r;
            LocalEdit e;
            Mask mask;
            mask.kind = kind;
            mask.radius = .6f;
            mask.color = {.2f, .2f, .2f};
            e.masks = {mask};
            e.adjustments.exposure = 1;
            r.local_edits = {e};
            require(difference(engine.render(*source, r).image, input) > 1e-6, "mask control observable");
        }
        for (auto kind : {"clone", "heal", "red_eye"}) {
            Recipe r;
            Retouch edit;
            edit.kind = kind;
            edit.target = {.7f, .3f};
            edit.source = {.2f, .8f};
            edit.radius = .2f;
            r.retouch = {edit};
            require(difference(engine.render(*source, r).image, input) > 1e-6, "retouch control observable");
        }
        Recipe cropped;
        cropped.geometry.crop_width = .5f;
        cropped.geometry.crop_height = .5f;
        auto crop = engine.render(*source, cropped);
        require(crop.image.width == 24 && crop.image.height == 20, "crop dimensions");
        Recipe color;
        color.adjustments.hsl[0].hue = 40;
        color.adjustments.monochrome = true;
        auto gray = engine.render(*source, color);
        require(gray.image.pixels[0] == gray.image.pixels[1] && gray.image.pixels[1] == gray.image.pixels[2],
                "monochrome channels");
        Image flat(4, 4);
        for (size_t n = 0; n < flat.pixels.size(); n += 3) {
            flat.pixels[n] = .2f;
            flat.pixels[n + 1] = .4f;
            flat.pixels[n + 2] = .1f;
        }
        Metadata meta;
        meta.as_shot = {2, 1, 4, 1};
        meta.daylight = meta.as_shot;
        meta.camera_to_srgb = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
        Recipe wb;
        wb.white_balance.mode = "neutral";
        auto white = develop(flat, meta, false, wb, 1, nullptr);
        require(std::abs(white.pixels[0] - white.pixels[1]) < 1e-6 &&
                    std::abs(white.pixels[1] - white.pixels[2]) < 1e-6,
                "neutral WB");
        wb.white_balance.mode = "auto";
        require(difference(white, develop(flat, meta, false, wb, 1, nullptr)) < 1e-6, "auto WB");
        wb.white_balance.mode = "temperature";
        wb.white_balance.temperature = 3000;
        auto cool = develop(flat, meta, false, wb, 1, nullptr);
        require(cool.pixels[2] > .1f, "temperature WB direction");
        // Controlled detail/noise and clipping cases complement observational control tests.
        Image noise(32, 32);
        for (size_t n = 0; n < noise.pixels.size(); n += 3) {
            float v = .25f + ((n / 3) % 2 ? .015f : -.015f);
            noise.pixels[n] = v;
            noise.pixels[n + 1] = v;
            noise.pixels[n + 2] = v;
        }
        Image denoised = noise;
        Detail denoise;
        denoise.luminance_noise = 80;
        apply_detail(denoised, denoise, 1, nullptr);
        double before_error = 0, after_error = 0;
        for (size_t n = 0; n < noise.pixels.size(); ++n) {
            before_error += std::pow(noise.pixels[n] - .25f, 2);
            after_error += std::pow(denoised.pixels[n] - .25f, 2);
        }
        require(after_error < before_error * .5, "denoise reduces known error");
        Image clipped(2, 2);
        for (size_t n = 0; n < clipped.pixels.size(); n += 3) {
            clipped.pixels[n] = 1;
            clipped.pixels[n + 1] = .8f;
            clipped.pixels[n + 2] = .6f;
        }
        Metadata hm;
        hm.as_shot = {1, 2, 1, 1};
        hm.camera_to_srgb = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
        Recipe recover;
        auto recovered_highlight = develop(clipped, hm, true, recover, 1, nullptr);
        recover.highlight_recovery = false;
        auto clipped_highlight = develop(clipped, hm, true, recover, 1, nullptr);
        require(difference(recovered_highlight, clipped_highlight) > 1e-4,
                "partial clipped highlight reconstructed");
        std::fill(clipped.pixels.begin(), clipped.pixels.end(), 1);
        recover.highlight_recovery = true;
        auto all_clipped = develop(clipped, hm, true, recover, 1, nullptr);
        recover.highlight_recovery = false;
        require(difference(all_clipped, develop(clipped, hm, true, recover, 1, nullptr)) == 0,
                "all-channel clipping conservative");
        Recipe all = Recipe::photographic();
        all.adjustments.hsl[0] = {10, 20, -10};
        all.adjustments.grading[1] = {120, 20, 10};
        all.adjustments.calibration_hue[1] = 10;
        all.grain = 10;
        all.vignette = -10;
        all.detail.luminance_noise = 10;
        all.detail.defringe = 10;
        all.local_edits = {LocalEdit{{Mask{}}, Adjustments{}, Detail{}, .8f}};
        all.local_edits[0].adjustments.exposure = .5f;
        all.retouch = {Retouch{}};
        all.retouch[0].target = {.3f, .3f};
        all.retouch[0].source = {.7f, .7f};
        require(Recipe::from_json(all.json()).json() == all.json(), "recipe roundtrip");
        auto rendered = engine.render(*source, all);
        require(difference(rendered.image, input) > .01f, "edits affect image");
        require(difference(rendered.image, engine.render(*source, all).image) == 0, "deterministic render");
        RenderOptions one;
        one.tile_rows = 1;
        RenderOptions many;
        many.tile_rows = 100;
        require(difference(engine.render(*source, all, one).image, engine.render(*source, all, many).image) ==
                    0,
                "tile independence");
        RenderOptions preview;
        preview.max_dimension = 24;
        auto reduced = engine.render(*source, all, preview);
        require(difference(reduced.image, resize(rendered.image, 24, 20)) < 1e-6, "preview reference");
        RenderOptions region;
        region.x = 4;
        region.y = 3;
        region.width = 8;
        region.height = 9;
        auto roi = engine.render(*source, all, region);
        require(roi.image.at(2, 3)[0] == rendered.image.at(6, 6)[0], "ROI exact");
        std::vector<float> mosaic(64, .2f);
        std::vector<uint8_t> cfa(64);
        for (int y = 0; y < 8; ++y)
            for (int x = 0; x < 8; ++x)
                cfa[y * 8 + x] = uint8_t((y % 2) ? ((x % 2) ? 2 : 1) : ((x % 2) ? 1 : 0));
        auto raw = demosaic(mosaic, cfa, 8, 8, true);
        for (float v : raw.pixels)
            require(std::abs(v - .2f) < 1e-6, "flat mosaic");
        require(difference(raw, demosaic(mosaic, cfa, 8, 8, false)) < 1e-6, "reference demosaic");
        invalid("{\"version\":2}");
        invalid("{\"extra\":1}");
        invalid("{\"adjustments\":{\"exposure\":999}}");
        invalid("{\"geometry\":{\"crop_x\":0.8,\"crop_width\":0.5}}");
        invalid("{\"local_edits\":[{\"masks\":[{\"kind\":\"magic\"}]}]}");
        invalid("{\"detail\":{\"radius\":0}}");
        invalid("[]");
        Job cancelled;
        cancelled.cancelled = true;
        try {
            engine.render(*source, {}, {}, &cancelled);
            throw std::runtime_error("cancellation ignored");
        } catch (const Error &e) {
            require(e.code == ErrorCode::cancelled, "cancel code");
        }
        auto parallel1 =
            std::async(std::launch::async, [&] { return engine.render(*source, exposure).image; });
        auto parallel2 = std::async(std::launch::async, [&] { return engine.render(*source).image; });
        require(difference(parallel1.get(), twice.image) == 0, "concurrent exposure");
        require(difference(parallel2.get(), input) < 1e-6, "concurrent neutral");
        auto temp = std::filesystem::temp_directory_path() / "rawengine-tests";
        std::filesystem::create_directories(temp);
        auto dngfile = temp / "synthetic.dng";
        synthetic::dng(dngfile);
        auto synthetic_source = engine.open(dngfile);
        require(synthetic_source->metadata().sensor == "Bayer", "DNG native adapter");
        require(std::abs(synthetic_source->metadata().black + synthetic_source->metadata().channel_black[0] +
                         synthetic_source->metadata().black_pattern[0] - 100) < 1,
                "DNG black metadata");
        auto normalized = engine.render(*synthetic_source);
        for (size_t n = 3; n < normalized.image.pixels.size(); ++n)
            require(std::abs(normalized.image.pixels[n] - normalized.image.pixels[n % 3]) < 1e-5,
                    "DNG flat normalization");
        auto broken = temp / "defect.dng";
        synthetic::dng(broken, true);
        auto damaged = engine.open(broken);
        Recipe repair;
        repair.bad_pixels = {{16.f / 31, 16.f / 31}};
        auto repaired = engine.render(*damaged, repair);
        require(difference(repaired.image, normalized.image) < 1e-5, "documented bad pixel corrected");
        auto profile = temp / "synthetic.dcp";
        synthetic::dcp(profile, true);
        Recipe profiled;
        profiled.camera_profile = profile.string();
        require(difference(engine.render(*synthetic_source, profiled).image, normalized.image) > 1e-4,
                "DCP profile observable");
        auto analytical = engine.render(*synthetic_source, profiled);
        require(std::abs(analytical.image.pixels[0] - .3f) < .001f,
                "DCP matrix/neutral/tone analytical result");
        synthetic::dcp(profile, false, false, true);
        auto colorfile = temp / "color.dng";
        synthetic::dng(colorfile, false, true);
        auto rawcolor = engine.open(colorfile);
        auto asshot = engine.render(*rawcolor, profiled);
        Recipe inactive = profiled;
        inactive.white_balance.temperature = 3000;
        require(difference(asshot.image, engine.render(*rawcolor, inactive).image) < 1e-6,
                "inactive WB temperature cannot alter DCP");
        Recipe tungsten = profiled;
        tungsten.white_balance.mode = "temperature";
        tungsten.white_balance.temperature = 2856;
        require(difference(asshot.image, engine.render(*rawcolor, tungsten).image) > .01f,
                "explicit tungsten DCP/WB observable");
        synthetic::dcp(profile, false, true);
        try {
            engine.render(*synthetic_source, profiled);
            throw std::runtime_error("unsupported DCP ignored");
        } catch (const Error &e) {
            require(e.code == ErrorCode::unsupported, "DCP unsupported error");
        }
        {
            std::ofstream bad(profile);
            bad << "bad";
        }
        try {
            engine.render(*synthetic_source, profiled);
            throw std::runtime_error("bad DCP accepted");
        } catch (const Error &e) {
            require(e.code == ErrorCode::profile, "DCP malformed error");
        }
        auto lensdir = temp / "lensdb";
        std::filesystem::create_directories(lensdir);
        {
            std::ofstream xml(lensdir / "synthetic.xml");
            xml << R"(<lensdatabase version="1"><mount><name>Test mount</name></mount><camera><maker>Test</maker><model>Synthetic</model><mount>Test mount</mount><cropfactor>1</cropfactor></camera><lens><maker>Test</maker><model>Synthetic lens</model><mount>Test mount</mount><cropfactor>1</cropfactor><calibration><distortion model="poly3" focal="35" k1="0.1"/><tca model="linear" focal="35" kr="1.001" kb="0.999"/><vignetting model="pa" focal="35" aperture="2.8" distance="1000" k1="-0.1" k2="0" k3="0"/></calibration></lens></lensdatabase>)";
        }
        Recipe lens;
        lens.geometry.lens_profile = true;
        lens.geometry.lens_database = lensdir.string();
        auto lenscorrected = engine.render(*source, lens);
        require(difference(lenscorrected.image, input) > 1e-5, "lens calibration observable");
        lens.geometry.lens_name = "Missing lens";
        auto missing_lens = engine.render(*source, lens);
        require(!missing_lens.diagnostics.warnings.empty(), "missing lens warned");
        for (auto format : {"png", "tiff", "jpeg"}) {
            ExportOptions o;
            o.format = format;
            o.bits = 16;
            o.color_space = "rec2020";
            o.quality = 100;
            auto file = temp / (std::string("output.") + format);
            engine.export_image(*source, {}, file, o);
            auto reread = engine.open(file);
            require(reread->metadata().width == input.width, "export dimensions");
            require(reread->metadata().make == capture.make && reread->metadata().iso == 400 &&
                        std::abs(reread->metadata().shutter - .01) < 1e-5,
                    "capture metadata roundtrip");
            float error = difference(engine.render(*reread).image, input);
            std::cerr << format << " roundtrip max error " << error << "\n";
            require(error < (std::string(format) == "jpeg" ? .025f : .001f), "export color roundtrip");
        }
        ExportOptions floating;
        floating.floating_point = true;
        floating.color_space = "rec2020";
        Image hdr = fixture();
        hdr.at(0, 0)[0] = 2;
        hdr.at(0, 0)[2] = -.1f;
        auto hdrsource = engine.from_image(hdr);
        auto tiff = temp / "float.tiff";
        engine.export_image(*hdrsource, {}, tiff, floating);
        require(difference(engine.render(*engine.open(tiff)).image, hdr) < 1e-5, "float HDR roundtrip");
        auto file = temp / "cancelled.png";
        {
            std::ofstream stream(file);
            stream << "existing";
        }
        Job cancel_export;
        cancel_export.progress = [&](float, const std::string &stage) {
            if (stage == "export")
                cancel_export.cancelled = true;
        };
        try {
            engine.export_image(*source, {}, file, {}, &cancel_export);
            throw std::runtime_error("export cancel ignored");
        } catch (const Error &e) {
            require(e.code == ErrorCode::cancelled, "export cancelled");
        }
        std::ifstream old(file);
        std::string text;
        old >> text;
        require(text == "existing", "atomic cancellation");
        for (auto &f : std::filesystem::directory_iterator(temp))
            require(f.path().filename().string().find(".tmp-") == std::string::npos, "temp cleanup");
        Engine constrained(Config{4 * 1024 * 1024, 0, 1});
        auto small = constrained.from_image(fixture(128, 128));
        std::vector<RenderResult> retained;
        bool reached_limit = false;
        for (int k = 0; k < 32; ++k) {
            try {
                retained.push_back(constrained.render(*small));
            } catch (const Error &e) {
                require(e.code == ErrorCode::memory_limit, "retained buffer memory code");
                reached_limit = true;
                break;
            }
        }
        require(reached_limit, "retained output bounded");
        retained.clear();
        auto recovered = constrained.render(*small);
        require(recovered.image.width == 128, "released buffer returns budget");
        invalid("{\"version\":1.5}");
        invalid("{\"adjustments\":{\"mono_mix\":[1,2,3,4]}}");
        std::cout << "Passed " << checks << " checks\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
}
