// A show as it will stand on stage: the fixtures of its MVR with what was changed since laid
// over them (vizcore/show_edits.h), the fixtures that were added, and the removed ones marked.
// One place for that, so that the visualizer, the patch list, the plot and the export agree.
#pragma once

#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "vizcore/gdtf.h"
#include "vizcore/mvr.h"
#include "vizcore/show_edits.h"
#include "vizcore/types.h"

namespace viz {

// A fixture type of the rig file that a file from the application's fixture library stands in for.
//
// Not every program that writes an MVR says what its fixtures can do. grandMA3 writes, for every
// type that comes from its own library, a "Dummy": the right number of channels in every mode, the
// dimmer named, and every other channel marked NoFeature; no colour, no gobos, no pan or tilt. A
// stand-in (the maker's own GDTF, say) tells what the channels do, while the rig file goes on
// saying where each fixture hangs, how it is patched and which mode it is in.
//
// Stand-ins are kept beside the show as "<show>.types.xml", apart from the edits: they change
// how a rig is read, not the rig, and nothing of them goes into an export.
//
//   <TypeSwaps Version="1">
//     <Type Spec="Martin@Mac Viper AirFX.gdtf" File="Martin Professional@MAC Viper AirFX@rev.gdtf"
//           TiltInvert="true" PanOffset="90">
//       <Mode From="Extended" To="Extended"/>
//     </Type>
//   </TypeSwaps>
//
// The same entry says how a type's pan and tilt are to be read where the file in force and the
// console disagree. Two descriptions of one fixture may run an axis opposite ways (GDTF leaves
// which side of a moving head is its front to whoever draws it, and the front decides which way
// tilt is positive), or start it from another angle. PanInvert and TiltInvert mirror the axis about
// the middle of its range, as MVR's own per-fixture DMXInvertPan does, and combine with that;
// PanOffset and TiltOffset, in degrees, are added to where the axis would stand. An entry may
// carry these without a File: they then apply to the rig file's own type.
struct TypeSwap {
    std::string type;                                          // the rig file's GDTFSpec, as written there
    std::string file;                                          // a file name in the fixture library, with its ending; empty: no stand-in
    std::vector<std::pair<std::string, std::string>> modes;    // the rig file's mode -> the stand-in's
    bool invert_pan = false;
    bool invert_tilt = false;
    double pan_offset = 0.0;                                   // degrees
    double tilt_offset = 0.0;
    DimmerCurve dimmer = DimmerCurve::Linear;                  // how its dimmer's level becomes light

    bool stands_in() const { return !file.empty(); }
    bool corrects() const { return invert_pan || invert_tilt || pan_offset != 0.0 || tilt_offset != 0.0 || dimmer != DimmerCurve::Linear; }
    VIZCORE_API const std::string* mode_for(std::string_view mode) const;    // null if the mode has no counterpart
    VIZCORE_API const std::string* mode_back(std::string_view mode) const;   // the other way, for an export
};

struct TypeSwaps {
    std::vector<TypeSwap> swaps;

    // Capitals do not count, nor whether the name is written with its ".gdtf". An entry found
    // may be there for its stand-in, for its pan and tilt corrections, or for both.
    VIZCORE_API const TypeSwap* find(std::string_view gdtf_spec) const;
    // The entry for a type that has a stand-in, else null.
    VIZCORE_API const TypeSwap* stand_in(std::string_view gdtf_spec) const;
    // Puts an entry in the place of the one the type had; and takes the type's entry away (false if it had none).
    VIZCORE_API void set(TypeSwap swap);
    VIZCORE_API bool remove(std::string_view gdtf_spec);
    // Sets a type's stand-in and leaves its pan and tilt corrections as they are; and takes the
    // stand-in away, keeping the entry only if it still corrects something (false if it had none).
    VIZCORE_API void set_stand_in(std::string_view gdtf_spec, std::string file, std::vector<std::pair<std::string, std::string>> modes);
    VIZCORE_API bool clear_stand_in(std::string_view gdtf_spec);
    // Sets a type's pan and tilt corrections and leaves its stand-in and dimmer curve as they are.
    VIZCORE_API void correct(std::string_view gdtf_spec, bool invert_pan, bool invert_tilt, double pan_offset, double tilt_offset);
    // Sets a type's dimmer curve and leaves the rest as it is.
    VIZCORE_API void set_dimmer_curve(std::string_view gdtf_spec, DimmerCurve curve);
    bool empty() const { return swaps.empty(); }
};

// Whether two GDTFSpecs name the same type: capitals do not count, nor a ".gdtf" at the end.
VIZCORE_API bool same_type(std::string_view a, std::string_view b);

// Entries without a type, or with neither a file nor a correction, are left out.
VIZCORE_API TypeSwaps parse_type_swaps(std::string_view xml);
VIZCORE_API std::string write_type_swaps(const TypeSwaps& swaps);
// "rig.mvr" -> "rig.types.xml", in the same folder.
VIZCORE_API std::string type_swaps_path(std::string_view mvr_path);

// Which mode of `stand_in` each mode of `original` is, for those that have one: a mode stands for
// another only if it takes exactly as many slots, on every break, since that is all a placeholder
// reliably says. Among several of the same length the one of the same name wins, then the one
// whose name has most words in common. Pairs are (the original's mode, the stand-in's).
VIZCORE_API std::vector<std::pair<std::string, std::string>> match_modes(const FixtureType& original, const FixtureType& stand_in);

// A check on a stand-in beyond its length. A placeholder still says where its dimmers are, so a
// mode that stands in for it should have a dimmer on one of those slots at least; if it has none
// there, its channels are not in the console's order. 1: it has. 0: it has not. -1: the
// placeholder names no dimmer, or a mode is not there: nothing to go by.
VIZCORE_API int dimmers_agree(const FixtureType& original, size_t mode_index, const FixtureType& stand_in, size_t stand_in_mode);

// How much a mode says about its channels: how many it has, and how many of them are NoFeature
// through and through. `says_little` is true when the silent ones are more than half, or all but one.
struct ModeSilence {
    size_t channels = 0;
    size_t silent = 0;
    bool says_little() const { return silent > 0 && (silent * 2 > channels || channels - silent <= 1); }
};
VIZCORE_API ModeSilence mode_silence(const FixtureType& type, size_t mode_index);

struct RigFixture {
    std::string uuid;
    std::string name;
    std::string fixture_id;
    std::string layer;
    std::string class_name;
    std::string gdtf;             // from the MVR: its GDTFSpec as written; added, stood in for or retyped: a file name in the fixture library
    bool from_library = false;    // true for an added fixture, one whose type a library file stands in for, and one retyped
    std::string stood_in_for;     // the MVR's own GDTFSpec, when a library file stands in for it; else empty
    std::string mode;             // as the MVR or an edit names it; whether the type has it is for the caller to check
    std::string file_mode;        // the mode before any edit: the MVR's, or its counterpart in a stand-in
    Transform world;
    int universe = 0;             // the patch of its first DMX break; 0 = unpatched
    int address = 1;
    // Whether it has a place of its own: false for a fixture the rig file writes no matrix for
    // and that has not been given a place since. Such fixtures all stand in one spot; whoever
    // draws the rig should leave them dark and say so, not light them on top of each other.
    bool placed = true;
    // Pan and tilt as they are to be read: the file's own inversion (or an edit's in its place)
    // combined with what the type's entry says, and the type's offsets in degrees.
    bool invert_pan = false;
    bool invert_tilt = false;
    double pan_offset = 0.0;
    double tilt_offset = 0.0;
    DimmerCurve dimmer = DimmerCurve::Linear;    // its type's dimmer curve in this show
    bool own_invert_pan = false;  // the fixture's own inversion alone, as the file or an edit has it
    bool own_invert_tilt = false;

    bool moved = false;           // what an edit changed
    bool repatched = false;
    bool mode_changed = false;
    bool renamed = false;         // its name or its number
    bool reinverted = false;      // its own pan or tilt inversion
    bool retyped = false;         // given a type of its own from the fixture library (no stand-in, no type corrections then)
    bool added = false;
    bool removed = false;         // still listed, so that it can be shown as removed and put back

    const MvrFixture* source = nullptr;          // the MVR's own description; null for an added fixture
    const MvrAddress* first_patch = nullptr;     // which of the source's addresses is the first break's; null if it has none
};

// Fixtures of the MVR first, in the file's order, then the added ones. `scene` must outlive the result.
// With `swaps`, a fixture whose type has a stand-in is given the stand-in's file and the mode that
// stands for its own; a mode an edit chose is taken to name a mode of the stand-in.
VIZCORE_API std::vector<RigFixture> merge_rig(const MvrScene& scene, const ShowEdits& edits, const TypeSwaps& swaps = TypeSwaps());

// Where a stand-in's file, or a retyped fixture's, is not in `library_dir` (it was deleted, or the
// show came from another computer), the fixtures go back to the type their rig file gives them.
// Returns the files missed.
VIZCORE_API std::vector<std::string> drop_missing_stand_ins(std::vector<RigFixture>& rig, const std::string& library_dir);

// The edits as an export should write them: a mode chosen while a stand-in was in force is a mode
// of the stand-in, and goes back into the rig file as the mode it stands for, or not at all.
VIZCORE_API ShowEdits edits_for_export(const MvrScene& scene, const ShowEdits& edits, const TypeSwaps& swaps);

}  // namespace viz
