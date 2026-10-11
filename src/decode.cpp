#include "internal.hpp"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <libraw/libraw.h>
namespace rawengine {
static int progress(void *ctx, enum LibRaw_progress, int, int) {
    auto j = static_cast<Job *>(ctx);
    return j && j->cancelled.load() ? 1 : 0;
}
static void raw_error(int code, Job *j) {
    if (j && j->cancelled.load())
        throw Error(ErrorCode::cancelled, "decode cancelled");
    if (code)
        throw Error(ErrorCode::decode, LibRaw::strerror(code));
}
static void orient(Image &image, int flip) {
    if (!flip)
        return;
    uint32_t w = image.width, h = image.height;
    bool transpose = flip & 4;
    Image out(transpose ? h : w, transpose ? w : h);
    for (uint32_t y = 0; y < out.height; ++y)
        for (uint32_t x = 0; x < out.width; ++x) {
            uint32_t sx = x, sy = y;
            if (transpose)
                std::swap(sx, sy);
            if (flip & 1)
                sx = w - 1 - sx;
            if (flip & 2)
                sy = h - 1 - sy;
            std::copy_n(image.at(sx, sy), 3, out.at(x, y));
        }
    image = std::move(out);
}
std::shared_ptr<Source> Engine::open(const std::filesystem::path &path, Job *job) const {
    PoolScope scope(state_->pool.get());
    check_job(job, 0, "open");
    auto p = std::make_shared<Source::Impl>();
    auto &m = p->metadata;
    m.source = std::filesystem::absolute(path).string();
    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return char(std::tolower(c)); });
    if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".tif" || ext == ".tiff") {
        std::shared_ptr<void> reading_ticket;
        p->linear = read_image(path, m, config_.memory_budget, [&](uint64_t bytes) {
            reading_ticket = reserve_memory(state_->memory, bytes);
        });
        p->memory_ticket =
            reserve_memory(state_->memory, p->linear.pixels.size() * sizeof(float) + config_.cache_budget);
        reading_ticket.reset();
        m.width = p->linear.width;
        m.height = p->linear.height;
        m.sensor = "rendered RGB";
        return std::shared_ptr<Source>(new Source(p));
    }
    LibRaw raw;
    raw.set_progress_handler(progress, job);
    raw.imgdata.rawparams.max_raw_memory_mb =
        unsigned(std::min<size_t>(config_.memory_budget / (1024 * 1024), 4096));
    raw_error(raw.open_file(path.string().c_str()), job);
    auto &data = raw.imgdata;
    auto &s = data.sizes;
    auto &c = data.color;
    auto decoding_ticket =
        reserve_memory(state_->memory, uint64_t(s.raw_width) * s.raw_height * 32 + config_.cache_budget);
    raw_error(raw.unpack(), job);
    m.make = data.idata.make;
    m.model = data.idata.model;
    m.lens = data.lens.Lens;
    m.raw_width = s.raw_width;
    m.raw_height = s.raw_height;
    m.width = s.width;
    m.height = s.height;
    m.top = s.top_margin;
    m.left = s.left_margin;
    m.orientation = s.flip;
    m.iso = data.other.iso_speed;
    m.shutter = data.other.shutter;
    m.aperture = data.other.aperture;
    m.focal_length = data.other.focal_len;
    m.timestamp = double(data.other.timestamp);
    m.black_pattern_rows = c.cblack[4];
    m.black_pattern_columns = c.cblack[5];
    if (uint64_t(m.black_pattern_rows) * m.black_pattern_columns + 6 <= LIBRAW_CBLACK_SIZE)
        for (size_t k = 0; k < size_t(m.black_pattern_rows) * m.black_pattern_columns; ++k)
            m.black_pattern.push_back(float(c.cblack[6 + k]));
    m.black = float(c.black);
    m.white = c.fmaximum > 0 ? c.fmaximum : float(c.maximum);
    for (int k = 0; k < 4; ++k) {
        m.channel_black[k] = float(c.cblack[k]);
        m.as_shot[k] = std::isfinite(c.cam_mul[k]) && c.cam_mul[k] > 0 ? c.cam_mul[k] : 1;
        m.daylight[k] = c.pre_mul[k] > 0 ? c.pre_mul[k] : 1;
        for (int r = 0; r < 3; ++r)
            m.camera_to_srgb[r * 4 + k] = c.rgb_cam[r][k];
    }
    float matrix_norm = 0;
    for (float v : m.camera_to_srgb)
        matrix_norm += std::abs(v);
    if (matrix_norm < 1e-5) {
        m.camera_to_srgb = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
        m.warnings.push_back("Missing camera color calibration; identity camera-to-sRGB fallback.");
    }
    if (c.profile_length && c.profile) {
        if (c.profile_length > 16 * 1024 * 1024)
            throw Error(ErrorCode::profile, "embedded profile too large");
        auto *bytes = static_cast<const uint8_t *>(c.profile);
        p->embedded_profile.assign(bytes, bytes + c.profile_length);
    }
    if (data.idata.dng_version)
        for (int list = 0; list < 3; ++list)
            if (c.dng_levels.rawopcodes[list].len)
                m.warnings.push_back("DNG opcode list " + std::to_string(list + 1) +
                                     " present; the native adapter does not independently execute it.");
    auto &rd = data.rawdata;
    bool bayer = data.idata.filters > 1000 && data.idata.colors == 3 && !data.idata.is_foveon;
    bool mono = data.idata.colors == 1;
    if ((bayer || mono) && (rd.raw_image || rd.float_image) && !rd.ph1_cblack && !rd.ph1_rblack) {
        if (uint64_t(m.left) + m.width > m.raw_width || uint64_t(m.top) + m.height > m.raw_height)
            throw Error(ErrorCode::decode, "active area outside sensor");
        m.sensor = mono ? "monochrome" : "Bayer";
        p->camera_space = !mono;
        p->mosaic.resize(size_t(m.width) * m.height);
        p->cfa.resize(p->mosaic.size());
        size_t pitch =
            s.raw_pitch ? s.raw_pitch / (rd.float_image ? sizeof(float) : sizeof(uint16_t)) : s.raw_width;
        if (pitch < s.raw_width)
            throw Error(ErrorCode::decode, "invalid raw row stride");
        parallel_rows(m.height, config_.threads, job, [&](uint32_t y) {
            for (uint32_t x = 0; x < m.width; ++x) {
                uint32_t ry = y + m.top, rx = x + m.left;
                int color = mono ? 0 : raw.COLOR(int(y), int(x));
                if (color == 3)
                    color = 1;
                if (color < 0 || color > 2)
                    throw Error(ErrorCode::unsupported, "unsupported CFA color");
                int black_channel = mono ? 0 : raw.COLOR(int(y), int(x));
                float black = float(c.black + c.cblack[black_channel]);
                unsigned rows = c.cblack[4], cols = c.cblack[5];
                if (rows && cols && uint64_t(rows) * cols + 6 <= LIBRAW_CBLACK_SIZE)
                    black += float(c.cblack[6 + (y % rows) * cols + x % cols]);
                float white = m.white;
                float value = rd.float_image ? rd.float_image[size_t(ry) * pitch + rx]
                                             : float(rd.raw_image[size_t(ry) * pitch + rx]);
                if (!std::isfinite(value) || white <= black)
                    throw Error(ErrorCode::decode, "invalid sensor calibration/sample");
                size_t idx = size_t(y) * m.width + x;
                p->mosaic[idx] = (value - black) / (white - black);
                p->cfa[idx] = uint8_t(color);
            }
        });
        if (mono) {
            p->linear = Image(m.width, m.height);
            for (size_t n = 0; n < p->mosaic.size(); ++n)
                for (int k = 0; k < 3; ++k)
                    p->linear.pixels[n * 3 + k] = p->mosaic[n];
            p->mosaic.clear();
            p->cfa.clear();
            orient(p->linear, s.flip);
        } else if (s.flip) {
            uint32_t w = m.width, h = m.height;
            bool trans = s.flip & 4;
            uint32_t ow = trans ? h : w, oh = trans ? w : h;
            std::vector<float> values(p->mosaic.size());
            std::vector<uint8_t> colors(p->cfa.size());
            for (uint32_t y = 0; y < oh; ++y)
                for (uint32_t x = 0; x < ow; ++x) {
                    uint32_t sx = x, sy = y;
                    if (trans)
                        std::swap(sx, sy);
                    if (s.flip & 1)
                        sx = w - 1 - sx;
                    if (s.flip & 2)
                        sy = h - 1 - sy;
                    values[size_t(y) * ow + x] = p->mosaic[size_t(sy) * w + sx];
                    colors[size_t(y) * ow + x] = p->cfa[size_t(sy) * w + sx];
                }
            p->mosaic = std::move(values);
            p->cfa = std::move(colors);
        }
    } else {
        m.sensor = data.idata.filters == 9 ? "X-Trans" : "LibRaw linear RGB adapter";
        auto &params = data.params;
        params.output_bps = 16;
        params.output_color = 1;
        params.gamm[0] = 1;
        params.gamm[1] = 1;
        params.no_auto_bright = 1;
        params.use_camera_wb = 1;
        params.user_flip = 0;
        params.user_qual = 3;
        params.highlight = 1;
        params.adjust_maximum_thr = 0;
        raw_error(raw.dcraw_process(), job);
        int status = 0;
        auto *bitmap = raw.dcraw_make_mem_image(&status);
        raw_error(status, job);
        if (!bitmap)
            throw Error(ErrorCode::decode, "no processed buffer");
        std::unique_ptr<libraw_processed_image_t, decltype(&LibRaw::dcraw_clear_mem)> holder(
            bitmap, &LibRaw::dcraw_clear_mem);
        if (bitmap->type != LIBRAW_IMAGE_BITMAP || bitmap->bits != 16 || bitmap->colors < 3 ||
            bitmap->colors > 4)
            throw Error(ErrorCode::unsupported, "unsupported LibRaw output layout");
        p->linear = Image(bitmap->width, bitmap->height);
        auto *samples = reinterpret_cast<const uint16_t *>(bitmap->data);
        for (size_t n = 0; n < size_t(bitmap->width) * bitmap->height; ++n) {
            float r = samples[n * bitmap->colors] / 65535.f, g = samples[n * bitmap->colors + 1] / 65535.f,
                  b = samples[n * bitmap->colors + 2] / 65535.f;
            auto *out = p->linear.pixels.data() + n * 3;
            out[0] = .627404f * r + .329283f * g + .043313f * b;
            out[1] = .069097f * r + .919540f * g + .011362f * b;
            out[2] = .016391f * r + .088013f * g + .895595f * b;
        }
        orient(p->linear, s.flip);
        m.warnings.push_back("LibRaw adapter uses as-shot WB and 16-bit linear sRGB; clipped values cannot "
                             "be recovered. Further white balance is relative.");
    }
    if (!p->camera_space && !p->embedded_profile.empty()) {
        p->embedded_profile.clear();
        m.warnings.push_back("Embedded camera ICC unavailable through the linear RGB adapter.");
    }
    if (s.flip & 4)
        std::swap(m.width, m.height);
    if (!p->linear.pixels.empty()) {
        m.width = p->linear.width;
        m.height = p->linear.height;
    }
    p->memory_ticket =
        reserve_memory(state_->memory, (p->linear.pixels.size() + p->mosaic.size()) * sizeof(float) +
                                           p->cfa.size() + config_.cache_budget);
    decoding_ticket.reset();
    check_job(job, 1, "decoded");
    return std::shared_ptr<Source>(new Source(p));
}
Image demosaic(const std::vector<float> &raw, const std::vector<uint8_t> &cfa, uint32_t w, uint32_t h,
               bool edge, unsigned threads, Job *job) {
    if (w < 2 || h < 2 || raw.size() != size_t(w) * h || cfa.size() != raw.size())
        throw Error(ErrorCode::decode, "invalid mosaic");
    Image out(w, h);
    auto get = [&](int x, int y) {
        x = std::clamp(x, 0, int(w) - 1);
        y = std::clamp(y, 0, int(h) - 1);
        return raw[size_t(y) * w + x];
    };
    // Preserve sensor samples. Reconstruct green along the smoother axis, then interpolate color differences.
    parallel_rows(h, threads, job, [&](uint32_t y) {
        for (uint32_t x = 0; x < w; ++x) {
            size_t n = size_t(y) * w + x;
            auto *p = out.at(x, y);
            if (cfa[n] == 1) {
                p[1] = raw[n];
                continue;
            }
            float sum = 0, weights = 0;
            for (int dy = -1; dy <= 1; ++dy)
                for (int dx = -1; dx <= 1; ++dx) {
                    int xx = int(x) + dx, yy = int(y) + dy;
                    if (xx >= 0 && xx < int(w) && yy >= 0 && yy < int(h) && cfa[size_t(yy) * w + xx] == 1) {
                        sum += get(xx, yy);
                        weights++;
                    }
                }
            p[1] = weights ? sum / weights : raw[n];
            if (edge && x >= 2 && y >= 2 && x + 2 < w && y + 2 < h) {
                float gh = (get(int(x) - 1, y) + get(int(x) + 1, y)) * .5f +
                           (2 * get(x, y) - get(int(x) - 2, y) - get(int(x) + 2, y)) * .25f;
                float gv = (get(x, int(y) - 1) + get(x, int(y) + 1)) * .5f +
                           (2 * get(x, y) - get(x, int(y) - 2) - get(x, int(y) + 2)) * .25f;
                float dh = std::abs(get(int(x) - 1, y) - get(int(x) + 1, y)) +
                           std::abs(2 * get(x, y) - get(int(x) - 2, y) - get(int(x) + 2, y));
                float dv = std::abs(get(x, int(y) - 1) - get(x, int(y) + 1)) +
                           std::abs(2 * get(x, y) - get(x, int(y) - 2) - get(x, int(y) + 2));
                p[1] = dh < dv ? gh : dv < dh ? gv : (gh + gv) * .5f;
            }
        }
    });
    parallel_rows(h, threads, job, [&](uint32_t y) {
        for (uint32_t x = 0; x < w; ++x) {
            auto *p = out.at(x, y);
            size_t n = size_t(y) * w + x;
            for (int c : {0, 2}) {
                if (cfa[n] == c) {
                    p[c] = raw[n];
                    continue;
                }
                double sum = 0, wt = 0;
                for (int dy = -2; dy <= 2; ++dy)
                    for (int dx = -2; dx <= 2; ++dx) {
                        int xx = int(x) + dx, yy = int(y) + dy;
                        if (xx < 0 || yy < 0 || xx >= int(w) || yy >= int(h) || cfa[size_t(yy) * w + xx] != c)
                            continue;
                        float weight = 1.f / (dx * dx + dy * dy);
                        if (edge)
                            weight /= 1 + 8 * std::abs(out.at(xx, yy)[1] - p[1]);
                        sum += weight * (raw[size_t(yy) * w + xx] - out.at(xx, yy)[1]);
                        wt += weight;
                    }
                p[c] = wt ? p[1] + float(sum / wt) : p[1];
            }
        }
    });
    return out;
}
} // namespace rawengine
