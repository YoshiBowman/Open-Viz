// What was changed about a rig after it left the console or the drawing program: where a fixture
// hangs, its patch, its mode. The MVR file is never rewritten; the changes are kept beside it as
// "<show>.edits.xml" and laid over the MVR each time the show is opened.
//
//   <ShowEdits Version="1">
//     <Fixture UUID="..." Matrix="{u}{v}{w}{o}" Universe="3" Address="101" Mode="Basic"
//              Name="Spot 12" FixtureID="212" InvertPan="true" InvertTilt="false"
//              GDTF="Acme@Spot.gdtf"/>
//     <Added UUID="..." Name="Spot 12" FixtureID="212" GDTF="Acme@Spot.gdtf" Mode="Basic"
//            Matrix="{u}{v}{w}{o}" Universe="3" Address="149"/>
//     <Removed UUID="..."/>
//   </ShowEdits>
//
// A fixture is found by its MVR UUID. Only what was changed is written: an attribute that is
// absent leaves the MVR's value in force. Matrix is the fixture's place in the world in MVR's own
// terms (millimetres, right-handed, Z up), so it can go back into an MVR export unchanged.
// InvertPan and InvertTilt take the place of the file's own DMXInvertPan and DMXInvertTilt.
// GDTF gives this one fixture a type of its own, a file name in the fixture library the
// application keeps, in the place of the rig file's (and of any stand-in for it); its Mode then
// names a mode of that file.
//
// Added is a fixture the MVR does not have. Its GDTF is a file name in the fixture library the
// application keeps (no folders in it), and everything about it is in the entry itself. Removed
// takes a fixture of the MVR out of the show.
#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "vizcore/types.h"

namespace viz {

struct FixtureEdit {
    std::string uuid;
    std::optional<Transform> world;
    std::optional<int> universe;    // 0 = unpatched; always written together with address
    std::optional<int> address;     // 1-512
    std::optional<std::string> mode;
    std::optional<std::string> name;          // what it is called
    std::optional<std::string> fixture_id;    // its number on the console; may be set to nothing
    std::optional<bool> invert_pan;           // in place of the file's DMXInvertPan
    std::optional<bool> invert_tilt;
    std::optional<std::string> gdtf;          // a file in the fixture library, in place of the rig file's type, for this fixture alone

    bool empty() const { return !world && !universe && !address && !mode && !name && !fixture_id && !invert_pan && !invert_tilt && !gdtf; }
};

struct AddedFixture {
    std::string uuid;
    std::string name;
    std::string fixture_id;
    std::string gdtf;      // file name in the fixture library, with its ending
    std::string mode;      // empty: the type's first mode
    Transform world;
    int universe = 0;      // 0 = unpatched
    int address = 1;       // 1-512
    bool invert_pan = false;
    bool invert_tilt = false;
};

struct ShowEdits {
    std::vector<FixtureEdit> fixtures;
    std::vector<AddedFixture> added;
    std::vector<std::string> removed;    // UUIDs of fixtures of the MVR that are taken out

    VIZCORE_API const FixtureEdit* find(std::string_view uuid) const;
    VIZCORE_API const AddedFixture* find_added(std::string_view uuid) const;
    // The entry to write a change to a fixture of the MVR into: the one it has, or a new one.
    VIZCORE_API FixtureEdit& edit_of(std::string_view uuid);
    VIZCORE_API AddedFixture* added_of(std::string_view uuid);
    VIZCORE_API bool is_removed(std::string_view uuid) const;
    bool empty() const { return fixtures.empty() && added.empty() && removed.empty(); }
};

// What is wanted when fixtures are added: `count` of one type, patched one after another from
// universe.address on (universe 0: left unpatched), each `footprint` slots long, numbered from
// `first_id` on (0: no fixture ids).
struct AddRequest {
    std::string gdtf;
    std::string mode;
    std::string name;      // each is called "<name> <n>"
    int count = 1;
    int universe = 0;
    int address = 1;
    int footprint = 1;
    int first_id = 0;
};

// Appends the fixtures to `edits` and returns their new UUIDs. They are set down in rows a metre
// above the floor in front of the stage, to be put in their places afterwards. A fixture that
// would run off the end of its universe starts the next one.
VIZCORE_API std::vector<std::string> add_fixtures(ShowEdits& edits, const AddRequest& request);

// Takes a fixture out: one that was added simply goes, one of the MVR is listed as removed and
// loses whatever else was changed about it. False if it was already out.
VIZCORE_API bool remove_fixture(ShowEdits& edits, std::string_view uuid);

// Puts a fixture of the MVR back in. False if it was not removed.
VIZCORE_API bool restore_fixture(ShowEdits& edits, std::string_view uuid);

// A new random identifier in the form MVR uses: 8-4-4-4-12 hexadecimal digits, upper case.
VIZCORE_API std::string new_uuid();

struct ShowEditsResult {
    bool ok = false;    // false only if the text is not a ShowEdits document at all
    ShowEdits edits;
    Diagnostics diagnostics;
};

// Entries that cannot be used (no UUID, a UUID seen before, numbers out of range) are dropped
// with a warning; the rest of the file still applies.
VIZCORE_API ShowEditsResult parse_show_edits(std::string_view xml);

// Entries with nothing changed are left out, and so is an edit to a fixture that is also removed.
// Numbers always use '.' whatever the host's locale.
VIZCORE_API std::string write_show_edits(const ShowEdits& edits);

// "rig.mvr" -> "rig.edits.xml", in the same folder.
VIZCORE_API std::string show_edits_path(std::string_view mvr_path);

}  // namespace viz
