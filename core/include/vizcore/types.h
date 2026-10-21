// Shared value types for the visualizer core. No Unreal headers, no exceptions.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// Marks what other Unreal modules may call. In the standalone build it is empty. Unreal's build
// defines it as DLLEXPORT or DLLIMPORT, macros its platform headers provide; the core is compiled
// without those headers, so they are supplied here when missing.
#ifndef VIZCORE_API
#define VIZCORE_API
#elif !defined(DLLEXPORT)
#if defined(_MSC_VER)
#define DLLEXPORT __declspec(dllexport)
#define DLLIMPORT __declspec(dllimport)
#else
#define DLLEXPORT __attribute__((visibility("default")))
#define DLLIMPORT __attribute__((visibility("default")))
#endif
#endif

namespace viz {

enum class Severity { Warning, Error };

struct Diagnostic {
    Severity severity;
    std::string where;    // path to the offending node, e.g. "DMXMode 'Standard' / DMXChannel 'Head_Pan'"
    std::string message;
};

// Everything a parser had to say about a file. An Error means the result is unusable or a part of
// it was dropped; a Warning means a default or a repair was applied.
struct VIZCORE_API Diagnostics {
    std::vector<Diagnostic> items;

    void warn(std::string where, std::string message);
    void error(std::string where, std::string message);
    bool has_errors() const;
    size_t count(Severity severity) const;
};

// CIE 1931 xyY. The default is the white point GDTF uses for unspecified colours.
struct ColorCIE {
    double x = 0.3127;
    double y = 0.3290;
    double Y = 100.0;
};

// A DMX value as GDTF writes it: "<value>/<bytes>", with a trailing "s" selecting byte shifting
// instead of byte mirroring when it is converted to another resolution.
struct DmxValue {
    uint32_t value = 0;
    int bytes = 1;
    bool shift = false;
};

VIZCORE_API bool parse_dmx_value(std::string_view text, DmxValue& out);

// Largest value an n-byte channel can carry; n is clamped to 1-4.
VIZCORE_API uint32_t dmx_max(int bytes);

// Converts to a channel's resolution. Widening mirrors the bytes (255/1 becomes 65535 on a 16-bit
// channel) unless the value asks for shifting (255/1s becomes 65280). Narrowing drops low bytes.
VIZCORE_API uint32_t convert_dmx_value(const DmxValue& value, int target_bytes);

// GDTF 4x4 matrix, row-major as written in the file: rotation in the upper-left 3x3, translation
// in the last column, metres. Right-handed, Z up. A point transforms as p' = M * p.
struct Matrix4 {
    double m[4][4] = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {0, 0, 0, 1}};
};

// MVR 4x3 matrix: three basis vectors and an origin, millimetres. Right-handed, Z up.
// A local point (x, y, z) sits at x*u + y*v + z*w + o in the parent's space.
struct Transform {
    double u[3] = {1, 0, 0};
    double v[3] = {0, 1, 0};
    double w[3] = {0, 0, 1};
    double o[3] = {0, 0, 0};
};

// The transform that applies `child` first and then `parent`.
VIZCORE_API Transform compose(const Transform& parent, const Transform& child);

// The transform that undoes `transform`. False, leaving `out` alone, if it flattens space and so
// cannot be undone.
VIZCORE_API bool invert(const Transform& transform, Transform& out);

// A placing's turn as three angles in degrees: about the world's X axis first, then its Y, then
// its Z (the matrix is Rz * Ry * Rx), which is how a console's or a drawing program's "Rot X, Rot
// Y, Rot Z" read. Two sets of angles give any one turn; the one with the least turning is given,
// so that a fixture standing on the floor reads 180 about one axis and not 180 about the two
// others. Angles are between -180 and 180. Scale is not carried: `set_turn_angles` leaves the
// axes one unit long and the origin where it was.
VIZCORE_API void turn_angles(const Transform& transform, double degrees[3]);
VIZCORE_API void set_turn_angles(Transform& transform, const double degrees[3]);

// How a dimmer channel's level becomes light. A file's dimmer runs straight from dark to full;
// a dimmer rack, or a console's profile, often bends that: "square" (square law: half the level
// gives a quarter of the light, as many racks do for incandescent lamps), "inverse-square" (the
// other way: brighter low down), "s-curve" (gentle at both ends). A type in a show can be given one.
enum class DimmerCurve { Linear, Square, InverseSquare, SCurve };
// The light (0 to 1) for a level (0 to 1) under the curve.
VIZCORE_API double dimmer_curve(DimmerCurve curve, double level);
// "linear", "square", "inverse-square", "s-curve", as the types file and the inspector write them.
VIZCORE_API const char* dimmer_curve_name(DimmerCurve curve);
VIZCORE_API bool parse_dimmer_curve(std::string_view name, DimmerCurve& out);

}  // namespace viz
