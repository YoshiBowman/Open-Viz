// Writes a rig back out as MVR with what was changed since it was read: where fixtures hang, their
// patch, their mode. The result is the original file in every other respect: the same objects,
// classes and layers in the same order, and every packed fixture type and model carried over as it
// was stored. So it can go back to the console or the drawing program it came from.
//
// A fixture's place is kept in an MVR relative to whatever it sits in (a group, a truss, its
// layer), while an edit holds its place in the world; the export works the one out from the other.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "vizcore/pieces.h"
#include "vizcore/show_edits.h"
#include "vizcore/types.h"

namespace viz {

struct MvrExport {
    bool ok = false;           // false: the scene description could not be read at all
    std::string xml;           // GeneralSceneDescription.xml with the edits written in
    int changed = 0;           // fixtures rewritten, added or taken out
    std::vector<std::string> library_files;    // GDTF files of added and retyped fixtures, to be packed into the archive
    int pieces = 0;            // set pieces written
    std::vector<std::pair<std::string, std::vector<uint8_t>>> models;    // their model files, to be packed: name in the archive, contents
    Diagnostics diagnostics;   // edits that fit no fixture of the file, and the like
};

// Patch edits go to the fixture's first break (MVR's break 0), in the notation the file already
// uses there: one absolute address, or Universe.Address. Unpatching removes that address. A
// fixture given a type of its own gets that file as its GDTFSpec, and the file is packed.
//
// A removed fixture's node is taken out, unless other fixtures hang on it: then it stays, with a
// warning. Added fixtures are written into a layer of their own, "Openviz", at the end.
//
// Set pieces added in the application go into that layer too, each a SceneObject with its shape as
// a GLB model of its own (vizcore/pieces.h, write_glb). A piece keeps its UUID from one export to
// the next, as does the layer, so that a program taking a newer export in can tell what it has.
VIZCORE_API MvrExport apply_show_edits(std::string_view scene_xml, const ShowEdits& edits, const std::vector<PlacedPiece>& pieces = {});

// Reads the MVR at `source_path`, applies `edits`, and writes the result to `out_path`. False,
// with a reason in `error`, if that fails; nothing is left at `out_path` then.
// `library_dir` is where the GDTF files of added fixtures are; each is packed into the new archive
// unless one of that name is in it already. An added fixture whose file is missing fails the export.
VIZCORE_API bool export_mvr(const std::string& source_path, const ShowEdits& edits, const std::string& out_path, int& changed, std::string& error,
                            const std::string& library_dir = std::string(), const std::vector<PlacedPiece>& pieces = {});

}  // namespace viz
