// vizcore_inspect: prints what the core reads from a .gdtf, a .mvr or a bare description.xml.
//
//   vizcore_inspect fixture.gdtf
//   vizcore_inspect rig.mvr
//   vizcore_inspect --summary rig.mvr     a few facts, one per line and tab-separated, for programs
//   vizcore_inspect --patch rig.mvr       every fixture with the patch and mode it will get, likewise
//   vizcore_inspect --plot rig.mvr [library]   where everything is and how big, for drawing a plot
//   vizcore_inspect --screens rig.mvr     its video screens: where each one's picture goes, and what the file says it shows
//   vizcore_inspect --walls rig.mvr [library]   the walls of LED panels among its fixtures, which a screen can stand in for
//   vizcore_inspect --types rig.mvr [library]   its fixture types, which of them the file says little about, and what stands in for each
//   vizcore_inspect --stand-in rig.mvr <type> <library> <file>   has a file of the library stand in for a type; --no-stand-in rig.mvr <type> undoes it
//   vizcore_inspect --models fixture.gdtf the 3D model of every part: its size as stated and as the file has it, and its materials
//   vizcore_inspect --wheels fixture.gdtf every wheel slot: its picture, and how much light that passes
//   vizcore_inspect --clusters fixture.gdtf   which of its beams are drawn as one, mode by mode
//   vizcore_inspect --export rig.mvr out.mvr [library]   the rig with the edits kept beside it (rig.edits.xml) written in
//   vizcore_inspect --add rig.mvr library type.gdtf mode count universe address first-id   add fixtures of a library type to the rig's edits
//   vizcore_inspect --remove rig.mvr uuid[,uuid...]   take fixtures out of the rig (in its edits; the MVR is not touched)
//   vizcore_inspect --restore rig.mvr uuid[,uuid...]  put removed fixtures back
//   vizcore_inspect --remove-type rig.mvr <type>      every fixture of that type out of the rig; --restore-type puts them back
//   vizcore_inspect --set rig.mvr library uuid[,uuid...] key=value ...   change fixtures' place, aim, patch, mode, name, number or inversion (in the edits)
//   vizcore_inspect --state fixture.gdtf <mode> [slot=level ...]   what the fixture's pan, tilt and beams do with that DMX
//   vizcore_inspect --movie-sound in.mov sound.wav <start seconds> out.mov   a movie of the visualizer's again, with sound
//   vizcore_inspect --panorama <prefix> out.png <width>   six views (<prefix>0.png to 5.png) made into a 360-degree picture
//   vizcore_inspect --correct rig.mvr <type> <pan 0|1> <tilt 0|1> <pan offset> <tilt offset>   which way a type's pan and tilt run, and from where
//   vizcore_inspect --dimmer-curve rig.mvr <type> <linear|square|inverse-square|s-curve>   how a type's dimmer level becomes light in the show
//   vizcore_inspect --psn packet.bin   a PosiStageNet datagram as the core reads it: its kind, then a line per tracker
// "library" is the folder of GDTF files that added fixtures take their types from; --patch takes it as a last argument too.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "vizcore/archive.h"
#include "vizcore/evaluate.h"
#include "vizcore/files.h"
#include "vizcore/fixture_state.h"
#include "vizcore/gdtf.h"
#include "vizcore/image.h"
#include "vizcore/mesh.h"
#include "vizcore/mvr.h"
#include "vizcore/movie.h"
#include "vizcore/mvr_export.h"
#include "vizcore/pieces.h"
#include "vizcore/psn.h"
#include "vizcore/rig.h"
#include "vizcore/screens.h"
#include "vizcore/show_edits.h"

using namespace viz;

namespace {

bool ends_with(const std::string& text, const char* suffix) {
    std::string s(suffix);
    if (text.size() < s.size()) return false;
    for (size_t i = 0; i < s.size(); ++i) {
        char c = text[text.size() - s.size() + i];
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        if (c != s[i]) return false;
    }
    return true;
}

size_t print(const Diagnostics& diagnostics) {
    for (const Diagnostic& item : diagnostics.items) {
        std::printf("  %s: %s: %s\n", item.severity == Severity::Error ? "ERROR" : "warning", item.where.c_str(),
                    item.message.c_str());
    }
    return diagnostics.count(Severity::Error);
}

void print_geometry(const Geometry& geometry, int depth) {
    std::printf("  %*s%s '%s'", depth * 2, "", to_string(geometry.type), geometry.name.c_str());
    if (geometry.type == GeometryType::GeometryReference) {
        std::printf(" -> '%s'", geometry.reference.c_str());
        for (const GeometryBreak& entry : geometry.breaks) std::printf(" [break %d offset %d]", entry.dmx_break, entry.dmx_offset);
    }
    if (geometry.beam) {
        std::printf("  %.0f lm, beam %.1f deg, field %.1f deg", geometry.beam->luminous_flux, geometry.beam->beam_angle,
                    geometry.beam->field_angle);
    }
    std::printf("\n");
    for (const Geometry& child : geometry.children) print_geometry(child, depth + 1);
}

size_t print_fixture(const FixtureType& fixture, const Diagnostics& diagnostics) {
    std::printf("%s / %s  (GDTF %s, id %s)\n", fixture.manufacturer.c_str(), fixture.name.c_str(),
                fixture.data_version.c_str(), fixture.fixture_type_id.c_str());
    std::printf("  %zu attributes, %zu wheels, %zu emitters, %zu models\n", fixture.attributes.size(),
                fixture.wheels.size(), fixture.emitters.size(), fixture.models.size());
    for (const Wheel& wheel : fixture.wheels) std::printf("  wheel '%s': %zu slots\n", wheel.name.c_str(), wheel.slots.size());
    std::printf("geometry:\n");
    for (const Geometry& geometry : fixture.geometries) print_geometry(geometry, 0);

    size_t errors = print(diagnostics);
    for (size_t m = 0; m < fixture.modes.size(); ++m) {
        ModeEvaluator evaluator(fixture, m);
        std::printf("mode '%s': %zu channels", fixture.modes[m].name.c_str(), evaluator.channels().size());
        for (int dmx_break : evaluator.breaks()) std::printf(", %zu slots on break %d", evaluator.footprint(dmx_break), dmx_break);
        std::printf("\n");
        for (const ChannelInstance& instance : evaluator.channels()) {
            const DmxChannel& channel = fixture.modes[m].channels[static_cast<size_t>(instance.channel)];
            std::string slots;
            for (int offset : instance.offsets) slots += (slots.empty() ? "" : ",") + std::to_string(offset);
            if (slots.empty()) slots = "virtual";
            size_t functions = 0;
            for (const LogicalChannel& logical : channel.logical_channels) functions += logical.functions.size();
            std::printf("  %-9s %2d-bit  %-28s on %-18s %zu function%s, default %u\n", slots.c_str(), channel.bytes * 8,
                        channel.name.c_str(), instance.geometry.c_str(), functions, functions == 1 ? "" : "s",
                        channel.default_value());
        }
        errors += print(evaluator.diagnostics());
    }
    return errors;
}

// Names come from files; a tab or a line break in one must not look like a new field or line.
std::string field(const std::string& text) {
    std::string out = text;
    for (char& c : out) {
        if (static_cast<unsigned char>(c) < 0x20) c = ' ';
    }
    return out;
}

// What the launcher shows about a file before it is opened.
TypeSwaps read_swaps(const std::string& mvr_path);
ShowEdits read_edits(const std::string& mvr_path);

int summarize(const std::string& path) {
    Archive archive;
    std::string error;
    if (!archive.open_file(path, error)) {
        std::printf("error\t%s\n", field(error).c_str());
        return 1;
    }

    if (archive.contains("GeneralSceneDescription.xml")) {
        MvrResult result = load_mvr(archive);
        if (!result.ok) {
            std::printf("error\tits scene description cannot be read\n");
            return 1;
        }
        std::printf("kind\tmvr\n");
        std::printf("provider\t%s\n", field(result.scene.provider + " " + result.scene.provider_version).c_str());
        std::printf("fixtures\t%zu\n", result.scene.fixtures.size());
        std::map<std::string, size_t> present;
        std::map<std::string, size_t> missing;
        size_t without_type = 0;
        size_t unpatched = 0;
        int universes = 0;
        for (const MvrFixture& fixture : result.scene.fixtures) {
            if (fixture.gdtf_spec.empty()) {
                ++without_type;
            } else if (find_gdtf_entry(archive, fixture.gdtf_spec).empty()) {
                ++missing[fixture.gdtf_spec];
            } else {
                ++present[fixture.gdtf_spec];
            }
            if (fixture.addresses.empty()) ++unpatched;
            for (const MvrAddress& address : fixture.addresses) {
                if (address.universe() > universes) universes = address.universe();
            }
        }
        for (const auto& [spec, count] : present) std::printf("type\t%zu\t%s\n", count, field(spec).c_str());
        for (const auto& [spec, count] : missing) std::printf("missing\t%zu\t%s\n", count, field(spec).c_str());
        // Types the file says next to nothing about (a console's placeholders): fixtures of it, and
        // of the mode most of them are in, how many channels are silent and how many it has.
        const TypeSwaps swaps = read_swaps(path);
        const ShowEdits edits = read_edits(path);
        for (const auto& [spec, everything] : present) {
            // Fixtures taken out of the show are not there to be given a real type.
            size_t count = 0;
            for (const MvrFixture& fixture : result.scene.fixtures) count += fixture.gdtf_spec == spec && !edits.is_removed(fixture.uuid) ? 1 : 0;
            if (count == 0) continue;
            std::vector<uint8_t> bytes;
            Archive inner;
            if (!archive.read(find_gdtf_entry(archive, spec), bytes) || !inner.open_memory(std::move(bytes), error)) continue;
            const GdtfResult type = load_gdtf(inner);
            if (!type.ok) continue;
            std::map<int, size_t> used;
            for (const MvrFixture& fixture : result.scene.fixtures) {
                if (fixture.gdtf_spec == spec && !edits.is_removed(fixture.uuid)) ++used[std::max(type.fixture.find_mode(fixture.gdtf_mode), 0)];
            }
            int busiest = 0;
            for (const auto& [index, fixtures] : used) {
                if (fixtures > used[busiest]) busiest = index;
            }
            const ModeSilence silence = mode_silence(type.fixture, static_cast<size_t>(busiest));
            if (silence.says_little()) std::printf("bare\t%zu\t%zu\t%zu\t%s\n", count, silence.silent, silence.channels, field(spec).c_str());
        }
        for (const TypeSwap& swap : swaps.swaps) {
            if (swap.stands_in()) std::printf("standin\t%s\t%s\n", field(swap.type).c_str(), field(swap.file).c_str());
        }
        // Fixtures the file gives no place, and that have not been given one since: they stand in one spot and are left dark.
        size_t unplaced = 0;
        for (const RigFixture& fixture : merge_rig(result.scene, read_edits(path), swaps)) unplaced += !fixture.removed && !fixture.placed ? 1 : 0;
        std::printf("unplaced\t%zu\n", unplaced);
        std::printf("without_type\t%zu\n", without_type);
        std::printf("unpatched\t%zu\n", unpatched);
        std::printf("universes\t%d\n", universes);
        // The scenery around the fixtures: how many objects, and how many of them come with a shape.
        size_t shaped = 0;
        for (const MvrObject& object : result.scene.objects) shaped += object.geometry.empty() ? 0 : 1;
        std::printf("objects\t%zu\n", result.scene.objects.size());
        std::printf("objects_with_shape\t%zu\n", shaped);
        std::printf("layers\t%zu\n", result.scene.layers.size());
        std::printf("classes\t%zu\n", result.scene.classes.size());
        // What is in each layer and each class: "layer" or "class", fixtures in it, set pieces in it, its name.
        // Only those with something in them; a thing without a class is in none.
        std::map<std::string, std::pair<size_t, size_t>> by_layer, by_class;
        for (const MvrFixture& fixture : result.scene.fixtures) {
            ++by_layer[fixture.layer].first;
            const std::string name = result.scene.class_name(fixture.classing);
            if (!name.empty()) ++by_class[name].first;
        }
        for (const MvrObject& object : result.scene.objects) {
            if (object.geometry.empty()) continue;
            ++by_layer[object.layer].second;
            const std::string name = result.scene.class_name(object.classing);
            if (!name.empty()) ++by_class[name].second;
        }
        for (const auto& [name, counts] : by_layer) std::printf("layer\t%zu\t%zu\t%s\n", counts.first, counts.second, field(name).c_str());
        for (const auto& [name, counts] : by_class) std::printf("class\t%zu\t%zu\t%s\n", counts.first, counts.second, field(name).c_str());
        std::printf("warnings\t%zu\n", result.diagnostics.count(Severity::Warning));
        return 0;
    }

    GdtfResult result = load_gdtf(archive);
    if (!result.ok) {
        std::printf("error\tits description cannot be read\n");
        return 1;
    }
    std::printf("kind\tgdtf\n");
    std::printf("manufacturer\t%s\n", field(result.fixture.manufacturer).c_str());
    std::printf("name\t%s\n", field(result.fixture.name).c_str());
    size_t warnings = result.diagnostics.count(Severity::Warning);
    for (size_t m = 0; m < result.fixture.modes.size(); ++m) {
        ModeEvaluator evaluator(result.fixture, m);
        size_t slots = 0;
        for (int dmx_break : evaluator.breaks()) slots += evaluator.footprint(dmx_break);
        std::printf("mode\t%zu\t%s\n", slots, field(result.fixture.modes[m].name).c_str());
        warnings += evaluator.diagnostics().count(Severity::Warning);
    }
    std::printf("warnings\t%zu\n", warnings);
    return 0;
}

// Every fixture of a rig as it will be patched when the show is opened: the MVR's own patch and
// mode with any saved edits laid over, and the slots its mode occupies on DMX break 1.
//
//   fixture <uuid> <fixture id> <name> <layer> <type> <mode> <universe> <address> <slots> <notes> <kg> <watts> <rated|sources>
// kg is 0 where the fixture's file states no weight. watts is the fixture's rated draw where its
// file gives one ("rated"), else what its light sources consume ("sources"), which is less.
//
// Notes, comma-separated: moved, repatched, remoded, renamed, reinverted, retyped (changed by a
// saved edit; retyped: a type of its own from the fixture library); added (not in the MVR: added since, its type from the fixture library); removed (taken
// out since: listed so that it can be put back, but not on stage); notype (its GDTF cannot be
// found, so it will be left out); nomode (the mode named does not exist; the first is used);
// unplaced (the file gives it no place and it has not been given one: it is left dark).
//
// After each fixture, where it stands and how its pan and tilt are read:
//   place <uuid> <x> <y> <z> <turn about X> <about Y> <about Z> <own pan inverted 0|1> <own tilt inverted> <has pan 0|1> <has tilt>
// Millimetres and degrees, the rig file's coordinates (vizcore/types.h says how the angles read).
// The inversions are the fixture's own, before what its type's entry adds; has pan and has tilt
// say whether its mode has such a channel at all. Then, for a type in force that offers more than one mode:
//   modes <uuid> <mode> <slots> [<mode> <slots> ...]
// A whole number and nothing else.
bool whole_number(const std::string& written, long long& out) {
    if (written.empty() || written.size() > 12) return false;
    long long value = 0;
    for (char c : written) {
        if (c < '0' || c > '9') return false;
        value = value * 10 + (c - '0');
    }
    out = value;
    return true;
}

// Identifiers are the same whatever the case of their letters.
bool same_id(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        const char x = a[i] >= 'a' && a[i] <= 'z' ? static_cast<char>(a[i] - 'a' + 'A') : a[i];
        const char y = b[i] >= 'a' && b[i] <= 'z' ? static_cast<char>(b[i] - 'a' + 'A') : b[i];
        if (x != y) return false;
    }
    return true;
}

ShowEdits read_edits(const std::string& mvr_path) {
    std::ifstream file = read_stream(show_edits_path(mvr_path));
    if (!file) return ShowEdits();
    const std::string xml((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    return parse_show_edits(xml).edits;
}

// The set pieces added to the show in the application, as it keeps them beside the rig file, with
// model pieces' files looked for in the models folder beside the fixture library.
std::vector<PlacedPiece> read_pieces(const std::string& mvr_path, const std::string& library_dir = std::string()) {
    std::ifstream file = read_stream(pieces_path(mvr_path));
    if (!file) return {};
    const std::string json((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    std::vector<PlacedPiece> pieces = parse_pieces(json);
    if (!library_dir.empty()) resolve_models(pieces, models_folder(library_dir));
    return pieces;
}

TypeSwaps read_swaps(const std::string& mvr_path) {
    std::ifstream file = read_stream(type_swaps_path(mvr_path));
    if (!file) return TypeSwaps();
    const std::string xml((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    return parse_type_swaps(xml);
}

// The show's fixtures as they will stand: its edits and its stand-in types laid over the rig file,
// and a stand-in whose file is not in the library left out again.
std::vector<RigFixture> rig_of(const MvrScene& scene, const std::string& mvr_path, const std::string& library_dir) {
    std::vector<RigFixture> rig = merge_rig(scene, read_edits(mvr_path), read_swaps(mvr_path));
    drop_missing_stand_ins(rig, library_dir);
    return rig;
}

// Writes the stand-ins beside the MVR, whole or not at all; with none left, the file goes.
bool write_swaps(const std::string& mvr_path, const TypeSwaps& swaps) {
    const std::string path = type_swaps_path(mvr_path);
    if (swaps.empty()) {
        remove_file(path);
        return true;
    }
    const std::string partial = path + ".part";
    {
        std::ofstream file = write_stream(partial);
        file << write_type_swaps(swaps);
        file.flush();
        if (!file) {
            remove_file(partial);
            return false;
        }
    }
    return rename_file(partial, path);
}

// Writes the edits beside the MVR, whole or not at all.
bool write_edits(const std::string& mvr_path, const ShowEdits& edits) {
    const std::string path = show_edits_path(mvr_path);
    const std::string partial = path + ".part";
    {
        std::ofstream file = write_stream(partial);
        file << write_show_edits(edits);
        file.flush();
        if (!file) {
            remove_file(partial);
            return false;
        }
    }
    return rename_file(partial, path);
}

// A fixture type from the fixture library, by file name. Not ok if it is not there or cannot be read.
GdtfResult library_type(const std::string& library_dir, const std::string& name) {
    Archive archive;
    std::string error;
    if (library_dir.empty() || !archive.open_file(library_dir + "/" + name, error)) return GdtfResult();
    return load_gdtf(archive);
}

// Whether a mode has a channel of this attribute, on any part of the fixture.
bool mode_has(const FixtureType& type, size_t mode_index, const char* attribute) {
    if (mode_index >= type.modes.size()) return false;
    for (const DmxChannel& channel : type.modes[mode_index].channels) {
        for (const LogicalChannel& logical : channel.logical_channels) {
            if (logical.attribute == attribute) return true;
            for (const ChannelFunction& function : logical.functions) {
                if (function.attribute == attribute) return true;
            }
        }
    }
    return false;
}

// --patch also gives the rig file's focus points, "focuspoint <uuid> <name> <x> <y> <z>" (millimetres),
// first; and after a fixture's own lines, "focus <uuid> <focus point uuid>" where the file aims it at one.
int patch(const std::string& path, const std::string& library_dir) {
    Archive archive;
    std::string error;
    if (!archive.open_file(path, error)) {
        std::printf("error\t%s\n", field(error).c_str());
        return 1;
    }
    MvrResult result = load_mvr(archive);
    if (!result.ok) {
        std::printf("error\tits scene description cannot be read\n");
        return 1;
    }
    for (const MvrObject& object : result.scene.objects) {
        if (object.kind != MvrObjectKind::FocusPoint) continue;
        std::printf("focuspoint\t%s\t%s\t%.3f\t%.3f\t%.3f\n", field(object.uuid).c_str(), field(object.name).c_str(), object.world.o[0], object.world.o[1],
                    object.world.o[2]);
    }
    std::map<std::string, GdtfResult> types;    // by archive entry, or "library|<file>" for an added fixture's or a stand-in
    std::map<std::string, std::string> mode_lengths;    // by the same: the type's modes and their slots, as written out
    // What a mode of a type comes to, worked out once: a rig has hundreds of fixtures and a dozen types.
    struct ModeFacts {
        size_t slots = 0;
        double watts = 0.0;
        bool rated = false;
        bool has_pan = false;
        bool has_tilt = false;
    };
    std::map<std::pair<std::string, int>, ModeFacts> mode_facts;
    for (const RigFixture& fixture : rig_of(result.scene, path, library_dir)) {
        std::string mode = fixture.mode;
        std::string notes;
        auto note = [&notes](const char* text) { notes += (notes.empty() ? "" : ",") + std::string(text); };
        if (fixture.added) note("added");
        if (!fixture.stood_in_for.empty()) note("standin");
        if (fixture.removed) note("removed");
        if (fixture.moved) note("moved");
        if (fixture.repatched) note("repatched");
        if (fixture.renamed) note("renamed");
        if (fixture.reinverted) note("reinverted");
        if (fixture.retyped) note("retyped");
        if (!fixture.placed && !fixture.removed) note("unplaced");
        bool has_pan = false, has_tilt = false;
        std::string modes_line;

        size_t slots = 0;
        double weight = 0.0;    // kilograms; 0 if its file does not say
        double watts = 0.0;
        bool rated = false;     // watts is the fixture's rated draw, not just its light sources
        const std::string entry = fixture.from_library ? fixture.gdtf : find_gdtf_entry(archive, fixture.gdtf);
        const std::string key = (fixture.from_library ? "library|" : "") + entry;
        if (entry.empty()) {
            note("notype");
        } else {
            auto found = types.find(key);
            if (found == types.end()) {
                GdtfResult loaded;
                if (fixture.from_library) {
                    loaded = library_type(library_dir, entry);
                } else {
                    std::vector<uint8_t> bytes;
                    Archive gdtf_archive;
                    if (archive.read(entry, bytes) && gdtf_archive.open_memory(std::move(bytes), error)) loaded = load_gdtf(gdtf_archive);
                }
                found = types.emplace(key, std::move(loaded)).first;
            }
            const GdtfResult& type = found->second;
            if (!type.ok || type.fixture.modes.empty()) {
                note("notype");
            } else {
                int index = mode.empty() && (fixture.added || fixture.retyped) ? 0 : type.fixture.find_mode(mode);
                if (fixture.mode_changed) {
                    if (index >= 0) {
                        note("remoded");
                    } else {
                        // The mode an edit chose is gone from the type: the MVR's own stands, as in the visualizer.
                        index = fixture.source ? type.fixture.find_mode(fixture.file_mode) : -1;
                    }
                }
                if (index < 0) {
                    index = 0;
                    note("nomode");
                }
                mode = type.fixture.modes[static_cast<size_t>(index)].name;
                auto facts = mode_facts.find({key, index});
                if (facts == mode_facts.end()) {
                    ModeFacts worked;
                    const ModeEvaluator evaluator(type.fixture, static_cast<size_t>(index));
                    worked.slots = evaluator.footprint(1);
                    worked.watts = mode_power_watts(evaluator, &worked.rated);
                    worked.has_pan = mode_has(type.fixture, static_cast<size_t>(index), "Pan");
                    worked.has_tilt = mode_has(type.fixture, static_cast<size_t>(index), "Tilt");
                    facts = mode_facts.emplace(std::make_pair(key, index), worked).first;
                }
                slots = facts->second.slots;
                weight = type.fixture.weight;
                watts = facts->second.watts;
                rated = facts->second.rated;
                has_pan = facts->second.has_pan;
                has_tilt = facts->second.has_tilt;
                if (type.fixture.modes.size() > 1) {
                    auto lengths = mode_lengths.find(key);
                    if (lengths == mode_lengths.end()) {
                        std::string listed;
                        for (size_t m = 0; m < type.fixture.modes.size(); ++m) {
                            listed += "\t" + field(type.fixture.modes[m].name) + "\t" + std::to_string(ModeEvaluator(type.fixture, m).footprint(1));
                        }
                        lengths = mode_lengths.emplace(key, std::move(listed)).first;
                    }
                    modes_line = lengths->second;
                }
            }
        }

        std::string type_name = fixture.gdtf;
        if (ends_with(type_name, ".gdtf")) type_name.resize(type_name.size() - 5);
        std::printf("fixture\t%s\t%s\t%s\t%s\t%s\t%s\t%d\t%d\t%zu\t%s\t%.3f\t%.1f\t%s\n", field(fixture.uuid).c_str(),
                    field(fixture.fixture_id).c_str(), field(fixture.name).c_str(), field(fixture.layer).c_str(),
                    field(type_name).c_str(), field(mode).c_str(), fixture.universe, fixture.address, slots, notes.c_str(), weight, watts,
                    rated ? "rated" : "sources");
        double turn[3];
        turn_angles(fixture.world, turn);
        std::printf("place\t%s\t%.3f\t%.3f\t%.3f\t%.4f\t%.4f\t%.4f\t%d\t%d\t%d\t%d\n", field(fixture.uuid).c_str(), fixture.world.o[0], fixture.world.o[1],
                    fixture.world.o[2], turn[0], turn[1], turn[2], fixture.own_invert_pan ? 1 : 0, fixture.own_invert_tilt ? 1 : 0, has_pan ? 1 : 0, has_tilt ? 1 : 0);
        if (!modes_line.empty()) std::printf("modes\t%s%s\n", field(fixture.uuid).c_str(), modes_line.c_str());
        if (fixture.source && !fixture.source->focus.empty()) std::printf("focus\t%s\t%s\n", field(fixture.uuid).c_str(), field(fixture.source->focus).c_str());
    }
    return 0;
}

const char* beam_type_name(BeamType type) {
    switch (type) {
        case BeamType::Wash: return "Wash";
        case BeamType::Spot: return "Spot";
        case BeamType::None: return "None";
        case BeamType::Rectangle: return "Rectangle";
        case BeamType::PC: return "PC";
        case BeamType::Fresnel: return "Fresnel";
        case BeamType::Glow: return "Glow";
    }
    return "Wash";
}

// What a plot is drawn from: where each fixture and each piece of scenery is, and how big.
// Millimetres throughout, MVR's coordinates (X toward stage left, which is the audience's right;
// Y upstage; Z up). A placing is twelve numbers: the three axes and the origin of an MVR matrix.
//
//   type <number> <min x y z> <max x y z>      the box a fixture type fills at rest, about its own origin;
//                                              all zero if its file gives no sizes
//   points <number> <x y z> ...                the points that outline it from above, the front and the side
//                                              (fixture_outline_points), where its file gives sizes
//   place <uuid> <type number> <placing>       a fixture that will be on stage (-1: its type is unknown)
//   beam <uuid> <n> <origin x y z> <direction x y z> <lumens> <beam angle> <field angle> <intensity> <kind>
//                                              one of its beams at rest, dimmers full: where the light leaves and which way
//                                              (a unit vector), for light levels; kind is the GDTF beam type
//   unplaced <count>                           fixtures the rig file gives no place: left out of the lines above
//   object <kind> <uuid> <name> <layer> <class>
//   box <object uuid> <placing> <min x y z> <max x y z>   one model file of that object: its bounding box
// Set pieces added in the application come last, as objects of the layer "Added in Openviz" whose
// uuid is "piece:" and the piece's id.
int plot(const std::string& path, const std::string& library_dir) {
    Archive archive;
    std::string error;
    if (!archive.open_file(path, error)) {
        std::printf("error\t%s\n", field(error).c_str());
        return 1;
    }
    MvrResult result = load_mvr(archive);
    if (!result.ok) {
        std::printf("error\tits scene description cannot be read\n");
        return 1;
    }
    auto print_placing = [](const Transform& at) {
        std::printf("\t%.5f\t%.5f\t%.5f\t%.5f\t%.5f\t%.5f\t%.5f\t%.5f\t%.5f\t%.1f\t%.1f\t%.1f", at.u[0], at.u[1], at.u[2], at.v[0], at.v[1], at.v[2],
                    at.w[0], at.w[1], at.w[2], at.o[0], at.o[1], at.o[2]);
    };

    struct Loaded {
        Archive archive;
        GdtfResult type;
    };
    std::map<std::string, std::unique_ptr<Loaded>> types;    // by archive entry, or "library|<file>"
    std::map<std::string, int> numbers;                      // by that and the mode's geometry
    size_t unplaced = 0;
    for (const RigFixture& fixture : rig_of(result.scene, path, library_dir)) {
        if (fixture.removed) continue;
        if (!fixture.placed) {
            ++unplaced;    // all in one spot, and dark in the visualizer: not drawn, and lighting nothing
            continue;
        }
        const std::string entry = fixture.from_library ? fixture.gdtf : find_gdtf_entry(archive, fixture.gdtf);
        int number = -1;
        if (!entry.empty()) {
            const std::string key = (fixture.from_library ? "library|" : "") + entry;
            auto found = types.find(key);
            if (found == types.end()) {
                auto loaded = std::make_unique<Loaded>();
                bool opened = false;
                if (fixture.from_library) {
                    opened = !library_dir.empty() && loaded->archive.open_file(library_dir + "/" + entry, error);
                } else {
                    std::vector<uint8_t> bytes;
                    opened = archive.read(entry, bytes) && loaded->archive.open_memory(std::move(bytes), error);
                }
                if (opened) loaded->type = load_gdtf(loaded->archive);
                found = types.emplace(key, std::move(loaded)).first;
            }
            const Loaded& loaded = *found->second;
            if (loaded.type.ok && !loaded.type.fixture.modes.empty()) {
                // The mode it will have, found as the patch list finds it.
                int index = fixture.mode.empty() && fixture.added ? 0 : loaded.type.fixture.find_mode(fixture.mode);
                if (index < 0 && fixture.mode_changed && fixture.source) index = loaded.type.fixture.find_mode(fixture.file_mode);
                if (index < 0) index = 0;
                const std::string& geometry = loaded.type.fixture.modes[static_cast<size_t>(index)].geometry;
                auto numbered = numbers.find(key + "|" + geometry);
                if (numbered == numbers.end()) {
                    numbered = numbers.emplace(key + "|" + geometry, static_cast<int>(numbers.size())).first;
                    const Bounds bounds = fixture_bounds(loaded.type.fixture, geometry, &loaded.archive);
                    std::printf("type\t%d\t%.1f\t%.1f\t%.1f\t%.1f\t%.1f\t%.1f\n", numbered->second, bounds.min[0] * 1000.0, bounds.min[1] * 1000.0,
                                bounds.min[2] * 1000.0, bounds.max[0] * 1000.0, bounds.max[1] * 1000.0, bounds.max[2] * 1000.0);
                    // Its outline, to draw it as it looks rather than as its box.
                    const std::vector<std::array<double, 3>> outline = fixture_outline_points(loaded.type.fixture, geometry, &loaded.archive);
                    if (!outline.empty()) {
                        std::printf("points\t%d", numbered->second);
                        for (const auto& point : outline) std::printf("\t%.1f\t%.1f\t%.1f", point[0] * 1000.0, point[1] * 1000.0, point[2] * 1000.0);
                        std::printf("\n");
                    }
                }
                number = numbered->second;
                // Its beams at rest, in the rig's world: the fixture's placing is millimetres and may carry a scale.
                size_t count = 0;
                for (const RestBeam& beam : rest_beams(loaded.type.fixture, static_cast<size_t>(index))) {
                    const Transform& at = fixture.world;
                    const double ox = beam.world.m[0][3] * 1000.0, oy = beam.world.m[1][3] * 1000.0, oz = beam.world.m[2][3] * 1000.0;
                    const double dx = -beam.world.m[0][2], dy = -beam.world.m[1][2], dz = -beam.world.m[2][2];    // the light leaves along the beam's -Z
                    const double origin[3] = {ox * at.u[0] + oy * at.v[0] + oz * at.w[0] + at.o[0], ox * at.u[1] + oy * at.v[1] + oz * at.w[1] + at.o[1],
                                              ox * at.u[2] + oy * at.v[2] + oz * at.w[2] + at.o[2]};
                    double direction[3] = {dx * at.u[0] + dy * at.v[0] + dz * at.w[0], dx * at.u[1] + dy * at.v[1] + dz * at.w[1], dx * at.u[2] + dy * at.v[2] + dz * at.w[2]};
                    const double length = std::sqrt(direction[0] * direction[0] + direction[1] * direction[1] + direction[2] * direction[2]);
                    if (length <= 0.0 || !std::isfinite(length)) continue;
                    for (double& part : direction) part /= length;
                    std::printf("beam\t%s\t%zu\t%.1f\t%.1f\t%.1f\t%.5f\t%.5f\t%.5f\t%.1f\t%.3f\t%.3f\t%.4f\t%s\n", field(fixture.uuid).c_str(), count++,
                                origin[0], origin[1], origin[2], direction[0], direction[1], direction[2], beam.lumens, beam.beam_angle, beam.field_angle,
                                beam.intensity, beam_type_name(beam.beam_type));
                }
            }
        }
        std::printf("place\t%s\t%d", field(fixture.uuid).c_str(), number);
        print_placing(fixture.world);
        std::printf("\n");
    }

    std::printf("unplaced\t%zu\n", unplaced);

    std::map<std::string, MeshResult> shapes;    // by file name; only the bounding boxes are used
    for (const MvrObject& object : result.scene.objects) {
        std::printf("object\t%s\t%s\t%s\t%s\t%s\n", to_string(object.kind), field(object.uuid).c_str(), field(object.name).c_str(),
                    field(object.layer).c_str(), field(result.scene.class_name(object.classing)).c_str());
        for (const MvrGeometry& geometry : object.geometry) {
            auto found = shapes.find(geometry.file);
            if (found == shapes.end()) {
                MeshResult shape = load_scene_mesh(archive, geometry.file);
                shape.mesh.positions.clear();    // the box is all that is kept
                shape.mesh.normals.clear();
                shape.mesh.triangles.clear();
                found = shapes.emplace(geometry.file, std::move(shape)).first;
            }
            const MeshResult& shape = found->second;
            if (!shape.ok) continue;
            std::printf("box\t%s", field(object.uuid).c_str());
            print_placing(compose(object.world, geometry.local));
            std::printf("\t%.1f\t%.1f\t%.1f\t%.1f\t%.1f\t%.1f\n", static_cast<double>(shape.mesh.min[0]) * 1000.0, static_cast<double>(shape.mesh.min[1]) * 1000.0,
                        static_cast<double>(shape.mesh.min[2]) * 1000.0, static_cast<double>(shape.mesh.max[0]) * 1000.0,
                        static_cast<double>(shape.mesh.max[1]) * 1000.0, static_cast<double>(shape.mesh.max[2]) * 1000.0);
        }
    }
    // The set pieces added in the application, as objects of a layer of their own.
    for (const PlacedPiece& placed : read_pieces(path, library_dir)) {
        const std::string& kind = placed.piece.kind;
        const bool rigging = kind == "truss" || kind == "circle" || kind == "corner" || kind == "triangle" || kind == "pipe" || kind == "boom";
        std::printf("object\t%s\tpiece:%s\t%s\tAdded in Openviz\t\n", rigging ? "Truss" : "SceneObject", field(placed.id).c_str(),
                    field(placed.name.empty() ? placed.piece.kind : placed.name).c_str());
        const Mesh shape = placed_piece_mesh(placed);
        if (shape.triangle_count() == 0) continue;    // a model that cannot be read has no box
        double low[3] = {shape.min[0], shape.min[1], shape.min[2]}, high[3] = {shape.max[0], shape.max[1], shape.max[2]};
        std::printf("box\tpiece:%s", field(placed.id).c_str());
        print_placing(placed.world);
        std::printf("\t%.1f\t%.1f\t%.1f\t%.1f\t%.1f\t%.1f\n", low[0] * 1000.0, low[1] * 1000.0, low[2] * 1000.0, high[0] * 1000.0, high[1] * 1000.0,
                    high[2] * 1000.0);
    }
    return 0;
}

// The video screens of a rig, one line each:
//   screen <uuid> <name> <layer> <class> <width> <height> <placing> <source type> <source>
// Millimetres. The placing is that of the picture's middle: its first axis runs to the picture's
// right, the second up it, the third out of it toward whoever looks. The source is the first the
// file names for the screen, both fields empty if it names none.
// The walls of LED panels among a show's fixtures, for putting a screen in their place:
//   wall <number> <fixture type> <panels> <width> <height> <the face's u, v, w and middle>
//   panel <number> <uuid> <fixture id> <name>
int walls(const std::string& path, const std::string& library_dir) {
    Archive archive;
    std::string error;
    if (!archive.open_file(path, error)) {
        std::printf("error\t%s\n", field(error).c_str());
        return 1;
    }
    MvrResult result = load_mvr(archive);
    if (!result.ok) {
        std::printf("error\tits scene description cannot be read\n");
        return 1;
    }
    struct Loaded {
        Archive archive;
        GdtfResult type;
    };
    struct Shape {
        Bounds bounds;
        bool lit = false;
        double toward[3] = {0, 0, 0};    // the first beam's way out, in the fixture's own frame
    };
    std::map<std::string, std::unique_ptr<Loaded>> types;
    std::map<std::string, Shape> shapes;    // by type and the mode's geometry
    std::vector<WallPanel> panels;
    std::vector<const RigFixture*> whose;
    const std::vector<RigFixture> rig = rig_of(result.scene, path, library_dir);
    for (const RigFixture& fixture : rig) {
        if (fixture.removed) continue;
        const std::string entry = fixture.from_library ? fixture.gdtf : find_gdtf_entry(archive, fixture.gdtf);
        if (entry.empty()) continue;
        const std::string key = (fixture.from_library ? "library|" : "") + entry;
        auto found = types.find(key);
        if (found == types.end()) {
            auto loaded = std::make_unique<Loaded>();
            bool opened = false;
            if (fixture.from_library) {
                opened = !library_dir.empty() && loaded->archive.open_file(library_dir + "/" + entry, error);
            } else {
                std::vector<uint8_t> bytes;
                opened = archive.read(entry, bytes) && loaded->archive.open_memory(std::move(bytes), error);
            }
            if (opened) loaded->type = load_gdtf(loaded->archive);
            found = types.emplace(key, std::move(loaded)).first;
        }
        const Loaded& loaded = *found->second;
        if (!loaded.type.ok || loaded.type.fixture.modes.empty()) continue;
        int index = fixture.mode.empty() && fixture.added ? 0 : loaded.type.fixture.find_mode(fixture.mode);
        if (index < 0 && fixture.mode_changed && fixture.source) index = loaded.type.fixture.find_mode(fixture.file_mode);
        if (index < 0) index = 0;
        const std::string& geometry = loaded.type.fixture.modes[static_cast<size_t>(index)].geometry;
        const std::string shape_key = key + "|" + geometry;
        auto shaped = shapes.find(shape_key);
        if (shaped == shapes.end()) {
            Shape shape;
            shape.bounds = fixture_bounds(loaded.type.fixture, geometry, &loaded.archive);
            const std::vector<RestBeam> beams = rest_beams(loaded.type.fixture, static_cast<size_t>(index));
            if (!beams.empty()) {
                shape.lit = true;
                for (int k = 0; k < 3; ++k) shape.toward[k] = -beams[0].world.m[k][2];    // the light leaves along the beam's -Z
            }
            shaped = shapes.emplace(shape_key, shape).first;
        }
        const Shape& shape = shaped->second;
        WallPanel panel;
        panel.uuid = fixture.uuid;
        panel.type = shape_key;
        panel.world = fixture.world;
        for (int k = 0; k < 3; ++k) {
            panel.low[k] = shape.bounds.min[k] * 1000.0;
            panel.high[k] = shape.bounds.max[k] * 1000.0;
        }
        panel.lit = shape.lit;
        const Transform& at = fixture.world;
        for (int k = 0; k < 3; ++k) panel.toward[k] = shape.toward[0] * at.u[k] + shape.toward[1] * at.v[k] + shape.toward[2] * at.w[k];
        panels.push_back(std::move(panel));
        whose.push_back(&fixture);
    }
    int number = 0;
    for (const PanelWall& wall : find_walls(panels)) {
        const RigFixture& first = *whose[wall.panels.front()];
        std::string type = first.stood_in_for.empty() ? first.gdtf : first.stood_in_for;    // as the rig file calls it
        if (type.size() > 5 && type.compare(type.size() - 5, 5, ".gdtf") == 0) type.resize(type.size() - 5);
        const Transform& at = wall.face.world;
        std::printf("wall\t%d\t%s\t%zu\t%.1f\t%.1f\t%.5f\t%.5f\t%.5f\t%.5f\t%.5f\t%.5f\t%.5f\t%.5f\t%.5f\t%.1f\t%.1f\t%.1f\n", number, field(type).c_str(),
                    wall.panels.size(), wall.face.width, wall.face.height, at.u[0], at.u[1], at.u[2], at.v[0], at.v[1], at.v[2], at.w[0], at.w[1], at.w[2],
                    at.o[0], at.o[1], at.o[2]);
        for (size_t index : wall.panels) {
            const RigFixture& fixture = *whose[index];
            std::printf("panel\t%d\t%s\t%s\t%s\n", number, field(fixture.uuid).c_str(), field(fixture.fixture_id).c_str(), field(fixture.name).c_str());
        }
        ++number;
    }
    return 0;
}

int screens(const std::string& path) {
    Archive archive;
    std::string error;
    if (!archive.open_file(path, error)) {
        std::printf("error\t%s\n", field(error).c_str());
        return 1;
    }
    const MvrResult result = load_mvr(archive);
    if (!result.ok) {
        std::printf("error\tits scene description cannot be read\n");
        return 1;
    }
    for (const ScreenFace& face : screen_faces(result.scene, archive)) {
        const Transform& at = face.world;
        std::printf("screen\t%s\t%s\t%s\t%s\t%.1f\t%.1f\t%.5f\t%.5f\t%.5f\t%.5f\t%.5f\t%.5f\t%.5f\t%.5f\t%.5f\t%.1f\t%.1f\t%.1f\t%s\t%s\n",
                    field(face.uuid).c_str(), field(face.name).c_str(), field(face.layer).c_str(), field(face.class_name).c_str(), face.width,
                    face.height, at.u[0], at.u[1], at.u[2], at.v[0], at.v[1], at.v[2], at.w[0], at.w[1], at.w[2], at.o[0], at.o[1], at.o[2],
                    face.sources.empty() ? "" : field(face.sources[0].type).c_str(), face.sources.empty() ? "" : field(face.sources[0].value).c_str());
    }
    return 0;
}

// added <uuid> for each fixture made.
int add(const std::string& path, const std::string& library_dir, const std::string& gdtf, const std::string& mode, const std::string& count,
        const std::string& universe, const std::string& address, const std::string& first_id) {
    const GdtfResult type = library_type(library_dir, gdtf);
    if (!type.ok || type.fixture.modes.empty()) {
        std::printf("error\t%s is not a usable fixture type in the fixture library\n", field(gdtf).c_str());
        return 1;
    }
    const int index = mode.empty() ? 0 : type.fixture.find_mode(mode);
    if (index < 0) {
        std::printf("error\t%s has no mode called %s\n", field(type.fixture.name).c_str(), field(mode).c_str());
        return 1;
    }
    long long numbers[4] = {0, 0, 1, 0};
    const std::string* written[4] = {&count, &universe, &address, &first_id};
    for (int k = 0; k < 4; ++k) {
        if (!whole_number(*written[k], numbers[k]) || numbers[k] > 1000000) {
            std::printf("error\tcount, universe, address and first id are whole numbers\n");
            return 1;
        }
    }
    if (numbers[0] < 1 || numbers[0] > 512 || numbers[1] > 32767 || numbers[2] < 1 || numbers[2] > 512) {
        std::printf("error\tbetween 1 and 512 fixtures, universe 0 (unpatched) to 32767, address 1 to 512\n");
        return 1;
    }
    AddRequest request;
    request.gdtf = gdtf;
    request.mode = type.fixture.modes[static_cast<size_t>(index)].name;
    request.name = type.fixture.name.empty() ? std::string("Fixture") : type.fixture.name;
    request.count = static_cast<int>(numbers[0]);
    request.universe = static_cast<int>(numbers[1]);
    request.address = static_cast<int>(numbers[2]);
    request.first_id = static_cast<int>(numbers[3]);
    request.footprint = static_cast<int>(ModeEvaluator(type.fixture, static_cast<size_t>(index)).footprint(1));
    ShowEdits edits = read_edits(path);
    const std::vector<std::string> made = add_fixtures(edits, request);
    if (!write_edits(path, edits)) {
        std::printf("error\tthe show's changes could not be saved\n");
        return 1;
    }
    for (const std::string& uuid : made) std::printf("added\t%s\n", uuid.c_str());
    return 0;
}

// removed <uuid> or restored <uuid>.
// `uuids` is one fixture or several, separated by commas: all are known, or nothing is done.
int remove_or_restore(const std::string& path, const std::string& uuids, bool restore) {
    MvrResult result = load_mvr_file(path);
    if (!result.ok) {
        std::printf("error\tits scene description cannot be read\n");
        return 1;
    }
    ShowEdits edits = read_edits(path);
    std::vector<std::string> named;
    for (size_t start = 0; start <= uuids.size();) {
        size_t end = uuids.find(',', start);
        if (end == std::string::npos) end = uuids.size();
        if (end > start) named.push_back(uuids.substr(start, end - start));
        start = end + 1;
    }
    if (named.empty()) {
        std::printf("error\tthe show has no such fixture\n");
        return 1;
    }
    for (const std::string& uuid : named) {
        bool known = edits.find_added(uuid) != nullptr;
        for (const MvrFixture& fixture : result.scene.fixtures) known = known || same_id(fixture.uuid, uuid);
        if (!known) {
            std::printf("error\tthe show has no such fixture\n");
            return 1;
        }
    }
    std::vector<bool> done;
    bool any = false;
    for (const std::string& uuid : named) {
        done.push_back(restore ? restore_fixture(edits, uuid) : remove_fixture(edits, uuid));
        any = any || done.back();
    }
    if (any && !write_edits(path, edits)) {
        std::printf("error\tthe show's changes could not be saved\n");
        return 1;
    }
    for (size_t k = 0; k < named.size(); ++k) {
        std::printf("%s\t%s\n", restore ? (done[k] ? "restored" : "unchanged") : (done[k] ? "removed" : "unchanged"), named[k].c_str());
    }
    return 0;
}

// The same for every fixture of the rig file whose type is `spec`: a type the show can do without
// (dimmer channels, media server layers) taken out, or put back, in one go.
int remove_or_restore_type(const std::string& path, const std::string& spec, bool restore) {
    MvrResult result = load_mvr_file(path);
    if (!result.ok) {
        std::printf("error\tits scene description cannot be read\n");
        return 1;
    }
    std::string uuids;
    for (const MvrFixture& fixture : result.scene.fixtures) {
        if (!fixture.uuid.empty() && same_type(fixture.gdtf_spec, spec)) uuids += (uuids.empty() ? "" : ",") + fixture.uuid;
    }
    if (uuids.empty()) {
        std::printf("error\tthe show has no fixture type %s\n", field(spec).c_str());
        return 1;
    }
    return remove_or_restore(path, uuids, restore);
}

// model <name> <file kind> <triangles> <stated L W H> <file's own L W H> <lowest Z> <problem>
// Sizes in metres. A part whose own size differs much from the stated one is being stretched.
int models_of(const Archive& gdtf, const char* label) {
    GdtfResult type = load_gdtf(gdtf);
    if (!type.ok) {
        std::printf("error\tits description cannot be read\n");
        return 1;
    }
    for (const Model& model : type.fixture.models) {
        MeshResult mesh = load_model_mesh(gdtf, model);
        std::string problem;
        for (const Diagnostic& item : mesh.diagnostics.items) problem += (problem.empty() ? "" : "; ") + item.message;
        const char* kind = model.file.empty() ? "none" : gdtf.contains("models/gltf/" + model.file + ".glb") ? "glb" : gdtf.contains("models/3ds/" + model.file + ".3ds") ? "3ds" : "missing";
        size_t pictures = 0;
        for (const MeshMaterial& material : mesh.mesh.materials) pictures += material.textured() ? 1 : 0;
        std::printf("model\t%s\t%s\t%s\t%zu\t%.4f %.4f %.4f\t%.4f %.4f %.4f\t%.4f\t%s\t%zu materials, %zu with a picture\n", label, field(model.name).c_str(), kind,
                    mesh.mesh.triangle_count(), model.length, model.width, model.height, mesh.native_size[0], mesh.native_size[1],
                    mesh.native_size[2], static_cast<double>(mesh.mesh.min[2]), field(problem).c_str(), mesh.mesh.materials.size(), pictures);
    }
    return 0;
}

// slot <wheel> <number> <name> <picture> <width> <height> <share of light passed> <facets> <problem>
int wheels_of(const Archive& gdtf, const char* label) {
    GdtfResult type = load_gdtf(gdtf);
    if (!type.ok) {
        std::printf("error\tits description cannot be read\n");
        return 1;
    }
    for (const Wheel& wheel : type.fixture.wheels) {
        for (size_t s = 0; s < wheel.slots.size(); ++s) {
            const WheelSlot& slot = wheel.slots[s];
            const ImageResult picture = load_wheel_image(gdtf, slot);
            std::string problem;
            for (const Diagnostic& item : picture.diagnostics.items) problem += (problem.empty() ? "" : "; ") + item.message;
            std::printf("slot\t%s\t%s\t%zu\t%s\t%s\t%d\t%d\t%.3f\t%zu\t%s\n", label, field(wheel.name).c_str(), s + 1, field(slot.name).c_str(),
                        field(slot.media_file).c_str(), picture.image.width, picture.image.height,
                        picture.ok ? mean_transmission(gobo_transmission(picture.image)) : 1.0, slot.facets.size(), field(problem).c_str());
        }
    }
    return 0;
}

// clusters <mode> <beams> <drawn as> then "cluster <members> <radius in metres> <leader's part>" for each
// set of beams that is drawn as one (see BeamCluster).
int clusters_of(const Archive& gdtf, const char* label) {
    const GdtfResult result = load_gdtf(gdtf);
    if (!result.ok) {
        std::printf("error\t%s cannot be read\n", field(label).c_str());
        return 1;
    }
    std::printf("type\t%s\n", field(label).c_str());
    for (size_t m = 0; m < result.fixture.modes.size(); ++m) {
        const FixtureModel model(result.fixture, m);
        if (!model.valid()) continue;
        size_t lit = 0;
        for (const GeometryInstance& part : model.geometry()) lit += part.geometry->beam && part.geometry->beam->beam_type != BeamType::None ? 1 : 0;
        size_t clustered = 0;
        for (const BeamCluster& cluster : model.clusters()) clustered += cluster.members.size();
        std::printf("clusters\t%s\t%zu\t%zu\n", field(result.fixture.modes[m].name).c_str(), lit, lit - clustered + model.clusters().size());
        FixtureState state;
        std::vector<ChannelValue> values;
        model.evaluator().evaluate(nullptr, 0, values);
        model.resolve(values, state);
        for (const BeamCluster& cluster : model.clusters()) {
            const int part = cluster.leader >= 0 && static_cast<size_t>(cluster.leader) < state.beams.size() ? state.beams[static_cast<size_t>(cluster.leader)].geometry : -1;
            std::printf("cluster\t%zu\t%.3f\t%s\n", cluster.members.size(), cluster.radius, part >= 0 ? field(model.geometry()[static_cast<size_t>(part)].name).c_str() : "");
        }
    }
    return 0;
}

int each_fixture_type(const std::string& path, int (*what)(const Archive&, const char*)) {
    Archive archive;
    std::string error;
    if (!archive.open_file(path, error)) {
        std::printf("error\t%s\n", field(error).c_str());
        return 1;
    }
    if (!archive.contains("GeneralSceneDescription.xml")) return what(archive, "");
    int status = 0;
    for (const std::string& entry : archive.entries()) {    // every fixture type packed in the rig
        if (!ends_with(entry, ".gdtf")) continue;
        std::vector<uint8_t> bytes;
        Archive inner;
        if (archive.read(entry, bytes) && inner.open_memory(std::move(bytes), error)) status |= what(inner, entry.c_str());
    }
    return status;
}

// A show's fixture types as its rig file has them, and what stands in for each:
//   type <spec> <fixtures> <says little: 0|1> <stand-in file> <"" | ok | missing>
//   mode <spec> <mode> <fixtures> <slots> <channels> <silent> <stand-in mode> <its slots> <dimmer: 1 where the file has it, 0 elsewhere, - unknown>
//   moves <spec> <pan inverted 0|1> <tilt inverted> <pan offset> <tilt offset> <has pan 0|1> <has tilt>
//   removed <spec> <fixtures>     how many of the type's fixtures were taken out of the show (only where some were)
// A type all of whose fixtures were taken out is still listed, with 0 fixtures.
// A mode is listed if fixtures are in it. Slots are those of break 1. "moves" says how the type's
// pan and tilt are read (see TypeSwap), and whether the file in force has such channels in a mode in use.
int types_of(const std::string& path, const std::string& library_dir) {
    Archive archive;
    std::string error;
    if (!archive.open_file(path, error)) {
        std::printf("error\t%s\n", field(error).c_str());
        return 1;
    }
    const MvrResult result = load_mvr(archive);
    if (!result.ok) {
        std::printf("error\tits scene description cannot be read\n");
        return 1;
    }
    const ShowEdits edits = read_edits(path);
    const TypeSwaps swaps = read_swaps(path);
    std::vector<std::string> order;
    std::map<std::string, std::map<std::string, size_t>> used;    // spec -> mode -> fixtures
    std::map<std::string, size_t> taken_out;                       // spec -> fixtures taken out of the show
    for (const MvrFixture& fixture : result.scene.fixtures) {
        if (fixture.gdtf_spec.empty()) continue;
        if (!used.count(fixture.gdtf_spec) && !taken_out.count(fixture.gdtf_spec)) order.push_back(fixture.gdtf_spec);
        if (edits.is_removed(fixture.uuid)) {
            ++taken_out[fixture.gdtf_spec];
            used[fixture.gdtf_spec];    // listed, if with no mode in use
            continue;
        }
        ++used[fixture.gdtf_spec][fixture.gdtf_mode];
    }
    std::sort(order.begin(), order.end());
    for (const std::string& spec : order) {
        GdtfResult type;
        std::vector<uint8_t> bytes;
        Archive inner;
        const std::string entry = find_gdtf_entry(archive, spec);
        if (!entry.empty() && archive.read(entry, bytes) && inner.open_memory(std::move(bytes), error)) type = load_gdtf(inner);
        const TypeSwap* swap = swaps.stand_in(spec);
        const TypeSwap* reading = swaps.find(spec);
        GdtfResult stand_in;
        if (swap) stand_in = library_type(library_dir, swap->file);
        size_t fixtures = 0;
        bool little = false;
        bool has_pan = false, has_tilt = false;
        std::string lines;
        for (const auto& [mode, count] : used[spec]) {
            fixtures += count;
            size_t slots = 0, their_slots = 0;
            ModeSilence silence;
            const int index = type.ok ? type.fixture.find_mode(mode) : -1;
            if (index >= 0) {
                slots = ModeEvaluator(type.fixture, static_cast<size_t>(index)).footprint(1);
                silence = mode_silence(type.fixture, static_cast<size_t>(index));
                little = little || silence.says_little();
            }
            std::string theirs;
            int agree = -1;
            if ((!swap || !stand_in.ok) && index >= 0) {
                has_pan = has_pan || mode_has(type.fixture, static_cast<size_t>(index), "Pan");
                has_tilt = has_tilt || mode_has(type.fixture, static_cast<size_t>(index), "Tilt");
            }
            if (swap) {
                const std::string* counterpart = swap->mode_for(mode);
                theirs = counterpart ? *counterpart : mode;
                const int their_index = stand_in.ok ? stand_in.fixture.find_mode(theirs) : -1;
                if (their_index >= 0) {
                    has_pan = has_pan || mode_has(stand_in.fixture, static_cast<size_t>(their_index), "Pan");
                    has_tilt = has_tilt || mode_has(stand_in.fixture, static_cast<size_t>(their_index), "Tilt");
                    their_slots = ModeEvaluator(stand_in.fixture, static_cast<size_t>(their_index)).footprint(1);
                    if (index >= 0) agree = dimmers_agree(type.fixture, static_cast<size_t>(index), stand_in.fixture, static_cast<size_t>(their_index));
                } else if (stand_in.ok) {
                    theirs.clear();
                }
            }
            lines += "mode\t" + field(spec) + "\t" + field(mode) + "\t" + std::to_string(count) + "\t" + std::to_string(slots) + "\t" +
                     std::to_string(silence.channels) + "\t" + std::to_string(silence.silent) + "\t" + field(theirs) + "\t" + std::to_string(their_slots) + "\t" +
                     (agree < 0 ? "-" : agree ? "1" : "0") + "\n";
        }
        std::printf("type\t%s\t%zu\t%d\t%s\t%s\n", field(spec).c_str(), fixtures, little ? 1 : 0, swap ? field(swap->file).c_str() : "",
                    !swap ? "" : stand_in.ok ? "ok" : "missing");
        std::fputs(lines.c_str(), stdout);
        if (taken_out.count(spec)) std::printf("removed\t%s\t%zu\n", field(spec).c_str(), taken_out[spec]);
        std::printf("moves\t%s\t%d\t%d\t%.6g\t%.6g\t%d\t%d\n", field(spec).c_str(), reading && reading->invert_pan ? 1 : 0, reading && reading->invert_tilt ? 1 : 0,
                    reading ? reading->pan_offset : 0.0, reading ? reading->tilt_offset : 0.0, has_pan ? 1 : 0, has_tilt ? 1 : 0);
        std::printf("curve\t%s\t%s\n", field(spec).c_str(), dimmer_curve_name(reading ? reading->dimmer : DimmerCurve::Linear));
    }
    return 0;
}

// Has a file of the fixture library stand in for a type of the rig file. Every mode fixtures are
// in must have a mode of the stand-in that is exactly as long: channels that do not line up would
// show a wrong picture, which is worse than a plain one.
//   standin <spec> <file>, then "map <the file's mode> <the stand-in's> <slots>" for each mode in use
int stand_in(const std::string& path, const std::string& spec, const std::string& library_dir, const std::string& file) {
    Archive archive;
    std::string error;
    if (!archive.open_file(path, error)) {
        std::printf("error\t%s\n", field(error).c_str());
        return 1;
    }
    const MvrResult result = load_mvr(archive);
    if (!result.ok) {
        std::printf("error\tits scene description cannot be read\n");
        return 1;
    }
    if (file.empty() || file.find('/') != std::string::npos || file.find('\\') != std::string::npos) {
        std::printf("error\tname a file of the fixture library, without a folder\n");
        return 1;
    }
    std::vector<uint8_t> bytes;
    Archive inner;
    const std::string entry = find_gdtf_entry(archive, spec);
    GdtfResult original;
    if (!entry.empty() && archive.read(entry, bytes) && inner.open_memory(std::move(bytes), error)) original = load_gdtf(inner);
    if (!original.ok) {
        std::printf("error\tthe show has no fixture type %s\n", field(spec).c_str());
        return 1;
    }
    const GdtfResult replacement = library_type(library_dir, file);
    if (!replacement.ok || replacement.fixture.modes.empty()) {
        std::printf("error\t%s is not a fixture file in the library that can be read\n", field(file).c_str());
        return 1;
    }
    TypeSwap swap;
    swap.type = spec;
    swap.file = file;
    swap.modes = match_modes(original.fixture, replacement.fixture);
    // Every mode in use has to have its counterpart.
    const ShowEdits edits = read_edits(path);
    std::map<std::string, size_t> used;
    for (const MvrFixture& fixture : result.scene.fixtures) {
        if (same_type(fixture.gdtf_spec, spec) && !edits.is_removed(fixture.uuid)) ++used[fixture.gdtf_mode];
    }
    for (const auto& [mode, count] : used) {
        if (swap.mode_for(mode)) continue;
        const int index = original.fixture.find_mode(mode);
        const size_t slots = index >= 0 ? ModeEvaluator(original.fixture, static_cast<size_t>(index)).footprint(1) : 0;
        std::string theirs;
        for (size_t m = 0; m < replacement.fixture.modes.size(); ++m) {
            theirs += (theirs.empty() ? "" : ", ") + replacement.fixture.modes[m].name + " (" + std::to_string(ModeEvaluator(replacement.fixture, m).footprint(1)) + ")";
        }
        std::printf("error\t%zu fixtures are in the mode %s, which takes %zu slots; %s has no mode of that length. Its modes: %s\n", count, field(mode).c_str(), slots,
                    field(file).c_str(), field(theirs).c_str());
        return 1;
    }
    TypeSwaps swaps = read_swaps(path);
    swaps.set_stand_in(swap.type, swap.file, swap.modes);    // which way its pan and tilt are read stays as it was set
    if (!write_swaps(path, swaps)) {
        std::printf("error\tthe stand-ins could not be written beside the show\n");
        return 1;
    }
    std::printf("standin\t%s\t%s\n", field(spec).c_str(), field(file).c_str());
    for (const auto& [mode, count] : used) {
        const std::string* counterpart = swap.mode_for(mode);
        const int index = original.fixture.find_mode(mode);
        std::printf("map\t%s\t%s\t%zu\n", field(mode).c_str(), field(*counterpart).c_str(),
                    index >= 0 ? ModeEvaluator(original.fixture, static_cast<size_t>(index)).footprint(1) : size_t(0));
    }
    return 0;
}

// Takes a type's stand-in away again: its fixtures are read from the rig file's own type once more.
int no_stand_in(const std::string& path, const std::string& spec) {
    TypeSwaps swaps = read_swaps(path);
    if (!swaps.clear_stand_in(spec)) {
        std::printf("error\t%s has no stand-in\n", field(spec).c_str());
        return 1;
    }
    if (!write_swaps(path, swaps)) {
        std::printf("error\tthe stand-ins could not be written beside the show\n");
        return 1;
    }
    std::printf("plain\t%s\n", field(spec).c_str());
    return 0;
}

// How a type's pan and tilt are to be read: mirrored or not, and turned by how much.
//   moves <spec> <pan inverted> <tilt inverted> <pan offset> <tilt offset>
int correct(const std::string& path, const std::string& spec, const std::string& pan, const std::string& tilt, const std::string& pan_offset,
            const std::string& tilt_offset) {
    const MvrResult result = load_mvr_file(path);
    if (!result.ok) {
        std::printf("error\tits scene description cannot be read\n");
        return 1;
    }
    bool known = false;
    for (const MvrFixture& fixture : result.scene.fixtures) known = known || same_type(fixture.gdtf_spec, spec);
    if (!known) {
        std::printf("error\tthe show has no fixture type %s\n", field(spec).c_str());
        return 1;
    }
    double offsets[2] = {0.0, 0.0};
    const std::string* written[2] = {&pan_offset, &tilt_offset};
    for (int k = 0; k < 2; ++k) {
        char* end = nullptr;
        offsets[k] = std::strtod(written[k]->c_str(), &end);
        if (written[k]->empty() || *end != '\0' || !(offsets[k] >= -360.0 && offsets[k] <= 360.0)) {
            std::printf("error\tan offset is a number of degrees between -360 and 360\n");
            return 1;
        }
    }
    if ((pan != "0" && pan != "1") || (tilt != "0" && tilt != "1")) {
        std::printf("error\tsay 1 for an axis that runs the other way, 0 for one that does not\n");
        return 1;
    }
    TypeSwaps swaps = read_swaps(path);
    swaps.correct(spec, pan == "1", tilt == "1", offsets[0], offsets[1]);
    if (!write_swaps(path, swaps)) {
        std::printf("error\tit could not be written beside the show\n");
        return 1;
    }
    const TypeSwap* now = swaps.find(spec);
    std::printf("moves\t%s\t%d\t%d\t%.6g\t%.6g\n", field(spec).c_str(), now && now->invert_pan ? 1 : 0, now && now->invert_tilt ? 1 : 0, now ? now->pan_offset : 0.0,
                now ? now->tilt_offset : 0.0);
    return 0;
}

// How a type's dimmer level becomes light in this show: "linear" (as the file has it), "square",
// "inverse-square" or "s-curve". Kept in the types file beside the show, as the pan and tilt corrections are.
int set_curve(const std::string& path, const std::string& spec, const std::string& name) {
    const MvrResult result = load_mvr_file(path);
    if (!result.ok) {
        std::printf("error\tits scene description cannot be read\n");
        return 1;
    }
    bool known = false;
    for (const MvrFixture& fixture : result.scene.fixtures) known = known || same_type(fixture.gdtf_spec, spec);
    if (!known) {
        std::printf("error\tthe show has no fixture type %s\n", field(spec).c_str());
        return 1;
    }
    DimmerCurve curve = DimmerCurve::Linear;
    if (!parse_dimmer_curve(name, curve)) {
        std::printf("error\ta dimmer curve is linear, square, inverse-square or s-curve\n");
        return 1;
    }
    TypeSwaps swaps = read_swaps(path);
    swaps.set_dimmer_curve(spec, curve);
    if (!write_swaps(path, swaps)) {
        std::printf("error\tit could not be written beside the show\n");
        return 1;
    }
    std::printf("curve\t%s\t%s\n", field(spec).c_str(), dimmer_curve_name(curve));
    return 0;
}

// Changes fixtures of a show: where they stand, how they are turned, their patch, mode, name,
// number and whether their pan and tilt are mirrored. Everything goes into the show's edits; the
// rig file is not touched. `uuids` are separated by commas and taken in that order.
//
//   x= y= z=            where it stands, millimetres, the rig file's coordinates
//   dx= dy= dz=         or: moved by that much
//   stepx= stepy= stepz=  each fixture after the first that much further on than the one before (a row)
//   rx= ry= rz=         its turn about the world's X, Y and Z axes, degrees (see turn_angles)
//   spin=               or: turned by that many degrees about its own vertical axis
//   aimx= aimy= aimz=   or: turned where it hangs, by the least turn, so that its beam at rest points
//                       at that point (millimetres). Not a fixture that pans or tilts: the console aims those
//   universe= address=  its patch (break 1); several fixtures follow one another, each as long as its mode;
//                       universe=0 unpatches
//   mode=               a mode of the fixture type in force
//   name=               what it is called; with number=<n> the fixtures are called "<name> <n>", "<name> <n+1>" ...
//   id=                 its number on the console: the first of consecutive numbers, or empty for none
//   invertpan= inverttilt=   0 or 1: the fixture's own mirroring, in place of the rig file's
//   type=               a file in the fixture library as this fixture's own type (with mode=, a mode
//                       of it; else the one as long as the mode it has, or the first); empty: the rig
//                       file's type again. Not together with a patch: the patch goes by the mode's length
//   reset=              place, patch, mode, name, invert, type or all: back to what the rig file says
//
// Answers "set <uuid>" for every fixture, or one "error" line and nothing changed. Aiming adds
// "aimed <uuid>" for each fixture turned, and "notaimed <uuid> <why>" for each that was not: it
// moves (pans or tilts), unplaced (has no place to aim from), nobeam, unreadable (its type).
int set_fixtures(const std::string& path, const std::string& library_dir, const std::string& uuids, const std::vector<std::string>& settings) {
    Archive archive;
    std::string error;
    if (!archive.open_file(path, error)) {
        std::printf("error\t%s\n", field(error).c_str());
        return 1;
    }
    const MvrResult result = load_mvr(archive);
    if (!result.ok) {
        std::printf("error\tits scene description cannot be read\n");
        return 1;
    }
    std::map<std::string, std::string> wanted;
    for (const std::string& setting : settings) {
        const size_t equals = setting.find('=');
        if (equals == std::string::npos || equals == 0) {
            std::printf("error\t%s is not of the form key=value\n", field(setting).c_str());
            return 1;
        }
        wanted[setting.substr(0, equals)] = setting.substr(equals + 1);
    }
    static const char* const known[] = {"x", "y", "z", "dx", "dy", "dz", "stepx", "stepy", "stepz", "rx", "ry", "rz", "spin", "aimx", "aimy", "aimz", "universe",
                                        "address", "mode", "name", "number", "id", "invertpan", "inverttilt", "type", "reset"};
    for (const auto& [key, value] : wanted) {
        bool found = false;
        for (const char* name : known) found = found || key == name;
        if (!found) {
            std::printf("error\t%s cannot be set\n", field(key).c_str());
            return 1;
        }
    }
    auto has = [&wanted](const char* key) { return wanted.count(key) > 0; };
    bool bad_number = false;
    auto real = [&wanted, &bad_number](const char* key, double low, double high) {
        const std::string& written = wanted[key];
        char* end = nullptr;
        const double value = std::strtod(written.c_str(), &end);
        if (written.empty() || *end != '\0' || !(value >= low && value <= high)) bad_number = true;
        return value;
    };
    auto whole = [&wanted, &bad_number](const char* key, long long low, long long high) {
        long long value = 0;
        if (!whole_number(wanted[key], value) || value < low || value > high) bad_number = true;
        return value;
    };
    auto flag = [&wanted, &bad_number](const char* key) {
        if (wanted[key] != "0" && wanted[key] != "1") bad_number = true;
        return wanted[key] == "1";
    };

    ShowEdits edits = read_edits(path);
    const TypeSwaps swaps = read_swaps(path);
    std::vector<RigFixture> rig = merge_rig(result.scene, edits, swaps);
    drop_missing_stand_ins(rig, library_dir);

    // The fixtures named, in the order named.
    std::vector<const RigFixture*> chosen;
    for (size_t start = 0; start <= uuids.size();) {
        size_t end = uuids.find(',', start);
        if (end == std::string::npos) end = uuids.size();
        const std::string uuid = uuids.substr(start, end - start);
        start = end + 1;
        if (uuid.empty()) continue;
        const RigFixture* found = nullptr;
        for (const RigFixture& fixture : rig) {
            if (same_id(fixture.uuid, uuid)) found = &fixture;
        }
        if (!found || found->removed) {
            std::printf("error\tthe show has no fixture %s\n", field(uuid).c_str());
            return 1;
        }
        chosen.push_back(found);
    }
    if (chosen.empty()) {
        std::printf("error\tsay which fixtures\n");
        return 1;
    }

    // Numbers first, so that nothing is half done when one of them is wrong.
    const std::string reset = has("reset") ? wanted["reset"] : "";
    if (!reset.empty() && reset != "place" && reset != "patch" && reset != "mode" && reset != "name" && reset != "invert" && reset != "type" && reset != "all") {
        std::printf("error\treset takes place, patch, mode, name, invert, type or all\n");
        return 1;
    }
    const bool retyping = has("type") || reset == "type" || reset == "all";
    if (retyping && (has("universe") || has("address"))) {
        std::printf("error\tchange the type first, then the patch: the patch goes by the mode's length\n");
        return 1;
    }
    const double limit = 5e6;    // five kilometres either way: further off than that is a slip of the hand
    double at[3] = {0, 0, 0}, by[3] = {0, 0, 0}, step[3] = {0, 0, 0}, turn[3] = {0, 0, 0};
    const char* at_keys[3] = {"x", "y", "z"};
    const char* by_keys[3] = {"dx", "dy", "dz"};
    const char* step_keys[3] = {"stepx", "stepy", "stepz"};
    const char* turn_keys[3] = {"rx", "ry", "rz"};
    for (int k = 0; k < 3; ++k) {
        if (has(at_keys[k])) at[k] = real(at_keys[k], -limit, limit);
        if (has(by_keys[k])) by[k] = real(by_keys[k], -limit, limit);
        if (has(step_keys[k])) step[k] = real(step_keys[k], -limit, limit);
        if (has(turn_keys[k])) turn[k] = real(turn_keys[k], -360.0, 360.0);
    }
    const double spin = has("spin") ? real("spin", -360.0, 360.0) : 0.0;
    // Aimed at a point: turned where it hangs so that its beam at rest points there.
    const bool aiming = has("aimx") || has("aimy") || has("aimz");
    if (aiming && !(has("aimx") && has("aimy") && has("aimz"))) {
        std::printf("error\taim at a point takes aimx, aimy and aimz together\n");
        return 1;
    }
    double aim[3] = {0, 0, 0};
    if (aiming) {
        aim[0] = real("aimx", -limit, limit);
        aim[1] = real("aimy", -limit, limit);
        aim[2] = real("aimz", -limit, limit);
    }
    std::vector<std::string> aimed, not_aimed;
    if (has("universe") != has("address") && !(has("universe") && wanted["universe"] == "0")) {
        std::printf("error\ta patch is a universe and an address; universe=0 unpatches\n");
        return 1;
    }
    long long universe = has("universe") ? whole("universe", 0, 32767) : 0;
    long long address = has("address") ? whole("address", 1, 512) : 1;
    const long long first_number = has("number") ? whole("number", 0, 1000000) : -1;
    long long first_id = -1;
    if (has("id") && !wanted["id"].empty()) first_id = whole("id", 0, 100000000);
    const bool invert_pan = has("invertpan") ? flag("invertpan") : false;
    const bool invert_tilt = has("inverttilt") ? flag("inverttilt") : false;
    if (bad_number) {
        std::printf("error\tone of the numbers given is not a number, or out of range\n");
        return 1;
    }
    if (has("name") && wanted["name"].size() > 200) {
        std::printf("error\tthat name is too long\n");
        return 1;
    }

    // The type each fixture is read from, for its modes and their lengths.
    std::map<std::string, GdtfResult> types;
    auto type_of = [&](const RigFixture& fixture) -> const GdtfResult& {
        const std::string entry = fixture.from_library ? fixture.gdtf : find_gdtf_entry(archive, fixture.gdtf);
        const std::string key = (fixture.from_library ? "library|" : "") + entry;
        auto found = types.find(key);
        if (found == types.end()) {
            GdtfResult loaded;
            if (fixture.from_library) {
                loaded = library_type(library_dir, entry);
            } else if (!entry.empty()) {
                std::vector<uint8_t> bytes;
                Archive inner;
                std::string unused;
                if (archive.read(entry, bytes) && inner.open_memory(std::move(bytes), unused)) loaded = load_gdtf(inner);
            }
            found = types.emplace(key, std::move(loaded)).first;
        }
        return found->second;
    };
    // The mode a fixture will be in once this is done, and how many slots that takes; 0 if its type cannot be read.
    auto slots_of = [&](const RigFixture& fixture, const std::string& mode) -> size_t {
        const GdtfResult& type = type_of(fixture);
        if (!type.ok || type.fixture.modes.empty()) return 0;
        int index = mode.empty() ? 0 : type.fixture.find_mode(mode);
        if (index < 0) index = fixture.source ? std::max(type.fixture.find_mode(fixture.file_mode), 0) : 0;
        return ModeEvaluator(type.fixture, static_cast<size_t>(index)).footprint(1);
    };
    // A type of its own from the fixture library, and the mode each fixture will have in it.
    const std::string new_file = has("type") ? wanted["type"] : std::string();
    GdtfResult new_type;
    std::vector<std::string> new_modes(chosen.size());
    if (!new_file.empty()) {
        if (new_file.find_first_of("/\\") != std::string::npos || new_file == "." || new_file == "..") {
            std::printf("error\ta type is the name of one file in the fixture library\n");
            return 1;
        }
        new_type = library_type(library_dir, new_file);
        if (!new_type.ok || new_type.fixture.modes.empty()) {
            std::printf("error\tthe fixture library has no readable fixture type %s\n", field(new_file).c_str());
            return 1;
        }
        if (has("mode") && new_type.fixture.find_mode(wanted["mode"]) < 0) {
            std::printf("error\t%s has no mode called %s\n", field(new_file).c_str(), field(wanted["mode"]).c_str());
            return 1;
        }
        for (size_t n = 0; n < chosen.size(); ++n) {
            if (has("mode")) {
                new_modes[n] = wanted["mode"];
                continue;
            }
            // As long as the mode it has, so that its patch still fits; the same name first. Else the first mode.
            const size_t length = slots_of(*chosen[n], chosen[n]->mode);
            int best = -1;
            for (size_t m = 0; m < new_type.fixture.modes.size(); ++m) {
                if (ModeEvaluator(new_type.fixture, m).footprint(1) != length) continue;
                if (best < 0 || new_type.fixture.modes[m].name == chosen[n]->mode) best = static_cast<int>(m);
            }
            new_modes[n] = new_type.fixture.modes[static_cast<size_t>(best < 0 ? 0 : best)].name;
        }
    }
    if (has("mode") && !has("type")) {
        for (const RigFixture* fixture : chosen) {
            const GdtfResult& type = type_of(*fixture);
            if (!type.ok || type.fixture.find_mode(wanted["mode"]) < 0) {
                std::printf("error\t%s has no mode called %s\n", field(fixture->name).c_str(), field(wanted["mode"]).c_str());
                return 1;
            }
        }
    }

    for (size_t n = 0; n < chosen.size(); ++n) {
        const RigFixture& now = *chosen[n];
        AddedFixture* added = now.added ? edits.added_of(now.uuid) : nullptr;
        if (now.added && !added) continue;
        FixtureEdit scratch;
        FixtureEdit& edit = added ? scratch : edits.edit_of(now.uuid);
        const MvrFixture* file = now.source;

        // --- place
        Transform world = now.world;
        bool place = false;
        if (reset == "place" || reset == "all") {
            if (file) world = file->world;
            edit.world.reset();
        }
        for (int k = 0; k < 3; ++k) {
            if (has(at_keys[k])) {
                world.o[k] = at[k] + step[k] * static_cast<double>(n);
                place = true;
            } else if (step[k] != 0.0 && n > 0) {
                world.o[k] = chosen[0]->world.o[k] + (has(by_keys[k]) ? by[k] : 0.0) + step[k] * static_cast<double>(n);
                place = true;
            }
            if (has(by_keys[k]) && !(step[k] != 0.0 && n > 0 && !has(at_keys[k]))) {
                world.o[k] += by[k];
                place = true;
            }
        }
        if (has("rx") || has("ry") || has("rz")) {
            double angles[3];
            turn_angles(world, angles);
            for (int k = 0; k < 3; ++k) {
                if (has(turn_keys[k])) angles[k] = turn[k];
            }
            set_turn_angles(world, angles);
            place = true;
        }
        if (spin != 0.0) {
            // About its own vertical axis: its X and Y axes turn in their own plane.
            const double c = std::cos(spin * 3.14159265358979323846 / 180.0), s = std::sin(spin * 3.14159265358979323846 / 180.0);
            for (int k = 0; k < 3; ++k) {
                const double u = world.u[k], v = world.v[k];
                world.u[k] = c * u + s * v;
                world.v[k] = -s * u + c * v;
            }
            place = true;
        }
        if (aiming) {
            // The type and mode it will have once this is done.
            const GdtfResult& type = new_file.empty() ? type_of(now) : new_type;
            const std::string aim_mode = !new_file.empty() ? new_modes[n] : has("mode") ? wanted["mode"] : now.mode;
            std::string why;
            if (!now.placed && !place) {
                why = "unplaced";
            } else if (!type.ok || type.fixture.modes.empty()) {
                why = "unreadable";
            } else {
                const size_t index = static_cast<size_t>(std::max(type.fixture.find_mode(aim_mode), 0));
                const FixtureModel model(type.fixture, index);
                FixtureState state;
                std::vector<ChannelValue> values;
                model.evaluator().evaluate(nullptr, 0, values);
                model.resolve(values, state);
                for (const AxisState& axis : state.axes) {
                    if (axis.motion == Motion::Pan || axis.motion == Motion::Tilt) why = "moves";    // the console aims it
                }
                if (why.empty() && !aim_at(world, rest_beams(type.fixture, index), aim)) why = "nobeam";
            }
            if (why.empty()) {
                aimed.push_back(now.uuid);
                place = true;
            } else {
                not_aimed.push_back(now.uuid + "\t" + why);
            }
        }
        if (place) {
            bool same = file && file->placed;
            for (int k = 0; same && k < 3; ++k) {
                same = std::fabs(world.u[k] - file->world.u[k]) < 1e-9 && std::fabs(world.v[k] - file->world.v[k]) < 1e-9 &&
                       std::fabs(world.w[k] - file->world.w[k]) < 1e-9 && std::fabs(world.o[k] - file->world.o[k]) < 1e-6;
            }
            if (added) {
                added->world = world;
            } else if (same) {
                edit.world.reset();    // back where the rig file has it: nothing to keep
            } else {
                edit.world = world;
            }
        }

        // --- type, then the mode, then the patch, which goes by the mode's length
        std::string mode = now.mode;
        if (!added && (reset == "type" || reset == "all" || (has("type") && new_file.empty())) && edit.gdtf) {
            edit.gdtf.reset();
            edit.mode.reset();    // a mode of the library's type means nothing to the rig file's
        }
        if (!new_file.empty()) {
            if (added) {
                added->gdtf = new_file;
                added->mode = new_modes[n];
            } else {
                edit.gdtf = new_file;
                edit.mode = new_modes[n];
            }
        }
        if (reset == "mode" || reset == "all") {
            if (!edit.gdtf) edit.mode.reset();
            if (!added && !edit.gdtf) mode = now.file_mode;
        }
        if (has("mode") && !has("type")) {
            mode = wanted["mode"];
            if (added) {
                added->mode = mode;
            } else if (mode == now.file_mode && !now.retyped) {
                edit.mode.reset();
            } else {
                edit.mode = mode;    // a retyped fixture keeps its mode written, whatever it is
            }
        }
        if (reset == "patch" || reset == "all") {
            edit.universe.reset();
            edit.address.reset();
        }
        if (has("universe")) {
            int u = static_cast<int>(universe), a = static_cast<int>(address);
            if (u > 0) {
                const size_t slots = std::max<size_t>(slots_of(now, mode), 1);
                if (a + static_cast<int>(slots) - 1 > 512 && a > 1) {    // it would run off the end: the next universe
                    a = 1;
                    u = u < 32767 ? u + 1 : 0;
                }
                universe = u;
                address = a + static_cast<long long>(slots);
                if (address > 512) {
                    address = 1;
                    universe = u < 32767 ? u + 1 : 0;
                }
            } else {
                a = 1;
            }
            const int file_universe = file && now.first_patch ? now.first_patch->universe() : 0;
            const int file_address = file && now.first_patch && now.first_patch->universe() > 0 ? now.first_patch->address() : 1;
            if (added) {
                added->universe = u;
                added->address = a;
            } else if (u == file_universe && a == file_address) {
                edit.universe.reset();
                edit.address.reset();
            } else {
                edit.universe = u;
                edit.address = a;
            }
        }

        // --- what it is called
        if (reset == "name" || reset == "all") {
            edit.name.reset();
            edit.fixture_id.reset();
        }
        if (has("name")) {
            const std::string name = first_number >= 0 ? wanted["name"] + " " + std::to_string(first_number + static_cast<long long>(n)) : wanted["name"];
            if (added) {
                added->name = name;
            } else if (file && name == file->name) {
                edit.name.reset();
            } else {
                edit.name = name;
            }
        }
        if (has("id")) {
            const std::string id = first_id >= 0 ? std::to_string(first_id + static_cast<long long>(n)) : std::string();
            if (added) {
                added->fixture_id = id;
            } else if (file && id == file->fixture_id) {
                edit.fixture_id.reset();
            } else {
                edit.fixture_id = id;
            }
        }

        // --- mirroring
        if (reset == "invert" || reset == "all") {
            edit.invert_pan.reset();
            edit.invert_tilt.reset();
        }
        if (has("invertpan")) {
            if (added) {
                added->invert_pan = invert_pan;
            } else if (file && invert_pan == file->dmx_invert_pan) {
                edit.invert_pan.reset();
            } else {
                edit.invert_pan = invert_pan;
            }
        }
        if (has("inverttilt")) {
            if (added) {
                added->invert_tilt = invert_tilt;
            } else if (file && invert_tilt == file->dmx_invert_tilt) {
                edit.invert_tilt.reset();
            } else {
                edit.invert_tilt = invert_tilt;
            }
        }
    }
    // Entries that came to nothing are not written.
    if (!write_edits(path, edits)) {
        std::printf("error\tthe show's changes could not be saved\n");
        return 1;
    }
    for (const RigFixture* fixture : chosen) std::printf("set\t%s\n", fixture->uuid.c_str());
    for (const std::string& uuid : aimed) std::printf("aimed\t%s\n", uuid.c_str());
    for (const std::string& line : not_aimed) std::printf("notaimed\t%s\n", line.c_str());    // uuid, then: moves, unplaced, nobeam, unreadable
    return 0;
}

// exported <fixtures changed>, then a "note" line for each change that fitted no fixture.
int export_show(const std::string& path, const std::string& out_path, const std::string& library_dir) {
    ShowEdits edits = read_edits(path);
    const TypeSwaps swaps = read_swaps(path);
    if (!swaps.empty()) {
        // A mode chosen while a stand-in was in force goes out as the mode of the file's own type it stands for.
        Archive archive;
        std::string unused;
        if (archive.open_file(path, unused)) {
            const MvrResult scene = load_mvr(archive);
            if (scene.ok) edits = edits_for_export(scene.scene, edits, swaps);
        }
    }
    int changed = 0;
    std::string error;
    const std::vector<PlacedPiece> pieces = read_pieces(path, library_dir);
    if (!export_mvr(path, edits, out_path, changed, error, library_dir, pieces)) {
        std::printf("error\t%s\n", field(error).c_str());
        return 1;
    }
    std::printf("exported\t%d\n", changed);
    std::printf("pieces\t%zu\n", pieces.size());
    return 0;
}

// The files of the fixture library a show uses, one line each: "library <file>". Every stand-in its
// types file names, and the type of every fixture added or given a type of its own; each once,
// whether or not it is in the library now (a show being packed says which are missing).
int uses(const std::string& path) {
    Archive archive;
    std::string error;
    if (!archive.open_file(path, error)) {
        std::printf("error\t%s\n", field(error).c_str());
        return 1;
    }
    const MvrResult result = load_mvr(archive);
    if (!result.ok) {
        std::printf("error\tits scene description cannot be read\n");
        return 1;
    }
    std::vector<std::string> files;
    auto note = [&files](const std::string& file) {
        if (!file.empty() && std::find(files.begin(), files.end(), file) == files.end()) files.push_back(file);
    };
    for (const TypeSwap& swap : read_swaps(path).swaps) note(swap.file);
    for (const RigFixture& fixture : merge_rig(result.scene, read_edits(path), read_swaps(path))) {
        if (fixture.from_library && !fixture.removed) note(fixture.gdtf);
    }
    for (const std::string& file : files) std::printf("library\t%s\n", field(file).c_str());
    return 0;
}

// Makes a movie of JPEG files, as the visualizer makes its videos: for trying the file on a
// player. Every picture must be as big as the first; the size is read from the first's header.
int make_movie(const std::string& out_path, const std::string& rate, const std::vector<std::string>& pictures) {
    long long fps = 0;
    if (!whole_number(rate, fps) || fps < 1 || fps > 240) {
        std::printf("error\ta frame rate is 1 to 240\n");
        return 1;
    }
    // A JPEG's size is in its start-of-frame marker.
    auto size_of = [](const std::vector<uint8_t>& jpeg, int& width, int& height) {
        for (size_t at = 2; at + 9 < jpeg.size();) {
            if (jpeg[at] != 0xFF) return false;
            const uint8_t marker = jpeg[at + 1];
            const size_t length = static_cast<size_t>(jpeg[at + 2] << 8 | jpeg[at + 3]);
            if (marker >= 0xC0 && marker <= 0xC3) {
                height = jpeg[at + 5] << 8 | jpeg[at + 6];
                width = jpeg[at + 7] << 8 | jpeg[at + 8];
                return true;
            }
            at += 2 + length;
        }
        return false;
    };
    MovieWriter movie;
    std::string error;
    for (const std::string& path : pictures) {
        std::ifstream file = read_stream(path);
        const std::vector<uint8_t> jpeg((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        int width = 0, height = 0;
        if (jpeg.size() < 4 || jpeg[0] != 0xFF || jpeg[1] != 0xD8 || !size_of(jpeg, width, height)) {
            std::printf("error\t%s is not a JPEG\n", field(path).c_str());
            return 1;
        }
        if (!movie.is_open() && !movie.open(out_path, width, height, static_cast<int>(fps), error)) {
            std::printf("error\t%s\n", field(error).c_str());
            return 1;
        }
        movie.add(jpeg.data(), jpeg.size());
    }
    if (!movie.is_open() || !movie.close(error)) {
        std::printf("error\t%s\n", field(movie.is_open() ? error : std::string("no pictures")).c_str());
        return 1;
    }
    std::printf("movie\t%zu\n", pictures.size());
    return 0;
}

// What a fixture does with some DMX: --state <file.gdtf> <mode name or number> [slot=level ...], slots of
// break 1 counted from 1, levels 0-255, every other slot at its default.
//   fog <0-1>   (a haze or fog machine only)
//   axis <part> <pan|tilt|turn|slide> <degrees or metres>
//   beam <part> <intensity 0-1> <strobe Hz> <r g b> <beam angle> <field angle> <gobo: wheel/slot or -> <gobo deg/s>
//        <prism: slot or -> <prism copies> <iris> <frost> <defocus> <colour wheel turning: wheel or ->
int state_of(const std::string& path, const std::string& mode_name, const std::vector<std::string>& levels) {
    Archive archive;
    std::string error;
    if (!archive.open_file(path, error)) {
        std::printf("error\t%s\n", field(error).c_str());
        return 1;
    }
    const GdtfResult type = load_gdtf(archive);
    if (!type.ok) {
        std::printf("error\tits description cannot be read\n");
        return 1;
    }
    size_t mode = type.fixture.modes.size();
    for (size_t m = 0; m < type.fixture.modes.size(); ++m) {
        if (type.fixture.modes[m].name == mode_name || std::to_string(m + 1) == mode_name) mode = m;
    }
    if (mode == type.fixture.modes.size()) {
        std::printf("error\tno mode %s\n", field(mode_name).c_str());
        return 1;
    }
    const FixtureModel model(type.fixture, mode);
    if (!model.valid()) {
        std::printf("error\tthe mode cannot be evaluated\n");
        return 1;
    }
    std::vector<uint8_t> frame = model.evaluator().default_frame();
    for (const std::string& level : levels) {
        const size_t equals = level.find('=');
        const int slot = equals == std::string::npos ? 0 : std::atoi(level.substr(0, equals).c_str());
        const int value = equals == std::string::npos ? -1 : std::atoi(level.substr(equals + 1).c_str());
        if (slot < 1 || slot > 512 || value < 0 || value > 255) {
            std::printf("error\tnot slot=level: %s\n", field(level).c_str());
            return 1;
        }
        if (frame.size() < static_cast<size_t>(slot)) frame.resize(static_cast<size_t>(slot), 0);
        frame[static_cast<size_t>(slot - 1)] = static_cast<uint8_t>(value);
    }
    std::vector<ChannelValue> values;
    model.evaluator().evaluate(frame.data(), frame.size(), values);
    FixtureState state;
    model.resolve(values, state);
    static const char* motions[] = {"pan", "tilt", "turn", "slide"};
    if (model.has_fog()) std::printf("fog\t%.4f\n", state.fog);    // a haze or fog machine: how hard it works, 0 to 1
    for (const AxisState& axis : state.axes) {
        std::printf("axis\t%s\t%s\t%.3f\n", field(model.geometry()[static_cast<size_t>(axis.geometry)].name).c_str(), motions[static_cast<int>(axis.motion)], axis.target);
    }
    for (const BeamState& beam : state.beams) {
        const GateLayer& gobo = beam.layers[0];
        const std::string gobo_name = gobo.wheel && gobo.slot >= 1 ? gobo.wheel->name + "/" + gobo.wheel->slots[static_cast<size_t>(gobo.slot - 1)].name : "-";
        const PrismState& prism = beam.prisms[0];
        std::printf("beam\t%s\t%.4f\t%.3f\t%.4f %.4f %.4f\t%.3f\t%.3f\t%s\t%.3f\t%s\t%zu\t%.3f\t%.3f\t%.3f\t%s\n",
                    field(model.geometry()[static_cast<size_t>(beam.geometry)].name).c_str(), beam.intensity, beam.strobe_hz, beam.color.r, beam.color.g,
                    beam.color.b, beam.beam_angle, beam.field_angle, field(gobo_name).c_str(), gobo.degrees_per_second,
                    prism.slot ? field(prism.slot->name).c_str() : "-", prism.slot ? prism_images(*prism.slot).size() : size_t(0), beam.iris, beam.frost,
                    beam.defocus, beam.wheel ? field(beam.wheel->name).c_str() : "-");
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if ((argc == 4 || argc == 5) && std::string(argv[1]) == "--export") return export_show(argv[2], argv[3], argc == 5 ? argv[4] : "");
    if (argc == 3 && std::string(argv[1]) == "--uses") return uses(argv[2]);
    if (argc >= 5 && std::string(argv[1]) == "--movie") return make_movie(argv[2], argv[3], std::vector<std::string>(argv + 4, argv + argc));
    if (argc == 5 && std::string(argv[1]) == "--panorama") {
        // prefix out.png width: the six views "<prefix>0.png" to "<prefix>5.png" (ahead, right, behind,
        // left, up, down) made into one 360-degree picture, width by width / 2.
        Image faces[6];
        for (int f = 0; f < 6; ++f) {
            const std::string face = std::string(argv[2]) + std::to_string(f) + ".png";
            std::ifstream file = read_stream(face);
            const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
            ImageResult read = parse_png(bytes.data(), bytes.size());
            if (!read.ok) {
                std::printf("error\t%s cannot be read\n", field(face).c_str());
                return 1;
            }
            faces[f] = std::move(read.image);
        }
        const Image round = equirect_from_faces(faces, std::atoi(argv[4]));
        const std::vector<uint8_t> png = encode_png(round);
        std::ofstream out = write_stream(std::string(argv[3]) + ".part");
        out.write(reinterpret_cast<const char*>(png.data()), static_cast<std::streamsize>(png.size()));
        out.close();
        if (png.empty() || !out || !rename_file(std::string(argv[3]) + ".part", argv[3])) {
            remove_file(std::string(argv[3]) + ".part");
            std::printf("error\t%s\n", round.empty() ? "the six views are not squares of one size, or the width is not 64 to 16384" : "the picture cannot be written");
            return 1;
        }
        std::printf("panorama\t%d\t%d\n", round.width, round.height);
        return 0;
    }
    if (argc == 3 && std::string(argv[1]) == "--psn") {
        // One datagram from a file: "packet", its kind, frame and system; then "tracker", its id
        // and name, and each field the packet gave, by name, with its numbers.
        std::ifstream file = read_stream(argv[2]);
        const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        const PsnPacket packet = parse_psn(bytes.data(), bytes.size());
        if (packet.kind == PsnPacket::Kind::None) {
            std::printf("error\tnot a PosiStageNet packet\n");
            return 1;
        }
        std::printf("packet\t%s\t%u\t%s\n", packet.kind == PsnPacket::Kind::Data ? "data" : "info", static_cast<unsigned>(packet.frame),
                    field(packet.system_name).c_str());
        for (const PsnTracker& tracker : packet.trackers) {
            std::printf("tracker\t%u\t%s", static_cast<unsigned>(tracker.id), field(tracker.name).c_str());
            const struct { bool has; const char* name; const float* value; } threes[] = {
                {tracker.has_position, "position", tracker.position}, {tracker.has_speed, "speed", tracker.speed},
                {tracker.has_orientation, "orientation", tracker.orientation}, {tracker.has_acceleration, "acceleration", tracker.acceleration},
                {tracker.has_target, "target", tracker.target}};
            for (const auto& three : threes) {
                if (three.has) std::printf("\t%s %g %g %g", three.name, static_cast<double>(three.value[0]), static_cast<double>(three.value[1]), static_cast<double>(three.value[2]));
            }
            if (tracker.has_validity) std::printf("\tvalidity %g", static_cast<double>(tracker.validity));
            if (tracker.has_timestamp) std::printf("\ttimestamp %llu", static_cast<unsigned long long>(tracker.timestamp));
            std::printf("\n");
        }
        return 0;
    }
    if (argc == 6 && std::string(argv[1]) == "--movie-sound") {
        // in.mov sound.wav start out.mov: the movie again, with the sound from `start` seconds into it.
        std::string error;
        char* end = nullptr;
        const double start = std::strtod(argv[4], &end);
        if (!end || *end != '\0' || !std::isfinite(start)) {
            std::printf("error\tthe start is a number of seconds\n");
            return 1;
        }
        if (!movie_with_sound(argv[2], argv[3], start, argv[5], error)) {
            std::printf("error\t%s\n", field(error).c_str());
            return 1;
        }
        std::printf("movie\t%s\n", field(argv[5]).c_str());
        return 0;
    }
    if (argc == 10 && std::string(argv[1]) == "--add") return add(argv[2], argv[3], argv[4], argv[5], argv[6], argv[7], argv[8], argv[9]);
    if (argc == 4 && std::string(argv[1]) == "--remove") return remove_or_restore(argv[2], argv[3], false);
    if (argc == 4 && std::string(argv[1]) == "--restore") return remove_or_restore(argv[2], argv[3], true);
    if (argc == 4 && std::string(argv[1]) == "--remove-type") return remove_or_restore_type(argv[2], argv[3], false);
    if (argc == 4 && std::string(argv[1]) == "--restore-type") return remove_or_restore_type(argv[2], argv[3], true);
    if (argc == 4 && std::string(argv[1]) == "--patch") return patch(argv[2], argv[3]);
    if ((argc == 3 || argc == 4) && std::string(argv[1]) == "--plot") return plot(argv[2], argc == 4 ? argv[3] : "");
    if (argc == 3 && std::string(argv[1]) == "--models") return each_fixture_type(argv[2], models_of);
    if (argc == 3 && std::string(argv[1]) == "--wheels") return each_fixture_type(argv[2], wheels_of);
    if (argc == 3 && std::string(argv[1]) == "--clusters") return each_fixture_type(argv[2], clusters_of);
    if (argc == 3 && std::string(argv[1]) == "--summary") return summarize(argv[2]);
    if (argc == 3 && std::string(argv[1]) == "--screens") return screens(argv[2]);
    if ((argc == 3 || argc == 4) && std::string(argv[1]) == "--walls") return walls(argv[2], argc == 4 ? argv[3] : "");
    if ((argc == 3 || argc == 4) && std::string(argv[1]) == "--types") return types_of(argv[2], argc == 4 ? argv[3] : "");
    if (argc == 6 && std::string(argv[1]) == "--stand-in") return stand_in(argv[2], argv[3], argv[4], argv[5]);
    if (argc == 4 && std::string(argv[1]) == "--no-stand-in") return no_stand_in(argv[2], argv[3]);
    if (argc == 8 && std::string(argv[1]) == "--correct") return correct(argv[2], argv[3], argv[4], argv[5], argv[6], argv[7]);
    if (argc == 5 && std::string(argv[1]) == "--dimmer-curve") return set_curve(argv[2], argv[3], argv[4]);
    if (argc >= 6 && std::string(argv[1]) == "--set") return set_fixtures(argv[2], argv[3], argv[4], std::vector<std::string>(argv + 5, argv + argc));
    if (argc >= 4 && std::string(argv[1]) == "--state") return state_of(argv[2], argv[3], std::vector<std::string>(argv + 4, argv + argc));
    if (argc == 3 && std::string(argv[1]) == "--patch") return patch(argv[2], "");
    if (argc != 2) {
        std::fprintf(stderr, "usage: vizcore_inspect [--summary | --patch | --plot | --screens | --walls | --models | --wheels] <file.gdtf | file.mvr | description.xml>\n       vizcore_inspect --export <rig.mvr> <out.mvr>\n");
        return 2;
    }
    std::string path = argv[1];

    if (ends_with(path, ".xml")) {
        std::ifstream file = read_stream(path);
        std::string xml((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        GdtfResult result = parse_gdtf_description(xml);
        if (!result.ok) {
            print(result.diagnostics);
            return 1;
        }
        return print_fixture(result.fixture, result.diagnostics) ? 1 : 0;
    }

    Archive archive;
    std::string error;
    if (!archive.open_file(path, error)) {
        std::fprintf(stderr, "%s: %s\n", path.c_str(), error.c_str());
        return 1;
    }

    if (archive.contains("GeneralSceneDescription.xml")) {
        MvrResult result = load_mvr(archive);
        size_t errors = print(result.diagnostics);
        if (!result.ok) return 1;
        std::printf("MVR %d.%d from %s %s: %zu fixtures in %zu layers\n", result.scene.version_major,
                    result.scene.version_minor, result.scene.provider.c_str(), result.scene.provider_version.c_str(),
                    result.scene.fixtures.size(), result.scene.layers.size());
        std::vector<std::string> shown;
        for (const MvrFixture& fixture : result.scene.fixtures) {
            std::string patch;
            for (const MvrAddress& address : fixture.addresses) {
                patch += (patch.empty() ? "" : " ") + std::to_string(address.universe()) + "." + std::to_string(address.address());
            }
            std::printf("  %-6s %-24s %-32s %-16s %-10s at %8.3f %8.3f %8.3f m\n", fixture.fixture_id.c_str(),
                        fixture.name.c_str(), fixture.gdtf_spec.c_str(), fixture.gdtf_mode.c_str(),
                        patch.empty() ? "unpatched" : patch.c_str(), fixture.world.o[0] / 1000.0,
                        fixture.world.o[1] / 1000.0, fixture.world.o[2] / 1000.0);
        }
        for (const MvrFixture& fixture : result.scene.fixtures) {
            std::string entry = find_gdtf_entry(archive, fixture.gdtf_spec);
            bool seen = false;
            for (const std::string& name : shown) seen = seen || name == entry;
            if (entry.empty() || seen) continue;
            shown.push_back(entry);
            std::vector<uint8_t> bytes;
            Archive gdtf_archive;
            if (!archive.read(entry, bytes) || !gdtf_archive.open_memory(std::move(bytes), error)) {
                std::printf("\n%s: cannot open: %s\n", entry.c_str(), error.c_str());
                ++errors;
                continue;
            }
            GdtfResult gdtf = load_gdtf(gdtf_archive);
            std::printf("\n%s\n", entry.c_str());
            if (!gdtf.ok) {
                errors += print(gdtf.diagnostics);
                continue;
            }
            errors += print_fixture(gdtf.fixture, gdtf.diagnostics);
        }
        return errors ? 1 : 0;
    }

    GdtfResult result = load_gdtf(archive);
    if (!result.ok) {
        print(result.diagnostics);
        return 1;
    }
    return print_fixture(result.fixture, result.diagnostics) ? 1 : 0;
}
