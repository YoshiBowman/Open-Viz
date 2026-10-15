#include "vizcore/mvr.h"

#include <cstring>
#include <utility>

#include "pugixml.hpp"
#include "text.h"
#include "vizcore/archive.h"

namespace viz {

namespace {

using pugi::xml_node;

std::string child_text(const xml_node& node, const char* name) {
    return std::string(text::trim(node.child(name).text().as_string()));
}

std::string quoted(const std::string& text) { return "'" + text + "'"; }

int child_int(const xml_node& node, const char* name, Diagnostics& diagnostics, const std::string& where) {
    std::string written = child_text(node, name);
    if (written.empty()) return 0;
    long long value = 0;
    if (!text::parse_int(written, value)) {
        diagnostics.warn(where, std::string("<") + name + "> \"" + written + "\" is not an integer");
        return 0;
    }
    return static_cast<int>(value);
}

bool child_bool(const xml_node& node, const char* name, Diagnostics& diagnostics, const std::string& where) {
    std::string written = child_text(node, name);
    if (written.empty()) return false;
    bool value = false;
    if (!text::parse_bool(written, value)) {
        diagnostics.warn(where, std::string("<") + name + "> \"" + written + "\" is not true or false");
    }
    return value;
}

Transform child_matrix(const xml_node& node, Diagnostics& diagnostics, const std::string& where) {
    Transform transform;
    std::string written = child_text(node, "Matrix");
    if (written.empty()) return transform;
    double values[12];
    if (!text::parse_braced_rows(written, 4, 3, values)) {
        diagnostics.warn(where, "<Matrix> is not four rows of three numbers; identity used");
        return transform;
    }
    for (int k = 0; k < 3; ++k) {
        transform.u[k] = values[k];
        transform.v[k] = values[3 + k];
        transform.w[k] = values[6 + k];
        transform.o[k] = values[9 + k];
    }
    return transform;
}

void parse_addresses(const xml_node& fixture_node, MvrFixture& fixture, Diagnostics& diagnostics,
                     const std::string& where) {
    for (const xml_node& node : fixture_node.child("Addresses").children("Address")) {
        MvrAddress address;
        long long break_id = 0;
        if (node.attribute("break").value()[0] != '\0' && !text::parse_int(node.attribute("break").value(), break_id)) {
            diagnostics.warn(where, std::string("Address break=\"") + node.attribute("break").value() + "\" is not an integer; using 0");
            break_id = 0;
        }
        address.break_id = static_cast<int>(break_id);

        // Either one absolute address, or "Universe.Address" with both counting from 1.
        std::string written(text::trim(node.text().as_string()));
        size_t dot = written.find('.');
        long long absolute = 0;
        bool valid = false;
        if (dot == std::string::npos) {
            valid = text::parse_int(written, absolute) && absolute >= 1;
        } else {
            long long universe = 0;
            long long slot = 0;
            valid = text::parse_int(std::string_view(written).substr(0, dot), universe) &&
                    text::parse_int(std::string_view(written).substr(dot + 1), slot) && universe >= 1 && slot >= 1 &&
                    slot <= 512;
            absolute = (universe - 1) * 512 + slot;
        }
        if (!valid || absolute > 0xFFFFFFFFll) {
            diagnostics.warn(where, "Address \"" + written + "\" is neither an absolute address nor Universe.Address; ignored");
            continue;
        }
        address.absolute = static_cast<uint32_t>(absolute);
        for (const MvrAddress& other : fixture.addresses) {
            if (other.break_id == address.break_id) {
                diagnostics.warn(where, "break " + std::to_string(address.break_id) + " is addressed more than once");
            }
        }
        fixture.addresses.push_back(address);
    }
}

std::string upper(std::string text) {
    for (char& c : text) {
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    }
    return text;
}

// Symbol definitions by UUID (upper case). The nodes belong to the document being parsed.
using Symdefs = std::vector<std::pair<std::string, xml_node>>;

// Collects the model files under a <Geometries> node or inside a symbol definition. A <Symbol>
// stands for everything its definition holds, placed by the symbol's own matrix.
void gather_geometry(const xml_node& list, const Transform& parent, const Symdefs& symdefs, std::vector<MvrGeometry>& out,
                     Diagnostics& diagnostics, const std::string& where, int depth) {
    if (depth > 8) {
        diagnostics.warn(where, "symbols are nested more than 8 deep; the rest was left out");
        return;
    }
    for (const xml_node& node : list.children()) {
        if (node.type() != pugi::node_element) continue;
        const Transform local = compose(parent, child_matrix(node, diagnostics, where));
        if (std::strcmp(node.name(), "Geometry3D") == 0) {
            MvrGeometry geometry;
            geometry.file = std::string(text::trim(node.attribute("fileName").as_string()));
            geometry.local = local;
            if (geometry.file.empty()) {
                diagnostics.warn(where, "a Geometry3D names no file");
                continue;
            }
            out.push_back(std::move(geometry));
        } else if (std::strcmp(node.name(), "Symbol") == 0) {
            const std::string wanted = upper(node.attribute("symdef").as_string());
            const xml_node* definition = nullptr;
            for (const auto& entry : symdefs) {
                if (entry.first == wanted) definition = &entry.second;
            }
            if (!definition) {
                diagnostics.warn(where, "uses the symbol " + quoted(node.attribute("symdef").as_string()) + ", which the file does not define");
                continue;
            }
            // Real files put a definition's content in a ChildList; the specification's text has it directly inside.
            const xml_node inside = definition->child("ChildList") ? definition->child("ChildList") : *definition;
            gather_geometry(inside, local, symdefs, out, diagnostics, where, depth + 1);
        }
    }
}

bool object_kind(const char* name, MvrObjectKind& kind) {
    static const struct {
        const char* name;
        MvrObjectKind kind;
    } kinds[] = {{"SceneObject", MvrObjectKind::SceneObject}, {"Truss", MvrObjectKind::Truss},           {"Support", MvrObjectKind::Support},
                 {"VideoScreen", MvrObjectKind::VideoScreen}, {"Projector", MvrObjectKind::Projector}, {"FocusPoint", MvrObjectKind::FocusPoint}};
    for (const auto& entry : kinds) {
        if (std::strcmp(name, entry.name) == 0) {
            kind = entry.kind;
            return true;
        }
    }
    return false;
}

struct Walk {
    MvrScene& scene;
    Diagnostics& diagnostics;
    std::string layer;
    const Symdefs& symdefs;
};

void walk_child_list(const xml_node& child_list, const Transform& parent, const std::string& parent_fixture,
                     Walk& walk, int depth) {
    if (depth > 64) {
        walk.diagnostics.error("Scene", "objects are nested more than 64 levels deep; the rest was dropped");
        return;
    }
    for (const xml_node& node : child_list.children()) {
        if (node.type() != pugi::node_element) continue;
        std::string where = std::string(node.name()) + " " + quoted(node.attribute("name").as_string());
        Transform local = child_matrix(node, walk.diagnostics, where);
        Transform world = compose(parent, local);

        if (std::strcmp(node.name(), "Fixture") != 0) {
            // A truss, a scene object and the like are kept; a group is only a place for what is inside it.
            MvrObjectKind kind;
            if (object_kind(node.name(), kind)) {
                MvrObject object;
                object.kind = kind;
                object.uuid = node.attribute("uuid").as_string();
                object.name = node.attribute("name").as_string();
                object.layer = walk.layer;
                object.classing = child_text(node, "Classing");
                object.world = world;
                object.parent_fixture = parent_fixture;
                object.child_position = child_text(node, "ChildPosition");
                gather_geometry(node.child("Geometries"), Transform(), walk.symdefs, object.geometry, walk.diagnostics, where, 0);
                for (const xml_node& source : node.child("Sources").children("Source")) {
                    MvrSource named;
                    named.type = source.attribute("type").as_string();
                    named.linked_geometry = source.attribute("linkedGeometry").as_string();
                    named.value = std::string(text::trim(source.text().as_string()));
                    if (!named.value.empty()) object.sources.push_back(std::move(named));
                }
                walk.scene.objects.push_back(std::move(object));
            }
            walk_child_list(node.child("ChildList"), world, parent_fixture, walk, depth + 1);
            continue;
        }

        MvrFixture fixture;
        fixture.uuid = node.attribute("uuid").as_string();
        fixture.name = node.attribute("name").as_string();
        fixture.multipatch = node.attribute("multipatch").as_string();
        fixture.layer = walk.layer;
        fixture.parent_uuid = parent_fixture;
        fixture.local = local;
        fixture.world = world;
        fixture.placed = !child_text(node, "Matrix").empty();
        if (fixture.uuid.empty()) walk.diagnostics.warn(where, "fixture has no uuid");
        fixture.gdtf_spec = child_text(node, "GDTFSpec");
        fixture.gdtf_mode = child_text(node, "GDTFMode");
        if (!fixture.gdtf_spec.empty() && fixture.gdtf_mode.empty()) {
            walk.diagnostics.warn(where, "GDTFSpec is given without a GDTFMode");
        }
        fixture.fixture_id = child_text(node, "FixtureID");
        fixture.fixture_id_numeric = child_int(node, "FixtureIDNumeric", walk.diagnostics, where);
        fixture.unit_number = child_int(node, "UnitNumber", walk.diagnostics, where);
        fixture.custom_id = child_int(node, "CustomId", walk.diagnostics, where);
        fixture.custom_id_type = child_int(node, "CustomIdType", walk.diagnostics, where);
        fixture.function = child_text(node, "Function");
        fixture.focus = child_text(node, "Focus");
        fixture.position = child_text(node, "Position");
        fixture.classing = child_text(node, "Classing");
        fixture.child_position = child_text(node, "ChildPosition");
        fixture.dmx_invert_pan = child_bool(node, "DMXInvertPan", walk.diagnostics, where);
        fixture.dmx_invert_tilt = child_bool(node, "DMXInvertTilt", walk.diagnostics, where);
        fixture.cast_shadow = child_bool(node, "CastShadow", walk.diagnostics, where);
        std::string color = child_text(node, "Color");
        if (!color.empty()) {
            ColorCIE parsed;
            if (text::parse_color(color, parsed)) {
                fixture.color = parsed;
            } else {
                walk.diagnostics.warn(where, "<Color> \"" + color + "\" is not an x,y,Y colour");
            }
        }
        parse_addresses(node, fixture, walk.diagnostics, where);

        std::string uuid = fixture.uuid;
        walk.scene.fixtures.push_back(std::move(fixture));
        walk_child_list(node.child("ChildList"), world, uuid, walk, depth + 1);
    }
}

}  // namespace

MvrResult parse_mvr_scene(std::string_view xml) {
    MvrResult result;
    while (!xml.empty() && (xml.back() == '\0' || xml.back() == '\n' || xml.back() == '\r' || xml.back() == ' ')) {
        xml.remove_suffix(1);
    }
    pugi::xml_document document;
    pugi::xml_parse_result parsed = document.load_buffer(xml.data(), xml.size(), pugi::parse_default, pugi::encoding_auto);
    if (!parsed) {
        result.diagnostics.error("GeneralSceneDescription.xml", std::string("XML error at byte ") +
                                                                    std::to_string(parsed.offset) + ": " + parsed.description());
        return result;
    }
    xml_node root = document.child("GeneralSceneDescription");
    if (!root) {
        result.diagnostics.error("GeneralSceneDescription.xml", "no <GeneralSceneDescription> root node");
        return result;
    }
    result.scene.version_major = root.attribute("verMajor").as_int();
    result.scene.version_minor = root.attribute("verMinor").as_int();
    result.scene.provider = root.attribute("provider").as_string();
    result.scene.provider_version = root.attribute("providerVersion").as_string();

    xml_node scene = root.child("Scene");
    if (!scene) {
        result.diagnostics.error("GeneralSceneDescription.xml", "no <Scene> node");
        return result;
    }
    Symdefs symdefs;
    for (const xml_node& node : scene.child("AUXData").children()) {
        if (std::strcmp(node.name(), "Symdef") == 0) {
            symdefs.emplace_back(upper(node.attribute("uuid").as_string()), node);
        } else if (std::strcmp(node.name(), "Class") == 0) {
            result.scene.classes.push_back({node.attribute("uuid").as_string(), node.attribute("name").as_string()});
        }
    }
    for (const xml_node& layer : scene.child("Layers").children("Layer")) {
        Walk walk{result.scene, result.diagnostics, layer.attribute("name").as_string(), symdefs};
        result.scene.layers.push_back(walk.layer);
        Transform layer_transform = child_matrix(layer, result.diagnostics, "Layer " + quoted(walk.layer));
        walk_child_list(layer.child("ChildList"), layer_transform, std::string(), walk, 0);
    }
    result.ok = true;
    return result;
}

const char* to_string(MvrObjectKind kind) {
    switch (kind) {
        case MvrObjectKind::SceneObject: return "SceneObject";
        case MvrObjectKind::Truss: return "Truss";
        case MvrObjectKind::Support: return "Support";
        case MvrObjectKind::VideoScreen: return "VideoScreen";
        case MvrObjectKind::Projector: return "Projector";
        case MvrObjectKind::FocusPoint: return "FocusPoint";
    }
    return "SceneObject";
}

std::string MvrScene::class_name(std::string_view uuid) const {
    if (uuid.empty()) return std::string();
    for (const MvrClass& entry : classes) {
        if (text::iequals(entry.uuid, uuid)) return entry.name;
    }
    return std::string();
}

MvrResult load_mvr(const Archive& archive) {
    std::vector<uint8_t> xml;
    if (!archive.read("GeneralSceneDescription.xml", xml)) {
        MvrResult result;
        result.diagnostics.error("archive", "no GeneralSceneDescription.xml in the archive");
        return result;
    }
    return parse_mvr_scene(std::string_view(reinterpret_cast<const char*>(xml.data()), xml.size()));
}

MvrResult load_mvr_file(const std::string& path) {
    Archive archive;
    std::string error;
    if (!archive.open_file(path, error)) {
        MvrResult result;
        result.diagnostics.error(path, error);
        return result;
    }
    return load_mvr(archive);
}

std::string find_gdtf_entry(const Archive& archive, std::string_view gdtf_spec) {
    std::string name(gdtf_spec);
    if (name.empty()) return std::string();
    if (archive.contains(name)) return name;
    if (archive.contains(name + ".gdtf")) return name + ".gdtf";
    return std::string();
}

}  // namespace viz
