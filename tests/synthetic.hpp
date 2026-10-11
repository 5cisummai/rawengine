#pragma once
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>
// Original deterministic fixtures. No third-party photographs or camera profiles.
namespace synthetic {
struct Tag {
    uint16_t id, type;
    uint32_t count;
    std::vector<uint8_t> data;
};
inline std::vector<uint8_t> integers(std::initializer_list<uint32_t> values, int bytes) {
    std::vector<uint8_t> data;
    for (auto v : values)
        for (int k = 0; k < bytes; ++k)
            data.push_back(uint8_t(v >> (k * 8)));
    return data;
}
inline std::vector<uint8_t> rationals(std::initializer_list<int32_t> values) {
    std::vector<uint8_t> data;
    for (auto v : values) {
        auto part = integers({uint32_t(v), 10000}, 4);
        data.insert(data.end(), part.begin(), part.end());
    }
    return data;
}
inline std::vector<uint8_t> floats(std::initializer_list<float> values) {
    std::vector<uint8_t> data;
    for (float v : values) {
        uint32_t bits;
        std::memcpy(&bits, &v, 4);
        auto part = integers({bits}, 4);
        data.insert(data.end(), part.begin(), part.end());
    }
    return data;
}
inline void tiff(const std::filesystem::path &path, std::vector<Tag> tags, std::vector<uint8_t> pixels = {}) {
    std::sort(tags.begin(), tags.end(), [](const Tag &a, const Tag &b) { return a.id < b.id; });
    std::vector<uint8_t> file(8 + 2 + tags.size() * 12 + 4);
    file[0] = 'I';
    file[1] = 'I';
    file[2] = 42;
    file[4] = 8;
    auto put = [&](size_t offset, uint32_t value, int bytes) {
        for (int k = 0; k < bytes; ++k)
            file[offset + k] = uint8_t(value >> (8 * k));
    };
    put(8, uint32_t(tags.size()), 2);
    size_t strip_entry = 0;
    for (size_t k = 0; k < tags.size(); ++k) {
        auto &tag = tags[k];
        size_t entry = 10 + k * 12;
        put(entry, tag.id, 2);
        put(entry + 2, tag.type, 2);
        put(entry + 4, tag.count, 4);
        if (tag.id == 273)
            strip_entry = entry + 8;
        if (tag.data.size() <= 4)
            std::copy(tag.data.begin(), tag.data.end(), file.begin() + entry + 8);
        else {
            put(entry + 8, uint32_t(file.size()), 4);
            file.insert(file.end(), tag.data.begin(), tag.data.end());
            if (file.size() % 2)
                file.push_back(0);
        }
    }
    if (strip_entry) {
        put(strip_entry, uint32_t(file.size()), 4);
        file.insert(file.end(), pixels.begin(), pixels.end());
    }
    std::ofstream output(path, std::ios::binary);
    output.write(reinterpret_cast<const char *>(file.data()), std::streamsize(file.size()));
}
inline void dng(const std::filesystem::path &path, bool defect = false, bool colored = false) {
    const uint32_t w = 32, h = 32;
    std::vector<uint8_t> raw;
    for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < w; ++x) {
            uint16_t value = (defect && x == 16 && y == 16) ? 4000
                             : colored ? ((y % 2) ? (x % 2 ? 1800 : 600) : (x % 2 ? 600 : 1100))
                                       : 1100;
            raw.push_back(uint8_t(value));
            raw.push_back(uint8_t(value >> 8));
        }
    auto i = [](uint16_t id, uint16_t type, std::initializer_list<uint32_t> v) {
        return Tag{id, type, uint32_t(v.size()), integers(v, type == 3 ? 2 : 4)};
    };
    std::vector<Tag> tags{i(256, 4, {w}),
                          i(257, 4, {h}),
                          i(258, 3, {16}),
                          i(259, 3, {1}),
                          i(262, 3, {32803}),
                          i(273, 4, {0}),
                          i(277, 3, {1}),
                          i(278, 4, {h}),
                          i(279, 4, {w * h * 2}),
                          i(284, 3, {1}),
                          i(33421, 3, {2, 2}),
                          Tag{33422, 1, 4, {0, 1, 1, 2}},
                          Tag{50706, 1, 4, {1, 4, 0, 0}},
                          Tag{50707, 1, 4, {1, 1, 0, 0}},
                          Tag{50708, 2, 5, {'T', 'e', 's', 't', 0}},
                          i(50713, 3, {2, 2}),
                          Tag{50714, 5, 4, rationals({1000000, 1000000, 1000000, 1000000})},
                          i(50717, 4, {4100}),
                          Tag{50721, 10, 9, rationals({10000, 0, 0, 0, 10000, 0, 0, 0, 10000})},
                          Tag{50728, 5, 3, rationals({10000, 10000, 10000})},
                          i(50778, 3, {21})};
    tiff(path, tags, raw);
}
inline void dcp(const std::filesystem::path &path, bool with_table = false, bool unsupported = false,
                bool dual = false) {
    std::vector<Tag> tags{{50721, 10, 9, rationals({10000, 0, 0, 0, 10000, 0, 0, 0, 10000})},
                          {50778, 3, 1, integers({21}, 2)},
                          {50940, 11, 6, floats({0, 0, .5f, .6f, 1, 1})}};
    if (dual) {
        tags.push_back(Tag{50722, 10, 9, rationals({10000, 1000, 0, 0, 10000, 0, 0, 0, 10000})});
        tags.push_back(Tag{50779, 3, 1, integers({17}, 2)});
    }
    if (with_table) {
        tags.push_back(Tag{50937, 4, 3, integers({1, 2, 1}, 4)});
        tags.push_back(Tag{50938, 11, 6, floats({0, 1, 1, 20, 1.1f, 1})});
    }
    if (unsupported)
        tags.push_back(Tag{50964, 10, 9, rationals({10000, 0, 0, 0, 10000, 0, 0, 0, 10000})});
    tiff(path, tags);
}
} // namespace synthetic
