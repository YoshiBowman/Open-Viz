#include "vizcore/fixture_state.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <string>
#include <string_view>

namespace viz {

namespace {

double clamp01(double value) { return value < 0.0 ? 0.0 : (value > 1.0 ? 1.0 : value); }

double largest(const ColorRGB& c) { return std::max(c.r, std::max(c.g, c.b)); }

// Scales a colour so its largest component is 1. Returns that component (0 for black).
double normalise(ColorRGB& c) {
    c.r = std::max(c.r, 0.0);
    c.g = std::max(c.g, 0.0);
    c.b = std::max(c.b, 0.0);
    const double peak = largest(c);
    if (peak > 0.0) {
        c.r /= peak;
        c.g /= peak;
        c.b /= peak;
    }
    return peak;
}

bool starts_with(std::string_view text, std::string_view prefix) {
    return text.size() >= prefix.size() && text.compare(0, prefix.size(), prefix) == 0;
}

// True if `text` is `prefix`, one or more digits, then anything; `rest` gets the "anything" and
// `number` what the digits say.
// Whether a name says a shutter is open (1) or closed (-1); 0 when it says neither, or both ("Close/Open").
int open_or_closed(std::string_view name) {
    std::string lower(name);
    for (char& ch : lower) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    const bool open = lower.find("open") != std::string::npos;
    const bool closed = lower.find("close") != std::string::npos || lower.find("blackout") != std::string::npos;
    return open == closed ? 0 : open ? 1 : -1;
}

bool numbered(std::string_view text, std::string_view prefix, int& number, std::string_view& rest) {
    if (!starts_with(text, prefix)) return false;
    size_t i = prefix.size();
    const size_t first_digit = i;
    int value = 0;
    while (i < text.size() && text[i] >= '0' && text[i] <= '9') {
        if (value < 100000) value = value * 10 + (text[i] - '0');
        ++i;
    }
    if (i == first_digit) return false;
    number = value;
    rest = text.substr(i);
    return true;
}

bool numbered(std::string_view text, std::string_view prefix, std::string_view& rest) {
    int number = 0;
    return numbered(text, prefix, number, rest);
}

// The physical value a function's default DMX value stands for, by its plain linear mapping.
double physical_at_default(const ChannelFunction& function) {
    if (function.dmx_to <= function.dmx_from) return function.physical_from;
    const double along = (static_cast<double>(function.default_value) - static_cast<double>(function.dmx_from)) /
                         (static_cast<double>(function.dmx_to) - static_cast<double>(function.dmx_from));
    return function.physical_from + clamp01(along) * (function.physical_to - function.physical_from);
}

bool open_by_name(const WheelSlot& slot) {
    static const char open[] = "open";
    if (slot.name.size() < 4) return slot.name.empty();
    for (size_t i = 0; i < 4; ++i) {
        const char c = slot.name[i];
        if ((c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c) != open[i]) return false;
    }
    return true;
}

// What an additive emitter looks like when the file does not link one: by the attribute's suffix.
ColorRGB default_emitter(std::string_view suffix) {
    struct Named {
        const char* suffix;
        ColorRGB color;
    };
    static const Named table[] = {
        {"R", {1.0, 0.0, 0.0}},   {"G", {0.0, 1.0, 0.0}},   {"B", {0.0, 0.0, 1.0}},   {"W", {1.0, 1.0, 1.0}},
        {"WW", {1.0, 0.72, 0.42}}, {"CW", {0.78, 0.88, 1.0}}, {"A", {1.0, 0.5, 0.0}},   {"UV", {0.25, 0.0, 0.6}},
        {"C", {0.0, 1.0, 1.0}},   {"M", {1.0, 0.0, 1.0}},   {"Y", {1.0, 1.0, 0.0}},   {"RY", {1.0, 0.35, 0.0}},
        {"GY", {0.55, 1.0, 0.0}}, {"GC", {0.0, 1.0, 0.5}},  {"BC", {0.0, 0.5, 1.0}},  {"BM", {0.35, 0.0, 1.0}},
        {"RM", {1.0, 0.0, 0.5}},
    };
    for (const Named& entry : table) {
        if (suffix == entry.suffix) return entry.color;
    }
    return ColorRGB();
}

double srgb_to_linear(double encoded) {
    return encoded <= 0.04045 ? encoded / 12.92 : std::pow((encoded + 0.055) / 1.055, 2.4);
}

}  // namespace

ColorRGB to_linear_srgb(const ColorCIE& color) {
    ColorRGB result{0.0, 0.0, 0.0};
    if (color.y <= 0.0) return result;
    const double Y = color.Y / 100.0;
    const double X = color.x * Y / color.y;
    const double Z = (1.0 - color.x - color.y) * Y / color.y;
    result.r = 3.2404542 * X - 1.5371385 * Y - 0.4985314 * Z;
    result.g = -0.9692660 * X + 1.8760108 * Y + 0.0415560 * Z;
    result.b = 0.0556434 * X - 0.2040259 * Y + 1.0572252 * Z;
    return result;
}

ColorRGB color_temperature_rgb(double kelvin) {
    // Tanner Helland's fit to the black-body locus, in gamma-encoded sRGB.
    const double t = std::min(std::max(kelvin, 1000.0), 40000.0) / 100.0;
    double r = t <= 66.0 ? 255.0 : 329.698727446 * std::pow(t - 60.0, -0.1332047592);
    double g = t <= 66.0 ? 99.4708025861 * std::log(t) - 161.1195681661 : 288.1221695283 * std::pow(t - 60.0, -0.0755148492);
    double b = t >= 66.0 ? 255.0 : (t <= 19.0 ? 0.0 : 138.5177312231 * std::log(t - 10.0) - 305.0447927307);
    ColorRGB result;
    result.r = srgb_to_linear(clamp01(r / 255.0));
    result.g = srgb_to_linear(clamp01(g / 255.0));
    result.b = srgb_to_linear(clamp01(b / 255.0));
    normalise(result);
    return result;
}

FixtureModel::FixtureModel(const FixtureType& fixture, size_t mode_index)
    : evaluator_(fixture, mode_index) {
    if (!evaluator_.valid()) return;
    const DmxMode& mode = evaluator_.mode();

    // Functions that are the same control wherever they appear share a slot, so that only the
    // one nearest a beam is used.
    std::vector<std::string> slot_names;
    auto slot_for = [&slot_names](std::string_view name) {
        for (size_t i = 0; i < slot_names.size(); ++i) {
            if (slot_names[i] == name) return static_cast<int>(i);
        }
        slot_names.emplace_back(name);
        return static_cast<int>(slot_names.size() - 1);
    };

    channels_.resize(mode.channels.size());
    for (size_t c = 0; c < mode.channels.size(); ++c) {
        const DmxChannel& channel = mode.channels[c];
        ChannelInfo& info = channels_[c];
        info.is_virtual = channel.offsets.empty();
        for (const LogicalChannel& logical : channel.logical_channels) {
            info.first_function.push_back(info.functions.size());
            for (const ChannelFunction& function : logical.functions) {
                FunctionInfo entry;
                const std::string_view attribute = function.attribute;
                std::string_view rest;
                int number = 0;
                if (attribute == "Pan") {
                    entry.kind = Kind::Pan;
                } else if (attribute == "Tilt") {
                    entry.kind = Kind::Tilt;
                } else if (attribute.size() == 5 && (attribute.substr(0, 4) == "XYZ_" || attribute.substr(0, 4) == "Rot_") &&
                           attribute[4] >= 'X' && attribute[4] <= 'Z') {
                    entry.kind = attribute[0] == 'X' ? Kind::Slide : Kind::Turn;
                    entry.component = attribute[4] - 'X';
                } else if (attribute == "Dimmer") {
                    entry.kind = Kind::Dimmer;
                } else if ((numbered(attribute, "Fog", number, rest) && rest.empty()) || (numbered(attribute, "Haze", number, rest) && rest.empty()) ||
                           attribute == "Fog" || attribute == "Haze") {
                    // "Fog", "Haze", and numbered as GDTF numbers them ("Haze1": MDG's ATMe has its output level so).
                    entry.kind = Kind::Fog;
                    entry.on_off = !function.channel_sets.empty();
                    for (const ChannelSet& set : function.channel_sets) {
                        const bool fixed = set.physical_from && set.physical_to && *set.physical_from == *set.physical_to &&
                                           (*set.physical_from == 0.0 || *set.physical_from == 1.0);
                        entry.on_off = entry.on_off && fixed;
                    }
                    if (!info.is_virtual) has_fog_ = true;
                } else if (attribute == "Zoom") {
                    entry.kind = Kind::Zoom;
                    entry.slot = slot_for("Zoom");
                } else if (numbered(attribute, "Shutter", rest)) {
                    if (rest.empty()) {
                        entry.kind = Kind::Shutter;
                    } else if (starts_with(rest, "Strobe")) {
                        entry.kind = Kind::Strobe;
                        entry.slot = slot_for("Strobe");
                        const std::string_view how = rest.substr(6);
                        entry.shape = how == "Pulse" ? StrobeShape::Pulse
                                    : how == "PulseClose" ? StrobeShape::RampDown
                                    : how == "PulseOpen" ? StrobeShape::RampUp
                                    : how == "Random" ? StrobeShape::Random
                                    : how == "RandomPulse" ? StrobeShape::RandomPulse
                                    : how == "RandomPulseClose" ? StrobeShape::RandomRampDown
                                    : how == "RandomPulseOpen" ? StrobeShape::RandomRampUp
                                    : StrobeShape::Square;
                    }
                } else if (starts_with(attribute, "ColorAdd_")) {
                    entry.kind = Kind::Additive;
                    entry.slot = slot_for(attribute);
                    entry.color = default_emitter(attribute.substr(9));
                    const Emitter* emitter = function.emitter.empty() ? nullptr : fixture.find_emitter(function.emitter);
                    if (emitter && emitter->color) {
                        // Only the hue is taken: emitter luminances in real files are not on a
                        // common scale, and all emitters at full is expected to read as white.
                        ColorRGB measured = to_linear_srgb(*emitter->color);
                        if (normalise(measured) > 0.0) entry.color = measured;
                    }
                } else if (attribute == "ColorRGB_Red" || attribute == "ColorRGB_Green" || attribute == "ColorRGB_Blue") {
                    entry.kind = Kind::DirectRgb;
                    entry.component = attribute == "ColorRGB_Red" ? 0 : (attribute == "ColorRGB_Green" ? 1 : 2);
                    entry.slot = slot_for(attribute);
                } else if (attribute == "ColorSub_C" || attribute == "ColorSub_M" || attribute == "ColorSub_Y") {
                    entry.kind = Kind::Subtractive;
                    entry.component = attribute == "ColorSub_C" ? 0 : (attribute == "ColorSub_M" ? 1 : 2);
                    entry.slot = slot_for(attribute);
                    // What the flag lets through when fully in: ideally everything but its own
                    // third of the spectrum; really, whatever the manufacturer measured.
                    entry.color = ColorRGB{entry.component == 0 ? 0.0 : 1.0, entry.component == 1 ? 0.0 : 1.0, entry.component == 2 ? 0.0 : 1.0};
                    const Filter* glass = function.filter.empty() ? nullptr : fixture.find_filter(function.filter);
                    if (glass && glass->color) {
                        ColorRGB measured = to_linear_srgb(*glass->color);
                        measured = ColorRGB{clamp01(measured.r), clamp01(measured.g), clamp01(measured.b)};
                        if (normalise(measured) > 0.0) entry.color = measured;
                    }
                } else if (numbered(attribute, "Color", rest) && (rest.empty() || rest == "WheelSpin" || rest == "WheelIndex")) {
                    // One wheel, three ways of driving it; whichever the channel is doing counts.
                    entry.kind = rest.empty() ? Kind::Wheel : rest == "WheelSpin" ? Kind::WheelSpin : Kind::WheelIndex;
                    entry.slot = slot_for(attribute.substr(0, attribute.size() - rest.size()));
                    entry.wheel = function.wheel.empty() ? nullptr : fixture.find_wheel(function.wheel);
                } else if (attribute == "CTO" || attribute == "CTC" || attribute == "CTB") {
                    entry.kind = Kind::Temperature;
                    entry.slot = slot_for(attribute);
                } else if ((numbered(attribute, "Gobo", number, rest) && number >= 1 && number <= 2) ||
                           (numbered(attribute, "AnimationWheel", number, rest) && number == 1)) {
                    struct Named {
                        const char* suffix;
                        Role role;
                    };
                    static const Named roles[] = {{"", Select},          {"SelectSpin", SelectSpin}, {"SelectShake", SelectShake},
                                                  {"SelectEffects", Select}, {"WheelSpin", WheelTurn},   {"WheelIndex", WheelHold},
                                                  {"WheelShake", WheelShake}, {"Pos", Pos},               {"PosRotate", PosRotate},
                                                  {"PosShake", PosShake}};
                    for (const Named& named : roles) {
                        if (rest != named.suffix) continue;
                        entry.kind = Kind::Gate;
                        entry.component = named.role;
                        entry.index = attribute[0] == 'A' ? kGateLayers - 1 : number - 1;
                        // Choosing the slot is one control; turning it in its holder, usually on a channel of its own, another.
                        std::string control(attribute.substr(0, attribute.size() - rest.size()));
                        if (named.role == Pos || named.role == PosRotate || named.role == PosShake) control += "Pos";
                        entry.slot = slot_for(control);
                        entry.wheel = function.wheel.empty() ? nullptr : fixture.find_wheel(function.wheel);
                        break;
                    }
                } else if (numbered(attribute, "Prism", number, rest) && number >= 1 && number <= kPrisms &&
                           (rest.empty() || rest == "SelectSpin" || rest == "Pos" || rest == "PosRotate")) {
                    entry.kind = Kind::Prism;
                    entry.component = rest.empty() ? Select : rest == "SelectSpin" ? SelectSpin : rest == "Pos" ? Pos : PosRotate;
                    entry.index = number - 1;
                    std::string control(attribute.substr(0, attribute.size() - rest.size()));
                    if (entry.component == Pos || entry.component == PosRotate) control += "Pos";
                    entry.slot = slot_for(control);
                    entry.wheel = function.wheel.empty() ? nullptr : fixture.find_wheel(function.wheel);
                } else if (attribute == "Iris") {
                    entry.kind = Kind::Iris;
                    entry.slot = slot_for("Iris");
                } else if (starts_with(attribute, "Iris")) {
                    const std::string_view how = attribute.substr(4);
                    entry.shape = how == "Strobe" ? StrobeShape::Square
                                : how == "StrobeRandom" ? StrobeShape::Random
                                : how == "PulseClose" ? StrobeShape::RampDown
                                : how == "PulseOpen" ? StrobeShape::RampUp
                                : how == "RandomPulseClose" ? StrobeShape::RandomRampDown
                                : how == "RandomPulseOpen" ? StrobeShape::RandomRampUp
                                : StrobeShape::Pulse;
                    if (entry.shape != StrobeShape::Pulse) {    // Pulse stands for "none of these" here
                        entry.kind = Kind::IrisPulse;
                        entry.slot = slot_for("Iris");
                    }
                } else if (numbered(attribute, "Frost", number, rest) && rest.empty()) {
                    entry.kind = Kind::Frost;
                    entry.slot = slot_for(attribute);
                } else if (numbered(attribute, "Focus", number, rest) && rest.empty()) {
                    entry.kind = Kind::Focus;
                    entry.slot = slot_for(attribute);
                    entry.reference = physical_at_default(function);
                } else if ((numbered(attribute, "Blade", number, rest) || numbered(attribute, "BladeSoft", number, rest)) && number >= 1 &&
                           number <= 4 && (rest == "A" || rest == "B" || rest == "Rot")) {
                    entry.kind = Kind::Blade;
                    entry.component = rest == "A" ? 0 : rest == "B" ? 1 : 2;
                    entry.index = number - 1;
                    entry.slot = slot_for(attribute);
                } else if (attribute == "ShaperRot") {
                    entry.kind = Kind::ShaperRot;
                    entry.slot = slot_for(attribute);
                }
                info.functions.push_back(entry);
            }
        }
    }
    slot_count_ = static_cast<int>(slot_names.size());

    // A pulsing iris closes as far as the fixture's plain iris control goes.
    double closed = 1.0;
    bool has_iris = false;
    for (size_t c = 0; c < mode.channels.size(); ++c) {
        size_t f = 0;
        for (const LogicalChannel& logical : mode.channels[c].logical_channels) {
            for (const ChannelFunction& function : logical.functions) {
                if (channels_[c].functions[f++].kind != Kind::Iris) continue;
                has_iris = true;
                closed = std::min(closed, std::min(function.physical_from, function.physical_to));
            }
        }
    }
    closed = has_iris ? clamp01(closed) : 0.15;
    for (ChannelInfo& info : channels_) {
        for (FunctionInfo& function : info.functions) {
            if (function.kind == Kind::IrisPulse) function.reference = closed;
        }
    }

    const std::vector<ChannelInstance>& instances = evaluator_.channels();
    const std::vector<GeometryInstance>& tree = evaluator_.geometry();

    for (size_t i = 0; i < instances.size(); ++i) {
        if (instances[i].geometry_index < 0) continue;
        for (const FunctionInfo& function : channels_[static_cast<size_t>(instances[i].channel)].functions) {
            AxisBinding binding;
            if (!moves(function, binding.motion, binding.axis)) continue;
            binding.geometry = instances[i].geometry_index;
            binding.channel_instance = static_cast<int>(i);
            axes_.push_back(binding);
            break;
        }
    }

    for (size_t g = 0; g < tree.size(); ++g) {
        if (!tree[g].geometry->beam) continue;
        BeamBinding binding;
        binding.geometry = static_cast<int>(g);
        binding.properties = &*tree[g].geometry->beam;
        for (int node = static_cast<int>(g); node >= 0; node = tree[static_cast<size_t>(node)].parent) {
            for (size_t i = 0; i < instances.size(); ++i) {
                if (instances[i].geometry_index == node) binding.channel_instances.push_back(static_cast<int>(i));
            }
        }
        beams_.push_back(std::move(binding));
    }
    find_clusters();
}

void FixtureModel::find_clusters() {
    cluster_of_.assign(beams_.size(), -1);
    clusters_.clear();
    if (beams_.size() < 2) return;
    const std::vector<GeometryInstance>& tree = evaluator_.geometry();

    // Where every part is at rest in the fixture's own frame, and the nearest part at or above
    // it that an axis moves: beams that share that one move as one.
    std::vector<Matrix4> placed(tree.size());
    std::vector<int> mover(tree.size(), -1);
    std::vector<char> moved(tree.size(), 0);
    for (const AxisBinding& axis : axes_) {
        if (axis.geometry >= 0 && static_cast<size_t>(axis.geometry) < tree.size()) moved[static_cast<size_t>(axis.geometry)] = 1;
    }
    for (size_t i = 0; i < tree.size(); ++i) {
        const int parent = tree[i].parent;
        placed[i] = parent >= 0 ? multiply(placed[static_cast<size_t>(parent)], tree[i].local) : tree[i].local;
        mover[i] = moved[i] ? static_cast<int>(i) : (parent >= 0 ? mover[static_cast<size_t>(parent)] : -1);
    }

    struct Lens {
        bool fit = false;       // may be clustered at all
        bool wide = false;      // its light is a glow rather than a shaft
        double at[3] = {0, 0, 0};
        double along[3] = {0, 0, -1};
        double radius = 0.01;
        double field = 25.0;
        int mover = -1;
        BeamType type = BeamType::Wash;
    };
    std::vector<Lens> lenses(beams_.size());
    for (size_t b = 0; b < beams_.size(); ++b) {
        const BeamBinding& binding = beams_[b];
        const size_t g = static_cast<size_t>(binding.geometry);
        Lens& lens = lenses[b];
        lens.type = binding.properties->beam_type;
        lens.field = std::max(binding.properties->field_angle, binding.properties->beam_angle);
        lens.wide = lens.field >= 100.0;
        lens.radius = std::max(binding.properties->beam_radius, 0.005);
        lens.mover = mover[g];
        double length = 0.0;
        for (int k = 0; k < 3; ++k) {
            lens.at[k] = placed[g].m[k][3];
            lens.along[k] = -placed[g].m[k][2];    // a beam shines along its own -Z
            length += lens.along[k] * lens.along[k];
        }
        length = std::sqrt(length);
        if (length > 1e-9) {
            for (double& part : lens.along) part /= length;
        }
        // Not a beam that is only a glowing surface, not one that moves by itself, and not one whose path has anything in it.
        bool shaped = false;
        for (int instance : binding.channel_instances) {
            const ChannelInfo& info = channels_[static_cast<size_t>(evaluator_.channels()[static_cast<size_t>(instance)].channel)];
            for (const FunctionInfo& function : info.functions) {
                shaped = shaped || function.kind == Kind::Gate || function.kind == Kind::Prism || function.kind == Kind::Iris ||
                         function.kind == Kind::IrisPulse || function.kind == Kind::Blade || function.kind == Kind::ShaperRot;
            }
        }
        lens.fit = length > 1e-9 && !shaped && lens.type != BeamType::None && lens.type != BeamType::Glow && lens.mover != binding.geometry;
    }

    // Lenses that belong together: side by side for shafts, anywhere near for glows.
    std::vector<int> group(beams_.size());
    for (size_t b = 0; b < group.size(); ++b) group[b] = static_cast<int>(b);
    auto root = [&group](int b) {
        while (group[static_cast<size_t>(b)] != b) b = group[static_cast<size_t>(b)] = group[static_cast<size_t>(group[static_cast<size_t>(b)])];
        return b;
    };
    for (size_t a = 0; a < lenses.size(); ++a) {
        if (!lenses[a].fit) continue;
        for (size_t b = a + 1; b < lenses.size(); ++b) {
            const Lens& one = lenses[a];
            const Lens& other = lenses[b];
            if (!other.fit || one.mover != other.mover || one.type != other.type || one.wide != other.wide || std::fabs(one.field - other.field) > 5.0) continue;
            double dot = 0.0, across2 = 0.0, axial = 0.0;
            for (int k = 0; k < 3; ++k) {
                dot += one.along[k] * other.along[k];
                axial += (other.at[k] - one.at[k]) * one.along[k];
            }
            for (int k = 0; k < 3; ++k) {
                const double across = (other.at[k] - one.at[k]) - axial * one.along[k];
                across2 += across * across;
            }
            if (dot < 0.9995) continue;
            const double reach = one.wide ? 0.7 : 1.6 * (one.radius + other.radius);
            if (std::fabs(axial) > (one.wide ? 0.1 : 0.03) || across2 > reach * reach) continue;
            group[static_cast<size_t>(root(static_cast<int>(b)))] = root(static_cast<int>(a));
        }
    }

    // A group is a cluster if it is compact; a glow's group that is too big is cut into lengths.
    std::vector<std::vector<int>> groups(beams_.size());
    for (size_t b = 0; b < lenses.size(); ++b) {
        if (lenses[b].fit) groups[static_cast<size_t>(root(static_cast<int>(b)))].push_back(static_cast<int>(b));
    }
    auto close_cluster = [&](const std::vector<int>& members, bool must_be_round) {
        if (members.size() < 2) return;
        double middle[3] = {0, 0, 0};
        for (int member : members) {
            for (int k = 0; k < 3; ++k) middle[k] += lenses[static_cast<size_t>(member)].at[k] / static_cast<double>(members.size());
        }
        BeamCluster cluster;
        double nearest = 1e30, lens_area = 0.0;
        for (int member : members) {
            const Lens& lens = lenses[static_cast<size_t>(member)];
            double distance = 0.0;
            for (int k = 0; k < 3; ++k) distance += (lens.at[k] - middle[k]) * (lens.at[k] - middle[k]);
            distance = std::sqrt(distance);
            cluster.radius = std::max(cluster.radius, distance + lens.radius);
            lens_area += lens.radius * lens.radius;
            if (distance < nearest) {
                nearest = distance;
                cluster.leader = member;
            }
        }
        // Round enough: the lenses fill a fair share of the circle that holds them. Three in a row do; a batten's twenty do not.
        if (must_be_round && (cluster.radius > 0.35 || lens_area < 0.3 * cluster.radius * cluster.radius)) return;
        cluster.members = members;
        for (int member : members) cluster_of_[static_cast<size_t>(member)] = static_cast<int>(clusters_.size());
        clusters_.push_back(std::move(cluster));
    };
    for (const std::vector<int>& members : groups) {
        if (members.size() < 2) continue;
        if (!lenses[static_cast<size_t>(members[0])].wide) {
            close_cluster(members, true);
            continue;
        }
        // The longest way across the group, along one of the fixture's own axes: cut there into equal lengths of at most half a metre.
        double low[3] = {1e30, 1e30, 1e30}, high[3] = {-1e30, -1e30, -1e30};
        for (int member : members) {
            for (int k = 0; k < 3; ++k) {
                low[k] = std::min(low[k], lenses[static_cast<size_t>(member)].at[k]);
                high[k] = std::max(high[k], lenses[static_cast<size_t>(member)].at[k]);
            }
        }
        int longest = 0;
        for (int k = 1; k < 3; ++k) {
            if (high[k] - low[k] > high[longest] - low[longest]) longest = k;
        }
        const double extent = high[longest] - low[longest];
        const int pieces = extent > 0.7 ? static_cast<int>(std::ceil(extent / 0.5)) : 1;
        std::vector<std::vector<int>> cut(static_cast<size_t>(pieces));
        for (int member : members) {
            int piece = pieces > 1 ? static_cast<int>((lenses[static_cast<size_t>(member)].at[longest] - low[longest]) / extent * pieces) : 0;
            cut[static_cast<size_t>(std::min(std::max(piece, 0), pieces - 1))].push_back(member);
        }
        for (const std::vector<int>& piece : cut) close_cluster(piece, false);
    }
}

const FixtureModel::FunctionInfo* FixtureModel::active(int channel_instance, const ChannelValue& value) const {
    if (value.function < 0 || value.logical_channel < 0) return nullptr;
    const ChannelInfo& info = channels_[static_cast<size_t>(evaluator_.channels()[static_cast<size_t>(channel_instance)].channel)];
    const size_t index = info.first_function[static_cast<size_t>(value.logical_channel)] + static_cast<size_t>(value.function);
    return index < info.functions.size() ? &info.functions[index] : nullptr;
}

Matrix4 axis_matrix(const AxisState& axis, double value) {
    Matrix4 out;
    const int about = axis.axis == 'X' ? 0 : axis.axis == 'Y' ? 1 : 2;
    if (axis.motion == Motion::Slide) {
        out.m[about][3] = value;
        return out;
    }
    const double radians = value * 3.14159265358979323846 / 180.0;
    const double c = std::cos(radians), s = std::sin(radians);
    // A right-handed turn about the axis: the next axis round turns toward the one after it.
    const int a = (about + 1) % 3, b = (about + 2) % 3;
    out.m[a][a] = c;
    out.m[a][b] = -s;
    out.m[b][a] = s;
    out.m[b][b] = c;
    return out;
}

int FixtureModel::carrier() const {
    int found = -1;
    for (const AxisBinding& binding : axes_) {
        if (binding.motion == Motion::Slide || binding.motion == Motion::Turn) found = std::max(found, binding.geometry);
    }
    return found;
}

bool FixtureModel::moves(const FunctionInfo& info, Motion& motion, char& axis) {
    switch (info.kind) {
        case Kind::Pan: motion = Motion::Pan; axis = 'Z'; return true;
        case Kind::Tilt: motion = Motion::Tilt; axis = 'X'; return true;
        case Kind::Slide: motion = Motion::Slide; axis = static_cast<char>('X' + info.component); return true;
        case Kind::Turn: motion = Motion::Turn; axis = static_cast<char>('X' + info.component); return true;
        default: return false;
    }
}

void FixtureModel::set_dimmers(std::vector<uint8_t>& frame, double level) const {
    if (!evaluator_.valid()) return;
    const DmxMode& mode = evaluator_.mode();
    for (const ChannelInstance& instance : evaluator_.channels()) {
        if (instance.dmx_break != 1 || instance.offsets.empty()) continue;
        const DmxChannel& channel = mode.channels[static_cast<size_t>(instance.channel)];
        // A dimmer channel is one whose resting function is the dimmer.
        const ChannelInfo& info = channels_[static_cast<size_t>(instance.channel)];
        const size_t resting = info.first_function[static_cast<size_t>(channel.initial_logical)] + static_cast<size_t>(channel.initial_function);
        if (resting >= info.functions.size() || info.functions[resting].kind != Kind::Dimmer) continue;
        const ChannelFunction& function = channel.logical_channels[static_cast<size_t>(channel.initial_logical)].functions[static_cast<size_t>(channel.initial_function)];
        // Full is whichever end of the function's range has the larger physical value.
        const bool rising = function.physical_to >= function.physical_from;
        const double position = rising ? clamp01(level) : 1.0 - clamp01(level);
        uint32_t value = function.dmx_from + static_cast<uint32_t>(position * static_cast<double>(function.dmx_to - function.dmx_from) + 0.5);
        for (size_t i = instance.offsets.size(); i-- > 0;) {
            const size_t slot = static_cast<size_t>(instance.offsets[i] - 1);
            if (slot < frame.size()) frame[slot] = static_cast<uint8_t>(value & 0xFFu);
            value >>= 8;
        }
    }
}

std::vector<RestBeam> rest_beams(const FixtureType& fixture, size_t mode_index) {
    std::vector<RestBeam> out;
    const FixtureModel model(fixture, mode_index);
    if (!model.valid()) return out;
    std::vector<uint8_t> frame = model.evaluator().default_frame();
    model.set_dimmers(frame, 1.0);
    std::vector<ChannelValue> values;
    model.evaluator().evaluate(frame.data(), frame.size(), values);
    FixtureState state;
    model.resolve(values, state);

    // Every part's place in the fixture: its parent's, then its own rest position, then its turn
    // about its own axis where pan or tilt moves it.
    const std::vector<GeometryInstance>& tree = model.geometry();
    std::vector<Matrix4> placed(tree.size());
    for (size_t i = 0; i < tree.size(); ++i) {
        // Turned about its own origin first, then slid: a slide is along the part's own axes as
        // they stand at rest, whichever way the part has been turned.
        Matrix4 own = tree[i].local;
        for (int pass = 0; pass < 2; ++pass) {
            for (const AxisState& axis : state.axes) {
                if (axis.geometry != static_cast<int>(i) || (axis.motion == Motion::Slide) != (pass == 0)) continue;
                own = multiply(own, axis_matrix(axis, axis.target));
            }
        }
        placed[i] = tree[i].parent >= 0 ? multiply(placed[static_cast<size_t>(tree[i].parent)], own) : own;
    }
    for (const BeamState& beam : state.beams) {
        if (beam.geometry < 0 || static_cast<size_t>(beam.geometry) >= tree.size() || !tree[static_cast<size_t>(beam.geometry)].geometry->beam) continue;
        const BeamProperties& properties = *tree[static_cast<size_t>(beam.geometry)].geometry->beam;
        RestBeam rest;
        rest.geometry = beam.geometry;
        rest.world = placed[static_cast<size_t>(beam.geometry)];
        rest.beam_type = properties.beam_type;
        rest.lumens = properties.luminous_flux;
        rest.beam_angle = beam.beam_angle;
        rest.field_angle = beam.field_angle;
        rest.intensity = beam.intensity;
        out.push_back(rest);
    }
    return out;
}

bool aim_at(Transform& world, const std::vector<RestBeam>& beams, const double target[3]) {
    if (beams.empty()) return false;
    double centre[3] = {0, 0, 0}, along[3] = {0, 0, 0};
    for (const RestBeam& beam : beams) {
        for (int k = 0; k < 3; ++k) {
            centre[k] += beam.world.m[k][3] * 1000.0 / static_cast<double>(beams.size());    // metres to the placing's millimetres
            along[k] -= beam.world.m[k][2];                                                  // a beam shines along its own -Z
        }
    }
    auto unit = [](double v[3]) {
        const double length = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
        if (length < 1e-12) return false;
        for (int k = 0; k < 3; ++k) v[k] /= length;
        return true;
    };
    if (!unit(along)) return false;
    Transform turned = world;
    // The lens moves as the fixture turns about its origin, so the turn is found again from where it now is.
    for (int pass = 0; pass < 6; ++pass) {
        double lens[3], now[3], wanted[3];
        for (int k = 0; k < 3; ++k) {
            lens[k] = turned.o[k] + centre[0] * turned.u[k] + centre[1] * turned.v[k] + centre[2] * turned.w[k];
            now[k] = along[0] * turned.u[k] + along[1] * turned.v[k] + along[2] * turned.w[k];
            wanted[k] = target[k] - lens[k];
        }
        if (std::sqrt(wanted[0] * wanted[0] + wanted[1] * wanted[1] + wanted[2] * wanted[2]) < 1.0) return false;    // within a millimetre
        if (!unit(now) || !unit(wanted)) return false;
        double axis[3] = {now[1] * wanted[2] - now[2] * wanted[1], now[2] * wanted[0] - now[0] * wanted[2], now[0] * wanted[1] - now[1] * wanted[0]};
        const double cosine = now[0] * wanted[0] + now[1] * wanted[1] + now[2] * wanted[2];
        double sine = std::sqrt(axis[0] * axis[0] + axis[1] * axis[1] + axis[2] * axis[2]);
        if (sine < 1e-12) {
            if (cosine > 0.0) break;    // already there
            // Straight away from it: half a turn about any line across the beam.
            const double across[3] = {std::fabs(now[0]) < 0.9 ? 1.0 : 0.0, std::fabs(now[0]) < 0.9 ? 0.0 : 1.0, 0.0};
            axis[0] = now[1] * across[2] - now[2] * across[1];
            axis[1] = now[2] * across[0] - now[0] * across[2];
            axis[2] = now[0] * across[1] - now[1] * across[0];
            sine = 0.0;
        }
        unit(axis);
        // Rodrigues: v' = v cos + (axis x v) sin + axis (axis . v)(1 - cos).
        auto turn = [&](double v[3]) {
            const double cross[3] = {axis[1] * v[2] - axis[2] * v[1], axis[2] * v[0] - axis[0] * v[2], axis[0] * v[1] - axis[1] * v[0]};
            const double dot = axis[0] * v[0] + axis[1] * v[1] + axis[2] * v[2];
            for (int k = 0; k < 3; ++k) v[k] = v[k] * cosine + cross[k] * sine + axis[k] * dot * (1.0 - cosine);
        };
        turn(turned.u);
        turn(turned.v);
        turn(turned.w);
    }
    world = turned;
    return true;
}

double dimmer_curve(DimmerCurve curve, double level) {
    const double x = level < 0.0 ? 0.0 : (level > 1.0 ? 1.0 : level);
    switch (curve) {
        case DimmerCurve::Square: return x * x;
        case DimmerCurve::InverseSquare: return std::sqrt(x);
        case DimmerCurve::SCurve: return x * x * (3.0 - 2.0 * x);
        case DimmerCurve::Linear: break;
    }
    return x;
}

const char* dimmer_curve_name(DimmerCurve curve) {
    switch (curve) {
        case DimmerCurve::Square: return "square";
        case DimmerCurve::InverseSquare: return "inverse-square";
        case DimmerCurve::SCurve: return "s-curve";
        case DimmerCurve::Linear: break;
    }
    return "linear";
}

bool parse_dimmer_curve(std::string_view name, DimmerCurve& out) {
    for (DimmerCurve curve : {DimmerCurve::Linear, DimmerCurve::Square, DimmerCurve::InverseSquare, DimmerCurve::SCurve}) {
        if (name == dimmer_curve_name(curve)) {
            out = curve;
            return true;
        }
    }
    return false;
}

void FixtureModel::resolve(const std::vector<ChannelValue>& values, FixtureState& out, DimmerCurve curve) const {
    out.axes.resize(axes_.size());
    out.beams.resize(beams_.size());
    if (!evaluator_.valid() || values.size() != evaluator_.channels().size()) return;
    const DmxMode& mode = evaluator_.mode();

    auto function_of = [&](int channel_instance, const ChannelValue& value) -> const ChannelFunction& {
        const DmxChannel& channel = mode.channels[static_cast<size_t>(evaluator_.channels()[static_cast<size_t>(channel_instance)].channel)];
        return channel.logical_channels[static_cast<size_t>(value.logical_channel)].functions[static_cast<size_t>(value.function)];
    };

    for (size_t a = 0; a < axes_.size(); ++a) {
        const AxisBinding& binding = axes_[a];
        AxisState& axis = out.axes[a];
        axis.geometry = binding.geometry;
        axis.motion = binding.motion;
        axis.axis = binding.axis;
        const ChannelValue& value = values[static_cast<size_t>(binding.channel_instance)];
        const FunctionInfo* info = active(binding.channel_instance, value);
        Motion motion = Motion::Pan;
        char about = 'Z';
        if (!info || !moves(*info, motion, about) || motion != binding.motion || about != binding.axis) continue;
        const ChannelFunction& function = function_of(binding.channel_instance, value);
        axis.target = value.physical;
        axis.range = std::fabs(function.physical_to - function.physical_from);
        axis.center = 0.5 * (function.physical_from + function.physical_to);
        axis.real_fade = function.real_fade;
        axis.real_acceleration = function.real_acceleration;
    }

    // The fog or haze machine, wherever in the fixture its channel sits.
    double amount = -1.0, gate = 1.0;    // -1: no channel sets an amount, only switches
    for (size_t c = 0; c < values.size(); ++c) {
        if (channels_[static_cast<size_t>(evaluator_.channels()[c].channel)].is_virtual) continue;
        const ChannelValue& value = values[c];
        const FunctionInfo* info = active(static_cast<int>(c), value);
        if (!info || info->kind != Kind::Fog) continue;
        const ChannelFunction& function = function_of(static_cast<int>(c), value);
        const double low = function.physical_from, high = function.physical_to;
        const double share = high > low ? clamp01((value.physical - low) / (high - low)) : clamp01(value.physical);
        if (info->on_off) {
            gate *= clamp01(value.physical);
        } else {
            amount = std::max(amount, share);
        }
    }
    out.fog = (amount < 0.0 ? 1.0 : amount) * gate;
    if (!has_fog_) out.fog = 0.0;

    for (size_t b = 0; b < beams_.size(); ++b) {
        const BeamBinding& binding = beams_[b];
        BeamState& beam = out.beams[b];
        beam.geometry = binding.geometry;
        beam.beam_angle = binding.properties->beam_angle;
        beam.field_angle = binding.properties->field_angle;
        beam.strobe_hz = 0.0;
        beam.strobe_shape = StrobeShape::Square;
        beam.wheel = nullptr;
        beam.wheel_degrees = 0.0;
        beam.wheel_degrees_per_second = 0.0;
        for (GateLayer& layer : beam.layers) layer = GateLayer();
        for (PrismState& prism : beam.prisms) prism = PrismState();
        beam.iris = 1.0;
        beam.iris_hz = 0.0;
        beam.iris_shape = StrobeShape::Square;
        beam.frost = 0.0;
        beam.defocus = 0.0;
        for (BladeState& blade : beam.blades) blade = BladeState();
        beam.shaper_degrees = 0.0;

        double level = 1.0;
        uint64_t seen[4] = {0, 0, 0, 0};
        bool has_additive = false;
        ColorRGB mix{0.0, 0.0, 0.0};     // what the additive emitters add up to now
        ColorRGB full{0.0, 0.0, 0.0};    // what they add up to with every one at full
        bool has_rgb = false;
        double rgb[3] = {0.0, 0.0, 0.0};
        ColorRGB filter;                 // everything subtractive, multiplied together

        for (int channel_instance : binding.channel_instances) {
            const ChannelValue& value = values[static_cast<size_t>(channel_instance)];
            const FunctionInfo* info = active(channel_instance, value);
            if (!info || info->kind == Kind::None || info->kind == Kind::Pan || info->kind == Kind::Tilt || info->kind == Kind::Slide ||
                info->kind == Kind::Turn) {
                continue;
            }
            const ChannelInfo& channel = channels_[static_cast<size_t>(evaluator_.channels()[static_cast<size_t>(channel_instance)].channel)];
            const ChannelFunction& function = function_of(channel_instance, value);

            // Nearest wins: skip a control that a geometry closer to the beam already supplied.
            if (info->slot >= 0) {
                const int place = std::min(info->slot, 255);
                const uint64_t bit = uint64_t(1) << (place & 63);
                if (seen[place >> 6] & bit) continue;
                // A virtual zoom still gives the manual setting; virtual colour and strobe do nothing.
                if (channel.is_virtual && info->kind != Kind::Zoom) continue;
                seen[place >> 6] |= bit;
            } else if (channel.is_virtual) {
                continue;
            }

            const double low = std::min(function.physical_from, function.physical_to);
            const double high = std::max(function.physical_from, function.physical_to);
            const double fraction = high > low ? clamp01((value.physical - low) / (high - low)) : clamp01(value.physical);

            // The slot of the function's wheel that the active channel set names, or 0.
            const auto chosen_slot = [&]() {
                if (!info->wheel || value.channel_set < 0) return 0;
                const int slot = function.channel_sets[static_cast<size_t>(value.channel_set)].wheel_slot_index;
                return (slot >= 1 && static_cast<size_t>(slot) <= info->wheel->slots.size()) ? slot : 0;
            };
            // A shake's rate in Hz. A file that leaves the range at 0 to 1 gives no real figure: 1 to 10 Hz then.
            const auto shake_rate = [&]() { return (low == 0.0 && high == 1.0) ? 1.0 + 9.0 * fraction : std::max(value.physical, 0.0); };

            switch (info->kind) {
                case Kind::Gate: {
                    GateLayer& layer = beam.layers[info->index];
                    switch (info->component) {
                        case Select:
                        case SelectSpin:
                        case SelectShake:
                        case WheelShake:
                            if (const int slot = chosen_slot()) {
                                layer.wheel = info->wheel;
                                layer.slot = slot;
                                if (info->component == SelectSpin) layer.degrees_per_second = value.physical;
                                if (info->component == SelectShake || info->component == WheelShake) layer.shake_hz = shake_rate();
                            }
                            break;
                        case WheelTurn:
                        case WheelHold:
                            if (info->wheel && !info->wheel->slots.empty()) {
                                layer.wheel = info->wheel;
                                layer.slot = 0;
                                (info->component == WheelTurn ? layer.wheel_degrees_per_second : layer.wheel_degrees) = value.physical;
                            }
                            break;
                        case Pos:
                            layer.degrees = value.physical;
                            break;
                        case PosRotate:
                            layer.degrees_per_second = value.physical;
                            break;
                        default:
                            layer.shake_hz = shake_rate();
                            break;
                    }
                    break;
                }
                case Kind::Prism: {
                    PrismState& prism = beam.prisms[info->index];
                    if (info->component == Select || info->component == SelectSpin) {
                        const int slot = chosen_slot();
                        if (!slot) break;
                        const WheelSlot& glass = info->wheel->slots[static_cast<size_t>(slot - 1)];
                        // A slot without facets is the open one if it is the first or says so; otherwise a prism the file does not describe.
                        if (glass.facets.empty() && (slot == 1 || open_by_name(glass))) break;
                        prism.slot = &glass;
                        if (info->component == SelectSpin) prism.degrees_per_second = value.physical;
                    } else if (info->component == Pos) {
                        prism.degrees = value.physical;
                    } else {
                        prism.degrees_per_second = value.physical;
                    }
                    break;
                }
                case Kind::Iris:
                    beam.iris = clamp01(value.physical);
                    break;
                case Kind::IrisPulse:
                    beam.iris = info->reference;
                    beam.iris_hz = std::max(value.physical, 0.0);
                    beam.iris_shape = info->shape;
                    break;
                case Kind::Frost:
                    beam.frost = std::max(beam.frost, clamp01(high > 1.0 ? value.physical / high : value.physical));
                    break;
                case Kind::Focus: {
                    const double reach = std::max(info->reference - low, high - info->reference);
                    beam.defocus = reach > 0.0 ? clamp01(std::fabs(value.physical - info->reference) / reach) : 0.0;
                    break;
                }
                case Kind::Blade: {
                    BladeState& blade = beam.blades[info->index];
                    blade.present = true;
                    if (info->component == 0) {
                        blade.a = value.physical;
                    } else if (info->component == 1) {
                        blade.b = value.physical;
                        blade.two_ends = true;
                    } else {
                        blade.degrees = value.physical;
                    }
                    break;
                }
                case Kind::ShaperRot:
                    beam.shaper_degrees = value.physical;
                    break;
                case Kind::Dimmer:
                    level *= dimmer_curve(curve, fraction);
                    break;
                case Kind::Shutter: {
                    // Open is 1 and closed 0. Files do not all keep to that: Clay Paky's Sharpy has "Closed" at
                    // 1 to 12 and "Open" as a set from 0 to 1 across four values. Where the set or the function is
                    // called open or closed, that is what it is; otherwise its value, if it runs within 0 to 1.
                    int said = value.channel_set >= 0 ? open_or_closed(function.channel_sets[static_cast<size_t>(value.channel_set)].name) : 0;
                    if (said == 0) said = open_or_closed(function.name);
                    if (said != 0) {
                        level *= said > 0 ? 1.0 : 0.0;
                    } else if (std::max(std::fabs(function.physical_from), std::fabs(function.physical_to)) <= 1.0) {
                        level *= clamp01(value.physical);
                    }
                    break;
                }
                case Kind::Strobe:
                    // Hz. A file that leaves the range at 0 to 1 gives no real figure (the community RUSH PAR 2's
                    // file): up to 20 flashes a second then, slow to fast, as most fixtures' strobes run; at 0 none,
                    // as a separate strobe-effect channel at rest (GLP's X4 Bar) has it.
                    beam.strobe_hz = std::max(high <= 1.0 ? (fraction > 0.0 ? 1.0 + 19.0 * fraction : 0.0) : value.physical, 0.0);
                    beam.strobe_shape = info->shape;
                    break;
                case Kind::Zoom:
                    // Degrees, as GDTF has a zoom. A file whose zoom runs no further than 0 to 1 gives no angle
                    // (the RUSH PAR 2 RGBW Zoom on GDTF Share, whose beam would otherwise be half a degree wide):
                    // the beam keeps the angles the file gives it.
                    if (value.physical > 0.0 && std::max(std::fabs(function.physical_from), std::fabs(function.physical_to)) > 1.0) {
                        const double ratio = binding.properties->beam_angle > 0.0
                                                 ? binding.properties->field_angle / binding.properties->beam_angle
                                                 : 1.0;
                        beam.beam_angle = value.physical;
                        beam.field_angle = value.physical * ratio;
                    }
                    break;
                case Kind::Additive:
                    has_additive = true;
                    mix.r += fraction * info->color.r;
                    mix.g += fraction * info->color.g;
                    mix.b += fraction * info->color.b;
                    full.r += info->color.r;
                    full.g += info->color.g;
                    full.b += info->color.b;
                    break;
                case Kind::DirectRgb:
                    has_rgb = true;
                    rgb[info->component] = fraction;
                    break;
                case Kind::Subtractive:
                    // Part way in, the beam is part filtered and part not.
                    filter.r *= 1.0 - fraction * (1.0 - info->color.r);
                    filter.g *= 1.0 - fraction * (1.0 - info->color.g);
                    filter.b *= 1.0 - fraction * (1.0 - info->color.b);
                    break;
                case Kind::WheelSpin:
                    if (info->wheel && !info->wheel->slots.empty()) {
                        beam.wheel = info->wheel;
                        beam.wheel_degrees_per_second = value.physical;
                    }
                    break;
                case Kind::WheelIndex:
                    if (info->wheel && !info->wheel->slots.empty()) {
                        beam.wheel = info->wheel;
                        beam.wheel_degrees = value.physical;
                    }
                    break;
                case Kind::Wheel: {
                    if (!info->wheel || value.channel_set < 0) break;
                    const ChannelSet& set = function.channel_sets[static_cast<size_t>(value.channel_set)];
                    const int slot = set.wheel_slot_index;
                    if (slot < 1 || static_cast<size_t>(slot) > info->wheel->slots.size()) break;
                    // A set that runs from a slot's middle toward its neighbour (one end at 0, the other
                    // within half a slot, as Clay Paky's files have "White->White+Red") holds the wheel
                    // that far round: between two colours, as a spinning wheel is drawn.
                    const double from = set.physical_from.value_or(0.0), to = set.physical_to.value_or(0.0);
                    const bool offset = (set.physical_from || set.physical_to) && (from == 0.0 || to == 0.0) && std::fabs(from) <= 0.5 && std::fabs(to) <= 0.5;
                    if (offset && std::fabs(value.physical) > 1e-3) {
                        beam.wheel = info->wheel;
                        beam.wheel_degrees = (static_cast<double>(slot - 1) + value.physical) / static_cast<double>(info->wheel->slots.size()) * 360.0;
                        break;
                    }
                    const ColorRGB glass = to_linear_srgb(info->wheel->slots[static_cast<size_t>(slot - 1)].color);
                    filter.r *= clamp01(glass.r);
                    filter.g *= clamp01(glass.g);
                    filter.b *= clamp01(glass.b);
                    break;
                }
                case Kind::Temperature: {
                    // In kelvin, the tint relative to the start of the range. A file that gives a CTO or CTB as an
                    // amount from 0 to 1 instead (GLP's X4 Bar 20) is read as going from daylight to tungsten (CTO)
                    // or the other way (CTB), 6500 K and 3200 K.
                    double kelvin = value.physical, from = function.physical_from;
                    if (std::max(std::fabs(function.physical_from), std::fabs(function.physical_to)) <= 1.0 && function.attribute != "CTC") {
                        const double share = clamp01(value.physical);
                        const bool warmer = function.attribute == "CTO";
                        from = warmer ? 6500.0 : 3200.0;
                        kelvin = warmer ? 6500.0 + (3200.0 - 6500.0) * share : 3200.0 + (6500.0 - 3200.0) * share;
                    }
                    if (kelvin < 1000.0 || from < 1000.0) break;
                    const ColorRGB now = color_temperature_rgb(kelvin);
                    const ColorRGB start = color_temperature_rgb(from);
                    ColorRGB tint{now.r / std::max(start.r, 1e-6), now.g / std::max(start.g, 1e-6), now.b / std::max(start.b, 1e-6)};
                    normalise(tint);
                    filter.r *= tint.r;
                    filter.g *= tint.g;
                    filter.b *= tint.b;
                    break;
                }
                default:
                    break;
            }
        }

        ColorRGB color;
        if (has_additive) {
            const double scale = largest(full);
            color = scale > 0.0 ? ColorRGB{mix.r / scale, mix.g / scale, mix.b / scale} : ColorRGB{0.0, 0.0, 0.0};
        } else if (has_rgb) {
            color = ColorRGB{rgb[0], rgb[1], rgb[2]};
        }
        // A filament's light is warm, and warmer the lower it burns. Light grows about as the voltage
        // to the power 3.4 and colour temperature as its 0.42nd power, so a lamp burning at a fraction
        // f of its light is at its rated temperature times f^(0.42/3.4): a 3200 K lamp at a fifth
        // glows near 2600 K, at a twentieth near 2200 K. Only for lamps the file says are filaments,
        // and not where emitters make the colour.
        const LampType lamp = binding.properties->lamp_type;
        if ((lamp == LampType::Tungsten || lamp == LampType::Halogen) && !has_additive && !has_rgb && level > 0.0) {
            const double rated = binding.properties->color_temperature >= 1000.0 ? binding.properties->color_temperature : 3200.0;
            const ColorRGB warm = color_temperature_rgb(std::max(rated * std::pow(clamp01(level), 0.42 / 3.4), 1200.0));
            color.r *= warm.r;
            color.g *= warm.g;
            color.b *= warm.b;
        }
        color.r *= filter.r;
        color.g *= filter.g;
        color.b *= filter.b;
        // Split what is left into a hue and how much light gets through.
        level *= normalise(color);
        if (level <= 0.0) color = ColorRGB();
        beam.color = color;
        beam.intensity = clamp01(level);
    }
}

void MotorAxis::configure(double range, double real_fade, double real_acceleration) {
    max_speed_ = 0.0;
    acceleration_ = 0.0;
    if (range <= 0.0 || real_fade <= 0.0) return;
    const double ramp = std::max(real_acceleration, 0.0);
    if (real_fade > 2.0 * ramp) {
        // Ramp up, cruise, ramp down: the whole move takes range / speed + ramp.
        max_speed_ = range / (real_fade - ramp);
        acceleration_ = ramp > 0.0 ? max_speed_ / ramp : 0.0;
    } else {
        // Too short to reach a cruise: accelerate for half the time, brake for the other half.
        max_speed_ = 2.0 * range / real_fade;
        acceleration_ = 4.0 * range / (real_fade * real_fade);
    }
}

void MotorAxis::reset(double position) {
    position_ = position;
    velocity_ = 0.0;
}

double MotorAxis::step(double target, double dt) {
    if (max_speed_ <= 0.0 || dt <= 0.0) {
        if (max_speed_ <= 0.0) {
            position_ = target;
            velocity_ = 0.0;
        }
        return position_;
    }
    const double error = target - position_;
    const double distance = std::fabs(error);
    const double direction = error < 0.0 ? -1.0 : 1.0;

    if (acceleration_ <= 0.0) {
        velocity_ = direction * max_speed_;
    } else {
        // The fastest speed from which the axis can still stop on the target, braking in steps
        // of this size. The continuous formula sqrt(2ad) would arrive with speed left over.
        const double half_step = 0.5 * acceleration_ * dt;
        const double stopping = std::sqrt(half_step * half_step + 2.0 * acceleration_ * distance) - half_step;
        const double wanted = direction * std::min(max_speed_, stopping);
        const double change = wanted - velocity_;
        const double limit = acceleration_ * dt;
        velocity_ += change < -limit ? -limit : (change > limit ? limit : change);
    }

    const double travel = velocity_ * dt;
    if (travel * direction >= distance) {
        position_ = target;
        velocity_ = 0.0;
    } else {
        position_ += travel;
    }
    return position_;
}

}  // namespace viz

namespace viz {

double strobe_level(StrobeShape shape, double phase, double chance) {
    phase = phase - std::floor(phase);
    const bool random = shape == StrobeShape::Random || shape == StrobeShape::RandomPulse || shape == StrobeShape::RandomRampDown ||
                        shape == StrobeShape::RandomRampUp;
    if (random && chance >= 0.5) return 0.0;    // this cycle stays dark
    switch (shape) {
        case StrobeShape::Square:
        case StrobeShape::Random:
            return phase < 0.5 ? 1.0 : 0.0;
        case StrobeShape::Pulse:
        case StrobeShape::RandomPulse:
            return phase < 0.2 ? 1.0 : 0.0;
        case StrobeShape::RampDown:
        case StrobeShape::RandomRampDown:
            return 1.0 - phase;    // snaps open, fades shut
        case StrobeShape::RampUp:
        case StrobeShape::RandomRampUp:
            return phase;          // fades up, snaps shut
    }
    return 1.0;
}

ColorRGB wheel_color_at(const Wheel& wheel, double turns) {
    const size_t count = wheel.slots.size();
    if (count == 0 || !std::isfinite(turns)) return ColorRGB{};
    auto glass = [&wheel](size_t slot) {
        const ColorRGB color = to_linear_srgb(wheel.slots[slot].color);
        return ColorRGB{clamp01(color.r), clamp01(color.g), clamp01(color.b)};
    };
    const double place = (turns - std::floor(turns)) * static_cast<double>(count);
    const size_t slot = std::min(static_cast<size_t>(place), count - 1);
    const double within = place - static_cast<double>(slot);
    const ColorRGB here = glass(slot);
    if (within <= 0.9) return here;
    const ColorRGB next = glass((slot + 1) % count);
    const double share = (within - 0.9) / 0.1;    // 0 to 1 across the last tenth
    return ColorRGB{here.r + (next.r - here.r) * share, here.g + (next.g - here.g) * share, here.b + (next.b - here.b) * share};
}

bool BeamState::gate_open() const {
    for (const GateLayer& layer : layers) {
        if (!layer.wheel) continue;
        if (layer.slot == 0) return false;    // a turning wheel: something is passing through
        if (static_cast<size_t>(layer.slot) <= layer.wheel->slots.size() && !layer.wheel->slots[static_cast<size_t>(layer.slot - 1)].media_file.empty()) {
            return false;
        }
    }
    for (const PrismState& prism : prisms) {
        if (prism.slot) return false;
    }
    if (iris < 0.999 || iris_hz > 0.0 || frost > 0.001 || defocus > 0.001) return false;
    for (const BladeState& blade : blades) {
        if (blade.present && (blade.a > 0.0 || (blade.two_ends && blade.b > 0.0))) return false;
    }
    return true;
}

WheelPlace wheel_place_at(const Wheel& wheel, double turns) {
    WheelPlace place;
    const size_t count = wheel.slots.size();
    if (count == 0 || !std::isfinite(turns)) return place;
    const double along = (turns - std::floor(turns)) * static_cast<double>(count);
    const double nearest = std::floor(along + 0.5);
    place.slot = static_cast<int>(static_cast<size_t>(nearest) % count) + 1;
    place.offset = along - nearest;
    return place;
}

std::vector<PrismImage> prism_images(const WheelSlot& slot) {
    std::vector<PrismImage> images;
    for (const PrismFacet& facet : slot.facets) {
        const auto& m = facet.rotation;
        PrismImage image;
        const bool in_row = m[2][0] != 0.0 || m[2][1] != 0.0;
        image.x = in_row ? m[2][0] : m[0][2];
        image.y = in_row ? m[2][1] : m[1][2];
        image.scale_x = std::hypot(m[0][0], m[0][1]);
        image.scale_y = std::hypot(m[1][0], m[1][1]);
        if (!std::isfinite(image.x) || !std::isfinite(image.y) || !std::isfinite(image.scale_x) || !std::isfinite(image.scale_y)) continue;
        image.scale_x = image.scale_x > 1e-3 ? std::min(image.scale_x, 20.0) : 1.0;
        image.scale_y = image.scale_y > 1e-3 ? std::min(image.scale_y, 20.0) : 1.0;
        ColorRGB tint = to_linear_srgb(facet.color);
        if (normalise(tint) > 0.0) image.color = tint;
        images.push_back(image);
    }
    if (images.empty()) {
        for (int k = 0; k < 3; ++k) {
            const double angle = (90.0 + 120.0 * k) * 3.14159265358979323846 / 180.0;
            PrismImage image;
            image.x = 1.1 * std::cos(angle);
            image.y = 1.1 * std::sin(angle);
            images.push_back(image);
        }
    }
    return images;
}

BladeEdge blade_edge(int blade, const BladeState& state, double shaper_degrees) {
    static const double outward[4][2] = {{0.0, 1.0}, {1.0, 0.0}, {0.0, -1.0}, {-1.0, 0.0}};
    const double ox = outward[blade & 3][0];
    const double oy = outward[blade & 3][1];
    const double lx = oy;    // along the blade's edge
    const double ly = -ox;
    double nx = ox;
    double ny = oy;
    double distance = 2.0;    // well clear of the beam
    const double a = state.a;
    const double b = state.two_ends ? state.b : state.a;
    if (state.present && (a > 0.0 || b > 0.0)) {
        if (state.two_ends) {
            // The edge runs from one end of the blade, a in, to the other, b in.
            const double height_a = 1.0 - 2.0 * a;
            const double delta = (1.0 - 2.0 * b) - height_a;
            nx = 2.0 * ox - delta * lx;
            ny = 2.0 * oy - delta * ly;
            const double length = std::hypot(nx, ny);
            nx /= length;
            ny /= length;
            distance = nx * (-lx + height_a * ox) + ny * (-ly + height_a * oy);
        } else {
            // The edge passes a in on the blade's own line, turned about that point.
            const double turn = state.degrees * 3.14159265358979323846 / 180.0;
            nx = ox * std::cos(turn) - oy * std::sin(turn);
            ny = ox * std::sin(turn) + oy * std::cos(turn);
            distance = (1.0 - 2.0 * a) * std::cos(turn);
        }
    }
    const double all = shaper_degrees * 3.14159265358979323846 / 180.0;
    BladeEdge edge;
    edge.nx = nx * std::cos(all) - ny * std::sin(all);
    edge.ny = nx * std::sin(all) + ny * std::cos(all);
    edge.distance = distance;
    return edge;
}

}  // namespace viz
