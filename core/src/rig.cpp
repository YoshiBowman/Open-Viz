#include "vizcore/rig.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <map>

#include <pugixml.hpp>

#include "text.h"
#include "vizcore/evaluate.h"
#include "vizcore/files.h"

namespace viz {

namespace {

// A type's name as it is compared: without its ".gdtf", capitals aside.
std::string type_key(std::string_view spec) {
    std::string key(text::trim(spec));
    if (key.size() > 5) {
        std::string ending = key.substr(key.size() - 5);
        for (char& ch : ending) ch = static_cast<char>(ch >= 'A' && ch <= 'Z' ? ch - 'A' + 'a' : ch);
        if (ending == ".gdtf") key.resize(key.size() - 5);
    }
    for (char& ch : key) ch = static_cast<char>(ch >= 'A' && ch <= 'Z' ? ch - 'A' + 'a' : ch);
    return key;
}

std::string xml_escaped(std::string_view value) {
    std::string out;
    for (char ch : value) {
        switch (ch) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            default: out += ch;
        }
    }
    return out;
}

// The words of a mode's name, lower case: "97Ch Mode" -> 97, ch, mode. Letters and figures part company.
std::vector<std::string> words_of(std::string_view name) {
    std::vector<std::string> words;
    std::string word;
    int kind = 0;    // 1 letters, 2 figures
    auto flush = [&]() {
        if (!word.empty()) words.push_back(word);
        word.clear();
    };
    for (char ch : name) {
        const bool letter = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z');
        const bool figure = ch >= '0' && ch <= '9';
        const int now = letter ? 1 : figure ? 2 : 0;
        if (now != kind) flush();
        kind = now;
        if (now) word += static_cast<char>(ch >= 'A' && ch <= 'Z' ? ch - 'A' + 'a' : ch);
    }
    flush();
    return words;
}

// The slots a mode takes on each break it has.
std::map<int, size_t> lengths_of(const FixtureType& type, size_t mode_index) {
    std::map<int, size_t> lengths;
    const ModeEvaluator evaluator(type, mode_index);
    for (int dmx_break : evaluator.breaks()) lengths[dmx_break] = evaluator.footprint(dmx_break);
    return lengths;
}

}  // namespace

bool same_type(std::string_view a, std::string_view b) {
    const std::string key = type_key(a);
    return !key.empty() && key == type_key(b);
}

const std::string* TypeSwap::mode_for(std::string_view mode) const {
    for (const auto& pair : modes) {
        if (pair.first == mode) return &pair.second;
    }
    return nullptr;
}

const std::string* TypeSwap::mode_back(std::string_view mode) const {
    for (const auto& pair : modes) {
        if (pair.second == mode) return &pair.first;
    }
    return nullptr;
}

const TypeSwap* TypeSwaps::find(std::string_view gdtf_spec) const {
    const std::string key = type_key(gdtf_spec);
    if (key.empty()) return nullptr;
    for (const TypeSwap& swap : swaps) {
        if (type_key(swap.type) == key) return &swap;
    }
    return nullptr;
}

const TypeSwap* TypeSwaps::stand_in(std::string_view gdtf_spec) const {
    const TypeSwap* found = find(gdtf_spec);
    return found && found->stands_in() ? found : nullptr;
}

void TypeSwaps::set_stand_in(std::string_view gdtf_spec, std::string file, std::vector<std::pair<std::string, std::string>> modes) {
    const std::string key = type_key(gdtf_spec);
    for (TypeSwap& have : swaps) {
        if (type_key(have.type) == key) {
            have.file = std::move(file);
            have.modes = std::move(modes);
            return;
        }
    }
    TypeSwap swap;
    swap.type = std::string(gdtf_spec);
    swap.file = std::move(file);
    swap.modes = std::move(modes);
    swaps.push_back(std::move(swap));
}

bool TypeSwaps::clear_stand_in(std::string_view gdtf_spec) {
    const std::string key = type_key(gdtf_spec);
    for (size_t index = 0; index < swaps.size(); ++index) {
        TypeSwap& have = swaps[index];
        if (type_key(have.type) != key) continue;
        if (!have.stands_in()) return false;
        have.file.clear();
        have.modes.clear();
        if (!have.corrects()) swaps.erase(swaps.begin() + static_cast<std::ptrdiff_t>(index));
        return true;
    }
    return false;
}

void TypeSwaps::correct(std::string_view gdtf_spec, bool invert_pan, bool invert_tilt, double pan_offset, double tilt_offset) {
    const std::string key = type_key(gdtf_spec);
    if (key.empty()) return;
    // An offset is an angle: whole turns say nothing, and anything that is not a number says nothing either.
    auto angle = [](double degrees) {
        if (!(degrees > -1e9 && degrees < 1e9)) return 0.0;
        degrees = std::fmod(degrees, 360.0);
        if (degrees > 180.0) degrees -= 360.0;
        if (degrees <= -180.0) degrees += 360.0;
        return degrees == 0.0 ? 0.0 : degrees;
    };
    for (size_t index = 0; index < swaps.size(); ++index) {
        TypeSwap& have = swaps[index];
        if (type_key(have.type) != key) continue;
        have.invert_pan = invert_pan;
        have.invert_tilt = invert_tilt;
        have.pan_offset = angle(pan_offset);
        have.tilt_offset = angle(tilt_offset);
        if (!have.stands_in() && !have.corrects()) swaps.erase(swaps.begin() + static_cast<std::ptrdiff_t>(index));
        return;
    }
    TypeSwap swap;
    swap.type = std::string(gdtf_spec);
    swap.invert_pan = invert_pan;
    swap.invert_tilt = invert_tilt;
    swap.pan_offset = angle(pan_offset);
    swap.tilt_offset = angle(tilt_offset);
    if (swap.corrects()) swaps.push_back(std::move(swap));
}

void TypeSwaps::set(TypeSwap swap) {
    const std::string key = type_key(swap.type);
    for (TypeSwap& have : swaps) {
        if (type_key(have.type) == key) {
            have = std::move(swap);
            return;
        }
    }
    swaps.push_back(std::move(swap));
}

bool TypeSwaps::remove(std::string_view gdtf_spec) {
    const std::string key = type_key(gdtf_spec);
    const size_t before = swaps.size();
    swaps.erase(std::remove_if(swaps.begin(), swaps.end(), [&key](const TypeSwap& swap) { return type_key(swap.type) == key; }), swaps.end());
    return swaps.size() != before;
}

TypeSwaps parse_type_swaps(std::string_view xml) {
    TypeSwaps result;
    pugi::xml_document document;
    if (!document.load_buffer(xml.data(), xml.size())) return result;
    for (pugi::xml_node node : document.child("TypeSwaps").children("Type")) {
        TypeSwap swap;
        swap.type = std::string(text::trim(node.attribute("Spec").value()));
        swap.file = std::string(text::trim(node.attribute("File").value()));
        // A file name and nothing else: the library has no folders, and nothing outside it is to be opened.
        if (swap.type.empty() || swap.file.find('/') != std::string::npos || swap.file.find('\\') != std::string::npos ||
            swap.file == "." || swap.file == ".." || result.find(swap.type)) {
            continue;
        }
        if (swap.stands_in()) {
            for (pugi::xml_node mode : node.children("Mode")) {
                const std::string from = mode.attribute("From").value(), to = mode.attribute("To").value();
                if (!from.empty() && !to.empty() && !swap.mode_for(from)) swap.modes.emplace_back(from, to);
            }
        }
        text::parse_bool(node.attribute("PanInvert").value(), swap.invert_pan);
        text::parse_bool(node.attribute("TiltInvert").value(), swap.invert_tilt);
        double degrees = 0.0;
        if (text::parse_double(node.attribute("PanOffset").value(), degrees) && degrees > -360.0 && degrees < 360.0) swap.pan_offset = degrees;
        if (text::parse_double(node.attribute("TiltOffset").value(), degrees) && degrees > -360.0 && degrees < 360.0) swap.tilt_offset = degrees;
        if (!swap.stands_in() && !swap.corrects()) continue;
        result.swaps.push_back(std::move(swap));
    }
    return result;
}

std::string write_type_swaps(const TypeSwaps& swaps) {
    std::string out = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<TypeSwaps Version=\"1\">\n";
    auto degrees = [](double value) {
        char buffer[40];
        std::snprintf(buffer, sizeof(buffer), "%.6g", value);
        for (char* c = buffer; *c; ++c) {
            if (*c == ',') *c = '.';    // a host application may have set a comma locale
        }
        return std::string(buffer);
    };
    for (const TypeSwap& swap : swaps.swaps) {
        if (!swap.stands_in() && !swap.corrects()) continue;
        out += "  <Type Spec=\"" + xml_escaped(swap.type) + "\"";
        if (swap.stands_in()) out += " File=\"" + xml_escaped(swap.file) + "\"";
        if (swap.invert_pan) out += " PanInvert=\"true\"";
        if (swap.invert_tilt) out += " TiltInvert=\"true\"";
        if (swap.pan_offset != 0.0) out += " PanOffset=\"" + degrees(swap.pan_offset) + "\"";
        if (swap.tilt_offset != 0.0) out += " TiltOffset=\"" + degrees(swap.tilt_offset) + "\"";
        if (swap.modes.empty() || !swap.stands_in()) {
            out += "/>\n";
            continue;
        }
        out += ">\n";
        for (const auto& pair : swap.modes) out += "    <Mode From=\"" + xml_escaped(pair.first) + "\" To=\"" + xml_escaped(pair.second) + "\"/>\n";
        out += "  </Type>\n";
    }
    return out + "</TypeSwaps>\n";
}

std::string type_swaps_path(std::string_view mvr_path) {
    std::string path(mvr_path);
    const size_t dot = path.find_last_of('.');
    const size_t slash = path.find_last_of("/\\");
    if (dot != std::string::npos && (slash == std::string::npos || dot > slash)) path.resize(dot);
    return path + ".types.xml";
}

std::vector<std::pair<std::string, std::string>> match_modes(const FixtureType& original, const FixtureType& stand_in) {
    std::vector<std::map<int, size_t>> theirs;
    for (size_t index = 0; index < stand_in.modes.size(); ++index) theirs.push_back(lengths_of(stand_in, index));
    std::vector<std::pair<std::string, std::string>> pairs;
    for (size_t index = 0; index < original.modes.size(); ++index) {
        const std::map<int, size_t> wanted = lengths_of(original, index);
        const std::string& name = original.modes[index].name;
        const std::vector<std::string> words = words_of(name);
        int best = -1;
        int best_score = -1;
        for (size_t candidate = 0; candidate < stand_in.modes.size(); ++candidate) {
            if (theirs[candidate] != wanted) continue;
            const std::string& other = stand_in.modes[candidate].name;
            int score = 0;
            if (other == name) {
                score = 1000000;
            } else {
                for (const std::string& word : words_of(other)) score += std::count(words.begin(), words.end(), word) > 0 ? 1 : 0;
            }
            if (score > best_score) {
                best_score = score;
                best = static_cast<int>(candidate);
            }
        }
        if (best >= 0) pairs.emplace_back(name, stand_in.modes[static_cast<size_t>(best)].name);
    }
    return pairs;
}

namespace {

// The slots (break and first, coarse, slot) of a mode's dimmer channels, wherever in the fixture they sit.
std::vector<std::pair<int, int>> dimmer_slots(const FixtureType& type, size_t mode_index) {
    std::vector<std::pair<int, int>> slots;
    const ModeEvaluator evaluator(type, mode_index);
    const DmxMode& mode = type.modes[mode_index];
    for (const ChannelInstance& instance : evaluator.channels()) {
        if (instance.offsets.empty() || instance.channel < 0 || static_cast<size_t>(instance.channel) >= mode.channels.size()) continue;
        for (const LogicalChannel& logical : mode.channels[static_cast<size_t>(instance.channel)].logical_channels) {
            if (logical.attribute == "Dimmer") {
                slots.emplace_back(instance.dmx_break, instance.offsets.front());
                break;
            }
        }
    }
    return slots;
}

}  // namespace

int dimmers_agree(const FixtureType& original, size_t mode_index, const FixtureType& stand_in, size_t stand_in_mode) {
    if (mode_index >= original.modes.size() || stand_in_mode >= stand_in.modes.size()) return -1;
    const std::vector<std::pair<int, int>> wanted = dimmer_slots(original, mode_index);
    if (wanted.empty()) return -1;
    // One slot in common is enough: a console calls the level of every cell of a strobe's tube a
    // dimmer, where the maker's own file calls those white, and both are right.
    const std::vector<std::pair<int, int>> theirs = dimmer_slots(stand_in, stand_in_mode);
    for (const std::pair<int, int>& slot : wanted) {
        if (std::find(theirs.begin(), theirs.end(), slot) != theirs.end()) return 1;
    }
    return 0;
}

ModeSilence mode_silence(const FixtureType& type, size_t mode_index) {
    ModeSilence result;
    if (mode_index >= type.modes.size()) return result;
    for (const DmxChannel& channel : type.modes[mode_index].channels) {
        ++result.channels;
        bool says = false;
        for (const LogicalChannel& logical : channel.logical_channels) {
            if (!logical.attribute.empty() && logical.attribute != "NoFeature") says = true;
            for (const ChannelFunction& function : logical.functions) {
                if (!function.attribute.empty() && function.attribute != "NoFeature") says = true;
            }
        }
        if (!says) ++result.silent;
    }
    return result;
}

std::vector<std::string> drop_missing_stand_ins(std::vector<RigFixture>& rig, const std::string& library_dir) {
    std::map<std::string, bool> there;
    std::vector<std::string> missed;
    for (RigFixture& fixture : rig) {
        if ((fixture.stood_in_for.empty() && !fixture.retyped) || !fixture.from_library || fixture.added) continue;
        auto known = there.find(fixture.gdtf);
        if (known == there.end()) {
            const bool found = !library_dir.empty() && read_stream(library_dir + "/" + fixture.gdtf).good();
            known = there.emplace(fixture.gdtf, found).first;
            if (!found) missed.push_back(fixture.gdtf);
        }
        if (known->second) continue;
        fixture.gdtf = fixture.retyped && fixture.source ? fixture.source->gdtf_spec : fixture.stood_in_for;
        fixture.from_library = false;
        fixture.stood_in_for.clear();
        fixture.retyped = false;
        if (fixture.source) {
            fixture.file_mode = fixture.source->gdtf_mode;
            fixture.mode = fixture.source->gdtf_mode;    // a mode chosen for the stand-in means nothing to the file's own type
            fixture.mode_changed = false;
        }
    }
    return missed;
}

ShowEdits edits_for_export(const MvrScene& scene, const ShowEdits& edits, const TypeSwaps& swaps) {
    ShowEdits out = edits;
    if (swaps.empty()) return out;
    for (FixtureEdit& edit : out.fixtures) {
        if (!edit.mode || edit.gdtf) continue;    // a type of its own: its mode is a mode of that type, and goes out as it is
        for (const MvrFixture& described : scene.fixtures) {
            if (described.uuid != edit.uuid) continue;
            if (const TypeSwap* swap = swaps.stand_in(described.gdtf_spec)) {
                const std::string* back = swap->mode_back(*edit.mode);
                if (back) {
                    edit.mode = *back;
                } else {
                    edit.mode.reset();
                }
            }
            break;
        }
    }
    return out;
}

std::vector<RigFixture> merge_rig(const MvrScene& scene, const ShowEdits& edits, const TypeSwaps& swaps) {
    std::vector<RigFixture> rig;
    rig.reserve(scene.fixtures.size() + edits.added.size());
    for (const MvrFixture& described : scene.fixtures) {
        RigFixture fixture;
        fixture.source = &described;
        fixture.uuid = described.uuid;
        fixture.name = described.name;
        fixture.fixture_id = described.fixture_id;
        fixture.layer = described.layer;
        fixture.class_name = scene.class_name(described.classing);
        fixture.gdtf = described.gdtf_spec;
        fixture.mode = described.gdtf_mode;
        const TypeSwap* entry = swaps.find(described.gdtf_spec);
        if (entry && entry->stands_in()) {
            fixture.stood_in_for = described.gdtf_spec;
            fixture.gdtf = entry->file;
            fixture.from_library = true;
            if (const std::string* counterpart = entry->mode_for(described.gdtf_mode)) fixture.mode = *counterpart;
        }
        fixture.file_mode = fixture.mode;
        fixture.world = described.world;
        fixture.placed = described.placed;
        fixture.invert_pan = described.dmx_invert_pan;
        fixture.invert_tilt = described.dmx_invert_tilt;
        // MVR counts breaks from 0. The first break's patch is the address on break 0, or failing
        // that the first one given: files in the wild do not all start at 0.
        for (const MvrAddress& patch : described.addresses) {
            if (patch.break_id == 0 || !fixture.first_patch) {
                fixture.first_patch = &patch;
                fixture.universe = patch.universe();
                fixture.address = patch.address();
            }
        }
        if (fixture.universe <= 0) fixture.address = 1;

        if (!described.uuid.empty()) {
            fixture.removed = edits.is_removed(described.uuid);
            if (const FixtureEdit* edit = fixture.removed ? nullptr : edits.find(described.uuid)) {
                if (edit->world) {
                    fixture.world = *edit->world;
                    fixture.moved = true;
                    fixture.placed = true;
                }
                if (edit->name && *edit->name != fixture.name) {
                    fixture.name = *edit->name;
                    fixture.renamed = true;
                }
                if (edit->fixture_id && *edit->fixture_id != fixture.fixture_id) {
                    fixture.fixture_id = *edit->fixture_id;
                    fixture.renamed = true;
                }
                if (edit->invert_pan && *edit->invert_pan != fixture.invert_pan) {
                    fixture.invert_pan = *edit->invert_pan;
                    fixture.reinverted = true;
                }
                if (edit->invert_tilt && *edit->invert_tilt != fixture.invert_tilt) {
                    fixture.invert_tilt = *edit->invert_tilt;
                    fixture.reinverted = true;
                }
                if (edit->universe && edit->address) {
                    fixture.universe = *edit->universe;
                    fixture.address = *edit->address;
                    fixture.repatched = true;
                }
                if (edit->gdtf) {
                    // A type of its own: the rig file's, and any stand-in for it, no longer apply.
                    fixture.gdtf = *edit->gdtf;
                    fixture.from_library = true;
                    fixture.stood_in_for.clear();
                    fixture.retyped = true;
                    fixture.mode = edit->mode ? *edit->mode : std::string();
                    fixture.file_mode = fixture.mode;    // the rig file's mode names nothing of this type
                    fixture.mode_changed = false;
                } else if (edit->mode) {
                    fixture.mode = *edit->mode;
                    fixture.mode_changed = true;
                }
            }
        }
        fixture.own_invert_pan = fixture.invert_pan;
        fixture.own_invert_tilt = fixture.invert_tilt;
        if (entry && !fixture.retyped) {
            // Mirroring twice is not mirroring: the type's and the fixture's own combine.
            fixture.invert_pan = fixture.invert_pan != entry->invert_pan;
            fixture.invert_tilt = fixture.invert_tilt != entry->invert_tilt;
            fixture.pan_offset = entry->pan_offset;
            fixture.tilt_offset = entry->tilt_offset;
        }
        rig.push_back(std::move(fixture));
    }
    for (const AddedFixture& entry : edits.added) {
        RigFixture fixture;
        fixture.added = true;
        fixture.from_library = true;
        fixture.uuid = entry.uuid;
        fixture.name = entry.name;
        fixture.fixture_id = entry.fixture_id;
        fixture.layer = "Added in Openviz";
        fixture.gdtf = entry.gdtf;
        fixture.mode = entry.mode;
        fixture.file_mode = entry.mode;
        fixture.world = entry.world;
        fixture.universe = entry.universe;
        fixture.address = entry.address;
        fixture.invert_pan = entry.invert_pan;
        fixture.invert_tilt = entry.invert_tilt;
        fixture.own_invert_pan = entry.invert_pan;
        fixture.own_invert_tilt = entry.invert_tilt;
        if (const TypeSwap* correction = swaps.find(entry.gdtf)) {
            fixture.invert_pan = fixture.invert_pan != correction->invert_pan;
            fixture.invert_tilt = fixture.invert_tilt != correction->invert_tilt;
            fixture.pan_offset = correction->pan_offset;
            fixture.tilt_offset = correction->tilt_offset;
        }
        rig.push_back(std::move(fixture));
    }
    return rig;
}

}  // namespace viz
