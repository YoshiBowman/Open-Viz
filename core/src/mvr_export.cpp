#include "vizcore/mvr_export.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <vector>

#include "pugixml.hpp"
#include "text.h"
#include "vizcore/archive.h"
#include "vizcore/mesh.h"
#include "vizcore/files.h"

namespace viz {

namespace {

using pugi::xml_node;

std::string number(double value) {
    char buffer[40];
    std::snprintf(buffer, sizeof(buffer), "%.6f", value);
    for (char* c = buffer; *c; ++c) {
        if (*c == ',') *c = '.';    // a host application may have set a comma locale
    }
    return buffer;
}

std::string matrix_text(const Transform& transform) {
    std::string out;
    for (const double* row : {transform.u, transform.v, transform.w, transform.o}) {
        out += "{" + number(row[0]) + "," + number(row[1]) + "," + number(row[2]) + "}";
    }
    return out;
}

// The node's <Matrix>, or identity if it has none or one that cannot be read: as the reader takes it.
Transform matrix_of(const xml_node& node) {
    Transform transform;
    double values[12];
    const std::string written(text::trim(node.child("Matrix").text().as_string()));
    if (written.empty() || !text::parse_braced_rows(written, 4, 3, values)) return transform;
    for (int k = 0; k < 3; ++k) {
        transform.u[k] = values[k];
        transform.v[k] = values[3 + k];
        transform.w[k] = values[6 + k];
        transform.o[k] = values[9 + k];
    }
    return transform;
}

void set_child_text(xml_node node, const char* name, const std::string& value, bool first) {
    xml_node child = node.child(name);
    if (!child) child = first ? node.prepend_child(name) : node.append_child(name);
    while (child.first_child()) child.remove_child(child.first_child());
    child.append_child(pugi::node_pcdata).set_value(value.c_str());
}

// A UUID that is the same each time for the same seed: two 64-bit FNV-1a hashes of it, mixed, written as
// MVR writes UUIDs, marked as of no standard kind (version 8).
std::string stable_uuid(std::string_view seed) {
    uint64_t a = 0xcbf29ce484222325ull, b = 0x84222325cbf29ce4ull;
    for (const char c : seed) {
        a = (a ^ static_cast<uint8_t>(c)) * 0x100000001b3ull;
        b = (b ^ static_cast<uint8_t>(c)) * 0x100000001b3ull;
        b ^= b >> 29;
    }
    // Mixed through, so that seeds differing in one letter give UUIDs that differ throughout.
    auto mix = [](uint64_t x) {
        x ^= x >> 30;
        x *= 0xbf58476d1ce4e5b9ull;
        x ^= x >> 27;
        x *= 0x94d049bb133111ebull;
        return x ^ (x >> 31);
    };
    a = mix(a);
    b = mix(b ^ a);
    uint8_t bytes[16];
    for (int k = 0; k < 8; ++k) {
        bytes[k] = static_cast<uint8_t>(a >> (8 * k));
        bytes[8 + k] = static_cast<uint8_t>(b >> (8 * k));
    }
    bytes[6] = static_cast<uint8_t>((bytes[6] & 0x0F) | 0x80);
    bytes[8] = static_cast<uint8_t>((bytes[8] & 0x3F) | 0x80);
    char out[37];
    std::snprintf(out, sizeof out, "%02X%02X%02X%02X-%02X%02X-%02X%02X-%02X%02X-%02X%02X%02X%02X%02X%02X", bytes[0], bytes[1], bytes[2], bytes[3], bytes[4],
                  bytes[5], bytes[6], bytes[7], bytes[8], bytes[9], bytes[10], bytes[11], bytes[12], bytes[13], bytes[14], bytes[15]);
    return out;
}

struct Writer : pugi::xml_writer {
    std::string out;
    void write(const void* data, size_t size) override { out.append(static_cast<const char*>(data), size); }
};

struct Walk {
    const ShowEdits& edits;
    std::vector<bool> used;
    std::vector<bool> removed_used;
    MvrExport& result;
};

// `was` is where the file puts the thing this list belongs to, `now` where the export puts it;
// they differ below a fixture that was moved.
bool holds_fixtures(const xml_node& node, int depth = 0) {
    if (depth > 64) return false;
    for (const xml_node& child : node.child("ChildList").children()) {
        if (std::strcmp(child.name(), "Fixture") == 0 || holds_fixtures(child, depth + 1)) return true;
    }
    return false;
}

void walk(xml_node child_list, const Transform& was, const Transform& now, bool moved, Walk& state, int depth) {
    if (depth > 64) return;
    std::vector<xml_node> taken_out;
    for (xml_node node : child_list.children()) {
        if (node.type() != pugi::node_element) continue;
        if (std::strcmp(node.name(), "Fixture") == 0 && node.attribute("uuid").as_string()[0] != '\0' &&
            state.edits.is_removed(node.attribute("uuid").as_string())) {
            for (size_t r = 0; r < state.edits.removed.size(); ++r) {
                if (text::iequals(state.edits.removed[r], node.attribute("uuid").as_string())) state.removed_used[r] = true;
            }
            if (holds_fixtures(node)) {
                state.result.diagnostics.warn(std::string("Fixture '") + node.attribute("name").as_string() + "'",
                                              "was removed, but other fixtures hang on it; it is left in the export");
            } else {
                taken_out.push_back(node);
                ++state.result.changed;
                continue;
            }
        }
        const Transform local = matrix_of(node);
        const Transform world_was = compose(was, local);
        Transform world_now = compose(now, local);
        bool below_moved = moved;
        if (std::strcmp(node.name(), "Fixture") == 0) {
            const char* uuid = node.attribute("uuid").as_string();
            const FixtureEdit* edit = nullptr;
            for (size_t e = 0; e < state.edits.fixtures.size(); ++e) {
                if (uuid[0] != '\0' && text::iequals(state.edits.fixtures[e].uuid, uuid)) {
                    edit = &state.edits.fixtures[e];
                    state.used[e] = true;
                }
            }
            if (edit && edit->empty()) edit = nullptr;
            const std::string where = std::string("Fixture '") + node.attribute("name").as_string() + "'";
            // A fixture is shown where its own edit puts it, else where the file has it: also one
            // that hangs on a fixture that was moved, which therefore needs its matrix rewritten
            // to stay where it is.
            if ((edit && edit->world) || moved) {
                const Transform wanted = (edit && edit->world) ? *edit->world : world_was;
                Transform back;
                if (invert(now, back)) {
                    set_child_text(node, "Matrix", matrix_text(compose(back, wanted)), true);
                    world_now = wanted;
                    below_moved = true;
                } else {
                    state.result.diagnostics.warn(where, "sits in something scaled to nothing, so its place cannot be written; left as the file has it");
                }
            }
            if (edit && edit->gdtf) {
                // A type of its own from the fixture library: named, and packed into the archive with it.
                set_child_text(node, "GDTFSpec", *edit->gdtf, false);
                bool listed = false;
                for (const std::string& file : state.result.library_files) listed = listed || file == *edit->gdtf;
                if (!listed) state.result.library_files.push_back(*edit->gdtf);
                if (!edit->mode) state.result.diagnostics.warn(where, "was given another type but no mode; its mode is left as the file has it");
            }
            if (edit && edit->mode) set_child_text(node, "GDTFMode", *edit->mode, false);
            if (edit && edit->name) {
                pugi::xml_attribute name = node.attribute("name");
                if (!name) name = node.append_attribute("name");
                name.set_value(edit->name->c_str());
            }
            if (edit && edit->fixture_id) {
                set_child_text(node, "FixtureID", *edit->fixture_id, false);
                // The number beside it, where the file keeps one: a console reads either.
                long long numeric = 0;
                if (node.child("FixtureIDNumeric") && text::parse_int(*edit->fixture_id, numeric)) {
                    set_child_text(node, "FixtureIDNumeric", std::to_string(numeric), false);
                }
            }
            if (edit && edit->invert_pan) set_child_text(node, "DMXInvertPan", *edit->invert_pan ? "true" : "false", false);
            if (edit && edit->invert_tilt) set_child_text(node, "DMXInvertTilt", *edit->invert_tilt ? "true" : "false", false);
            if (edit && edit->universe && edit->address) {
                xml_node addresses = node.child("Addresses");
                xml_node first;
                for (xml_node address : addresses.children("Address")) {
                    long long break_id = 0;
                    text::parse_int(address.attribute("break").value(), break_id);
                    if (break_id == 0 && !first) first = address;
                }
                if (*edit->universe <= 0) {
                    if (first) addresses.remove_child(first);
                } else {
                    const bool dotted = first && std::strchr(first.text().as_string(), '.') != nullptr;
                    if (!addresses) addresses = node.append_child("Addresses");
                    if (!first) {
                        first = addresses.prepend_child("Address");
                        first.append_attribute("break").set_value("0");
                    }
                    while (first.first_child()) first.remove_child(first.first_child());
                    const long long absolute = static_cast<long long>(*edit->universe - 1) * 512 + *edit->address;
                    const std::string written = dotted ? std::to_string(*edit->universe) + "." + std::to_string(*edit->address) : std::to_string(absolute);
                    first.append_child(pugi::node_pcdata).set_value(written.c_str());
                }
            }
            if (edit) ++state.result.changed;
        }
        walk(node.child("ChildList"), world_was, world_now, below_moved, state, depth + 1);
    }
    for (xml_node node : taken_out) child_list.remove_child(node);
}

}  // namespace

MvrExport apply_show_edits(std::string_view scene_xml, const ShowEdits& edits, const std::vector<PlacedPiece>& pieces) {
    MvrExport result;
    while (!scene_xml.empty() && (scene_xml.back() == '\0' || scene_xml.back() == '\n' || scene_xml.back() == '\r' || scene_xml.back() == ' ')) {
        scene_xml.remove_suffix(1);
    }
    pugi::xml_document document;
    const pugi::xml_parse_result parsed =
        document.load_buffer(scene_xml.data(), scene_xml.size(), pugi::parse_default | pugi::parse_declaration | pugi::parse_comments, pugi::encoding_auto);
    xml_node scene = document.child("GeneralSceneDescription").child("Scene");
    if (!parsed || !scene) {
        result.diagnostics.error("GeneralSceneDescription.xml", parsed ? "no <Scene> to write the changes into" : std::string("XML error: ") + parsed.description());
        return result;
    }
    Walk state{edits, std::vector<bool>(edits.fixtures.size(), false), std::vector<bool>(edits.removed.size(), false), result};
    for (xml_node layer : scene.child("Layers").children("Layer")) {
        walk(layer.child("ChildList"), matrix_of(layer), matrix_of(layer), false, state, 0);
    }
    for (size_t e = 0; e < edits.fixtures.size(); ++e) {
        if (!state.used[e] && !edits.fixtures[e].empty()) {
            result.diagnostics.warn("Fixture " + edits.fixtures[e].uuid, "was changed, but the file has no such fixture; that change is not in the export");
        }
    }
    for (size_t r = 0; r < edits.removed.size(); ++r) {
        if (!state.removed_used[r]) {
            result.diagnostics.warn("Fixture " + edits.removed[r], "was removed, but the file has no such fixture");
        }
    }

    // Added fixtures and set pieces: a layer of their own at the end, each placed in the world as it stands.
    // Its UUID, and each piece's, follow from the file's first layer, so that they stay the same from one export to the next.
    xml_node list;
    std::string seed;
    if (!edits.added.empty() || !pieces.empty()) {
        xml_node layers = scene.child("Layers");
        if (!layers) layers = scene.append_child("Layers");
        seed = layers.child("Layer").attribute("uuid").as_string();
        xml_node layer;
        for (xml_node candidate : layers.children("Layer")) {
            if (std::strcmp(candidate.attribute("name").as_string(), "Openviz") == 0 && !candidate.child("Matrix")) layer = candidate;
        }
        if (!layer) {
            layer = layers.append_child("Layer");
            layer.append_attribute("uuid").set_value(stable_uuid("Openviz layer|" + seed).c_str());
            layer.append_attribute("name").set_value("Openviz");
        }
        list = layer.child("ChildList");
        if (!list) list = layer.append_child("ChildList");
    }
    if (!edits.added.empty()) {
        for (const AddedFixture& added : edits.added) {
            xml_node node = list.append_child("Fixture");
            node.append_attribute("uuid").set_value(added.uuid.c_str());
            node.append_attribute("name").set_value(added.name.c_str());
            auto put = [&node](const char* name, const std::string& value) {
                node.append_child(name).append_child(pugi::node_pcdata).set_value(value.c_str());
            };
            put("Matrix", matrix_text(added.world));
            put("GDTFSpec", added.gdtf);
            put("GDTFMode", added.mode);
            put("FixtureID", added.fixture_id);
            long long numeric = 0;
            text::parse_int(added.fixture_id, numeric);
            put("FixtureIDNumeric", std::to_string(numeric));
            put("UnitNumber", "0");
            xml_node addresses = node.append_child("Addresses");
            if (added.universe > 0) {
                xml_node address = addresses.append_child("Address");
                address.append_attribute("break").set_value("0");
                address.append_child(pugi::node_pcdata).set_value((std::to_string(added.universe) + "." + std::to_string(added.address)).c_str());
            }
            if (added.invert_pan) put("DMXInvertPan", "true");
            if (added.invert_tilt) put("DMXInvertTilt", "true");
            bool listed = false;
            for (const std::string& file : result.library_files) listed = listed || file == added.gdtf;
            if (!listed) result.library_files.push_back(added.gdtf);
            ++result.changed;
        }
    }
    for (const PlacedPiece& placed : pieces) {
        const std::string uuid = stable_uuid("Openviz piece|" + seed + "|" + placed.id);
        const std::string file = "Openviz_" + uuid + ".glb";
        const std::string name = placed.name.empty() ? placed.piece.kind : placed.name;
        xml_node node = list.append_child("SceneObject");
        node.append_attribute("uuid").set_value(uuid.c_str());
        node.append_attribute("name").set_value(name.c_str());
        node.append_child("Matrix").append_child(pugi::node_pcdata).set_value(matrix_text(placed.world).c_str());
        node.append_child("Geometries").append_child("Geometry3D").append_attribute("fileName").set_value(file.c_str());
        const Mesh shape = placed_piece_mesh(placed);
        if (shape.triangle_count() == 0) {
            result.diagnostics.warn("Set piece '" + name + "'", "its model cannot be read, so it is not in the export");
            list.remove_child(node);
            continue;
        }
        result.models.emplace_back(file, write_glb(shape, name));
        ++result.pieces;
    }

    Writer writer;
    document.save(writer, "  ", pugi::format_default, pugi::encoding_utf8);
    result.xml = std::move(writer.out);
    result.ok = true;
    return result;
}

bool export_mvr(const std::string& source_path, const ShowEdits& edits, const std::string& out_path, int& changed, std::string& error,
                const std::string& library_dir, const std::vector<PlacedPiece>& pieces) {
    changed = 0;
    Archive archive;
    if (!archive.open_file(source_path, error)) return false;
    std::vector<uint8_t> xml;
    if (!archive.read("GeneralSceneDescription.xml", xml)) {
        error = "no GeneralSceneDescription.xml in the archive";
        return false;
    }
    MvrExport written = apply_show_edits(std::string_view(reinterpret_cast<const char*>(xml.data()), xml.size()), edits, pieces);
    if (!written.ok) {
        error = written.diagnostics.items.empty() ? "the scene description cannot be read" : written.diagnostics.items.front().message;
        return false;
    }
    // The fixture types of added fixtures go into the archive with them, as an MVR must carry them.
    std::vector<std::pair<std::string, std::string>> entries = {{"GeneralSceneDescription.xml", written.xml}};
    for (const std::string& name : written.library_files) {
        if (archive.contains(name)) continue;
        std::ifstream file = read_stream(library_dir + "/" + name);
        if (library_dir.empty() || !file) {
            error = "the fixture type " + name + " of an added or retyped fixture is not in the fixture library";
            return false;
        }
        entries.emplace_back(name, std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>()));
    }
    for (auto& model : written.models) entries.emplace_back(model.first, std::string(model.second.begin(), model.second.end()));
    std::vector<uint8_t> bytes;
    if (!archive.copy_with(entries, bytes, error)) return false;
    // To a neighbour first, then into place: an export that fails half-way leaves nothing behind.
    const std::string partial = out_path + ".part";
    {
        std::ofstream file = write_stream(partial);
        file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        file.flush();
        if (!file) {
            error = "cannot write " + out_path;
            remove_file(partial);
            return false;
        }
    }
    if (!rename_file(partial, out_path)) {
        error = "cannot write " + out_path;
        remove_file(partial);
        return false;
    }
    changed = written.changed;
    return true;
}

}  // namespace viz
