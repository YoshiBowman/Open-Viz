#include "vizcore/gdtf.h"

#include <algorithm>
#include <cstring>
#include <utility>

#include "pugixml.hpp"
#include "text.h"
#include "vizcore/archive.h"

namespace viz {

namespace {

using pugi::xml_node;

std::string str(const xml_node& node, const char* name) { return node.attribute(name).as_string(); }

bool has_value(const xml_node& node, const char* name) { return node.attribute(name).value()[0] != '\0'; }

std::string quoted(const std::string& text) { return "'" + text + "'"; }

// "Default value: object type with an index in parent" (spec, Table 1, Name).
std::string name_or_indexed(const xml_node& node, const char* type, size_t index) {
    std::string name = str(node, "Name");
    return name.empty() ? std::string(type) + " " + std::to_string(index + 1) : name;
}

double number(const xml_node& node, const char* name, double fallback, Diagnostics& diagnostics,
              const std::string& where) {
    if (!has_value(node, name)) return fallback;
    double value = fallback;
    if (!text::parse_double(node.attribute(name).value(), value)) {
        diagnostics.warn(where, std::string(name) + "=\"" + node.attribute(name).value() + "\" is not a number");
        return fallback;
    }
    return value;
}

std::optional<double> optional_number(const xml_node& node, const char* name, Diagnostics& diagnostics,
                                      const std::string& where) {
    if (!has_value(node, name)) return std::nullopt;
    double value = 0;
    if (!text::parse_double(node.attribute(name).value(), value)) {
        diagnostics.warn(where, std::string(name) + "=\"" + node.attribute(name).value() + "\" is not a number");
        return std::nullopt;
    }
    return value;
}

long long integer(const xml_node& node, const char* name, long long fallback, Diagnostics& diagnostics,
                  const std::string& where) {
    if (!has_value(node, name)) return fallback;
    long long value = fallback;
    if (!text::parse_int(node.attribute(name).value(), value)) {
        diagnostics.warn(where, std::string(name) + "=\"" + node.attribute(name).value() + "\" is not an integer");
        return fallback;
    }
    return value;
}

std::optional<ColorCIE> optional_color(const xml_node& node, const char* name, Diagnostics& diagnostics,
                                       const std::string& where) {
    if (!has_value(node, name)) return std::nullopt;
    // grandMA3 writes "-nan,-nan,-nan" for an attribute that has no colour; that is "absent", not an error.
    if (std::strstr(node.attribute(name).value(), "nan")) return std::nullopt;
    ColorCIE color;
    if (!text::parse_color(node.attribute(name).value(), color)) {
        diagnostics.warn(where, std::string(name) + "=\"" + node.attribute(name).value() + "\" is not an x,y,Y colour");
        return std::nullopt;
    }
    return color;
}

std::optional<DmxValue> optional_dmx(const xml_node& node, const char* name, Diagnostics& diagnostics,
                                     const std::string& where) {
    if (!has_value(node, name)) return std::nullopt;
    DmxValue value;
    if (!parse_dmx_value(node.attribute(name).value(), value)) {
        diagnostics.warn(where, std::string(name) + "=\"" + node.attribute(name).value() +
                                    "\" is not a DMX value of the form value/bytes");
        return std::nullopt;
    }
    return value;
}

template <typename Enum>
struct EnumName {
    const char* name;
    Enum value;
};

template <typename Enum, size_t N>
Enum enumeration(const xml_node& node, const char* name, const EnumName<Enum> (&names)[N], Enum fallback,
                 Diagnostics& diagnostics, const std::string& where) {
    if (!has_value(node, name)) return fallback;
    const char* written = node.attribute(name).value();
    for (const EnumName<Enum>& candidate : names) {
        if (text::iequals(written, candidate.name)) return candidate.value;
    }
    diagnostics.warn(where, std::string(name) + "=\"" + written + "\" is not a known value");
    return fallback;
}

const EnumName<GeometryType> kGeometryTypes[] = {
    {"Geometry", GeometryType::Geometry},
    {"Axis", GeometryType::Axis},
    {"FilterBeam", GeometryType::FilterBeam},
    {"FilterColor", GeometryType::FilterColor},
    {"FilterGobo", GeometryType::FilterGobo},
    {"FilterShaper", GeometryType::FilterShaper},
    {"Beam", GeometryType::Beam},
    {"MediaServerLayer", GeometryType::MediaServerLayer},
    {"MediaServerCamera", GeometryType::MediaServerCamera},
    {"MediaServerMaster", GeometryType::MediaServerMaster},
    {"Display", GeometryType::Display},
    {"GeometryReference", GeometryType::GeometryReference},
    {"Laser", GeometryType::Laser},
    {"WiringObject", GeometryType::WiringObject},
    {"Inventory", GeometryType::Inventory},
    {"Structure", GeometryType::Structure},
    {"Support", GeometryType::Support},
    {"Magnet", GeometryType::Magnet},
};

const EnumName<LampType> kLampTypes[] = {{"Discharge", LampType::Discharge},
                                         {"Tungsten", LampType::Tungsten},
                                         {"Halogen", LampType::Halogen},
                                         {"LED", LampType::LED}};

const EnumName<BeamType> kBeamTypes[] = {{"Wash", BeamType::Wash},       {"Spot", BeamType::Spot},
                                         {"None", BeamType::None},       {"Rectangle", BeamType::Rectangle},
                                         {"PC", BeamType::PC},           {"Fresnel", BeamType::Fresnel},
                                         {"Glow", BeamType::Glow}};

const EnumName<Interpolation> kInterpolations[] = {{"Linear", Interpolation::Linear},
                                                   {"Step", Interpolation::Step},
                                                   {"Log", Interpolation::Log}};

const EnumName<MasterType> kMasterTypes[] = {{"None", MasterType::None},
                                             {"Grand", MasterType::Grand},
                                             {"Group", MasterType::Group}};

const EnumName<RelationType> kRelationTypes[] = {{"Multiply", RelationType::Multiply},
                                                 {"Override", RelationType::Override}};

const EnumName<bool> kYesNo[] = {{"Yes", true}, {"No", false}, {"On", true}, {"Off", false}};

void parse_attributes(const xml_node& fixture_type, FixtureType& fixture, Diagnostics& diagnostics) {
    for (const xml_node& node : fixture_type.child("AttributeDefinitions").child("Attributes").children("Attribute")) {
        GdtfAttribute attribute;
        attribute.name = str(node, "Name");
        std::string where = "Attribute " + quoted(attribute.name);
        attribute.pretty = str(node, "Pretty");
        attribute.activation_group = str(node, "ActivationGroup");
        attribute.feature = str(node, "Feature");
        attribute.main_attribute = str(node, "MainAttribute");
        if (has_value(node, "PhysicalUnit")) attribute.physical_unit = str(node, "PhysicalUnit");
        attribute.color = optional_color(node, "Color", diagnostics, where);
        fixture.attributes.push_back(std::move(attribute));
    }
}

void parse_wheels(const xml_node& fixture_type, FixtureType& fixture, Diagnostics& diagnostics) {
    size_t wheel_index = 0;
    for (const xml_node& wheel_node : fixture_type.child("Wheels").children("Wheel")) {
        Wheel wheel;
        wheel.name = name_or_indexed(wheel_node, "Wheel", wheel_index++);
        size_t slot_index = 0;
        for (const xml_node& slot_node : wheel_node.children("Slot")) {
            WheelSlot slot;
            slot.name = name_or_indexed(slot_node, "Slot", slot_index++);
            std::string where = "Wheel " + quoted(wheel.name) + " / Slot " + quoted(slot.name);
            if (auto color = optional_color(slot_node, "Color", diagnostics, where)) slot.color = *color;
            slot.filter = str(slot_node, "Filter");
            slot.media_file = str(slot_node, "MediaFileName");
            for (const xml_node& facet_node : slot_node.children("Facet")) {
                PrismFacet facet;
                if (auto color = optional_color(facet_node, "Color", diagnostics, where)) facet.color = *color;
                if (has_value(facet_node, "Rotation") &&
                    !text::parse_braced_rows(facet_node.attribute("Rotation").value(), 3, 3, &facet.rotation[0][0])) {
                    diagnostics.warn(where, "Facet Rotation is not a 3x3 matrix");
                    facet = PrismFacet{facet.color};
                }
                slot.facets.push_back(facet);
            }
            if (xml_node animation_node = slot_node.child("AnimationSystem")) {
                AnimationSystem animation;
                const struct {
                    const char* name;
                    double* target;
                } points[] = {{"P1", animation.p1}, {"P2", animation.p2}, {"P3", animation.p3}};
                for (const auto& point : points) {
                    if (has_value(animation_node, point.name) &&
                        !text::parse_numbers(animation_node.attribute(point.name).value(), 2, point.target)) {
                        diagnostics.warn(where, std::string("AnimationSystem ") + point.name + " is not two numbers");
                    }
                }
                animation.radius = number(animation_node, "Radius", 0, diagnostics, where);
                slot.animation = animation;
            }
            wheel.slots.push_back(std::move(slot));
        }
        fixture.wheels.push_back(std::move(wheel));
    }
}

void parse_filters(const xml_node& fixture_type, FixtureType& fixture, Diagnostics& diagnostics) {
    size_t index = 0;
    for (const xml_node& node : fixture_type.child("PhysicalDescriptions").child("Filters").children("Filter")) {
        Filter filter;
        filter.name = name_or_indexed(node, "Filter", index++);
        filter.color = optional_color(node, "Color", diagnostics, "Filter " + quoted(filter.name));
        fixture.filters.push_back(std::move(filter));
    }
}

void parse_emitters(const xml_node& fixture_type, FixtureType& fixture, Diagnostics& diagnostics) {
    size_t index = 0;
    for (const xml_node& node : fixture_type.child("PhysicalDescriptions").child("Emitters").children("Emitter")) {
        Emitter emitter;
        emitter.name = name_or_indexed(node, "Emitter", index++);
        std::string where = "Emitter " + quoted(emitter.name);
        emitter.color = optional_color(node, "Color", diagnostics, where);
        emitter.dominant_wavelength = number(node, "DominantWaveLength", 0, diagnostics, where);
        emitter.diode_part = str(node, "DiodePart");
        for (const xml_node& measurement_node : node.children("Measurement")) {
            Measurement measurement;
            measurement.physical = number(measurement_node, "Physical", 0, diagnostics, where);
            measurement.luminous_intensity = number(measurement_node, "LuminousIntensity", 0, diagnostics, where);
            measurement.transmission = number(measurement_node, "Transmission", 0, diagnostics, where);
            measurement.interpolation_to = enumeration(measurement_node, "InterpolationTo", kInterpolations,
                                                       Interpolation::Linear, diagnostics, where);
            for (const xml_node& point_node : measurement_node.children("MeasurementPoint")) {
                MeasurementPoint point;
                point.wavelength = number(point_node, "WaveLength", 0, diagnostics, where);
                point.energy = number(point_node, "Energy", 0, diagnostics, where);
                measurement.points.push_back(point);
            }
            emitter.measurements.push_back(std::move(measurement));
        }
        fixture.emitters.push_back(std::move(emitter));
    }
}

void parse_models(const xml_node& fixture_type, FixtureType& fixture, Diagnostics& diagnostics) {
    size_t index = 0;
    for (const xml_node& node : fixture_type.child("Models").children("Model")) {
        Model model;
        model.name = name_or_indexed(node, "Model", index++);
        std::string where = "Model " + quoted(model.name);
        model.length = number(node, "Length", 0, diagnostics, where);
        model.width = number(node, "Width", 0, diagnostics, where);
        model.height = number(node, "Height", 0, diagnostics, where);
        if (has_value(node, "PrimitiveType")) model.primitive_type = str(node, "PrimitiveType");
        model.file = str(node, "File");
        fixture.models.push_back(std::move(model));
    }
}

void parse_geometry_children(const xml_node& parent, std::vector<Geometry>& out, Diagnostics& diagnostics, int depth) {
    if (depth > 64) {
        diagnostics.error("Geometries", "geometry tree is nested more than 64 levels deep; the rest was dropped");
        return;
    }
    size_t index = 0;
    for (const xml_node& node : parent.children()) {
        if (node.type() != pugi::node_element) continue;
        if (std::strcmp(node.name(), "Break") == 0) continue;    // read with its GeometryReference
        const EnumName<GeometryType>* known = nullptr;
        for (const EnumName<GeometryType>& candidate : kGeometryTypes) {
            if (std::strcmp(node.name(), candidate.name) == 0) known = &candidate;
        }
        if (!known) {
            diagnostics.warn("Geometries", std::string("unknown geometry type <") + node.name() + "> skipped with its children");
            continue;
        }
        Geometry geometry;
        geometry.type = known->value;
        geometry.name = name_or_indexed(node, node.name(), index++);
        std::string where = std::string(node.name()) + " " + quoted(geometry.name);
        geometry.model = str(node, "Model");
        if (has_value(node, "Position") &&
            !text::parse_braced_rows(node.attribute("Position").value(), 4, 4, &geometry.position.m[0][0])) {
            diagnostics.warn(where, "Position is not a 4x4 matrix; identity used");
            geometry.position = Matrix4();
        }
        if (geometry.type == GeometryType::WiringObject) {
            geometry.electrical_payload = std::max(0.0, number(node, "ElectricalPayLoad", 0, diagnostics, where));
        }
        if (geometry.type == GeometryType::Beam) {
            BeamProperties beam;
            beam.lamp_type = enumeration(node, "LampType", kLampTypes, beam.lamp_type, diagnostics, where);
            beam.power_consumption = number(node, "PowerConsumption", beam.power_consumption, diagnostics, where);
            beam.luminous_flux = number(node, "LuminousFlux", beam.luminous_flux, diagnostics, where);
            beam.color_temperature = number(node, "ColorTemperature", beam.color_temperature, diagnostics, where);
            beam.beam_angle = number(node, "BeamAngle", beam.beam_angle, diagnostics, where);
            beam.field_angle = number(node, "FieldAngle", beam.field_angle, diagnostics, where);
            beam.throw_ratio = number(node, "ThrowRatio", beam.throw_ratio, diagnostics, where);
            beam.rectangle_ratio = number(node, "RectangleRatio", beam.rectangle_ratio, diagnostics, where);
            beam.beam_radius = number(node, "BeamRadius", beam.beam_radius, diagnostics, where);
            beam.beam_type = enumeration(node, "BeamType", kBeamTypes, beam.beam_type, diagnostics, where);
            beam.color_rendering_index = static_cast<int>(
                integer(node, "ColorRenderingIndex", beam.color_rendering_index, diagnostics, where));
            beam.emitter_spectrum = str(node, "EmitterSpectrum");
            geometry.beam = beam;
        }
        if (geometry.type == GeometryType::GeometryReference) {
            geometry.reference = str(node, "Geometry");
            if (geometry.reference.empty()) diagnostics.warn(where, "GeometryReference does not name a geometry");
            for (const xml_node& break_node : node.children("Break")) {
                GeometryBreak entry;
                long long dmx_break = integer(break_node, "DMXBreak", 1, diagnostics, where);
                if (dmx_break < 1 || dmx_break > 255) {
                    diagnostics.warn(where, "Break DMXBreak is outside 1-255; using 1");
                    dmx_break = 1;
                }
                entry.dmx_break = static_cast<int>(dmx_break);
                // DMXOffset is a DMXAddress: one absolute number, or "Universe.Address".
                std::string written = str(break_node, "DMXOffset");
                long long offset = 1;
                size_t dot = written.find('.');
                bool valid = true;
                if (dot != std::string::npos) {
                    long long universe = 0;
                    long long slot = 0;
                    valid = text::parse_int(std::string_view(written).substr(0, dot), universe) &&
                            text::parse_int(std::string_view(written).substr(dot + 1), slot) && universe >= 1 &&
                            universe <= 65536 && slot >= 1 && slot <= 512;
                    offset = (universe - 1) * 512 + slot;
                } else if (!written.empty()) {
                    valid = text::parse_int(written, offset) && offset >= 1 && offset <= 65536 * 512;
                }
                if (!valid) {
                    diagnostics.warn(where, "Break DMXOffset=\"" + written + "\" is not a DMX address; using 1");
                    offset = 1;
                }
                entry.dmx_offset = static_cast<int>(offset);
                geometry.breaks.push_back(entry);
            }
        } else {
            parse_geometry_children(node, geometry.children, diagnostics, depth + 1);
        }
        out.push_back(std::move(geometry));
    }
}

// DMX values of a channel are written at whatever resolution the author chose and can only be
// converted once the channel's own resolution is known, so they are held back until then.
struct PendingFunction {
    DmxValue dmx_from;
    std::optional<DmxValue> default_value;
    std::optional<DmxValue> mode_from;
    std::optional<DmxValue> mode_to;
    std::vector<DmxValue> set_from;
};

struct PendingChannel {
    std::optional<DmxValue> legacy_default;    // GDTF 1.0 kept Default on the DMXChannel
    std::optional<DmxValue> highlight;
    std::string initial_function;
    std::vector<std::vector<PendingFunction>> functions;    // [logical channel][function]
};

int widest(const PendingChannel& pending) {
    int bytes = 1;
    auto consider = [&bytes](const std::optional<DmxValue>& value) {
        if (value) bytes = std::max(bytes, value->bytes);
    };
    consider(pending.legacy_default);
    consider(pending.highlight);
    for (const auto& logical : pending.functions) {
        for (const PendingFunction& function : logical) {
            bytes = std::max(bytes, function.dmx_from.bytes);
            consider(function.default_value);
            for (const DmxValue& from : function.set_from) bytes = std::max(bytes, from.bytes);
        }
    }
    return bytes;
}

bool parse_channel(const xml_node& node, const std::string& mode_where, DmxChannel& channel,
                   PendingChannel& pending, Diagnostics& diagnostics) {
    channel.geometry = str(node, "Geometry");
    std::string first_attribute = str(node.child("LogicalChannel"), "Attribute");
    channel.name = channel.geometry + "_" + first_attribute;
    std::string where = mode_where + " / DMXChannel " + quoted(channel.name);

    std::string dmx_break = str(node, "DMXBreak");
    if (text::iequals(dmx_break, "Overwrite")) {
        channel.break_overwrite = true;
    } else if (!dmx_break.empty()) {
        long long value = 1;
        if (!text::parse_int(dmx_break, value) || value < 1) {
            diagnostics.warn(where, "DMXBreak=\"" + dmx_break + "\" is not a break number; using 1");
            value = 1;
        }
        channel.dmx_break = static_cast<int>(value);
    }

    std::string offset = str(node, "Offset");
    if (!offset.empty() && !text::iequals(offset, "None")) {
        for (std::string_view part : text::split(offset, ',')) {
            long long value = 0;
            if (!text::parse_int(part, value) || value < 1 || value > 512) {
                diagnostics.error(where, "Offset=\"" + offset + "\" is not a list of addresses from 1 to 512; channel dropped");
                return false;
            }
            channel.offsets.push_back(static_cast<int>(value));
        }
        if (channel.offsets.size() > 4) {
            diagnostics.error(where, "Offset lists more than 4 bytes; channel dropped");
            return false;
        }
    }

    if (!text::iequals(str(node, "Highlight"), "None")) pending.highlight = optional_dmx(node, "Highlight", diagnostics, where);
    pending.legacy_default = optional_dmx(node, "Default", diagnostics, where);
    pending.initial_function = str(node, "InitialFunction");

    for (const xml_node& logical_node : node.children("LogicalChannel")) {
        LogicalChannel logical;
        logical.attribute = str(logical_node, "Attribute");
        std::string logical_where = where + " / LogicalChannel " + quoted(logical.attribute);
        logical.snap = enumeration(logical_node, "Snap", kYesNo, false, diagnostics, logical_where);
        logical.master = enumeration(logical_node, "Master", kMasterTypes, MasterType::None, diagnostics, logical_where);
        logical.mib_fade = number(logical_node, "MibFade", 0, diagnostics, logical_where);
        logical.dmx_change_time_limit = number(logical_node, "DMXChangeTimeLimit", 0, diagnostics, logical_where);

        std::vector<PendingFunction> pending_functions;
        for (const xml_node& function_node : logical_node.children("ChannelFunction")) {
            ChannelFunction function;
            PendingFunction held;
            if (has_value(function_node, "Attribute")) function.attribute = str(function_node, "Attribute");
            function.name = str(function_node, "Name");
            if (function.name.empty()) {
                function.name = function.attribute + " " + std::to_string(logical.functions.size() + 1);
            }
            std::string function_where = logical_where + " / ChannelFunction " + quoted(function.name);
            function.original_attribute = str(function_node, "OriginalAttribute");
            if (auto from = optional_dmx(function_node, "DMXFrom", diagnostics, function_where)) held.dmx_from = *from;
            held.default_value = optional_dmx(function_node, "Default", diagnostics, function_where);
            function.physical_from = number(function_node, "PhysicalFrom", 0, diagnostics, function_where);
            function.physical_to = number(function_node, "PhysicalTo", 1, diagnostics, function_where);
            function.real_fade = number(function_node, "RealFade", 0, diagnostics, function_where);
            function.real_acceleration = number(function_node, "RealAcceleration", 0, diagnostics, function_where);
            function.wheel = str(function_node, "Wheel");
            function.emitter = str(function_node, "Emitter");
            function.filter = str(function_node, "Filter");
            function.dmx_profile = str(function_node, "DMXProfile");
            function.mode_master = str(function_node, "ModeMaster");
            held.mode_from = optional_dmx(function_node, "ModeFrom", diagnostics, function_where);
            held.mode_to = optional_dmx(function_node, "ModeTo", diagnostics, function_where);

            for (const xml_node& set_node : function_node.children("ChannelSet")) {
                ChannelSet set;
                set.name = str(set_node, "Name");
                std::string set_where = function_where + " / ChannelSet " + quoted(set.name);
                DmxValue from;
                if (auto written = optional_dmx(set_node, "DMXFrom", diagnostics, set_where)) from = *written;
                held.set_from.push_back(from);
                set.physical_from = optional_number(set_node, "PhysicalFrom", diagnostics, set_where);
                set.physical_to = optional_number(set_node, "PhysicalTo", diagnostics, set_where);
                set.wheel_slot_index = static_cast<int>(integer(set_node, "WheelSlotIndex", 0, diagnostics, set_where));
                function.channel_sets.push_back(std::move(set));
            }
            logical.functions.push_back(std::move(function));
            pending_functions.push_back(std::move(held));
        }
        if (logical.functions.empty()) {
            diagnostics.warn(logical_where, "logical channel has no channel functions");
        }
        channel.logical_channels.push_back(std::move(logical));
        pending.functions.push_back(std::move(pending_functions));
    }
    if (channel.logical_channels.empty()) {
        diagnostics.error(where, "DMX channel has no logical channel; channel dropped");
        return false;
    }

    channel.bytes = channel.offsets.empty() ? widest(pending) : static_cast<int>(channel.offsets.size());
    for (size_t l = 0; l < channel.logical_channels.size(); ++l) {
        for (size_t f = 0; f < channel.logical_channels[l].functions.size(); ++f) {
            ChannelFunction& function = channel.logical_channels[l].functions[f];
            const PendingFunction& held = pending.functions[l][f];
            function.dmx_from = convert_dmx_value(held.dmx_from, channel.bytes);
            for (size_t s = 0; s < function.channel_sets.size(); ++s) {
                function.channel_sets[s].dmx_from = convert_dmx_value(held.set_from[s], channel.bytes);
            }
        }
    }
    if (pending.highlight) channel.highlight = convert_dmx_value(*pending.highlight, channel.bytes);
    return true;
}

// Finds "<Channel>.<LogicalChannel>.<ChannelFunction>" (or just "<Channel>") in a mode.
struct FunctionLink {
    int channel = -1;
    int logical = -1;
    int function = -1;
};

FunctionLink resolve_link(const DmxMode& mode, const std::string& link) {
    FunctionLink result;
    std::vector<std::string_view> parts = text::split(link, '.');
    result.channel = mode.find_channel(parts[0]);
    if (result.channel < 0 || parts.size() < 3) return result;
    const DmxChannel& channel = mode.channels[static_cast<size_t>(result.channel)];
    for (size_t l = 0; l < channel.logical_channels.size(); ++l) {
        if (channel.logical_channels[l].attribute != parts[1]) continue;
        for (size_t f = 0; f < channel.logical_channels[l].functions.size(); ++f) {
            if (channel.logical_channels[l].functions[f].name == parts[2]) {
                result.logical = static_cast<int>(l);
                result.function = static_cast<int>(f);
                return result;
            }
        }
    }
    return result;
}

// Two functions of one logical channel can be active for the same master state unless both hang
// off the same master with ranges that do not touch.
bool can_coexist(const ChannelFunction& a, const ChannelFunction& b) {
    if (a.mode_master_channel < 0 || b.mode_master_channel < 0) return true;
    if (a.mode_master_channel != b.mode_master_channel) return true;
    return a.mode_from <= b.mode_to && b.mode_from <= a.mode_to;
}

void resolve_mode(DmxMode& mode, std::vector<PendingChannel>& pending, const std::string& where,
                  Diagnostics& diagnostics) {
    for (size_t c = 0; c < mode.channels.size(); ++c) {
        for (size_t other = 0; other < c; ++other) {
            if (mode.channels[other].name == mode.channels[c].name) {
                diagnostics.warn(where, "two DMX channels are named " + quoted(mode.channels[c].name) +
                                            "; links to that name reach the first one");
                break;
            }
        }
    }

    // Mode masters first: function ranges depend on which functions can be active together.
    for (size_t c = 0; c < mode.channels.size(); ++c) {
        DmxChannel& channel = mode.channels[c];
        for (size_t l = 0; l < channel.logical_channels.size(); ++l) {
            for (size_t f = 0; f < channel.logical_channels[l].functions.size(); ++f) {
                ChannelFunction& function = channel.logical_channels[l].functions[f];
                if (function.mode_master.empty()) continue;
                std::string function_where = where + " / DMXChannel " + quoted(channel.name) + " / ChannelFunction " +
                                             quoted(function.name);
                FunctionLink link = resolve_link(mode, function.mode_master);
                if (link.channel < 0) {
                    diagnostics.warn(function_where, "ModeMaster " + quoted(function.mode_master) +
                                                         " does not name a DMX channel of this mode; the function is treated as always available");
                    continue;
                }
                if (text::split(function.mode_master, '.').size() >= 3 && link.function < 0) {
                    diagnostics.warn(function_where, "ModeMaster " + quoted(function.mode_master) +
                                                         " names a channel function that does not exist; the whole channel is used as master");
                }
                function.mode_master_channel = link.channel;
                function.mode_master_logical = link.logical;
                function.mode_master_function = link.function;
                int master_bytes = mode.channels[static_cast<size_t>(link.channel)].bytes;
                const PendingFunction& held = pending[c].functions[l][f];
                function.mode_from = convert_dmx_value(held.mode_from.value_or(DmxValue{}), master_bytes);
                function.mode_to = convert_dmx_value(held.mode_to.value_or(DmxValue{}), master_bytes);
                if (function.mode_from > function.mode_to) {
                    diagnostics.warn(function_where, "ModeFrom is above ModeTo; the function can never be active");
                }
            }
        }
    }

    for (size_t c = 0; c < mode.channels.size(); ++c) {
        DmxChannel& channel = mode.channels[c];
        std::string channel_where = where + " / DMXChannel " + quoted(channel.name);
        const uint32_t channel_max = dmx_max(channel.bytes);

        for (size_t l = 0; l < channel.logical_channels.size(); ++l) {
            std::vector<ChannelFunction>& functions = channel.logical_channels[l].functions;
            for (size_t f = 0; f < functions.size(); ++f) {
                ChannelFunction& function = functions[f];
                function.dmx_to = channel_max;
                for (const ChannelFunction& other : functions) {
                    if (other.dmx_from > function.dmx_from && can_coexist(function, other)) {
                        function.dmx_to = std::min(function.dmx_to, other.dmx_from - 1);
                    }
                }

                const PendingFunction& held = pending[c].functions[l][f];
                // Manufacturer files commonly write a Default of 0 on every function of a channel,
                // which lies outside all but the first; such a function starts at its own DMXFrom.
                function.default_value = function.dmx_from;
                if (held.default_value) {
                    const uint32_t written = convert_dmx_value(*held.default_value, channel.bytes);
                    if (written >= function.dmx_from && written <= function.dmx_to) function.default_value = written;
                }

                for (size_t s = 0; s < function.channel_sets.size(); ++s) {
                    ChannelSet& set = function.channel_sets[s];
                    if (set.dmx_from < function.dmx_from || set.dmx_from > function.dmx_to) {
                        diagnostics.warn(channel_where + " / ChannelFunction " + quoted(function.name),
                                         "ChannelSet " + quoted(set.name) + " starts outside the function's DMX range");
                    }
                    set.dmx_to = function.dmx_to;
                    for (const ChannelSet& other : function.channel_sets) {
                        if (other.dmx_from > set.dmx_from) set.dmx_to = std::min(set.dmx_to, other.dmx_from - 1);
                    }
                    if (set.dmx_to < set.dmx_from) set.dmx_to = set.dmx_from;
                }
            }
        }

        const PendingChannel& held = pending[c];
        bool initial_given = false;
        if (!held.initial_function.empty()) {
            std::vector<std::string_view> parts = text::split(held.initial_function, '.');
            // The link starts at the mode, so its first part is this channel's own name.
            if (parts.size() >= 3) {
                for (size_t l = 0; l < channel.logical_channels.size() && !initial_given; ++l) {
                    if (channel.logical_channels[l].attribute != parts[parts.size() - 2]) continue;
                    for (size_t f = 0; f < channel.logical_channels[l].functions.size(); ++f) {
                        if (channel.logical_channels[l].functions[f].name == parts[parts.size() - 1]) {
                            channel.initial_logical = static_cast<int>(l);
                            channel.initial_function = static_cast<int>(f);
                            initial_given = true;
                            break;
                        }
                    }
                }
            }
            if (!initial_given) {
                diagnostics.warn(channel_where, "InitialFunction " + quoted(held.initial_function) +
                                                    " does not name a function of this channel; the first one is used");
            }
        }
        if (held.legacy_default) {
            uint32_t value = convert_dmx_value(*held.legacy_default, channel.bytes);
            bool placed = false;
            for (size_t l = 0; l < channel.logical_channels.size() && !placed; ++l) {
                std::vector<ChannelFunction>& functions = channel.logical_channels[l].functions;
                for (size_t f = 0; f < functions.size(); ++f) {
                    if (value < functions[f].dmx_from || value > functions[f].dmx_to) continue;
                    if (!pending[c].functions[l][f].default_value) functions[f].default_value = value;
                    if (!initial_given) {
                        channel.initial_logical = static_cast<int>(l);
                        channel.initial_function = static_cast<int>(f);
                    }
                    placed = true;
                    break;
                }
            }
        }
    }

    for (Relation& relation : mode.relations) {
        std::string relation_where = where + " / Relation " + quoted(relation.name);
        relation.master_channel = mode.find_channel(text::split(relation.master, '.')[0]);
        if (relation.master_channel < 0) {
            diagnostics.warn(relation_where, "Master " + quoted(relation.master) + " does not name a DMX channel of this mode");
        }
        FunctionLink follower = resolve_link(mode, relation.follower);
        relation.follower_channel = follower.channel;
        relation.follower_logical = follower.logical;
        relation.follower_function = follower.function;
        if (follower.function < 0) {
            diagnostics.warn(relation_where, "Follower " + quoted(relation.follower) + " does not name a channel function of this mode");
        }
    }
}

void parse_modes(const xml_node& fixture_type, FixtureType& fixture, Diagnostics& diagnostics) {
    size_t mode_index = 0;
    for (const xml_node& mode_node : fixture_type.child("DMXModes").children("DMXMode")) {
        DmxMode mode;
        mode.name = name_or_indexed(mode_node, "DMXMode", mode_index++);
        std::string where = "DMXMode " + quoted(mode.name);
        mode.description = str(mode_node, "Description");
        mode.geometry = str(mode_node, "Geometry");
        if (!fixture.find_top_level_geometry(mode.geometry)) {
            diagnostics.warn(where, "Geometry " + quoted(mode.geometry) + " is not a top-level geometry of this fixture");
        }

        std::vector<PendingChannel> pending;
        for (const xml_node& channel_node : mode_node.child("DMXChannels").children("DMXChannel")) {
            DmxChannel channel;
            PendingChannel held;
            if (!parse_channel(channel_node, where, channel, held, diagnostics)) continue;
            mode.channels.push_back(std::move(channel));
            pending.push_back(std::move(held));
        }
        size_t relation_index = 0;
        for (const xml_node& relation_node : mode_node.child("Relations").children("Relation")) {
            Relation relation;
            relation.name = name_or_indexed(relation_node, "Relation", relation_index++);
            relation.master = str(relation_node, "Master");
            relation.follower = str(relation_node, "Follower");
            relation.type = enumeration(relation_node, "Type", kRelationTypes, RelationType::Multiply, diagnostics,
                                        where + " / Relation " + quoted(relation.name));
            mode.relations.push_back(std::move(relation));
        }
        resolve_mode(mode, pending, where, diagnostics);
        fixture.modes.push_back(std::move(mode));
    }
}

template <typename T>
const T* find_named(const std::vector<T>& items, std::string_view name) {
    for (const T& item : items) {
        if (item.name == name) return &item;
    }
    return nullptr;
}

}  // namespace

const char* to_string(GeometryType type) {
    for (const EnumName<GeometryType>& candidate : kGeometryTypes) {
        if (candidate.value == type) return candidate.name;
    }
    return "Geometry";
}

uint32_t DmxChannel::default_value() const {
    if (logical_channels.empty()) return 0;
    const LogicalChannel& logical = logical_channels[static_cast<size_t>(initial_logical)];
    if (logical.functions.empty()) return 0;
    return logical.functions[static_cast<size_t>(initial_function)].default_value;
}

int DmxMode::find_channel(std::string_view channel_name) const {
    for (size_t i = 0; i < channels.size(); ++i) {
        if (channels[i].name == channel_name) return static_cast<int>(i);
    }
    return -1;
}

const Geometry* FixtureType::find_top_level_geometry(std::string_view geometry_name) const {
    return find_named(geometries, geometry_name);
}

const Wheel* FixtureType::find_wheel(std::string_view wheel_name) const { return find_named(wheels, wheel_name); }

const Filter* FixtureType::find_filter(std::string_view filter_name) const { return find_named(filters, filter_name); }

const Emitter* FixtureType::find_emitter(std::string_view emitter_name) const {
    return find_named(emitters, emitter_name);
}

const GdtfAttribute* FixtureType::find_attribute(std::string_view attribute_name) const {
    return find_named(attributes, attribute_name);
}

int FixtureType::find_mode(std::string_view mode_name) const {
    for (size_t i = 0; i < modes.size(); ++i) {
        if (modes[i].name == mode_name) return static_cast<int>(i);
    }
    return -1;
}

GdtfResult parse_gdtf_description(std::string_view xml) {
    GdtfResult result;
    // Two of the seven fixtures in grandMA3's demo rig end their XML with a NUL byte.
    while (!xml.empty() && (xml.back() == '\0' || xml.back() == '\n' || xml.back() == '\r' || xml.back() == ' ')) {
        xml.remove_suffix(1);
    }
    pugi::xml_document document;
    pugi::xml_parse_result parsed = document.load_buffer(xml.data(), xml.size(), pugi::parse_default, pugi::encoding_auto);
    if (!parsed) {
        result.diagnostics.error("description.xml", std::string("XML error at byte ") + std::to_string(parsed.offset) +
                                                        ": " + parsed.description());
        return result;
    }
    xml_node root = document.child("GDTF");
    if (!root) {
        result.diagnostics.error("description.xml", "no <GDTF> root node");
        return result;
    }
    xml_node fixture_type = root.child("FixtureType");
    if (!fixture_type) {
        result.diagnostics.error("description.xml", "no <FixtureType> node");
        return result;
    }

    FixtureType& fixture = result.fixture;
    fixture.data_version = str(root, "DataVersion");
    fixture.name = str(fixture_type, "Name");
    fixture.short_name = str(fixture_type, "ShortName");
    fixture.long_name = str(fixture_type, "LongName");
    fixture.manufacturer = str(fixture_type, "Manufacturer");
    fixture.description = str(fixture_type, "Description");
    fixture.fixture_type_id = str(fixture_type, "FixtureTypeID");
    fixture.thumbnail = str(fixture_type, "Thumbnail");
    fixture.ref_ft = str(fixture_type, "RefFT");
    fixture.can_have_children = enumeration(fixture_type, "CanHaveChildren", kYesNo, true, result.diagnostics, "FixtureType");

    fixture.weight = std::max(0.0, number(fixture_type.child("PhysicalDescriptions").child("Properties").child("Weight"), "Value", 0,
                                          result.diagnostics, "Properties"));
    parse_attributes(fixture_type, fixture, result.diagnostics);
    parse_wheels(fixture_type, fixture, result.diagnostics);
    parse_emitters(fixture_type, fixture, result.diagnostics);
    parse_filters(fixture_type, fixture, result.diagnostics);
    parse_models(fixture_type, fixture, result.diagnostics);
    parse_geometry_children(fixture_type.child("Geometries"), fixture.geometries, result.diagnostics, 0);
    parse_modes(fixture_type, fixture, result.diagnostics);

    if (fixture.geometries.empty()) result.diagnostics.warn("FixtureType", "fixture has no geometries");
    if (fixture.modes.empty()) result.diagnostics.warn("FixtureType", "fixture has no DMX modes");
    result.ok = true;
    return result;
}

GdtfResult load_gdtf(const Archive& archive) {
    std::vector<uint8_t> xml;
    if (!archive.read("description.xml", xml)) {
        GdtfResult result;
        result.diagnostics.error("archive", "no description.xml in the archive");
        return result;
    }
    return parse_gdtf_description(std::string_view(reinterpret_cast<const char*>(xml.data()), xml.size()));
}

GdtfResult load_gdtf_file(const std::string& path) {
    Archive archive;
    std::string error;
    if (!archive.open_file(path, error)) {
        GdtfResult result;
        result.diagnostics.error(path, error);
        return result;
    }
    return load_gdtf(archive);
}

}  // namespace viz
