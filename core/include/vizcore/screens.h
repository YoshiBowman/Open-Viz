// Where the picture on a video screen goes.
//
// An MVR draws a screen as a model like any other piece of scenery: a slab. The picture belongs
// on its largest face, on the side that is looked at, the right way up. This works that out from
// the box the screen's models fill.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "vizcore/mvr.h"
#include "vizcore/types.h"

namespace viz {

class Archive;

struct ScreenFace {
    std::string uuid;
    std::string name;
    std::string layer;
    std::string class_name;
    std::string parent_fixture;    // the fixture it hangs on, as MvrObject has it: the screen rides along with that
    std::string child_position;
    // The middle of the picture, in the MVR's world, millimetres. `u` runs to the picture's
    // right, `v` up it, and `w` out of it toward whoever is looking; all three of length one.
    Transform world;
    double width = 0;     // millimetres, along u
    double height = 0;    // along v
    std::vector<MvrSource> sources;
};

// The face of a slab that fills the box `low`..`high` (millimetres, in its own space) and is
// placed by `placed`. False if the box has no face to speak of.
//
// The thinnest direction of the box is taken to be through the screen. Up the picture is
// whichever of the other two points more nearly up (for a screen lying flat: upstage). It is
// looked at from the house; a screen edge-on to the house, from the centre line; a screen lying
// flat, from above if it is near the floor and from below if it hangs high.
VIZCORE_API bool screen_face(const Transform& placed, const double low[3], const double high[3], ScreenFace& out);

// A screen that is not in the rig file: one that stands upright, `width` by `height`, with the
// middle of its lower edge at x, y, z. `turn_degrees` turns it anticlockwise as seen from above;
// at 0 it faces the house. Millimetres.
VIZCORE_API ScreenFace standing_screen(double x, double y, double z, double width, double height, double turn_degrees);

// The face of every video screen in a scene that has a shape, in the scene's order. A screen's
// models are read from `archive` for their sizes.
VIZCORE_API std::vector<ScreenFace> screen_faces(const MvrScene& scene, const Archive& archive);

// A fixture taken as a slab hanging in the rig: an LED panel, when looking for walls of them.
struct WallPanel {
    std::string uuid;
    std::string type;                       // whatever tells one fixture type and mode from another
    Transform world;                        // its placing, millimetres
    double low[3] = {0, 0, 0};              // the box it fills at rest, in its own frame, millimetres
    double high[3] = {0, 0, 0};
    bool lit = false;                       // whether `toward` is known
    double toward[3] = {0, 0, 0};           // the way its light leaves, in the world
};

// Panels of one type that hang edge to edge as one flat surface: a wall of LED panels, which a
// screen can stand in for. A panel by itself counts as a wall of one.
struct PanelWall {
    std::string type;
    std::vector<size_t> panels;             // indices into what was given, rising
    ScreenFace face;                        // the side the light leaves from (toward the house if that is not known); only its place and size are filled in
};

// The walls among `panels`. A fixture is a panel if the box it fills is flat: at least 10 cm both
// ways across and no thicker than a quarter of that. Panels belong to one wall when they are of
// one type, lie in one plane with their edges square to each other, and touch (within 2.5 cm).
VIZCORE_API std::vector<PanelWall> find_walls(const std::vector<WallPanel>& panels);

// A screen lit pixel by pixel over DMX: an LED strip, a low-resolution wall, anything a console or
// a media server maps a picture onto and sends as levels.
//
// The pixels take their slots in order from the first address on; when the next pixel no longer
// fits in a universe it starts at slot 1 of the next (a pixel is never split over two universes),
// which is how pixel mappers and LED drivers count.
struct PixelMap {
    enum class Wiring { Rows, Snake, Columns, SnakeColumns };
    int universe = 1;            // of the first pixel
    int address = 1;             // its first slot, 1 to 512
    int across = 1;
    int down = 1;
    std::string order = "RGB";   // what each pixel's slots are, in order: R, G, B and W (white, added to all three)
    Wiring wiring = Wiring::Rows;

    int slots_per_pixel() const { return static_cast<int>(order.size()); }
    int pixels() const { return across * down; }
    // How many universes the pixels reach over, the first included.
    VIZCORE_API int universes() const;
};

// Reads a pixel map as Openviz writes it: "<universe>.<address> <across>x<down> [order] [wiring]",
// for example "20.1 40x10 GRB snake". The order is RGB and the wiring rows unless given; wirings are
// rows (every row left to right, from the top), snake (every other row right to left), columns
// (every column top to bottom, from the left) and snake-columns. False with a reason if it is not one.
VIZCORE_API bool parse_pixel_map(std::string_view text, PixelMap& out, std::string& error);

// Where the pixel in `column` and `row` (0, 0 is top left) has its first slot: the universe, and
// the slot counted from 0.
struct PixelPlace {
    int universe = 0;
    int slot = 0;
};
VIZCORE_API PixelPlace pixel_place(const PixelMap& map, int column, int row);

// The picture the universes hold: four bytes a pixel (red, green, blue, 255), row by row from the
// top left. `lookup` gives a universe's 512 slots, or null where nothing has arrived: those pixels
// are black. Returns how many of the map's universes had something.
using UniverseLookup = const uint8_t* (*)(int universe, void* context);
VIZCORE_API int read_pixels(const PixelMap& map, UniverseLookup lookup, void* context, std::vector<uint8_t>& rgba);

}  // namespace viz
