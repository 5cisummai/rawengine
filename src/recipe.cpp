#include "internal.hpp"
#include <algorithm>
#include <cmath>
namespace rawengine {
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(Point, x, y)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(CurvePoint, x, y)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(HSLBand, hue, saturation, luminance)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(Grade, hue, saturation, luminance)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(Adjustments, exposure, contrast, highlights, shadows, whites,
                                                blacks, saturation, vibrance, texture, clarity, dehaze,
                                                temperature_shift, tint_shift, curve, parametric, hsl,
                                                monochrome, mono_mix, grading, calibration_hue,
                                                calibration_saturation)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(WhiteBalance, mode, temperature, tint, sample, sample_radius)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(Detail, luminance_noise, chroma_noise, sharpen, radius,
                                                threshold, masking, defringe)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(Geometry, crop_x, crop_y, crop_width, crop_height, rotation,
                                                vertical, horizontal, distortion, chromatic_aberration,
                                                vignette_correction, lens_profile, lens_database, lens_name,
                                                guides)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(Mask, kind, operation, start, end, radius, radius_y, feather,
                                                low, high, color, invert, points)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(LocalEdit, masks, adjustments, detail, opacity)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(Retouch, kind, target, source, points, radius, feather,
                                                opacity)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(Recipe, version, process, preset, camera_profile,
                                                white_balance, highlight_recovery, bad_pixels, adjustments,
                                                detail, geometry, local_edits, retouch, grain, grain_size,
                                                vignette, grain_seed)
static void range(float v, float a, float b, const char *name) {
    if (!std::isfinite(v) || v < a || v > b)
        throw Error(ErrorCode::invalid_argument, std::string(name) + " out of range");
}
static void point(const Point &p) {
    range(p.x, 0, 1, "point.x");
    range(p.y, 0, 1, "point.y");
}
static void validate_adjustments(const Adjustments &a) {
    range(a.exposure, -20, 20, "exposure");
    for (float v : {a.contrast, a.highlights, a.shadows, a.whites, a.blacks, a.saturation, a.vibrance,
                    a.texture, a.clarity, a.dehaze, a.temperature_shift, a.tint_shift})
        range(v, -100, 100, "adjustment");
    if (a.curve.size() < 2 || a.curve.size() > 256 || a.curve.front().x != 0 || a.curve.back().x != 1)
        throw Error(ErrorCode::invalid_argument, "curve must span 0..1 with 2..256 points");
    float prev = -1;
    for (auto p : a.curve) {
        range(p.x, 0, 1, "curve.x");
        range(p.y, 0, 1, "curve.y");
        if (p.x <= prev)
            throw Error(ErrorCode::invalid_argument, "curve x must increase");
        prev = p.x;
    }
    for (float v : a.parametric)
        range(v, -100, 100, "parametric");
    for (auto b : a.hsl) {
        range(b.hue, -100, 100, "hsl.hue");
        range(b.saturation, -100, 100, "hsl.saturation");
        range(b.luminance, -100, 100, "hsl.luminance");
    }
    for (float v : a.mono_mix)
        range(v, -2, 2, "mono_mix");
    for (auto g : a.grading) {
        range(g.hue, 0, 360, "grading.hue");
        range(g.saturation, 0, 100, "grading.saturation");
        range(g.luminance, -100, 100, "grading.luminance");
    }
    for (float v : a.calibration_hue)
        range(v, -100, 100, "calibration_hue");
    for (float v : a.calibration_saturation)
        range(v, -100, 100, "calibration_saturation");
}
static void validate_detail(const Detail &d) {
    for (float v : {d.luminance_noise, d.chroma_noise, d.sharpen, d.threshold, d.masking, d.defringe})
        range(v, 0, 100, "detail");
    range(d.radius, 0.2f, 10, "detail.radius");
}
void Recipe::validate() const {
    if (version != 1 || process != process_version)
        throw Error(ErrorCode::unsupported, "unsupported recipe/process version");
    if (preset != "neutral" && preset != "photographic")
        throw Error(ErrorCode::invalid_argument, "unknown preset");
    if (white_balance.mode != "as_shot" && white_balance.mode != "auto" && white_balance.mode != "neutral" &&
        white_balance.mode != "temperature")
        throw Error(ErrorCode::invalid_argument, "unknown white balance mode");
    range(white_balance.temperature, 2000, 50000, "temperature");
    range(white_balance.tint, -150, 150, "tint");
    point(white_balance.sample);
    range(white_balance.sample_radius, 0.001f, 0.25f, "sample_radius");
    if (bad_pixels.size() > 65536)
        throw Error(ErrorCode::invalid_argument, "too many bad pixels");
    for (auto p : bad_pixels)
        point(p);
    validate_adjustments(this->adjustments);
    validate_detail(this->detail);
    const auto &g = geometry;
    range(g.crop_x, 0, 1, "crop_x");
    range(g.crop_y, 0, 1, "crop_y");
    range(g.crop_width, 0.0001f, 1, "crop_width");
    range(g.crop_height, 0.0001f, 1, "crop_height");
    if (g.crop_x + g.crop_width > 1.000001f || g.crop_y + g.crop_height > 1.000001f)
        throw Error(ErrorCode::invalid_argument, "crop exceeds image");
    range(g.rotation, -180, 180, "rotation");
    for (float v : {g.vertical, g.horizontal, g.distortion, g.chromatic_aberration, g.vignette_correction})
        range(v, -100, 100, "geometry");
    if (g.guides.size() % 2 || g.guides.size() > 32)
        throw Error(ErrorCode::invalid_argument, "guides require up to 16 point pairs");
    for (auto p : g.guides)
        point(p);
    if (local_edits.size() > 256 || retouch.size() > 4096)
        throw Error(ErrorCode::invalid_argument, "too many edits");
    for (const auto &e : local_edits) {
        if (e.masks.empty() || e.masks.size() > 256)
            throw Error(ErrorCode::invalid_argument, "local edit needs 1..256 masks");
        range(e.opacity, 0, 1, "opacity");
        validate_adjustments(e.adjustments);
        validate_detail(e.detail);
        for (const auto &m : e.masks) {
            if (m.kind != "brush" && m.kind != "linear" && m.kind != "radial" && m.kind != "luminance" &&
                m.kind != "color")
                throw Error(ErrorCode::invalid_argument, "unknown mask kind");
            if (m.operation != "union" && m.operation != "intersect" && m.operation != "subtract")
                throw Error(ErrorCode::invalid_argument, "unknown mask operation");
            point(m.start);
            point(m.end);
            range(m.radius, 0.0001f, 2, "mask radius");
            range(m.radius_y, 0.0001f, 2, "mask radius_y");
            range(m.feather, 0, 1, "mask feather");
            range(m.low, 0, 1, "mask low");
            range(m.high, 0, 1, "mask high");
            if (m.low > m.high)
                throw Error(ErrorCode::invalid_argument, "mask low exceeds high");
            for (float v : m.color)
                range(v, 0, 16, "mask color");
            if (m.points.size() > 65536)
                throw Error(ErrorCode::invalid_argument, "too many mask points");
            for (auto p : m.points)
                point(p);
            if (m.kind == "linear" && m.start.x == m.end.x && m.start.y == m.end.y)
                throw Error(ErrorCode::invalid_argument, "zero length gradient");
        }
    }
    for (const auto &r : retouch) {
        if (r.kind != "clone" && r.kind != "heal" && r.kind != "red_eye")
            throw Error(ErrorCode::invalid_argument, "unknown retouch kind");
        point(r.target);
        point(r.source);
        range(r.radius, 0.0001f, 1, "retouch radius");
        range(r.feather, 0, 1, "retouch feather");
        range(r.opacity, 0, 1, "retouch opacity");
        if (r.points.size() > 65536)
            throw Error(ErrorCode::invalid_argument, "too many stroke points");
        for (auto p : r.points)
            point(p);
    }
    range(grain, 0, 100, "grain");
    range(grain_size, 0.2f, 10, "grain_size");
    range(vignette, -100, 100, "vignette");
}
static void unknown_keys(const json &value, const json &schema, const std::string &path) {
    bool compatible = schema.is_number_float()     ? value.is_number()
                      : schema.is_number_integer() ? value.is_number_integer()
                                                   : value.type() == schema.type();
    if (!compatible)
        throw Error(ErrorCode::invalid_argument, "wrong recipe type: " + path);
    if (schema.is_number_integer()) {
        if (schema.is_number_unsigned()) {
            if (value.is_number_integer() && !value.is_number_unsigned() && value.get<int64_t>() < 0)
                throw Error(ErrorCode::invalid_argument, "negative unsigned recipe value");
            if (value.get<uint64_t>() > UINT32_MAX)
                throw Error(ErrorCode::invalid_argument, "recipe integer overflow");
        } else if (value.is_number_unsigned()
                       ? value.get<uint64_t>() > INT32_MAX
                       : (value.get<int64_t>() < INT32_MIN || value.get<int64_t>() > INT32_MAX))
            throw Error(ErrorCode::invalid_argument, "recipe integer overflow");
    }
    if (schema.is_array() &&
        (path.ends_with("parametric.") || path.ends_with("hsl.") || path.ends_with("mono_mix.") ||
         path.ends_with("grading.") || path.ends_with("calibration_hue.") ||
         path.ends_with("calibration_saturation.") || path.ends_with("color.")) &&
        value.size() != schema.size())
        throw Error(ErrorCode::invalid_argument, "wrong recipe array length: " + path);

    if (value.is_object() && schema.is_object())
        for (auto it = value.begin(); it != value.end(); ++it) {
            if (!schema.contains(it.key()))
                throw Error(ErrorCode::invalid_argument, "unknown recipe key: " + path + it.key());
            unknown_keys(it.value(), schema.at(it.key()), path + it.key() + ".");
        }
    else if (value.is_array() && schema.is_array() && !schema.empty())
        for (const auto &item : value)
            unknown_keys(item, schema.front(), path);
}
Recipe Recipe::from_json(const std::string &input) {
    try {
        if (input.size() > 16 * 1024 * 1024)
            throw Error(ErrorCode::invalid_argument, "recipe too large");
        auto j =
            rawengine::json::parse(input, [](int depth, rawengine::json::parse_event_t, rawengine::json &) {
                if (depth > 64)
                    throw Error(ErrorCode::invalid_argument, "recipe nesting exceeds 64 levels");
                return true;
            });
        if (!j.is_object())
            throw Error(ErrorCode::invalid_argument, "recipe must be an object");
        Recipe schema;
        schema.local_edits = {LocalEdit{{Mask{}}, Adjustments{}, Detail{}, 1}};
        schema.retouch = {Retouch{}};
        schema.bad_pixels = {Point{}};
        schema.geometry.guides = {Point{}};
        schema.local_edits[0].masks[0].points = {Point{}};
        schema.retouch[0].points = {Point{}};
        unknown_keys(j, rawengine::json(schema), "");
        Recipe r = j.get<Recipe>();
        r.validate();
        return r;
    } catch (const Error &) {
        throw;
    } catch (const std::exception &e) {
        throw Error(ErrorCode::invalid_argument, std::string("invalid recipe: ") + e.what());
    }
}
std::string Recipe::json() const {
    validate();
    return rawengine::json(*this).dump(2);
}
Recipe Recipe::photographic() {
    Recipe r;
    r.preset = "photographic";
    r.adjustments.contrast = 8;
    r.adjustments.highlights = -15;
    r.adjustments.shadows = 8;
    r.adjustments.vibrance = 10;
    r.adjustments.curve = {{0, 0}, {0.18f, 0.22f}, {0.5f, 0.58f}, {1, 1}};
    r.detail.chroma_noise = 15;
    r.detail.sharpen = 20;
    return r;
}
std::string Metadata::json() const {
    return rawengine::json{{"make", make},
                           {"model", model},
                           {"lens", lens},
                           {"sensor", sensor},
                           {"source", source},
                           {"width", width},
                           {"height", height},
                           {"raw_width", raw_width},
                           {"raw_height", raw_height},
                           {"top", top},
                           {"left", left},
                           {"orientation", orientation},
                           {"iso", iso},
                           {"shutter", shutter},
                           {"aperture", aperture},
                           {"focal_length", focal_length},
                           {"timestamp", timestamp},
                           {"black", black},
                           {"black_pattern_rows", black_pattern_rows},
                           {"black_pattern_columns", black_pattern_columns},
                           {"black_pattern", black_pattern},
                           {"white", white},
                           {"channel_black", channel_black},
                           {"as_shot", as_shot},
                           {"daylight", daylight},
                           {"camera_to_srgb", camera_to_srgb},
                           {"warnings", warnings}}
        .dump(2);
}
std::string capabilities_json() {
    return rawengine::json{{"engine", "rawengine"},
                           {"version", "0.1.0"},
                           {"process_version", process_version},
                           {"working_space", "linear Rec.2020 D65"},
                           {"raw_decoder", "LibRaw"},
                           {"output", {"jpeg", "png", "tiff16", "tiff32f"}},
                           {"deferred", {"AI", "HDR merge", "panorama", "Metal", "Adobe XMP"}}}
        .dump(2);
}
} // namespace rawengine
