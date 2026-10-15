#include "vizcore/screens.h"

#include <algorithm>
#include <cmath>
#include <map>

#include "vizcore/archive.h"
#include "vizcore/mesh.h"

namespace viz {

namespace {

double length_of(const double v[3]) { return std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); }

}  // namespace

bool screen_face(const Transform& placed, const double low[3], const double high[3], ScreenFace& out) {
    const double* axes[3] = {placed.u, placed.v, placed.w};
    double size[3], unit[3][3];
    for (int axis = 0; axis < 3; ++axis) {
        const double stretch = length_of(axes[axis]);
        size[axis] = (high[axis] - low[axis]) * stretch;
        if (!std::isfinite(size[axis]) || size[axis] < 0.0 || stretch <= 0.0) return false;
        for (int k = 0; k < 3; ++k) unit[axis][k] = axes[axis][k] / stretch;
    }
    int thin = 0;
    for (int axis = 1; axis < 3; ++axis) {
        if (size[axis] < size[thin]) thin = axis;
    }
    const int first = (thin + 1) % 3, second = (thin + 2) % 3;
    if (size[first] < 10.0 || size[second] < 10.0) return false;    // a line or a point, not a face

    // Up the picture: whichever edge points more nearly up; for a screen lying flat, upstage.
    const bool flat = std::fabs(unit[thin][2]) > 0.9;
    const int rise = flat ? 1 : 2;
    const int up = std::fabs(unit[first][rise]) >= std::fabs(unit[second][rise]) ? first : second;
    const int across = up == first ? second : first;
    double v[3], w[3];
    const double v_sign = unit[up][rise] < 0.0 ? -1.0 : 1.0;
    for (int k = 0; k < 3; ++k) v[k] = unit[up][k] * v_sign;

    // The middle of the box, in the world.
    double middle[3];
    for (int k = 0; k < 3; ++k) {
        middle[k] = placed.o[k];
        for (int axis = 0; axis < 3; ++axis) middle[k] += axes[axis][k] * (low[axis] + high[axis]) / 2.0;
    }

    // Which side it is looked at from.
    double w_sign;
    if (flat) {
        w_sign = (middle[2] > 4000.0) == (unit[thin][2] > 0.0) ? -1.0 : 1.0;    // up from the floor, down from the roof
    } else if (std::fabs(unit[thin][1]) > 0.05) {
        w_sign = unit[thin][1] > 0.0 ? -1.0 : 1.0;                              // toward the house, which is at -Y
    } else {
        w_sign = (unit[thin][0] > 0.0) == (middle[0] > 0.0) ? -1.0 : 1.0;       // edge-on to the house: toward the centre line
    }
    for (int k = 0; k < 3; ++k) w[k] = unit[thin][k] * w_sign;

    // To the right as whoever looks sees it: up crossed with the direction toward them.
    const double u[3] = {v[1] * w[2] - v[2] * w[1], v[2] * w[0] - v[0] * w[2], v[0] * w[1] - v[1] * w[0]};
    for (int k = 0; k < 3; ++k) {
        out.world.u[k] = u[k];
        out.world.v[k] = v[k];
        out.world.w[k] = w[k];
        out.world.o[k] = middle[k] + w[k] * size[thin] / 2.0;    // on the face, not inside the slab
    }
    out.width = size[across];
    out.height = size[up];
    return true;
}

ScreenFace standing_screen(double x, double y, double z, double width, double height, double turn_degrees) {
    const double turn = turn_degrees * 3.14159265358979323846 / 180.0;
    const double c = std::cos(turn), s = std::sin(turn);
    ScreenFace face;
    face.world.u[0] = c;
    face.world.u[1] = s;
    face.world.u[2] = 0.0;
    face.world.v[0] = 0.0;
    face.world.v[1] = 0.0;
    face.world.v[2] = 1.0;
    face.world.w[0] = s;
    face.world.w[1] = -c;
    face.world.w[2] = 0.0;
    face.world.o[0] = x;
    face.world.o[1] = y;
    face.world.o[2] = z + height / 2.0;
    face.width = width;
    face.height = height;
    return face;
}

std::vector<ScreenFace> screen_faces(const MvrScene& scene, const Archive& archive) {
    std::vector<ScreenFace> faces;
    std::map<std::string, MeshResult> shapes;    // by file name: only their boxes are wanted
    for (const MvrObject& object : scene.objects) {
        if (object.kind != MvrObjectKind::VideoScreen) continue;
        // The box all its models fill together, in the screen's own space.
        double low[3] = {0, 0, 0}, high[3] = {0, 0, 0};
        bool any = false;
        for (const MvrGeometry& geometry : object.geometry) {
            auto found = shapes.find(geometry.file);
            if (found == shapes.end()) {
                MeshResult shape = load_scene_mesh(archive, geometry.file);
                shape.mesh.positions.clear();
                shape.mesh.normals.clear();
                shape.mesh.triangles.clear();
                found = shapes.emplace(geometry.file, std::move(shape)).first;
            }
            if (!found->second.ok) continue;
            const Mesh& mesh = found->second.mesh;
            for (int corner = 0; corner < 8; ++corner) {
                // Model files are in metres; an MVR's placings in millimetres.
                const double at[3] = {static_cast<double>(corner & 1 ? mesh.max[0] : mesh.min[0]) * 1000.0,
                                      static_cast<double>(corner & 2 ? mesh.max[1] : mesh.min[1]) * 1000.0,
                                      static_cast<double>(corner & 4 ? mesh.max[2] : mesh.min[2]) * 1000.0};
                for (int k = 0; k < 3; ++k) {
                    const double value = geometry.local.u[k] * at[0] + geometry.local.v[k] * at[1] + geometry.local.w[k] * at[2] + geometry.local.o[k];
                    if (!any || value < low[k]) low[k] = value;
                    if (!any || value > high[k]) high[k] = value;
                }
                any = true;
            }
        }
        ScreenFace face;
        if (!any || !screen_face(object.world, low, high, face)) continue;
        face.uuid = object.uuid;
        face.name = object.name;
        face.layer = object.layer;
        face.parent_fixture = object.parent_fixture;
        face.child_position = object.child_position;
        face.class_name = scene.class_name(object.classing);
        face.sources = object.sources;
        faces.push_back(std::move(face));
    }
    return faces;
}

std::vector<PanelWall> find_walls(const std::vector<WallPanel>& panels) {
    struct Box {
        double low[3], high[3];
    };
    // Panels that could share a wall: one type, one plane, edges square to the first of them.
    struct Plane {
        size_t first;                       // the panel whose frame the boxes are measured in
        double unit[3][3];                  // that panel's axes, unit length
        int thin;
        std::vector<size_t> members;
        std::vector<Box> boxes;             // each member's box along `unit`, from the first's origin
    };
    std::vector<Plane> planes;

    for (size_t index = 0; index < panels.size(); ++index) {
        const WallPanel& panel = panels[index];
        const double* axes[3] = {panel.world.u, panel.world.v, panel.world.w};
        double size[3], unit[3][3];
        bool sound = true;
        for (int axis = 0; axis < 3; ++axis) {
            const double stretch = length_of(axes[axis]);
            size[axis] = (panel.high[axis] - panel.low[axis]) * stretch;
            if (!(stretch > 0.0) || !std::isfinite(size[axis]) || size[axis] < 0.0) sound = false;
            for (int k = 0; k < 3; ++k) unit[axis][k] = sound ? axes[axis][k] / stretch : 0.0;
        }
        if (!sound) continue;
        int thin = 0;
        for (int axis = 1; axis < 3; ++axis) {
            if (size[axis] < size[thin]) thin = axis;
        }
        const double narrow = std::min(size[(thin + 1) % 3], size[(thin + 2) % 3]);
        if (narrow < 100.0 || size[thin] > 0.25 * narrow) continue;    // not flat: a moving head, a batten

        // Its eight corners in the world.
        double corner[8][3];
        for (int c = 0; c < 8; ++c) {
            const double local[3] = {c & 1 ? panel.high[0] : panel.low[0], c & 2 ? panel.high[1] : panel.low[1], c & 4 ? panel.high[2] : panel.low[2]};
            for (int k = 0; k < 3; ++k) corner[c][k] = panel.world.o[k] + axes[0][k] * local[0] + axes[1][k] * local[1] + axes[2][k] * local[2];
        }
        Plane* home = nullptr;
        Box box{};
        for (Plane& plane : planes) {
            const WallPanel& first = panels[plane.first];
            if (first.type != panel.type) continue;
            bool square = true;    // every edge of this panel runs along an edge of the first
            for (int axis = 0; axis < 3 && square; ++axis) {
                double best = 0.0;
                for (int other = 0; other < 3; ++other) {
                    best = std::max(best, std::fabs(unit[axis][0] * plane.unit[other][0] + unit[axis][1] * plane.unit[other][1] + unit[axis][2] * plane.unit[other][2]));
                }
                square = best > 0.999;
            }
            if (!square) continue;
            for (int k = 0; k < 3; ++k) {
                box.low[k] = 1e300;
                box.high[k] = -1e300;
            }
            for (int c = 0; c < 8; ++c) {
                for (int axis = 0; axis < 3; ++axis) {
                    const double along = (corner[c][0] - first.world.o[0]) * plane.unit[axis][0] + (corner[c][1] - first.world.o[1]) * plane.unit[axis][1] +
                                         (corner[c][2] - first.world.o[2]) * plane.unit[axis][2];
                    box.low[axis] = std::min(box.low[axis], along);
                    box.high[axis] = std::max(box.high[axis], along);
                }
            }
            const Box& theirs = plane.boxes.front();
            const bool level = std::fabs((box.low[plane.thin] + box.high[plane.thin]) - (theirs.low[plane.thin] + theirs.high[plane.thin])) / 2.0 <= 20.0 &&
                               std::fabs((box.high[plane.thin] - box.low[plane.thin]) - (theirs.high[plane.thin] - theirs.low[plane.thin])) <= 20.0;
            if (level) {
                home = &plane;
                break;
            }
        }
        if (!home) {
            Plane plane;
            plane.first = index;
            plane.thin = thin;
            for (int axis = 0; axis < 3; ++axis) {
                for (int k = 0; k < 3; ++k) plane.unit[axis][k] = unit[axis][k];
                box.low[axis] = panel.low[axis] * length_of(axes[axis]);
                box.high[axis] = panel.high[axis] * length_of(axes[axis]);
            }
            planes.push_back(std::move(plane));
            home = &planes.back();
        }
        home->members.push_back(index);
        home->boxes.push_back(box);
    }

    std::vector<PanelWall> walls;
    for (const Plane& plane : planes) {
        // Which of them touch, directly or through others.
        const size_t count = plane.members.size();
        std::vector<size_t> group(count);
        for (size_t i = 0; i < count; ++i) group[i] = i;
        const int first = (plane.thin + 1) % 3, second = (plane.thin + 2) % 3;
        const double reach = 25.0;
        bool merged = true;
        while (merged) {
            merged = false;
            for (size_t i = 0; i < count; ++i) {
                for (size_t j = i + 1; j < count; ++j) {
                    if (group[i] == group[j]) continue;
                    const Box& a = plane.boxes[i];
                    const Box& b = plane.boxes[j];
                    if (a.low[first] - reach <= b.high[first] && b.low[first] - reach <= a.high[first] && a.low[second] - reach <= b.high[second] &&
                        b.low[second] - reach <= a.high[second]) {
                        const size_t from = std::max(group[i], group[j]), to = std::min(group[i], group[j]);
                        for (size_t& entry : group) {
                            if (entry == from) entry = to;
                        }
                        merged = true;
                    }
                }
            }
        }
        for (size_t leader = 0; leader < count; ++leader) {
            if (group[leader] != leader) continue;
            PanelWall wall;
            wall.type = panels[plane.first].type;
            double low[3] = {1e300, 1e300, 1e300}, high[3] = {-1e300, -1e300, -1e300};
            double toward[3] = {0, 0, 0};
            for (size_t i = 0; i < count; ++i) {
                if (group[i] != leader) continue;
                wall.panels.push_back(plane.members[i]);
                for (int axis = 0; axis < 3; ++axis) {
                    low[axis] = std::min(low[axis], plane.boxes[i].low[axis]);
                    high[axis] = std::max(high[axis], plane.boxes[i].high[axis]);
                }
                const WallPanel& panel = panels[plane.members[i]];
                if (panel.lit) {
                    for (int k = 0; k < 3; ++k) toward[k] += panel.toward[k];
                }
            }
            Transform placed;
            for (int k = 0; k < 3; ++k) {
                placed.u[k] = plane.unit[0][k];
                placed.v[k] = plane.unit[1][k];
                placed.w[k] = plane.unit[2][k];
                placed.o[k] = panels[plane.first].world.o[k];
            }
            if (!screen_face(placed, low, high, wall.face)) continue;
            // The picture belongs on the side the light leaves from.
            Transform& face = wall.face.world;
            if (toward[0] * face.w[0] + toward[1] * face.w[1] + toward[2] * face.w[2] < -1e-6) {
                const double thick = high[plane.thin] - low[plane.thin];
                for (int k = 0; k < 3; ++k) {
                    face.o[k] -= face.w[k] * thick;
                    face.w[k] = -face.w[k];
                    face.u[k] = -face.u[k];
                }
            }
            walls.push_back(std::move(wall));
        }
    }
    std::sort(walls.begin(), walls.end(), [](const PanelWall& a, const PanelWall& b) { return a.panels.front() < b.panels.front(); });
    return walls;
}

int PixelMap::universes() const {
    const int each = slots_per_pixel();
    if (each <= 0 || pixels() <= 0) return 0;
    const int first = (512 - (address - 1)) / each;    // how many fit in the first universe
    if (pixels() <= first) return 1;
    const int full = 512 / each;
    return 1 + (pixels() - first + full - 1) / full;
}

namespace {

// A whole number from the front of `text`, which is moved past it. False if there is none or it is too long.
bool take_number(std::string_view& text, int& out) {
    size_t digits = 0;
    long value = 0;
    while (digits < text.size() && text[digits] >= '0' && text[digits] <= '9' && digits < 7) {
        value = value * 10 + (text[digits] - '0');
        ++digits;
    }
    if (digits == 0 || (digits < text.size() && text[digits] >= '0' && text[digits] <= '9')) return false;
    out = static_cast<int>(value);
    text.remove_prefix(digits);
    return true;
}

std::string_view take_word(std::string_view& text) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) text.remove_prefix(1);
    size_t end = 0;
    while (end < text.size() && text[end] != ' ' && text[end] != '\t') ++end;
    std::string_view word = text.substr(0, end);
    text.remove_prefix(end);
    return word;
}

}  // namespace

bool parse_pixel_map(std::string_view text, PixelMap& out, std::string& error) {
    PixelMap map;
    std::string_view where = take_word(text);
    if (!take_number(where, map.universe) || where.empty() || where.front() != '.' || (where.remove_prefix(1), !take_number(where, map.address)) ||
        !where.empty()) {
        error = "a pixel map starts with where its first pixel is patched, as universe.address";
        return false;
    }
    if (map.universe < 1 || map.universe > 32767 || map.address < 1 || map.address > 512) {
        error = "the universe is 1 to 32767 and the address 1 to 512";
        return false;
    }
    std::string_view size = take_word(text);
    if (!take_number(size, map.across) || size.empty() || (size.front() != 'x' && size.front() != 'X') || (size.remove_prefix(1), !take_number(size, map.down)) ||
        !size.empty()) {
        error = "after the address comes the number of pixels, as across x down";
        return false;
    }
    if (map.across < 1 || map.down < 1 || map.across > 1024 || map.down > 1024 || map.across * map.down > 262144) {
        error = "a pixel map has 1 to 1024 pixels each way and no more than 262144 in all";
        return false;
    }
    for (std::string_view word = take_word(text); !word.empty(); word = take_word(text)) {
        std::string lower(word);
        for (char& ch : lower) ch = static_cast<char>(ch >= 'A' && ch <= 'Z' ? ch - 'A' + 'a' : ch);
        if (lower == "rows") {
            map.wiring = PixelMap::Wiring::Rows;
        } else if (lower == "snake") {
            map.wiring = PixelMap::Wiring::Snake;
        } else if (lower == "columns") {
            map.wiring = PixelMap::Wiring::Columns;
        } else if (lower == "snake-columns") {
            map.wiring = PixelMap::Wiring::SnakeColumns;
        } else {
            // An order: each of R, G and B once, and at most one W.
            int seen[4] = {0, 0, 0, 0};
            bool letters = lower.size() >= 3 && lower.size() <= 4;
            for (char ch : lower) {
                const int which = ch == 'r' ? 0 : ch == 'g' ? 1 : ch == 'b' ? 2 : ch == 'w' ? 3 : -1;
                if (which < 0 || seen[which]++) letters = false;
            }
            if (!letters || !seen[0] || !seen[1] || !seen[2]) {
                error = "\"" + std::string(word) + "\" is neither an order of colours (such as RGB, GRB or RGBW) nor a wiring (rows, snake, columns, snake-columns)";
                return false;
            }
            map.order.clear();
            for (char ch : lower) map.order.push_back(static_cast<char>(ch - 'a' + 'A'));
        }
    }
    if (map.universe + map.universes() - 1 > 32767) {
        error = "the pixels run past the last universe there is";
        return false;
    }
    out = map;
    return true;
}

PixelPlace pixel_place(const PixelMap& map, int column, int row) {
    int index = 0;
    switch (map.wiring) {
        case PixelMap::Wiring::Rows: index = row * map.across + column; break;
        case PixelMap::Wiring::Snake: index = row * map.across + (row % 2 ? map.across - 1 - column : column); break;
        case PixelMap::Wiring::Columns: index = column * map.down + row; break;
        case PixelMap::Wiring::SnakeColumns: index = column * map.down + (column % 2 ? map.down - 1 - row : row); break;
    }
    const int each = map.slots_per_pixel();
    const int first = (512 - (map.address - 1)) / each;
    if (index < first) return {map.universe, map.address - 1 + index * each};
    const int full = 512 / each;
    return {map.universe + 1 + (index - first) / full, (index - first) % full * each};
}

int read_pixels(const PixelMap& map, UniverseLookup lookup, void* context, std::vector<uint8_t>& rgba) {
    rgba.assign(static_cast<size_t>(map.pixels()) * 4, 0);
    const int count = map.universes();
    std::vector<const uint8_t*> slots(static_cast<size_t>(count), nullptr);
    int arriving = 0;
    for (int i = 0; i < count; ++i) {
        slots[static_cast<size_t>(i)] = lookup ? lookup(map.universe + i, context) : nullptr;
        arriving += slots[static_cast<size_t>(i)] ? 1 : 0;
    }
    const int each = map.slots_per_pixel();
    for (int row = 0; row < map.down; ++row) {
        for (int column = 0; column < map.across; ++column) {
            uint8_t* pixel = &rgba[(static_cast<size_t>(row) * static_cast<size_t>(map.across) + static_cast<size_t>(column)) * 4];
            pixel[3] = 255;
            const PixelPlace place = pixel_place(map, column, row);
            const uint8_t* from = slots[static_cast<size_t>(place.universe - map.universe)];
            if (!from) continue;
            int level[3] = {0, 0, 0};
            for (int k = 0; k < each; ++k) {
                const int value = from[place.slot + k];
                switch (map.order[static_cast<size_t>(k)]) {
                    case 'R': level[0] += value; break;
                    case 'G': level[1] += value; break;
                    case 'B': level[2] += value; break;
                    default: level[0] += value; level[1] += value; level[2] += value; break;    // white
                }
            }
            for (int c = 0; c < 3; ++c) pixel[c] = static_cast<uint8_t>(level[c] > 255 ? 255 : level[c]);
        }
    }
    return arriving;
}

}  // namespace viz
