// The 3D shape of a fixture part, read from the model file packed in a GDTF: 3DS, or binary
// glTF (GLB).
//
// What comes out is in GDTF's own terms: metres, right-handed, Z up, the part's origin where the
// file has it. A GDTF Model states the part's Length (X), Width (Y) and Height (Z); the shape is
// stretched from its origin so that its bounding box has exactly those sizes, as the GDTF tools
// do. For a well-made file that changes nothing.
//
// Read: triangles and, from GLB, the normals stored with them. Where a file has no normals (3DS
// never does) they are worked out here, smooth across edges of less than 45 degrees. Also each
// material's base colour and picture (a GLB's own, embedded; a 3DS's or a GLB's by file name,
// read from the archive the model came in), which faces wear which, and texture coordinates.
// Not read: animation, metalness and roughness, normal maps, and compressed GLB (Draco,
// meshopt), which is refused.
#pragma once

#include <cstddef>
#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "vizcore/archive.h"
#include "vizcore/gdtf.h"
#include "vizcore/types.h"

namespace viz {

// What a face is made of, as far as it is drawn: a colour, and a picture over it.
struct MeshMaterial {
    std::string name;
    float color[4] = {1, 1, 1, 1};     // linear, 0 to 1; the picture is multiplied by it
    bool double_sided = false;
    std::string texture_file;          // the picture's file name as the model names it, if any
    std::string image_type;            // "image/png", "image/jpeg": the picture's kind, where known
    std::vector<uint8_t> image;        // the picture's file as stored (PNG, JPEG); empty if there is none or it was not found
    bool textured() const { return !image.empty(); }
};

// A triangle that wears no material.
const uint32_t kNoMaterial = 0xFFFFFFFFu;

struct Mesh {
    std::vector<float> positions;       // x, y, z for each vertex
    std::vector<float> normals;         // x, y, z for each vertex, unit length
    std::vector<float> uvs;             // u, v for each vertex, (0, 0) the picture's top left; empty if the file gives none
    std::vector<uint32_t> triangles;    // three vertex numbers each, anticlockwise seen from outside
    std::vector<uint32_t> triangle_material;    // one per triangle: an index into `materials`, or kNoMaterial; empty if none wears any
    std::vector<MeshMaterial> materials;
    float min[3] = {0, 0, 0};           // the bounding box
    float max[3] = {0, 0, 0};
    size_t vertex_count() const { return positions.size() / 3; }
    size_t triangle_count() const { return triangles.size() / 3; }
};

struct MeshResult {
    bool ok = false;
    Mesh mesh;
    // The bounding box's size as the file has it, in metres, before any stretching to a Model's
    // dimensions. Comparing it with those dimensions shows whether file and description agree.
    double native_size[3] = {0, 0, 0};
    Diagnostics diagnostics;
};

// 3DS coordinates are taken as millimetres, as GDTF prescribes.
VIZCORE_API MeshResult parse_3ds(const uint8_t* data, size_t size);

// glTF is in metres with Y up; it is turned to Z up.
VIZCORE_API MeshResult parse_glb(const uint8_t* data, size_t size);

// The mesh as a binary glTF 2 file, the form an MVR is best given its models in: metres, with
// glTF's Y up (the mesh's Z becomes Y, its Y becomes -Z), one primitive for each material worn and
// one for the triangles that wear none, colours as base colours, pictures packed in. parse_glb
// reads it back as it was.
VIZCORE_API std::vector<uint8_t> write_glb(const Mesh& mesh, std::string_view name = std::string_view());

// An OBJ file as it is written (no units, no axis turned), with the materials of its MTL file
// when that is given: each one's diffuse colour and picture file's name. Polygons become fans of
// triangles; normals are worked out from the faces unless every corner names one.
VIZCORE_API MeshResult parse_obj(std::string_view obj, std::string_view mtl = std::string_view());

// A model file on its own, for a set piece: GLB, 3DS or OBJ by its ending, with an OBJ's MTL file
// and the pictures its materials name read from beside it. Metres, Z up. `units` says what the
// file's numbers are ("mm", "cm", "m", "in", "ft"; empty: as its format has it, GLB metres, 3DS
// millimetres, OBJ metres). `up` says which of its axes is up ("y" or "z"; empty: as its format
// has it, GLB Y, 3DS Z, OBJ Y).
VIZCORE_API MeshResult load_model_file(const std::string& path, std::string_view units = std::string_view(), std::string_view up = std::string_view());

// Stretches a mesh from its origin so that its bounding box measures length x width x height.
// A dimension of zero, or a mesh that is flat in that direction, leaves that direction alone.
VIZCORE_API void fit_mesh(Mesh& mesh, double length, double width, double height);

// The mesh of a GDTF model: models/gltf/<File>.glb if the archive has it, else
// models/3ds/<File>.3ds, fitted to the model's dimensions. Not ok, and without any diagnostic, if
// the model names no file; the caller then draws the model's primitive shape instead.
VIZCORE_API MeshResult load_model_mesh(const Archive& gdtf, const Model& model);

// The box a fixture fills at rest (every part where its description puts it, nothing panned or
// tilted), in the fixture's own coordinates: metres, Z up, the origin where it hangs. A drawing
// uses it to show a fixture at its true size.
struct Bounds {
    bool valid = false;    // false if no part of the fixture states a size
    double min[3] = {0, 0, 0};
    double max[3] = {0, 0, 0};
};

// `root_geometry` is the geometry a DMX mode names. A part with a model file is measured from
// that file when `gdtf` is given and has it; otherwise from the size its Model states: a plain
// shape (cube, cylinder, sphere) or a part that turns is centred on its origin, the rest hang
// below theirs, as the visualizer draws them.
VIZCORE_API Bounds fixture_bounds(const FixtureType& fixture, std::string_view root_geometry, const Archive* gdtf);

// The points that outline a fixture at rest, as seen from above, from the front and from the
// side, in the same coordinates as fixture_bounds: those of every part's shape that lie on its
// outline in one of the three views (each a rubber band round the shape as it looks from there),
// so that a drawing can show the fixture's real outline rather than its box. Parts are measured as
// fixture_bounds measures them. Empty where no part states a size.
VIZCORE_API std::vector<std::array<double, 3>> fixture_outline_points(const FixtureType& fixture, std::string_view root_geometry, const Archive* gdtf);

// The mesh of a model file in an MVR, as a Geometry3D names it: 3DS or binary glTF, told apart by
// the name's ending, at the size the file has it (there is nothing to fit it to).
VIZCORE_API MeshResult load_scene_mesh(const Archive& mvr, const std::string& file_name);

}  // namespace viz
