#include "vizcore/types.h"

#include <cmath>
#include <utility>

#include "text.h"

namespace viz {

void Diagnostics::warn(std::string where, std::string message) {
    items.push_back({Severity::Warning, std::move(where), std::move(message)});
}

void Diagnostics::error(std::string where, std::string message) {
    items.push_back({Severity::Error, std::move(where), std::move(message)});
}

bool Diagnostics::has_errors() const { return count(Severity::Error) != 0; }

size_t Diagnostics::count(Severity severity) const {
    size_t n = 0;
    for (const Diagnostic& item : items) n += item.severity == severity;
    return n;
}

uint32_t dmx_max(int bytes) {
    if (bytes <= 1) return 0xFFu;
    if (bytes == 2) return 0xFFFFu;
    if (bytes == 3) return 0xFFFFFFu;
    return 0xFFFFFFFFu;
}

bool parse_dmx_value(std::string_view text, DmxValue& out) {
    text = text::trim(text);
    size_t slash = text.find('/');
    if (slash == std::string_view::npos) return false;
    std::string_view bytes_part = text.substr(slash + 1);
    bool shift = false;
    if (!bytes_part.empty() && (bytes_part.back() == 's' || bytes_part.back() == 'S')) {
        shift = true;
        bytes_part.remove_suffix(1);
    }
    long long value = 0;
    long long bytes = 0;
    if (!text::parse_int(text.substr(0, slash), value) || !text::parse_int(bytes_part, bytes)) return false;
    if (bytes < 1 || bytes > 4 || value < 0 || value > static_cast<long long>(dmx_max(static_cast<int>(bytes)))) {
        return false;
    }
    out.value = static_cast<uint32_t>(value);
    out.bytes = static_cast<int>(bytes);
    out.shift = shift;
    return true;
}

uint32_t convert_dmx_value(const DmxValue& value, int target_bytes) {
    int source = value.bytes < 1 ? 1 : (value.bytes > 4 ? 4 : value.bytes);
    int target = target_bytes < 1 ? 1 : (target_bytes > 4 ? 4 : target_bytes);
    uint32_t v = value.value & dmx_max(source);
    if (target == source) return v;
    if (target < source) return v >> (8 * (source - target));
    if (value.shift) return v << (8 * (target - source));
    // Mirroring: repeat the source bytes, most significant first, until the target is full.
    uint32_t result = 0;
    for (int i = 0; i < target; ++i) {
        int source_byte = i % source;
        uint32_t byte = (v >> (8 * (source - 1 - source_byte))) & 0xFFu;
        result = (result << 8) | byte;
    }
    return result;
}

Transform compose(const Transform& parent, const Transform& child) {
    auto direction = [&parent](const double in[3], double out[3]) {
        for (int k = 0; k < 3; ++k) out[k] = in[0] * parent.u[k] + in[1] * parent.v[k] + in[2] * parent.w[k];
    };
    Transform result;
    direction(child.u, result.u);
    direction(child.v, result.v);
    direction(child.w, result.w);
    direction(child.o, result.o);
    for (int k = 0; k < 3; ++k) result.o[k] += parent.o[k];
    return result;
}

namespace {

const double kPi = 3.14159265358979323846;

double wrapped(double degrees) {
    while (degrees > 180.0) degrees -= 360.0;
    while (degrees <= -180.0) degrees += 360.0;
    return degrees;
}

}  // namespace

void turn_angles(const Transform& transform, double degrees[3]) {
    // With R = Rz * Ry * Rx acting on column vectors, the object's X axis (row u) is R's first
    // column: (cz cy, sz cy, -sy); its Y and Z axes end in cy sx and cy cx.
    double u[3], v[3], w[3];
    auto unit = [](const double in[3], double out[3], double fallback_x, double fallback_y, double fallback_z) {
        const double length = std::sqrt(in[0] * in[0] + in[1] * in[1] + in[2] * in[2]);
        if (length > 1e-12) {
            for (int k = 0; k < 3; ++k) out[k] = in[k] / length;
        } else {
            out[0] = fallback_x;
            out[1] = fallback_y;
            out[2] = fallback_z;
        }
    };
    unit(transform.u, u, 1, 0, 0);
    unit(transform.v, v, 0, 1, 0);
    unit(transform.w, w, 0, 0, 1);
    double x, y, z;
    const double sy = -u[2];
    if (sy > 0.999999 || sy < -0.999999) {
        // Looking straight up or down the Y turn, X and Z turn about the same line: all of it is given to X.
        y = sy > 0 ? 90.0 : -90.0;
        z = 0.0;
        x = std::atan2(sy > 0 ? v[0] : -v[0], v[1]) * 180.0 / kPi;
    } else {
        y = std::asin(sy) * 180.0 / kPi;
        x = std::atan2(v[2], w[2]) * 180.0 / kPi;
        z = std::atan2(u[1], u[0]) * 180.0 / kPi;
    }
    // The same turn the other way round: (x + 180, 180 - y, z + 180).
    const double other[3] = {wrapped(x + 180.0), wrapped(180.0 - y), wrapped(z + 180.0)};
    auto size = [](double a, double b, double c) { return std::fabs(a) + std::fabs(b) + std::fabs(c); };
    if (size(other[0], other[1], other[2]) + 1e-6 < size(x, y, z)) {
        x = other[0];
        y = other[1];
        z = other[2];
    }
    auto tidy = [](double value) {
        const double rounded = std::round(value * 1e6) / 1e6;
        return rounded == 0.0 ? 0.0 : (rounded == -180.0 ? 180.0 : rounded);    // no "-0", and one name for the half turn
    };
    degrees[0] = tidy(x);
    degrees[1] = tidy(y);
    degrees[2] = tidy(z);
}

void set_turn_angles(Transform& transform, const double degrees[3]) {
    const double cx = std::cos(degrees[0] * kPi / 180.0), sx = std::sin(degrees[0] * kPi / 180.0);
    const double cy = std::cos(degrees[1] * kPi / 180.0), sy = std::sin(degrees[1] * kPi / 180.0);
    const double cz = std::cos(degrees[2] * kPi / 180.0), sz = std::sin(degrees[2] * kPi / 180.0);
    const double u[3] = {cz * cy, sz * cy, -sy};
    const double v[3] = {cz * sy * sx - sz * cx, sz * sy * sx + cz * cx, cy * sx};
    const double w[3] = {cz * sy * cx + sz * sx, sz * sy * cx - cz * sx, cy * cx};
    for (int k = 0; k < 3; ++k) {
        // Whole turns should come out as whole numbers: -1.2e-16 has no business in a show file.
        auto clean = [](double value) { return std::fabs(value) < 1e-12 ? 0.0 : (std::fabs(std::fabs(value) - 1.0) < 1e-12 ? (value > 0 ? 1.0 : -1.0) : value); };
        transform.u[k] = clean(u[k]);
        transform.v[k] = clean(v[k]);
        transform.w[k] = clean(w[k]);
    }
}

bool invert(const Transform& transform, Transform& out) {
    // Rows u, v, w are where the local axes point; a local point p sits at p.x u + p.y v + p.z w + o.
    const double* u = transform.u;
    const double* v = transform.v;
    const double* w = transform.w;
    const double cross_vw[3] = {v[1] * w[2] - v[2] * w[1], v[2] * w[0] - v[0] * w[2], v[0] * w[1] - v[1] * w[0]};
    const double cross_wu[3] = {w[1] * u[2] - w[2] * u[1], w[2] * u[0] - w[0] * u[2], w[0] * u[1] - w[1] * u[0]};
    const double cross_uv[3] = {u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0]};
    const double determinant = u[0] * cross_vw[0] + u[1] * cross_vw[1] + u[2] * cross_vw[2];
    if (!(determinant > 1e-12 || determinant < -1e-12)) return false;
    // A world direction d has local coordinates (d . v x w, d . w x u, d . u x v) / determinant;
    // the inverse's rows are the images of the world axes.
    Transform result;
    double* rows[3] = {result.u, result.v, result.w};
    for (int axis = 0; axis < 3; ++axis) {
        rows[axis][0] = cross_vw[axis] / determinant;
        rows[axis][1] = cross_wu[axis] / determinant;
        rows[axis][2] = cross_uv[axis] / determinant;
    }
    for (int k = 0; k < 3; ++k) {
        result.o[k] = -(transform.o[0] * result.u[k] + transform.o[1] * result.v[k] + transform.o[2] * result.w[k]);
    }
    out = result;
    return true;
}

}  // namespace viz
