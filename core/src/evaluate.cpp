#include "vizcore/evaluate.h"

#include <algorithm>
#include <utility>

namespace viz {

namespace {

struct TreeWalk {
    const FixtureType& fixture;
    Diagnostics& diagnostics;
    std::vector<GeometryInstance> instances;
    std::vector<const Geometry*> references;
};

void walk_geometry(const Geometry& geometry, int parent, const Matrix4* placement, TreeWalk& walk) {
    if (geometry.type == GeometryType::GeometryReference) {
        const Geometry* target = walk.fixture.find_top_level_geometry(geometry.reference);
        if (!target) {
            walk.diagnostics.warn("GeometryReference '" + geometry.name + "'",
                                  "referenced geometry '" + geometry.reference + "' is not a top-level geometry");
            return;
        }
        if (walk.references.size() >= 8) {
            walk.diagnostics.warn("GeometryReference '" + geometry.name + "'",
                                  "references are nested more than 8 deep (a reference loop?); not followed");
            return;
        }
        walk.references.push_back(&geometry);
        walk_geometry(*target, parent, &geometry.position, walk);
        walk.references.pop_back();
        return;
    }

    GeometryInstance instance;
    instance.geometry = &geometry;
    instance.parent = parent;
    instance.references = walk.references;
    // A referenced geometry sits where its reference says, and takes the reference's name.
    instance.local = placement ? multiply(*placement, geometry.position) : geometry.position;
    instance.name = geometry.name;
    if (!walk.references.empty()) {
        const std::string& reference_name = walk.references.back()->name;
        instance.name = placement ? reference_name : reference_name + "." + geometry.name;
    }
    const int index = static_cast<int>(walk.instances.size());
    walk.instances.push_back(std::move(instance));
    for (const Geometry& child : geometry.children) walk_geometry(child, index, nullptr, walk);
}

std::string context_key(const std::vector<const Geometry*>& references) {
    std::string key;
    for (const Geometry* reference : references) key += reference->name + "/";
    return key;
}

}  // namespace

Matrix4 multiply(const Matrix4& a, const Matrix4& b) {
    Matrix4 result;
    for (int row = 0; row < 4; ++row) {
        for (int column = 0; column < 4; ++column) {
            double sum = 0.0;
            for (int k = 0; k < 4; ++k) sum += a.m[row][k] * b.m[k][column];
            result.m[row][column] = sum;
        }
    }
    return result;
}

std::vector<GeometryInstance> expand_geometry(const FixtureType& fixture, std::string_view root_geometry,
                                              Diagnostics& diagnostics) {
    TreeWalk walk{fixture, diagnostics, {}, {}};
    if (const Geometry* root = fixture.find_top_level_geometry(root_geometry)) {
        walk_geometry(*root, -1, nullptr, walk);
    }
    return std::move(walk.instances);
}

ModeEvaluator::ModeEvaluator(const FixtureType& fixture, size_t mode_index) {
    if (mode_index >= fixture.modes.size()) {
        diagnostics_.error("ModeEvaluator", "fixture has no DMX mode with index " + std::to_string(mode_index));
        return;
    }
    mode_ = &fixture.modes[mode_index];
    const std::string where = "DMXMode '" + mode_->name + "'";

    geometry_ = expand_geometry(fixture, mode_->geometry, diagnostics_);
    if (geometry_.empty()) {
        diagnostics_.warn(where, "mode geometry '" + mode_->geometry + "' not found; channels are taken as written");
    }

    std::vector<std::string> contexts;    // parallel to instances_
    for (size_t c = 0; c < mode_->channels.size(); ++c) {
        const DmxChannel& channel = mode_->channels[c];
        bool found = false;
        for (size_t g = 0; g < geometry_.size(); ++g) {
            const GeometryInstance& occurrence = geometry_[g];
            if (occurrence.geometry->name != channel.geometry) continue;
            found = true;
            ChannelInstance instance;
            instance.channel = static_cast<int>(c);
            instance.geometry = occurrence.name;
            instance.geometry_index = static_cast<int>(g);
            instance.dmx_break = channel.dmx_break;
            int shift = 0;
            for (const Geometry* reference : occurrence.references) {
                if (channel.break_overwrite) {
                    // The reference supplies both the break and the offset, in its last Break child.
                    if (!reference->breaks.empty()) {
                        instance.dmx_break = reference->breaks.back().dmx_break;
                        shift += reference->breaks.back().dmx_offset - 1;
                    }
                } else {
                    // The first Break child for the channel's break. A second one for the same
                    // break is the Overwrite entry above, not a further shift: Robe's Spiider
                    // gives each pixel two, spaced for its RGBW modes and for its RGB modes.
                    for (const GeometryBreak& entry : reference->breaks) {
                        if (entry.dmx_break != channel.dmx_break) continue;
                        shift += entry.dmx_offset - 1;
                        break;
                    }
                }
            }
            if (channel.break_overwrite && occurrence.references.empty()) {
                diagnostics_.warn(where + " / DMXChannel '" + channel.name + "'",
                                  "DMXBreak is Overwrite but the geometry is not reached through a GeometryReference; break 1 used");
            }
            for (int offset : channel.offsets) instance.offsets.push_back(offset + shift);
            instances_.push_back(std::move(instance));
            contexts.push_back(context_key(occurrence.references));
        }
        if (!found) {
            if (!geometry_.empty()) {
                diagnostics_.warn(where + " / DMXChannel '" + channel.name + "'",
                                  "geometry '" + channel.geometry + "' is not part of the mode's geometry tree");
            }
            ChannelInstance instance;
            instance.channel = static_cast<int>(c);
            instance.geometry = channel.geometry;
            instance.dmx_break = channel.dmx_break;
            instance.offsets = channel.offsets;
            instances_.push_back(std::move(instance));
            contexts.push_back(std::string());
        }
    }

    // Two channels on one slot means the file, or the reading of its breaks, is wrong.
    for (size_t a = 0; a < instances_.size(); ++a) {
        for (size_t b = a + 1; b < instances_.size(); ++b) {
            if (instances_[a].dmx_break != instances_[b].dmx_break) continue;
            for (int offset : instances_[a].offsets) {
                if (std::find(instances_[b].offsets.begin(), instances_[b].offsets.end(), offset) != instances_[b].offsets.end()) {
                    diagnostics_.warn(where, "break " + std::to_string(instances_[a].dmx_break) + " offset " +
                                                 std::to_string(offset) + " is used by both '" +
                                                 mode_->channels[static_cast<size_t>(instances_[a].channel)].name + "' on '" +
                                                 instances_[a].geometry + "' and '" +
                                                 mode_->channels[static_cast<size_t>(instances_[b].channel)].name + "' on '" +
                                                 instances_[b].geometry + "'");
                    break;
                }
            }
        }
    }

    // A function's master is the master channel's instance in the same reference context if
    // there is one (per-cell masters), otherwise the fixture-wide instance.
    master_instance_.resize(instances_.size());
    for (size_t i = 0; i < instances_.size(); ++i) {
        const DmxChannel& channel = mode_->channels[static_cast<size_t>(instances_[i].channel)];
        for (const LogicalChannel& logical : channel.logical_channels) {
            for (const ChannelFunction& function : logical.functions) {
                int master = -1;
                if (function.mode_master_channel >= 0) {
                    for (size_t m = 0; m < instances_.size(); ++m) {
                        if (instances_[m].channel != function.mode_master_channel) continue;
                        if (master < 0) master = static_cast<int>(m);
                        if (contexts[m] == contexts[i]) {
                            master = static_cast<int>(m);
                            break;
                        }
                    }
                }
                master_instance_[i].push_back(master);
            }
        }
    }
}

std::vector<int> ModeEvaluator::breaks() const {
    std::vector<int> result;
    for (const ChannelInstance& instance : instances_) {
        if (instance.offsets.empty()) continue;
        if (std::find(result.begin(), result.end(), instance.dmx_break) == result.end()) result.push_back(instance.dmx_break);
    }
    std::sort(result.begin(), result.end());
    return result;
}

size_t ModeEvaluator::footprint(int dmx_break) const {
    int highest = 0;
    for (const ChannelInstance& instance : instances_) {
        if (instance.dmx_break != dmx_break) continue;
        for (int offset : instance.offsets) highest = std::max(highest, offset);
    }
    return static_cast<size_t>(highest);
}

std::vector<uint8_t> ModeEvaluator::default_frame(int dmx_break) const {
    std::vector<uint8_t> frame(footprint(dmx_break), 0);
    if (!mode_) return frame;
    for (const ChannelInstance& instance : instances_) {
        if (instance.dmx_break != dmx_break) continue;
        uint32_t value = mode_->channels[static_cast<size_t>(instance.channel)].default_value();
        for (size_t i = instance.offsets.size(); i-- > 0;) {
            frame[static_cast<size_t>(instance.offsets[i] - 1)] = static_cast<uint8_t>(value & 0xFFu);
            value >>= 8;
        }
    }
    return frame;
}

int ModeEvaluator::find(std::string_view channel_name, std::string_view geometry) const {
    if (!mode_) return -1;
    for (size_t i = 0; i < instances_.size(); ++i) {
        if (mode_->channels[static_cast<size_t>(instances_[i].channel)].name != channel_name) continue;
        if (geometry.empty() || instances_[i].geometry == geometry) return static_cast<int>(i);
    }
    return -1;
}

void ModeEvaluator::evaluate(const uint8_t* dmx, size_t size, std::vector<ChannelValue>& out) const {
    std::vector<DmxBreakData> breaks(1);
    breaks[0].dmx_break = 1;
    breaks[0].data = dmx;
    breaks[0].size = size;
    evaluate(breaks, out);
}

void ModeEvaluator::evaluate(const std::vector<DmxBreakData>& breaks, std::vector<ChannelValue>& out) const {
    out.assign(instances_.size(), ChannelValue());
    // 0 = not started, 1 = in progress, 2 = done. A master is evaluated before its dependants.
    std::vector<uint8_t> state(instances_.size(), 0);
    for (size_t i = 0; i < instances_.size(); ++i) evaluate_one(i, breaks, out, state);
}

void ModeEvaluator::evaluate_one(size_t index, const std::vector<DmxBreakData>& breaks,
                                 std::vector<ChannelValue>& out, std::vector<uint8_t>& state) const {
    if (state[index] != 0) return;
    state[index] = 1;

    const ChannelInstance& instance = instances_[index];
    const DmxChannel& channel = mode_->channels[static_cast<size_t>(instance.channel)];
    ChannelValue value;

    const DmxBreakData* source = nullptr;
    for (const DmxBreakData& candidate : breaks) {
        if (candidate.dmx_break == instance.dmx_break && candidate.data) source = &candidate;
    }
    bool covered = source && !instance.offsets.empty();
    uint32_t raw = 0;
    if (covered) {
        for (int offset : instance.offsets) {
            if (static_cast<size_t>(offset) > source->size) {
                covered = false;
                break;
            }
            raw = (raw << 8) | source->data[offset - 1];
        }
    }
    value.from_dmx = covered;
    value.dmx = covered ? raw : channel.default_value();

    size_t flat = 0;
    for (size_t l = 0; l < channel.logical_channels.size() && value.function < 0; ++l) {
        const std::vector<ChannelFunction>& functions = channel.logical_channels[l].functions;
        for (size_t f = 0; f < functions.size(); ++f, ++flat) {
            if (value.function >= 0) continue;    // keep `flat` in step with the remaining functions
            const ChannelFunction& function = functions[f];
            if (value.dmx < function.dmx_from || value.dmx > function.dmx_to) continue;
            int master = master_instance_[index][flat];
            if (master >= 0) {
                evaluate_one(static_cast<size_t>(master), breaks, out, state);
                if (state[static_cast<size_t>(master)] != 2) continue;    // dependency loop: never satisfied
                const ChannelValue& master_value = out[static_cast<size_t>(master)];
                if (master_value.dmx < function.mode_from || master_value.dmx > function.mode_to) continue;
                if (function.mode_master_function >= 0 &&
                    (master_value.logical_channel != function.mode_master_logical ||
                     master_value.function != function.mode_master_function)) {
                    continue;
                }
            }
            value.logical_channel = static_cast<int>(l);
            value.function = static_cast<int>(f);
        }
    }

    if (value.function >= 0) {
        const ChannelFunction& function =
            channel.logical_channels[static_cast<size_t>(value.logical_channel)].functions[static_cast<size_t>(value.function)];
        uint32_t from = function.dmx_from;
        uint32_t to = function.dmx_to;
        double physical_from = function.physical_from;
        double physical_to = function.physical_to;
        for (size_t s = 0; s < function.channel_sets.size(); ++s) {
            const ChannelSet& set = function.channel_sets[s];
            if (value.dmx < set.dmx_from || value.dmx > set.dmx_to) continue;
            value.channel_set = static_cast<int>(s);
            if (set.physical_from || set.physical_to) {
                from = set.dmx_from;
                to = set.dmx_to;
                physical_from = set.physical_from.value_or(function.physical_from);
                physical_to = set.physical_to.value_or(function.physical_to);
            }
            break;
        }
        double position = to > from ? static_cast<double>(value.dmx - from) / static_cast<double>(to - from) : 0.0;
        value.physical = physical_from + position * (physical_to - physical_from);
    }

    out[index] = value;
    state[index] = 2;
}

double mode_power_watts(const ModeEvaluator& mode, bool* rated) {
    double payload = 0.0;
    double sources = 0.0;
    for (const GeometryInstance& instance : mode.geometry()) {
        if (!instance.geometry) continue;
        if (instance.geometry->type == GeometryType::WiringObject) payload = std::max(payload, instance.geometry->electrical_payload);
        if (instance.geometry->beam) sources += std::max(0.0, instance.geometry->beam->power_consumption);
    }
    if (rated) *rated = payload > 0.0;
    return payload > 0.0 ? payload : sources;
}

}  // namespace viz
