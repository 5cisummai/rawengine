#include "exif.hpp"
#include "internal.hpp"
#include <algorithm>
#include <cmath>
#include <csetjmp>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <jpeglib.h>
#include <lcms2.h>
#include <png.h>
#include <random>
#include <sstream>
#include <tiffio.h>
namespace rawengine {
struct Profile {
    cmsHPROFILE p = nullptr;
    explicit Profile(cmsHPROFILE v) : p(v) {}
    ~Profile() {
        if (p)
            cmsCloseProfile(p);
    }
    Profile(const Profile &) = delete;
};
static cmsHPROFILE make_profile(const std::string &space, bool linear) {
    if (space == "srgb" && !linear)
        return cmsCreate_sRGBProfile();
    cmsCIExyY white{.3127, .329, 1};
    cmsCIExyYTRIPLE primaries;
    if (space == "srgb")
        primaries = {{.64, .33, 1}, {.30, .60, 1}, {.15, .06, 1}};
    else if (space == "display_p3")
        primaries = {{.68, .32, 1}, {.265, .69, 1}, {.15, .06, 1}};
    else if (space == "rec2020")
        primaries = {{.708, .292, 1}, {.170, .797, 1}, {.131, .046, 1}};
    else
        throw Error(ErrorCode::invalid_argument, "unknown output color space");
    cmsToneCurve *curves[3]{};
    if (linear)
        for (auto &c : curves)
            c = cmsBuildGamma(nullptr, 1);
    else if (space == "display_p3") {
        double params[7]{2.4, 1 / 1.055, .055 / 1.055, 1 / 12.92, .04045, 0, 0};
        for (auto &c : curves)
            c = cmsBuildParametricToneCurve(nullptr, 4, params);
    } else
        for (auto &c : curves)
            c = cmsBuildGamma(nullptr, 2.4);
    auto result = cmsCreateRGBProfile(&white, &primaries, curves);
    for (auto c : curves)
        cmsFreeToneCurve(c);
    return result;
}
static std::vector<uint8_t> profile_bytes(cmsHPROFILE p) {
    cmsUInt32Number size = 0;
    if (!cmsSaveProfileToMem(p, nullptr, &size))
        throw Error(ErrorCode::profile, "cannot serialize ICC");
    std::vector<uint8_t> b(size);
    if (!cmsSaveProfileToMem(p, b.data(), &size))
        throw Error(ErrorCode::profile, "cannot serialize ICC");
    return b;
}
static void convert_input(Image &image, const std::vector<uint8_t> &bytes, bool linear) {
    Profile in(bytes.empty() ? make_profile("srgb", linear)
                             : cmsOpenProfileFromMem(bytes.data(), cmsUInt32Number(bytes.size())));
    Profile out(make_profile("rec2020", true));
    if (!in.p || !out.p)
        throw Error(ErrorCode::profile, "invalid embedded RGB ICC profile");
    auto tx = cmsCreateTransform(in.p, TYPE_RGB_FLT, out.p, TYPE_RGB_FLT, INTENT_RELATIVE_COLORIMETRIC,
                                 cmsFLAGS_NOOPTIMIZE | cmsFLAGS_NOCACHE);
    if (!tx)
        throw Error(ErrorCode::profile, "unsupported input ICC profile");
    for (uint32_t y = 0; y < image.height; ++y)
        cmsDoTransform(tx, image.at(0, y), image.at(0, y), image.width);
    cmsDeleteTransform(tx);
}
struct File {
    FILE *f;
    explicit File(const std::filesystem::path &p, const char *mode)
        : f(std::fopen(p.string().c_str(), mode)) {
        if (!f)
            throw Error(ErrorCode::io, "cannot open " + p.string());
    }
    ~File() {
        if (f)
            std::fclose(f);
    }
    void close() {
        if (f) {
            auto *p = f;
            f = nullptr;
            if (std::fclose(p))
                throw Error(ErrorCode::io, "file close failed");
        }
    }
};
struct JpegError {
    jpeg_error_mgr base;
    jmp_buf jump;
    char message[JMSG_LENGTH_MAX]{};
};
static void jpeg_error(j_common_ptr c) {
    auto *e = reinterpret_cast<JpegError *>(c->err);
    (*c->err->format_message)(c, e->message);
    longjmp(e->jump, 1);
}
static void orient_input(Image &image, int orientation) {
    if (orientation == 1)
        return;
    if (orientation < 1 || orientation > 8)
        throw Error(ErrorCode::unsupported, "invalid image orientation");
    uint32_t w = image.width, h = image.height;
    Image out(orientation >= 5 ? h : w, orientation >= 5 ? w : h);
    for (uint32_t y = 0; y < out.height; ++y)
        for (uint32_t x = 0; x < out.width; ++x) {
            uint32_t sx = x, sy = y;
            switch (orientation) {
            case 2:
                sx = w - 1 - x;
                break;
            case 3:
                sx = w - 1 - x;
                sy = h - 1 - y;
                break;
            case 4:
                sy = h - 1 - y;
                break;
            case 5:
                sx = y;
                sy = x;
                break;
            case 6:
                sx = y;
                sy = h - 1 - x;
                break;
            case 7:
                sx = w - 1 - y;
                sy = h - 1 - x;
                break;
            case 8:
                sx = w - 1 - y;
                sy = x;
                break;
            }
            std::copy_n(image.at(sx, sy), 3, out.at(x, y));
        }
    image = std::move(out);
}
Image read_image(const std::filesystem::path &path, Metadata &meta, size_t budget,
                 const std::function<void(uint64_t)> &reserve) {
    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return char(std::tolower(c)); });
    Image image;
    std::vector<uint8_t> icc;
    if (ext == ".jpg" || ext == ".jpeg") {
        File file(path, "rb");
        jpeg_decompress_struct decoder{};
        JpegError error{};
        decoder.err = jpeg_std_error(&error.base);
        error.base.error_exit = jpeg_error;
        std::vector<uint8_t> row;
        std::map<unsigned, std::vector<uint8_t>> chunks;
        if (setjmp(error.jump)) {
            jpeg_destroy_decompress(&decoder);
            throw Error(ErrorCode::decode, error.message);
        }
        jpeg_create_decompress(&decoder);
        jpeg_stdio_src(&decoder, file.f);
        jpeg_save_markers(&decoder, JPEG_APP0 + 2, 0xffff);
        jpeg_save_markers(&decoder, JPEG_APP0 + 1, 0xffff);
        jpeg_read_header(&decoder, TRUE);
        uint64_t estimate = uint64_t(decoder.image_width) * decoder.image_height * 3 * sizeof(float) * 4;
        if (estimate > budget) {
            jpeg_destroy_decompress(&decoder);
            throw Error(ErrorCode::memory_limit, "JPEG exceeds budget");
        }
        try {
            reserve(estimate);
        } catch (...) {
            jpeg_destroy_decompress(&decoder);
            throw;
        }
        decoder.out_color_space = JCS_RGB;
        jpeg_start_decompress(&decoder);
        image = Image(decoder.output_width, decoder.output_height);
        row.resize(size_t(image.width) * 3);
        int orientation = 1;
        for (auto *marker = decoder.marker_list; marker; marker = marker->next)
            if (marker->marker == JPEG_APP0 + 1 && marker->data_length >= 14 &&
                !std::memcmp(marker->data, "Exif\0\0", 6)) {
                try {
                    orientation = read_exif(marker->data + 6, marker->data_length - 6, meta);
                } catch (...) {
                    jpeg_destroy_decompress(&decoder);
                    throw;
                }
            }
        unsigned expected = 0;
        for (auto *marker = decoder.marker_list; marker; marker = marker->next)
            if (marker->marker == JPEG_APP0 + 2 && marker->data_length >= 14 &&
                !std::memcmp(marker->data, "ICC_PROFILE\0", 12)) {
                expected = marker->data[13];
                chunks[marker->data[12]] =
                    std::vector<uint8_t>(marker->data + 14, marker->data + marker->data_length);
            }
        if (expected) {
            if (chunks.size() != expected) {
                jpeg_destroy_decompress(&decoder);
                throw Error(ErrorCode::profile, "incomplete JPEG ICC");
            }
            for (unsigned k = 1; k <= expected; ++k) {
                if (!chunks.contains(k)) {
                    jpeg_destroy_decompress(&decoder);
                    throw Error(ErrorCode::profile, "invalid JPEG ICC sequence");
                }
                icc.insert(icc.end(), chunks[k].begin(), chunks[k].end());
            }
        }
        while (decoder.output_scanline < decoder.output_height) {
            uint32_t y = decoder.output_scanline;
            JSAMPROW ptr = row.data();
            jpeg_read_scanlines(&decoder, &ptr, 1);
            for (uint32_t x = 0; x < image.width * 3; ++x)
                image.at(0, y)[x] = row[x] / 255.f;
        }
        jpeg_finish_decompress(&decoder);
        jpeg_destroy_decompress(&decoder);
        convert_input(image, icc, false);
        orient_input(image, orientation);
    } else if (ext == ".png") {
        File file(path, "rb");
        auto png = png_create_read_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
        if (!png)
            throw std::bad_alloc();
        auto info = png_create_info_struct(png);
        if (!info) {
            png_destroy_read_struct(&png, nullptr, nullptr);
            throw std::bad_alloc();
        }
        std::vector<uint8_t> bytes;
        std::vector<png_bytep> rows;
        if (setjmp(png_jmpbuf(png))) {
            png_destroy_read_struct(&png, &info, nullptr);
            throw Error(ErrorCode::decode, "invalid PNG");
        }
        png_init_io(png, file.f);
        png_read_info(png, info);
        uint32_t w = png_get_image_width(png, info), h = png_get_image_height(png, info);
        int depth = png_get_bit_depth(png, info), color = png_get_color_type(png, info);
        if (uint64_t(w) * h * 3 * sizeof(float) * 4 > budget) {
            png_destroy_read_struct(&png, &info, nullptr);
            throw Error(ErrorCode::memory_limit, "PNG exceeds budget");
        }
        try {
            reserve(uint64_t(w) * h * 3 * sizeof(float) * 4);
        } catch (...) {
            png_destroy_read_struct(&png, &info, nullptr);
            throw;
        }
        char *name;
        int compression;
        png_bytep profile;
        png_uint_32 length;
        if (png_get_iCCP(png, info, &name, &compression, &profile, &length))
            icc.assign(profile, profile + length);
        if (color == PNG_COLOR_TYPE_PALETTE)
            png_set_palette_to_rgb(png);
        if (color == PNG_COLOR_TYPE_GRAY && depth < 8)
            png_set_expand_gray_1_2_4_to_8(png);
        if (png_get_valid(png, info, PNG_INFO_tRNS))
            png_set_tRNS_to_alpha(png);
        if (color == PNG_COLOR_TYPE_GRAY || color == PNG_COLOR_TYPE_GRAY_ALPHA)
            png_set_gray_to_rgb(png);
        png_set_strip_alpha(png);
        png_set_interlace_handling(png);
        png_read_update_info(png, info);
        depth = png_get_bit_depth(png, info);
        size_t stride = png_get_rowbytes(png, info);
        bytes.resize(stride * h);
        rows.resize(h);
        for (uint32_t y = 0; y < h; ++y)
            rows[y] = bytes.data() + y * stride;
        png_read_image(png, rows.data());
        png_read_end(png, info);
        int orientation = 1;
#ifdef PNG_eXIf_SUPPORTED
        png_uint_32 exif_length = 0;
        png_bytep exif_bytes = nullptr;
        if (png_get_eXIf_1(png, info, &exif_length, &exif_bytes)) {
            try {
                orientation = read_exif(exif_bytes, exif_length, meta);
            } catch (...) {
                png_destroy_read_struct(&png, &info, nullptr);
                throw;
            }
        }
#endif
        png_destroy_read_struct(&png, &info, nullptr);
        image = Image(w, h);
        for (uint32_t y = 0; y < h; ++y)
            for (uint32_t x = 0; x < w * 3; ++x) {
                size_t k = y * stride + x * (depth == 16 ? 2 : 1);
                image.at(0, y)[x] =
                    depth == 16 ? float((unsigned(bytes[k]) << 8) | bytes[k + 1]) / 65535 : bytes[k] / 255.f;
            }
        convert_input(image, icc, false);
        orient_input(image, orientation);
        if (icc.empty())
            meta.warnings.push_back("PNG without ICC interpreted as sRGB; custom gAMA/cHRM are not used.");
    } else {
        TIFF *raw = TIFFOpen(path.string().c_str(), "r");
        if (!raw)
            throw Error(ErrorCode::decode, "cannot open TIFF");
        std::unique_ptr<TIFF, decltype(&TIFFClose)> t(raw, &TIFFClose);
        uint32_t w = 0, h = 0;
        uint16_t bits = 0, channels = 0, photometric = 0, planar = 0, format = 0, orientation = 1;
        TIFFGetField(raw, TIFFTAG_IMAGEWIDTH, &w);
        TIFFGetField(raw, TIFFTAG_IMAGELENGTH, &h);
        TIFFGetFieldDefaulted(raw, TIFFTAG_BITSPERSAMPLE, &bits);
        TIFFGetFieldDefaulted(raw, TIFFTAG_SAMPLESPERPIXEL, &channels);
        TIFFGetField(raw, TIFFTAG_PHOTOMETRIC, &photometric);
        TIFFGetFieldDefaulted(raw, TIFFTAG_PLANARCONFIG, &planar);
        TIFFGetFieldDefaulted(raw, TIFFTAG_SAMPLEFORMAT, &format);
        TIFFGetFieldDefaulted(raw, TIFFTAG_ORIENTATION, &orientation);
        if (photometric != PHOTOMETRIC_RGB || planar != PLANARCONFIG_CONTIG || channels < 3 || channels > 4 ||
            (bits != 8 && bits != 16 && bits != 32) || (bits == 32 && format != SAMPLEFORMAT_IEEEFP) ||
            TIFFIsTiled(raw))
            throw Error(ErrorCode::unsupported, "TIFF requires stripped contiguous RGB 8/16 integer or 32f");
        budget_check(uint64_t(w) * h * 3 * sizeof(float) * 4, budget);
        reserve(uint64_t(w) * h * 3 * sizeof(float) * 4);
        image = Image(w, h);
        std::vector<uint8_t> row(size_t(TIFFScanlineSize(raw)));
        if (row.size() < size_t(w) * channels * (bits / 8))
            throw Error(ErrorCode::decode, "invalid TIFF stride");
        uint32_t size;
        void *data;
        if (TIFFGetField(raw, TIFFTAG_ICCPROFILE, &size, &data))
            icc.assign(static_cast<uint8_t *>(data), static_cast<uint8_t *>(data) + size);
        char *text;
        if (TIFFGetField(raw, TIFFTAG_MAKE, &text))
            meta.make = text;
        if (TIFFGetField(raw, TIFFTAG_MODEL, &text))
            meta.model = text;
        for (uint32_t y = 0; y < h; ++y) {
            if (TIFFReadScanline(raw, row.data(), y) < 0)
                throw Error(ErrorCode::decode, "TIFF read failure");
            for (uint32_t x = 0; x < w; ++x)
                for (int c = 0; c < 3; ++c) {
                    size_t k = size_t(x) * channels + c;
                    float v;
                    if (bits == 8)
                        v = row[k] / 255.f;
                    else if (bits == 16) {
                        uint16_t s;
                        std::memcpy(&s, row.data() + k * 2, 2);
                        v = s / 65535.f;
                    } else
                        std::memcpy(&v, row.data() + k * 4, 4);
                    image.at(x, y)[c] = v;
                }
        }
        uint64_t exif_offset = 0;
        if (TIFFGetField(raw, TIFFTAG_EXIFIFD, &exif_offset) && TIFFReadEXIFDirectory(raw, exif_offset)) {
            float value = 0;
            if (TIFFGetField(raw, EXIFTAG_EXPOSURETIME, &value))
                meta.shutter = value;
            if (TIFFGetField(raw, EXIFTAG_FNUMBER, &value))
                meta.aperture = value;
            if (TIFFGetField(raw, EXIFTAG_FOCALLENGTH, &value))
                meta.focal_length = value;
            uint16_t count = 0;
            uint16_t *iso = nullptr;
            if (TIFFGetField(raw, EXIFTAG_ISOSPEEDRATINGS, &count, &iso) && count)
                meta.iso = iso[0];
            if (TIFFGetField(raw, EXIFTAG_LENSMODEL, &text))
                meta.lens = text;
        }
        convert_input(image, icc, bits == 32);
        orient_input(image, orientation);
        if (icc.empty())
            meta.warnings.push_back(bits == 32 ? "Unprofiled float TIFF interpreted as linear sRGB."
                                               : "Unprofiled integer TIFF interpreted as sRGB.");
    }
    validate_image(image);
    return image;
}
static std::vector<uint8_t> exif(const Metadata &m) {
    struct Tag {
        uint16_t id, type;
        uint32_t count;
        std::vector<uint8_t> bytes;
    };
    std::vector<Tag> root, sub;
    auto shorttag = [](uint16_t id, uint16_t value) {
        return Tag{id, 3, 1, {uint8_t(value), uint8_t(value >> 8)}};
    };
    auto strtag = [](uint16_t id, std::string value) {
        value.resize(std::min<size_t>(value.size(), 1024));
        value.push_back('\0');
        return Tag{id, 2, uint32_t(value.size()), std::vector<uint8_t>(value.begin(), value.end())};
    };
    auto rational = [](uint16_t id, double value) {
        uint32_t den = 1000000, num = uint32_t(std::clamp(value * den, 0., double(UINT32_MAX)));
        Tag t{id, 5, 1, {}};
        for (uint32_t v : {num, den})
            for (int k = 0; k < 4; ++k)
                t.bytes.push_back(uint8_t(v >> (k * 8)));
        return t;
    };
    root.push_back(shorttag(274, 1));
    if (!m.make.empty())
        root.push_back(strtag(271, m.make));
    if (!m.model.empty())
        root.push_back(strtag(272, m.model));
    root.push_back(strtag(305, "rawengine 0.1.0"));
    if (m.shutter > 0)
        sub.push_back(rational(33434, m.shutter));
    if (m.aperture > 0)
        sub.push_back(rational(33437, m.aperture));
    if (m.iso > 0)
        sub.push_back(shorttag(34855, uint16_t(std::min(m.iso, 65535.))));
    if (m.focal_length > 0)
        sub.push_back(rational(37386, m.focal_length));
    if (!m.lens.empty())
        sub.push_back(strtag(42036, m.lens));
    if (m.timestamp > 0) {
        std::time_t timestamp = std::time_t(m.timestamp);
        std::tm date{};
#ifdef _WIN32
        gmtime_s(&date, &timestamp);
#else
        gmtime_r(&timestamp, &date);
#endif
        std::ostringstream text;
        text << std::put_time(&date, "%Y:%m:%d %H:%M:%S");
        sub.push_back(strtag(36867, text.str()));
    }
    std::sort(root.begin(), root.end(), [](auto a, auto b) { return a.id < b.id; });
    std::sort(sub.begin(), sub.end(), [](auto a, auto b) { return a.id < b.id; });
    uint32_t suboffset = 8 + 2 + uint32_t(root.size() + 1) * 12 + 4;
    root.push_back(Tag{
        34665,
        4,
        1,
        {uint8_t(suboffset), uint8_t(suboffset >> 8), uint8_t(suboffset >> 16), uint8_t(suboffset >> 24)}});
    size_t dataoffset = suboffset + 2 + sub.size() * 12 + 4;
    std::vector<uint8_t> bytes(dataoffset);
    bytes[0] = 'I';
    bytes[1] = 'I';
    bytes[2] = 42;
    bytes[4] = 8;
    auto put = [&](size_t at, uint32_t value, int n) {
        for (int k = 0; k < n; ++k)
            bytes[at + k] = uint8_t(value >> (8 * k));
    };
    auto directory = [&](size_t at, const std::vector<Tag> &tags) {
        put(at, uint32_t(tags.size()), 2);
        for (size_t i = 0; i < tags.size(); ++i) {
            auto &t = tags[i];
            size_t p = at + 2 + i * 12;
            put(p, t.id, 2);
            put(p + 2, t.type, 2);
            put(p + 4, t.count, 4);
            if (t.bytes.size() <= 4)
                std::copy(t.bytes.begin(), t.bytes.end(), bytes.begin() + p + 8);
            else {
                put(p + 8, uint32_t(bytes.size()), 4);
                bytes.insert(bytes.end(), t.bytes.begin(), t.bytes.end());
                if (bytes.size() % 2)
                    bytes.push_back(0);
            }
        }
    };
    directory(8, root);
    directory(suboffset, sub);
    return bytes;
}
void write_image(const Image &input, const Metadata &metadata, const std::filesystem::path &path,
                 const ExportOptions &opts, Job *job) {
    validate_image(input);
    std::string format = opts.format.empty() ? path.extension().string() : opts.format;
    if (!format.empty() && format[0] == '.')
        format.erase(0, 1);
    std::transform(format.begin(), format.end(), format.begin(),
                   [](unsigned char c) { return char(std::tolower(c)); });
    if (format == "jpg")
        format = "jpeg";
    if (format == "tif")
        format = "tiff";
    if (format != "jpeg" && format != "png" && format != "tiff")
        throw Error(ErrorCode::invalid_argument, "output format must be JPEG, PNG, or TIFF");
    if (opts.quality < 1 || opts.quality > 100 || (opts.bits != 8 && opts.bits != 16) ||
        opts.output_sharpen < 0 || opts.output_sharpen > 100 || !std::isfinite(opts.output_sharpen))
        throw Error(ErrorCode::invalid_argument, "invalid export options");
    if (opts.floating_point && format != "tiff")
        throw Error(ErrorCode::invalid_argument, "float output requires TIFF");
    Profile output(opts.output_profile.empty() ? make_profile(opts.color_space, opts.floating_point)
                                               : cmsOpenProfileFromFile(opts.output_profile.c_str(), "r"));
    Profile working(make_profile("rec2020", true));
    if (!output.p || cmsGetColorSpace(output.p) != cmsSigRgbData)
        throw Error(ErrorCode::profile, "invalid RGB output ICC");
    if (opts.floating_point && !opts.output_profile.empty())
        throw Error(ErrorCode::unsupported, "float TIFF uses built-in linear profiles only");
    auto icc = profile_bytes(output.p);
    cmsHTRANSFORM transform =
        cmsCreateTransform(working.p, TYPE_RGB_FLT, output.p, TYPE_RGB_FLT, INTENT_RELATIVE_COLORIMETRIC,
                           cmsFLAGS_NOOPTIMIZE | cmsFLAGS_NOCACHE);
    if (!transform)
        throw Error(ErrorCode::profile, "cannot create output transform");
    struct Tx {
        cmsHTRANSFORM t;
        ~Tx() {
            cmsDeleteTransform(t);
        }
    } tx{transform};
    std::vector<float> converted(size_t(input.width) * 3);
    std::vector<uint8_t> bytes(size_t(input.width) * 3 * 2);
    auto metadata_bytes = opts.preserve_metadata ? exif(metadata) : std::vector<uint8_t>{};
    auto row = [&](uint32_t y, int bits, bool big_endian) {
        check_job(job, float(y) / input.height, "export");
        cmsDoTransform(transform, input.at(0, y), converted.data(), input.width);
        if (bits == 32)
            return;
        for (size_t k = 0; k < converted.size(); ++k) {
            float v = std::clamp(converted[k], 0.f, 1.f);
            if (bits == 8)
                bytes[k] = uint8_t(std::round(v * 255));
            else {
                uint16_t n = uint16_t(std::round(v * 65535));
                if (big_endian) {
                    bytes[k * 2] = uint8_t(n >> 8);
                    bytes[k * 2 + 1] = uint8_t(n);
                } else
                    std::memcpy(bytes.data() + k * 2, &n, 2);
            }
        }
    };
    std::filesystem::path temporary;

    for (int attempt = 0; attempt < 16; ++attempt) {
        temporary = path;
        temporary += ".tmp-" + std::to_string(std::random_device{}());
        FILE *file = std::fopen(temporary.string().c_str(), "wbx");
        if (file) {
            std::fclose(file);
            break;
        }
        if (attempt == 15)
            throw Error(ErrorCode::io, "cannot reserve temporary export");
    }

    struct Cleanup {
        std::filesystem::path p;
        bool done = false;
        ~Cleanup() {
            if (!done) {
                std::error_code e;
                std::filesystem::remove(p, e);
            }
        }
    } cleanup{temporary};
    if (format == "jpeg") {
        File file(temporary, "wb");
        jpeg_compress_struct encoder{};
        JpegError error{};
        std::vector<uint8_t> app;
        encoder.err = jpeg_std_error(&error.base);
        error.base.error_exit = jpeg_error;
        if (setjmp(error.jump)) {
            jpeg_destroy_compress(&encoder);
            throw Error(ErrorCode::io, error.message);
        }
        jpeg_create_compress(&encoder);
        jpeg_stdio_dest(&encoder, file.f);
        encoder.image_width = input.width;
        encoder.image_height = input.height;
        encoder.input_components = 3;
        encoder.in_color_space = JCS_RGB;
        jpeg_set_defaults(&encoder);
        jpeg_set_quality(&encoder, opts.quality, TRUE);
        for (int k = 0; k < 3; ++k) {
            encoder.comp_info[k].h_samp_factor = 1;
            encoder.comp_info[k].v_samp_factor = 1;
        }
        jpeg_start_compress(&encoder, TRUE);
        if (!metadata_bytes.empty()) {
            app = {'E', 'x', 'i', 'f', 0, 0};
            app.insert(app.end(), metadata_bytes.begin(), metadata_bytes.end());
            jpeg_write_marker(&encoder, JPEG_APP0 + 1, app.data(), unsigned(app.size()));
        }
        constexpr size_t chunk = 65519;
        size_t count = (icc.size() + chunk - 1) / chunk;
        if (count > 255) {
            jpeg_destroy_compress(&encoder);
            throw Error(ErrorCode::profile, "ICC too large for JPEG");
        }
        for (size_t k = 0; k < count; ++k) {
            app = {'I', 'C', 'C', '_', 'P', 'R', 'O', 'F', 'I', 'L', 'E', 0, uint8_t(k + 1), uint8_t(count)};
            auto start = icc.begin() + k * chunk;
            app.insert(app.end(), start, icc.begin() + std::min(icc.size(), (k + 1) * chunk));
            jpeg_write_marker(&encoder, JPEG_APP0 + 2, app.data(), unsigned(app.size()));
        }
        try {
            while (encoder.next_scanline < encoder.image_height) {
                row(encoder.next_scanline, 8, false);
                JSAMPROW ptr = bytes.data();
                jpeg_write_scanlines(&encoder, &ptr, 1);
            }
            jpeg_finish_compress(&encoder);
        } catch (...) {
            jpeg_destroy_compress(&encoder);
            throw;
        }
        jpeg_destroy_compress(&encoder);
        file.close();
    } else if (format == "png") {
        File file(temporary, "wb");
        auto png = png_create_write_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
        if (!png)
            throw std::bad_alloc();
        auto info = png_create_info_struct(png);
        if (!info) {
            png_destroy_write_struct(&png, nullptr);
            throw std::bad_alloc();
        }
        if (setjmp(png_jmpbuf(png))) {
            png_destroy_write_struct(&png, &info);
            throw Error(ErrorCode::io, "PNG write failure");
        }
        png_init_io(png, file.f);
        png_set_IHDR(png, info, input.width, input.height, opts.bits, PNG_COLOR_TYPE_RGB, PNG_INTERLACE_NONE,
                     PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
        png_set_iCCP(png, info, "rawengine", PNG_COMPRESSION_TYPE_BASE, icc.data(), png_uint_32(icc.size()));
#ifdef PNG_eXIf_SUPPORTED
        if (!metadata_bytes.empty())
            png_set_eXIf_1(png, info, png_uint_32(metadata_bytes.size()), metadata_bytes.data());
#endif
        png_write_info(png, info);
        try {
            for (uint32_t y = 0; y < input.height; ++y) {
                row(y, opts.bits, true);
                png_write_row(png, bytes.data());
            }
            png_write_end(png, info);
        } catch (...) {
            png_destroy_write_struct(&png, &info);
            throw;
        }
        png_destroy_write_struct(&png, &info);
        file.close();
    } else {
        TIFF *raw = TIFFOpen(temporary.string().c_str(), "w");
        if (!raw)
            throw Error(ErrorCode::io, "cannot write TIFF");
        std::unique_ptr<TIFF, decltype(&TIFFClose)> t(raw, &TIFFClose);
        uint64_t exif_offset = 0;
        if (opts.preserve_metadata) {
            TIFFCreateEXIFDirectory(raw);
            TIFFSetField(raw, EXIFTAG_EXIFVERSION, "0232");
            if (metadata.shutter > 0)
                TIFFSetField(raw, EXIFTAG_EXPOSURETIME, metadata.shutter);
            if (metadata.aperture > 0)
                TIFFSetField(raw, EXIFTAG_FNUMBER, metadata.aperture);
            if (metadata.focal_length > 0)
                TIFFSetField(raw, EXIFTAG_FOCALLENGTH, metadata.focal_length);
            if (metadata.iso > 0) {
                uint16_t iso = uint16_t(std::min(metadata.iso, 65535.));
                TIFFSetField(raw, EXIFTAG_ISOSPEEDRATINGS, 1, &iso);
            }
            if (!metadata.lens.empty())
                TIFFSetField(raw, EXIFTAG_LENSMODEL, metadata.lens.c_str());
            if (!TIFFWriteCustomDirectory(raw, &exif_offset))
                throw Error(ErrorCode::io, "TIFF EXIF failure");
            TIFFCreateDirectory(raw);
        }
        TIFFSetField(raw, TIFFTAG_IMAGEWIDTH, input.width);
        TIFFSetField(raw, TIFFTAG_IMAGELENGTH, input.height);
        TIFFSetField(raw, TIFFTAG_SAMPLESPERPIXEL, 3);
        TIFFSetField(raw, TIFFTAG_BITSPERSAMPLE, opts.floating_point ? 32 : opts.bits);
        TIFFSetField(raw, TIFFTAG_SAMPLEFORMAT,
                     opts.floating_point ? SAMPLEFORMAT_IEEEFP : SAMPLEFORMAT_UINT);
        TIFFSetField(raw, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_RGB);
        TIFFSetField(raw, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);
        TIFFSetField(raw, TIFFTAG_ORIENTATION, ORIENTATION_TOPLEFT);
        TIFFSetField(raw, TIFFTAG_COMPRESSION, COMPRESSION_ADOBE_DEFLATE);
        TIFFSetField(raw, TIFFTAG_ROWSPERSTRIP, 32);
        TIFFSetField(raw, TIFFTAG_ICCPROFILE, uint32_t(icc.size()), icc.data());
        TIFFSetField(raw, TIFFTAG_SOFTWARE, "rawengine 0.1.0");
        if (opts.preserve_metadata) {
            if (exif_offset)
                TIFFSetField(raw, TIFFTAG_EXIFIFD, exif_offset);
            if (!metadata.make.empty())
                TIFFSetField(raw, TIFFTAG_MAKE, metadata.make.c_str());
            if (!metadata.model.empty())
                TIFFSetField(raw, TIFFTAG_MODEL, metadata.model.c_str());
        }
        for (uint32_t y = 0; y < input.height; ++y) {
            row(y, opts.floating_point ? 32 : opts.bits, false);
            if (TIFFWriteScanline(raw,
                                  opts.floating_point ? static_cast<void *>(converted.data())
                                                      : static_cast<void *>(bytes.data()),
                                  y) < 0)
                throw Error(ErrorCode::io, "TIFF write failure");
        }
        if (!TIFFWriteDirectory(raw))
            throw Error(ErrorCode::io, "TIFF directory failure");
        t.reset();
    }
    check_job(job, 1, "publish export");
    std::error_code error;
    std::filesystem::rename(temporary, path, error);
    if (error)
        throw Error(ErrorCode::io, "cannot publish export: " + error.message());
    cleanup.done = true;
}
} // namespace rawengine
