#include "vizcore/pieces.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iterator>

#include "json.h"
#include "text.h"
#include "vizcore/files.h"

namespace viz {

namespace {

struct Vec {
    double x, y, z;
};
Vec operator+(Vec a, Vec b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
Vec operator-(Vec a, Vec b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
Vec operator*(Vec a, double s) { return {a.x * s, a.y * s, a.z * s}; }
Vec cross(Vec a, Vec b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
double length(Vec a) { return std::sqrt(a.x * a.x + a.y * a.y + a.z * a.z); }
Vec unit(Vec a) {
    const double l = length(a);
    return l > 1e-12 ? a * (1.0 / l) : Vec{0, 0, 1};
}

// One flat face of four corners, anticlockwise seen from outside: its own four vertices, so that its edges stay sharp.
void face(Mesh& mesh, Vec a, Vec b, Vec c, Vec d) {
    const Vec normal = unit(cross(b - a, c - a));
    const uint32_t first = static_cast<uint32_t>(mesh.vertex_count());
    for (const Vec& corner : {a, b, c, d}) {
        mesh.positions.insert(mesh.positions.end(), {static_cast<float>(corner.x), static_cast<float>(corner.y), static_cast<float>(corner.z)});
        mesh.normals.insert(mesh.normals.end(), {static_cast<float>(normal.x), static_cast<float>(normal.y), static_cast<float>(normal.z)});
    }
    mesh.triangles.insert(mesh.triangles.end(), {first, first + 1, first + 2, first, first + 2, first + 3});
}

// A prism between two quadrilaterals that face each other: `near` anticlockwise seen from outside
// (from in front of it), `far` the same corners moved along. Six faces.
void prism(Mesh& mesh, const Vec near[4], const Vec far[4]) {
    face(mesh, near[0], near[1], near[2], near[3]);
    face(mesh, far[3], far[2], far[1], far[0]);
    for (int k = 0; k < 4; ++k) {
        const int next = (k + 1) % 4;
        face(mesh, near[next], near[k], far[k], far[next]);
    }
}

// A box from `low` to `high`.
void box(Mesh& mesh, Vec low, Vec high) {
    const Vec near[4] = {{low.x, low.y, low.z}, {high.x, low.y, low.z}, {high.x, low.y, high.z}, {low.x, low.y, high.z}};    // the side toward -Y
    const Vec far[4] = {{low.x, high.y, low.z}, {high.x, high.y, low.z}, {high.x, high.y, high.z}, {low.x, high.y, high.z}};
    prism(mesh, near, far);
}

// A square tube of that thickness from one point to another.
void tube(Mesh& mesh, Vec from, Vec to, double thickness) {
    const Vec along = unit(to - from);
    const Vec aside = unit(cross(along, std::fabs(along.z) > 0.9 ? Vec{1, 0, 0} : Vec{0, 0, 1}));
    const Vec up = cross(aside, along);
    const double h = thickness * 0.5;
    // Anticlockwise for whoever stands before `from` and looks along the tube: that end's face then faces back at them.
    const Vec ring[4] = {aside * -h + up * -h, aside * h + up * -h, aside * h + up * h, aside * -h + up * h};
    Vec near[4], far[4];
    for (int k = 0; k < 4; ++k) {
        near[k] = from + ring[k];
        far[k] = to + ring[k];
    }
    prism(mesh, near, far);
}

// A round tube of that diameter from one point to another, as many sided as a pipe needs to look round at stage distances.
void rod(Mesh& mesh, Vec from, Vec to, double diameter, int sides = 12) {
    const Vec along = unit(to - from);
    const Vec aside = unit(cross(along, std::fabs(along.z) > 0.9 ? Vec{1, 0, 0} : Vec{0, 0, 1}));
    const Vec up = cross(aside, along);
    const double r = diameter * 0.5;
    const double step = 2.0 * 3.14159265358979323846 / sides;
    for (int k = 0; k < sides; ++k) {
        // Each side a flat strip: anticlockwise seen from outside.
        const Vec a = aside * (r * std::cos(step * k)) + up * (r * std::sin(step * k));
        const Vec b = aside * (r * std::cos(step * (k + 1))) + up * (r * std::sin(step * (k + 1)));
        face(mesh, from + b, from + a, to + a, to + b);
    }
    // The two ends, as fans about their middles.
    for (int end = 0; end < 2; ++end) {
        const Vec middle = end == 0 ? from : to;
        const Vec normal = end == 0 ? along * -1.0 : along;
        const uint32_t first = static_cast<uint32_t>(mesh.vertex_count());
        for (int k = 0; k < sides; ++k) {
            const Vec at = middle + aside * (r * std::cos(step * k)) + up * (r * std::sin(step * k));
            mesh.positions.insert(mesh.positions.end(), {static_cast<float>(at.x), static_cast<float>(at.y), static_cast<float>(at.z)});
            mesh.normals.insert(mesh.normals.end(), {static_cast<float>(normal.x), static_cast<float>(normal.y), static_cast<float>(normal.z)});
        }
        for (int k = 1; k + 1 < sides; ++k) {
            // The ring runs anticlockwise seen from before `from`: so the near end in its order, the far end against it.
            if (end == 0) {
                mesh.triangles.insert(mesh.triangles.end(), {first, first + static_cast<uint32_t>(k), first + static_cast<uint32_t>(k + 1)});
            } else {
                mesh.triangles.insert(mesh.triangles.end(), {first, first + static_cast<uint32_t>(k + 1), first + static_cast<uint32_t>(k)});
            }
        }
    }
}

double held(double value, double low, double high, double otherwise) {
    if (!(value == value)) return otherwise;    // not a number
    return std::min(std::max(value, low), high);
}

void finish(Mesh& mesh, const SetPiece& piece) {
    MeshMaterial material;
    material.name = piece.kind;
    for (int k = 0; k < 3; ++k) material.color[k] = std::min(std::max(piece.color[k], 0.0f), 1.0f);
    mesh.materials.push_back(material);
    mesh.triangle_material.assign(mesh.triangle_count(), 0);
    if (mesh.vertex_count() == 0) return;
    for (int k = 0; k < 3; ++k) mesh.min[k] = mesh.max[k] = mesh.positions[static_cast<size_t>(k)];
    for (size_t v = 0; v < mesh.vertex_count(); ++v) {
        for (int k = 0; k < 3; ++k) {
            mesh.min[k] = std::min(mesh.min[k], mesh.positions[v * 3 + static_cast<size_t>(k)]);
            mesh.max[k] = std::max(mesh.max[k], mesh.positions[v * 3 + static_cast<size_t>(k)]);
        }
    }
}

// Box truss along X: four chords, a square frame at every bay's end and one diagonal a side in
// every bay, leaning the other way from bay to bay. Its side is measured over the outside of the chords.
void truss(Mesh& mesh, double length_m, double side) {
    const double chord = std::min(std::max(side * 0.17, 0.02), 0.06);    // 5 cm tube on 29 cm truss
    const double brace = chord * 0.45;
    const double reach = (side - chord) * 0.5;    // from the middle to a chord's own middle
    const Vec corners[4] = {{0, -reach, chord * 0.5}, {0, reach, chord * 0.5}, {0, reach, side - chord * 0.5}, {0, -reach, side - chord * 0.5}};
    const double half = length_m * 0.5;
    for (const Vec& corner : corners) tube(mesh, {-half, corner.y, corner.z}, {half, corner.y, corner.z}, chord);
    const int bays = std::max(1, std::min(static_cast<int>(std::lround(length_m / side)), 400));
    const double bay = (length_m - chord) / bays;
    for (int b = 0; b <= bays; ++b) {
        const double x = -half + chord * 0.5 + bay * b;
        for (int k = 0; k < 4; ++k) {
            const Vec a = corners[k], c = corners[(k + 1) % 4];
            tube(mesh, {x, a.y, a.z}, {x, c.y, c.z}, brace);    // the frame at the bay's end
            if (b < bays) {
                const bool lean = (b + k) % 2 == 0;
                tube(mesh, {lean ? x : x + bay, a.y, a.z}, {lean ? x + bay : x, c.y, c.z}, brace);
            }
        }
    }
}

// Triangle truss along X: two bottom chords and one at the apex, a triangular frame at every bay's
// end and a diagonal on each of its three sides. `side` is its width over the bottom chords.
void triangle(Mesh& mesh, double length_m, double side) {
    const double chord = std::min(std::max(side * 0.17, 0.02), 0.06);
    const double brace = chord * 0.45;
    const double reach = (side - chord) * 0.5;
    const double rise = reach * std::sqrt(3.0);    // an equilateral triangle between the chords' middles
    const Vec corners[3] = {{0, -reach, chord * 0.5}, {0, reach, chord * 0.5}, {0, 0, chord * 0.5 + rise}};
    const double half = length_m * 0.5;
    for (const Vec& corner : corners) rod(mesh, {-half, corner.y, corner.z}, {half, corner.y, corner.z}, chord, 8);
    const int bays = std::max(1, std::min(static_cast<int>(std::lround(length_m / side)), 400));
    const double bay = (length_m - chord) / bays;
    for (int b = 0; b <= bays; ++b) {
        const double x = -half + chord * 0.5 + bay * b;
        for (int k = 0; k < 3; ++k) {
            const Vec a = corners[k], c = corners[(k + 1) % 3];
            tube(mesh, {x, a.y, a.z}, {x, c.y, c.z}, brace);
            if (b < bays) {
                const bool lean = (b + k) % 2 == 0;
                tube(mesh, {lean ? x : x + bay, a.y, a.z}, {lean ? x + bay : x, c.y, c.z}, brace);
            }
        }
    }
}

// `part`, turned by `angle` about Z and moved by `offset`, added to `mesh`.
void append_turned(Mesh& mesh, const Mesh& part, double angle, Vec offset) {
    const double c = std::cos(angle), s = std::sin(angle);
    const uint32_t first = static_cast<uint32_t>(mesh.vertex_count());
    for (size_t v = 0; v < part.vertex_count(); ++v) {
        const double x = part.positions[v * 3], y = part.positions[v * 3 + 1], z = part.positions[v * 3 + 2];
        mesh.positions.insert(mesh.positions.end(), {static_cast<float>(c * x - s * y + offset.x), static_cast<float>(s * x + c * y + offset.y), static_cast<float>(z + offset.z)});
        const double nx = part.normals[v * 3], ny = part.normals[v * 3 + 1];
        mesh.normals.insert(mesh.normals.end(), {static_cast<float>(c * nx - s * ny), static_cast<float>(s * nx + c * ny), part.normals[v * 3 + 2]});
    }
    for (uint32_t index : part.triangles) mesh.triangles.push_back(first + index);
}

// A ring of straight lengths of truss, about a metre each, their middles on the circle.
void circle(Mesh& mesh, double diameter, double side) {
    const double radius = diameter * 0.5;
    const int pieces = std::max(8, std::min(static_cast<int>(std::lround(3.14159265358979323846 * diameter / 1.0)), 96));
    const double step = 2.0 * 3.14159265358979323846 / pieces;
    const double length = 2.0 * radius * std::sin(step * 0.5);
    Mesh part;
    truss(part, length, side);
    for (int k = 0; k < pieces; ++k) {
        const double middle = step * (k + 0.5);
        const double inner = radius * std::cos(step * 0.5);    // the chord's middle lies a little inside the circle
        append_turned(mesh, part, middle + 3.14159265358979323846 * 0.5, {inner * std::cos(middle), inner * std::sin(middle), 0});
    }
}

// Two arms along X and Y from a corner block at the origin.
void corner(Mesh& mesh, double arm, double side) {
    box(mesh, {-side * 0.5, -side * 0.5, 0}, {side * 0.5, side * 0.5, side});
    Mesh part;
    truss(part, arm, side);
    append_turned(mesh, part, 0.0, {side * 0.5 + arm * 0.5, 0, 0});
    append_turned(mesh, part, 3.14159265358979323846 * 0.5, {0, side * 0.5 + arm * 0.5, 0});
}

// A figure of that height from a few boxes: enough to say how big a person is beside the rig.
void person(Mesh& mesh, double tall) {
    const double h = tall;
    const double leg = 0.47 * h, hip = 0.09 * h, shoulder = 0.82 * h;
    box(mesh, {-hip - 0.055 * h, -0.05 * h, 0}, {-hip + 0.055 * h, 0.05 * h, leg});            // legs
    box(mesh, {hip - 0.055 * h, -0.05 * h, 0}, {hip + 0.055 * h, 0.05 * h, leg});
    box(mesh, {-0.13 * h, -0.065 * h, leg}, {0.13 * h, 0.065 * h, shoulder});                  // trunk
    box(mesh, {-0.13 * h - 0.065 * h, -0.04 * h, 0.45 * h}, {-0.13 * h, 0.04 * h, shoulder});  // arms
    box(mesh, {0.13 * h, -0.04 * h, 0.45 * h}, {0.13 * h + 0.065 * h, 0.04 * h, shoulder});
    box(mesh, {-0.035 * h, -0.035 * h, shoulder}, {0.035 * h, 0.035 * h, 0.87 * h});           // neck
    box(mesh, {-0.065 * h, -0.075 * h, 0.87 * h}, {0.065 * h, 0.065 * h, h});                  // head, its face a little forward
}

// An audience standing in a field `width` across and `depth` deep, about one and a half people to a
// square metre, set out in rows with a little give, each as tall as `tall` give or take a tenth,
// some with their hands up. The same field always gives the same crowd.
void crowd(Mesh& mesh, double width, double depth, double tall) {
    uint32_t seed = 0x9E3779B9u;
    auto next = [&seed]() {
        seed ^= seed << 13;
        seed ^= seed >> 17;
        seed ^= seed << 5;
        return static_cast<double>(seed & 0xFFFFFF) / static_cast<double>(0x1000000);
    };
    const double spacing = 0.8;    // about 1.5 to a square metre
    const int across = std::max(1, std::min(static_cast<int>(width / spacing), 200));
    const int rows = std::max(1, std::min(static_cast<int>(depth / spacing), 200));
    for (int row = 0; row < rows; ++row) {
        for (int k = 0; k < across; ++k) {
            const double x = -width * 0.5 + (k + 0.5 + (next() - 0.5) * 0.6 + (row % 2) * 0.5) * width / (across + 0.5);
            const double y = -depth * 0.5 + (row + 0.5 + (next() - 0.5) * 0.5) * depth / rows;
            const double h = tall * (0.9 + 0.2 * next());
            Mesh one;
            person(one, h);
            if (next() < 0.25) {
                // Hands up: the arms raised above the shoulders.
                box(one, {-0.13 * h - 0.06 * h, -0.035 * h, 0.82 * h}, {-0.13 * h, 0.035 * h, 1.12 * h});
                box(one, {0.13 * h, -0.035 * h, 0.82 * h}, {0.13 * h + 0.06 * h, 0.035 * h, 1.12 * h});
            }
            // Facing the stage, which is upstage of them (+Y), more or less.
            append_turned(mesh, one, 3.14159265358979323846 + (next() - 0.5) * 0.6, {x, y, 0});
        }
    }
}

}  // namespace

bool is_piece_kind(std::string_view kind) {
    return kind == "deck" || kind == "drape" || kind == "truss" || kind == "box" || kind == "person" || kind == "circle" || kind == "corner" || kind == "model" ||
           kind == "triangle" || kind == "pipe" || kind == "boom" || kind == "crowd" || kind == "stairs";
}

SetPiece default_piece(std::string_view kind) {
    SetPiece piece;
    piece.kind = std::string(is_piece_kind(kind) ? kind : std::string_view("box"));
    auto shade = [&piece](float grey) { piece.color[0] = piece.color[1] = piece.color[2] = grey; };
    if (piece.kind == "deck") {    // a riser of two decks, 2.44 m by 2.44 m, 60 cm up
        piece.width = 2.44, piece.depth = 2.44, piece.height = 0.6;
        shade(0.03f);
    } else if (piece.kind == "drape") {    // a black backdrop
        piece.width = 12.0, piece.depth = 0.04, piece.height = 7.0;
        shade(0.012f);
    } else if (piece.kind == "truss") {    // twelve metres of 29 cm box truss
        piece.width = 12.0, piece.depth = 0.29, piece.height = 0.29;
        shade(0.45f);
    } else if (piece.kind == "person") {
        piece.width = 0.5, piece.depth = 0.3, piece.height = 1.75;
        shade(0.25f);
    } else if (piece.kind == "circle") {    // a ring of 29 cm box truss, eight metres across
        piece.width = 8.0, piece.depth = 0.29, piece.height = 0.29;
        shade(0.45f);
    } else if (piece.kind == "corner") {    // a corner of 29 cm box truss, two metre arms
        piece.width = 2.0, piece.depth = 0.29, piece.height = 0.29;
        shade(0.45f);
    } else if (piece.kind == "triangle") {    // eight metres of 29 cm triangle truss
        piece.width = 8.0, piece.depth = 0.29, piece.height = 0.29;
        shade(0.45f);
    } else if (piece.kind == "pipe") {    // a ten metre bar of 48 mm pipe, as theatres fly
        piece.width = 10.0, piece.depth = 0.048, piece.height = 0.048;
        shade(0.04f);
    } else if (piece.kind == "boom") {    // a 3 m upright of 48 mm pipe on a base plate 45 cm square
        piece.width = 0.45, piece.depth = 0.048, piece.height = 3.0;
        shade(0.04f);
    } else if (piece.kind == "crowd") {    // an audience twelve metres across and eight deep
        piece.width = 12.0, piece.depth = 8.0, piece.height = 1.75;
        shade(0.12f);
    } else if (piece.kind == "stairs") {    // up onto a 60 cm riser: a metre and twenty wide, four steps
        piece.width = 1.2, piece.depth = 1.2, piece.height = 0.6;
        shade(0.03f);
    } else {
        piece.width = 1.0, piece.depth = 1.0, piece.height = 1.0;
        shade(0.08f);
    }
    return piece;
}

Mesh piece_mesh(const SetPiece& piece) {
    Mesh mesh;
    if (piece.kind == "model") return mesh;    // its file is its shape: placed_piece_mesh
    const double width = held(piece.width, 0.001, 100.0, 1.0);
    const double depth = held(piece.depth, 0.001, 100.0, 1.0);
    const double height = held(piece.height, 0.001, 100.0, 1.0);
    if (piece.kind == "truss") {
        truss(mesh, width, std::min(depth, 2.0));
    } else if (piece.kind == "person") {
        person(mesh, std::min(height, 3.0));
    } else if (piece.kind == "circle") {
        circle(mesh, std::max(width, 1.0), std::min(depth, 2.0));
    } else if (piece.kind == "corner") {
        corner(mesh, width, std::min(depth, 2.0));
    } else if (piece.kind == "crowd") {
        crowd(mesh, std::min(width, 160.0), std::min(depth, 160.0), std::min(height, 2.5));
    } else if (piece.kind == "triangle") {
        triangle(mesh, width, std::min(depth, 2.0));
    } else if (piece.kind == "pipe") {
        // Lying along X with its underside at its foot: what hangs on it hangs from there.
        const double across = std::min(depth, 0.5);
        rod(mesh, {-width * 0.5, 0, across * 0.5}, {width * 0.5, 0, across * 0.5}, across, 12);
    } else if (piece.kind == "boom") {
        // Standing on a base plate `width` square, one centimetre thick.
        const double plate = std::min(width, 3.0);
        box(mesh, {-plate * 0.5, -plate * 0.5, 0}, {plate * 0.5, plate * 0.5, 0.01});
        rod(mesh, {0, 0, 0.01}, {0, 0, std::max(height, 0.02)}, std::min(depth, 0.5), 12);
    } else if (piece.kind == "drape") {
        box(mesh, {-width * 0.5, -0.02, 0}, {width * 0.5, 0.02, height});
    } else if (piece.kind == "stairs") {
        // Solid steps from the floor, each a little higher, the top one at `height` and at its +Y end:
        // set in front of a deck as tall, they climb onto it.
        const int steps = std::clamp(static_cast<int>(std::lround(height / 0.18)), 1, 60);
        const double tread = depth / steps;
        for (int k = 0; k < steps; ++k) {
            box(mesh, {-width * 0.5, -depth * 0.5 + k * tread, 0}, {width * 0.5, -depth * 0.5 + (k + 1) * tread, height * (k + 1) / steps});
        }
    } else {
        box(mesh, {-width * 0.5, -depth * 0.5, 0}, {width * 0.5, depth * 0.5, height});
    }
    finish(mesh, piece);
    return mesh;
}

void piece_bounds(const SetPiece& piece, double low[3], double high[3]) {
    const Mesh mesh = piece_mesh(piece);
    for (int k = 0; k < 3; ++k) {
        low[k] = mesh.min[k];
        high[k] = mesh.max[k];
    }
}

std::vector<PlacedPiece> parse_pieces(std::string_view text) {
    std::vector<PlacedPiece> out;
    json::Value root;
    if (!json::parse(text, root)) return out;
    const json::Value* list = root.get("pieces");
    if (!list) return out;
    for (const json::Value& entry : list->array) {
        const json::Value* id = entry.get("id");
        const json::Value* kind = entry.get("kind");
        if (!id || id->type != json::Value::Type::String || id->string.empty() || !kind || !is_piece_kind(kind->string)) continue;
        if (kind->string == "model") {
            // A model: its file (a plain name), what its numbers are and which way up; it is as big as it is.
            const json::Value* file = entry.get("file");
            const json::Value* units = entry.get("units");
            const json::Value* up = entry.get("up");
            const std::string name = file ? file->string : std::string();
            std::string lower = name;
            for (char& c : lower) c = static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
            auto ends = [&lower](const char* ending) { return lower.size() > 4 && lower.compare(lower.size() - 4, 4, ending) == 0; };
            const bool plain = !name.empty() && name.find_first_of("/\\") == std::string::npos && name != "." && name != ".." &&
                               (ends(".glb") || ends(".3ds") || ends(".obj"));
            const std::string unit = units ? units->string : std::string();
            const std::string axis = up ? up->string : std::string();
            if (!plain || !(unit.empty() || unit == "mm" || unit == "cm" || unit == "m" || unit == "in" || unit == "ft") || !(axis.empty() || axis == "y" || axis == "z")) continue;
        }
        bool seen = false;
        for (const PlacedPiece& earlier : out) seen = seen || earlier.id == id->string;
        if (seen) continue;
        PlacedPiece placed;
        placed.id = id->string;
        if (const json::Value* name = entry.get("name")) placed.name = name->string;
        placed.piece = default_piece(kind->string);
        if (const json::Value* picture = entry.get("picture")) {
            if (picture->type == json::Value::Type::String && (placed.piece.kind == "drape" || placed.piece.kind == "box" || placed.piece.kind == "deck")) {
                placed.piece.picture = picture->string;
            }
        }
        if (placed.piece.kind == "model") {
            placed.piece.file = entry.get("file")->string;
            if (const json::Value* units = entry.get("units")) placed.piece.units = units->string;
            if (const json::Value* up = entry.get("up")) placed.piece.up = up->string;
        }
        // A number in bounds, or what the kind usually has; out of bounds and the entry is not used.
        bool usable = true;
        auto number = [&](const char* key, double otherwise, double low, double high) {
            const json::Value* value = entry.get(key);
            if (!value) return otherwise;
            if (value->type != json::Value::Type::Number || !std::isfinite(value->number) || value->number < low || value->number > high) {
                usable = false;
                return otherwise;
            }
            return value->number;
        };
        placed.piece.width = number("width", placed.piece.width * 1000.0, 1.0, 100000.0) / 1000.0;
        placed.piece.depth = number("depth", placed.piece.depth * 1000.0, 1.0, 100000.0) / 1000.0;
        placed.piece.height = number("height", placed.piece.height * 1000.0, 1.0, 100000.0) / 1000.0;
        const double x = number("x", 0.0, -500000.0, 500000.0);
        const double y = number("y", 0.0, -500000.0, 500000.0);
        const double z = number("z", 0.0, -500000.0, 500000.0);
        const double turn = number("turn", 0.0, -360.0, 360.0);
        if (const json::Value* colour = entry.get("colour")) {
            if (colour->size() == 3) {
                for (size_t k = 0; k < 3; ++k) {
                    const double part = colour->array[k].num(-1.0);
                    if (part < 0.0 || part > 1.0) usable = false;
                    else placed.piece.color[k] = static_cast<float>(part);
                }
            } else {
                usable = false;
            }
        }
        placed.tracker = static_cast<int>(number("tracker", 0.0, 0.0, 65535.0));
        if (const json::Value* follow = entry.get("follow_height")) placed.follow_height = follow->type == json::Value::Type::Bool && follow->boolean;
        if (!usable) continue;
        placed.world = piece_place(x, y, z, turn);
        out.push_back(std::move(placed));
    }
    return out;
}

std::string pieces_path(std::string_view mvr_path) {
    std::string path(mvr_path);
    if (path.size() >= 4 && text::iequals(std::string_view(path).substr(path.size() - 4), ".mvr")) path.resize(path.size() - 4);
    return path + ".pieces.json";
}

Transform piece_place(double x, double y, double z, double turn_degrees) {
    const double angle = turn_degrees * 3.14159265358979323846 / 180.0;
    Transform place;
    place.u[0] = std::cos(angle) + 0.0;    // + 0.0: never a negative zero in a written matrix
    place.u[1] = std::sin(angle) + 0.0;
    place.v[0] = -std::sin(angle) + 0.0;
    place.v[1] = std::cos(angle) + 0.0;
    place.o[0] = x;
    place.o[1] = y;
    place.o[2] = z;
    return place;
}

void resolve_models(std::vector<PlacedPiece>& pieces, const std::string& models_dir) {
    for (PlacedPiece& placed : pieces) {
        if (placed.piece.kind == "model" && !models_dir.empty()) placed.model_path = models_dir + "/" + placed.piece.file;
    }
}

std::string models_folder(const std::string& fixture_library) {
    std::string folder = fixture_library;
    while (!folder.empty() && (folder.back() == '/' || folder.back() == '\\')) folder.pop_back();
    const size_t cut = folder.find_last_of("/\\");
    return (cut == std::string::npos ? std::string(".") : folder.substr(0, cut)) + "/models";
}

Mesh placed_piece_mesh(const PlacedPiece& placed) {
    if (placed.piece.kind != "model") return dressed_piece_mesh(placed.piece);
    if (placed.model_path.empty()) return Mesh();
    MeshResult loaded = load_model_file(placed.model_path, placed.piece.units, placed.piece.up);
    return loaded.ok ? std::move(loaded.mesh) : Mesh();
}

Mesh dressed_piece_mesh(const SetPiece& piece) {
    Mesh mesh = piece_mesh(piece);
    if (piece.picture.empty() || (piece.kind != "drape" && piece.kind != "box" && piece.kind != "deck") || mesh.vertex_count() < 4) return mesh;
    std::ifstream file = read_stream(piece.picture);
    if (!file) return mesh;
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    if (bytes.empty() || bytes.size() > (size_t(64) << 20)) return mesh;
    // The first face a box is made with is the side toward its own -Y, the house's side: corners
    // low left, low right, high right, high left, as one looks at it. The picture's top left is high left.
    mesh.uvs.assign(mesh.vertex_count() * 2, 0.0f);
    const float corners[8] = {0, 1, 1, 1, 1, 0, 0, 0};
    for (int k = 0; k < 8; ++k) mesh.uvs[static_cast<size_t>(k)] = corners[k];
    MeshMaterial picture;
    picture.name = "picture";
    std::string lower = piece.picture;
    for (char& c : lower) c = static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
    auto ends = [&lower](const char* ending) { const size_t n = std::strlen(ending); return lower.size() >= n && lower.compare(lower.size() - n, n, ending) == 0; };
    picture.image_type = ends(".png") ? "image/png" : ends(".jpg") || ends(".jpeg") ? "image/jpeg" : ends(".bmp") ? "image/bmp" : "";
    picture.image = std::move(bytes);
    mesh.materials.push_back(picture);
    mesh.triangle_material[0] = mesh.triangle_material[1] = static_cast<uint32_t>(mesh.materials.size() - 1);
    return mesh;
}

}  // namespace viz
