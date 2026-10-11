#pragma once
#include "rawengine/engine.hpp"
#include <cstring>
#include <functional>
#include <iomanip>
#include <sstream>
namespace rawengine {
// Bounded reader for the capture metadata we preserve; deliberately ignores maker notes and GPS.
inline int read_exif(const uint8_t *bytes, size_t length, Metadata &metadata) {
    if (length < 8)
        return 1;
    bool little = bytes[0] == 'I' && bytes[1] == 'I';
    if (!little && !(bytes[0] == 'M' && bytes[1] == 'M'))
        return 1;
    auto integer = [&](size_t at, size_t count) -> uint32_t {
        if (count > 4 || at > length || count > length - at)
            throw Error(ErrorCode::decode, "EXIF offset exceeds segment");
        uint32_t value = 0;
        for (size_t k = 0; k < count; ++k)
            value |= uint32_t(bytes[at + k]) << ((little ? k : count - 1 - k) * 8);
        return value;
    };
    if (integer(2, 2) != 42)
        return 1;
    int orientation = 1;
    size_t exif_offset = 0;
    auto directory = [&](size_t at, bool sub) {
        uint32_t n = integer(at, 2);
        if (n > 1024)
            throw Error(ErrorCode::decode, "too many EXIF tags");
        for (uint32_t k = 0; k < n; ++k) {
            size_t entry = at + 2 + k * 12;
            uint16_t tag = uint16_t(integer(entry, 2)), type = uint16_t(integer(entry + 2, 2));
            size_t count = integer(entry + 4, 4), unit = type == 3                 ? 2
                                                         : type == 4               ? 4
                                                         : type == 5 || type == 10 ? 8
                                                                                   : 1;
            if (count > 1024 * 1024)
                throw Error(ErrorCode::decode, "oversize EXIF tag");
            size_t data = count * unit <= 4 ? entry + 8 : integer(entry + 8, 4);
            if (data > length || count * unit > length - data)
                throw Error(ErrorCode::decode, "EXIF data outside segment");
            auto text = [&] {
                if (type != 2 || !count)
                    return std::string{};
                size_t size = 0;
                while (size < count && bytes[data + size])
                    ++size;
                return std::string(reinterpret_cast<const char *>(bytes + data), size);
            };
            auto value = [&]() -> double {
                if (!count)
                    return 0;
                if (type == 3)
                    return integer(data, 2);
                if (type == 4)
                    return integer(data, 4);
                if (type == 5) {
                    double denominator = integer(data + 4, 4);
                    return denominator ? integer(data, 4) / denominator : 0;
                }
                return 0;
            };
            if (!sub) {
                if (tag == 274)
                    orientation = int(value());
                else if (tag == 271)
                    metadata.make = text();
                else if (tag == 272)
                    metadata.model = text();
                else if (tag == 34665)
                    exif_offset = size_t(value());
            } else {
                if (tag == 33434)
                    metadata.shutter = value();
                else if (tag == 33437)
                    metadata.aperture = value();
                else if (tag == 34855)
                    metadata.iso = value();
                else if (tag == 37386)
                    metadata.focal_length = value();
                else if (tag == 42036)
                    metadata.lens = text();
                else if (tag == 36867) {
                    std::tm date{};
                    std::istringstream stream(text());
                    stream >> std::get_time(&date, "%Y:%m:%d %H:%M:%S");
                    if (!stream.fail()) {
#ifdef _WIN32
                        metadata.timestamp = double(_mkgmtime(&date));
#else
                        metadata.timestamp = double(timegm(&date));
#endif
                    }
                }
            }
        }
    };
    directory(integer(4, 4), false);
    if (exif_offset)
        directory(exif_offset, true);
    return orientation;
}
} // namespace rawengine
