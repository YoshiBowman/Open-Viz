// Turns raw DMX into physical values for one DMX mode of a GDTF fixture.
//
// How a channel's value is read:
//  1. The channel's bytes are combined most significant first into one raw value (8/16/24/32 bit).
//  2. The active channel function is the first one, in file order across the channel's logical
//     channels, whose DMX range contains the raw value and whose mode master (if it has one) is
//     inside ModeFrom-ModeTo.
//  3. The physical value is a linear map from the function's DMX range onto PhysicalFrom-PhysicalTo.
//     If the raw value falls in a channel set that states PhysicalFrom and/or PhysicalTo, the map
//     runs over the set's own DMX range instead, with the missing end taken from the function.
//
// Two points the spec leaves open and this file decides; both should be checked against
// libMVRgdtf on real files:
//  - A channel set with neither physical value follows the function's linear map. Read literally,
//    the spec's default would make each such set sweep the function's whole physical range.
//  - A function's DMX range ends before the next function in its logical channel that starts
//    higher and could be active at the same time. Functions on the same master whose
//    ModeFrom-ModeTo ranges do not overlap never limit each other.
//
// Not applied yet: DMX profiles, relations (multiply/override) and sub channel sets.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "vizcore/gdtf.h"

namespace viz {

// One node of a mode's geometry tree with every GeometryReference expanded, so a geometry that
// is referenced four times appears four times. This is the tree a fixture is built from.
struct GeometryInstance {
    // Unique within the fixture. A referenced geometry takes its reference's name ("Cell3");
    // what hangs below it is addressed as "<reference>.<geometry>".
    std::string name;
    const Geometry* geometry = nullptr;    // the node in the fixture type that describes this instance
    int parent = -1;                       // index into the same list; -1 for the root
    Matrix4 local;                         // rest position relative to the parent, metres
    // The GeometryReferences this instance was reached through, outermost first.
    std::vector<const Geometry*> references;
};

// Parents always come before their children. Empty if `root_geometry` is not a top-level geometry.
VIZCORE_API std::vector<GeometryInstance> expand_geometry(const FixtureType& fixture, std::string_view root_geometry,
                                                          Diagnostics& diagnostics);

VIZCORE_API Matrix4 multiply(const Matrix4& a, const Matrix4& b);

// One addressable instance of a DMX channel. A channel that belongs to a geometry instanced by
// GeometryReferences (the cells of a batten, say) appears once per reference.
struct ChannelInstance {
    int channel = 0;             // index into DmxMode::channels
    std::string geometry;        // name of the geometry instance it acts on
    int geometry_index = -1;     // index into ModeEvaluator::geometry(); -1 if the geometry is not in the tree
    int dmx_break = 1;
    std::vector<int> offsets;    // 1-based within the break, most significant byte first; empty if virtual
};

struct ChannelValue {
    uint32_t dmx = 0;            // raw value at the channel's resolution
    bool from_dmx = false;       // false: the supplied DMX did not cover this channel, its default was used
    int logical_channel = -1;    // active function; -1 when no function accepts the value
    int function = -1;
    int channel_set = -1;        // within the active function, -1 if the value is in none
    double physical = 0.0;
};

struct DmxBreakData {
    int dmx_break = 1;
    const uint8_t* data = nullptr;    // data[0] is the break's first slot (the fixture's start address)
    size_t size = 0;
};

class VIZCORE_API ModeEvaluator {
public:
    // `fixture` must outlive the evaluator.
    ModeEvaluator(const FixtureType& fixture, size_t mode_index);

    bool valid() const { return mode_ != nullptr; }
    const DmxMode& mode() const { return *mode_; }
    const Diagnostics& diagnostics() const { return diagnostics_; }

    // The mode's geometry tree with references expanded.
    const std::vector<GeometryInstance>& geometry() const { return geometry_; }

    // Every channel instance of the mode, in channel order.
    const std::vector<ChannelInstance>& channels() const { return instances_; }

    // Breaks the mode uses, ascending, and the number of slots each one occupies.
    std::vector<int> breaks() const;
    size_t footprint(int dmx_break = 1) const;

    // First instance of the channel named e.g. "Head_Pan" on the given geometry instance
    // (any instance if `geometry` is empty), or -1.
    int find(std::string_view channel_name, std::string_view geometry = {}) const;

    // The DMX a console sends for this mode when nothing has been programmed: every channel at
    // its default, most significant byte first. One byte per slot of the break's footprint.
    std::vector<uint8_t> default_frame(int dmx_break = 1) const;

    // `out` gets one value per entry of channels().
    void evaluate(const uint8_t* dmx, size_t size, std::vector<ChannelValue>& out) const;    // break 1 only
    void evaluate(const std::vector<DmxBreakData>& breaks, std::vector<ChannelValue>& out) const;

private:
    void evaluate_one(size_t index, const std::vector<DmxBreakData>& breaks,
                      std::vector<ChannelValue>& out, std::vector<uint8_t>& state) const;

    const DmxMode* mode_ = nullptr;
    std::vector<GeometryInstance> geometry_;
    std::vector<ChannelInstance> instances_;
    // Per instance, one entry per channel function in file order: the instance its mode master
    // resolves to, or -1 for a function without a master.
    std::vector<std::vector<int>> master_instance_;
    Diagnostics diagnostics_;
};

// What one fixture draws in this mode, in watts. If the file rates a power connection (a wiring
// object's ElectricalPayLoad), that is it, and `rated` is set. Otherwise it is what the mode's
// light sources consume added up, which leaves out motors, fans and electronics.
VIZCORE_API double mode_power_watts(const ModeEvaluator& mode, bool* rated = nullptr);

}  // namespace viz
