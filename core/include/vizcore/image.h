// The pictures packed in a GDTF's wheels folder: gobos, animation wheels. GDTF asks for PNG.
//
// Read: every pixel format PNG has (greyscale, RGB, palette, with or without alpha, 1 to 16 bits
// a channel), interlaced or not. What comes out is always 8 bits each of red, green, blue and
// alpha, rows from the top. Not read: colour profiles and gamma chunks; the values are taken as
// sRGB, which is GDTF's default. Checksums are not verified.
//
// How GDTF means a gobo picture to be read (Annex E of the specification): where the picture is
// fully transparent is the gobo's holder, which lets nothing through; elsewhere black blocks the
// light, white passes it and a colour tints it. gobo_transmission() turns a picture into exactly
// that: how much of red, green and blue gets through at each pixel.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "vizcore/archive.h"
#include "vizcore/gdtf.h"
#include "vizcore/types.h"

namespace viz {

struct Image {
    int width = 0;
    int height = 0;
    std::vector<uint8_t> rgba;    // width * height * 4, rows top to bottom, alpha not multiplied in

    bool empty() const { return rgba.empty(); }
};

struct ImageResult {
    bool ok = false;
    Image image;
    Diagnostics diagnostics;
};

// Pictures larger than this on a side are refused. GDTF allows 1024.
constexpr int kMaxImageSide = 4096;

VIZCORE_API ImageResult parse_png(const uint8_t* data, size_t size);

// The picture of a wheel slot: wheels/<MediaFileName>.png in the archive. Not ok, and without any
// diagnostic, if the slot names no picture (an open slot, a plain colour).
VIZCORE_API ImageResult load_wheel_image(const Archive& gdtf, const WheelSlot& slot);

// What a gobo picture lets through: red, green and blue are the share of light passed (still in
// sRGB encoding), alpha is 255 throughout. The holder around the gobo comes out black.
VIZCORE_API Image gobo_transmission(const Image& picture);

// The share of a gobo's light that gets through, 0 to 1, averaged over the circle the beam
// passes through and over the three colours, in linear light.
VIZCORE_API double mean_transmission(const Image& transmission);

// A picture as a PNG file (8 bits of red, green, blue and alpha). Empty if it cannot be made.
VIZCORE_API std::vector<uint8_t> encode_png(const Image& picture);

// A 360-degree picture from the six square views a camera takes with a lens 90 degrees wide,
// in this order: ahead, right, behind, left, up and down. Ahead, right, behind and left are
// level, each turned a quarter further to the right; up and down are turned from ahead, so
// that the top of the view looking up is behind and the top of the view looking down is ahead
// (as Unreal's camera turns: pitch then yaw, no roll). What comes out is equirectangular,
// `width` by width / 2: ahead in the middle, behind at both edges, right to the right, straight
// up along the top. Each pixel is taken from the view that sees it most squarely, between its
// four nearest pixels. Empty if the six are not squares of one size, or `width` is not 64 to 16384.
VIZCORE_API Image equirect_from_faces(const Image faces[6], int width);

}  // namespace viz
