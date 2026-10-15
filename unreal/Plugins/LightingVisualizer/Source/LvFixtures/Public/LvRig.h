#pragma once

#include "CoreMinimal.h"

class AActor;
class ALvFixtureActor;
class UWorld;

struct LVFIXTURES_API FLvRigReport
{
	int32 Spawned = 0;
	int32 Skipped = 0;
	/** Fixtures that a saved edit moved, re-patched or put in another mode. */
	int32 Edited = 0;
	/** Fixtures that are not in the rig file but were added in Openviz, and fixtures of the file that were taken out. */
	int32 Added = 0;
	int32 Removed = 0;
	/** The ids of the fixtures taken out: whoever saves the show's edits has to keep them. */
	TArray<FString> RemovedUuids;
	/** Fixtures the file hangs on another fixture's travelling part, which ride along with it. */
	int32 Riding = 0;
	/** Fixtures whose type is read from a file of the fixture library standing in for the rig file's own, and how many types that is. */
	int32 StoodIn = 0;
	int32 StoodInTypes = 0;
	/** Fixtures the rig file gives no place and that have not been given one since: built, and left dark. */
	int32 Unplaced = 0;
	/** One line per fixture that could not be built as described, and why. */
	TArray<FString> Messages;
};

/** What bringing a rig in line with its files did (LvRig::Reconcile). */
struct LVFIXTURES_API FLvRigChanges
{
	/** Fixtures that are new, among them those built again because their type is now read from another file. */
	TArray<ALvFixtureActor*> New;
	/** How many of those took the place of a fixture that stood before. */
	int32 Rebuilt = 0;
	/** Fixtures that stayed and were moved, re-patched, put in another mode, renamed or had their pan and tilt read another way. */
	int32 Changed = 0;
	/** Fixtures taken away. */
	int32 Gone = 0;
	bool Any() const { return New.Num() > 0 || Changed > 0 || Gone > 0; }
};

struct LVFIXTURES_API FLvSceneryReport
{
	/** Objects put on stage, and the model placements and triangles they are drawn with. */
	int32 Objects = 0;
	int32 Pieces = 0;
	int32 Triangles = 0;
	/** Objects the file lists that have nothing to draw them with, such as focus points. */
	int32 WithoutShape = 0;
	/** Objects the file hangs on a fixture's travelling part, which ride along with it. */
	int32 Riding = 0;
	TArray<FString> Messages;
};

class ALvSceneryActor;
class ALvScreenActor;

/** What Openviz has chosen for one screen of a show: kept beside the show as "<show>.screens.json". */
/** One section of a screen that is split into a grid: a screen of its own in all but its place. */
struct LVFIXTURES_API FLvScreenSection
{
	FString Source = TEXT("pattern");
	FString Value;
	float Nits = 1000.0f;
	/** The part of its picture it shows: left, top, right, bottom, as shares of the whole. */
	FVector4 Part = FVector4(0.0, 0.0, 1.0, 1.0);
};

struct LVFIXTURES_API FLvScreenSetting
{
	/** A video screen of the rig file, by its uuid; or a screen added in Openviz, by the id it was given. */
	FString Id;
	bool bAdded = false;
	/** Added screens only: its name, its size, the middle of its lower edge, and its turn about the vertical. Millimetres and degrees, the rig file's coordinates. */
	FString Name;
	double Width = 0.0;
	double Height = 0.0;
	double X = 0.0;
	double Y = 0.0;
	double Z = 0.0;
	double Turn = 0.0;
	/**
	 * Added screens that do not stand upright where numbers put them but lie where fixtures hang:
	 * the picture's right, up and out (unit vectors) and its middle, in the rig file's
	 * coordinates. Used instead of X, Y, Z and Turn.
	 */
	bool bFace = false;
	double Face[12] = {1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, -1.0, 0.0, 0.0, 0.0, 0.0};
	/** "pattern", "picture", "film", "ndi", "pixels", "fixtures" or "off", and the file, stream or pixel map that goes with it. */
	FString Source = TEXT("pattern");
	FString Value;
	float Nits = 1000.0f;
	/** The part of its picture it shows: left, top, right, bottom, as shares of the whole. */
	FVector4 Part = FVector4(0.0, 0.0, 1.0, 1.0);
	/**
	 * A screen split into a grid has Columns x Rows sections, row by row from the top left, and
	 * shows nothing itself. Otherwise all three are empty.
	 */
	int32 Columns = 0;
	int32 Rows = 0;
	TArray<FLvScreenSection> Sections;
	bool IsSplit() const { return Columns > 0 && Rows > 0 && Sections.Num() == Columns * Rows; }
	/** The fixtures (by their ids) this screen stands in for: wherever it shows anything but "fixtures", they are left out. */
	TArray<FString> Replaces;
};

/**
 * A set piece added in Openviz: a deck, a drape, a length of truss, a box, a figure
 * (vizcore/pieces.h has the kinds and makes the shapes). Kept beside the show as
 * "<show>.pieces.json". Millimetres and degrees, the rig file's coordinates: X, Y and Z are the
 * middle of its footprint at its foot, Turn is about the vertical.
 */
struct LVFIXTURES_API FLvPieceSetting
{
	FString Id;
	FString Kind = TEXT("box");
	FString Name;
	double X = 0.0;
	double Y = 0.0;
	double Z = 0.0;
	double Turn = 0.0;
	double Width = 1000.0;
	double Depth = 1000.0;
	double Height = 1000.0;
	FLinearColor Colour = FLinearColor(0.08f, 0.08f, 0.08f);
	/** Kind "model": its file (the whole path), what its numbers are and which axis is up (vizcore load_model_file). */
	FString ModelPath;
	FString Units;
	FString Up;
	/** A drape, box or deck: a picture on its side toward the house (the whole path), or empty. */
	FString Picture;
};

/** The id of a section's own screen: the screen's id, "#", and the section's number from 1. */
LVFIXTURES_API FString LvSectionId(const FString& ScreenId, int32 Index);

struct LVFIXTURES_API FLvScreenReport
{
	/** Screens of the rig file, and screens added in Openviz. */
	int32 FromRig = 0;
	int32 Added = 0;
	/** Sections of screens that are split, each a screen of its own beside those. */
	int32 Sections = 0;
	TArray<FString> Messages;
};

namespace LvRig
{
	/**
	 * Spawns what an MVR file has on stage besides fixtures: scene objects, trusses, supports,
	 * video screens and projectors, each drawn with the model files the archive packs for it.
	 * They take light and stop beams. Like the fixtures, they are transient.
	 */
	LVFIXTURES_API TArray<ALvSceneryActor*> SpawnScenery(UWorld& World, const FString& MvrPath, FLvSceneryReport& OutReport);

	/**
	 * Puts the set pieces added in Openviz on stage. Like the rig file's own scenery they take
	 * light and stop beams; like everything else of a show they are transient. A shape is built
	 * once for every kind, size and colour, however many pieces have it.
	 */
	LVFIXTURES_API TArray<ALvSceneryActor*> SpawnPieces(UWorld& World, const TArray<FLvPieceSetting>& Pieces);

	/**
	 * Spawns the picture of every video screen: those the MVR file has (on the face of the model
	 * it draws them with, toward the house) and those added in Openviz, each showing what
	 * `Settings` says, or a test pattern if nothing was chosen for it. `MvrPath` may be empty.
	 */
	LVFIXTURES_API TArray<ALvScreenActor*> SpawnScreens(UWorld& World, const FString& MvrPath, const TArray<FLvScreenSetting>& Settings,
		FLvScreenReport& OutReport);

	/**
	 * Spawns one ALvFixtureActor for every fixture in an MVR file that names a GDTF packed in the
	 * same archive, at its position in the rig and with its patch. Fixtures without a usable GDTF
	 * are skipped and reported.
	 *
	 * A fixture nested in another fixture's child list is placed at its parent's origin.
	 *
	 * If an edits file sits beside the MVR (see vizcore/show_edits.h), what it changes is laid
	 * over the MVR's own placement, patch and mode; fixtures it takes out are not built, and
	 * fixtures it adds are, from the GDTF files in `LibraryFolder` (Openviz's fixture library).
	 */
	LVFIXTURES_API TArray<ALvFixtureActor*> SpawnFromMvr(UWorld& World, const FString& MvrPath, FLvRigReport& OutReport,
		const FString& LibraryFolder = FString());

	/**
	 * Brings fixtures that stand already in line with what the show's files say now: the rig file,
	 * the edits beside it and the stand-in types beside it. A fixture is moved, re-patched, put
	 * in another mode or renamed where it stands; one whose type is now read from another file is
	 * built again; one the files no longer have is taken away; one they have gained is built.
	 * `Fixtures` comes back as the rig in the files' order. False, with nothing touched, if the
	 * rig file cannot be read (it may be half written: try again shortly).
	 */
	LVFIXTURES_API bool Reconcile(UWorld& World, const FString& MvrPath, const FString& LibraryFolder, TArray<ALvFixtureActor*>& Fixtures, FLvRigReport& OutReport,
		FLvRigChanges& OutChanges);

	/**
	 * Hangs `Rider` on the fixture with the id `ParentUuid`, on the part `ChildPosition` names or
	 * the part of it that travels, so that it rides along when DMX moves that part. Only while
	 * watching: in the editor a rider that moved by itself would be taken for an edit. Returns
	 * whether it was hung.
	 */
	LVFIXTURES_API bool HangOnFixture(AActor& Rider, const FString& ParentUuid, const FString& ChildPosition);

	/** Where the edits of the show in `MvrPath` are kept: beside it, as "<name>.edits.xml". */
	LVFIXTURES_API FString EditsPath(const FString& MvrPath);

	/**
	 * What differs between these fixtures as they are now and as their rig file described them,
	 * as the text of an edits file: changed, added and (from `RemovedUuids`) taken out.
	 * `OutEdited` is the number of fixtures that differ.
	 */
	LVFIXTURES_API FString DescribeEdits(const TArray<ALvFixtureActor*>& Fixtures, const TArray<FString>& RemovedUuids, int32& OutEdited);
}
