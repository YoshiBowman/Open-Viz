// GDTF fixture type description (DIN SPEC 15800), parsed from description.xml.
//
// The model keeps what the visualizer needs to build and drive a fixture: DMX modes down to
// channel sets, mode-master dependencies, wheels, emitters and the geometry tree. Sections that
// are not read yet (filters, colour spaces, DMX profiles, macros, revisions, protocols) are
// skipped without a diagnostic.
#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "vizcore/types.h"

namespace viz {

class Archive;

struct GdtfAttribute {
    std::string name;
    std::string pretty;
    std::string activation_group;
    std::string feature;
    std::string main_attribute;
    std::string physical_unit = "None";
    std::optional<ColorCIE> color;
};

struct PrismFacet {
    ColorCIE color;
    double rotation[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
};

struct AnimationSystem {
    double p1[2] = {0, 0};
    double p2[2] = {0, 0};
    double p3[2] = {0, 0};
    double radius = 0;
};

struct WheelSlot {
    std::string name;
    ColorCIE color;
    std::string filter;
    std::string media_file;                 // resource name without folder or extension; lives in ./wheels
    std::vector<PrismFacet> facets;
    std::optional<AnimationSystem> animation;
};

// Channel sets address slots by WheelSlotIndex, which counts from 1.
struct Wheel {
    std::string name;
    std::vector<WheelSlot> slots;
};

struct MeasurementPoint {
    double wavelength = 0;    // nm
    double energy = 0;        // W/m2/nm
};

enum class Interpolation { Linear, Step, Log };

struct Measurement {
    double physical = 0;              // percent of the controlling channel
    double luminous_intensity = 0;    // cd, additive mixing
    double transmission = 0;          // subtractive mixing
    Interpolation interpolation_to = Interpolation::Linear;
    std::vector<MeasurementPoint> points;
};

// A colour filter the fixture moves into the beam: a CMY flag, a CTO. Only its colour is read.
struct Filter {
    std::string name;
    std::optional<ColorCIE> color;
};

struct Emitter {
    std::string name;
    std::optional<ColorCIE> color;    // absent for non-visible emitters such as UV
    double dominant_wavelength = 0;   // nm
    std::string diode_part;
    std::vector<Measurement> measurements;
};

struct Model {
    std::string name;
    double length = 0, width = 0, height = 0;    // metres
    std::string primitive_type = "Undefined";
    std::string file;
};

enum class GeometryType {
    Geometry, Axis, FilterBeam, FilterColor, FilterGobo, FilterShaper, Beam,
    MediaServerLayer, MediaServerCamera, MediaServerMaster, Display, GeometryReference,
    Laser, WiringObject, Inventory, Structure, Support, Magnet,
};

VIZCORE_API const char* to_string(GeometryType type);

enum class LampType { Discharge, Tungsten, Halogen, LED };
enum class BeamType { Wash, Spot, None, Rectangle, PC, Fresnel, Glow };

// Defaults are the ones the spec gives for the Beam geometry.
struct BeamProperties {
    LampType lamp_type = LampType::Discharge;
    double power_consumption = 1000;    // W
    double luminous_flux = 10000;       // lm
    double color_temperature = 6000;    // K
    double beam_angle = 25;             // degrees
    double field_angle = 25;            // degrees
    double throw_ratio = 1;
    double rectangle_ratio = 1.7777;
    double beam_radius = 0.05;          // metres
    BeamType beam_type = BeamType::Wash;
    int color_rendering_index = 100;
    std::string emitter_spectrum;
};

// One DMX offset a GeometryReference applies to the channels of the geometry it instances.
struct GeometryBreak {
    int dmx_break = 1;
    int dmx_offset = 1;    // 1 means no shift
};

struct Geometry {
    GeometryType type = GeometryType::Geometry;
    std::string name;
    std::string model;
    Matrix4 position;                      // relative to the parent geometry
    std::optional<BeamProperties> beam;    // Beam geometries only
    double electrical_payload = 0;         // WiringObject only: what it is rated to carry or draw, in watts
    std::string reference;                 // GeometryReference only: name of the top-level geometry it instances
    std::vector<GeometryBreak> breaks;     // GeometryReference only
    std::vector<Geometry> children;
};

struct ChannelSet {
    std::string name;
    uint32_t dmx_from = 0;    // channel resolution, inclusive
    uint32_t dmx_to = 0;
    // Present only when the file states them. A set with neither follows its function's
    // linear mapping; see evaluate.h for the full rule.
    std::optional<double> physical_from;
    std::optional<double> physical_to;
    int wheel_slot_index = 0;    // 1-based, 0 when the set does not select a slot
};

struct ChannelFunction {
    std::string name;                  // defaults to "<Attribute> <index in logical channel>"
    std::string attribute = "NoFeature";
    std::string original_attribute;
    uint32_t dmx_from = 0;             // channel resolution, inclusive
    uint32_t dmx_to = 0;
    uint32_t default_value = 0;        // channel resolution
    double physical_from = 0;
    double physical_to = 1;
    double real_fade = 0;              // seconds from min to max
    double real_acceleration = 0;      // seconds from stop to full speed
    std::string wheel;
    std::string emitter;
    std::string filter;
    std::string dmx_profile;           // recorded, not applied yet

    // Mode master: this function is only active while the master channel's value is inside
    // [mode_from, mode_to]. Indices are -1 when the function has no master.
    std::string mode_master;           // the link as written
    int mode_master_channel = -1;      // index into DmxMode::channels
    int mode_master_logical = -1;      // set when the link names a channel function, not a whole channel
    int mode_master_function = -1;
    uint32_t mode_from = 0;            // master channel's resolution, inclusive
    uint32_t mode_to = 0;

    std::vector<ChannelSet> channel_sets;
};

enum class MasterType { None, Grand, Group };

struct LogicalChannel {
    std::string attribute;
    bool snap = false;
    MasterType master = MasterType::None;
    double mib_fade = 0;
    double dmx_change_time_limit = 0;
    std::vector<ChannelFunction> functions;
};

struct VIZCORE_API DmxChannel {
    std::string name;                  // "<Geometry>_<attribute of the first logical channel>"
    std::string geometry;
    int dmx_break = 1;
    bool break_overwrite = false;      // DMXBreak="Overwrite": break and offset come from the GeometryReference
    std::vector<int> offsets;          // 1-based, most significant byte first; empty for a virtual channel
    int bytes = 1;                     // resolution: offsets.size(), or what the values imply for a virtual channel
    std::optional<uint32_t> highlight;
    int initial_logical = 0;           // the function active when nothing else selects one
    int initial_function = 0;
    std::vector<LogicalChannel> logical_channels;

    uint32_t default_value() const;    // the initial function's default
};

enum class RelationType { Multiply, Override };

struct Relation {
    std::string name;
    std::string master;      // DMX channel name
    std::string follower;    // channel function link
    RelationType type = RelationType::Multiply;
    int master_channel = -1;
    int follower_channel = -1;
    int follower_logical = -1;
    int follower_function = -1;
};

struct VIZCORE_API DmxMode {
    std::string name;
    std::string description;
    std::string geometry;    // the top-level geometry this mode drives
    std::vector<DmxChannel> channels;
    std::vector<Relation> relations;

    int find_channel(std::string_view channel_name) const;
};

struct VIZCORE_API FixtureType {
    std::string data_version;
    std::string name, short_name, long_name, manufacturer, description;
    std::string fixture_type_id;
    std::string thumbnail;
    std::string ref_ft;
    bool can_have_children = true;
    double weight = 0;    // kilograms, with all accessories; 0 when the file does not say

    std::vector<GdtfAttribute> attributes;
    std::vector<Wheel> wheels;
    std::vector<Emitter> emitters;
    std::vector<Filter> filters;
    std::vector<Model> models;
    std::vector<Geometry> geometries;    // top-level geometries
    std::vector<DmxMode> modes;

    const Geometry* find_top_level_geometry(std::string_view geometry_name) const;
    const Wheel* find_wheel(std::string_view wheel_name) const;
    const Emitter* find_emitter(std::string_view emitter_name) const;
    const Filter* find_filter(std::string_view filter_name) const;
    const GdtfAttribute* find_attribute(std::string_view attribute_name) const;
    int find_mode(std::string_view mode_name) const;
};

struct GdtfResult {
    bool ok = false;    // false: the fixture is unusable, see diagnostics
    FixtureType fixture;
    Diagnostics diagnostics;
};

VIZCORE_API GdtfResult parse_gdtf_description(std::string_view xml);
VIZCORE_API GdtfResult load_gdtf(const Archive& archive);
VIZCORE_API GdtfResult load_gdtf_file(const std::string& path);

}  // namespace viz
