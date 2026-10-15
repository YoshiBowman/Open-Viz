#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "LvShowSubsystem.generated.h"

class ALvFixtureActor;
class ALvScreenActor;
class APlayerController;

/** Fired when a show has been laid out in the editor's level; the box encloses its fixtures. */
DECLARE_MULTICAST_DELEGATE_OneParam(FLvShowReadyToEdit, const FBox&);

/**
 * Opens a show when the process is started with -LvShow or -LvShowGdtf: builds a bare stage,
 * spawns the rig and listens for DMX.
 *
 * Started with -game it is the visualizer: a window for watching the rig under live DMX.
 * Nothing can be changed there. The camera is flown like the Unreal Editor's own viewport:
 * the pointer stays free, and the view turns only while the right mouse button is held.
 *
 *   right button + move     look around
 *   W A S D, Q and E        move; down and up
 *   Shift                   four times as fast
 *   wheel                   forward and back
 *   middle button + move    slide the view sideways and up and down
 *   Tab                     switch to game-style mouse (always looking, no pointer) and back
 *
 * Started in the Unreal Editor it is where a show is edited. The rig appears in the level as
 * fixtures that can be selected, moved and turned with the editor's tools; universe, address
 * and mode are in each one's Details panel. Whatever differs from the rig file is saved, as it
 * happens, to an edits file beside the MVR (vizcore/show_edits.h), and both the editor and the
 * visualizer lay that over the MVR the next time the show is opened. Pressing Play in the
 * editor shows the rig as the visualizer would.
 *
 *   -LvShow=<.mvr>            the rig to open
 *   -LvShowGdtf=<.gdtf>       or: a row of one fixture type, to look at it
 *   -LvShowGdtfMode=<name>    its DMX mode (default: the file's first)
 *   -LvShowCount=<n>          how many of it (default 6)
 *   -LvScenery=0|1            also put the rig file's stage, trusses and set pieces on stage (default 1)
 *   -LvProtocol=artnet|sacn   default artnet
 *   -LvDmxAddress=<ip>        the network address to listen on (default 127.0.0.1)
 *   -LvDmxVia=<name>@<ip>     only changes what is reported: the data really comes from there, through Openviz's relay
 *   -LvExposure=<stops>       default -8, which suits photometric fixtures
 *   -LvHaze=<density>         fog density, 0 to 0.5 (default 0.06)
 *   -LvAmbient=<0..1>         house lights: an even light on everything (default 0, off)
 *   -LvCameraAt=<x,y,z>       where the view starts, in centimetres (default: in front of the rig)
 *   -LvCameraLook=<x,y,z>     and what it looks at
 *   -LvView=<1..9>            start at that stored view; -LvStoreView=<1..9> stores the starting view under that number
 *   -LvMouseLook=hold|always  hold: look while the right button is held (default); always: game style
 *   -LvLookSpeed=<0.2..3>     how far the view turns for a given mouse movement (default 1)
 *   -LvMoveSpeed=<cm/s>       flying speed (default 600)
 *   -LvInvertY=0|1            mouse up looks down
 *   -LvBeams=cone|fog         cone: every beam drawn on its own cone (default); fog: beams only light the fog
 *   -LvBeamShadows=0|1        whether what stands in a beam cuts the part of it behind (default 1)
 *   -LvHazers=0|1             whether fog and haze machines in the rig, driven by DMX, add to the haze (default 1)
 *   -LvHazeDrift=<0..1>       how unevenly the haze hangs in the beams: 0 even, 1 thinning to nothing in places (default 0.4)
 *   -LvShadows=0|1            default 0
 *   -LvMegaLights=0|1         default: whatever the project is set to
 *   -LvBeamRes=<25..100>      draw beams in haze at this percentage of the picture's resolution (default 100)
 *   -LvAdaptBeams=0|1         lower that by itself while the picture runs slower than 30 frames a second, and put it back when it does not (default 1)
 *   -LvShowUnplaced=0|1       also show fixtures the rig file gives no place: they all stand in one spot (default 0)
 *   -LvPicture=<file>         Openviz's picture settings as JSON, looked at twice a second: what changes there is taken up
 *                             while the show is open (exposure, haze and its drifts, house lights, beam style, detail and
 *                             shadows, shadows on surfaces, MegaLights, hazers, the camera's mouse mode and speeds)
 *   -LvIdleLook=0|1           light fixtures at their defaults until DMX arrives (default 1)
 *   -LvDmx=<slot:level,...>   with the idle look: these levels in these slots of every fixture, slots counted
 *                             from the fixture's own first. For seeing a gobo or a prism without a console
 *   -LvPictures=<folder>      where the P key saves pictures (default: the project's Saved/Screenshots)
 *   -LvShot=<file>            save a screenshot, -LvShotAfter=<seconds> in (default 8)
 *   -LvViewSelfTest           press the view keys the way a hand would (through the engine's input), check what they do, then quit
 *   -LvShotQuit               and quit once it is saved
 *   -LvShowDump=<file>        write where every fixture is and how it is patched, for checks
 *   -LvQuitAfter=<seconds>    quit by itself, for checks
 *   -LvExec=<command>@<s>     run an engine console command that many seconds in, for looking into the picture's cost ("ProfileGPU@30")
 *   -LvFixtureLibrary=<dir>   the folder of GDTF files that fixtures added in Openviz take their types from
 *   -LvScreens=<file>         what the show's screens show, and screens added in Openviz (default: "<show>.screens.json" beside the show)
 *   -LvPieces=<file>          the set pieces added in Openviz: decks, drapes, trusses (default: "<show>.pieces.json" beside the show)
 *   -LvEditSelfTest           in the editor: move, re-patch and re-mode three fixtures, then quit
 *   -LvEditDeleteTest         in the editor: delete the last fixture of the rig file and the last added one, then quit
 *
 * While watching: 1 to 9 go to a stored view, Shift with 1 to 9 stores the present one (kept
 * beside the show), 0 goes back to where the view began, P saves a picture. T, F and L show the
 * rig as a plot does: from above, from the house, and from stage left, flat (seen from so far off
 * through so narrow a lens that nothing leans); there the wheel zooms and W A S D slide the picture.
 *
 *   -LvCommands=<file>        Openviz's word to a visualizer that is open, as JSON {"serial", "command", "value"}: looked at
 *                             several times a second and done when the serial changes. "view" with "1" to "9", "0", "top",
 *                             "front" or "side"; "store" with "1" to "9"; "picture" with "" (as on screen) or "3840x2160";
 *                             "record" with "start" or "stop": a video of the window, into -LvVideos.
 *   -LvVideos=<folder>        where videos recorded while watching go
 *   -LvRender=<file.dmxrec>   render this DMX recording into the movie -LvRenderOut=<file.mov>, frame by frame at
 *                             -LvRenderRate=<n> frames a second (default 30), every frame drawn in full at a fixed clock,
 *                             with only the recording's levels heard; then quit. The camera is at -LvRenderView=<0..9>
 *                             (a stored view, 0 the start), or follows -LvRenderPath=<view@seconds,...>, easing from
 *                             each view to the next ("1@0,1@4,3@8": view 1, held four seconds, then to view 3 by eight).
 *
 * What happened is logged in lines starting "LvShow:", which the launcher reads.
 *
 * While a show is open, in the visualizer or in the editor, the files Openviz keeps beside it are
 * looked at twice a second and what changed in them is taken up at once: fixtures moved,
 * re-patched, renamed, added or taken out (the edits), fixture files that stand in for types and
 * which way a type's pan and tilt run (the types), layers and classes shown or not (the hidden
 * list), and what the screens show.
 */
class FLvVideo;
namespace viz { class DmxRecordingReader; }

UCLASS()
class LVSHOW_API ULvShowSubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
	virtual void OnWorldBeginPlay(UWorld& InWorld) override;
	virtual void Deinitialize() override;
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;
	virtual bool IsTickableInEditor() const override { return true; }

	/** For editor-only code that wants to react, such as framing the viewport on the rig. */
	static FLvShowReadyToEdit& OnReadyToEdit();

protected:
	virtual bool DoesSupportWorldType(const EWorldType::Type WorldType) const override;

private:
	void Start(UWorld& World);
	void SpawnRig(UWorld& World, const FString& MvrPath);
	void SpawnAmbient(UWorld& World, float Level);
	void ApplyMouseMode(APlayerController& Controller);
	void UpdateCamera();
	TArray<ALvFixtureActor*> LiveFixtures() const;
	void SaveEdits();
	void StepSelfTest();
	void WriteDump(const FString& Path) const;
	void SpawnPreview(UWorld& World, const FString& GdtfPath);
	TArray<ALvScreenActor*> SpawnScreens(UWorld& World, const FString& MvrPath);
	void ReportScreens();
	void FollowScreensFile(UWorld& World);
	void LeaveWallsToScreens(const TArray<struct FLvScreenSetting>& Settings);
	void HideByLayerAndClass(AActor& Actor) const;
	/** Gives a fixture the picture settings every fixture of the show has: for those built at the start and those built later. */
	void DressFixture(ALvFixtureActor& Fixture);
	/** The one place that says whether a fixture is to be seen, lit and ticking: its layer and class, a screen in its place, and whether it has a place at all. */
	void ShowOrHide(ALvFixtureActor& Fixture);
	bool IsHiddenByChoice(const FString& Layer, const FString& Class) const;
	bool ReadHidden();
	void ApplyHidden(UWorld& World);
	void FollowShowFiles(UWorld& World);
	void GiveLooks();
	void WatchFrameRate(float DeltaTime);
	void FollowPictureFile(UWorld& World);
	void FollowCommands();
	void PlanView(const FString& Which);
	void LeavePlanView();
	FBox StageBounds() const;
	void SpawnPieces(UWorld& World);
	void FollowPiecesFile(UWorld& World);
	void ApplyHaze();
	void SetBeamDetail(float Percent);
	bool PlaceCamera(UWorld& World);
	FString ViewsPath() const;
	void LoadViews();
	void SaveViews() const;
	void GlideTo(const FVector& Location, const FRotator& Rotation);
	void TakePicture(int32 Width = 0, int32 Height = 0);

	// Video: what the window shows, into a movie (LvVideo.h). Live while watching, on Openviz's word;
	// or a DMX recording rendered frame by frame at a fixed time step (-LvRender).
	TSharedPtr<FLvVideo> Video;
	FString VideosFolder;            // -LvVideos: where live recordings go
	void StartVideo();
	void FollowVideo();
	bool IsFilming() const;
	FString RenderRecording;         // -LvRender: the recording to render
	FString RenderOut;               // -LvRenderOut: the movie it goes into
	int32 RenderRate = 30;           // -LvRenderRate: frames a second
	int32 RenderView = 0;            // -LvRenderView: the stored view to render from, 0 the start
	TArray<TPair<int32, double>> RenderPath;    // -LvRenderPath=1@0,1@4,3@8: the views the camera is at, and when; it eases from one to the next
	bool ViewOf(int32 Index, FVector& OutLocation, FRotator& OutRotation) const;
	void PlaceRenderCamera(double Seconds);
	int32 RenderWarmup = 60;         // frames drawn at the recording's start before the movie begins, for things to settle
	int32 RenderStage = 0;           // 0 not rendering, 1 waiting for the stage, 2 settling, 3 recording, 4 finishing
	int64 RenderFrame = 0;
	int64 RenderFrames = 0;          // how many the movie will have
	double RenderSaidAt = 0.0;
	TSharedPtr<viz::DmxRecordingReader> Reader;
	void StepRender();

	/** Views stored with the number keys, kept beside the show as "<show>.views.txt". */
	struct FView
	{
		bool bStored = false;
		FVector Location = FVector::ZeroVector;
		FRotator Rotation = FRotator::ZeroRotator;
	};
	FView Views[9];
	FView StartView;             // where the view began: the 0 key goes back to it
	bool bGliding = false;       // on the way to a recalled view
	double GlideStarted = 0.0;
	FVector GlideFrom = FVector::ZeroVector;
	FVector GlideEnd = FVector::ZeroVector;
	FQuat GlideFromTurn = FQuat::Identity;
	FQuat GlideEndTurn = FQuat::Identity;
	FString PicturesFolder;      // where the P key saves pictures

	TArray<TWeakObjectPtr<ALvFixtureActor>> Fixtures;
	TArray<TWeakObjectPtr<class ALvSceneryActor>> Pieces;
	TArray<TWeakObjectPtr<ALvScreenActor>> Screens;
	// The show's files as last seen: when each was changed, and what the two that say how the rig stands held.
	FDateTime EditsStamp;
	FDateTime TypesStamp;
	FDateTime HiddenStamp;
	FString EditsSeen;
	FString TypesSeen;
	double NextFilesLook = 0.0;
	bool bReconciling = false;       // fixtures are being taken away by us, not by someone in the editor
	TArray<TWeakObjectPtr<class ALvSceneryActor>> AddedPieces;    // the set pieces added in Openviz
	FString PiecesFile;
	FDateTime PiecesStamp;
	FString CommandsFile;            // Openviz's word to an open visualizer: views to go to, pictures to take
	FDateTime CommandsStamp;
	int64 CommandSerial = -1;
	double NextCommandsLook = 0.0;
	bool bPlan = false;              // one of the flat views: from above, from the house, from stage left
	float PlanFov = 10.0f;
	double PlanDistance = 30000.0;
	FString PictureFile;             // Openviz's picture settings, taken up while the show is open
	FDateTime PictureStamp;
	float ExposureNow = -8.0f;
	float AmbientNow = 0.0f;
	TWeakObjectPtr<class ASkyLight> AmbientLight;
	bool bShowUnplaced = false;
	// The picture settings every fixture gets, kept for fixtures that are built later.
	bool bSurfaceShadows = false;
	bool bBeamShadowsOn = true;
	float HazeDriftSetting = 0.4f;
	TArray<TPair<int32, uint8>> Poked;
	// How fast the picture runs, and the beam detail that is lowered while it runs too slowly.
	bool bAdaptBeams = true;
	float BeamDetailSetting = 100.0f;    // what was asked for
	float BeamDetailNow = 100.0f;        // what it is drawn at
	double FrameSeconds = 0.0;
	int32 FrameCount = 0;
	double SlowestFrame = 0.0;
	double NextFrameLook = 0.0;
	double NextFrameSay = 0.0;
	double LastRaised = -100.0;
	TArray<FString> ScreensSaid;     // what was last logged about each screen's picture
	TSet<TWeakObjectPtr<ALvFixtureActor>> BehindScreens;    // fixtures left out because a screen stands in for them
	bool bScreensSettled = true;     // nothing more to say about them
	FString ScreensFile;             // where Openviz keeps what the screens show; watched while the show runs
	FDateTime ScreensStamp;          // when that file was last changed, as last seen
	FString ScreensShape;            // which screens there are and where: if this changes they are built again
	double NextScreensLook = 0.0;
	TSet<FString> HiddenLayers;      // chosen in Openviz not to be shown
	TSet<FString> HiddenClasses;
	TSet<int32> LiveUniverses;
	FString Listening;
	FString ShowPath;            // the MVR being shown; empty for a fixture preview
	FString LibraryFolder;       // where Openviz keeps its fixture types, for fixtures added to a show
	TArray<FString> RemovedUuids;    // fixtures of the MVR taken out of the show: kept when the edits are saved
	FDelegateHandle ActorDeletedHandle;
	void OnActorDeleted(AActor* Actor);
	int32 SelfTestKind = 0;      // 0: move, re-patch and re-mode; 1: delete two fixtures
	FString LastSavedEdits;      // the edits file's text as last written, or as found
	double NextSave = 0.0;
	double QuitAfter = -1.0;
	FString ExecCommand;         // -LvExec: an engine console command, and when it is due
	double ExecAt = -1.0;
	int32 SelfTestStage = -1;    // -1: not asked for
	double SelfTestAt = 0.0;
	bool bEditing = false;       // in the editor's own world, as opposed to playing
	// Haze: the setting, and what the rig's own machines add to it, following their DMX with the
	// slowness of real haze (it builds over a quarter of a minute and clears over a minute).
	float HazeSetting = 0.06f;
	bool bRigHaze = true;
	bool bConeBeams = true;
	int32 Hazers = 0;            // fixtures with a fog or haze channel
	double HazeAdded = 0.0;      // density the machines have put into the air so far
	double HazeApplied = -1.0;   // what the fog and the cones were last set to
	double NextHazeLook = 0.0;
	void FollowHazers(float DeltaTime);
	bool bCameraControl = false; // the visualizer's camera is being flown by UpdateCamera
	// The number keys: when each was pressed (-1 while it is not), and whether holding it has stored a view already.
	double NumberHeldSince[10];
	bool bNumberStored[10];
	void StoreView(int32 Index);
	void RecallView(int32 Index);
	// -LvViewSelfTest: the keys pressed in turn and what was expected of them.
	int32 ViewTestStage = -1;
	double ViewTestAt = 0.0;
	int32 ViewTestPassed = 0;
	int32 ViewTestFailed = 0;
	FVector ViewTestFrom = FVector::ZeroVector;
	FRotator ViewTestFromTurn = FRotator::ZeroRotator;
	void StepViewTest();
	void PressKey(const FKey& Key, bool bDown);
	bool bAlwaysLook = false;
	bool bInvertY = false;
	float LookSpeed = 1.0f;
	float MoveSpeed = 600.0f;
	FString ShotPath;
	double ShotAfter = 8.0;
	double Elapsed = 0.0;
	double NextStatus = 0.0;
	double QuitAt = -1.0;
	bool bIdleLook = true;
	bool bShotTaken = false;
	bool bCameraPlaced = false;
	bool bActive = false;
};
