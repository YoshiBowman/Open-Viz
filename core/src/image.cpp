#include "vizcore/image.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <string>

#include "miniz.h"

namespace viz {

namespace {

uint32_t be32(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) | (static_cast<uint32_t>(p[2]) << 8) | p[3];
}

struct Header {
    uint32_t width = 0;
    uint32_t height = 0;
    int depth = 0;
    int color_type = 0;
    bool interlaced = false;

    int channels() const { return color_type == 0 ? 1 : color_type == 2 ? 3 : color_type == 3 ? 1 : color_type == 4 ? 2 : 4; }
    // Bytes between a pixel and the one to its left, as the filters count them: at least one.
    size_t stride() const { return static_cast<size_t>(channels() * depth + 7) / 8; }
    size_t row_bytes(uint32_t pixels) const { return (static_cast<size_t>(pixels) * static_cast<size_t>(channels() * depth) + 7) / 8; }
};

// Undoes PNG's per-row prediction in place. `previous` is the finished row above, or null.
bool unfilter(int filter, uint8_t* row, const uint8_t* previous, size_t bytes, size_t stride) {
    switch (filter) {
        case 0:
            return true;
        case 1:
            for (size_t i = stride; i < bytes; ++i) row[i] = static_cast<uint8_t>(row[i] + row[i - stride]);
            return true;
        case 2:
            if (previous) {
                for (size_t i = 0; i < bytes; ++i) row[i] = static_cast<uint8_t>(row[i] + previous[i]);
            }
            return true;
        case 3:
            for (size_t i = 0; i < bytes; ++i) {
                const int left = i >= stride ? row[i - stride] : 0;
                const int up = previous ? previous[i] : 0;
                row[i] = static_cast<uint8_t>(row[i] + ((left + up) >> 1));
            }
            return true;
        case 4:
            for (size_t i = 0; i < bytes; ++i) {
                const int left = i >= stride ? row[i - stride] : 0;
                const int up = previous ? previous[i] : 0;
                const int corner = (previous && i >= stride) ? previous[i - stride] : 0;
                const int estimate = left + up - corner;
                const int to_left = std::abs(estimate - left);
                const int to_up = std::abs(estimate - up);
                const int to_corner = std::abs(estimate - corner);
                const int nearest = (to_left <= to_up && to_left <= to_corner) ? left : (to_up <= to_corner ? up : corner);
                row[i] = static_cast<uint8_t>(row[i] + nearest);
            }
            return true;
        default:
            return false;
    }
}

// One sample of `depth` bits, number `index` in a row, scaled to 0-255. Palette indices are not scaled.
unsigned sample(const uint8_t* row, size_t index, int depth, bool scale) {
    if (depth == 8) return row[index];
    if (depth == 16) return row[index * 2];    // the high byte: near enough for a picture on screen
    const size_t bit = index * static_cast<size_t>(depth);
    const unsigned raw = (static_cast<unsigned>(row[bit >> 3]) >> (8u - static_cast<unsigned>(depth) - static_cast<unsigned>(bit & 7u))) &
                         ((1u << depth) - 1u);
    return scale ? raw * 255u / ((1u << depth) - 1u) : raw;
}

// The full value of a sample, for comparing with a tRNS colour key.
unsigned raw_sample(const uint8_t* row, size_t index, int depth) {
    if (depth == 16) return (static_cast<unsigned>(row[index * 2]) << 8) | row[index * 2 + 1];
    return sample(row, index, depth, false);
}

struct Palette {
    uint8_t rgba[256][4];
    int count = 0;
};

struct ColorKey {
    bool present = false;
    unsigned value[3] = {0, 0, 0};
};

// Writes one finished row of `pixels` pixels to the picture, at row y, columns x0, x0 + step, ...
void store_row(const Header& header, const Palette& palette, const ColorKey& key, const uint8_t* row, uint32_t pixels, Image& image, uint32_t y,
               uint32_t x0, uint32_t step) {
    for (uint32_t i = 0; i < pixels; ++i) {
        uint8_t* out = &image.rgba[(static_cast<size_t>(y) * static_cast<size_t>(image.width) + x0 + static_cast<size_t>(i) * step) * 4];
        switch (header.color_type) {
            case 0: {
                const unsigned grey = sample(row, i, header.depth, true);
                out[0] = out[1] = out[2] = static_cast<uint8_t>(grey);
                out[3] = (key.present && raw_sample(row, i, header.depth) == key.value[0]) ? 0 : 255;
                break;
            }
            case 2: {
                bool keyed = key.present;
                for (size_t c = 0; c < 3; ++c) {
                    out[c] = static_cast<uint8_t>(sample(row, static_cast<size_t>(i) * 3 + c, header.depth, true));
                    keyed = keyed && raw_sample(row, static_cast<size_t>(i) * 3 + c, header.depth) == key.value[c];
                }
                out[3] = keyed ? 0 : 255;
                break;
            }
            case 3: {
                const unsigned index = sample(row, i, header.depth, false);
                // An index past the end of the palette is an error in the file: shown as opaque black.
                static const uint8_t missing[4] = {0, 0, 0, 255};
                const uint8_t* entry = static_cast<int>(index) < palette.count ? palette.rgba[index] : missing;
                for (int c = 0; c < 4; ++c) out[c] = entry[c];
                break;
            }
            case 4: {
                const unsigned grey = sample(row, static_cast<size_t>(i) * 2, header.depth, true);
                out[0] = out[1] = out[2] = static_cast<uint8_t>(grey);
                out[3] = static_cast<uint8_t>(sample(row, static_cast<size_t>(i) * 2 + 1, header.depth, true));
                break;
            }
            default:
                for (size_t c = 0; c < 4; ++c) out[c] = static_cast<uint8_t>(sample(row, static_cast<size_t>(i) * 4 + c, header.depth, true));
                break;
        }
    }
}

double srgb_to_linear(double encoded) { return encoded <= 0.04045 ? encoded / 12.92 : std::pow((encoded + 0.055) / 1.055, 2.4); }

}  // namespace

ImageResult parse_png(const uint8_t* data, size_t size) {
    ImageResult result;
    static const uint8_t signature[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    if (!data || size < 8 || !std::equal(signature, signature + 8, data)) {
        result.diagnostics.error("PNG", "not a PNG file");
        return result;
    }

    Header header;
    Palette palette;
    ColorKey key;
    bool have_header = false;
    std::vector<uint8_t> packed;
    for (size_t at = 8; at + 12 <= size;) {
        const size_t length = be32(data + at);
        const uint8_t* type = data + at + 4;
        const uint8_t* body = data + at + 8;
        if (length > size - at - 12) {
            result.diagnostics.error("PNG", "is cut short");
            return result;
        }
        const auto is = [type](const char* name) { return type[0] == name[0] && type[1] == name[1] && type[2] == name[2] && type[3] == name[3]; };
        if (is("IHDR")) {
            if (length < 13 || have_header) {
                result.diagnostics.error("PNG", "has a damaged header");
                return result;
            }
            header.width = be32(body);
            header.height = be32(body + 4);
            header.depth = body[8];
            header.color_type = body[9];
            header.interlaced = body[12] == 1;
            const int d = header.depth;
            const bool depth_ok = header.color_type == 0   ? (d == 1 || d == 2 || d == 4 || d == 8 || d == 16)
                                  : header.color_type == 3 ? (d == 1 || d == 2 || d == 4 || d == 8)
                                  : (header.color_type == 2 || header.color_type == 4 || header.color_type == 6) ? (d == 8 || d == 16)
                                  : false;
            if (!depth_ok || body[10] != 0 || body[11] != 0 || body[12] > 1) {
                result.diagnostics.error("PNG", "uses a pixel format that PNG does not define");
                return result;
            }
            if (header.width == 0 || header.height == 0 || header.width > static_cast<uint32_t>(kMaxImageSide) ||
                header.height > static_cast<uint32_t>(kMaxImageSide)) {
                result.diagnostics.error("PNG", "is " + std::to_string(header.width) + " x " + std::to_string(header.height) +
                                                    " pixels; pictures up to " + std::to_string(kMaxImageSide) + " on a side are read");
                return result;
            }
            have_header = true;
        } else if (!have_header) {
            result.diagnostics.error("PNG", "does not start with a header");
            return result;
        } else if (is("PLTE")) {
            palette.count = static_cast<int>(std::min<size_t>(length / 3, 256));
            for (int i = 0; i < palette.count; ++i) {
                for (int c = 0; c < 3; ++c) palette.rgba[i][c] = body[i * 3 + c];
                palette.rgba[i][3] = 255;
            }
        } else if (is("tRNS")) {
            if (header.color_type == 3) {
                for (size_t i = 0; i < length && static_cast<int>(i) < palette.count; ++i) palette.rgba[i][3] = body[i];
            } else if (header.color_type == 0 && length >= 2) {
                key.present = true;
                key.value[0] = (static_cast<unsigned>(body[0]) << 8) | body[1];
            } else if (header.color_type == 2 && length >= 6) {
                key.present = true;
                for (size_t c = 0; c < 3; ++c) key.value[c] = (static_cast<unsigned>(body[c * 2]) << 8) | body[c * 2 + 1];
            }
        } else if (is("IDAT")) {
            packed.insert(packed.end(), body, body + length);
        } else if (is("IEND")) {
            break;
        }
        at += length + 12;
    }
    if (!have_header || packed.empty()) {
        result.diagnostics.error("PNG", "has no picture data");
        return result;
    }

    // An interlaced picture is stored as seven smaller ones, each a coarser grid of the whole.
    struct Pass {
        uint32_t x0, y0, dx, dy;
    };
    static const Pass adam7[7] = {{0, 0, 8, 8}, {4, 0, 8, 8}, {0, 4, 4, 8}, {2, 0, 4, 4}, {0, 2, 2, 4}, {1, 0, 2, 2}, {0, 1, 1, 2}};
    static const Pass whole[1] = {{0, 0, 1, 1}};
    const Pass* passes = header.interlaced ? adam7 : whole;
    const int pass_count = header.interlaced ? 7 : 1;

    size_t expected = 0;
    for (int p = 0; p < pass_count; ++p) {
        const uint32_t columns = (header.width + passes[p].dx - 1 - passes[p].x0) / passes[p].dx;
        const uint32_t rows = (header.height + passes[p].dy - 1 - passes[p].y0) / passes[p].dy;
        if (columns == 0 || rows == 0) continue;
        expected += static_cast<size_t>(rows) * (1 + header.row_bytes(columns));
    }

    std::vector<uint8_t> raw(expected);
    const size_t unpacked = tinfl_decompress_mem_to_mem(raw.data(), raw.size(), packed.data(), packed.size(), TINFL_FLAG_PARSE_ZLIB_HEADER);
    if (unpacked == TINFL_DECOMPRESS_MEM_TO_MEM_FAILED || unpacked != expected) {
        result.diagnostics.error("PNG", "has damaged or incomplete picture data");
        return result;
    }

    result.image.width = static_cast<int>(header.width);
    result.image.height = static_cast<int>(header.height);
    result.image.rgba.assign(static_cast<size_t>(header.width) * header.height * 4, 0);

    size_t at = 0;
    for (int p = 0; p < pass_count; ++p) {
        const uint32_t columns = (header.width + passes[p].dx - 1 - passes[p].x0) / passes[p].dx;
        const uint32_t rows = (header.height + passes[p].dy - 1 - passes[p].y0) / passes[p].dy;
        if (columns == 0 || rows == 0) continue;
        const size_t bytes = header.row_bytes(columns);
        const uint8_t* previous = nullptr;
        for (uint32_t r = 0; r < rows; ++r) {
            const int filter = raw[at];
            uint8_t* row = &raw[at + 1];
            if (!unfilter(filter, row, previous, bytes, header.stride())) {
                result.image = Image();
                result.diagnostics.error("PNG", "uses a row filter that PNG does not define");
                return result;
            }
            store_row(header, palette, key, row, columns, result.image, passes[p].y0 + r * passes[p].dy, passes[p].x0, passes[p].dx);
            previous = row;
            at += bytes + 1;
        }
    }
    result.ok = true;
    return result;
}

ImageResult load_wheel_image(const Archive& gdtf, const WheelSlot& slot) {
    ImageResult result;
    if (slot.media_file.empty()) return result;
    std::vector<uint8_t> bytes;
    // Some files give the name with its extension, against the letter of the specification.
    if (!gdtf.read("wheels/" + slot.media_file + ".png", bytes) && !gdtf.read("wheels/" + slot.media_file, bytes)) {
        result.diagnostics.warn("Slot '" + slot.name + "'", "names the picture '" + slot.media_file + "', which is not in the archive's wheels folder");
        return result;
    }
    result = parse_png(bytes.data(), bytes.size());
    for (Diagnostic& item : result.diagnostics.items) item.where = "wheels/" + slot.media_file + ".png";
    return result;
}

Image gobo_transmission(const Image& picture) {
    Image out;
    out.width = picture.width;
    out.height = picture.height;
    out.rgba.resize(picture.rgba.size());
    for (size_t i = 0; i + 3 < picture.rgba.size(); i += 4) {
        const unsigned alpha = picture.rgba[i + 3];
        // The values are sRGB-encoded shares of light; the holder's share is none. Scaling the
        // encoded value by alpha is not exact for a half-transparent edge pixel, but those only
        // smooth the outline.
        for (size_t c = 0; c < 3; ++c) out.rgba[i + c] = static_cast<uint8_t>((picture.rgba[i + c] * alpha + 127u) / 255u);
        out.rgba[i + 3] = 255;
    }
    return out;
}

double mean_transmission(const Image& transmission) {
    if (transmission.empty()) return 1.0;
    double table[256];
    for (int v = 0; v < 256; ++v) table[v] = srgb_to_linear(v / 255.0);
    const double cx = transmission.width * 0.5;
    const double cy = transmission.height * 0.5;
    double sum = 0.0;
    size_t counted = 0;
    for (int y = 0; y < transmission.height; ++y) {
        for (int x = 0; x < transmission.width; ++x) {
            const double u = (x + 0.5 - cx) / cx;
            const double v = (y + 0.5 - cy) / cy;
            if (u * u + v * v > 1.0) continue;
            const uint8_t* pixel = &transmission.rgba[(static_cast<size_t>(y) * static_cast<size_t>(transmission.width) + static_cast<size_t>(x)) * 4];
            sum += (table[pixel[0]] + table[pixel[1]] + table[pixel[2]]) / 3.0;
            ++counted;
        }
    }
    return counted ? sum / static_cast<double>(counted) : 1.0;
}

}  // namespace viz
