#include "internal.hpp"
#include <algorithm>
#include <cmath>
#include <lensfun.h>
#include <numbers>
#include <numeric>
namespace rawengine {
static constexpr float pi = std::numbers::pi_v<float>;
static std::array<float, 3> hsv(const float *p) {
    float hi = std::max({p[0], p[1], p[2]}), lo = std::min({p[0], p[1], p[2]}), d = hi - lo, h = 0;
    if (d > 1e-8f) {
        if (hi == p[0])
            h = std::fmod((p[1] - p[2]) / d + 6, 6.f);
        else if (hi == p[1])
            h = (p[2] - p[0]) / d + 2;
        else
            h = (p[0] - p[1]) / d + 4;
        h /= 6;
    }
    return {h, hi > 1e-8f ? d / hi : 0, hi};
}
static std::array<float, 3> rgb(float h, float s, float v) {
    h -= std::floor(h);
    float q = h * 6;
    int k = int(q);
    float f = q - k, a = v * (1 - s), b = v * (1 - s * f), c = v * (1 - s * (1 - f));
    switch (k % 6) {
    case 0:
        return {v, c, a};
    case 1:
        return {b, v, a};
    case 2:
        return {a, v, c};
    case 3:
        return {a, b, v};
    case 4:
        return {c, a, v};
    default:
        return {v, a, b};
    }
}
static std::array<float, 3> kelvin(float t) {
    t /= 100;
    float r = t <= 66 ? 255 : 329.698727f * std::pow(t - 60, -.13320476f);
    float g = t <= 66 ? 99.470802f * std::log(t) - 161.119568f : 288.12217f * std::pow(t - 60, -.07551485f);
    float b = t >= 66 ? 255 : t <= 19 ? 0 : 138.51773f * std::log(t - 10) - 305.04479f;
    std::array<float, 3> o{r, g, b};
    for (auto &v : o) {
        v = std::clamp(v / 255, .02f, 1.f);
        v = v <= .04045f ? v / 12.92f : std::pow((v + .055f) / 1.055f, 2.4f);
    }
    return o;
}
static std::array<float, 3> white_balance_gains(const Image &input, const Metadata &m, bool camera,
                                                const Recipe &r, unsigned, Job *) {
    std::array<float, 3> gains{1, 1, 1};
    const auto &wb = r.white_balance;
    if (camera) {
        float green = std::max(1e-6f, m.as_shot[1]);
        gains = {m.as_shot[0] / green, 1, m.as_shot[2] / green};
    }
    if (wb.mode == "auto" || wb.mode == "neutral") {
        std::array<double, 3> sums{};
        size_t count = 0;
        uint32_t x0 = 0, y0 = 0, x1 = input.width, y1 = input.height;
        if (wb.mode == "neutral") {
            int radius = std::max(1, int(wb.sample_radius * std::min(input.width, input.height)));
            int cx = int(wb.sample.x * (input.width - 1)), cy = int(wb.sample.y * (input.height - 1));
            x0 = uint32_t(std::max(0, cx - radius));
            y0 = uint32_t(std::max(0, cy - radius));
            x1 = uint32_t(std::min(int(input.width), cx + radius + 1));
            y1 = uint32_t(std::min(int(input.height), cy + radius + 1));
        }
        for (uint32_t y = y0; y < y1; ++y)
            for (uint32_t x = x0; x < x1; ++x) {
                auto *p = input.at(x, y);
                if (std::min({p[0], p[1], p[2]}) <= .005f || std::max({p[0], p[1], p[2]}) >= .98f)
                    continue;
                for (int c = 0; c < 3; ++c)
                    sums[c] += p[c];
                count++;
            }
        if (!count || std::min({sums[0], sums[1], sums[2]}) <= 0)
            throw Error(ErrorCode::invalid_argument, "white balance sample contains no usable pixels");
        for (int c = 0; c < 3; ++c)
            gains[c] = float(sums[1] / sums[c]);
    } else if (wb.mode == "temperature") {
        auto k = kelvin(wb.temperature), ref = kelvin(6500);
        for (int c = 0; c < 3; ++c)
            gains[c] = (camera ? m.daylight[c] / std::max(m.daylight[1], 1e-6f) : 1) * ref[c] / k[c];
        gains[1] *= std::exp(-wb.tint / 300);
    }
    return gains;
}
static float infer_temperature(const std::array<float, 3> &gains, const Metadata &m, bool camera) {
    auto ref = kelvin(6500);
    float best = 6500;
    double minimum = 1e30;
    for (int n = 0; n < 256; ++n) {
        float temp = 2000 * std::pow(25.f, n / 255.f);
        auto k = kelvin(temp);
        double error = 0;
        for (int c : {0, 2}) {
            float expected = (camera ? m.daylight[c] / std::max(m.daylight[1], 1e-6f) : 1) * ref[c] / k[c];
            expected /= ref[1] / k[1];
            double delta = std::log(std::max(gains[c] / gains[1], 1e-6f) / std::max(expected, 1e-6f));
            error += delta * delta;
        }
        if (error < minimum) {
            minimum = error;
            best = temp;
        }
    }
    return best;
}
Image develop(const Image &input, const Metadata &m, bool camera, const Recipe &r, unsigned threads,
              Job *job) {
    Image out = input;
    auto gains = white_balance_gains(input, m, camera, r, threads, job);
    out.effective_temperature = r.white_balance.mode == "temperature" ? r.white_balance.temperature
                                                                      : infer_temperature(gains, m, camera);
    parallel_rows(out.height, threads, job, [&](uint32_t y) {
        for (uint32_t x = 0; x < out.width; ++x) {
            auto *p = out.at(x, y);
            float raw[3]{p[0], p[1], p[2]};
            if (r.highlight_recovery && camera) {
                int clipped = 0;
                float ratio = 0;
                for (int c = 0; c < 3; ++c) {
                    clipped += raw[c] >= .995f;
                    if (raw[c] < .995f)
                        ratio = std::max(ratio, raw[c] * gains[c]);
                }
                if (clipped > 0 && clipped < 3)
                    for (int c = 0; c < 3; ++c)
                        if (raw[c] >= .995f)
                            raw[c] = std::max(raw[c], ratio / std::max(gains[c], 1e-6f));
            }
            for (int c = 0; c < 3; ++c)
                raw[c] *= gains[c];
            if (camera) {
                float srgb[3]{};
                for (int row = 0; row < 3; ++row)
                    for (int c = 0; c < 3; ++c)
                        srgb[row] += m.camera_to_srgb[row * 4 + c] * raw[c];
                p[0] = .627404f * srgb[0] + .329283f * srgb[1] + .043313f * srgb[2];
                p[1] = .069097f * srgb[0] + .919540f * srgb[1] + .011362f * srgb[2];
                p[2] = .016391f * srgb[0] + .088013f * srgb[1] + .895595f * srgb[2];
            } else
                std::copy_n(raw, 3, p);
        }
    });
    return out;
}
Image blur(const Image &input, float radius, unsigned threads, Job *job) {
    int r = std::max(1, int(std::ceil(radius * 2)));
    r = std::min(r, 256);
    Image tmp(input.width, input.height), out(input.width, input.height);
    // Separable sliding box filter, replicated borders. O(pixels), independent of radius.
    parallel_rows(input.height, threads, job, [&](uint32_t y) {
        double sum[3]{};
        for (int x = -r; x <= r; ++x)
            for (int c = 0; c < 3; ++c)
                sum[c] += input.at(uint32_t(std::clamp(x, 0, int(input.width) - 1)), y)[c];
        for (uint32_t x = 0; x < input.width; ++x) {
            for (int c = 0; c < 3; ++c) {
                tmp.at(x, y)[c] = float(sum[c] / (2 * r + 1));
                sum[c] += input.at(uint32_t(std::min(int(input.width) - 1, int(x) + r + 1)), y)[c] -
                          input.at(uint32_t(std::max(0, int(x) - r)), y)[c];
            }
        }
    });
    parallel_rows(input.width, threads, job, [&](uint32_t x) {
        double sum[3]{};
        for (int y = -r; y <= r; ++y)
            for (int c = 0; c < 3; ++c)
                sum[c] += tmp.at(x, uint32_t(std::clamp(y, 0, int(input.height) - 1)))[c];
        for (uint32_t y = 0; y < input.height; ++y) {
            for (int c = 0; c < 3; ++c) {
                out.at(x, y)[c] = float(sum[c] / (2 * r + 1));
                sum[c] += tmp.at(x, uint32_t(std::min(int(input.height) - 1, int(y) + r + 1)))[c] -
                          tmp.at(x, uint32_t(std::max(0, int(y) - r)))[c];
            }
        }
    });
    return out;
}
static float curve(float v, const std::vector<CurvePoint> &pts) {
    if (v < 0)
        return v;
    if (v > 1) {
        auto a = pts[pts.size() - 2], b = pts.back();
        return b.y + (v - 1) * (b.y - a.y) / (b.x - a.x);
    }
    auto it = std::upper_bound(pts.begin(), pts.end(), v, [](float x, auto p) { return x < p.x; });
    if (it == pts.begin())
        return it->y;
    if (it == pts.end())
        return pts.back().y;
    auto b = *it, a = *(it - 1);
    return std::lerp(a.y, b.y, (v - a.x) / (b.x - a.x));
}
void apply_adjustments(Image &image, const Adjustments &a, unsigned threads, Job *job, uint32_t tile_rows) {
    std::unique_ptr<Image> fine, coarse, haze;
    if (a.texture)
        fine = std::make_unique<Image>(blur(image, 1, threads, job));
    if (a.clarity)
        coarse = std::make_unique<Image>(blur(image, 12, threads, job));
    if (a.dehaze)
        haze = std::make_unique<Image>(blur(image, 24, threads, job));
    // Tile scheduling affects only execution, not filtering neighborhoods.
    uint32_t groups = uint32_t((uint64_t(image.height) + tile_rows - 1) / tile_rows);
    parallel_rows(groups, threads, job, [&](uint32_t group) {
        for (uint32_t y = group * tile_rows;
             y < std::min<uint64_t>(image.height, uint64_t(group + 1) * tile_rows); ++y)
            for (uint32_t x = 0; x < image.width; ++x) {
                auto *p = image.at(x, y);
                float before[3]{p[0], p[1], p[2]};
                float Y = luminance(p);
                if (fine)
                    for (int c = 0; c < 3; ++c)
                        p[c] += (before[c] - fine->at(x, y)[c]) * a.texture * .012f;
                if (coarse)
                    for (int c = 0; c < 3; ++c)
                        p[c] +=
                            (before[c] - coarse->at(x, y)[c]) * a.clarity * .015f * (1 - smooth(.8f, 2.f, Y));
                if (haze) {
                    float dark = std::min({haze->at(x, y)[0], haze->at(x, y)[1], haze->at(x, y)[2]});
                    float t = std::clamp(1 - a.dehaze * .008f * std::clamp(dark, 0.f, 1.f), .15f, 2.f);
                    for (int c = 0; c < 3; ++c)
                        p[c] = (p[c] - (1 - t)) / t;
                }
                float gain = std::exp2(a.exposure);
                p[0] *= gain * std::exp(a.temperature_shift * .004f);
                p[1] *= gain * std::exp(-a.tint_shift * .003f);
                p[2] *= gain * std::exp(-a.temperature_shift * .004f);
                Y = luminance(p);
                float positive = std::max(0.f, Y), tone = positive / (1 + positive);
                float stops = a.shadows * .015f * (1 - smooth(.05f, .5f, tone)) +
                              a.highlights * .015f * smooth(.25f, .75f, tone) +
                              a.whites * .008f * smooth(.45f, .9f, tone);
                float contrast_gain = std::pow(std::max(positive, 1e-5f) / .18f, a.contrast * .005f);
                float tonal_gain = std::exp2(stops) * contrast_gain;
                for (int c = 0; c < 3; ++c)
                    p[c] = p[c] * tonal_gain + a.blacks * .001f * (1 - smooth(0, .25f, tone));
                Y = luminance(p);
                if (Y > 1e-6f) {
                    float mapped = curve(Y, a.curve);
                    for (int k = 0; k < 4; ++k) {
                        float center = (k + .5f) / 4;
                        float wt = std::max(0.f, 1 - std::abs(std::clamp(Y, 0.f, 1.f) - center) * 4);
                        mapped += a.parametric[k] * .0025f * wt * std::sin(pi * std::clamp(Y, 0.f, 1.f));
                    }
                    for (int c = 0; c < 3; ++c)
                        p[c] *= mapped / Y;
                }
                Y = luminance(p);
                auto h = hsv(p);
                float satgain = std::max(0.f, 1 + a.saturation * .01f +
                                                  a.vibrance * .01f * (1 - std::clamp(h[1], 0.f, 1.f)));
                for (int c = 0; c < 3; ++c)
                    p[c] = Y + (p[c] - Y) * satgain;
                auto v = hsv(p);
                float dh = 0, ds = 0, dl = 0;
                constexpr float centers[8]{0, .083333f, .166667f, .333333f, .5f, .666667f, .75f, .833333f};
                for (int k = 0; k < 8; ++k) {
                    float d = std::abs(v[0] - centers[k]);
                    d = std::min(d, 1 - d);
                    float wt = std::max(0.f, 1 - d / .12f);
                    dh += a.hsl[k].hue * .0005f * wt;
                    ds += a.hsl[k].saturation * .01f * wt;
                    dl += a.hsl[k].luminance * .005f * wt;
                }
                if (dh || ds || dl) {
                    auto rr = rgb(v[0] + dh, std::max(0.f, v[1] * (1 + ds)), v[2] * std::exp2(dl));
                    std::copy(rr.begin(), rr.end(), p);
                }
                for (int c = 0; c < 3; ++c) {
                    float hue = a.calibration_hue[c] * .002f;
                    p[(c + 1) % 3] += p[c] * hue;
                    p[(c + 2) % 3] -= p[c] * hue;
                    p[c] *= 1 + a.calibration_saturation[c] * .005f;
                }
                Y = luminance(p);
                float t = std::clamp(Y / (1 + std::max(0.f, Y)), 0.f, 1.f);
                float wt[3]{1 - smooth(.1f, .6f, t), 0, smooth(.3f, .85f, t)};
                wt[1] = std::max(0.f, 1 - wt[0] - wt[2]);
                for (int k = 0; k < 3; ++k)
                    if (a.grading[k].saturation || a.grading[k].luminance) {
                        auto color = rgb(a.grading[k].hue / 360, 1, 1);
                        float lum = luminance(color.data());
                        for (int c = 0; c < 3; ++c)
                            p[c] +=
                                wt[k] * (color[c] - lum) * a.grading[k].saturation * .002f * std::max(0.f, Y);
                        for (int c = 0; c < 3; ++c)
                            p[c] *= std::exp2(wt[k] * a.grading[k].luminance * .01f);
                    }
                if (a.monochrome) {
                    float mono = std::inner_product(p, p + 3, a.mono_mix.begin(), 0.f);
                    std::fill_n(p, 3, mono);
                }
            }
    });
}
void apply_detail(Image &image, const Detail &d, unsigned threads, Job *job) {
    if (d.luminance_noise || d.chroma_noise) {
        Image input = image;
        parallel_rows(image.height, threads, job, [&](uint32_t y) {
            for (uint32_t x = 0; x < image.width; ++x) {
                auto *p = input.at(x, y);
                float Y = luminance(p);
                float sums[3]{}, weight = 0;
                int radius = 2 + int(d.luminance_noise / 35);
                for (int dy = -radius; dy <= radius; ++dy)
                    for (int dx = -radius; dx <= radius; ++dx) {
                        auto *q = input.at(uint32_t(std::clamp(int(x) + dx, 0, int(image.width) - 1)),
                                           uint32_t(std::clamp(int(y) + dy, 0, int(image.height) - 1)));
                        float delta = luminance(q) - Y;
                        float sigma = .005f + d.luminance_noise * .0015f;
                        float wt = std::exp(-(dx * dx + dy * dy) / float(2 * radius * radius) -
                                            delta * delta / (2 * sigma * sigma));
                        weight += wt;
                        for (int c = 0; c < 3; ++c)
                            sums[c] += q[c] * wt;
                    }
                for (float &v : sums)
                    v /= weight;
                float newY = luminance(sums), denY = std::lerp(Y, newY, d.luminance_noise / 100);
                for (int c = 0; c < 3; ++c)
                    image.at(x, y)[c] = denY + std::lerp(p[c] - Y, sums[c] - newY, d.chroma_noise / 100);
            }
        });
    }
    if (d.defringe) {
        Image input = image;
        parallel_rows(image.height, threads, job, [&](uint32_t y) {
            for (uint32_t x = 0; x < image.width; ++x) {
                auto *p = input.at(x, y);
                float Y = luminance(p);
                float purple = std::max(0.f, std::min(p[0], p[2]) - p[1]);
                float green = std::max(0.f, p[1] - std::max(p[0], p[2]));
                float amount = std::clamp(std::max(purple, green) * d.defringe * .03f, 0.f, 1.f);
                for (int c = 0; c < 3; ++c)
                    image.at(x, y)[c] = std::lerp(p[c], Y, amount);
            }
        });
    }
    if (d.sharpen) {
        Image low = blur(image, d.radius, threads, job);
        parallel_rows(image.height, threads, job, [&](uint32_t y) {
            for (uint32_t x = 0; x < image.width; ++x) {
                auto *p = image.at(x, y);
                float delta = luminance(p) - luminance(low.at(x, y));
                float edge = std::abs(delta);
                float mask = std::lerp(1.f, smooth(.001f, .03f, edge), d.masking / 100);
                float gate = smooth(d.threshold * .001f, d.threshold * .001f + .005f, edge);
                for (int c = 0; c < 3; ++c)
                    p[c] += delta * d.sharpen * .02f * mask * gate;
            }
        });
    }
}
static float segment_distance(float x, float y, Point a, Point b) {
    float dx = b.x - a.x, dy = b.y - a.y,
          t = std::clamp(((x - a.x) * dx + (y - a.y) * dy) / std::max(1e-12f, dx * dx + dy * dy), 0.f, 1.f);
    return std::hypot(x - a.x - t * dx, y - a.y - t * dy);
}
static float mask_value(const Mask &m, float x, float y, const float *p) {
    float value = 0;
    if (m.kind == "radial") {
        float d = std::hypot((x - m.start.x) / m.radius, (y - m.start.y) / m.radius_y);
        value = 1 - smooth(1 - m.feather, 1, d);
    } else if (m.kind == "brush") {
        float d = std::hypot(x - m.start.x, y - m.start.y);
        if (!m.points.empty()) {
            d = std::hypot(x - m.points[0].x, y - m.points[0].y);
            for (size_t k = 1; k < m.points.size(); ++k)
                d = std::min(d, segment_distance(x, y, m.points[k - 1], m.points[k]));
        }
        value = 1 - smooth(m.radius * (1 - m.feather), m.radius, d);
    } else if (m.kind == "linear") {
        float dx = m.end.x - m.start.x, dy = m.end.y - m.start.y;
        float t = ((x - m.start.x) * dx + (y - m.start.y) * dy) / (dx * dx + dy * dy);
        value = smooth((1 - m.feather) * .5f, (1 + m.feather) * .5f, t);
    } else if (m.kind == "luminance") {
        float v = luminance(p), f = std::max(.0001f, m.feather * .25f);
        value = smooth(m.low - f, m.low, v) * (1 - smooth(m.high, m.high + f, v));
    } else {
        float d =
            std::sqrt((p[0] - m.color[0]) * (p[0] - m.color[0]) + (p[1] - m.color[1]) * (p[1] - m.color[1]) +
                      (p[2] - m.color[2]) * (p[2] - m.color[2]));
        value = 1 - smooth(m.radius * (1 - m.feather), m.radius, d);
    }
    return m.invert ? 1 - value : value;
}
void apply_local(Image &image, const std::vector<LocalEdit> &edits, unsigned threads, Job *job) {
    for (const auto &edit : edits) {
        check_job(job, .55f, "local edit");
        Image adjusted = image;
        apply_adjustments(adjusted, edit.adjustments, threads, job);
        apply_detail(adjusted, edit.detail, threads, job);
        parallel_rows(image.height, threads, job, [&](uint32_t y) {
            for (uint32_t x = 0; x < image.width; ++x) {
                auto *p = image.at(x, y);
                float coverage = 0;
                bool first = true;
                for (const auto &m : edit.masks) {
                    float v = mask_value(m, (x + .5f) / image.width, (y + .5f) / image.height, p);
                    if (first) {
                        coverage = m.operation == "subtract" ? 1 - v : v;
                        first = false;
                    } else if (m.operation == "union")
                        coverage = 1 - (1 - coverage) * (1 - v);
                    else if (m.operation == "intersect")
                        coverage *= v;
                    else
                        coverage *= 1 - v;
                }
                for (int c = 0; c < 3; ++c)
                    p[c] = std::lerp(p[c], adjusted.at(x, y)[c], coverage * edit.opacity);
            }
        });
    }
}
void apply_retouch(Image &image, const std::vector<Retouch> &edits, unsigned threads, Job *job) {
    for (const auto &edit : edits) {
        Image input = image;
        float radius = edit.radius * std::min(image.width, image.height);
        std::vector<Point> points = edit.points.empty() ? std::vector<Point>{edit.target} : edit.points;
        float offx = (edit.source.x - edit.target.x) * image.width,
              offy = (edit.source.y - edit.target.y) * image.height;
        std::unique_ptr<Image> healing_lowpass;
        if (edit.kind == "heal")
            healing_lowpass = std::make_unique<Image>(blur(input, std::max(1.f, radius * .5f), threads, job));
        parallel_rows(image.height, threads, job, [&](uint32_t y) {
            for (uint32_t x = 0; x < image.width; ++x) {
                float d = 1e9f;
                for (size_t k = 0; k < points.size(); ++k) {
                    Point a{points[k].x * image.width, points[k].y * image.height},
                        b = k ? Point{points[k - 1].x * image.width, points[k - 1].y * image.height} : a;
                    d = std::min(d, segment_distance(x + .5f, y + .5f, a, b));
                }
                float mask = (1 - smooth(radius * (1 - edit.feather), radius, d)) * edit.opacity;
                if (!mask)
                    continue;
                auto *p = image.at(x, y);
                if (edit.kind == "red_eye") {
                    float excess = std::max(0.f, p[0] - 1.2f * std::max(p[1], p[2]));
                    p[0] -= mask * excess;
                } else {
                    auto source = sample(input, x + offx, y + offy);
                    for (int c = 0; c < 3; ++c) {
                        float v = source[c];
                        if (edit.kind == "heal")
                            v += healing_lowpass->at(x, y)[c] -
                                 sample(*healing_lowpass, x + offx, y + offy)[c];
                        p[c] = std::lerp(input.at(x, y)[c], v, mask);
                    }
                }
            }
        });
    }
}
Image apply_geometry(const Image &input, const Geometry &g, const Metadata &m, Diagnostics &diagnostics,
                     unsigned threads, Job *job) {
    Image corrected = input;
    if (g.lens_profile && ((!g.lens_name.empty() || !m.lens.empty()) && m.focal_length > 0)) {
        lfDatabase db;
        std::filesystem::path folder = g.lens_database.empty() ? RAWENGINE_INSTALL_DB : g.lens_database;
        if (g.lens_database.empty() && !std::filesystem::is_directory(folder))
            folder = RAWENGINE_LENS_DB;
        bool loaded = false;
        if (std::filesystem::is_directory(folder))
            for (const auto &e : std::filesystem::directory_iterator(folder))
                if (e.path().extension() == ".xml") {
                    if (db.Load(e.path().string().c_str()) == LF_NO_ERROR)
                        loaded = true;
                }
        const lfCamera **cameras = db.FindCameras(m.make.c_str(), m.model.c_str());
        const lfCamera *camera = cameras ? cameras[0] : nullptr;
        const lfLens **lenses =
            db.FindLenses(camera, nullptr, (g.lens_name.empty() ? m.lens : g.lens_name).c_str());
        if (!loaded || !lenses || !lenses[0])
            diagnostics.warnings.push_back("Lens profile not found; only manual corrections applied.");
        else {
            const lfLens *lens = lenses[0];
            lfModifier modifier(lens, camera ? camera->CropFactor : lens->CropFactor, int(input.width),
                                int(input.height));
            int requested = LF_MODIFY_DISTORTION | LF_MODIFY_TCA | LF_MODIFY_VIGNETTING;
            int enabled = modifier.Initialize(lens, LF_PF_F32, float(m.focal_length), float(m.aperture), 1000,
                                              1, LF_RECTILINEAR, requested, false);
            if ((enabled & requested) != requested)
                diagnostics.warnings.push_back(
                    "Lens profile lacks one or more distortion/TCA/vignetting calibrations.");
            modifier.ApplyColorModification(corrected.pixels.data(), 0, 0, int(input.width),
                                            int(input.height), LF_CR_3(RED, GREEN, BLUE),
                                            int(input.width * 3 * sizeof(float)));
            Image source = corrected;
            parallel_rows(input.height, threads, job, [&](uint32_t y) {
                std::vector<float> coords(size_t(input.width) * 6);
                if (modifier.ApplySubpixelGeometryDistortion(0, float(y), int(input.width), 1, coords.data()))
                    for (uint32_t x = 0; x < input.width; ++x)
                        for (int c = 0; c < 3; ++c)
                            corrected.at(x, y)[c] =
                                sample(source, coords[x * 6 + c * 2], coords[x * 6 + c * 2 + 1])[c];
            });
        }
        lf_free(lenses);
        lf_free(cameras);
    }
    if (g.lens_profile && ((g.lens_name.empty() && m.lens.empty()) || m.focal_length <= 0))
        diagnostics.warnings.push_back(
            "Lens name/focal length unavailable; only manual corrections applied.");
    float angle = g.rotation * pi / 180;
    if (!g.guides.empty()) {
        double total = 0;
        for (size_t k = 0; k < g.guides.size(); k += 2) {
            auto a = g.guides[k], b = g.guides[k + 1];
            float dx = (b.x - a.x) * input.width, dy = (b.y - a.y) * input.height;
            if (std::hypot(dx, dy) < 1)
                throw Error(ErrorCode::invalid_argument, "guide too short");
            total += std::atan2(dx, dy);
        }
        angle += float(total / (g.guides.size() / 2));
    }
    uint32_t w = std::max(1u, uint32_t(std::round(input.width * g.crop_width))),
             h = std::max(1u, uint32_t(std::round(input.height * g.crop_height)));
    Image out(w, h);
    float cx = (input.width - 1) * .5f, cy = (input.height - 1) * .5f,
          scale = std::max(1.f, std::max(cx, cy)), cos = std::cos(angle), sin = std::sin(angle);
    float guided_vertical = 0;
    if (g.guides.size() >= 4) {
        double num = 0, den = 0;
        for (size_t k = 0; k < g.guides.size(); k += 2) {
            auto a = g.guides[k], b = g.guides[k + 1];
            float ax = (a.x * input.width - cx) / scale, ay = (a.y * input.height - cy) / scale,
                  bx = (b.x * input.width - cx) / scale, by = (b.y * input.height - cy) / scale;
            float x1 = cos * ax - sin * ay, y1 = sin * ax + cos * ay, x2 = cos * bx - sin * by,
                  y2 = sin * bx + cos * by;
            float coeff = x1 * y2 - x2 * y1;
            num += coeff * (x1 - x2);
            den += coeff * coeff;
        }
        if (den > 1e-8)
            guided_vertical = std::clamp(float(num / den), -.4f, .4f);
    }
    parallel_rows(h, threads, job, [&](uint32_t y) {
        for (uint32_t x = 0; x < w; ++x) {
            float u = (x + g.crop_x * input.width - cx) / std::max(1.f, scale),
                  v = (y + g.crop_y * input.height - cy) / std::max(1.f, scale);
            float den = 1 + (g.vertical * .004f + guided_vertical) * v + g.horizontal * .004f * u;
            if (std::abs(den) < .1f)
                den = std::copysign(.1f, den);
            u /= den;
            v /= den;
            float ru = cos * u + sin * v, rv = -sin * u + cos * v, rad = ru * ru + rv * rv,
                  factor = 1 + g.distortion * .002f * rad;
            float sx = cx + ru * factor * scale, sy = cy + rv * factor * scale;
            auto *p = out.at(x, y);
            for (int c = 0; c < 3; ++c) {
                float ca = 1 + (c == 0 ? 1 : c == 2 ? -1 : 0) * g.chromatic_aberration * .0001f;
                auto q = sample(corrected, cx + (sx - cx) * ca, cy + (sy - cy) * ca);
                p[c] = q[c] * std::exp2(g.vignette_correction * .01f * rad);
            }
            if (sx < -.0001f || sy < -.0001f || sx > input.width - 1 + .0001f ||
                sy > input.height - 1 + .0001f)
                std::fill_n(p, 3, 0);
        }
    });
    return out;
}
static uint32_t hash(uint32_t x) {
    x ^= x >> 16;
    x *= 0x7feb352dU;
    x ^= x >> 15;
    x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}
void apply_effects(Image &image, const Recipe &r, unsigned threads, Job *job) {
    if (!r.grain && !r.vignette)
        return;
    parallel_rows(image.height, threads, job, [&](uint32_t y) {
        for (uint32_t x = 0; x < image.width; ++x) {
            auto *p = image.at(x, y);
            float u = (x + .5f) / image.width * 2 - 1, v = (y + .5f) / image.height * 2 - 1;
            float gain = std::exp2(r.vignette * .015f * smooth(.15f, 1.5f, u * u + v * v));
            float noise = 0;
            if (r.grain) {
                uint32_t gx = uint32_t(x / r.grain_size), gy = uint32_t(y / r.grain_size);
                uint32_t h = hash(gx + hash(gy + r.grain_seed));
                noise = (float(h) / float(UINT32_MAX) - .5f) * r.grain * .001f *
                        std::sqrt(std::max(.01f, luminance(p)));
            }
            for (int c = 0; c < 3; ++c)
                p[c] = p[c] * gain + noise;
        }
    });
}
} // namespace rawengine
