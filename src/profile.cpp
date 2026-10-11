#include "internal.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <lcms2.h>
#include <map>
namespace rawengine {
using Matrix = std::array<double, 9>;
static constexpr Matrix srgb_to_work{.627404, .329283, .043313, .069097, .919540,
                                     .011362, .016391, .088013, .895595};
static constexpr Matrix xyz_to_work{1.716651, -.355671, -.253366, -.666684, 1.616481,
                                    .015769,  .017640,  -.042771, .942103};
static Matrix inverse(Matrix m) {
    double d = m[0] * (m[4] * m[8] - m[5] * m[7]) - m[1] * (m[3] * m[8] - m[5] * m[6]) +
               m[2] * (m[3] * m[7] - m[4] * m[6]);
    if (std::abs(d) < 1e-10)
        throw Error(ErrorCode::profile, "singular color matrix");
    return {
        (m[4] * m[8] - m[5] * m[7]) / d, (m[2] * m[7] - m[1] * m[8]) / d, (m[1] * m[5] - m[2] * m[4]) / d,
        (m[5] * m[6] - m[3] * m[8]) / d, (m[0] * m[8] - m[2] * m[6]) / d, (m[2] * m[3] - m[0] * m[5]) / d,
        (m[3] * m[7] - m[4] * m[6]) / d, (m[1] * m[6] - m[0] * m[7]) / d, (m[0] * m[4] - m[1] * m[3]) / d};
}
static Matrix multiply(const Matrix &a, const Matrix &b) {
    Matrix o{};
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            for (int k = 0; k < 3; ++k)
                o[i * 3 + j] += a[i * 3 + k] * b[k * 3 + j];
    return o;
}
static std::array<float, 3> transform(const Matrix &m, const float *p) {
    std::array<float, 3> o{};
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            o[i] += float(m[i * 3 + j] * p[j]);
    return o;
}
static cmsHPROFILE working_profile() {
    cmsCIExyY white{.3127, .3290, 1};
    cmsCIExyYTRIPLE primaries{{.708, .292, 1}, {.170, .797, 1}, {.131, .046, 1}};
    cmsToneCurve *curves[3]{cmsBuildGamma(nullptr, 1), cmsBuildGamma(nullptr, 1), cmsBuildGamma(nullptr, 1)};
    auto p = cmsCreateRGBProfile(&white, &primaries, curves);
    for (auto c : curves)
        cmsFreeToneCurve(c);
    return p;
}
struct Dcp {
    std::vector<uint8_t> bytes;
    bool little = false;
    std::map<uint16_t, std::vector<double>> tags;
    uint64_t integer(size_t at, size_t length) const {
        if (at > bytes.size() || length > bytes.size() - at || length > 8)
            throw Error(ErrorCode::profile, "DCP offset exceeds file");
        uint64_t v = 0;
        for (size_t k = 0; k < length; ++k)
            v |= uint64_t(bytes[at + k]) << ((little ? k : length - 1 - k) * 8);
        return v;
    }
    explicit Dcp(const std::string &path) {
        std::ifstream f(path, std::ios::binary | std::ios::ate);
        if (!f)
            throw Error(ErrorCode::io, "cannot open DCP");
        auto size = f.tellg();
        if (size < 8 || size > 16 * 1024 * 1024)
            throw Error(ErrorCode::profile, "invalid DCP size");
        bytes.resize(size_t(size));
        f.seekg(0);
        f.read(reinterpret_cast<char *>(bytes.data()), size);
        little = bytes[0] == 'I' && bytes[1] == 'I';
        if (!little && !(bytes[0] == 'M' && bytes[1] == 'M'))
            throw Error(ErrorCode::profile, "invalid DCP byte order");
        if (integer(2, 2) != 42)
            throw Error(ErrorCode::profile, "invalid DCP header");
        size_t offset = size_t(integer(4, 4)), count = size_t(integer(offset, 2));
        if (count > 1024)
            throw Error(ErrorCode::profile, "too many DCP tags");
        for (size_t k = 0; k < count; ++k) {
            size_t entry = offset + 2 + k * 12;
            uint16_t tag = uint16_t(integer(entry, 2)), type = uint16_t(integer(entry + 2, 2));
            size_t n = size_t(integer(entry + 4, 4));
            size_t unit = type == 3                               ? 2
                          : type == 4 || type == 9 || type == 11  ? 4
                          : type == 5 || type == 10 || type == 12 ? 8
                                                                  : 1;
            if (n > 1024 * 1024)
                throw Error(ErrorCode::profile, "oversize DCP tag");
            size_t at = n * unit <= 4 ? entry + 8 : size_t(integer(entry + 8, 4));
            if (at > bytes.size() || n * unit > bytes.size() - at)
                throw Error(ErrorCode::profile, "DCP tag outside file");
            if (type != 3 && type != 4 && type != 5 && type != 9 && type != 10 && type != 11 && type != 12)
                continue;
            auto &values = tags[tag];
            for (size_t i = 0; i < n; ++i) {
                size_t pos = at + i * unit;
                double v = 0;
                if (type == 3 || type == 4)
                    v = double(integer(pos, unit));
                else if (type == 9)
                    v = int32_t(integer(pos, 4));
                else if (type == 5 || type == 10) {
                    double a = type == 10 ? int32_t(integer(pos, 4)) : double(integer(pos, 4)),
                           b = type == 10 ? int32_t(integer(pos + 4, 4)) : double(integer(pos + 4, 4));
                    if (!b)
                        throw Error(ErrorCode::profile, "zero DCP rational denominator");
                    v = a / b;
                } else if (type == 11) {
                    uint32_t bits = uint32_t(integer(pos, 4));
                    float value;
                    std::memcpy(&value, &bits, 4);
                    v = value;
                } else {
                    uint64_t bits = integer(pos, 8);
                    std::memcpy(&v, &bits, 8);
                }
                if (!std::isfinite(v))
                    throw Error(ErrorCode::profile, "nonfinite DCP value");
                values.push_back(v);
            }
        }
    }
};
static float illuminant(double code) {
    switch (int(code)) {
    case 17:
        return 2856;
    case 20:
        return 5500;
    case 21:
        return 6504;
    case 22:
        return 7500;
    case 23:
        return 5000;
    case 1:
    case 9:
        return 5500;
    default:
        throw Error(ErrorCode::unsupported, "unsupported DCP calibration illuminant");
    }
}
static std::array<float, 3> to_hsv(const float *p) {
    float max = std::max({p[0], p[1], p[2]}), min = std::min({p[0], p[1], p[2]}), d = max - min, h = 0;
    if (d > 1e-7f) {
        if (max == p[0])
            h = std::fmod((p[1] - p[2]) / d + 6, 6.f);
        else if (max == p[1])
            h = (p[2] - p[0]) / d + 2;
        else
            h = (p[0] - p[1]) / d + 4;
    }
    return {h / 6, max > 0 ? d / max : 0, max};
}
static std::array<float, 3> from_hsv(float h, float s, float v) {
    h -= std::floor(h);
    float q = h * 6, f = q - std::floor(q), a = v * (1 - s), b = v * (1 - s * f), c = v * (1 - s * (1 - f));
    switch (int(q) % 6) {
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
void apply_profile(Image &image, const std::string &path, float temperature, const Metadata &meta,
                   bool camera, const std::vector<uint8_t> &embedded, unsigned threads, Job *job) {
    if (path.empty() && embedded.empty())
        return;
    if (!camera)
        throw Error(ErrorCode::unsupported, "camera profiles require the native Bayer camera-space adapter");
    Matrix default_matrix{};
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c)
            default_matrix[r * 3 + c] = meta.camera_to_srgb[r * 4 + c];
    Matrix work_to_camera = inverse(multiply(srgb_to_work, default_matrix));
    auto extension = std::filesystem::path(path).extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char c) { return char(std::tolower(c)); });
    if (extension != ".dcp") {
        cmsHPROFILE input = path.empty()
                                ? cmsOpenProfileFromMem(embedded.data(), cmsUInt32Number(embedded.size()))
                                : cmsOpenProfileFromFile(path.c_str(), "r");
        if (!input)
            throw Error(ErrorCode::profile, "cannot open ICC camera profile");
        cmsHPROFILE output = working_profile();
        cmsHTRANSFORM tx =
            cmsCreateTransform(input, TYPE_RGB_FLT, output, TYPE_RGB_FLT, INTENT_RELATIVE_COLORIMETRIC,
                               cmsFLAGS_NOOPTIMIZE | cmsFLAGS_NOCACHE);
        cmsCloseProfile(input);
        cmsCloseProfile(output);
        if (!tx)
            throw Error(ErrorCode::profile, "unsupported ICC camera profile");
        try {
            for (uint32_t y = 0; y < image.height; ++y) {
                if (job && job->cancelled.load())
                    throw Error(ErrorCode::cancelled, "profile cancelled");
                for (uint32_t x = 0; x < image.width; ++x) {
                    auto cam = transform(work_to_camera, image.at(x, y));
                    cmsDoTransform(tx, cam.data(), image.at(x, y), 1);
                }
            }
        } catch (...) {
            cmsDeleteTransform(tx);
            throw;
        }
        cmsDeleteTransform(tx);
        return;
    }
    Dcp dcp(path);
    auto &tags = dcp.tags;
    for (auto tag : {50964, 50965, 50966, 50967, 50981, 50982, 51107, 51108, 51109, 51110})
        if (tags.contains(uint16_t(tag)))
            throw Error(ErrorCode::unsupported, "unsupported DCP feature tag " + std::to_string(tag));
    if (!tags.contains(50721) || tags[50721].size() != 9)
        throw Error(ErrorCode::profile, "DCP needs a 3x3 ColorMatrix1");
    float weight = 0, temp1 = 6504, temp2 = 2856;
    if (tags.contains(50778) && tags[50778].size() != 1)
        throw Error(ErrorCode::profile, "invalid illuminant tag");
    if (tags.contains(50778))
        temp1 = illuminant(tags[50778][0]);
    if (tags.contains(50722)) {
        if (tags[50722].size() != 9 || !tags.contains(50779) || tags[50779].size() != 1)
            throw Error(ErrorCode::profile, "invalid dual-illuminant DCP");
        temp2 = illuminant(tags[50779][0]);
        if (temp1 == temp2)
            throw Error(ErrorCode::profile, "duplicate DCP illuminants");
        weight = std::clamp((1 / temperature - 1 / temp1) / (1 / temp2 - 1 / temp1), 0.f, 1.f);
    }
    Matrix color{};
    for (int k = 0; k < 9; ++k)
        color[k] = tags[50721][k] * (1 - weight) + (tags.contains(50722) ? tags[50722][k] * weight : 0);
    // Normalize camera primaries to D65 neutral, preserving matrix color relationships.
    constexpr float white[3]{.95047f, 1, 1.08883f};
    auto neutral = transform(color, white);
    Matrix diagonal{neutral[0], 0, 0, 0, neutral[1], 0, 0, 0, neutral[2]};
    Matrix conversion = multiply(multiply(xyz_to_work, inverse(color)), diagonal);
    conversion = multiply(conversion, work_to_camera);
    std::vector<CurvePoint> tone;
    if (tags.contains(50940)) {
        auto &data = tags[50940];
        if (data.size() < 4 || data.size() % 2)
            throw Error(ErrorCode::profile, "invalid DCP tone curve");
        for (size_t k = 0; k < data.size(); k += 2) {
            CurvePoint p{float(data[k]), float(data[k + 1])};
            if (p.x < 0 || p.x > 1 || p.y < 0 || p.y > 1 || (!tone.empty() && p.x <= tone.back().x))
                throw Error(ErrorCode::profile, "invalid DCP tone point");
            tone.push_back(p);
        }
        if (tone.front().x != 0 || tone.back().x != 1)
            throw Error(ErrorCode::profile, "DCP curve must span 0..1");
    }
    size_t nh = 0, ns = 0, nv = 0;
    std::vector<double> table;
    if (tags.contains(50937)) {
        auto dims = tags[50937];
        if (dims.size() != 3 || dims[0] < 1 || dims[1] < 2 || dims[2] < 1 || dims[0] > 360 || dims[1] > 256 ||
            dims[2] > 256)
            throw Error(ErrorCode::profile, "invalid DCP hue table dimensions");
        nh = size_t(dims[0]);
        ns = size_t(dims[1]);
        nv = size_t(dims[2]);
        for (double v : dims)
            if (v != std::floor(v))
                throw Error(ErrorCode::profile, "fractional DCP dimensions");
        if (!tags.contains(50938) || tags[50938].size() != nh * ns * nv * 3)
            throw Error(ErrorCode::profile, "invalid DCP hue table");
        table = tags[50938];
        if (tags.contains(50939)) {
            if (tags[50939].size() != table.size())
                throw Error(ErrorCode::profile, "inconsistent dual DCP tables");
            for (size_t k = 0; k < table.size(); ++k)
                table[k] = std::lerp(table[k], tags[50939][k], double(weight));
        }
    }
    const Matrix adapt{1.0478112, .0228866,  -.050127, .0295424, .9904844,
                       -.0170491, -.0092345, .0150436, .7521316};
    const Matrix xyz50_to_pro{1.3459433, -.2556075, -.0511118, -.5445989, 1.5081673,
                              .0205351,  0,         0,         1.2118128};
    const Matrix work_to_pro = multiply(multiply(xyz50_to_pro, adapt), inverse(xyz_to_work));
    const Matrix pro_to_work = inverse(work_to_pro);
    parallel_rows(image.height, threads, job, [&](uint32_t y) {
        for (uint32_t x = 0; x < image.width; ++x) {
            auto *p = image.at(x, y);
            auto rgb = transform(conversion, p);
            std::copy(rgb.begin(), rgb.end(), p);
            if (!table.empty()) {
                auto pro = transform(work_to_pro, p);
                auto h = to_hsv(pro.data());
                float hh = h[0] * nh, ss = std::clamp(h[1], 0.f, 1.f) * (ns - 1),
                      vv = std::clamp(h[2], 0.f, 1.f) * (nv - 1);
                size_t hi = size_t(hh) % nh, si = std::min(size_t(ss), ns - 2),
                       vi = std::min(size_t(vv), nv - 1);
                float fractions[3]{hh - std::floor(hh), ss - si, vv - vi};
                float correction[3]{};
                for (int a = 0; a < 2; ++a)
                    for (int b = 0; b < 2; ++b)
                        for (int c = 0; c < 2; ++c) {
                            float wt = (a ? fractions[0] : 1 - fractions[0]) *
                                       (b ? fractions[1] : 1 - fractions[1]) *
                                       (c ? fractions[2] : 1 - fractions[2]);
                            size_t offset =
                                ((std::min(vi + c, nv - 1) * nh + (hi + a) % nh) * ns + si + b) * 3;
                            for (int k = 0; k < 3; ++k)
                                correction[k] += float(table[offset + k]) * wt;
                        }
                auto value = from_hsv(h[0] + correction[0] / 360, std::max(0.f, h[1] * correction[1]),
                                      h[2] * correction[2]);
                auto mapped = transform(pro_to_work, value.data());
                std::copy(mapped.begin(), mapped.end(), p);
            }
            if (!tone.empty()) {
                float lum = luminance(p);
                if (lum > 0 && lum <= 1) {
                    auto it = std::upper_bound(tone.begin(), tone.end(), lum,
                                               [](float v, auto pt) { return v < pt.x; });
                    float mapped = it == tone.end() ? tone.back().y
                                                    : std::lerp((it - 1)->y, it->y,
                                                                (lum - (it - 1)->x) / (it->x - (it - 1)->x));
                    for (int c = 0; c < 3; ++c)
                        p[c] *= mapped / lum;
                }
            }
        }
    });
}
} // namespace rawengine
