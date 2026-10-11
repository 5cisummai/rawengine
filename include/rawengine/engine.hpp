#pragma once
#include <array>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace rawengine {
inline constexpr int process_version = 1;
enum class ErrorCode { invalid_argument = 1, io, decode, unsupported, cancelled, memory_limit, profile };
class Error : public std::runtime_error {
  public:
    ErrorCode code;
    Error(ErrorCode c, const std::string &message) : std::runtime_error(message), code(c) {}
};
struct Image {
    uint32_t width = 0, height = 0;
    std::vector<float> pixels; // interleaved linear Rec.2020 RGB, unassociated; finite HDR values allowed
    float effective_temperature = 6500; // estimated illuminant used for profile interpolation
    Image() = default;
    Image(uint32_t w, uint32_t h);
    float *at(uint32_t x, uint32_t y) {
        return pixels.data() + (size_t(y) * width + x) * 3;
    }
    const float *at(uint32_t x, uint32_t y) const {
        return pixels.data() + (size_t(y) * width + x) * 3;
    }

  private:
    std::shared_ptr<void> memory_ticket_;
    friend class Engine;
    friend struct CBridgeAccess;
};
struct Metadata {
    std::string make, model, lens, sensor, source;
    uint32_t width = 0, height = 0, raw_width = 0, raw_height = 0, top = 0, left = 0;
    int orientation = 0;
    double iso = 0, shutter = 0, aperture = 0, focal_length = 0, timestamp = 0;
    float black = 0, white = 1;
    uint32_t black_pattern_rows = 0, black_pattern_columns = 0;
    std::vector<float> black_pattern;
    std::array<float, 4> channel_black{}, as_shot{1, 1, 1, 1}, daylight{1, 1, 1, 1};
    std::array<float, 12> camera_to_srgb{};
    std::vector<std::string> warnings;
    std::string json() const;
};
struct Point {
    float x = 0, y = 0;
};
struct CurvePoint {
    float x = 0, y = 0;
};
struct HSLBand {
    float hue = 0, saturation = 0, luminance = 0;
};
struct Grade {
    float hue = 0, saturation = 0, luminance = 0;
};
struct Adjustments {
    float exposure = 0, contrast = 0, highlights = 0, shadows = 0, whites = 0, blacks = 0;
    float saturation = 0, vibrance = 0, texture = 0, clarity = 0, dehaze = 0;
    float temperature_shift = 0, tint_shift = 0;
    std::vector<CurvePoint> curve{{0, 0}, {1, 1}};
    std::array<float, 4> parametric{}; // shadows, darks, lights, highlights
    std::array<HSLBand, 8> hsl{};      // red, orange, yellow, green, aqua, blue, purple, magenta
    bool monochrome = false;
    std::array<float, 3> mono_mix{0.2627f, 0.6780f, 0.0593f};
    std::array<Grade, 3> grading{}; // shadows, midtones, highlights
    std::array<float, 3> calibration_hue{}, calibration_saturation{};
};
struct WhiteBalance {
    std::string mode = "as_shot"; // as_shot, auto, neutral, temperature
    float temperature = 6500, tint = 0;
    Point sample{0.5f, 0.5f};
    float sample_radius = 0.01f;
};
struct Detail {
    float luminance_noise = 0, chroma_noise = 0, sharpen = 0, radius = 1, threshold = 0, masking = 0;
    float defringe = 0;
};
struct Geometry {
    float crop_x = 0, crop_y = 0, crop_width = 1, crop_height = 1;
    float rotation = 0, vertical = 0, horizontal = 0, distortion = 0, chromatic_aberration = 0,
          vignette_correction = 0;
    bool lens_profile = false;
    std::string lens_database, lens_name;
    std::vector<Point> guides; // pairs of points expected to become vertical
};
struct Mask {
    std::string kind = "radial", operation = "union";
    Point start{0.5f, 0.5f}, end{0.5f, 1};
    float radius = 0.25f, radius_y = 0.25f, feather = 0.5f, low = 0, high = 1;
    std::array<float, 3> color{1, 0, 0}; // linear working RGB
    bool invert = false;
    std::vector<Point> points;
};
struct LocalEdit {
    std::vector<Mask> masks;
    Adjustments adjustments;
    Detail detail;
    float opacity = 1;
};
struct Retouch {
    std::string kind = "clone"; // clone, heal, red_eye
    Point target, source;
    std::vector<Point> points;
    float radius = 0.025f, feather = 0.5f, opacity = 1;
};
struct Recipe {
    int version = 1, process = process_version;
    std::string preset = "neutral", camera_profile;
    WhiteBalance white_balance;
    bool highlight_recovery = true;
    std::vector<Point> bad_pixels; // normalized documented defect locations, before demosaicing
    Adjustments adjustments;
    Detail detail;
    Geometry geometry;
    std::vector<LocalEdit> local_edits;
    std::vector<Retouch> retouch;
    float grain = 0, grain_size = 1, vignette = 0;
    uint32_t grain_seed = 1;
    static Recipe photographic();
    static Recipe from_json(const std::string &);
    std::string json() const;
    void validate() const;
};
struct Config {
    size_t memory_budget = size_t(8) * 1024 * 1024 * 1024;
    size_t cache_budget = size_t(128) * 1024 * 1024;
    unsigned threads = 0;
};
struct RenderOptions {
    uint32_t max_dimension = 0, x = 0, y = 0, width = 0, height = 0;
    uint32_t tile_rows = 64;
};
struct Job {
    std::atomic<bool> cancelled{false};
    std::function<void(float, const std::string &)> progress;
    void check(float fraction, const std::string &stage) const;
};
struct Diagnostics {
    std::array<std::array<uint64_t, 256>, 3> histogram{};
    uint64_t shadow_clipped = 0, highlight_clipped = 0;
    std::vector<std::string> warnings;
};
struct RenderResult {
    Image image;
    Diagnostics diagnostics;
};
struct ExportOptions {
    std::string format = "", color_space = "srgb", output_profile;
    int quality = 92, bits = 16;
    uint32_t width = 0, height = 0;
    bool floating_point = false, preserve_metadata = true;
    float output_sharpen = 0;
};
class Source {
  public:
    const Metadata &metadata() const;
    ~Source();

  private:
    struct Impl;
    std::shared_ptr<const Impl> impl_;
    explicit Source(std::shared_ptr<const Impl>);
    friend class Engine;
};
class Engine {
  public:
    explicit Engine(Config = {});
    std::shared_ptr<Source> open(const std::filesystem::path &, Job * = nullptr) const;
    std::shared_ptr<Source>
        from_image(Image, Metadata = {}) const; // already linear Rec.2020; useful for integrations/tests
    RenderResult render(const Source &, const Recipe & = {}, RenderOptions = {}, Job * = nullptr) const;
    void export_image(const Source &, const Recipe &, const std::filesystem::path &, ExportOptions = {},
                      Job * = nullptr) const;
    const Config &config() const {
        return config_;
    }

  private:
    friend struct CBridgeAccess;
    struct State;
    std::shared_ptr<State> state_;
    Config config_;
};
Image resize(const Image &, uint32_t width, uint32_t height);
std::string capabilities_json();
} // namespace rawengine
