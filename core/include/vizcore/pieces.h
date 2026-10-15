// Set pieces that are not in a rig file but added in the application: a stage deck, a drape, a
// length of truss, a plain box, a figure to give the picture its scale. A console's rig file
// often has none of these (it knows fixtures, not scenery), and a rig that hangs in the void over
// a bare floor is hard to read.
//
// A piece is a kind and three measures; its shape is made here, so that whoever draws the stage,
// whoever draws the plot and whoever writes it out agree on it. Where it stands is the caller's
// business: the mesh has its origin in the middle of its footprint, at its foot.
#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "vizcore/mesh.h"
#include "vizcore/types.h"

namespace viz {

struct SetPiece {
    // "deck"    a platform: width by depth, its top `height` above its foot
    // "drape"   a cloth hung flat: width by height, a few centimetres thick, facing along its own Y
    // "truss"   box truss lying along its own X: `width` long, `depth` square (its side), on its bottom chords
    // "box"     anything else that is a box: a monitor wedge, a PA stack, a wall
    // "person"  a figure `height` tall, facing along its own -Y (toward the house when not turned)
    // "circle"  a ring of box truss `width` across its middle line, `depth` its side, lying flat on its bottom chords
    // "corner"  two arms of box truss `width` long, along its own X and Y from a corner block, `depth` their side
    // "model"   a model file of the application's models folder (GLB, 3DS or OBJ), as big as it is
    std::string kind = "box";
    double width = 1.0;     // metres, along its own X
    double depth = 1.0;     // along its own Y
    double height = 1.0;    // along Z
    float color[3] = {0.5f, 0.5f, 0.5f};    // linear, 0 to 1
    std::string file;      // "model": the file's name in the models folder
    std::string units;     // "model": what its numbers are ("mm", "cm", "m", "in", "ft"), empty as its format has it
    std::string up;        // "model": which of its axes is up ("y", "z"), empty as its format has it
    std::string picture;   // "drape", "box", "deck": a picture (PNG, JPEG, BMP) on the side toward the house; the whole path
};

// Whether that is a kind there is a shape for.
VIZCORE_API bool is_piece_kind(std::string_view kind);

// A piece of that kind as it is when nothing more is said: a usual size and colour.
VIZCORE_API SetPiece default_piece(std::string_view kind);

// The piece's shape: metres, Z up, the origin in the middle of its footprint at its foot, one
// material of its colour. Measures are held to sensible bounds (a millimetre to a hundred metres),
// and a kind that is not known is drawn as a box. A model has no shape of its own here: see
// placed_piece_mesh.
VIZCORE_API Mesh piece_mesh(const SetPiece& piece);

// The shape with its picture on it, where it has one: piece_mesh, and the picture's file read and
// laid on the side toward the house (its own -Y) as a material of its own. Without a readable
// picture, the same as piece_mesh.
VIZCORE_API Mesh dressed_piece_mesh(const SetPiece& piece);

// The box the piece fills, as its mesh has it: for a plot.
VIZCORE_API void piece_bounds(const SetPiece& piece, double low[3], double high[3]);

// A set piece as a show keeps it. The application writes a show's pieces beside it as
// "<show>.pieces.json"; the visualizer, the plot and the export read them from there:
//
//   {"pieces": [{"id": "piece-1", "kind": "truss", "name": "Truss 1", "colour": [0.45, 0.45, 0.45],
//                "width": 12000, "depth": 290, "height": 290, "x": 0, "y": 0, "z": 6000, "turn": 0}]}
//
// Measures and places in millimetres, MVR's coordinates; turn in degrees about the vertical,
// anticlockwise seen from above.
struct PlacedPiece {
    std::string id;
    std::string name;
    SetPiece piece;        // its measures in metres, as piece_mesh wants them
    Transform world;       // where it stands, in the rig file's terms (millimetres)
    std::string model_path;    // a model's file, once resolve_models has found it
};

// Entries that are not usable (no id, an id seen before, a kind there is no shape for, measures
// out of bounds) are left out; an unreadable file gives none.
VIZCORE_API std::vector<PlacedPiece> parse_pieces(std::string_view json);
// "rig.mvr" -> "rig.pieces.json", in the same folder.
VIZCORE_API std::string pieces_path(std::string_view mvr_path);
// A place and a turn about the vertical as a placing.
VIZCORE_API Transform piece_place(double x, double y, double z, double turn_degrees);

// Where the model pieces' files are: `models_dir`/<file>. The folder the application keeps its
// models in sits beside its fixture library: models_folder("<library>/fixtures") is "<library>/models".
VIZCORE_API void resolve_models(std::vector<PlacedPiece>& pieces, const std::string& models_dir);
VIZCORE_API std::string models_folder(const std::string& fixture_library);
// The piece's shape, in its own coordinates: a model's file as it is read (load_model_file; empty
// if it cannot be read), or the shape made for its kind.
VIZCORE_API Mesh placed_piece_mesh(const PlacedPiece& placed);

}  // namespace viz
