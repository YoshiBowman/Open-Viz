// MVR scene description (DIN SPEC 15801), parsed from GeneralSceneDescription.xml.
//
// Read: fixtures with their patch, identification and transforms, found anywhere in the layer /
// group / child-list hierarchy; and the scenery around them: scene objects, trusses, supports,
// video screens, projectors and focus points, each with the model files it is drawn with (symbols
// resolved to the files they stand for), its layer and its class, and for a video screen or a
// projector the sources it names. Not read: mappings, connections, and the user data
// applications add.
#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "vizcore/types.h"

namespace viz {

class Archive;

struct MvrAddress {
    int break_id = 0;        // the Address node's break attribute, which counts from 0
    uint32_t absolute = 0;   // 1-based across universes: (universe - 1) * 512 + address

    int universe() const { return absolute ? static_cast<int>((absolute - 1) / 512) + 1 : 0; }
    int address() const { return absolute ? static_cast<int>((absolute - 1) % 512) + 1 : 0; }
};

struct MvrFixture {
    std::string uuid;
    std::string name;
    std::string layer;               // name of the layer it sits in
    std::string parent_uuid;         // set for a fixture nested in another fixture's child list
    std::string multipatch;
    std::string gdtf_spec;           // file name inside the archive, as written
    std::string gdtf_mode;
    std::string fixture_id;
    int fixture_id_numeric = 0;
    int unit_number = 0;
    int custom_id = 0;
    int custom_id_type = 0;
    std::string function;
    std::string focus;               // UUID of a focus point
    std::string position;            // UUID of a position
    std::string classing;
    std::string child_position;      // geometry link into the parent fixture's GDTF
    bool dmx_invert_pan = false;
    bool dmx_invert_tilt = false;
    bool cast_shadow = false;
    std::optional<ColorCIE> color;
    std::vector<MvrAddress> addresses;

    Transform local;    // as written, relative to the parent object
    // Composed through every parent group and the layer. For a nested fixture this stops at the
    // parent fixture's origin: the offset of the geometry named by child_position is not applied.
    Transform world;
    // False when the file writes no <Matrix> for it. A console writes none for what was patched
    // and never set down in its 3D space (dimmer channels, media server layers): such fixtures
    // would all stand in one spot, that of whatever they sit in, on top of each other.
    bool placed = true;
};

// One model file (3DS or binary glTF, in the archive beside the scene description) and where it
// sits within the object it belongs to. Millimetres, like every MVR transform.
struct MvrGeometry {
    std::string file;
    Transform local;
};

// A picture source an MVR names for a video screen or a projector.
struct MvrSource {
    std::string type;               // "NDI", "File", "CITP" or "CaptureDevice", as written
    std::string linked_geometry;    // the part of the object's GDTF that shows it, if it has one
    std::string value;              // a stream's name, a file in the archive, or a capture device's name
};

enum class MvrObjectKind { SceneObject, Truss, Support, VideoScreen, Projector, FocusPoint };

VIZCORE_API const char* to_string(MvrObjectKind kind);

// Something on stage that is not a fixture.
struct MvrObject {
    MvrObjectKind kind = MvrObjectKind::SceneObject;
    std::string uuid;
    std::string name;
    std::string layer;
    std::string classing;                 // UUID of its class; MvrScene::class_name() gives the name
    Transform world;                      // composed through every parent group and the layer
    std::string parent_fixture;           // UUID of the fixture whose child list it is in, however deep: it rides on that fixture
    std::string child_position;           // geometry link into that fixture's GDTF, where the file gives one
    std::vector<MvrGeometry> geometry;    // may be empty: a focus point is only a place
    std::vector<MvrSource> sources;       // video screens and projectors: what they are meant to show
};

struct MvrClass {
    std::string uuid;
    std::string name;
};

struct VIZCORE_API MvrScene {
    int version_major = 0;
    int version_minor = 0;
    std::string provider;
    std::string provider_version;
    std::vector<std::string> layers;
    std::vector<MvrClass> classes;
    std::vector<MvrFixture> fixtures;
    std::vector<MvrObject> objects;

    // The name of the class with this UUID (compared without regard to case), or empty.
    std::string class_name(std::string_view uuid) const;
};

struct MvrResult {
    bool ok = false;
    MvrScene scene;
    Diagnostics diagnostics;
};

VIZCORE_API MvrResult parse_mvr_scene(std::string_view xml);
VIZCORE_API MvrResult load_mvr(const Archive& archive);
VIZCORE_API MvrResult load_mvr_file(const std::string& path);

// The archive entry holding a fixture's GDTF. Writers disagree about including the ".gdtf"
// extension in GDTFSpec, so both spellings are tried. Empty if the archive has neither.
VIZCORE_API std::string find_gdtf_entry(const Archive& archive, std::string_view gdtf_spec);

}  // namespace viz
