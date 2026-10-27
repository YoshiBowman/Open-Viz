#include "LvShowSubsystem.h"

#include "Misc/DateTime.h"

#include "Components/DirectionalLightComponent.h"
#include "Components/SkyLightComponent.h"
#include "Engine/DirectionalLight.h"
#include "Engine/SkyLight.h"
#include "Engine/TextureCube.h"
#include "Engine/Engine.h"
#include "Engine/ExponentialHeightFog.h"
#include "Components/ExponentialHeightFogComponent.h"
#include "Engine/World.h"
#include "Framework/Application/SlateApplication.h"
#include "EngineUtils.h"
#include "GameFramework/FloatingPawnMovement.h"
#include "GameFramework/Pawn.h"
#include "InputCoreTypes.h"
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformMisc.h"
#include "LvDmxSubsystem.h"
#include "LvFixtureActor.h"
#include "LvFixtureLibrary.h"
#include "Dom/JsonObject.h"
#include "LvRig.h"
#include "LvSceneryActor.h"
#include "LvScreenActor.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "LvStage.h"
#include "LvCoordinates.h"
#include "vizcore/dmx_recording.h"
#include "vizcore/movie.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "Modules/ModuleManager.h"
#include "vizcore/pieces.h"
#include "vizcore/psn.h"
#include "LvLightMeter.h"
#include "IPAddress.h"
#include "Sockets.h"
#include "SocketSubsystem.h"
#include "LvVideo.h"
#include "HAL/FileManager.h"
#include "Misc/App.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "UnrealClient.h"
#include "Engine/GameViewportClient.h"
#include "Camera/PlayerCameraManager.h"
#include "HighResScreenshot.h"

DEFINE_LOG_CATEGORY_STATIC(LogLvShow, Log, All);

namespace
{
	TArray<FLvScreenSetting> ReadScreenSettings(const FString& Path);    // further down, with the rest about screens

	// The set pieces Openviz keeps beside a show: {"pieces": [{"id", "kind", "name", "x", "y", "z", "turn", "width", "depth", "height", "colour": [r, g, b]}]},
	// millimetres and degrees. What is missing is as the kind usually is.
	TArray<FLvPieceSetting> ReadPieceSettings(const FString& Path, const FString& FixtureLibrary)
	{
		const FString Models = LvToFString(viz::models_folder(LvToStd(FixtureLibrary)));
		TArray<FLvPieceSetting> Settings;
		FString Text;
		TSharedPtr<FJsonObject> Root;
		const TArray<TSharedPtr<FJsonValue>>* Listed = nullptr;
		if (Path.IsEmpty() || !FFileHelper::LoadFileToString(Text, *Path) || !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Root) ||
			!Root.IsValid() || !Root->TryGetArrayField(TEXT("pieces"), Listed))
		{
			return Settings;
		}
		for (const TSharedPtr<FJsonValue>& Entry : *Listed)
		{
			const TSharedPtr<FJsonObject>* Object = nullptr;
			if (!Entry.IsValid() || !Entry->TryGetObject(Object) || !Object->IsValid())
			{
				continue;
			}
			FLvPieceSetting Setting;
			(*Object)->TryGetStringField(TEXT("id"), Setting.Id);
			(*Object)->TryGetStringField(TEXT("kind"), Setting.Kind);
			(*Object)->TryGetStringField(TEXT("name"), Setting.Name);
			const viz::SetPiece Usual = viz::default_piece(LvToStd(Setting.Kind));
			Setting.Width = Usual.width * 1000.0;
			Setting.Depth = Usual.depth * 1000.0;
			Setting.Height = Usual.height * 1000.0;
			Setting.Colour = FLinearColor(Usual.color[0], Usual.color[1], Usual.color[2]);
			(*Object)->TryGetNumberField(TEXT("x"), Setting.X);
			(*Object)->TryGetNumberField(TEXT("y"), Setting.Y);
			(*Object)->TryGetNumberField(TEXT("z"), Setting.Z);
			(*Object)->TryGetNumberField(TEXT("turn"), Setting.Turn);
			(*Object)->TryGetNumberField(TEXT("width"), Setting.Width);
			(*Object)->TryGetNumberField(TEXT("depth"), Setting.Depth);
			(*Object)->TryGetNumberField(TEXT("height"), Setting.Height);
			const TArray<TSharedPtr<FJsonValue>>* Colour = nullptr;
			if ((*Object)->TryGetArrayField(TEXT("colour"), Colour) && Colour->Num() == 3)
			{
				Setting.Colour = FLinearColor(static_cast<float>((*Colour)[0]->AsNumber()), static_cast<float>((*Colour)[1]->AsNumber()), static_cast<float>((*Colour)[2]->AsNumber()));
			}
			(*Object)->TryGetStringField(TEXT("picture"), Setting.Picture);
			// A tracker to follow: as the core reads it (vizcore/pieces.h), a number up to 65535 or the entry is not used.
			double Tracker = 0.0;
			if ((*Object)->TryGetNumberField(TEXT("tracker"), Tracker) && (Tracker < 0.0 || Tracker > 65535.0))
			{
				continue;
			}
			Setting.Tracker = static_cast<int32>(Tracker);
			(*Object)->TryGetBoolField(TEXT("follow_height"), Setting.bFollowHeight);
			if (Setting.Kind == TEXT("model"))
			{
				// Its file is a plain name in Openviz's models folder, beside the fixture library.
				FString File;
				(*Object)->TryGetStringField(TEXT("file"), File);
				(*Object)->TryGetStringField(TEXT("units"), Setting.Units);
				(*Object)->TryGetStringField(TEXT("up"), Setting.Up);
				if (File.IsEmpty() || File.Contains(TEXT("/")) || File.Contains(TEXT("\\")) || FixtureLibrary.IsEmpty())
				{
					continue;
				}
				Setting.ModelPath = FPaths::Combine(Models, File);
			}
			Settings.Add(Setting);
		}
		return Settings;
	}

	bool HasValue(const TCHAR* Key)
	{
		FString Value;
		return FParse::Value(FCommandLine::Get(), Key, Value, false) && !Value.IsEmpty();
	}

	int32 IntOption(const TCHAR* Key, int32 Default)
	{
		int32 Value = Default;
		FParse::Value(FCommandLine::Get(), Key, Value);
		return Value;
	}

	// Where the view points after the mouse has reported moving by (X, Y); Y is positive upward.
	// The scale is the engine's own for its fly camera, so Speed 1 feels like stock Unreal.
	FRotator TurnView(const FRotator& From, float X, float Y, float Speed, bool bInvertY)
	{
		FRotator To = From.GetNormalized();
		To.Yaw += X * 2.5 * Speed;
		To.Pitch = FMath::Clamp(To.Pitch + (bInvertY ? -Y : Y) * 2.5 * Speed, -89.0, 89.0);
		To.Roll = 0.0;
		return To;
	}
}

FLvShowReadyToEdit& ULvShowSubsystem::OnReadyToEdit()
{
	static FLvShowReadyToEdit Delegate;
	return Delegate;
}

bool ULvShowSubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
	return (HasValue(TEXT("LvShow=")) || HasValue(TEXT("LvShowGdtf="))) && Super::ShouldCreateSubsystem(Outer);
}

bool ULvShowSubsystem::DoesSupportWorldType(const EWorldType::Type WorldType) const
{
	// Editor is the level being edited, not the small preview worlds of asset editors.
	return WorldType == EWorldType::Game || WorldType == EWorldType::PIE || WorldType == EWorldType::Editor;
}

TStatId ULvShowSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(ULvShowSubsystem, STATGROUP_Tickables);
}

void ULvShowSubsystem::OnWorldBeginPlay(UWorld& InWorld)
{
	Super::OnWorldBeginPlay(InWorld);
	Start(InWorld);
}

void ULvShowSubsystem::Start(UWorld& InWorld)
{
	const TCHAR* CommandLine = FCommandLine::Get();
	bEditing = InWorld.WorldType == EWorldType::Editor;

	float Exposure = -8.0f;
	FParse::Value(CommandLine, TEXT("LvExposure="), Exposure);
	ExposureNow = FMath::Clamp(Exposure, -16.0f, 8.0f);
	LvStage::Build(InWorld, ExposureNow);
	FParse::Value(CommandLine, TEXT("LvFloor="), FloorRoughness);
	FParse::Value(CommandLine, TEXT("LvFloorShade="), FloorShade);
	FParse::Value(CommandLine, TEXT("LvBloom="), BloomNow);
	bMotionBlur = IntOption(TEXT("LvMotionBlur="), 1) != 0;
	FloorRoughness = FMath::Clamp(FloorRoughness, 0.0f, 1.0f);
	FloorShade = FMath::Clamp(FloorShade, 0.0f, 1.0f);
	BloomNow = FMath::Clamp(BloomNow, 0.0f, 4.0f);
	LvStage::SetFloor(InWorld, FloorShade, FloorRoughness);
	LvStage::SetLook(InWorld, BloomNow, bMotionBlur);
	// Openviz's picture settings, for what is changed there while the show is open. What they
	// say when the show starts is what the command line says: only later changes count.
	FParse::Value(CommandLine, TEXT("LvPicture="), PictureFile, false);
	PictureStamp = PictureFile.IsEmpty() ? FDateTime::MinValue() : IFileManager::Get().GetTimeStamp(*PictureFile);
	// And its word to this window while it is open: what is in the file now was said to an earlier one.
	FParse::Value(CommandLine, TEXT("LvCommands="), CommandsFile, false);
	CommandsStamp = CommandsFile.IsEmpty() ? FDateTime::MinValue() : IFileManager::Get().GetTimeStamp(*CommandsFile);

	float Haze = 0.06f;
	if (FParse::Value(CommandLine, TEXT("LvHaze="), Haze))
	{
		for (TActorIterator<AExponentialHeightFog> Fog(&InWorld); Fog; ++Fog)
		{
			Fog->GetComponent()->SetFogDensity(FMath::Clamp(Haze, 0.0f, 0.5f));
		}
	}
	HazeSetting = FMath::Clamp(Haze, 0.0f, 0.5f);
	bRigHaze = IntOption(TEXT("LvHazers="), 1) != 0;

	int32 MegaLights = -1;
	if (FParse::Value(CommandLine, TEXT("LvMegaLights="), MegaLights))
	{
		if (IConsoleVariable* CVar = IConsoleManager::Get().FindConsoleVariable(TEXT("r.MegaLights.EnableForProject")))
		{
			CVar->Set(MegaLights != 0 ? 1 : 0, ECVF_SetByConsole);
		}
	}

	FString MvrPath;
	FString GdtfPath;
	if (FParse::Value(CommandLine, TEXT("LvShow="), MvrPath, false) && !MvrPath.IsEmpty())
	{
		ShowPath = MvrPath;
		SpawnRig(InWorld, MvrPath);
	}
	// PosiStageNet as Openviz's receiver passes it on: set pieces given a tracker follow it while watching.
	bMeterOnStart = IntOption(TEXT("LvMeter="), 0) != 0 && !bEditing;
	const int32 PsnPort = IntOption(TEXT("LvPsnPort="), 0);
	if (PsnPort > 0 && !bEditing)
	{
		OpenPsn(PsnPort);
	}
	else if (FParse::Value(CommandLine, TEXT("LvShowGdtf="), GdtfPath, false))
	{
		SpawnPreview(InWorld, GdtfPath);
		SpawnScreens(InWorld, FString());    // none, unless -LvScreens names some
	}

	bIdleLook = IntOption(TEXT("LvIdleLook="), 1) != 0;
	const bool bShadows = IntOption(TEXT("LvShadows="), 0) != 0;
	bSurfaceShadows = bShadows;
	// Beams drawn on cones unless the fog look is asked for. The haze setting drives both: the
	// fog's density, and how much light a cone's air sends toward the eye.
	FString BeamStyle;
	FParse::Value(CommandLine, TEXT("LvBeams="), BeamStyle);
	const bool bCones = !BeamStyle.Equals(TEXT("fog"), ESearchCase::IgnoreCase);
	bConeBeams = bCones;
	// What stands in a beam cuts the part of it behind: on unless switched off.
	bBeamShadowsOn = IntOption(TEXT("LvBeamShadows="), 1) != 0;
	// Haze hangs in drifts that wander through the room: how much the beams show of that.
	HazeDriftSetting = 0.4f;
	FParse::Value(CommandLine, TEXT("LvHazeDrift="), HazeDriftSetting);
	// Beams are drawn at this share of the picture's resolution; and at less, by itself, while the picture runs slowly.
	BeamDetailSetting = 100.0f;
	FParse::Value(CommandLine, TEXT("LvBeamRes="), BeamDetailSetting);
	BeamDetailSetting = FMath::Clamp(BeamDetailSetting, 25.0f, 100.0f);
	BeamDetailNow = BeamDetailSetting;
	bAdaptBeams = IntOption(TEXT("LvAdaptBeams="), 1) != 0;
	// -LvDmx=13:60,22:255 puts those levels in those slots of every fixture's idle look, slots
	// counted from the fixture's own first: for seeing a gobo or a prism without a console.
	Poked.Reset();
	FString PokeList;
	if (FParse::Value(CommandLine, TEXT("LvDmx="), PokeList, false))
	{
		TArray<FString> Pairs;
		PokeList.ParseIntoArray(Pairs, TEXT(","));
		for (const FString& Pair : Pairs)
		{
			FString Slot;
			FString Level;
			if (Pair.Split(TEXT(":"), &Slot, &Level))
			{
				Poked.Emplace(FCString::Atoi(*Slot), static_cast<uint8>(FMath::Clamp(FCString::Atoi(*Level), 0, 255)));
			}
		}
	}
	int32 Beams = 0;
	int32 Drawn = 0;
	int32 LastUniverse = 1;
	Hazers = 0;
	for (const TWeakObjectPtr<ALvFixtureActor>& Fixture : Fixtures)
	{
		DressFixture(*Fixture);
		ShowOrHide(*Fixture);
		Hazers += Fixture->HasFogControl() && !Fixture->IsUnplaced() ? 1 : 0;
		Beams += Fixture->GetBeamCount();
		Drawn += Fixture->GetDrawnBeamCount();
		// A fixture that starts near the end of a universe runs on into the next.
		LastUniverse = FMath::Max(LastUniverse, Fixture->GetUniverse() + (Fixture->GetAddress() + Fixture->GetFootprint() - 2) / 512);
	}
	GiveLooks();
	UE_LOG(LogLvShow, Display, TEXT("LvShow: %d fixtures and %d beams are on stage"), Fixtures.Num(), Beams);
	if (Drawn < Beams)
	{
		UE_LOG(LogLvShow, Display, TEXT("LvShow: lenses that sit side by side are drawn as one beam: %d beams drawn for the %d"), Drawn, Beams);
	}
	if (Hazers > 0)
	{
		UE_LOG(LogLvShow, Display, TEXT("LvShow: %d fixture%s with a fog or haze channel: %s"), Hazers, Hazers == 1 ? TEXT("") : TEXT("s"),
			bRigHaze ? TEXT("what they put out adds to the haze") : TEXT("left out of the haze (-LvHazers=0)"));
	}

	FString Protocol = TEXT("artnet");
	FParse::Value(CommandLine, TEXT("LvProtocol="), Protocol);
	const bool bSacn = Protocol.Equals(TEXT("sacn"), ESearchCase::IgnoreCase);
	FString Address = TEXT("127.0.0.1");
	FParse::Value(CommandLine, TEXT("LvDmxAddress="), Address);
	// Art-Net is one socket whatever the universes, so all of them are taken: a rig patched high,
	// or a screen lit pixel by pixel on universes of its own, needs no counting. sACN is a group
	// to join for every universe, so there it is as many as the rig uses (and a screen that needs
	// more asks for them: ULvDmxSubsystem::CoverUniverses).
	const int32 Universes = bSacn ? FMath::Clamp(LastUniverse, 4, 256) : 32767;
	// One port for the process: the editor's level and a Play session in it share it.
	static bool bPortAdded = false;
	if (!bPortAdded)
	{
		bPortAdded = true;
		ULvDmxSubsystem::AddInputPort(FName(bSacn ? TEXT("sACN") : TEXT("Art-Net")), Address, 1, Universes);
	}
	Listening = bSacn ? FString::Printf(TEXT("sACN on %s, universes 1 to %d"), *Address, Universes) : FString::Printf(TEXT("Art-Net on %s, every universe"), *Address);
	FString Via;
	if (FParse::Value(CommandLine, TEXT("LvDmxVia="), Via) && Via.Split(TEXT("@"), &Via, &Address))
	{
		// Openviz is receiving for us and passing it on; say what the console needs to send.
		Listening = FString::Printf(TEXT("%s on %s (passed on by Openviz)"), *Via, *Address);
	}
	UE_LOG(LogLvShow, Display, TEXT("LvShow: listening for %s"), *Listening);

	FParse::Value(CommandLine, TEXT("LvShot="), ShotPath, false);
	FParse::Value(CommandLine, TEXT("LvPanorama="), PanoramaOnStart);
	FParse::Value(CommandLine, TEXT("LvPanoramaAfter="), PanoramaOnStartAt);
	FParse::Value(CommandLine, TEXT("LvShotAfter="), ShotAfter);
	FParse::Value(CommandLine, TEXT("LvPictures="), PicturesFolder, false);
	FParse::Value(CommandLine, TEXT("LvVideos="), VideosFolder, false);
	FParse::Value(CommandLine, TEXT("LvQuitAfter="), QuitAfter);
	if (FParse::Value(CommandLine, TEXT("LvRender="), RenderRecording, false) && FParse::Value(CommandLine, TEXT("LvRenderOut="), RenderOut, false))
	{
		FParse::Value(CommandLine, TEXT("LvRenderRate="), RenderRate);
		FParse::Value(CommandLine, TEXT("LvRenderView="), RenderView);
		FString Size;
		FString Wide;
		FString High;
		if (FParse::Value(CommandLine, TEXT("LvRenderSize="), Size) && Size.Split(TEXT("x"), &Wide, &High))
		{
			RenderSize = FIntPoint(FMath::Clamp(FCString::Atoi(*Wide), 320, 8192) & ~1, FMath::Clamp(FCString::Atoi(*High), 180, 8192) & ~1);
		}
		RenderRate = FMath::Clamp(RenderRate, 1, 120);
		RenderView = FMath::Clamp(RenderView, 0, 9);
		FString Path;
		if (FParse::Value(CommandLine, TEXT("LvRenderPath="), Path, false))
		{
			TArray<FString> Keys;
			Path.ParseIntoArray(Keys, TEXT(","));
			for (const FString& Key : Keys)
			{
				FString View;
				FString When;
				const bool bTimed = Key.Split(TEXT("@"), &View, &When);
				const int32 Index = FCString::Atoi(bTimed ? *View : *Key);
				const double Seconds = bTimed ? FCString::Atod(*When) : 0.0;
				if (Index >= 0 && Index <= 9 && Seconds >= 0.0 && (RenderPath.IsEmpty() || Seconds >= RenderPath.Last().Value))
				{
					RenderPath.Emplace(Index, Seconds);
				}
			}
		}
		if (RenderPath.IsEmpty())
		{
			RenderPath.Emplace(RenderView, 0.0);
		}
		RenderWarmup = 2 * RenderRate;
		RenderStage = 1;
		UE_LOG(LogLvShow, Display, TEXT("LvShow: render: %s into %s at %d frames a second, from %s"), *RenderRecording, *RenderOut, RenderRate,
			RenderView ? *FString::Printf(TEXT("view %d"), RenderView) : TEXT("the starting view"));
	}
	FString Exec;
	FString ExecWhen;
	if (FParse::Value(CommandLine, TEXT("LvExec="), Exec, false) && Exec.Split(TEXT("@"), &ExecCommand, &ExecWhen, ESearchCase::IgnoreCase, ESearchDir::FromEnd))
	{
		ExecAt = FCString::Atod(*ExecWhen);
	}
	FString DumpPath;
	if (FParse::Value(CommandLine, TEXT("LvShowDump="), DumpPath, false) && !bEditing)
	{
		WriteDump(DumpPath);
	}

	// House lights for the visualizer: an even light on everything, none of it in the haze.
	float Ambient = 0.0f;
	if (!bEditing && FParse::Value(CommandLine, TEXT("LvAmbient="), Ambient) && Ambient > 0.0f)
	{
		AmbientNow = FMath::Clamp(Ambient, 0.0f, 1.0f);
		SpawnAmbient(InWorld, AmbientNow);
	}

	if (bEditing)
	{
		// Work lights, so the fixtures' bodies and the floor can be seen while editing. They are
		// sized for the stage's exposure, light no haze, and exist in the editor only.
		FActorSpawnParameters Spawn;
		Spawn.ObjectFlags |= RF_Transient;
		const TPair<FRotator, float> WorkLights[] = {{FRotator(-50.0, 35.0, 0.0), 250.0f}, {FRotator(-35.0, -140.0, 0.0), 90.0f}};
		for (const TPair<FRotator, float>& Each : WorkLights)
		{
			if (ADirectionalLight* Light = InWorld.SpawnActor<ADirectionalLight>(FVector(0.0, 0.0, 1500.0), Each.Key, Spawn))
			{
				ULightComponent* Component = Light->GetLightComponent();
				Component->SetMobility(EComponentMobility::Movable);
				Component->SetIntensity(Each.Value);    // lux
				Component->SetCastShadows(false);
				Component->SetVolumetricScatteringIntensity(0.0f);
#if WITH_EDITOR
				Light->SetActorLabel(TEXT("Work light (editor only)"), false);
#endif
			}
		}

		// And an even light from all round, so no side of a fixture is ever black.
		SpawnAmbient(InWorld, 1.0f);

		// The fixtures show their look at once, whether or not the viewport is updating in real time.
		for (ALvFixtureActor* Fixture : LiveFixtures())
		{
			Fixture->Tick(0.0f);
		}

		FBox Bounds(ForceInit);
		for (const ALvFixtureActor* Fixture : LiveFixtures())
		{
			Bounds += Fixture->GetActorLocation();
		}
		if (Bounds.IsValid)
		{
			OnReadyToEdit().Broadcast(Bounds.ExpandBy(150.0));
		}

		// From here on, whatever differs from this is a change to save.
		int32 Edited = 0;
		LastSavedEdits = LvRig::DescribeEdits(LiveFixtures(), RemovedUuids, Edited);
		UE_LOG(LogLvShow, Display, TEXT("LvShow: ready to edit: move fixtures in the viewport, set universe, address and mode in Details; changes save themselves"));
#if WITH_EDITOR
		// A fixture deleted in the editor is taken out of the show. Heard from the editor itself,
		// which only says so for an actor someone deleted: a world closing down says nothing, and
		// must not be mistaken for the whole rig being deleted.
		if (GEngine && !ActorDeletedHandle.IsValid())
		{
			ActorDeletedHandle = GEngine->OnLevelActorDeleted().AddUObject(this, &ULvShowSubsystem::OnActorDeleted);
		}
#endif
		if (FParse::Param(CommandLine, TEXT("LvEditSelfTest")) || FParse::Param(CommandLine, TEXT("LvEditDeleteTest")))
		{
			SelfTestKind = FParse::Param(CommandLine, TEXT("LvEditDeleteTest")) ? 1 : 0;
			SelfTestStage = 0;
			SelfTestAt = 3.0;
		}
	}
	for (int32 Index = 0; Index < 10; ++Index)
	{
		NumberHeldSince[Index] = -1.0;
		bNumberStored[Index] = false;
	}
	if (FParse::Param(CommandLine, TEXT("LvViewSelfTest")))
	{
		ViewTestStage = 0;
		ViewTestAt = 4.0;
	}
	bActive = true;
}

void ULvShowSubsystem::SpawnAmbient(UWorld& World, float Level)
{
	FActorSpawnParameters Spawn;
	Spawn.ObjectFlags |= RF_Transient;
	if (ASkyLight* Sky = World.SpawnActor<ASkyLight>(FVector(0.0, 0.0, 1500.0), FRotator::ZeroRotator, Spawn))
	{
		AmbientLight = Sky;
		USkyLightComponent* Component = Sky->GetLightComponent();
		Component->SetMobility(EComponentMobility::Movable);
		Component->SourceType = SLS_SpecifiedCubemap;
		Component->Cubemap = LoadObject<UTextureCube>(nullptr, TEXT("/Engine/EngineResources/GrayLightTextureCube.GrayLightTextureCube"));
		Component->SetIntensity(250.0f * Level);    // sized for the stage's exposure of about -8 stops
		Component->SetVolumetricScatteringIntensity(0.0f);
		Component->SetCastShadows(false);
		Component->SetCaptureIsDirty();
#if WITH_EDITOR
		Sky->SetActorLabel(TEXT("Ambient light"), false);
#endif
	}
}

TArray<ALvFixtureActor*> ULvShowSubsystem::LiveFixtures() const
{
	TArray<ALvFixtureActor*> Live;
	for (const TWeakObjectPtr<ALvFixtureActor>& Fixture : Fixtures)
	{
		if (Fixture.IsValid())
		{
			Live.Add(Fixture.Get());
		}
	}
	return Live;
}

void ULvShowSubsystem::SaveEdits()
{
	if (ShowPath.IsEmpty())
	{
		return;    // a fixture preview has no show to save into
	}
	// A fixture the rig file gave no place has one as soon as it is moved from where it lay: it lights up.
	for (ALvFixtureActor* Fixture : LiveFixtures())
	{
		if (Fixture->IsUnplaced() && (!Fixture->GetActorLocation().Equals(Fixture->SourceTransform.GetLocation(), 0.005) ||
			FMath::RadiansToDegrees(Fixture->GetActorQuat().AngularDistance(Fixture->SourceTransform.GetRotation())) > 0.01))
		{
			Fixture->SetUnplaced(false);
			ShowOrHide(*Fixture);
			Fixture->Tick(0.0f);
		}
	}
	int32 Edited = 0;
	const FString Text = LvRig::DescribeEdits(LiveFixtures(), RemovedUuids, Edited);
	if (Text == LastSavedEdits)
	{
		return;
	}
	// Written beside the final name and moved over it, so a crash mid-write cannot leave half a file.
	const FString Path = LvRig::EditsPath(ShowPath);
	const FString Part = Path + TEXT(".part");
	if (FFileHelper::SaveStringToFile(Text, *Part, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM) &&
		IFileManager::Get().Move(*Path, *Part, true))
	{
		LastSavedEdits = Text;
		EditsSeen = Text;
		EditsStamp = IFileManager::Get().GetTimeStamp(*Path);
		UE_LOG(LogLvShow, Display, TEXT("LvShow: saved: %d fixture%s now differ from the rig file"), Edited, Edited == 1 ? TEXT("") : TEXT("s"));
	}
	else
	{
		UE_LOG(LogLvShow, Error, TEXT("LvShow: note: the changes could not be saved to %s"), *Path);
	}
}

void ULvShowSubsystem::WriteDump(const FString& Path) const
{
	FString Text;
	for (const ALvFixtureActor* Fixture : LiveFixtures())
	{
		const FVector At = Fixture->GetActorLocation();
		const FRotator Turn = Fixture->GetActorRotation();
		Text += FString::Printf(TEXT("%s\t%s\t%.2f\t%.2f\t%.2f\t%.2f\t%d\t%d\t%s\n"), *Fixture->SourceUuid, *Fixture->FixtureName,
			At.X, At.Y, At.Z, Turn.Yaw, Fixture->Universe, Fixture->Address, *Fixture->Mode);
	}
	FFileHelper::SaveStringToFile(Text, *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
}

// What a person would do by hand in the editor, done by itself so it can be checked: the first
// fixture is moved and turned, the second re-patched, the third put in another mode.
void ULvShowSubsystem::OnActorDeleted(AActor* Actor)
{
	const ALvFixtureActor* Fixture = Cast<ALvFixtureActor>(Actor);
	UWorld* World = GetWorld();
	if (!Fixture || !bEditing || bReconciling || !World || World->bIsTearingDown || IsEngineExitRequested() || Fixture->GetWorld() != World ||
		Fixture->SourceUuid.IsEmpty() || !Fixtures.Contains(Fixture))
	{
		return;
	}
	// One that was added simply goes: it is no longer among the fixtures the edits are written from.
	if (!Fixture->bAdded)
	{
		RemovedUuids.AddUnique(Fixture->SourceUuid);
	}
	UE_LOG(LogLvShow, Display, TEXT("LvShow: '%s' was deleted and is out of the show"), *Fixture->FixtureName);
}

void ULvShowSubsystem::Deinitialize()
{
	if (Video.IsValid() && Video->IsActive())
	{
		Video->StopNow();    // what was recorded is kept
		FollowVideo();
	}
	ULvDmxSubsystem::ClearProvided();
	ClosePsn();
#if WITH_EDITOR
	if (GEngine && ActorDeletedHandle.IsValid())
	{
		GEngine->OnLevelActorDeleted().Remove(ActorDeletedHandle);
	}
#endif
	ActorDeletedHandle.Reset();
	Super::Deinitialize();
}

void ULvShowSubsystem::StepSelfTest()
{
	const TArray<ALvFixtureActor*> Rig = LiveFixtures();
	if (SelfTestStage == 0 && SelfTestKind == 1)
	{
		// What deleting two fixtures by hand would do: the last fixture of the rig file, and the last added one.
		ALvFixtureActor* FromFile = nullptr;
		ALvFixtureActor* Added = nullptr;
		for (ALvFixtureActor* Fixture : Rig)
		{
			(Fixture->bAdded ? Added : FromFile) = Fixture;
		}
		UE_LOG(LogLvShow, Display, TEXT("LvShow: self-test: deleting '%s' and '%s'"), FromFile ? *FromFile->FixtureName : TEXT("(none)"),
			Added ? *Added->FixtureName : TEXT("(none)"));
		if (FromFile)
		{
			FromFile->Destroy();
		}
		if (Added)
		{
			Added->Destroy();
		}
		SelfTestStage = 1;
		SelfTestAt = Elapsed + 2.0;
	}
	else if (SelfTestStage == 0)
	{
		if (Rig.Num() >= 3)
		{
			Rig[0]->SetActorLocationAndRotation(Rig[0]->GetActorLocation() + FVector(100.0, -50.0, 25.0),
				Rig[0]->GetActorRotation() + FRotator(0.0, 30.0, 0.0));
			Rig[1]->Universe = 7;
			Rig[1]->Address = 101;
			const TArray<FString> Modes = Rig[2]->GetModeNames();
			const FString* Other = Modes.FindByPredicate([&](const FString& Name) { return Name != Rig[2]->Mode; });
			const bool bSwitched = Other && Rig[2]->SetMode(*Other);
			UE_LOG(LogLvShow, Display, TEXT("LvShow: self-test: moved '%s', re-patched '%s', mode of '%s' %s"), *Rig[0]->FixtureName,
				*Rig[1]->FixtureName, *Rig[2]->FixtureName, bSwitched ? *FString::Printf(TEXT("is now %s"), *Rig[2]->Mode) : TEXT("could not be changed"));
		}
		else
		{
			UE_LOG(LogLvShow, Error, TEXT("LvShow: note: self-test needs three fixtures, the rig has %d"), Rig.Num());
		}
		// And a screen added in Openviz, if the show has one: half a metre up and a quarter turn about the vertical.
		for (const TPair<TWeakObjectPtr<ALvScreenActor>, FTransform>& Placed : ScreensPlaced)
		{
			if (ALvScreenActor* Screen = Placed.Key.Get())
			{
				Screen->SetActorLocationAndRotation(Screen->GetActorLocation() + FVector(0.0, 0.0, 50.0),
					FQuat(FVector::UpVector, UE_HALF_PI) * Screen->GetActorQuat());
				UE_LOG(LogLvShow, Display, TEXT("LvShow: self-test: moved screen '%s'"), *Screen->ScreenName);
				break;
			}
		}
		SelfTestStage = 1;
		SelfTestAt = Elapsed + 2.0;    // time for the save to happen by itself
	}
	else if (SelfTestStage == 1)
	{
		SelfTestStage = 2;
		UE_LOG(LogLvShow, Display, TEXT("LvShow: self-test: done"));
		FPlatformMisc::RequestExit(false);
	}
}

void ULvShowSubsystem::SpawnRig(UWorld& World, const FString& MvrPath)
{
	FLvRigReport Report;
	FParse::Value(FCommandLine::Get(), TEXT("LvFixtureLibrary="), LibraryFolder, false);
	bShowUnplaced = IntOption(TEXT("LvShowUnplaced="), 0) != 0;
	// The show's own files as they are now: what changes in them later is taken up while the show is open.
	const FString EditsFile = LvRig::EditsPath(MvrPath);
	const FString TypesFile = FPaths::ChangeExtension(MvrPath, TEXT("types.xml"));
	EditsStamp = IFileManager::Get().GetTimeStamp(*EditsFile);
	TypesStamp = IFileManager::Get().GetTimeStamp(*TypesFile);
	EditsSeen.Reset();
	TypesSeen.Reset();
	FFileHelper::LoadFileToString(EditsSeen, *EditsFile);
	FFileHelper::LoadFileToString(TypesSeen, *TypesFile);
	for (ALvFixtureActor* Fixture : LvRig::SpawnFromMvr(World, MvrPath, Report, LibraryFolder))
	{
		Fixtures.Add(Fixture);
	}
	RemovedUuids = Report.RemovedUuids;
	if (Report.Unplaced > 0)
	{
		UE_LOG(LogLvShow, Display, TEXT("LvShow: %d fixture%s no place in the rig file: %s"), Report.Unplaced,
			Report.Unplaced == 1 ? TEXT(" has") : TEXT("s have"),
			bShowUnplaced ? TEXT("shown where the file leaves them, all in one spot") : TEXT("left dark until given one (in Openviz, the show's Patch)"));
	}
	UE_LOG(LogLvShow, Display, TEXT("LvShow: opened %s: %d fixtures built, %d skipped"), *FPaths::GetCleanFilename(MvrPath),
		Report.Spawned, Report.Skipped);
	if (Report.StoodIn > 0)
	{
		UE_LOG(LogLvShow, Display, TEXT("LvShow: %d fixtures of %d types are read from fixture files of the library that stand in for the rig file's placeholders"),
			Report.StoodIn, Report.StoodInTypes);
	}
	if (Report.Added > 0 || Report.Removed > 0)
	{
		UE_LOG(LogLvShow, Display, TEXT("LvShow: %d of them added in Openviz; %d of the rig file's taken out"), Report.Added, Report.Removed);
	}
	if (Report.Edited > 0)
	{
		UE_LOG(LogLvShow, Display, TEXT("LvShow: %d fixture%s carry changes made in the Unreal Editor"), Report.Edited, Report.Edited == 1 ? TEXT("") : TEXT("s"));
	}
	for (int32 Index = 0; Index < Report.Messages.Num() && Index < 40; ++Index)
	{
		UE_LOG(LogLvShow, Warning, TEXT("LvShow: note: %s"), *Report.Messages[Index]);
	}
	if (Report.Messages.Num() > 40)
	{
		UE_LOG(LogLvShow, Warning, TEXT("LvShow: note: and %d more"), Report.Messages.Num() - 40);
	}

	// The stage, trusses and set pieces the file describes around the fixtures.
	Pieces.Reset();
	if (IntOption(TEXT("LvScenery="), 1) != 0)
	{
		FLvSceneryReport Scenery;
		for (ALvSceneryActor* Piece : LvRig::SpawnScenery(World, MvrPath, Scenery))
		{
			Pieces.Add(Piece);
		}
		if (Scenery.Objects > 0 || Scenery.Messages.Num() > 0)
		{
			UE_LOG(LogLvShow, Display, TEXT("LvShow: %d set piece%s on stage, drawn with %d models and %d triangles"), Scenery.Objects,
				Scenery.Objects == 1 ? TEXT("") : TEXT("s"), Scenery.Pieces, Scenery.Triangles);
		}
		for (const FString& Message : Scenery.Messages)
		{
			UE_LOG(LogLvShow, Warning, TEXT("LvShow: note: %s"), *Message);
		}
		if (Scenery.Riding + Report.Riding > 0)
		{
			UE_LOG(LogLvShow, Display, TEXT("LvShow: %d set piece%s and %d fixture%s ride on fixtures that travel"), Scenery.Riding,
				Scenery.Riding == 1 ? TEXT("") : TEXT("s"), Report.Riding, Report.Riding == 1 ? TEXT("") : TEXT("s"));
		}
	}

	// Set pieces added in Openviz: decks, drapes, trusses, figures.
	PiecesFile.Reset();
	if (!FParse::Value(FCommandLine::Get(), TEXT("LvPieces="), PiecesFile, false))
	{
		PiecesFile = FPaths::ChangeExtension(MvrPath, TEXT("pieces.json"));
	}
	SpawnPieces(World);

	// Layers and classes chosen in Openviz not to be shown: kept beside the show as "<show>.hidden.json",
	// {"layers": [...], "classes": [...]}. What is hidden is still there, dark and out of the way of beams.
	HiddenStamp = IFileManager::Get().GetTimeStamp(*FPaths::ChangeExtension(MvrPath, TEXT("hidden.json")));
	ReadHidden();
	// The pictures on the show's screens. After the scenery: a screen's picture lies on the model the file draws it with.
	const TArray<ALvScreenActor*> Pictures = SpawnScreens(World, MvrPath);

	if (HiddenLayers.Num() > 0 || HiddenClasses.Num() > 0 || HiddenFixtureIds.Num() > 0)
	{
		int32 HiddenFixtures = 0;
		int32 HiddenPieces = 0;
		for (const TWeakObjectPtr<ALvFixtureActor>& Fixture : Fixtures)
		{
			HiddenFixtures += Fixture.IsValid() && IsFixtureHiddenByChoice(*Fixture) ? 1 : 0;    // Start hides them, with everything else that decides it
		}
		for (const TWeakObjectPtr<ALvSceneryActor>& Piece : Pieces)
		{
			if (Piece.IsValid() && IsHiddenByChoice(Piece->Layer, Piece->Class))
			{
				HideByLayerAndClass(*Piece);
				++HiddenPieces;
			}
		}
		for (ALvScreenActor* Picture : Pictures)
		{
			HiddenPieces += Picture && Picture->IsHidden() ? 1 : 0;    // SpawnScreens hid them
		}
		UE_LOG(LogLvShow, Display, TEXT("LvShow: %d fixture%s and %d set piece%s are hidden by layer, class or group"), HiddenFixtures,
			HiddenFixtures == 1 ? TEXT("") : TEXT("s"), HiddenPieces, HiddenPieces == 1 ? TEXT("") : TEXT("s"));
	}
}

void ULvShowSubsystem::SpawnPieces(UWorld& World)
{
	for (const TWeakObjectPtr<ALvSceneryActor>& Piece : AddedPieces)
	{
		if (Piece.IsValid())
		{
			Piece->Destroy();
		}
	}
	AddedPieces.Reset();
	Followers.Reset();
	People.Reset();
	PiecesStamp = PiecesFile.IsEmpty() ? FDateTime::MinValue() : IFileManager::Get().GetTimeStamp(*PiecesFile);
	const TArray<FLvPieceSetting> Settings = ReadPieceSettings(PiecesFile, LibraryFolder);
	for (ALvSceneryActor* Piece : LvRig::SpawnPieces(World, Settings))
	{
		AddedPieces.Add(Piece);
		const FLvPieceSetting* Setting = Settings.FindByPredicate([Piece](const FLvPieceSetting& Entry) { return Entry.Id == Piece->PieceId; });
		if (Setting && (Setting->Kind == TEXT("person") || Setting->Kind == TEXT("crowd")))
		{
			People.Add(Piece);
		}
		if (Setting && Setting->Tracker > 0)
		{
			FFollower& Follower = Followers.AddDefaulted_GetRef();
			Follower.Actor = Piece;
			Follower.Tracker = Setting->Tracker;
			Follower.bHeight = Setting->bFollowHeight;
			Follower.Z = Setting->Z;
			Follower.Turn = Setting->Turn;
			Follower.Name = Setting->Name;
		}
	}
	if (AddedPieces.Num() > 0)
	{
		UE_LOG(LogLvShow, Display, TEXT("LvShow: %d set piece%s added in Openviz %s on stage"), AddedPieces.Num(), AddedPieces.Num() == 1 ? TEXT("") : TEXT("s"),
			AddedPieces.Num() == 1 ? TEXT("is") : TEXT("are"));
	}
}

void ULvShowSubsystem::SetMeter(const FString& How)
{
	UWorld* World = GetWorld();
	if (!World || bEditing)
	{
		return;
	}
	if (!Meter.IsValid())
	{
		Meter = MakeShared<FLvLightMeter>();
	}
	const bool bOn = How == TEXT("on") || (How == TEXT("toggle") && !Meter->IsOn());
	if (!bOn)
	{
		if (Meter->IsOn())
		{
			Meter->Stop();
			UE_LOG(LogLvShow, Display, TEXT("LvShow: light levels off"));
		}
		return;
	}
	TArray<AActor*> NotSurfaces;
	for (const TWeakObjectPtr<ALvSceneryActor>& Person : People)
	{
		if (Person.IsValid())
		{
			NotSurfaces.Add(Person.Get());
		}
	}
	const double Started = FPlatformTime::Seconds();
	Meter->Start(*World, StageBounds(), NotSurfaces);
	if (Meter->IsOn())
	{
		UE_LOG(LogLvShow, Display, TEXT("LvShow: light levels on: %d by %d points %.0f cm apart, surfaces found in %.0f ms"), Meter->GetAcross(), Meter->GetDown(),
			Meter->GetCell(), (FPlatformTime::Seconds() - Started) * 1000.0);
	}
	else
	{
		UE_LOG(LogLvShow, Warning, TEXT("LvShow: light levels cannot be shown: there is no stage to read, or the screen material is missing"));
	}
}

void ULvShowSubsystem::FollowMeter()
{
	UWorld* World = GetWorld();
	const bool bHad = Meter->HasReading();
	Meter->Tick(*World, Fixtures);
	if (!bHad && Meter->HasReading())
	{
		const FVector Middle = StageBounds().GetCenter();
		UE_LOG(LogLvShow, Display, TEXT("LvShow: light levels: first reading from %d beams, highest %.0f lx, white at %.0f lx, %.0f lx below the middle of the rig; %d traces, %d of them stopped on the way"),
			Meter->LastBeams, Meter->Highest(), Meter->ScaleTop(), Meter->ReadingAt(FVector(Middle.X, Middle.Y, 0.0)), Meter->LastTraces, Meter->LastBlocked);
	}
	if (Elapsed < NextMeterSay || !GEngine || IsFilming())
	{
		return;
	}
	NextMeterSay = Elapsed + 0.1;
	// The reading under the pointer; where there is none (the mouse steering the view), in the middle of the picture.
	APlayerController* Controller = World->GetFirstPlayerController();
	FString Under;
	if (Controller && Meter->HasReading())
	{
		float MouseX = 0.0f;
		float MouseY = 0.0f;
		int32 Wide = 0;
		int32 High = 0;
		Controller->GetViewportSize(Wide, High);
		if (!Controller->GetMousePosition(MouseX, MouseY) || Controller->bShowMouseCursor == false)
		{
			MouseX = Wide * 0.5f;
			MouseY = High * 0.5f;
		}
		FVector From;
		FVector Way;
		FHitResult Hit;
		FCollisionQueryParams Query(SCENE_QUERY_STAT(LvLightMeterPointer), true);
		for (const TWeakObjectPtr<ALvFixtureActor>& Fixture : Fixtures)
		{
			if (Fixture.IsValid())
			{
				Query.AddIgnoredActor(Fixture.Get());
			}
		}
		if (Controller->DeprojectScreenPositionToWorld(MouseX, MouseY, From, Way) && World->LineTraceSingleByChannel(Hit, From, From + Way * 200000.0, ECC_Visibility, Query))
		{
			const double Lux = Meter->ReadingAt(Hit.ImpactPoint);
			if (Lux >= 0.0)
			{
				Under = FString::Printf(TEXT("   |   %s: %s lx"), Controller->bShowMouseCursor ? TEXT("under the pointer") : TEXT("in the middle"),
					*FText::AsNumber(FMath::RoundToInt64(Lux)).ToString());
			}
		}
	}
	const FString Said = !Meter->HasReading() ? FString(TEXT("Light levels: reading…"))
		: FString::Printf(TEXT("Light levels: dark 0 to white %s lx, highest %s lx%s   |   M hides them"), *FText::AsNumber(FMath::RoundToInt64(Meter->ScaleTop())).ToString(),
			*FText::AsNumber(FMath::RoundToInt64(Meter->Highest())).ToString(), *Under);
	GEngine->AddOnScreenDebugMessage(0x4C78, 0.3f, FColor(235, 225, 200), Said);
}

void ULvShowSubsystem::OpenPsn(int32 Port)
{
	ISocketSubsystem* Sockets = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
	if (!Sockets)
	{
		return;
	}
	// On this computer's own address: Openviz hears the tracking system on the lighting network and passes it on here.
	FSocket* Socket = Sockets->CreateSocket(NAME_DGram, TEXT("Openviz PosiStageNet"), FNetworkProtocolTypes::IPv4);
	TSharedRef<FInternetAddr> Here = Sockets->CreateInternetAddr(FNetworkProtocolTypes::IPv4);
	bool bValid = false;
	Here->SetIp(TEXT("127.0.0.1"), bValid);
	Here->SetPort(Port);
	if (!Socket || !Socket->SetNonBlocking(true) || !Socket->SetReuseAddr(true) || !Socket->Bind(*Here))
	{
		UE_LOG(LogLvShow, Warning, TEXT("LvShow: cannot listen for PosiStageNet on 127.0.0.1:%d: another program may hold the port"), Port);
		if (Socket)
		{
			Sockets->DestroySocket(Socket);
		}
		return;
	}
	PsnSocket = Socket;
	UE_LOG(LogLvShow, Display, TEXT("LvShow: listening for PosiStageNet from Openviz on 127.0.0.1:%d; %d set piece%s follow%s trackers"), Port,
		Followers.Num(), Followers.Num() == 1 ? TEXT("") : TEXT("s"), Followers.Num() == 1 ? TEXT("s") : TEXT(""));
}

void ULvShowSubsystem::ClosePsn()
{
	if (PsnSocket)
	{
		PsnSocket->Close();
		if (ISocketSubsystem* Sockets = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM))
		{
			Sockets->DestroySocket(PsnSocket);
		}
		PsnSocket = nullptr;
	}
}

void ULvShowSubsystem::FollowTrackers()
{
	// Everything that has come since the last frame; only the latest place of each tracker counts.
	uint8 Buffer[2048];
	uint32 Pending = 0;
	int32 Datagrams = 0;
	while (PsnSocket->HasPendingData(Pending) && Datagrams++ < 1000)
	{
		int32 Read = 0;
		if (!PsnSocket->Recv(Buffer, sizeof(Buffer), Read) || Read <= 0)
		{
			break;
		}
		const viz::PsnPacket Packet = viz::parse_psn(Buffer, static_cast<size_t>(Read));
		for (const viz::PsnTracker& Tracker : Packet.trackers)
		{
			if (Packet.kind == viz::PsnPacket::Kind::Info && !Tracker.name.empty())
			{
				TrackerNames.Add(Tracker.id, LvToFString(Tracker.name));
			}
			if (Packet.kind != viz::PsnPacket::Kind::Data || !Tracker.has_position)
			{
				continue;
			}
			double Place[3];
			viz::psn_to_mvr(Tracker.position, Place);
			Trackers.Add(Tracker.id, FVector(Place[0], Place[1], Place[2]));
			if (!TrackersSaid.Contains(Tracker.id))
			{
				TrackersSaid.Add(Tracker.id);
				const FString* Named = TrackerNames.Find(Tracker.id);
				UE_LOG(LogLvShow, Display, TEXT("LvShow: PosiStageNet: tracker %d%s heard at %.2f, %.2f, %.2f m"), Tracker.id,
					Named ? *FString::Printf(TEXT(" (%s)"), **Named) : TEXT(""), Place[0] / 1000.0, Place[1] / 1000.0, Place[2] / 1000.0);
			}
		}
	}
	for (FFollower& Follower : Followers)
	{
		const FVector* At = Trackers.Find(Follower.Tracker);
		if (!At || !Follower.Actor.IsValid())
		{
			continue;
		}
		// The rig file's terms, as the piece was placed: across the floor to the tracker, its own height unless it follows that too.
		const viz::Transform Place = viz::piece_place(At->X, At->Y, Follower.bHeight ? At->Z : Follower.Z, Follower.Turn);
		Follower.Actor->SetActorTransform(LvCoordinates::FromMvr(Place));
		if (!Follower.bSaid)
		{
			Follower.bSaid = true;
			UE_LOG(LogLvShow, Display, TEXT("LvShow: set piece '%s' follows tracker %d, now at %.2f, %.2f m"), *Follower.Name, Follower.Tracker,
				At->X / 1000.0, At->Y / 1000.0);
		}
	}
}

void ULvShowSubsystem::FollowPiecesFile(UWorld& World)
{
	// Few, and cheap to build (a shape is made once): when the file changes they are all set up again.
	if (PiecesFile.IsEmpty() || IFileManager::Get().GetTimeStamp(*PiecesFile) == PiecesStamp)
	{
		return;
	}
	const int32 Before = AddedPieces.Num();
	SpawnPieces(World);
	if (Meter.IsValid() && Meter->IsOn())
	{
		SetMeter(TEXT("on"));    // the surfaces may have changed
	}
	if (AddedPieces.Num() == 0 && Before > 0)
	{
		UE_LOG(LogLvShow, Display, TEXT("LvShow: the set pieces added in Openviz are gone from the stage"));
	}
}

bool ULvShowSubsystem::IsHiddenByChoice(const FString& Layer, const FString& Class) const
{
	return HiddenLayers.Contains(Layer) || (!Class.IsEmpty() && HiddenClasses.Contains(Class));
}

bool ULvShowSubsystem::IsFixtureHiddenByChoice(const ALvFixtureActor& Fixture) const
{
	return IsHiddenByChoice(Fixture.Layer, Fixture.Class) || (!Fixture.SourceUuid.IsEmpty() && HiddenFixtureIds.Contains(Fixture.SourceUuid.ToUpper()));
}

bool ULvShowSubsystem::ReadHidden()
{
	TSet<FString> Layers;
	TSet<FString> Classes;
	TSet<FString> Ids;
	FString HiddenText;
	if (!ShowPath.IsEmpty() && FFileHelper::LoadFileToString(HiddenText, *FPaths::ChangeExtension(ShowPath, TEXT("hidden.json"))))
	{
		TSharedPtr<FJsonObject> Hidden;
		if (FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(HiddenText), Hidden) && Hidden.IsValid())
		{
			const TArray<TSharedPtr<FJsonValue>>* Names = nullptr;
			if (Hidden->TryGetArrayField(TEXT("layers"), Names))
			{
				for (const TSharedPtr<FJsonValue>& Name : *Names)
				{
					Layers.Add(Name->AsString());
				}
			}
			if (Hidden->TryGetArrayField(TEXT("classes"), Names))
			{
				for (const TSharedPtr<FJsonValue>& Name : *Names)
				{
					Classes.Add(Name->AsString());
				}
			}
			// The fixtures of groups hidden in Openviz.
			if (Hidden->TryGetArrayField(TEXT("fixtures"), Names))
			{
				for (const TSharedPtr<FJsonValue>& Name : *Names)
				{
					Ids.Add(Name->AsString().ToUpper());
				}
			}
		}
	}
	const bool bChanged = Layers.Num() != HiddenLayers.Num() || Classes.Num() != HiddenClasses.Num() || !Layers.Includes(HiddenLayers) || !Classes.Includes(HiddenClasses) ||
		Ids.Num() != HiddenFixtureIds.Num() || !Ids.Includes(HiddenFixtureIds);
	HiddenLayers = MoveTemp(Layers);
	HiddenClasses = MoveTemp(Classes);
	HiddenFixtureIds = MoveTemp(Ids);
	return bChanged;
}

void ULvShowSubsystem::ApplyHidden(UWorld& World)
{
	// What was chosen in Openviz while the show is open: fixtures and set pieces come and go at once.
	for (const TWeakObjectPtr<ALvFixtureActor>& Fixture : Fixtures)
	{
		if (Fixture.IsValid())
		{
			ShowOrHide(*Fixture);
		}
	}
	for (const TWeakObjectPtr<ALvSceneryActor>& Piece : Pieces)
	{
		if (!Piece.IsValid())
		{
			continue;
		}
		const bool bOut = IsHiddenByChoice(Piece->Layer, Piece->Class);
		Piece->SetActorHiddenInGame(bOut);
		Piece->SetActorEnableCollision(!bOut);
#if WITH_EDITOR
		Piece->SetIsTemporarilyHiddenInEditor(bOut);
#endif
	}
	// Screens are few: they are built again, hidden or not as the lists now say.
	for (const TWeakObjectPtr<ALvScreenActor>& Screen : Screens)
	{
		if (Screen.IsValid())
		{
			Screen->Destroy();
		}
	}
	Screens.Reset();
	ScreensSaid.Reset();
	SpawnScreens(World, ShowPath);
	UE_LOG(LogLvShow, Display, TEXT("LvShow: layers and classes shown as chosen in Openviz: %d layer%s and %d class%s left out"), HiddenLayers.Num(),
		HiddenLayers.Num() == 1 ? TEXT("") : TEXT("s"), HiddenClasses.Num(), HiddenClasses.Num() == 1 ? TEXT("") : TEXT("es"));
}

void ULvShowSubsystem::DressFixture(ALvFixtureActor& Fixture)
{
	Fixture.SetCastShadows(bSurfaceShadows);
	// The haze setting drives both the fog's density and how much light a cone's air sends toward the eye.
	const double Density = HazeApplied >= 0.0 ? HazeApplied : static_cast<double>(HazeSetting);
	Fixture.SetBeamStyle(bConeBeams, static_cast<float>(Density) * 0.006f);
	Fixture.SetBeamShadows(bBeamShadowsOn);
	Fixture.SetHazeDrift(HazeDriftSetting);
}

void ULvShowSubsystem::GiveLooks()
{
	// A fixture shows its idle look until its universe has data, and follows the console from then on.
	for (const TWeakObjectPtr<ALvFixtureActor>& Fixture : Fixtures)
	{
		if (!Fixture.IsValid())
		{
			continue;
		}
		if (!bIdleLook || LiveUniverses.Contains(Fixture->GetUniverse()))
		{
			Fixture->SetDmxOverride(TConstArrayView<uint8>());
			continue;
		}
		Fixture->SetDefaultLook(1.0f);
		for (const TPair<int32, uint8>& Poke : Poked)
		{
			Fixture->SetLookSlot(Poke.Key, Poke.Value);
		}
	}
}

void ULvShowSubsystem::ShowOrHide(ALvFixtureActor& Fixture)
{
	const bool bChosen = IsFixtureHiddenByChoice(Fixture);
	const bool bBehind = BehindScreens.Contains(&Fixture);
	// A fixture without a place stands on top of every other such fixture. While watching it is
	// left out; in the editor it is there to be picked up and placed, but dark.
	const bool bNoPlace = Fixture.IsUnplaced() && !bShowUnplaced;
	const bool bOut = bChosen || bBehind || (bNoPlace && !bEditing);
	Fixture.SetActorHiddenInGame(bOut);
	Fixture.SetTicking(!(bOut || bNoPlace));    // a fixture that never ticks never lights its beams
	Fixture.SetActorEnableCollision(!bOut);
#if WITH_EDITOR
	Fixture.SetIsTemporarilyHiddenInEditor(bChosen);
#endif
}

void ULvShowSubsystem::FollowShowFiles(UWorld& World)
{
	if (ShowPath.IsEmpty())
	{
		return;
	}
	const FString HiddenFile = FPaths::ChangeExtension(ShowPath, TEXT("hidden.json"));
	const FDateTime HiddenNow = IFileManager::Get().GetTimeStamp(*HiddenFile);
	if (HiddenNow != HiddenStamp)
	{
		HiddenStamp = HiddenNow;
		if (ReadHidden())
		{
			ApplyHidden(World);
		}
	}

	const FString EditsFile = LvRig::EditsPath(ShowPath);
	const FString TypesFile = FPaths::ChangeExtension(ShowPath, TEXT("types.xml"));
	const FDateTime EditsNow = IFileManager::Get().GetTimeStamp(*EditsFile);
	const FDateTime TypesNow = IFileManager::Get().GetTimeStamp(*TypesFile);
	if (EditsNow == EditsStamp && TypesNow == TypesStamp)
	{
		return;
	}
	FString Edits;
	FString Types;
	FFileHelper::LoadFileToString(Edits, *EditsFile);    // a file that is not there reads as empty: nothing changed, no stand-ins
	FFileHelper::LoadFileToString(Types, *TypesFile);
	EditsStamp = EditsNow;
	TypesStamp = TypesNow;
	if (Edits == EditsSeen && Types == TypesSeen)
	{
		return;    // our own save, or a file written again with what it held
	}

	// Someone changed the show from outside: Openviz's Patch or Fixture types panel. The rig is
	// brought in line with the files, fixture by fixture.
	TArray<ALvFixtureActor*> Standing = LiveFixtures();
	FLvRigReport Report;
	FLvRigChanges Changes;
	bReconciling = true;
	const bool bRead = LvRig::Reconcile(World, ShowPath, LibraryFolder, Standing, Report, Changes);
	bReconciling = false;
	if (!bRead)
	{
		EditsStamp = FDateTime::MinValue();    // half written, perhaps: looked at again next time
		return;
	}
	EditsSeen = Edits;
	TypesSeen = Types;
	Fixtures.Reset();
	Hazers = 0;
	int32 LastUniverse = 1;
	for (ALvFixtureActor* Fixture : Standing)
	{
		Fixtures.Add(Fixture);
		Hazers += Fixture->HasFogControl() && !Fixture->IsUnplaced() ? 1 : 0;
		LastUniverse = FMath::Max(LastUniverse, Fixture->GetUniverse() + (Fixture->GetAddress() + Fixture->GetFootprint() - 2) / 512);
	}
	RemovedUuids = Report.RemovedUuids;
	for (ALvFixtureActor* Fixture : Changes.New)
	{
		DressFixture(*Fixture);
	}
	BehindScreens = BehindScreens.Intersect(TSet<TWeakObjectPtr<ALvFixtureActor>>(Fixtures));
	for (ALvFixtureActor* Fixture : Standing)
	{
		ShowOrHide(*Fixture);
	}
	GiveLooks();
	ULvDmxSubsystem::CoverUniverses(FMath::Clamp(LastUniverse, 1, 32767));    // a patch that now reaches further than what is listened to
	if (!ScreensFile.IsEmpty())
	{
		LeaveWallsToScreens(ReadScreenSettings(ScreensFile));    // a fixture built again behind a screen is left out again
	}
	if (bEditing)
	{
		// The fixtures show their new look at once, and what they are now is what the files say: nothing to save.
		for (ALvFixtureActor* Fixture : Changes.New)
		{
			Fixture->Tick(0.0f);
		}
		int32 Edited = 0;
		LastSavedEdits = LvRig::DescribeEdits(LiveFixtures(), RemovedUuids, Edited);
	}
	for (int32 Index = 0; Index < Report.Messages.Num() && Index < 10; ++Index)
	{
		UE_LOG(LogLvShow, Warning, TEXT("LvShow: note: %s"), *Report.Messages[Index]);
	}
	UE_LOG(LogLvShow, Display, TEXT("LvShow: the show was changed in Openviz: %d fixture%s changed, %d built, %d taken away; %d on stage"), Changes.Changed,
		Changes.Changed == 1 ? TEXT("") : TEXT("s"), Changes.New.Num(), Changes.Gone, Fixtures.Num());
}

void ULvShowSubsystem::WatchFrameRate(float DeltaTime)
{
	// Real time, whatever the game's clock does: this is about what the person watching sees.
	const double Seconds = FApp::GetDeltaTime();
	FrameSeconds += Seconds;
	SlowestFrame = FMath::Max(SlowestFrame, Seconds);
	++FrameCount;
	if (Elapsed < NextFrameLook || FrameCount < 2)
	{
		return;
	}
	const double Rate = FrameCount / FMath::Max(FrameSeconds, 1e-6);
	const double Slowest = SlowestFrame;
	NextFrameLook = Elapsed + 2.0;
	FrameSeconds = 0.0;
	SlowestFrame = 0.0;
	FrameCount = 0;
	if (Elapsed < 6.0)
	{
		return;    // the first seconds are loading and compiling, not the picture's pace
	}
	// Beams in haze are most of what a full stage costs to draw, and they are soft: drawn at fewer
	// points and scaled up they lose little. Lowered a step at a time while the picture runs
	// below 30 frames a second; raised again, slowly, once it runs well clear of that.
	if (bAdaptBeams && bConeBeams && !bEditing)
	{
		float Wanted = BeamDetailNow;
		if (Rate < 28.0 && BeamDetailNow > 25.0f)
		{
			Wanted = FMath::Max(25.0f, BeamDetailNow * (Rate < 15.0 ? 0.5f : 0.7f));
		}
		else if (Rate > 50.0 && BeamDetailNow < BeamDetailSetting && Elapsed - LastRaised > 20.0)
		{
			Wanted = FMath::Min(BeamDetailSetting, BeamDetailNow / 0.7f);
			LastRaised = Elapsed;
		}
		if (FMath::Abs(Wanted - BeamDetailNow) > 0.5f)
		{
			UE_LOG(LogLvShow, Display, TEXT("LvShow: picture at %.0f frames a second: beams in haze now drawn at %.0f%% detail (%s)"), Rate, Wanted,
				Wanted < BeamDetailNow ? TEXT("lowered to keep up") : TEXT("raised again"));
			if (Wanted < BeamDetailNow)
			{
				LastRaised = Elapsed;    // not straight back up
			}
			SetBeamDetail(Wanted);
			NextFrameSay = Elapsed + 10.0;
			return;
		}
	}
	if (Elapsed >= NextFrameSay)
	{
		NextFrameSay = Elapsed + 10.0;
		int32 Lit = 0;
		int32 Shaded = 0;
		for (const TWeakObjectPtr<ALvFixtureActor>& Fixture : Fixtures)
		{
			if (Fixture.IsValid() && Fixture->IsActorTickEnabled())
			{
				Lit += Fixture->GetLitBeamCount();
				Shaded += Fixture->GetShadedBeamCount();
			}
		}
		UE_LOG(LogLvShow, Display, TEXT("LvShow: picture: %.0f frames a second (slowest frame %.0f ms); %d beams lit in haze, %d of them with something standing in them; beams at %.0f%% detail"),
			Rate, Slowest * 1000.0, Lit, Shaded, BeamDetailNow);
	}
}

void ULvShowSubsystem::FollowHazers(float DeltaTime)
{
	// Each machine at full would, on its own, double the haze of the default setting (0.06); four
	// or more fill the room. The air follows slowly: toward more haze with a quarter of a
	// minute's time constant, toward less with a minute's.
	double Demand = 0.0;
	for (const TWeakObjectPtr<ALvFixtureActor>& Fixture : Fixtures)
	{
		if (Fixture.IsValid() && !Fixture->IsHidden())
		{
			Demand += Fixture->GetFogOutput();
		}
	}
	const double Target = FMath::Min(Demand, 4.0) * 0.06;
	const double TimeConstant = Target > HazeAdded ? 15.0 : 60.0;
	HazeAdded += (Target - HazeAdded) * (1.0 - FMath::Exp(-static_cast<double>(DeltaTime) / TimeConstant));
	if (Elapsed < NextHazeLook)
	{
		return;
	}
	NextHazeLook = Elapsed + 0.25;
	ApplyHaze();
}

void ULvShowSubsystem::ApplyHaze()
{
	// The setting, and what the rig's own machines have put into the air on top of it: the fog's
	// density, and how much light a cone's air sends toward the eye.
	const double Density = FMath::Clamp(static_cast<double>(HazeSetting) + (bRigHaze ? HazeAdded : 0.0), 0.0, 0.5);
	if (FMath::Abs(Density - HazeApplied) < 2e-4)
	{
		return;
	}
	HazeApplied = Density;
	for (TActorIterator<AExponentialHeightFog> Fog(GetWorld()); Fog; ++Fog)
	{
		Fog->GetComponent()->SetFogDensity(static_cast<float>(Density));
	}
	const float ConeHaze = static_cast<float>(Density) * 0.006f;
	for (const TWeakObjectPtr<ALvFixtureActor>& Fixture : Fixtures)
	{
		if (Fixture.IsValid())
		{
			Fixture->SetBeamStyle(bConeBeams, ConeHaze);
		}
	}
}

void ULvShowSubsystem::SetBeamDetail(float Percent)
{
	BeamDetailNow = FMath::Clamp(Percent, 25.0f, 100.0f);
	if (IConsoleVariable* Variable = IConsoleManager::Get().FindConsoleVariable(TEXT("r.SeparateTranslucencyScreenPercentage")))
	{
		Variable->Set(BeamDetailNow, ECVF_SetByCode);
	}
}

void ULvShowSubsystem::FollowPictureFile(UWorld& World)
{
	if (PictureFile.IsEmpty())
	{
		return;
	}
	const FDateTime Stamp = IFileManager::Get().GetTimeStamp(*PictureFile);
	if (Stamp == PictureStamp)
	{
		return;
	}
	PictureStamp = Stamp;
	FString Text;
	TSharedPtr<FJsonObject> Picture;
	if (!FFileHelper::LoadFileToString(Text, *PictureFile) || !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Picture) || !Picture.IsValid())
	{
		return;    // half written, or not what it should be: looked at again when it next changes
	}
	TArray<FString> Changed;
	double Number = 0.0;
	bool bFlag = false;
	FString Word;
	if (Picture->TryGetNumberField(TEXT("exposure"), Number) && !FMath::IsNearlyEqual(static_cast<float>(Number), ExposureNow, 1e-3f))
	{
		ExposureNow = FMath::Clamp(static_cast<float>(Number), -16.0f, 8.0f);
		LvStage::SetExposure(World, ExposureNow);
		Changed.Add(FString::Printf(TEXT("exposure %g stops"), ExposureNow));
	}
	if (Picture->TryGetNumberField(TEXT("haze"), Number) && !FMath::IsNearlyEqual(static_cast<float>(Number), HazeSetting, 1e-4f))
	{
		HazeSetting = FMath::Clamp(static_cast<float>(Number), 0.0f, 0.5f);
		ApplyHaze();
		Changed.Add(FString::Printf(TEXT("haze %g"), HazeSetting));
	}
	{
		bool bFloor = false;
		if (Picture->TryGetNumberField(TEXT("floor"), Number) && !FMath::IsNearlyEqual(static_cast<float>(Number), FloorRoughness, 1e-3f))
		{
			FloorRoughness = FMath::Clamp(static_cast<float>(Number), 0.0f, 1.0f);
			Changed.Add(FString::Printf(TEXT("floor roughness %g"), FloorRoughness));
			bFloor = true;
		}
		if (Picture->TryGetNumberField(TEXT("floor_shade"), Number) && !FMath::IsNearlyEqual(static_cast<float>(Number), FloorShade, 1e-3f))
		{
			FloorShade = FMath::Clamp(static_cast<float>(Number), 0.0f, 1.0f);
			Changed.Add(FString::Printf(TEXT("floor colour %g"), FloorShade));
			bFloor = true;
		}
		if (bFloor)
		{
			LvStage::SetFloor(World, FloorShade, FloorRoughness);
		}
		bool bLook = false;
		if (Picture->TryGetNumberField(TEXT("bloom"), Number) && !FMath::IsNearlyEqual(static_cast<float>(Number), BloomNow, 1e-3f))
		{
			BloomNow = FMath::Clamp(static_cast<float>(Number), 0.0f, 4.0f);
			Changed.Add(FString::Printf(TEXT("glow %g"), BloomNow));
			bLook = true;
		}
		if (Picture->TryGetBoolField(TEXT("motion_blur"), bFlag) && bFlag != bMotionBlur)
		{
			bMotionBlur = bFlag;
			Changed.Add(bMotionBlur ? TEXT("motion blur on") : TEXT("motion blur off"));
			bLook = true;
		}
		if (bLook)
		{
			LvStage::SetLook(World, BloomNow, bMotionBlur);
		}
	}
	if (Picture->TryGetBoolField(TEXT("rig_haze"), bFlag) && bFlag != bRigHaze)
	{
		bRigHaze = bFlag;
		ApplyHaze();
		Changed.Add(bRigHaze ? TEXT("the rig's hazers add to the haze") : TEXT("the rig's hazers left out of the haze"));
	}
	if (Picture->TryGetNumberField(TEXT("haze_drift"), Number) && !FMath::IsNearlyEqual(static_cast<float>(Number), HazeDriftSetting, 1e-3f))
	{
		HazeDriftSetting = FMath::Clamp(static_cast<float>(Number), 0.0f, 1.0f);
		Changed.Add(FString::Printf(TEXT("drifts in the haze %g"), HazeDriftSetting));
	}
	if (Picture->TryGetStringField(TEXT("beams"), Word) && Word.Equals(TEXT("fog"), ESearchCase::IgnoreCase) == bConeBeams)
	{
		bConeBeams = !Word.Equals(TEXT("fog"), ESearchCase::IgnoreCase);
		HazeApplied = -1.0;    // every fixture is told its beam style with the haze
		ApplyHaze();
		Changed.Add(bConeBeams ? TEXT("beams as shafts") : TEXT("beams as glow"));
	}
	if (Picture->TryGetBoolField(TEXT("beam_shadows"), bFlag) && bFlag != bBeamShadowsOn)
	{
		bBeamShadowsOn = bFlag;
		Changed.Add(bBeamShadowsOn ? TEXT("shadows in beams on") : TEXT("shadows in beams off"));
	}
	if (Picture->TryGetBoolField(TEXT("shadows"), bFlag) && bFlag != bSurfaceShadows)
	{
		bSurfaceShadows = bFlag;
		Changed.Add(bSurfaceShadows ? TEXT("shadows on surfaces on") : TEXT("shadows on surfaces off"));
	}
	if (Changed.Num() > 0)
	{
		for (const TWeakObjectPtr<ALvFixtureActor>& Fixture : Fixtures)
		{
			if (Fixture.IsValid())
			{
				Fixture->SetCastShadows(bSurfaceShadows);
				Fixture->SetBeamShadows(bBeamShadowsOn);
				Fixture->SetHazeDrift(HazeDriftSetting);
			}
		}
	}
	if (Picture->TryGetNumberField(TEXT("beam_detail"), Number) && !FMath::IsNearlyEqual(static_cast<float>(Number), BeamDetailSetting, 0.5f))
	{
		BeamDetailSetting = FMath::Clamp(static_cast<float>(Number), 25.0f, 100.0f);
		SetBeamDetail(BeamDetailSetting);
		Changed.Add(FString::Printf(TEXT("beam detail %.0f%%"), BeamDetailSetting));
	}
	if (Picture->TryGetBoolField(TEXT("adapt_beams"), bFlag) && bFlag != bAdaptBeams)
	{
		bAdaptBeams = bFlag;
		if (!bAdaptBeams)
		{
			SetBeamDetail(BeamDetailSetting);    // back to what was asked for
		}
		Changed.Add(bAdaptBeams ? TEXT("beam detail lowers itself when the picture is slow") : TEXT("beam detail stays as set"));
	}
	if (Picture->TryGetBoolField(TEXT("megalights"), bFlag))
	{
		if (IConsoleVariable* CVar = IConsoleManager::Get().FindConsoleVariable(TEXT("r.MegaLights.EnableForProject")))
		{
			if ((CVar->GetInt() != 0) != bFlag)
			{
				CVar->Set(bFlag ? 1 : 0, ECVF_SetByConsole);
				Changed.Add(bFlag ? TEXT("MegaLights on") : TEXT("MegaLights off"));
			}
		}
	}
	if (!bEditing)
	{
		if (Picture->TryGetNumberField(TEXT("house_lights"), Number) && !FMath::IsNearlyEqual(static_cast<float>(Number), AmbientNow, 1e-3f))
		{
			AmbientNow = FMath::Clamp(static_cast<float>(Number), 0.0f, 1.0f);
			if (AmbientLight.IsValid())
			{
				AmbientLight->GetLightComponent()->SetIntensity(250.0f * AmbientNow);
				AmbientLight->GetLightComponent()->SetCaptureIsDirty();
			}
			else if (AmbientNow > 0.0f)
			{
				SpawnAmbient(World, AmbientNow);
			}
			Changed.Add(FString::Printf(TEXT("house lights %.0f%%"), AmbientNow * 100.0f));
		}
		bool bMouse = false;
		if (Picture->TryGetStringField(TEXT("mouse_look"), Word) && Word.Equals(TEXT("always"), ESearchCase::IgnoreCase) != bAlwaysLook)
		{
			bAlwaysLook = !bAlwaysLook;
			bMouse = true;
		}
		if (Picture->TryGetNumberField(TEXT("look_speed"), Number) && !FMath::IsNearlyEqual(static_cast<float>(Number), LookSpeed, 1e-3f))
		{
			LookSpeed = FMath::Clamp(static_cast<float>(Number), 0.2f, 3.0f);
			Changed.Add(FString::Printf(TEXT("look speed %g"), LookSpeed));
		}
		// Openviz keeps the flying speed in metres a second.
		if (Picture->TryGetNumberField(TEXT("move_speed"), Number) && !FMath::IsNearlyEqual(static_cast<float>(Number * 100.0), MoveSpeed, 0.5f))
		{
			MoveSpeed = FMath::Clamp(static_cast<float>(Number * 100.0), 50.0f, 5000.0f);
			Changed.Add(FString::Printf(TEXT("flying speed %g m/s"), MoveSpeed / 100.0f));
		}
		if (Picture->TryGetBoolField(TEXT("invert_y"), bFlag) && bFlag != bInvertY)
		{
			bInvertY = bFlag;
			Changed.Add(bInvertY ? TEXT("mouse up looks down") : TEXT("mouse up looks up"));
		}
		if (bMouse && bCameraControl)
		{
			if (APlayerController* Controller = World.GetFirstPlayerController())
			{
				ApplyMouseMode(*Controller);    // which says so itself
			}
		}
	}
	if (Changed.Num() > 0)
	{
		UE_LOG(LogLvShow, Display, TEXT("LvShow: picture settings changed in Openviz: %s"), *FString::Join(Changed, TEXT(", ")));
	}
}

void ULvShowSubsystem::HideByLayerAndClass(AActor& Actor) const
{
	Actor.SetActorHiddenInGame(true);
	Actor.SetActorTickEnabled(false);    // a fixture that never ticks never lights its beams
	Actor.SetActorEnableCollision(false);
#if WITH_EDITOR
	Actor.SetIsTemporarilyHiddenInEditor(true);
#endif
}

namespace
{
	// What Openviz chose for the screens: {"screens": [{"id", "source", "value", "nits"}, and for
	// one that was added also "added": true, "name", "width", "height", "x", "y", "z", "turn"}]}.
	// "part": [left, top, right, bottom], the part of its picture a screen or a section shows.
	void ReadPart(const FJsonObject& Object, FVector4& OutPart)
	{
		const TArray<TSharedPtr<FJsonValue>>* Edges = nullptr;
		if (Object.TryGetArrayField(TEXT("part"), Edges) && Edges->Num() == 4)
		{
			double Edge[4];
			for (int32 Index = 0; Index < 4; ++Index)
			{
				Edge[Index] = (*Edges)[Index].IsValid() ? (*Edges)[Index]->AsNumber() : 0.0;
			}
			OutPart = FVector4(Edge[0], Edge[1], Edge[2], Edge[3]);
		}
	}

	// "map": [ox, oy, ux, uy, vx, vy], a part that is not upright in the picture.
	void ReadMap(const FJsonObject& Object, bool& bOutMap, double OutMap[6])
	{
		const TArray<TSharedPtr<FJsonValue>>* Numbers = nullptr;
		if (Object.TryGetArrayField(TEXT("map"), Numbers) && Numbers->Num() == 6)
		{
			bOutMap = true;
			for (int32 Index = 0; Index < 6; ++Index)
			{
				OutMap[Index] = (*Numbers)[Index].IsValid() ? (*Numbers)[Index]->AsNumber() : 0.0;
			}
		}
	}

	TArray<FLvScreenSetting> ReadScreenSettings(const FString& Path)
	{
		TArray<FLvScreenSetting> Settings;
		FString Text;
		TSharedPtr<FJsonObject> Root;
		const TArray<TSharedPtr<FJsonValue>>* Listed = nullptr;
		if (Path.IsEmpty() || !FFileHelper::LoadFileToString(Text, *Path) || !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Root) ||
			!Root.IsValid() || !Root->TryGetArrayField(TEXT("screens"), Listed))
		{
			return Settings;
		}
		for (const TSharedPtr<FJsonValue>& Entry : *Listed)
		{
			const TSharedPtr<FJsonObject>* Written = nullptr;
			if (!Entry.IsValid() || !Entry->TryGetObject(Written) || !Written->IsValid())
			{
				continue;
			}
			const FJsonObject& Object = **Written;
			FLvScreenSetting Setting;
			Object.TryGetStringField(TEXT("id"), Setting.Id);
			Object.TryGetBoolField(TEXT("added"), Setting.bAdded);
			Object.TryGetStringField(TEXT("name"), Setting.Name);
			Object.TryGetNumberField(TEXT("width"), Setting.Width);
			Object.TryGetNumberField(TEXT("height"), Setting.Height);
			Object.TryGetNumberField(TEXT("x"), Setting.X);
			Object.TryGetNumberField(TEXT("y"), Setting.Y);
			Object.TryGetNumberField(TEXT("z"), Setting.Z);
			Object.TryGetNumberField(TEXT("turn"), Setting.Turn);
			Object.TryGetStringField(TEXT("source"), Setting.Source);
			Object.TryGetStringField(TEXT("value"), Setting.Value);
			Object.TryGetNumberField(TEXT("nits"), Setting.Nits);
			ReadPart(Object, Setting.Part);
			ReadMap(Object, Setting.bMap, Setting.Map);
			const TArray<TSharedPtr<FJsonValue>>* Numbers = nullptr;
			if (Object.TryGetArrayField(TEXT("face"), Numbers) && Numbers->Num() == 12)
			{
				Setting.bFace = true;
				for (int32 Index = 0; Index < 12; ++Index)
				{
					Setting.Face[Index] = (*Numbers)[Index].IsValid() ? (*Numbers)[Index]->AsNumber() : 0.0;
				}
			}
			const TArray<TSharedPtr<FJsonValue>>* Replaced = nullptr;
			if (Object.TryGetArrayField(TEXT("replaces"), Replaced))
			{
				for (const TSharedPtr<FJsonValue>& Uuid : *Replaced)
				{
					FString Whose;
					if (Uuid.IsValid() && Uuid->TryGetString(Whose) && !Whose.IsEmpty())
					{
						Setting.Replaces.Add(Whose);
					}
				}
			}
			const TArray<TSharedPtr<FJsonValue>>* Pieces = nullptr;
			if (Object.TryGetNumberField(TEXT("columns"), Setting.Columns) && Object.TryGetNumberField(TEXT("rows"), Setting.Rows) &&
				Object.TryGetArrayField(TEXT("sections"), Pieces))
			{
				for (const TSharedPtr<FJsonValue>& Piece : *Pieces)
				{
					const TSharedPtr<FJsonObject>* Described = nullptr;
					FLvScreenSection Section;
					if (Piece.IsValid() && Piece->TryGetObject(Described) && Described->IsValid())
					{
						(*Described)->TryGetStringField(TEXT("source"), Section.Source);
						(*Described)->TryGetStringField(TEXT("value"), Section.Value);
						(*Described)->TryGetNumberField(TEXT("nits"), Section.Nits);
						ReadPart(**Described, Section.Part);
						ReadMap(**Described, Section.bMap, Section.Map);
					}
					Setting.Sections.Add(MoveTemp(Section));
				}
				if (!Setting.IsSplit() || Setting.Columns > 32 || Setting.Rows > 32)
				{
					Setting.Columns = Setting.Rows = 0;    // a grid that does not add up: one piece
					Setting.Sections.Reset();
				}
			}
			if (!Setting.Id.IsEmpty())
			{
				Settings.Add(MoveTemp(Setting));
			}
		}
		return Settings;
	}

	// Which screens there are and where they stand, as far as the settings decide it.
	FString ShapeOf(const TArray<FLvScreenSetting>& Settings)
	{
		FString Shape;
		for (const FLvScreenSetting& Setting : Settings)
		{
			Shape += Setting.bAdded
				? FString::Printf(TEXT("+%s|%s|%.1f|%.1f|%.1f|%.1f|%.1f|%.2f"), *Setting.Id, *Setting.Name, Setting.Width, Setting.Height, Setting.X, Setting.Y, Setting.Z, Setting.Turn)
				: FString::Printf(TEXT("=%s"), *Setting.Id.ToUpper());
			if (Setting.bFace)
			{
				for (double Number : Setting.Face)
				{
					Shape += FString::Printf(TEXT("|%.3f"), Number);
				}
			}
			// Split into a grid, its sections are screens too.
			Shape += Setting.IsSplit() ? FString::Printf(TEXT("|%dx%d;"), Setting.Columns, Setting.Rows) : TEXT(";");
		}
		return Shape;
	}
}

TArray<ALvScreenActor*> ULvShowSubsystem::SpawnScreens(UWorld& World, const FString& MvrPath)
{
	ScreensFile.Reset();
	if (!FParse::Value(FCommandLine::Get(), TEXT("LvScreens="), ScreensFile, false) && !MvrPath.IsEmpty())
	{
		ScreensFile = FPaths::ChangeExtension(MvrPath, TEXT("screens.json"));
	}
	ScreensStamp = ScreensFile.IsEmpty() ? FDateTime::MinValue() : IFileManager::Get().GetTimeStamp(*ScreensFile);
	const TArray<FLvScreenSetting> Settings = ReadScreenSettings(ScreensFile);
	ScreensShape = ShapeOf(Settings);

	FLvScreenReport Report;
	const TArray<ALvScreenActor*> Spawned = LvRig::SpawnScreens(World, MvrPath, Settings, Report);
	ScreensPlaced.Reset();
	for (ALvScreenActor* Screen : Spawned)
	{
		// A whole screen added in Openviz can be moved in the editor: where it was put is kept, to tell.
		if (bEditing && !Screen->ScreenId.Contains(TEXT("#")) &&
			Settings.ContainsByPredicate([Screen](const FLvScreenSetting& Setting) { return Setting.bAdded && Setting.Id == Screen->ScreenId; }))
		{
			ScreensPlaced.Add(Screen, Screen->GetActorTransform());
		}
		Screens.Add(Screen);
		ScreensSaid.Add(FString());
		if (IsHiddenByChoice(Screen->Layer, Screen->Class))
		{
			Screen->Show(TEXT("off"), FString(), 0.0f);    // a hidden screen plays nothing either
			HideByLayerAndClass(*Screen);
		}
	}
	if (Spawned.Num() > 0)
	{
		bScreensSettled = false;
		const int32 Whole = Spawned.Num() - Report.Sections;
		UE_LOG(LogLvShow, Display, TEXT("LvShow: %d screen%s: %d of the rig file, %d added in Openviz%s"), Whole, Whole == 1 ? TEXT("") : TEXT("s"),
			Report.FromRig, Report.Added, Report.Sections > 0 ? *FString::Printf(TEXT("; %d sections among them"), Report.Sections) : TEXT(""));
	}
	for (const FString& Message : Report.Messages)
	{
		UE_LOG(LogLvShow, Warning, TEXT("LvShow: note: %s"), *Message);
	}
	LeaveWallsToScreens(Settings);
	return Spawned;
}

void ULvShowSubsystem::LeaveWallsToScreens(const TArray<FLvScreenSetting>& Settings)
{
	// A screen that stands where fixtures hang (LED panels, patched one by one) takes their place
	// wherever it shows a picture of its own, and gives it back wherever it is set to "fixtures".
	TSet<ALvFixtureActor*> Covered;
	for (const FLvScreenSetting& Setting : Settings)
	{
		if (Setting.Replaces.Num() == 0)
		{
			continue;
		}
		const TWeakObjectPtr<ALvScreenActor>* Whole = Screens.FindByPredicate([&Setting](const TWeakObjectPtr<ALvScreenActor>& Screen)
			{ return Screen.IsValid() && Screen->ScreenId.Equals(Setting.Id, ESearchCase::IgnoreCase); });
		if (!Whole || (*Whole)->IsHidden() || (*Whole)->Size.X <= 0.0 || (*Whole)->Size.Y <= 0.0)
		{
			continue;
		}
		for (const FString& Uuid : Setting.Replaces)
		{
			const TWeakObjectPtr<ALvFixtureActor>* Fixture = Fixtures.FindByPredicate([&Uuid](const TWeakObjectPtr<ALvFixtureActor>& Candidate)
				{ return Candidate.IsValid() && Candidate->SourceUuid.Equals(Uuid, ESearchCase::IgnoreCase); });
			if (!Fixture)
			{
				continue;
			}
			FString Source = Setting.Source;
			if (Setting.IsSplit())
			{
				// The section in front of the fixture's middle: the screen's X runs to the picture's right, its Y down it.
				const FVector Middle = (*Whole)->GetActorTransform().InverseTransformPosition((*Fixture)->GetComponentsBoundingBox(true).GetCenter());
				const int32 Column = FMath::Clamp(FMath::FloorToInt32((Middle.X / (*Whole)->Size.X + 0.5) * Setting.Columns), 0, Setting.Columns - 1);
				const int32 Row = FMath::Clamp(FMath::FloorToInt32((Middle.Y / (*Whole)->Size.Y + 0.5) * Setting.Rows), 0, Setting.Rows - 1);
				Source = Setting.Sections[Row * Setting.Columns + Column].Source;
			}
			if (Source != TEXT("fixtures"))
			{
				Covered.Add(Fixture->Get());
			}
		}
	}
	int32 Changed = 0;
	for (const TWeakObjectPtr<ALvFixtureActor>& Fixture : Fixtures)
	{
		if (!Fixture.IsValid())
		{
			continue;
		}
		const bool bWas = BehindScreens.Contains(Fixture);
		const bool bNow = Covered.Contains(Fixture.Get());
		if (bWas == bNow)
		{
			continue;
		}
		if (bNow)
		{
			BehindScreens.Add(Fixture);
		}
		else
		{
			BehindScreens.Remove(Fixture);
		}
		ShowOrHide(*Fixture);    // which also knows of its layer and class: one hidden by those stays hidden
		++Changed;
	}
	if (Changed > 0)
	{
		UE_LOG(LogLvShow, Display, TEXT("LvShow: %d fixture%s behind screens that show a picture of their own, and are left out"), BehindScreens.Num(),
			BehindScreens.Num() == 1 ? TEXT(" is") : TEXT("s are"));
	}
}

void ULvShowSubsystem::SaveScreenMoves()
{
	// What was moved, turned or scaled in the editor since it was put there, by the screen's id.
	TMap<FString, TSharedPtr<FJsonObject>> Moved;
	for (TPair<TWeakObjectPtr<ALvScreenActor>, FTransform>& Placed : ScreensPlaced)
	{
		ALvScreenActor* Screen = Placed.Key.Get();
		if (!Screen || Screen->GetActorTransform().Equals(Placed.Value, 0.01))
		{
			continue;
		}
		const FTransform Now = Screen->GetActorTransform();
		const FVector Right = Now.GetUnitAxis(EAxis::X);
		const FVector Down = Now.GetUnitAxis(EAxis::Y);
		const FVector Out = Now.GetUnitAxis(EAxis::Z);
		const FVector At = Now.GetLocation();
		const FVector Scale = Now.GetScale3D().GetAbs();
		// Back into the rig file's terms: millimetres, Y mirrored, the picture's up rather than down.
		const double Face[12] = {Right.X, -Right.Y, Right.Z, -Down.X, Down.Y, -Down.Z, Out.X, -Out.Y, Out.Z, At.X * 10.0, -At.Y * 10.0, At.Z * 10.0};
		const double Width = Screen->Size.X * 10.0 * Scale.X;
		const double Height = Screen->Size.Y * 10.0 * Scale.Y;
		TSharedPtr<FJsonObject> Place = MakeShared<FJsonObject>();
		TArray<TSharedPtr<FJsonValue>> Numbers;
		for (const double Number : Face)
		{
			Numbers.Add(MakeShared<FJsonValueNumber>(FMath::RoundToDouble(Number * 1e6) / 1e6));
		}
		Place->SetArrayField(TEXT("face"), Numbers);
		Place->SetNumberField(TEXT("width"), FMath::RoundToDouble(Width * 10.0) / 10.0);
		Place->SetNumberField(TEXT("height"), FMath::RoundToDouble(Height * 10.0) / 10.0);
		// For Openviz's page and the plot, which list a screen by the middle of its foot and its turn about the vertical.
		Place->SetNumberField(TEXT("x"), FMath::RoundToDouble(Face[9] * 10.0) / 10.0);
		Place->SetNumberField(TEXT("y"), FMath::RoundToDouble(Face[10] * 10.0) / 10.0);
		Place->SetNumberField(TEXT("z"), FMath::RoundToDouble((Face[11] - Height / 2.0 * Face[5]) * 10.0) / 10.0);
		if (FMath::Abs(Face[6]) + FMath::Abs(Face[7]) > 0.3)
		{
			Place->SetNumberField(TEXT("turn"), FMath::RoundToDouble(FMath::RadiansToDegrees(FMath::Atan2(Face[6], -Face[7])) * 100.0) / 100.0);
		}
		Moved.Add(Screen->ScreenId, Place);
		Placed.Value = Now;
	}
	if (Moved.Num() == 0 || ScreensFile.IsEmpty())
	{
		return;
	}
	FString Text;
	TSharedPtr<FJsonObject> Root;
	const TArray<TSharedPtr<FJsonValue>>* Listed = nullptr;
	if (!FFileHelper::LoadFileToString(Text, *ScreensFile) || !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Root) || !Root.IsValid() ||
		!Root->TryGetArrayField(TEXT("screens"), Listed))
	{
		return;
	}
	int32 Saved = 0;
	for (const TSharedPtr<FJsonValue>& Entry : *Listed)
	{
		const TSharedPtr<FJsonObject>* Object = nullptr;
		FString Id;
		if (!Entry.IsValid() || !Entry->TryGetObject(Object) || !(*Object)->TryGetStringField(TEXT("id"), Id))
		{
			continue;
		}
		if (const TSharedPtr<FJsonObject>* Place = Moved.Find(Id))
		{
			for (const TPair<FString, TSharedPtr<FJsonValue>>& Field : (*Place)->Values)
			{
				(*Object)->SetField(Field.Key, Field.Value);
			}
			FString Name;
			(*Object)->TryGetStringField(TEXT("name"), Name);
			UE_LOG(LogLvShow, Display, TEXT("LvShow: screen %s moved in the editor: kept in the screens file"), *Name);
			++Saved;
		}
	}
	FString Written;
	if (Saved == 0 || !FJsonSerializer::Serialize(Root.ToSharedRef(), TJsonWriterFactory<>::Create(&Written)) || !FFileHelper::SaveStringToFile(Written, *ScreensFile))
	{
		return;
	}
	// Ours, not Openviz's: nothing to build again.
	ScreensStamp = IFileManager::Get().GetTimeStamp(*ScreensFile);
	ScreensShape = ShapeOf(ReadScreenSettings(ScreensFile));
}

void ULvShowSubsystem::FollowScreensFile(UWorld& World)
{
	// Openviz writes the file whenever something about a screen is changed there: what was
	// changed is taken up while the show runs.
	const FDateTime Stamp = IFileManager::Get().GetTimeStamp(*ScreensFile);
	if (Stamp == ScreensStamp)
	{
		return;
	}
	ScreensStamp = Stamp;
	const TArray<FLvScreenSetting> Settings = ReadScreenSettings(ScreensFile);
	const FString Shape = ShapeOf(Settings);
	if (Shape != ScreensShape)
	{
		// A screen more or less, or one that was moved: all of them are built again.
		for (const TWeakObjectPtr<ALvScreenActor>& Screen : Screens)
		{
			if (Screen.IsValid())
			{
				Screen->Destroy();
			}
		}
		Screens.Reset();
		ScreensSaid.Reset();
		SpawnScreens(World, ShowPath);
		return;
	}
	// What each screen, and each section that is a screen, should show now: by its id.
	struct FWanted
	{
		FString Source;
		FString Value;
		float Nits;
		FVector4 Part;
		bool bMap = false;
		double Map[6] = {0.0, 0.0, 1.0, 0.0, 0.0, 1.0};
	};
	TMap<FString, FWanted> Wanted;
	for (const FLvScreenSetting& Setting : Settings)
	{
		if (!Setting.IsSplit())
		{
			FWanted& Whole = Wanted.Add(Setting.Id.ToUpper(), FWanted{Setting.Source, Setting.Value, Setting.Nits, Setting.Part});
			Whole.bMap = Setting.bMap;
			FMemory::Memcpy(Whole.Map, Setting.Map, sizeof(Whole.Map));
			continue;
		}
		Wanted.Add(Setting.Id.ToUpper(), FWanted{TEXT("sections"), FString(), 0.0f, FVector4(0.0, 0.0, 1.0, 1.0)});
		for (int32 Piece = 0; Piece < Setting.Sections.Num(); ++Piece)
		{
			const FLvScreenSection& Section = Setting.Sections[Piece];
			FWanted& One = Wanted.Add(LvSectionId(Setting.Id, Piece).ToUpper(), FWanted{Section.Source, Section.Value, Section.Nits, Section.Part});
			One.bMap = Section.bMap;
			FMemory::Memcpy(One.Map, Section.Map, sizeof(One.Map));
		}
	}
	int32 Changed = 0;
	for (int32 Index = 0; Index < Screens.Num(); ++Index)
	{
		ALvScreenActor* Screen = Screens[Index].Get();
		if (!Screen || Screen->IsHidden())
		{
			continue;
		}
		const FWanted* Chosen = Wanted.Find(Screen->ScreenId.ToUpper());
		if (!Chosen)
		{
			continue;
		}
		if (Chosen->Source != Screen->Source || Chosen->Value != Screen->SourceValue || !FMath::IsNearlyEqual(Chosen->Nits, Screen->Nits, 0.01f))
		{
			const bool bOther = Chosen->Source != Screen->Source || Chosen->Value != Screen->SourceValue;
			Screen->Show(Chosen->Source, Chosen->Value, Chosen->Nits);
			if (bOther)
			{
				ScreensSaid[Index].Reset();    // say what it shows now
			}
			++Changed;
		}
		if (!Screen->GetPart().Equals(Chosen->Part, 1e-5))
		{
			Screen->SetPart(Chosen->Part);
		}
		if (!Screen->HasMap(Chosen->bMap, Chosen->Map))
		{
			Screen->SetMap(Chosen->bMap, Chosen->Map);
		}
	}
	LeaveWallsToScreens(Settings);
	if (Changed > 0)
	{
		bScreensSettled = false;
	}
}

void ULvShowSubsystem::ReportScreens()
{
	// A film or a stream takes a moment to open (an NDI stream some ten seconds): say what each
	// screen shows once that is known, and after half a minute whatever is still not up. A
	// stream is watched for as long as the show runs: it may start, or stop, at any time.
	bool bWatch = false;
	for (int32 Index = 0; Index < Screens.Num(); ++Index)
	{
		const ALvScreenActor* Screen = Screens[Index].Get();
		if (!Screen || Screen->IsHidden())
		{
			continue;
		}
		const bool bDone = Screen->IsSettled();
		bWatch = bWatch || !bDone || Screen->Source == TEXT("ndi") || Screen->Source == TEXT("pixels");    // either may start, or stop, at any time
		if (!bDone && Elapsed < 30.0)
		{
			continue;
		}
		const FString Says = Screen->Describe();
		if (Says != ScreensSaid[Index])
		{
			ScreensSaid[Index] = Says;
			const bool bGood = Says.StartsWith(TEXT("showing")) || Says.StartsWith(TEXT("playing")) || Says == TEXT("dark") ||
				(Says.StartsWith(TEXT("lit pixel by pixel")) && !Says.EndsWith(TEXT("yet")));
			if (bGood)
			{
				UE_LOG(LogLvShow, Display, TEXT("LvShow: screen %s: %s"), *Screen->ScreenName, *Says);
			}
			else
			{
				UE_LOG(LogLvShow, Warning, TEXT("LvShow: note: screen %s: %s"), *Screen->ScreenName, *Says);
			}
		}
	}
	bScreensSettled = !bWatch;
}

void ULvShowSubsystem::SpawnPreview(UWorld& World, const FString& GdtfPath)
{
	FString Error;
	ULvFixtureLibrary* Library = ULvFixtureLibrary::Get();
	TSharedPtr<const FLvFixtureDefinition> Definition = Library ? Library->LoadGdtfFile(GdtfPath, Error) : nullptr;
	if (!Definition || Definition->Type.modes.empty())
	{
		UE_LOG(LogLvShow, Error, TEXT("LvShow: note: %s could not be opened: %s"), *FPaths::GetCleanFilename(GdtfPath),
			Error.IsEmpty() ? TEXT("it has no DMX modes") : *Error);
		return;
	}
	FString ModeName;
	FParse::Value(FCommandLine::Get(), TEXT("LvShowGdtfMode="), ModeName, false);
	int32 Mode = ModeName.IsEmpty() ? 0 : Definition->FindMode(ModeName);
	if (Mode < 0)
	{
		UE_LOG(LogLvShow, Warning, TEXT("LvShow: note: no mode '%s'; the first mode is used"), *ModeName);
		Mode = 0;
	}

	// A row hanging at 5 m, 1.5 m apart, patched one after another from 1.1.
	const int32 Count = FMath::Clamp(IntOption(TEXT("LvShowCount="), 6), 1, 64);
	int32 Universe = 1;
	int32 Address = 1;
	for (int32 Index = 0; Index < Count; ++Index)
	{
		const FVector Location((Index - (Count - 1) * 0.5) * 150.0, 0.0, 500.0);
		ALvFixtureActor* Fixture = World.SpawnActor<ALvFixtureActor>(Location, FRotator::ZeroRotator);
		if (!Fixture || !Fixture->Initialize(Definition, Mode, Universe, Address))
		{
			UE_LOG(LogLvShow, Error, TEXT("LvShow: note: the fixture could not be built from %s"), *FPaths::GetCleanFilename(GdtfPath));
			if (Fixture)
			{
				Fixture->Destroy();
			}
			return;
		}
		Fixtures.Add(Fixture);
		const int32 Footprint = FMath::Max(Fixture->GetFootprint(), 1);
		Address += Footprint;
		if (Address + Footprint - 1 > 512)
		{
			++Universe;
			Address = 1;
		}
	}
	UE_LOG(LogLvShow, Display, TEXT("LvShow: opened %s: %d fixtures built, 0 skipped"), *FPaths::GetCleanFilename(GdtfPath), Fixtures.Num());
}

void ULvShowSubsystem::ApplyMouseMode(APlayerController& Controller)
{
	if (bAlwaysLook)
	{
		// Game style: the window keeps the mouse, there is no pointer, and moving it always looks.
		Controller.SetInputMode(FInputModeGameOnly());
		Controller.bShowMouseCursor = false;
	}
	else
	{
		// The pointer is free. While a button is held over the picture it stays put and its
		// movement is ours to use.
		FInputModeGameAndUI Mode;
		Mode.SetLockMouseToViewportBehavior(EMouseLockMode::DoNotLock);
		Mode.SetHideCursorDuringCapture(true);
		Controller.SetInputMode(Mode);
		Controller.bShowMouseCursor = true;
	}
	UE_LOG(LogLvShow, Display, TEXT("LvShow: mouse: %s"), bAlwaysLook
		? TEXT("always looks, game style (Tab to free the pointer)")
		: TEXT("hold the right button to look around; middle button slides the view; wheel moves in and out (Tab for game style)"));
}

void ULvShowSubsystem::UpdateCamera()
{
	APlayerController* Controller = GetWorld()->GetFirstPlayerController();
	APawn* Pawn = Controller ? Controller->GetPawn() : nullptr;
	if (!Pawn)
	{
		return;
	}
	if (Controller->WasInputKeyJustPressed(EKeys::Tab))
	{
		bAlwaysLook = !bAlwaysLook;
		ApplyMouseMode(*Controller);
	}

	// Stored views. A tap on a number key goes to the view stored under it; holding the key
	// for most of a second stores the present view there instead, as does the key with Shift,
	// Control or Command. The keypad's numbers count too. Every press is logged, so that a report
	// of a key doing nothing can be read against what arrived.
	static const FKey Numbers[10] = {EKeys::Zero, EKeys::One, EKeys::Two, EKeys::Three, EKeys::Four, EKeys::Five, EKeys::Six, EKeys::Seven, EKeys::Eight, EKeys::Nine};
	static const FKey Keypad[10] = {EKeys::NumPadZero, EKeys::NumPadOne, EKeys::NumPadTwo, EKeys::NumPadThree, EKeys::NumPadFour, EKeys::NumPadFive,
	                                EKeys::NumPadSix, EKeys::NumPadSeven, EKeys::NumPadEight, EKeys::NumPadNine};
	const bool bModifier = Controller->IsInputKeyDown(EKeys::LeftShift) || Controller->IsInputKeyDown(EKeys::RightShift) ||
		Controller->IsInputKeyDown(EKeys::LeftControl) || Controller->IsInputKeyDown(EKeys::RightControl) ||
		Controller->IsInputKeyDown(EKeys::LeftCommand) || Controller->IsInputKeyDown(EKeys::RightCommand);
	const double HoldToStore = 0.8;
	for (int32 Number = 0; Number < 10; ++Number)
	{
		const bool bPressed = Controller->WasInputKeyJustPressed(Numbers[Number]) || Controller->WasInputKeyJustPressed(Keypad[Number]);
		const bool bDown = Controller->IsInputKeyDown(Numbers[Number]) || Controller->IsInputKeyDown(Keypad[Number]);
		const bool bReleased = Controller->WasInputKeyJustReleased(Numbers[Number]) || Controller->WasInputKeyJustReleased(Keypad[Number]);
		if (bPressed)
		{
			UE_LOG(LogLvShow, Display, TEXT("LvShow: key %d pressed%s"), Number, bModifier ? TEXT(" with Shift, Control or Command") : TEXT(""));
			NumberHeldSince[Number] = Elapsed;
			bNumberStored[Number] = false;
			if (bModifier && Number > 0)
			{
				StoreView(Number);
				bNumberStored[Number] = true;
			}
		}
		else if (NumberHeldSince[Number] >= 0.0 && bDown && !bNumberStored[Number] && Number > 0)
		{
			const double Held = Elapsed - NumberHeldSince[Number];
			if (Held >= HoldToStore)
			{
				StoreView(Number);
				bNumberStored[Number] = true;
			}
			else if (Held >= 0.25)
			{
				GEngine->AddOnScreenDebugMessage(0x4C77, 0.5f, FColor(170, 170, 170), FString::Printf(TEXT("Keep holding %d to store this view there"), Number));
			}
		}
		if (bReleased && NumberHeldSince[Number] >= 0.0)
		{
			if (!bNumberStored[Number] && Elapsed - NumberHeldSince[Number] < HoldToStore)
			{
				RecallView(Number);
			}
			NumberHeldSince[Number] = -1.0;
		}
	}
	if (Controller->WasInputKeyJustPressed(EKeys::P))
	{
		TakePicture();
	}
	// The rig as a plot shows it: from above, from the house, from stage left.
	if (Controller->WasInputKeyJustPressed(EKeys::T))
	{
		PlanView(TEXT("top"));
	}
	else if (Controller->WasInputKeyJustPressed(EKeys::F))
	{
		PlanView(TEXT("front"));
	}
	else if (Controller->WasInputKeyJustPressed(EKeys::L))
	{
		PlanView(TEXT("side"));
	}
	if (Controller->WasInputKeyJustPressed(EKeys::M))
	{
		SetMeter(TEXT("toggle"));
	}
	if (bPlan)
	{
		// A flat view is slid and zoomed, not flown: the wheel narrows and widens the lens, W A S D
		// and the middle button slide the picture. Turning the view would lean it, so it is left alone.
		bGliding = false;
		const FRotationMatrix Plane(Controller->GetControlRotation());
		const double Across = 2.0 * PlanDistance * FMath::Tan(FMath::DegreesToRadians(PlanFov * 0.5));    // what the picture spans, side to side
		if (Controller->WasInputKeyJustPressed(EKeys::MouseScrollUp) || Controller->WasInputKeyJustPressed(EKeys::MouseScrollDown))
		{
			PlanFov = FMath::Clamp(PlanFov * (Controller->WasInputKeyJustPressed(EKeys::MouseScrollUp) ? 0.87f : 1.15f), 0.3f, 60.0f);
			if (Controller->PlayerCameraManager)
			{
				Controller->PlayerCameraManager->SetFOV(PlanFov);
			}
		}
		float SlideX = 0.0f;
		float SlideY = 0.0f;
		Controller->GetInputMouseDelta(SlideX, SlideY);
		FVector Slide = FVector::ZeroVector;
		if (Controller->IsInputKeyDown(EKeys::MiddleMouseButton))
		{
			Slide += (Plane.GetUnitAxis(EAxis::Y) * -SlideX + Plane.GetUnitAxis(EAxis::Z) * -SlideY) * (Across / 400.0);
		}
		const double Step = Across * 0.6 * GetWorld()->GetDeltaSeconds() * (Controller->IsInputKeyDown(EKeys::LeftShift) || Controller->IsInputKeyDown(EKeys::RightShift) ? 3.0 : 1.0);
		Slide += Plane.GetUnitAxis(EAxis::Y) * (((Controller->IsInputKeyDown(EKeys::D) ? 1.0 : 0.0) - (Controller->IsInputKeyDown(EKeys::A) ? 1.0 : 0.0)) * Step);
		Slide += Plane.GetUnitAxis(EAxis::Z) * (((Controller->IsInputKeyDown(EKeys::W) ? 1.0 : 0.0) - (Controller->IsInputKeyDown(EKeys::S) ? 1.0 : 0.0)) * Step);
		Pawn->AddActorWorldOffset(Slide);
		if (UFloatingPawnMovement* Movement = Pawn->FindComponentByClass<UFloatingPawnMovement>())
		{
			Movement->MaxSpeed = 0.0f;    // the pawn's own flying would carry the view off along its line of sight
			Movement->StopMovementImmediately();
		}
		return;
	}
	if (bGliding)
	{
		// Half a second, easing in and out. Any flying of one's own takes over at once.
		const double Along = FMath::Clamp((Elapsed - GlideStarted) / 0.5, 0.0, 1.0);
		const double Eased = Along * Along * (3.0 - 2.0 * Along);
		Pawn->SetActorLocation(FMath::Lerp(GlideFrom, GlideEnd, Eased));
		Controller->SetControlRotation(FQuat::Slerp(GlideFromTurn, GlideEndTurn, Eased).Rotator());
		const bool bFlying = Controller->IsInputKeyDown(EKeys::W) || Controller->IsInputKeyDown(EKeys::A) || Controller->IsInputKeyDown(EKeys::S) ||
			Controller->IsInputKeyDown(EKeys::D) || Controller->IsInputKeyDown(EKeys::Q) || Controller->IsInputKeyDown(EKeys::E) ||
			Controller->IsInputKeyDown(EKeys::RightMouseButton) || Controller->IsInputKeyDown(EKeys::MiddleMouseButton);
		if (Along >= 1.0 || bFlying)
		{
			bGliding = false;
		}
		return;
	}

	float X = 0.0f;
	float Y = 0.0f;
	Controller->GetInputMouseDelta(X, Y);
	const FRotationMatrix Axes(Controller->GetControlRotation());
	if (bAlwaysLook || Controller->IsInputKeyDown(EKeys::RightMouseButton))
	{
		Controller->SetControlRotation(TurnView(Controller->GetControlRotation(), X, Y, LookSpeed, bInvertY));
	}
	else if (Controller->IsInputKeyDown(EKeys::MiddleMouseButton))
	{
		// The picture follows the hand: drag right and the stage slides right.
		const double PerCount = MoveSpeed / 600.0 * 20.0;
		Pawn->AddActorWorldOffset(Axes.GetUnitAxis(EAxis::Y) * (-X * PerCount) + Axes.GetUnitAxis(EAxis::Z) * (-Y * PerCount));
	}
	if (Controller->WasInputKeyJustPressed(EKeys::MouseScrollUp))
	{
		Pawn->AddActorWorldOffset(Axes.GetUnitAxis(EAxis::X) * (MoveSpeed * 0.3));
	}
	if (Controller->WasInputKeyJustPressed(EKeys::MouseScrollDown))
	{
		Pawn->AddActorWorldOffset(Axes.GetUnitAxis(EAxis::X) * (-MoveSpeed * 0.3));
	}

	// W A S D, Q and E are the default pawn's own; only its speed is set here.
	if (UFloatingPawnMovement* Movement = Pawn->FindComponentByClass<UFloatingPawnMovement>())
	{
		const bool bFast = Controller->IsInputKeyDown(EKeys::LeftShift) || Controller->IsInputKeyDown(EKeys::RightShift);
		Movement->MaxSpeed = MoveSpeed * (bFast ? 4.0f : 1.0f);
		Movement->Acceleration = Movement->MaxSpeed * 6.0f;
		Movement->Deceleration = Movement->MaxSpeed * 10.0f;
	}
}

FString ULvShowSubsystem::ViewsPath() const
{
	return ShowPath.IsEmpty() ? FString() : FPaths::ChangeExtension(ShowPath, TEXT("views.txt"));
}

void ULvShowSubsystem::LoadViews()
{
	// One line a view: its number, where it is, and pitch, yaw and roll, in centimetres and degrees.
	TArray<FString> Lines;
	if (ViewsPath().IsEmpty() || !FFileHelper::LoadFileToStringArray(Lines, *ViewsPath()))
	{
		return;
	}
	int32 Found = 0;
	for (const FString& Line : Lines)
	{
		TArray<FString> Parts;
		Line.ParseIntoArrayWS(Parts);
		const int32 Number = Parts.Num() == 7 ? FCString::Atoi(*Parts[0]) : 0;
		if (Number < 1 || Number > 9)
		{
			continue;
		}
		double Values[6];
		bool bNumbers = true;
		for (int32 Index = 0; Index < 6; ++Index)
		{
			bNumbers = bNumbers && Parts[Index + 1].IsNumeric();
			Values[Index] = FCString::Atod(*Parts[Index + 1]);
			bNumbers = bNumbers && FMath::IsFinite(Values[Index]) && FMath::Abs(Values[Index]) < 1.0e7;
		}
		if (!bNumbers)
		{
			continue;
		}
		Views[Number - 1].bStored = true;
		Views[Number - 1].Location = FVector(Values[0], Values[1], Values[2]);
		Views[Number - 1].Rotation = FRotator(Values[3], Values[4], Values[5]);
		++Found;
	}
	if (Found > 0)
	{
		UE_LOG(LogLvShow, Display, TEXT("LvShow: %d stored view%s (number keys)"), Found, Found == 1 ? TEXT("") : TEXT("s"));
	}
}

void ULvShowSubsystem::SaveViews() const
{
	if (ViewsPath().IsEmpty())
	{
		return;    // a fixture preview: its views last as long as the window
	}
	FString Text;
	for (int32 Index = 0; Index < 9; ++Index)
	{
		if (Views[Index].bStored)
		{
			const FView& View = Views[Index];
			Text += FString::Printf(TEXT("%d %.2f %.2f %.2f %.3f %.3f %.3f\n"), Index + 1, View.Location.X, View.Location.Y, View.Location.Z,
				View.Rotation.Pitch, View.Rotation.Yaw, View.Rotation.Roll);
		}
	}
	if (!FFileHelper::SaveStringToFile(Text, *ViewsPath()))
	{
		UE_LOG(LogLvShow, Warning, TEXT("LvShow: note: the views could not be saved to %s"), *ViewsPath());
	}
}

FBox ULvShowSubsystem::StageBounds() const
{
	// What is on stage and to be seen: the fixtures that have a place and are not left out, and the set pieces added in Openviz.
	FBox Bounds(ForceInit);
	for (const TWeakObjectPtr<ALvFixtureActor>& Fixture : Fixtures)
	{
		if (Fixture.IsValid() && !Fixture->IsUnplaced() && !Fixture->IsHidden())
		{
			Bounds += Fixture->GetActorLocation();
		}
	}
	for (const TWeakObjectPtr<ALvSceneryActor>& Piece : AddedPieces)
	{
		if (Piece.IsValid())
		{
			Bounds += Piece->GetComponentsBoundingBox(true);
		}
	}
	if (!Bounds.IsValid)
	{
		for (const TWeakObjectPtr<ALvFixtureActor>& Fixture : Fixtures)
		{
			if (Fixture.IsValid())
			{
				Bounds += Fixture->GetActorLocation();
			}
		}
	}
	return Bounds;
}

void ULvShowSubsystem::PlanView(const FString& Which)
{
	APlayerController* Controller = GetWorld()->GetFirstPlayerController();
	APawn* Pawn = Controller ? Controller->GetPawn() : nullptr;
	if (!Pawn || !Controller->PlayerCameraManager)
	{
		return;
	}
	FBox Bounds = StageBounds();
	if (!Bounds.IsValid)
	{
		Bounds = FBox(FVector(-600.0, -600.0, 0.0), FVector(600.0, 600.0, 600.0));
	}
	Bounds += FVector(Bounds.GetCenter().X, Bounds.GetCenter().Y, 0.0);    // the floor is part of the picture
	const FVector Centre = Bounds.GetCenter();
	const FVector Size = Bounds.GetSize().ComponentMax(FVector(300.0, 300.0, 300.0));
	FVector2D ViewSize(16.0, 9.0);
	if (GEngine && GEngine->GameViewport)
	{
		GEngine->GameViewport->GetViewportSize(ViewSize);
	}
	const double Aspect = ViewSize.Y > 0.0 ? ViewSize.X / ViewSize.Y : 16.0 / 9.0;
	// From three hundred metres off through a lens a few degrees wide, nothing leans: what is seen
	// is to scale all over, as on a plot, and beams are drawn as from anywhere else. The audience
	// is on the +Y side, stage left on +X.
	PlanDistance = 30000.0;
	FVector From;
	FRotator Toward;
	double Wide;
	double High;
	FString Said;
	if (Which == TEXT("top"))
	{
		From = Centre + FVector(0.0, 0.0, PlanDistance);
		Toward = FRotator(-90.0, -90.0, 0.0);    // looking down, upstage at the top of the picture, stage left on the right
		Wide = Size.X;
		High = Size.Y;
		Said = TEXT("from above: upstage at the top, stage left on the right");
	}
	else if (Which == TEXT("side"))
	{
		From = Centre + FVector(PlanDistance, 0.0, 0.0);
		Toward = FRotator(0.0, 180.0, 0.0);      // from stage left, the house on the right
		Wide = Size.Y;
		High = Size.Z;
		Said = TEXT("from stage left: the house on the right");
	}
	else
	{
		From = Centre + FVector(0.0, PlanDistance, 0.0);
		Toward = FRotator(0.0, -90.0, 0.0);      // from the house
		Wide = Size.X;
		High = Size.Z;
		Said = TEXT("from the house");
	}
	const double Across = FMath::Max(Wide, High * Aspect) * 1.25;    // a margin all round
	PlanFov = FMath::Clamp(static_cast<float>(FMath::RadiansToDegrees(2.0 * FMath::Atan(Across * 0.5 / PlanDistance))), 0.3f, 60.0f);
	bGliding = false;
	bPlan = true;
	Pawn->SetActorLocation(From);
	Controller->SetControlRotation(Toward);
	Controller->PlayerCameraManager->SetFOV(PlanFov);
	UE_LOG(LogLvShow, Display, TEXT("LvShow: flat view %s"), *Said);
	GEngine->AddOnScreenDebugMessage(0x4C77, 3.0f, FColor(170, 170, 170), FString::Printf(TEXT("Flat view %s. Wheel zooms, W A S D slide; 0 or a number key goes back."), *Said));
}

void ULvShowSubsystem::LeavePlanView()
{
	if (!bPlan)
	{
		return;
	}
	bPlan = false;
	if (APlayerController* Controller = GetWorld()->GetFirstPlayerController())
	{
		if (Controller->PlayerCameraManager)
		{
			Controller->PlayerCameraManager->UnlockFOV();
		}
	}
}

void ULvShowSubsystem::FollowCommands()
{
	if (CommandsFile.IsEmpty())
	{
		return;
	}
	const FDateTime Stamp = IFileManager::Get().GetTimeStamp(*CommandsFile);
	if (Stamp == CommandsStamp)
	{
		return;
	}
	CommandsStamp = Stamp;
	FString Text;
	TSharedPtr<FJsonObject> Said;
	if (!FFileHelper::LoadFileToString(Text, *CommandsFile) || !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Said) || !Said.IsValid())
	{
		return;
	}
	double Serial = -1.0;
	FString Command;
	FString Value;
	Said->TryGetNumberField(TEXT("serial"), Serial);
	Said->TryGetStringField(TEXT("command"), Command);
	Said->TryGetStringField(TEXT("value"), Value);
	if (static_cast<int64>(Serial) == CommandSerial)
	{
		return;    // done already
	}
	CommandSerial = static_cast<int64>(Serial);
	if (Command == TEXT("view"))
	{
		if (Value == TEXT("top") || Value == TEXT("front") || Value == TEXT("side"))
		{
			PlanView(Value);
		}
		else if (Value.Len() == 1 && FChar::IsDigit(Value[0]))
		{
			RecallView(Value[0] - TEXT('0'));
		}
	}
	else if (Command == TEXT("store") && Value.Len() == 1 && Value[0] >= TEXT('1') && Value[0] <= TEXT('9'))
	{
		StoreView(Value[0] - TEXT('0'));
	}
	else if (Command == TEXT("record"))
	{
		if (Value == TEXT("start"))
		{
			StartVideo();
		}
		else if (Video.IsValid() && Video->IsRecording() && RenderStage == 0)
		{
			UE_LOG(LogLvShow, Display, TEXT("LvShow: video: stopping"));
			Video->RequestStop();
		}
	}
	else if (Command == TEXT("meter"))
	{
		SetMeter(Value);
	}
	else if (Command == TEXT("panorama"))
	{
		StartPanorama(Value.IsEmpty() ? 8192 : FCString::Atoi(*Value));
	}
	else if (Command == TEXT("picture"))
	{
		FString Wide;
		FString High;
		if (Value.Split(TEXT("x"), &Wide, &High))
		{
			TakePicture(FMath::Clamp(FCString::Atoi(*Wide), 320, 16384), FMath::Clamp(FCString::Atoi(*High), 180, 16384));
		}
		else
		{
			TakePicture();
		}
	}
	else
	{
		UE_LOG(LogLvShow, Warning, TEXT("LvShow: note: Openviz asked for '%s', which this visualizer does not know"), *Command);
	}
}

void ULvShowSubsystem::GlideTo(const FVector& Location, const FRotator& Rotation)
{
	APlayerController* Controller = GetWorld()->GetFirstPlayerController();
	APawn* Pawn = Controller ? Controller->GetPawn() : nullptr;
	if (!Pawn)
	{
		return;
	}
	if (bPlan)
	{
		// Out of a flat view there is nothing to glide through: the lens is put back and the view is where it was asked to be.
		LeavePlanView();
		Pawn->SetActorLocation(Location);
		Controller->SetControlRotation(Rotation);
		return;
	}
	bGliding = true;
	GlideStarted = Elapsed;
	GlideFrom = Pawn->GetActorLocation();
	GlideEnd = Location;
	GlideFromTurn = Controller->GetControlRotation().Quaternion();
	GlideEndTurn = Rotation.Quaternion();
}

void ULvShowSubsystem::StoreView(int32 Index)
{
	APlayerController* Controller = GetWorld()->GetFirstPlayerController();
	APawn* Pawn = Controller ? Controller->GetPawn() : nullptr;
	if (!Pawn || Index < 1 || Index > 9)
	{
		return;
	}
	if (bPlan)
	{
		UE_LOG(LogLvShow, Display, TEXT("LvShow: note: a flat view is not stored: T, F and L bring it back"));
		GEngine->AddOnScreenDebugMessage(0x4C77, 2.5f, FColor(170, 170, 170), TEXT("A flat view is not stored: T, F and L bring it back"));
		return;
	}
	Views[Index - 1].bStored = true;
	Views[Index - 1].Location = Pawn->GetActorLocation();
	Views[Index - 1].Rotation = Controller->GetControlRotation();
	SaveViews();
	UE_LOG(LogLvShow, Display, TEXT("LvShow: view %d stored"), Index);
	GEngine->AddOnScreenDebugMessage(0x4C77, 2.0f, FColor(170, 170, 170), FString::Printf(TEXT("View %d stored"), Index));
}

void ULvShowSubsystem::RecallView(int32 Index)
{
	if (Index == 0)
	{
		if (StartView.bStored)
		{
			UE_LOG(LogLvShow, Display, TEXT("LvShow: back to the starting view"));
			GlideTo(StartView.Location, StartView.Rotation);
		}
		return;
	}
	if (Index < 1 || Index > 9)
	{
		return;
	}
	if (Views[Index - 1].bStored)
	{
		UE_LOG(LogLvShow, Display, TEXT("LvShow: view %d"), Index);
		GlideTo(Views[Index - 1].Location, Views[Index - 1].Rotation);
	}
	else
	{
		UE_LOG(LogLvShow, Display, TEXT("LvShow: note: no view %d yet: hold the key, or press it with Shift, to store this view there"), Index);
		if (!IsFilming())
		{
			GEngine->AddOnScreenDebugMessage(0x4C77, 2.5f, FColor(170, 170, 170),
				FString::Printf(TEXT("No view stored under %d yet: hold %d, or press Shift+%d, to store this one"), Index, Index, Index));
		}
	}
}

void ULvShowSubsystem::PressKey(const FKey& Key, bool bDown)
{
	// Through Slate, the way the window's own key events go: to the focused widget, the game
	// viewport, and on into the player's input. Only the system's own layer is left out.
	if (!FSlateApplication::IsInitialized())
	{
		return;
	}
	const uint32 Character = Key == EKeys::P ? 'p' : Key == EKeys::Zero ? '0' : Key == EKeys::One ? '1' : Key == EKeys::Two ? '2' : 0;
	const FKeyEvent Event(Key, FModifierKeysState(), 0, false, Character, 0);
	if (bDown)
	{
		FSlateApplication::Get().ProcessKeyDownEvent(Event);
	}
	else
	{
		FSlateApplication::Get().ProcessKeyUpEvent(Event);
	}
}

void ULvShowSubsystem::StepViewTest()
{
	APlayerController* Controller = GetWorld()->GetFirstPlayerController();
	APawn* Pawn = Controller ? Controller->GetPawn() : nullptr;
	if (!Pawn)
	{
		return;
	}
	auto Check = [this](bool bGood, const TCHAR* What)
	{
		(bGood ? ViewTestPassed : ViewTestFailed) += 1;
		UE_LOG(LogLvShow, Display, TEXT("LvShow: view self-test: %s  %s"), bGood ? TEXT("PASS") : TEXT("FAIL"), What);
	};
	const FString ViewsFile = ViewsPath();
	switch (ViewTestStage)
	{
	case 0:
		// Shift and 1 store the view where it is now.
		FSlateApplication::Get().SetAllUserFocusToGameViewport();
		ViewTestFrom = Pawn->GetActorLocation();
		ViewTestFromTurn = Controller->GetControlRotation();
		IFileManager::Get().Delete(*ViewsFile, false, true, true);
		Views[0].bStored = false;
		Views[1].bStored = false;
		PressKey(EKeys::LeftShift, true);
		PressKey(EKeys::One, true);
		ViewTestAt = Elapsed + 0.2;
		break;
	case 1:
		Check(Views[0].bStored && Views[0].Location.Equals(ViewTestFrom, 0.01), TEXT("Shift and 1 store the view"));
		Check(IFileManager::Get().FileExists(*ViewsFile), TEXT("the stored view is written beside the show"));
		PressKey(EKeys::One, false);
		PressKey(EKeys::LeftShift, false);
		// Elsewhere, then a tap on 1 glides back.
		Pawn->SetActorLocation(ViewTestFrom + FVector(600.0, -400.0, 150.0));
		Controller->SetControlRotation(ViewTestFromTurn + FRotator(-20.0, 35.0, 0.0));
		ViewTestAt = Elapsed + 0.2;
		break;
	case 2:
		PressKey(EKeys::One, true);
		ViewTestAt = Elapsed + 0.12;
		break;
	case 3:
		PressKey(EKeys::One, false);
		ViewTestAt = Elapsed + 0.9;
		break;
	case 4:
		Check(Pawn->GetActorLocation().Equals(ViewTestFrom, 1.0), TEXT("a tap on 1 glides back to the stored view"));
		Check(Controller->GetControlRotation().Equals(ViewTestFromTurn, 0.5), TEXT("and turns the view as it was"));
		// Holding 2 stores, without any other key.
		Pawn->SetActorLocation(ViewTestFrom + FVector(-300.0, 0.0, 80.0));
		PressKey(EKeys::Two, true);
		ViewTestAt = Elapsed + 1.1;
		break;
	case 5:
		Check(Views[1].bStored && Views[1].Location.Equals(ViewTestFrom + FVector(-300.0, 0.0, 80.0), 0.01), TEXT("holding 2 stores the view there"));
		PressKey(EKeys::Two, false);
		ViewTestAt = Elapsed + 0.2;
		break;
	case 6:
		Check(Pawn->GetActorLocation().Equals(ViewTestFrom + FVector(-300.0, 0.0, 80.0), 0.01), TEXT("and letting go of it after that does not move the view"));
		// A tap on 0 goes back to where the view began.
		PressKey(EKeys::Zero, true);
		ViewTestAt = Elapsed + 0.12;
		break;
	case 7:
		PressKey(EKeys::Zero, false);
		ViewTestAt = Elapsed + 0.9;
		break;
	case 8:
		Check(Pawn->GetActorLocation().Equals(StartView.Location, 1.0), TEXT("a tap on 0 goes back to the starting view"));
		// A view is loaded again from the file: as the next run of the show will have it.
		Views[0].bStored = false;
		LoadViews();
		Check(Views[0].bStored && Views[0].Location.Equals(ViewTestFrom, 0.01) && Views[1].bStored, TEXT("the views file holds both views for next time"));
		// P takes a picture.
		PressKey(EKeys::P, true);
		ViewTestAt = Elapsed + 0.12;
		break;
	case 9:
		PressKey(EKeys::P, false);
		ViewTestAt = Elapsed + 2.5;
		break;
	case 10:
	{
		TArray<FString> Pictures;
		const FString Folder = PicturesFolder.IsEmpty() ? FPaths::ConvertRelativePathToFull(FPaths::ScreenShotDir()) : PicturesFolder;
		IFileManager::Get().FindFiles(Pictures, *(Folder / TEXT("*.png")), true, false);
		Check(Pictures.Num() > 0, *FString::Printf(TEXT("P saves a picture into %s"), *Folder));
		UE_LOG(LogLvShow, Display, TEXT("LvShow: view self-test: %d passed, %d failed"), ViewTestPassed, ViewTestFailed);
		ViewTestStage = -1;
		FPlatformMisc::RequestExit(false);
		return;
	}
	default:
		break;
	}
	++ViewTestStage;
}

void ULvShowSubsystem::TakePicture(int32 Width, int32 Height)
{
	FString Folder = PicturesFolder;
	if (Folder.IsEmpty())
	{
		Folder = FPaths::ConvertRelativePathToFull(FPaths::ScreenShotDir());
	}
	IFileManager::Get().MakeDirectory(*Folder, true);
	const FString Name = FString::Printf(TEXT("%s %s.png"), ShowPath.IsEmpty() ? TEXT("Preview") : *FPaths::GetBaseFilename(ShowPath),
		*FDateTime::Now().ToString(TEXT("%Y-%m-%d %H%M%S")));
	UGameViewportClient* Viewport = GetWorld()->GetGameViewport();
	if (Width > 0 && Height > 0 && Viewport && Viewport->Viewport)
	{
		// Larger than the window: the engine draws the view once at that size and writes it out.
		RequestPicture(Folder / Name, Width, Height);
		UE_LOG(LogLvShow, Display, TEXT("LvShow: picture saved as %s (%d by %d)"), *Name, Width, Height);
		GEngine->AddOnScreenDebugMessage(0x4C78, 2.0f, FColor(170, 170, 170), FString::Printf(TEXT("Picture saved as %s (%d by %d)"), *Name, Width, Height));
		return;
	}
	FScreenshotRequest::RequestScreenshot(Folder / Name, false, false);
	UE_LOG(LogLvShow, Display, TEXT("LvShow: picture saved as %s"), *Name);
	GEngine->AddOnScreenDebugMessage(0x4C78, 2.0f, FColor(170, 170, 170), FString::Printf(TEXT("Picture saved as %s"), *Name));
}

void ULvShowSubsystem::RequestPicture(const FString& Path, int32 Width, int32 Height)
{
	UGameViewportClient* Viewport = GetWorld()->GetGameViewport();
	if (!Viewport || !Viewport->Viewport)
	{
		return;
	}
	FHighResScreenshotConfig& Config = GetHighResScreenshotConfig();
	Config.SetResolution(static_cast<uint32>(Width), static_cast<uint32>(Height), 1.0f);
	Config.SetFilename(Path);
	Config.bDateTimeBasedNaming = false;
	Config.bMaskEnabled = false;
	Viewport->Viewport->TakeHighResScreenShot();
}

bool ULvShowSubsystem::PlaceCamera(UWorld& World)
{
	APlayerController* Controller = World.GetFirstPlayerController();
	APawn* Pawn = Controller ? Controller->GetPawn() : nullptr;
	if (!Pawn)
	{
		return false;    // the player arrives a little after the world begins play
	}

	const FBox Bounds = StageBounds();    // not the fixtures without a place: they lie in a heap at the origin
	const FVector Centre = Bounds.IsValid ? Bounds.GetCenter() : FVector(0.0, 0.0, 300.0);
	const double Span = Bounds.IsValid ? FMath::Max(Bounds.GetSize().X, Bounds.GetSize().Y) : 600.0;
	// From the house. MVR's Y axis runs from the audience toward the back of the stage, and Y is
	// mirrored on the way into Unreal, so the audience is on the +Y side here. The view is 90
	// degrees wide, so this far back the rig fills a little over half of it.
	FVector Location(Centre.X, Centre.Y + Span * 0.7 + 350.0, 200.0);
	FVector Target(Centre.X, Centre.Y, Centre.Z * 0.5);
	// -LvCameraAt=x,y,z and -LvCameraLook=x,y,z, in centimetres, start the view somewhere else.
	auto Place = [](const TCHAR* Key, FVector& Out)
	{
		FString Text;
		TArray<FString> Parts;
		if (FParse::Value(FCommandLine::Get(), Key, Text, false) && Text.ParseIntoArray(Parts, TEXT(",")) == 3)
		{
			Out = FVector(FCString::Atod(*Parts[0]), FCString::Atod(*Parts[1]), FCString::Atod(*Parts[2]));
		}
	};
	Place(TEXT("LvCameraAt="), Location);
	Place(TEXT("LvCameraLook="), Target);

	// The default pawn flies with W A S D and the mouse; its sphere would only be in the way.
	Pawn->SetActorHiddenInGame(true);
	Pawn->SetActorLocation(Location);
	Controller->SetControlRotation((Target - Location).Rotation());
	StartView.bStored = true;
	StartView.Location = Location;
	StartView.Rotation = (Target - Location).Rotation();
	LoadViews();
	// -LvStoreView=<1..9> stores where the view begins under that number, as Shift and the number
	// key would; -LvView=<1..9> begins at a stored view instead.
	const int32 StoreAs = IntOption(TEXT("LvStoreView="), 0);
	if (StoreAs >= 1 && StoreAs <= 9)
	{
		Views[StoreAs - 1] = StartView;
		SaveViews();
		UE_LOG(LogLvShow, Display, TEXT("LvShow: view %d stored"), StoreAs);
	}
	const int32 BeginAt = IntOption(TEXT("LvView="), 0);
	if (BeginAt >= 1 && BeginAt <= 9 && Views[BeginAt - 1].bStored)
	{
		Pawn->SetActorLocation(Views[BeginAt - 1].Location);
		Controller->SetControlRotation(Views[BeginAt - 1].Rotation);
	}
	// The view is turned by UpdateCamera, by the rules chosen in Openviz, not by the pawn's own
	// always-on mouse look.
	Controller->SetIgnoreLookInput(true);
	if (!ShotPath.IsEmpty())
	{
		// A picture was asked for: hold the view still, whatever the mouse and keyboard are doing.
		Controller->SetIgnoreMoveInput(true);
	}
	else
	{
		const TCHAR* CommandLine = FCommandLine::Get();
		FString MouseLook;
		FParse::Value(CommandLine, TEXT("LvMouseLook="), MouseLook);
		bAlwaysLook = MouseLook.Equals(TEXT("always"), ESearchCase::IgnoreCase);
		FParse::Value(CommandLine, TEXT("LvLookSpeed="), LookSpeed);
		FParse::Value(CommandLine, TEXT("LvMoveSpeed="), MoveSpeed);
		LookSpeed = FMath::Clamp(LookSpeed, 0.2f, 3.0f);
		MoveSpeed = FMath::Clamp(MoveSpeed, 50.0f, 5000.0f);
		bInvertY = IntOption(TEXT("LvInvertY="), 0) != 0;
		ApplyMouseMode(*Controller);
		bCameraControl = true;
		GEngine->AddOnScreenDebugMessage(0x4C79, 12.0f, FColor(170, 170, 170),
			TEXT("Right button: look.  Wheel: in and out.  W A S D, Q E: fly.  1 to 9: views (hold a key to store the view there).  0: start.  P: picture.  M: light levels.  Tab: game mouse."));
	}
	return true;
}

void ULvShowSubsystem::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);
	UWorld* World = GetWorld();
	if (!bActive && World && World->WorldType == EWorldType::Editor)
	{
		Start(*World);    // the editor's level never begins play; its first tick is the cue
	}
	if (!bActive)
	{
		return;
	}
	Elapsed += DeltaTime;
	if (bRigHaze && Hazers > 0)
	{
		FollowHazers(DeltaTime);
	}
	if (!ScreensFile.IsEmpty() && Elapsed >= NextScreensLook)
	{
		NextScreensLook = Elapsed + 0.5;
		FollowScreensFile(*World);
	}
	if (Elapsed >= NextFilesLook)
	{
		NextFilesLook = Elapsed + 0.5;
		FollowShowFiles(*World);
		FollowPictureFile(*World);
		FollowPiecesFile(*World);
	}
	if (PsnSocket)
	{
		FollowTrackers();
	}
	if (bMeterOnStart && bCameraPlaced && Elapsed >= 2.0)
	{
		bMeterOnStart = false;
		SetMeter(TEXT("on"));
	}
	if (Meter.IsValid() && Meter->IsOn())
	{
		FollowMeter();
	}
	WatchFrameRate(DeltaTime);
	if (!bScreensSettled)
	{
		ReportScreens();
	}
	if (bEditing)
	{
		if (Elapsed >= NextSave)
		{
			NextSave = Elapsed + 0.5;
			SaveEdits();
			SaveScreenMoves();
		}
		if (SelfTestStage >= 0 && SelfTestStage < 2 && Elapsed >= SelfTestAt)
		{
			StepSelfTest();
		}
	}
	else if (!bCameraPlaced)
	{
		bCameraPlaced = PlaceCamera(*GetWorld());
	}
	else if (bCameraControl && RenderStage == 0 && PanoramaStep >= 0)
	{
		StepPanorama();    // the camera is the panorama's while it is taken
	}
	else if (bCameraControl && RenderStage == 0)
	{
		if (Elapsed >= NextCommandsLook)
		{
			NextCommandsLook = Elapsed + 0.15;
			FollowCommands();
		}
		if (PanoramaOnStart > 0 && Elapsed >= PanoramaOnStartAt)
		{
			StartPanorama(PanoramaOnStart);
			PanoramaOnStart = 0;
		}
		UpdateCamera();
		if (ViewTestStage >= 0 && Elapsed >= ViewTestAt)
		{
			StepViewTest();
		}
	}
	if (ExecAt >= 0.0 && Elapsed >= ExecAt)
	{
		ExecAt = -1.0;
		UE_LOG(LogLvShow, Display, TEXT("LvShow: running the console command '%s'"), *ExecCommand);
		if (GEngine)
		{
			GEngine->Exec(World, *ExecCommand);
		}
	}
	if (QuitAfter > 0.0 && Elapsed >= QuitAfter)
	{
		QuitAfter = -1.0;
		FPlatformMisc::RequestExit(false);
	}

	// A fixture shows its idle look until its universe has data, and follows the console from then on.
	if (ULvDmxSubsystem* Dmx = GetWorld()->GetSubsystem<ULvDmxSubsystem>())
	{
		for (const TWeakObjectPtr<ALvFixtureActor>& Fixture : Fixtures)
		{
			if (!Fixture.IsValid())
			{
				continue;
			}
			const int32 Universe = Fixture->GetUniverse();
			if (LiveUniverses.Contains(Universe) || !Dmx->GetUniverse(Universe))
			{
				continue;
			}
			LiveUniverses.Add(Universe);
			UE_LOG(LogLvShow, Display, TEXT("LvShow: DMX is arriving on universe %d"), Universe);
			for (const TWeakObjectPtr<ALvFixtureActor>& Other : Fixtures)
			{
				if (Other.IsValid() && Other->GetUniverse() == Universe)
				{
					Other->SetDmxOverride(TConstArrayView<uint8>());
				}
			}
		}
	}

	if (Video.IsValid())
	{
		Video->Tick();
		FollowVideo();
	}
	if (RenderStage > 0)
	{
		StepRender();
	}
	if (GEngine && Elapsed >= NextStatus && !IsFilming())
	{
		NextStatus = Elapsed + 1.0;
		const FString Dmx = LiveUniverses.IsEmpty()
			? FString::Printf(TEXT("no DMX yet%s"), bIdleLook ? TEXT(", fixtures at their defaults") : TEXT(""))
			: FString::Printf(TEXT("DMX on %d universe%s"), LiveUniverses.Num(), LiveUniverses.Num() == 1 ? TEXT("") : TEXT("s"));
		GEngine->AddOnScreenDebugMessage(0x4C76, 1.5f, FColor(170, 170, 170),
			FString::Printf(TEXT("%d fixtures   |   %s   |   listening for %s"), Fixtures.Num(), *Dmx, *Listening));
	}

	if (!ShotPath.IsEmpty() && !bShotTaken && Elapsed >= ShotAfter)
	{
		bShotTaken = true;
		if (const APlayerController* Controller = GetWorld()->GetFirstPlayerController())
		{
			FVector From;
			FRotator Toward;
			Controller->GetPlayerViewPoint(From, Toward);
			UE_LOG(LogLvShow, Log, TEXT("picture taken from %s looking %s"), *From.ToCompactString(), *Toward.ToCompactString());
		}
		FScreenshotRequest::RequestScreenshot(ShotPath, false, false);
		UE_LOG(LogLvShow, Display, TEXT("LvShow: screenshot requested: %s"), *ShotPath);
		if (FParse::Param(FCommandLine::Get(), TEXT("LvShotQuit")))
		{
			QuitAt = Elapsed + 2.0;
		}
	}
	if (QuitAt > 0.0 && Elapsed >= QuitAt)
	{
		QuitAt = -1.0;
		FPlatformMisc::RequestExit(false);
	}
}

bool ULvShowSubsystem::IsFilming() const
{
	return (Video.IsValid() && Video->IsActive()) || RenderStage > 0 || PanoramaStep >= 0;
}

void ULvShowSubsystem::StartPanorama(int32 Width)
{
	APlayerController* Controller = GetWorld()->GetFirstPlayerController();
	if (PanoramaStep >= 0 || IsFilming() || !Controller || !Controller->PlayerCameraManager)
	{
		return;
	}
	if (bPlan)
	{
		UE_LOG(LogLvShow, Warning, TEXT("LvShow: panorama: not taken from a flat view: go to a view of the stage first"));
		return;
	}
	PanoramaWidth = FMath::Clamp(Width, 1024, 16384);
	// A view 90 degrees wide has as many pixels across as a quarter of the picture at its middle,
	// and more toward its edges: a little more than a quarter keeps the picture sharp everywhere.
	PanoramaSide = FMath::Clamp(FMath::RoundToInt(PanoramaWidth / 3.5), 256, 4096);
	FString Folder = PicturesFolder.IsEmpty() ? FPaths::ConvertRelativePathToFull(FPaths::ScreenShotDir()) : PicturesFolder;
	IFileManager::Get().MakeDirectory(*Folder, true);
	PanoramaPrefix = Folder / FString::Printf(TEXT("%s %s 360 view "), ShowPath.IsEmpty() ? TEXT("Preview") : *FPaths::GetBaseFilename(ShowPath),
		*FDateTime::Now().ToString(TEXT("%Y-%m-%d %H%M%S")));
	PanoramaFrom = Controller->GetControlRotation();
	bGliding = false;
	// Motion blur would smear each view's first frames after the camera turns, and the lens's
	// darkening toward the corners and its flares would show where the views are joined.
	LvStage::SetLook(*GetWorld(), BloomNow, false, false);
	Controller->PlayerCameraManager->SetFOV(90.0f);
	PanoramaStep = 0;
	AimPanorama();
	UE_LOG(LogLvShow, Display, TEXT("LvShow: panorama: taking six views of %d pixels for a picture %d wide"), PanoramaSide, PanoramaWidth);
	GEngine->AddOnScreenDebugMessage(0x4C78, 3.0f, FColor(170, 170, 170), TEXT("Taking a 360-degree picture: six views, a few seconds"));
}

void ULvShowSubsystem::AimPanorama()
{
	static const double Pitch[6] = {0.0, 0.0, 0.0, 0.0, 90.0, -90.0};
	static const double Yaw[6] = {0.0, 90.0, 180.0, 270.0, 0.0, 0.0};
	APlayerController* Controller = GetWorld()->GetFirstPlayerController();
	if (!Controller)
	{
		return;
	}
	Controller->SetControlRotation(FRotator(Pitch[PanoramaStep], PanoramaFrom.Yaw + Yaw[PanoramaStep], 0.0));
	if (Controller->PlayerCameraManager)
	{
		Controller->PlayerCameraManager->SetGameCameraCutThisFrame();    // nothing carried over from where it looked before
	}
	bPanoramaAsked = false;
	PanoramaSeenSize = -1;
	PanoramaAt = Elapsed + 0.8;    // long enough for what builds up over frames to settle
}

void ULvShowSubsystem::StepPanorama()
{
	const FString Path = PanoramaPrefix + FString::FromInt(PanoramaStep) + TEXT(".png");
	if (!bPanoramaAsked)
	{
		if (Elapsed >= PanoramaAt)
		{
			RequestPicture(Path, PanoramaSide, PanoramaSide);
			bPanoramaAsked = true;
			PanoramaAt = Elapsed + 30.0;
		}
		return;
	}
	// Written by a worker of the engine's: taken as whole once its size has stayed put a moment.
	const int64 Size = IFileManager::Get().FileSize(*Path);
	if (Size > 0 && Size == PanoramaSeenSize && Elapsed - PanoramaSeenAt >= 0.25)
	{
		if (++PanoramaStep >= 6)
		{
			FinishPanorama(true);
		}
		else
		{
			AimPanorama();
		}
		return;
	}
	if (Size != PanoramaSeenSize)
	{
		PanoramaSeenSize = Size;
		PanoramaSeenAt = Elapsed;
	}
	if (Elapsed >= PanoramaAt)
	{
		UE_LOG(LogLvShow, Warning, TEXT("LvShow: panorama: view %d was not saved"), PanoramaStep);
		FinishPanorama(false);
	}
}

void ULvShowSubsystem::FinishPanorama(bool bSaved)
{
	PanoramaStep = -1;
	if (APlayerController* Controller = GetWorld()->GetFirstPlayerController())
	{
		Controller->SetControlRotation(PanoramaFrom);
		if (Controller->PlayerCameraManager)
		{
			Controller->PlayerCameraManager->UnlockFOV();
			Controller->PlayerCameraManager->SetGameCameraCutThisFrame();
		}
	}
	LvStage::SetLook(*GetWorld(), BloomNow, bMotionBlur);
	if (bSaved)
	{
		UE_LOG(LogLvShow, Display, TEXT("LvShow: panorama: views saved: %s (%d wide)"), *PanoramaPrefix, PanoramaWidth);
		GEngine->AddOnScreenDebugMessage(0x4C78, 3.0f, FColor(170, 170, 170), TEXT("360-degree views taken: Openviz is making the picture"));
	}
	if (FParse::Param(FCommandLine::Get(), TEXT("LvShotQuit")) && ShotPath.IsEmpty())
	{
		QuitAt = Elapsed + 1.0;
	}
}

void ULvShowSubsystem::StartVideo()
{
	if (IsFilming())
	{
		return;
	}
	if (VideosFolder.IsEmpty())
	{
		UE_LOG(LogLvShow, Warning, TEXT("LvShow: video: not recorded: Openviz did not say where videos go"));
		return;
	}
	IFileManager::Get().MakeDirectory(*VideosFolder, true);
	const FString Name = ShowPath.IsEmpty() ? FString(TEXT("Openviz")) : FPaths::GetBaseFilename(ShowPath);
	const FString Path = FPaths::Combine(VideosFolder, FString::Printf(TEXT("%s %s.mov"), *Name, *FDateTime::Now().ToString(TEXT("%Y-%m-%d %H%M%S"))));
	if (!Video.IsValid())
	{
		Video = MakeShared<FLvVideo>();
	}
	FString Error;
	if (!Video->Start(Path, 30, false, Error))
	{
		UE_LOG(LogLvShow, Warning, TEXT("LvShow: video: not recorded: %s"), *Error);
		return;
	}
	if (GEngine)
	{
		GEngine->ClearOnScreenDebugMessages();    // the status line is not part of the show
	}
	UE_LOG(LogLvShow, Display, TEXT("LvShow: video: recording to %s, %d by %d, 30 frames a second"), *Path, Video->Size().X, Video->Size().Y);
}

void ULvShowSubsystem::FollowVideo()
{
	if (!Video.IsValid() || !Video->IsDone())
	{
		return;
	}
	if (Video->Result().IsEmpty())
	{
		UE_LOG(LogLvShow, Display, TEXT("LvShow: video: saved %s: %d frames, %.1f seconds"), *Video->GetPath(), Video->FramesWritten(), Video->Seconds());
	}
	else
	{
		UE_LOG(LogLvShow, Warning, TEXT("LvShow: video: not saved: %s"), *Video->Result());
	}
	Video.Reset();
}

void ULvShowSubsystem::StepRender()
{
	auto Fail = [this](const FString& Why)
	{
		UE_LOG(LogLvShow, Error, TEXT("LvShow: render: failed: %s"), *Why);
		if (SizedMovie.IsValid())
		{
			SizedMovie.Reset();    // closes its file, which is left as its ".part": no half movie is kept
			IFileManager::Get().Delete(*(RenderOut + TEXT(".part")), false, true, true);
		}
		RenderStage = 0;
		ULvDmxSubsystem::ClearProvided();
		FApp::SetUseFixedTimeStep(false);
		FPlatformMisc::RequestExit(false);
	};
	auto Give = [](int Universe, const uint8_t* Levels, size_t Count)
	{
		ULvDmxSubsystem::Provide(Universe, Levels, static_cast<int32>(Count));
	};
	switch (RenderStage)
	{
	case 1:
		// The stage built and the camera placed, and a moment for the window to settle.
		if (!bCameraPlaced || Elapsed < 2.0)
		{
			return;
		}
		{
			Reader = MakeShared<viz::DmxRecordingReader>();
			std::string Error;
			if (!Reader->open(LvToStd(RenderRecording), Error))
			{
				Fail(LvToFString(Error));
				return;
			}
			RenderFrames = static_cast<int64>(FMath::CeilToDouble(Reader->length() * RenderRate)) + RenderRate / 2 + 1;
			UE_LOG(LogLvShow, Display, TEXT("LvShow: render: %.1f seconds of DMX on %d universes, %lld frames"), Reader->length(),
				static_cast<int32>(Reader->universes().size()), RenderFrames);
		}
		// From here on the clock moves a frame at a time, however long a frame takes to draw, and only the recording is heard.
		FApp::SetFixedDeltaTime(1.0 / RenderRate);
		FApp::SetUseFixedTimeStep(true);
		bAdaptBeams = false;
		ULvDmxSubsystem::ProvideOnly(true);
		Reader->play_until(0.0, Give);
		if (GEngine)
		{
			GEngine->ClearOnScreenDebugMessages();
		}
		// The camera goes where the path begins and is moved by it alone from now on.
		{
			int32 Missing = 0;
			for (const TPair<int32, double>& Key : RenderPath)
			{
				FVector Location;
				FRotator Rotation;
				Missing += ViewOf(Key.Key, Location, Rotation) ? 0 : 1;
			}
			if (Missing > 0)
			{
				UE_LOG(LogLvShow, Warning, TEXT("LvShow: note: render: %d of the views the camera is to go to are not stored; it passes them by"), Missing);
			}
		}
		PlaceRenderCamera(0.0);
		RenderStage = 2;
		return;
	case 2:
		// The recording's first levels held while fixtures travel to them and the picture's history fills.
		if (--RenderWarmup > 0)
		{
			return;
		}
		if (RenderSize.X > 0)
		{
			SizedMovie = MakeShared<viz::MovieWriter>();
			std::string Error;
			if (!SizedMovie->open(LvToStd(RenderOut), RenderSize.X, RenderSize.Y, RenderRate, Error))
			{
				Fail(LvToFString(Error));
				return;
			}
			UE_LOG(LogLvShow, Display, TEXT("LvShow: video: recording to %s, %d by %d, %d frames a second"), *RenderOut, RenderSize.X, RenderSize.Y, RenderRate);
			RenderFrame = 0;
			SizedStep = 0;
			RenderSaidAt = FPlatformTime::Seconds();
			RenderStage = 3;
			return;
		}
		{
			Video = MakeShared<FLvVideo>();
			FString Error;
			if (!Video->Start(RenderOut, RenderRate, true, Error))
			{
				Fail(Error);
				return;
			}
		}
		RenderFrame = 0;
		RenderSaidAt = FPlatformTime::Seconds();
		RenderStage = 3;
		return;
	case 3:
		if (SizedMovie.IsValid())
		{
			if (!StepSizedRender())
			{
				Fail(TEXT("a frame's picture could not be made into the movie"));
			}
			return;
		}
		// This frame is drawn with what was given last; the next is given now, the camera too.
		++RenderFrame;
		Reader->play_until(static_cast<double>(RenderFrame) / RenderRate, Give);
		PlaceRenderCamera(static_cast<double>(RenderFrame) / RenderRate);
		if (FPlatformTime::Seconds() - RenderSaidAt >= 2.0)
		{
			RenderSaidAt = FPlatformTime::Seconds();
			UE_LOG(LogLvShow, Display, TEXT("LvShow: render: frame %lld of %lld"), RenderFrame, RenderFrames);
		}
		if (RenderFrame >= RenderFrames)
		{
			UE_LOG(LogLvShow, Display, TEXT("LvShow: render: frame %lld of %lld"), RenderFrame, RenderFrames);
			Video->RequestStop();
			RenderStage = 4;
		}
		return;
	case 4:
		if (Video.IsValid() && Video->IsActive())
		{
			return;
		}
		RenderStage = 0;
		ULvDmxSubsystem::ClearProvided();
		FApp::SetUseFixedTimeStep(false);
		UE_LOG(LogLvShow, Display, TEXT("LvShow: render: done"));
		FPlatformMisc::RequestExit(false);
		return;
	default:
		return;
	}
}

bool ULvShowSubsystem::StepSizedRender()
{
	auto Give = [](int Universe, const uint8_t* Levels, size_t Count)
	{
		ULvDmxSubsystem::Provide(Universe, Levels, static_cast<int32>(Count));
	};
	const double Seconds = static_cast<double>(RenderFrame) / RenderRate;
	switch (SizedStep)
	{
	case 0:
		// The frame's DMX and camera; the next tick moves the world on by one frame and draws it.
		Reader->play_until(Seconds, Give);
		PlaceRenderCamera(Seconds);
		FApp::SetFixedDeltaTime(1.0 / RenderRate);
		SizedStep = 1;
		return true;
	case 1:
		// Drawn now: its shot is asked for, and until the shot is written the clock all but stands
		// still, so that nothing moves on between the movie's frames.
		SizedShot = FPaths::Combine(FPaths::GetPath(RenderOut), FPaths::GetBaseFilename(RenderOut) + TEXT(".frame.png"));
		IFileManager::Get().Delete(*SizedShot, false, true, true);
		RequestPicture(SizedShot, RenderSize.X, RenderSize.Y);
		FApp::SetFixedDeltaTime(1e-5);
		SizedSeenSize = -1;
		SizedAskedAt = FPlatformTime::Seconds();
		SizedStep = 2;
		return true;
	default:
		break;
	}
	// The shot is whole once its size has stayed put for a moment.
	const int64 Size = IFileManager::Get().FileSize(*SizedShot);
	if (Size <= 0)
	{
		if (FPlatformTime::Seconds() - SizedAskedAt > 60.0)
		{
			UE_LOG(LogLvShow, Warning, TEXT("LvShow: render: the shot of frame %lld never came; asking again"), RenderFrame);
			SizedStep = 1;
		}
		return true;
	}
	if (Size != SizedSeenSize)
	{
		SizedSeenSize = Size;
		SizedSeenAt = FPlatformTime::Seconds();
		return true;
	}
	if (FPlatformTime::Seconds() - SizedSeenAt < 0.15)
	{
		return true;
	}
	TArray<uint8> Png;
	if (!FFileHelper::LoadFileToArray(Png, *SizedShot))
	{
		return false;
	}
	IImageWrapperModule& Images = FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));
	TSharedPtr<IImageWrapper> Reading = Images.CreateImageWrapper(EImageFormat::PNG);
	TArray64<uint8> Pixels;
	if (!Reading.IsValid() || !Reading->SetCompressed(Png.GetData(), Png.Num()) || !Reading->GetRaw(ERGBFormat::BGRA, 8, Pixels))
	{
		return false;
	}
	TSharedPtr<IImageWrapper> Writing = Images.CreateImageWrapper(EImageFormat::JPEG);
	if (!Writing.IsValid() || !Writing->SetRaw(Pixels.GetData(), Pixels.Num(), Reading->GetWidth(), Reading->GetHeight(), ERGBFormat::BGRA, 8))
	{
		return false;
	}
	const TArray64<uint8>& Jpeg = Writing->GetCompressed(92);
	if (Reading->GetWidth() != RenderSize.X || Reading->GetHeight() != RenderSize.Y || !SizedMovie->add(Jpeg.GetData(), static_cast<size_t>(Jpeg.Num())))
	{
		return false;
	}
	IFileManager::Get().Delete(*SizedShot, false, true, true);
	++RenderFrame;
	if (FPlatformTime::Seconds() - RenderSaidAt >= 2.0 || RenderFrame >= RenderFrames)
	{
		RenderSaidAt = FPlatformTime::Seconds();
		UE_LOG(LogLvShow, Display, TEXT("LvShow: render: frame %lld of %lld"), RenderFrame, RenderFrames);
	}
	if (RenderFrame < RenderFrames)
	{
		SizedStep = 0;
		return true;
	}
	std::string Error;
	const int32 Frames = static_cast<int32>(SizedMovie->frames());
	const double Length = SizedMovie->seconds();
	if (!SizedMovie->close(Error))
	{
		UE_LOG(LogLvShow, Warning, TEXT("LvShow: video: not saved: %s"), *LvToFString(Error));
	}
	else
	{
		UE_LOG(LogLvShow, Display, TEXT("LvShow: video: saved %s: %d frames, %.1f seconds"), *RenderOut, Frames, Length);
	}
	SizedMovie.Reset();
	FApp::SetFixedDeltaTime(1.0 / RenderRate);
	RenderStage = 4;
	return true;
}

bool ULvShowSubsystem::ViewOf(int32 Index, FVector& OutLocation, FRotator& OutRotation) const
{
	const FView* View = Index == 0 ? &StartView : (Index >= 1 && Index <= 9 ? &Views[Index - 1] : nullptr);
	if (!View || !View->bStored)
	{
		return false;
	}
	OutLocation = View->Location;
	OutRotation = View->Rotation;
	return true;
}

void ULvShowSubsystem::PlaceRenderCamera(double Seconds)
{
	APlayerController* Controller = GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr;
	APawn* Pawn = Controller ? Controller->GetPawn() : nullptr;
	if (!Pawn)
	{
		return;
	}
	// The stored views of the path, in order; the camera is at a view at its time and eases from each to the next.
	TArray<TPair<FTransform, double>> Keys;
	for (const TPair<int32, double>& Key : RenderPath)
	{
		FVector Location;
		FRotator Rotation;
		if (ViewOf(Key.Key, Location, Rotation))
		{
			Keys.Emplace(FTransform(Rotation, Location), Key.Value);
		}
	}
	if (Keys.IsEmpty())
	{
		return;
	}
	int32 Next = 0;
	while (Next < Keys.Num() && Keys[Next].Value <= Seconds)
	{
		++Next;
	}
	FTransform At;
	if (Next == 0)
	{
		At = Keys[0].Key;
	}
	else if (Next == Keys.Num())
	{
		At = Keys.Last().Key;
	}
	else
	{
		const TPair<FTransform, double>& From = Keys[Next - 1];
		const TPair<FTransform, double>& To = Keys[Next];
		const double Along = FMath::Clamp((Seconds - From.Value) / FMath::Max(To.Value - From.Value, 1e-6), 0.0, 1.0);
		const double Eased = Along * Along * (3.0 - 2.0 * Along);
		At.SetLocation(FMath::Lerp(From.Key.GetLocation(), To.Key.GetLocation(), Eased));
		At.SetRotation(FQuat::Slerp(From.Key.GetRotation(), To.Key.GetRotation(), Eased));
	}
	bGliding = false;
	Pawn->SetActorLocation(At.GetLocation());
	Controller->SetControlRotation(At.Rotator());
}
