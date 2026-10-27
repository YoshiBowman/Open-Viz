#include "LvRig.h"

#include "Engine/World.h"
#include "EngineUtils.h"
#include "LvCoordinates.h"
#include "LvFixtureActor.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "LvFixtureLibrary.h"
#include "LvSceneryActor.h"
#include "LvScreenActor.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "vizcore/mesh.h"
#include "vizcore/archive.h"
#include "vizcore/mvr.h"
#include "vizcore/pieces.h"
#include "vizcore/rig.h"
#include "vizcore/screens.h"
#include "vizcore/show_edits.h"

namespace
{
	/** A show as its files have it now: the rig file with the saved edits and the stand-in types laid over it. */
	struct FRigPlan
	{
		viz::Archive Archive;
		viz::MvrResult Scene;
		viz::ShowEdits Edits;
		std::vector<viz::RigFixture> Rig;    // points into Scene: the plan must not be moved once read
	};

	bool ReadPlan(const FString& MvrPath, const FString& LibraryFolder, FRigPlan& Plan, FLvRigReport& OutReport)
	{
		std::string ArchiveError;
		if (!Plan.Archive.open_file(LvToStd(MvrPath), ArchiveError))
		{
			OutReport.Messages.Add(FString::Printf(TEXT("%s: %s"), *MvrPath, *LvToFString(ArchiveError)));
			return false;
		}
		Plan.Scene = viz::load_mvr(Plan.Archive);
		if (!Plan.Scene.ok)
		{
			OutReport.Messages.Add(FString::Printf(TEXT("%s is not a usable MVR"), *MvrPath));
			return false;
		}

		// What was changed since the rig left the console or the drawing program.
		FString EditsText;
		if (FFileHelper::LoadFileToString(EditsText, *LvRig::EditsPath(MvrPath)))
		{
			viz::ShowEditsResult Parsed = viz::parse_show_edits(LvToStd(EditsText));
			if (!Parsed.ok)
			{
				OutReport.Messages.Add(TEXT("the saved edits cannot be read and were not applied"));
			}
			Plan.Edits = MoveTemp(Parsed.edits);
		}
		for (const std::string& Uuid : Plan.Edits.removed)
		{
			OutReport.RemovedUuids.Add(LvToFString(Uuid));
		}

		// Fixture files of the library that stand in for types the rig file only has placeholders for
		// (a console's "Dummy" types: the channels counted, and nothing said about them), and how
		// each type's pan and tilt are to be read.
		viz::TypeSwaps Swaps;
		FString SwapsText;
		if (FFileHelper::LoadFileToString(SwapsText, *LvToFString(viz::type_swaps_path(LvToStd(MvrPath)))))
		{
			Swaps = viz::parse_type_swaps(LvToStd(SwapsText));
		}

		// The rig as it stands: the file's fixtures with the edits laid over them, then the added ones.
		Plan.Rig = viz::merge_rig(Plan.Scene.scene, Plan.Edits, Swaps);
		for (const std::string& Missing : viz::drop_missing_stand_ins(Plan.Rig, LvToStd(LibraryFolder)))
		{
			OutReport.Messages.Add(FString::Printf(TEXT("%s was to stand in for a fixture type of this show, but is not in the fixture library: the rig file's own is used"),
				*LvToFString(Missing)));
		}
		return true;
	}

	/** What one fixture of a plan is to be built from. */
	struct FRigType
	{
		TSharedPtr<const FLvFixtureDefinition> Definition;
		FString Label;         // the type's file, for messages
		FString Key;           // which file that is, and where from: a fixture whose key changes is built anew
		int32 Mode = 0;        // the mode it is to be in
		FString SourceMode;    // the mode the rig file (or, for an added fixture, its entry) names: what an edit is measured against
	};

	bool FindType(FRigPlan& Plan, const viz::RigFixture& Wanted, const FString& MvrPath, const FString& LibraryFolder, FRigType& Out, FLvRigReport& OutReport)
	{
		ULvFixtureLibrary* Library = ULvFixtureLibrary::Get();
		const FString Name = LvToFString(Wanted.name);
		// Many fixtures share one GDTF; the library parses each only once.
		FString Error;
		Out.Label = LvToFString(Wanted.gdtf);
		if (!Library)
		{
			Error = TEXT("the fixture library is not there");
		}
		else if (Wanted.from_library)
		{
			if (LibraryFolder.IsEmpty())
			{
				Error = TEXT("its fixture type is a file of Openviz's fixture library, and where that is kept was not given");
			}
			else
			{
				Out.Definition = Library->LoadGdtfFile(LibraryFolder / Out.Label, Error);
			}
			Out.Key = TEXT("library|") + Out.Label;
		}
		else
		{
			const std::string Entry = viz::find_gdtf_entry(Plan.Archive, Wanted.gdtf);
			std::vector<uint8_t> Bytes;
			if (Entry.empty())
			{
				Error = FString::Printf(TEXT("no GDTF '%s' in the archive"), *Out.Label);
			}
			else if (Plan.Archive.read(Entry, Bytes))
			{
				Out.Label = LvToFString(Entry);
				Out.Definition = Library->LoadGdtfBytes(MvrPath + TEXT("|") + Out.Label, TArray<uint8>(Bytes.data(), static_cast<int32>(Bytes.size())), Error);
			}
			Out.Key = TEXT("file|") + Out.Label;
		}
		if (!Out.Definition)
		{
			OutReport.Messages.Add(FString::Printf(TEXT("%s: %s"), *Name, *Error));
			return false;
		}
		// The mode the file (or, for an added fixture, its entry) names, or what stands for it in a
		// stand-in; an added fixture may leave it to the type's first.
		const std::string& NamedMode = Wanted.file_mode;
		Out.Mode = NamedMode.empty() && (Wanted.added || Wanted.retyped) ? 0 : Out.Definition->FindMode(LvToFString(NamedMode));
		if (Out.Mode < 0)
		{
			OutReport.Messages.AddUnique(FString::Printf(TEXT("%s: no mode '%s' in %s; the first mode is used"), *Name, *LvToFString(NamedMode), *Out.Label));
			Out.Mode = 0;
		}
		// The file's own description is kept on the fixture; a saved edit is laid over it.
		Out.SourceMode = LvToFString(Out.Definition->Type.modes[static_cast<size_t>(Out.Mode)].name);
		if (Wanted.mode_changed)
		{
			const int32 Edited = Out.Definition->FindMode(LvToFString(Wanted.mode));
			if (Edited >= 0)
			{
				Out.Mode = Edited;
			}
			else
			{
				OutReport.Messages.AddUnique(FString::Printf(TEXT("%s: the mode '%s' chosen for it no longer exists; '%s' is used"), *Name,
					*LvToFString(Wanted.mode), *Out.SourceMode));
			}
		}
		return true;
	}

	FTransform PlaceOf(const viz::Transform& World)
	{
		// Position and orientation only: a fixture is never scaled.
		FTransform Placement = LvCoordinates::FromMvr(World);
		Placement.SetScale3D(FVector::OneVector);
		return Placement;
	}

	/**
	 * Makes a fixture that is built from the right type what the plan says in every other respect:
	 * where it stands, its patch and mode, how its pan and tilt are read, what it is called, and
	 * what the rig file itself says of it, which edits are measured against. True if anything
	 * that shows was different.
	 */
	bool Describe(ALvFixtureActor& Fixture, const viz::RigFixture& Wanted, const FRigType& Type)
	{
		bool bChanged = false;
		const FTransform Placement = PlaceOf(Wanted.world);
		const FTransform Now = Fixture.GetActorTransform();
		if (!Now.GetLocation().Equals(Placement.GetLocation(), 0.005) || FMath::RadiansToDegrees(Now.GetRotation().AngularDistance(Placement.GetRotation())) > 0.01)
		{
			Fixture.SetActorTransform(Placement);
			bChanged = true;
		}
		const FString ModeName = LvToFString(Type.Definition->Type.modes[static_cast<size_t>(Type.Mode)].name);
		if (Fixture.Mode != ModeName)
		{
			Fixture.SetMode(ModeName);
			bChanged = true;
		}
		if (Fixture.GetUniverse() != Wanted.universe || Fixture.GetAddress() != Wanted.address)
		{
			// Universe 0 is never received, which is right for a fixture that is left unpatched.
			Fixture.SetBreakPatch(1, Wanted.universe, Wanted.address);
			bChanged = true;
		}
		const bool bTypePan = Wanted.invert_pan != Wanted.own_invert_pan;
		const bool bTypeTilt = Wanted.invert_tilt != Wanted.own_invert_tilt;
		const uint8 Curve = static_cast<uint8>(Wanted.dimmer);
		bChanged = bChanged || Fixture.bInvertPan != Wanted.own_invert_pan || Fixture.bInvertTilt != Wanted.own_invert_tilt || Fixture.bTypeInvertPan != bTypePan ||
			Fixture.bTypeInvertTilt != bTypeTilt || Fixture.PanOffset != Wanted.pan_offset || Fixture.TiltOffset != Wanted.tilt_offset || Fixture.TypeDimmerCurve != Curve;
		Fixture.TypeDimmerCurve = Curve;
		Fixture.bInvertPan = Wanted.own_invert_pan;
		Fixture.bInvertTilt = Wanted.own_invert_tilt;
		Fixture.bTypeInvertPan = bTypePan;
		Fixture.bTypeInvertTilt = bTypeTilt;
		Fixture.PanOffset = Wanted.pan_offset;
		Fixture.TiltOffset = Wanted.tilt_offset;

		const FString Name = LvToFString(Wanted.name);
		const FString Shown = Wanted.fixture_id.empty() ? Name : FString::Printf(TEXT("%s %s"), *LvToFString(Wanted.fixture_id), *Name);
		if (Fixture.FixtureName != Shown)
		{
			Fixture.FixtureName = Shown;
			bChanged = true;
#if WITH_EDITOR
			Fixture.SetActorLabel(Shown, false);    // the name the editor's outliner lists it under
#endif
		}
		Fixture.ShowName = Name;
		Fixture.ShowFixtureId = LvToFString(Wanted.fixture_id);
		Fixture.Layer = LvToFString(Wanted.layer);
		Fixture.Class = LvToFString(Wanted.class_name);
		Fixture.SourceUuid = LvToFString(Wanted.uuid);
		Fixture.TypeKey = Type.Key;
		Fixture.SourceMode = Type.SourceMode;
		Fixture.OwnGdtf = Wanted.retyped ? LvToFString(Wanted.gdtf) : FString();
		if (Wanted.source)
		{
			Fixture.SourceTransform = PlaceOf(Wanted.source->world);
			Fixture.SourceUniverse = Wanted.first_patch ? Wanted.first_patch->universe() : 0;
			Fixture.SourceAddress = Wanted.first_patch && Wanted.first_patch->universe() > 0 ? Wanted.first_patch->address() : 1;
			Fixture.bSourceInvertPan = Wanted.source->dmx_invert_pan;
			Fixture.bSourceInvertTilt = Wanted.source->dmx_invert_tilt;
			Fixture.bSourcePlaced = Wanted.source->placed;
			Fixture.SourceName = LvToFString(Wanted.source->name);
			Fixture.SourceFixtureId = LvToFString(Wanted.source->fixture_id);
			Fixture.bAdded = false;
		}
		else
		{
			Fixture.SourceTransform = Placement;
			Fixture.SourceUniverse = Wanted.universe;
			Fixture.SourceAddress = Wanted.address;
			Fixture.bSourceInvertPan = Wanted.own_invert_pan;
			Fixture.bSourceInvertTilt = Wanted.own_invert_tilt;
			Fixture.bSourcePlaced = true;
			Fixture.SourceName = Name;
			Fixture.SourceFixtureId = LvToFString(Wanted.fixture_id);
			Fixture.bAdded = true;
			Fixture.AddedGdtf = LvToFString(Wanted.gdtf);
			Fixture.AddedName = Name;
			Fixture.AddedFixtureId = LvToFString(Wanted.fixture_id);
			Fixture.AddedWorld = Wanted.world;
		}
		if (Fixture.IsUnplaced() == Wanted.placed)
		{
			Fixture.SetUnplaced(!Wanted.placed);
			bChanged = true;
		}
		return bChanged;
	}

	ALvFixtureActor* Build(UWorld& World, const viz::RigFixture& Wanted, const FRigType& Type, FLvRigReport& OutReport)
	{
		// A rig is rebuilt from its file each time it is opened; it never belongs in a saved level.
		FActorSpawnParameters Spawn;
		Spawn.ObjectFlags |= RF_Transient;
		ALvFixtureActor* Fixture = World.SpawnActor<ALvFixtureActor>(ALvFixtureActor::StaticClass(), PlaceOf(Wanted.world), Spawn);
		if (!Fixture || !Fixture->Initialize(Type.Definition, Type.Mode, Wanted.universe, Wanted.address))
		{
			OutReport.Messages.Add(FString::Printf(TEXT("%s: could not be built from %s"), *LvToFString(Wanted.name), *Type.Label));
			if (Fixture)
			{
				Fixture->Destroy();
			}
			return nullptr;
		}
		if (Wanted.source)
		{
			// MVR counts a fixture's breaks from 0, GDTF from 1. The first is the patch above; the others
			// are as the file has them, and are not among what can be edited.
			for (const viz::MvrAddress& Patch : Wanted.source->addresses)
			{
				if (&Patch != Wanted.first_patch && Patch.break_id >= 1 && Patch.universe() > 0)
				{
					Fixture->SetBreakPatch(Patch.break_id + 1, Patch.universe(), Patch.address());
				}
			}
		}
		Describe(*Fixture, Wanted, Type);
		return Fixture;
	}

	void Count(const viz::RigFixture& Wanted, FLvRigReport& OutReport, TSet<FString>& StoodIn)
	{
		if (!Wanted.stood_in_for.empty())
		{
			++OutReport.StoodIn;
			StoodIn.Add(LvToFString(Wanted.stood_in_for));
		}
		if (Wanted.moved || Wanted.repatched || Wanted.mode_changed || Wanted.renamed || Wanted.reinverted || Wanted.retyped)
		{
			++OutReport.Edited;
		}
		OutReport.Added += Wanted.added ? 1 : 0;
		OutReport.Unplaced += Wanted.placed ? 0 : 1;
		++OutReport.Spawned;
	}
}

TArray<ALvFixtureActor*> LvRig::SpawnFromMvr(UWorld& World, const FString& MvrPath, FLvRigReport& OutReport, const FString& LibraryFolder)
{
	TArray<ALvFixtureActor*> Fixtures;
	FLvRigChanges Unused;
	Reconcile(World, MvrPath, LibraryFolder, Fixtures, OutReport, Unused);
	return Fixtures;
}

bool LvRig::Reconcile(UWorld& World, const FString& MvrPath, const FString& LibraryFolder, TArray<ALvFixtureActor*>& Fixtures, FLvRigReport& OutReport,
	FLvRigChanges& OutChanges)
{
	const TUniquePtr<FRigPlan> Plan = MakeUnique<FRigPlan>();
	if (!ReadPlan(MvrPath, LibraryFolder, *Plan, OutReport))
	{
		return false;    // a file half written, or gone: what stands, stands
	}
	// The fixtures there are, by the id the files know them by.
	TMap<FString, ALvFixtureActor*> Standing;
	for (ALvFixtureActor* Fixture : Fixtures)
	{
		if (Fixture && !Fixture->SourceUuid.IsEmpty())
		{
			Standing.Add(Fixture->SourceUuid.ToUpper(), Fixture);
		}
	}
	TArray<ALvFixtureActor*> Now;
	TSet<FString> StoodIn;
	TArray<TPair<ALvFixtureActor*, const viz::MvrFixture*>> Riders;
	for (const viz::RigFixture& Wanted : Plan->Rig)
	{
		ALvFixtureActor* Fixture = nullptr;
		Standing.RemoveAndCopyValue(LvToFString(Wanted.uuid).ToUpper(), Fixture);
		if (Wanted.removed)
		{
			++OutReport.Removed;
			if (Fixture)
			{
				Fixture->Destroy();
				++OutChanges.Gone;
			}
			continue;
		}
		FRigType Type;
		if (!FindType(*Plan, Wanted, MvrPath, LibraryFolder, Type, OutReport))
		{
			++OutReport.Skipped;
			if (Fixture)
			{
				Fixture->Destroy();
				++OutChanges.Gone;
			}
			continue;
		}
		if (Fixture && Fixture->TypeKey == Type.Key)
		{
			OutChanges.Changed += Describe(*Fixture, Wanted, Type) ? 1 : 0;
		}
		else
		{
			// New to the show, or read from another fixture file than before: built from nothing.
			if (Fixture)
			{
				Fixture->Destroy();
				++OutChanges.Rebuilt;
			}
			Fixture = Build(World, Wanted, Type, OutReport);
			if (!Fixture)
			{
				++OutReport.Skipped;
				continue;
			}
			OutChanges.New.Add(Fixture);
			if (Wanted.source && !Wanted.source->parent_uuid.empty())
			{
				Riders.Emplace(Fixture, Wanted.source);
			}
		}
		Count(Wanted, OutReport, StoodIn);
		Now.Add(Fixture);
	}
	// Whatever the files no longer have: an added fixture that was taken out again.
	for (const TPair<FString, ALvFixtureActor*>& Left : Standing)
	{
		Left.Value->Destroy();
		++OutChanges.Gone;
	}
	OutReport.StoodInTypes = StoodIn.Num();
	Fixtures = MoveTemp(Now);
	// A fixture the file hangs on another rides along with the part it hangs on.
	for (const TPair<ALvFixtureActor*, const viz::MvrFixture*>& Rider : Riders)
	{
		OutReport.Riding += HangOnFixture(*Rider.Key, LvToFString(Rider.Value->parent_uuid), LvToFString(Rider.Value->child_position)) ? 1 : 0;
	}
	return true;
}

TArray<ALvSceneryActor*> LvRig::SpawnPieces(UWorld& World, const TArray<FLvPieceSetting>& Pieces)
{
	TArray<ALvSceneryActor*> Spawned;
	ULvFixtureLibrary* Library = ULvFixtureLibrary::Get();
	if (!Library)
	{
		return Spawned;
	}
	FActorSpawnParameters Spawn;
	Spawn.ObjectFlags |= RF_Transient;
	for (const FLvPieceSetting& Setting : Pieces)
	{
		viz::SetPiece Piece = viz::default_piece(LvToStd(Setting.Kind));
		Piece.width = Setting.Width / 1000.0;
		Piece.depth = Setting.Depth / 1000.0;
		Piece.height = Setting.Height / 1000.0;
		Piece.color[0] = Setting.Colour.R;
		Piece.color[1] = Setting.Colour.G;
		Piece.color[2] = Setting.Colour.B;
		Piece.picture = LvToStd(Setting.Picture);
		// One shape for every piece of this kind, size and colour; a model's file once for each way it is read.
		UStaticMesh* Mesh = nullptr;
		if (Setting.Kind == TEXT("model"))
		{
			const FString Key = FString::Printf(TEXT("model|%s|%s|%s|%s"), *Setting.ModelPath, *Setting.Units, *Setting.Up,
				*IFileManager::Get().GetTimeStamp(*Setting.ModelPath).ToString());
			Mesh = Library->GetSceneryMesh(Key, [&Setting]()
			{
				return viz::load_model_file(LvToStd(Setting.ModelPath), LvToStd(Setting.Units), LvToStd(Setting.Up));
			});
		}
		else
		{
			const FString Key = FString::Printf(TEXT("piece|%s|%.0f|%.0f|%.0f|%.3f|%.3f|%.3f|%s|%s"), *LvToFString(Piece.kind), Setting.Width, Setting.Depth, Setting.Height,
				Setting.Colour.R, Setting.Colour.G, Setting.Colour.B, *Setting.Picture,
				Setting.Picture.IsEmpty() ? TEXT("") : *IFileManager::Get().GetTimeStamp(*Setting.Picture).ToString());
			Mesh = Library->GetSceneryMesh(Key, [&Piece]()
			{
				viz::MeshResult Made;
				Made.mesh = viz::dressed_piece_mesh(Piece);
				Made.ok = Made.mesh.triangle_count() > 0;
				return Made;
			});
		}
		if (!Mesh)
		{
			continue;
		}
		// Turned about the vertical, anticlockwise seen from above as the rig file's angles run, and set down at its place.
		viz::Transform Place;
		const double Angle = FMath::DegreesToRadians(Setting.Turn);
		Place.u[0] = FMath::Cos(Angle);
		Place.u[1] = FMath::Sin(Angle);
		Place.v[0] = -FMath::Sin(Angle);
		Place.v[1] = FMath::Cos(Angle);
		Place.o[0] = Setting.X;
		Place.o[1] = Setting.Y;
		Place.o[2] = Setting.Z;
		ALvSceneryActor* Actor = World.SpawnActor<ALvSceneryActor>(ALvSceneryActor::StaticClass(), LvCoordinates::FromMvr(Place), Spawn);
		if (!Actor)
		{
			continue;
		}
		Actor->SceneryName = Setting.Name.IsEmpty() ? Setting.Kind : Setting.Name;
		Actor->Kind = TEXT("Set piece");
		Actor->Layer = TEXT("Added in Openviz");
		Actor->PieceId = Setting.Id;
		UStaticMeshComponent* Component = NewObject<UStaticMeshComponent>(Actor);
		Component->SetMobility(EComponentMobility::Movable);
		Component->SetStaticMesh(Mesh);
		Component->SetCollisionProfileName(TEXT("BlockAll"));    // beams end where they land on it
		Component->SetupAttachment(Actor->GetRootComponent());
		Component->RegisterComponent();
		Actor->AddInstanceComponent(Component);
#if WITH_EDITOR
		Actor->SetActorLabel(Actor->SceneryName, false);
		Actor->SetFolderPath(FName(TEXT("Scenery/Added in Openviz")));
#endif
		Spawned.Add(Actor);
	}
	return Spawned;
}

bool LvRig::HangOnFixture(AActor& Rider, const FString& ParentUuid, const FString& ChildPosition)
{
	UWorld* World = Rider.GetWorld();
	if (!World || !World->IsGameWorld() || ParentUuid.IsEmpty())
	{
		return false;
	}
	for (TActorIterator<ALvFixtureActor> Fixture(World); Fixture; ++Fixture)
	{
		if (*Fixture == &Rider || !Fixture->SourceUuid.Equals(ParentUuid, ESearchCase::IgnoreCase))
		{
			continue;
		}
		USceneComponent* Carrier = Fixture->GetCarrier(ChildPosition);
		if (!Carrier)
		{
			return false;    // nothing on that fixture travels: the rider is where it belongs already
		}
		// Whatever is carried about has to be free to move, all of it.
		TInlineComponentArray<USceneComponent*> Parts(&Rider);
		for (USceneComponent* Part : Parts)
		{
			Part->SetMobility(EComponentMobility::Movable);
		}
		Rider.AttachToComponent(Carrier, FAttachmentTransformRules::KeepWorldTransform);
		return true;
	}
	return false;
}

TArray<ALvSceneryActor*> LvRig::SpawnScenery(UWorld& World, const FString& MvrPath, FLvSceneryReport& OutReport)
{
	TArray<ALvSceneryActor*> Scenery;
	ULvFixtureLibrary* Library = ULvFixtureLibrary::Get();
	viz::Archive Archive;
	std::string ArchiveError;
	if (!Library || !Archive.open_file(LvToStd(MvrPath), ArchiveError))
	{
		return Scenery;
	}
	const viz::MvrResult Scene = viz::load_mvr(Archive);
	if (!Scene.ok)
	{
		return Scenery;
	}

	FActorSpawnParameters Spawn;
	Spawn.ObjectFlags |= RF_Transient;
	for (const viz::MvrObject& Described : Scene.scene.objects)
	{
		if (Described.geometry.empty())
		{
			++OutReport.WithoutShape;
			continue;
		}
		FTransform Placement = LvCoordinates::FromMvr(Described.world);
		ALvSceneryActor* Actor = World.SpawnActor<ALvSceneryActor>(ALvSceneryActor::StaticClass(), Placement, Spawn);
		if (!Actor)
		{
			continue;
		}
		// Unlike a fixture, a piece of scenery may be drawn larger or smaller than its model.
		Actor->SetActorScale3D(Placement.GetScale3D());
		Actor->SceneryName = LvToFString(Described.name);
		Actor->Kind = LvToFString(viz::to_string(Described.kind));
		Actor->Layer = LvToFString(Described.layer);
		Actor->Class = LvToFString(Scene.scene.class_name(Described.classing));

		int32 Drawn = 0;
		for (const viz::MvrGeometry& Piece : Described.geometry)
		{
			const FString Key = MvrPath + TEXT("|") + LvToFString(Piece.file);
			UStaticMesh* Mesh = Library->GetSceneryMesh(Key, [&Archive, &Piece]() { return viz::load_scene_mesh(Archive, Piece.file); });
			if (!Mesh)
			{
				OutReport.Messages.AddUnique(FString::Printf(TEXT("the model file %s could not be read"), *LvToFString(Piece.file)));
				continue;
			}
			UStaticMeshComponent* Component = NewObject<UStaticMeshComponent>(Actor);
			Component->SetMobility(EComponentMobility::Movable);
			Component->SetStaticMesh(Mesh);
			Component->SetCollisionProfileName(TEXT("BlockAll"));    // beams end where they land on it
			Component->SetupAttachment(Actor->GetRootComponent());
			Component->SetRelativeTransform(LvCoordinates::FromMvr(Piece.local));
			Component->RegisterComponent();
			Actor->AddInstanceComponent(Component);
			++Drawn;
			++OutReport.Pieces;
			if (Mesh->GetRenderData() && Mesh->GetRenderData()->LODResources.Num() > 0)
			{
				OutReport.Triangles += Mesh->GetRenderData()->LODResources[0].GetNumTriangles();
			}
		}
		if (Drawn == 0)
		{
			Actor->Destroy();
			++OutReport.WithoutShape;
			continue;
		}
#if WITH_EDITOR
		const FString Label = Actor->SceneryName.IsEmpty() || Actor->SceneryName == TEXT("Geometry")
			? (Actor->Class.IsEmpty() ? Actor->Kind : Actor->Class) : Actor->SceneryName;
		Actor->SetActorLabel(Label, false);
		Actor->SetFolderPath(FName(*(FString(TEXT("Scenery/")) + (Actor->Layer.IsEmpty() ? TEXT("Unnamed layer") : Actor->Layer))));
#endif
		Scenery.Add(Actor);
		++OutReport.Objects;
		// What the file hangs on a fixture that travels (a winch, a hoist, a turntable) rides along.
		OutReport.Riding += HangOnFixture(*Actor, LvToFString(Described.parent_fixture), LvToFString(Described.child_position)) ? 1 : 0;
	}
	return Scenery;
}

FString LvSectionId(const FString& ScreenId, int32 Index)
{
	return FString::Printf(TEXT("%s#%d"), *ScreenId, Index + 1);
}

TArray<ALvScreenActor*> LvRig::SpawnScreens(UWorld& World, const FString& MvrPath, const TArray<FLvScreenSetting>& Settings, FLvScreenReport& OutReport)
{
	TArray<ALvScreenActor*> Screens;
	FActorSpawnParameters Spawn;
	Spawn.ObjectFlags |= RF_Transient;

	auto Put = [&](const viz::ScreenFace& Face, const FString& Id, const FString& Name, bool bStopsBeams) -> ALvScreenActor*
	{
		// Across the picture, down it, and out of it toward whoever looks; the rig file's Y is mirrored on the way in.
		const FVector Right(Face.world.u[0], -Face.world.u[1], Face.world.u[2]);
		const FVector Down(-Face.world.v[0], Face.world.v[1], -Face.world.v[2]);
		const FVector Out(Face.world.w[0], -Face.world.w[1], Face.world.w[2]);
		const FVector Middle(Face.world.o[0] / 10.0, -Face.world.o[1] / 10.0, Face.world.o[2] / 10.0);
		const FTransform Placement(FMatrix(Right, Down, Out, Middle));
		ALvScreenActor* Actor = World.SpawnActor<ALvScreenActor>(ALvScreenActor::StaticClass(), Placement, Spawn);
		if (!Actor)
		{
			return nullptr;
		}
		Actor->ScreenId = Id;
		Actor->ScreenName = Name;
		Actor->Layer = LvToFString(Face.layer);
		Actor->Class = LvToFString(Face.class_name);
		Actor->Build(Face.width / 10.0, Face.height / 10.0, bStopsBeams);
#if WITH_EDITOR
		Actor->SetActorLabel(Name.IsEmpty() ? TEXT("Screen") : Name);
		Actor->SetFolderPath(TEXT("Screens"));
#endif
		Screens.Add(Actor);
		return Actor;
	};

	// What a screen was set to show; and, for one split into a grid, a screen for each section
	// instead, each on its own piece of the face. Returns the sections' screens.
	auto Dress = [&](ALvScreenActor& Whole, const viz::ScreenFace& Face, const FLvScreenSetting& Setting, bool bStopsBeams) -> TArray<ALvScreenActor*>
	{
		TArray<ALvScreenActor*> Pieces;
		if (!Setting.IsSplit())
		{
			Whole.Show(Setting.Source, Setting.Value, Setting.Nits);
			Whole.SetPart(Setting.Part);
			Whole.SetMap(Setting.bMap, Setting.Map);
			return Pieces;
		}
		Whole.Show(TEXT("sections"), FString(), 0.0f);
		for (int32 Index = 0; Index < Setting.Sections.Num(); ++Index)
		{
			const int32 Column = Index % Setting.Columns;
			const int32 Row = Index / Setting.Columns;
			viz::ScreenFace Piece = Face;
			Piece.width = Face.width / Setting.Columns;
			Piece.height = Face.height / Setting.Rows;
			const double Right = ((Column + 0.5) / Setting.Columns - 0.5) * Face.width;
			const double Up = (0.5 - (Row + 0.5) / Setting.Rows) * Face.height;
			for (int32 Axis = 0; Axis < 3; ++Axis)
			{
				Piece.world.o[Axis] += Face.world.u[Axis] * Right + Face.world.v[Axis] * Up;
			}
			const FLvScreenSection& Section = Setting.Sections[Index];
			if (ALvScreenActor* Actor = Put(Piece, LvSectionId(Whole.ScreenId, Index), FString::Printf(TEXT("%s, section %d"), *Whole.ScreenName, Index + 1), bStopsBeams))
			{
				Actor->Show(Section.Source, Section.Value, Section.Nits);
				Actor->SetPart(Section.Part);
				Actor->SetMap(Section.bMap, Section.Map);
				Pieces.Add(Actor);
				++OutReport.Sections;
			}
		}
		return Pieces;
	};

	// The rig file's own screens.
	viz::Archive Archive;
	std::string ArchiveError;
	if (!MvrPath.IsEmpty() && Archive.open_file(LvToStd(MvrPath), ArchiveError))
	{
		const viz::MvrResult Scene = viz::load_mvr(Archive);
		if (Scene.ok)
		{
			for (const viz::ScreenFace& Face : viz::screen_faces(Scene.scene, Archive))
			{
				const FString Id = LvToFString(Face.uuid);
				ALvScreenActor* Actor = Put(Face, Id, LvToFString(Face.name), false);    // the model the file draws it with stops the beams
				if (!Actor)
				{
					continue;
				}
				++OutReport.FromRig;
				HangOnFixture(*Actor, LvToFString(Face.parent_fixture), LvToFString(Face.child_position));    // with the model it is the face of
				const FLvScreenSetting* Chosen = Settings.FindByPredicate([&Id](const FLvScreenSetting& Setting)
					{ return !Setting.bAdded && Setting.Id.Equals(Id, ESearchCase::IgnoreCase); });
				if (Chosen)
				{
					for (ALvScreenActor* Piece : Dress(*Actor, Face, *Chosen, false))
					{
						HangOnFixture(*Piece, LvToFString(Face.parent_fixture), LvToFString(Face.child_position));
					}
				}
				else if (!Face.sources.empty() && Face.sources[0].type == "NDI")
				{
					Actor->Show(TEXT("ndi"), LvToFString(Face.sources[0].value), 1000.0f);    // the rig file says which stream it shows
				}
				else
				{
					Actor->Show(TEXT("pattern"), FString(), 1000.0f);
					if (!Face.sources.empty())
					{
						OutReport.Messages.Add(FString::Printf(TEXT("the rig file names \"%s\" for the screen %s: choose what it shows in Openviz"),
							*LvToFString(Face.sources[0].value), *LvToFString(Face.name)));
					}
				}
			}
		}
	}

	// And those added in Openviz.
	for (const FLvScreenSetting& Setting : Settings)
	{
		if (!Setting.bAdded)
		{
			continue;
		}
		if (!(Setting.Width >= 100.0 && Setting.Height >= 100.0 && Setting.Width <= 200000.0 && Setting.Height <= 200000.0))
		{
			OutReport.Messages.Add(FString::Printf(TEXT("the screen %s has no usable size"), *Setting.Name));
			continue;
		}
		viz::ScreenFace Face = viz::standing_screen(Setting.X, Setting.Y, Setting.Z, Setting.Width, Setting.Height, Setting.Turn);
		if (Setting.bFace)
		{
			// Where fixtures hang, as the core found it: not necessarily upright.
			for (int32 Axis = 0; Axis < 3; ++Axis)
			{
				Face.world.u[Axis] = Setting.Face[Axis];
				Face.world.v[Axis] = Setting.Face[3 + Axis];
				Face.world.w[Axis] = Setting.Face[6 + Axis];
				Face.world.o[Axis] = Setting.Face[9 + Axis];
			}
		}
		if (ALvScreenActor* Actor = Put(Face, Setting.Id, Setting.Name, true))
		{
			++OutReport.Added;
			Dress(*Actor, Face, Setting, true);
		}
	}
	return Screens;
}

FString LvRig::EditsPath(const FString& MvrPath)
{
	return LvToFString(viz::show_edits_path(LvToStd(MvrPath)));
}

FString LvRig::DescribeEdits(const TArray<ALvFixtureActor*>& Fixtures, const TArray<FString>& RemovedUuids, int32& OutEdited)
{
	viz::ShowEdits Edits;
	for (const ALvFixtureActor* Fixture : Fixtures)
	{
		if (!Fixture || Fixture->SourceUuid.IsEmpty())
		{
			continue;    // not from a rig file, or the file gave it no id to find it by again
		}
		// A twentieth of a millimetre or a hundredth of a degree is the editor's rounding, not a move.
		const FTransform Now = Fixture->GetActorTransform();
		// And a fixture the rig file gave no place, once it has one, is written with it even if that is where it lay.
		const bool bMoved = !Now.GetLocation().Equals(Fixture->SourceTransform.GetLocation(), 0.005) ||
			FMath::RadiansToDegrees(Now.GetRotation().AngularDistance(Fixture->SourceTransform.GetRotation())) > 0.01 ||
			(!Fixture->bSourcePlaced && !Fixture->IsUnplaced());

		if (Fixture->bAdded)
		{
			// Everything about an added fixture lives in its entry, so all of it is written each time.
			viz::AddedFixture Entry;
			Entry.uuid = LvToStd(Fixture->SourceUuid);
			Entry.name = LvToStd(Fixture->AddedName);
			Entry.fixture_id = LvToStd(Fixture->AddedFixtureId);
			Entry.gdtf = LvToStd(Fixture->AddedGdtf);
			Entry.mode = LvToStd(Fixture->Mode);
			Entry.world = bMoved ? LvCoordinates::ToMvr(Now) : Fixture->AddedWorld;
			Entry.universe = Fixture->Universe;
			Entry.address = Fixture->Address;
			Entry.invert_pan = Fixture->bInvertPan;
			Entry.invert_tilt = Fixture->bInvertTilt;
			Edits.added.push_back(MoveTemp(Entry));
			continue;
		}

		viz::FixtureEdit Edit;
		Edit.uuid = LvToStd(Fixture->SourceUuid);
		if (bMoved)
		{
			Edit.world = LvCoordinates::ToMvr(Now);
		}
		if (Fixture->Universe != Fixture->SourceUniverse || Fixture->Address != Fixture->SourceAddress)
		{
			Edit.universe = Fixture->Universe;
			Edit.address = Fixture->Address;
		}
		if (!Fixture->OwnGdtf.IsEmpty())
		{
			// A type of its own: written with its mode, whatever that is, since the rig file's mode names nothing of this type.
			Edit.gdtf = LvToStd(Fixture->OwnGdtf);
			Edit.mode = LvToStd(Fixture->Mode);
		}
		else if (Fixture->Mode != Fixture->SourceMode)
		{
			Edit.mode = LvToStd(Fixture->Mode);
		}
		// Its own pan and tilt inversion can be changed here; its name and number only in Openviz, and are kept as they stand.
		if (Fixture->bInvertPan != Fixture->bSourceInvertPan)
		{
			Edit.invert_pan = Fixture->bInvertPan;
		}
		if (Fixture->bInvertTilt != Fixture->bSourceInvertTilt)
		{
			Edit.invert_tilt = Fixture->bInvertTilt;
		}
		if (Fixture->ShowName != Fixture->SourceName)
		{
			Edit.name = LvToStd(Fixture->ShowName);
		}
		if (Fixture->ShowFixtureId != Fixture->SourceFixtureId)
		{
			Edit.fixture_id = LvToStd(Fixture->ShowFixtureId);
		}
		if (!Edit.empty())
		{
			Edits.fixtures.push_back(MoveTemp(Edit));
		}
	}
	for (const FString& Uuid : RemovedUuids)
	{
		if (!Uuid.IsEmpty() && !Edits.is_removed(LvToStd(Uuid)))
		{
			Edits.removed.push_back(LvToStd(Uuid));
		}
	}
	OutEdited = static_cast<int32>(Edits.fixtures.size() + Edits.added.size() + Edits.removed.size());
	return LvToFString(viz::write_show_edits(Edits));
}
