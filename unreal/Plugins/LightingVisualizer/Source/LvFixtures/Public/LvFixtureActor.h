#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "LvFixtureLibrary.h"
#include "LvFixtureActor.generated.h"

class UMaterialInterface;
class USceneComponent;
class USpotLightComponent;
class UStaticMeshComponent;
struct FLvGateLook;

/**
 * A lighting fixture built at runtime from a GDTF file: one scene component per geometry, a spot
 * light per beam, pan and tilt moved by a motor model, all driven by raw DMX.
 *
 * Nothing about a particular fixture is hand-built; everything comes from the file: its parts'
 * 3D models (boxes of the stated size where it packs none), its colours and wheels, and what
 * shapes its beams: gobos, an animation wheel, prisms, iris, frost, focus and framing shutters.
 * LvGate.h says how those are drawn.
 */
UCLASS()
class LVFIXTURES_API ALvFixtureActor : public AActor
{
	GENERATED_BODY()

public:
	ALvFixtureActor();

	/** Builds the fixture from a .gdtf on disk. Universe and Address are the patch of DMX break 1. */
	UFUNCTION(BlueprintCallable, Category = "Openviz")
	bool InitializeFromGdtfFile(const FString& GdtfPath, const FString& ModeName, int32 InUniverse, int32 InAddress);

	/** Builds the fixture from an already loaded definition. Returns false if the mode does not exist. */
	bool Initialize(TSharedPtr<const FLvFixtureDefinition> InDefinition, int32 ModeIndex, int32 InUniverse, int32 InAddress);

	/**
	 * Feeds these slots as the fixture's DMX instead of what the input ports receive; slot 0 is
	 * the fixture's first channel. An empty view returns to the ports.
	 */
	void SetDmxOverride(TConstArrayView<uint8> Slots);

	/**
	 * Some fixtures take their DMX in several separately patched blocks ("breaks" in GDTF): the
	 * control channels at one address, the pixels at another. Universe and Address above are the
	 * patch of break 1; this sets the patch of any other. Universe 0 leaves it unpatched.
	 */
	void SetBreakPatch(int32 Break, int32 InUniverse, int32 InAddress);
	/** False for a break that has no patch. Break 1 answers with Universe and Address. */
	bool GetBreakPatch(int32 Break, int32& OutUniverse, int32& OutAddress) const;
	/** The same as SetDmxOverride, for a break other than the first. */
	void SetBreakOverride(int32 Break, TConstArrayView<uint8> Slots);

	/**
	 * Lights the fixture without a console: every channel at its default with the dimmers at
	 * `DimmerLevel` (0-1), fed as an override.
	 */
	void SetDefaultLook(float DimmerLevel);

	/**
	 * Changes one slot of the look SetDefaultLook or SetDmxOverride put in force; `Slot` counts
	 * from 1, as on a console. For trying a gobo or a prism without one.
	 */
	void SetLookSlot(int32 Slot, uint8 Value);

	/** Turns shadow casting on or off for every beam. */
	void SetCastShadows(bool bInCastShadows);

	int32 GetUniverse() const { return Universe; }
	int32 GetAddress() const { return Address; }

	virtual void Tick(float DeltaSeconds) override;

	/** Number of DMX slots the mode occupies on break 1. */
	int32 GetFootprint() const;

	/** True once a universe containing this fixture has arrived, or an override is set. */
	bool IsReceivingDmx() const;

	/** Where the motor of the first axis on this geometry is now: degrees, or metres for a part that slides. */
	bool GetAxisAngle(const FString& GeometryName, double& OutDegrees) const;

	USceneComponent* GetGeometryComponent(const FString& GeometryName) const;

	/**
	 * The part of this fixture that what hangs on it rides on: the geometry an MVR's ChildPosition
	 * names, else the deepest part an XYZ or Rot channel moves (a winch's hook, a turntable's top).
	 * Null when the fixture has neither: what hangs on it then stays where it is.
	 */
	USceneComponent* GetCarrier(const FString& ChildPosition) const;
	/** Null for a beam the file marks as BeamType "None", which has no light of its own. */
	USpotLightComponent* GetBeamLight(const FString& GeometryName) const;

	/** The cone that beam is drawn on in haze, or null if it has none. For checks. */
	UStaticMeshComponent* GetBeamCone(const FString& GeometryName) const;
	int32 GetBeamCount() const { return BeamLights.Num(); }

	/** What the DMX currently asks of the fixture, before motors and strobes. Null until initialised. */
	const viz::FixtureState* GetState() const;

	/** Whether the fixture has a fog or haze channel, and how hard that machine is working now, 0 to 1. */
	bool HasFogControl() const;
	float GetFogOutput() const;

	/**
	 * For ULvGateSubsystem, which answers this fixture's requests for gate materials. A beam's
	 * handle names the look it holds, or is INDEX_NONE; taking it leaves the beam without one.
	 * ApplyGate is false if the fixture no longer has that beam.
	 */
	int32 TakeGateHandle(int32 Beam);
	bool ApplyGate(int32 Beam, int32 Handle, UMaterialInterface* LightFunction, UMaterialInterface* BeamMaterial);
	/** Changes whenever the fixture is rebuilt: a request made before is not for the beams it has now. */
	int32 GetGateSerial() const { return GateSerial; }

	virtual void Destroyed() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

	/** Whether the beams' lights cast shadows. Off by default: it is the expensive part. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Openviz")
	bool bCastShadows = false;

	/** Multiplies the photometric intensity computed from the GDTF's luminous flux. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Openviz")
	float IntensityScale = 1.0f;

	/**
	 * How strongly the beams light volumetric fog. A look setting, not a physical one: it stands
	 * in for a proper beam renderer (roadmap phase 3). Read when the fixture is built.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Openviz")
	float HazeScattering = 6.0f;

	/** This fixture's own inversions, as the rig file's DMXInvertPan and DMXInvertTilt: the axis is mirrored about the middle of its range. Saved with the show. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Openviz")
	bool bInvertPan = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Openviz")
	bool bInvertTilt = false;

	/**
	 * How its fixture type's pan and tilt are read in this show (vizcore/rig.h, TypeSwap): mirrored
	 * once more where the fixture file and the console run an axis opposite ways, and turned by a
	 * number of degrees where they start it from different angles. Set for every fixture of the
	 * type from Openviz's Fixture types panel; not something a single fixture has.
	 */
	bool bTypeInvertPan = false;
	bool bTypeInvertTilt = false;
	double PanOffset = 0.0;
	double TiltOffset = 0.0;

	/**
	 * A fixture the rig file gives no place: it stands where whatever it sits in stands, on top
	 * of every other such fixture. It is kept dark (and, while watching, out of sight) until it
	 * is given a place.
	 */
	void SetUnplaced(bool bInUnplaced);
	bool IsUnplaced() const { return bUnplaced; }

	/** Whether the fixture follows its DMX and lights its beams each frame. One that does not stays as it was last; SetUnplaced also makes it dark. */
	void SetTicking(bool bTick);

	/** How many beams are drawn: fewer than GetBeamCount where lenses side by side are drawn as one. */
	int32 GetDrawnBeamCount() const;
	/** As of the last frame: how many of its beams were lit and drawn in haze, and for how many of those a shadow was worked out because something stands in them. */
	int32 GetLitBeamCount() const { return LitBeams; }
	int32 GetShadedBeamCount() const { return ShadedBeams; }

	/**
	 * How beams show in haze. True: each beam is drawn on a cone, with a clean edge and its own
	 * length (roadmap phase 3). False: beams only light the engine's volumetric fog, which blurs
	 * them together. `Haze` is how much light the air sends toward the eye, per metre.
	 */
	void SetBeamStyle(bool bCones, float Haze);

	/**
	 * Whether things standing in a beam cut the part of it behind them. Without it a beam ends,
	 * whole, where its middle lands. It costs a handful of line traces per lit beam and frame.
	 */
	void SetBeamShadows(bool bShadows) { bBeamShadows = bShadows; }

	/**
	 * How unevenly the haze hangs in beams drawn on cones: 0 even, 1 thinning to nothing in
	 * places. The drifts are the same for every fixture and wander slowly through the room.
	 */
	void SetHazeDrift(float Amount) { HazeDrift = FMath::Clamp(Amount, 0.0f, 1.0f); }

	/** The patch of DMX break 1. Universe 0 is never received: an unpatched fixture. */
	UPROPERTY(EditAnywhere, Category = "Openviz|Patch", meta = (ClampMin = 0, ClampMax = 32767))
	int32 Universe = 1;

	UPROPERTY(EditAnywhere, Category = "Openviz|Patch", meta = (ClampMin = 1, ClampMax = 512))
	int32 Address = 1;

	/** The GDTF DMX mode. Choosing another rebuilds the fixture where it hangs. */
	UPROPERTY(EditAnywhere, Category = "Openviz|Patch", meta = (GetOptions = "GetModeNames"))
	FString Mode;

	/** The modes the fixture's GDTF offers. */
	UFUNCTION()
	TArray<FString> GetModeNames() const;

	/** Switches to another of the GDTF's modes, keeping place and patch. False if it has no such mode. */
	bool SetMode(const FString& ModeName);

	/** What the rig file calls the fixture, and what kind it is. For reading, not for changing. */
	UPROPERTY(VisibleAnywhere, Category = "Openviz")
	FString FixtureName;

	UPROPERTY(VisibleAnywhere, Category = "Openviz")
	FString FixtureType;

	/** The layer and the class the rig file files it under; either may be empty. */
	UPROPERTY(VisibleAnywhere, Category = "Openviz")
	FString Layer;

	UPROPERTY(VisibleAnywhere, Category = "Openviz")
	FString Class;

	/**
	 * How the rig file described the fixture, set by LvRig. What differs from this has been
	 * changed since, and is what gets saved as an edit.
	 */
	FString SourceUuid;
	FTransform SourceTransform;
	int32 SourceUniverse = 0;
	int32 SourceAddress = 1;
	FString SourceMode;
	bool bSourceInvertPan = false;
	bool bSourceInvertTilt = false;
	bool bSourcePlaced = true;
	/** Its name and number as the rig file has them, and as the show has them now (changed in Openviz, not here). */
	FString SourceName;
	FString SourceFixtureId;
	FString ShowName;
	FString ShowFixtureId;
	/** What its type is read from: the rig file's own entry or a file of the fixture library. A fixture whose type changes is built anew. */
	FString TypeKey;
	/** A type of its own that the show's edits give it (a file of Openviz's fixture library), in place of the rig file's; empty if none. Written back with its mode. */
	FString OwnGdtf;

	/**
	 * A fixture that is not in the rig file but was added in Openviz. Everything about it is kept
	 * in the show's edits: its type (a file in Openviz's fixture library), its name and number as
	 * given there, and its place as last read, in MVR's terms, so that a fixture nobody moved is
	 * written back exactly as it was.
	 */
	bool bAdded = false;
	FString AddedGdtf;
	FString AddedName;
	FString AddedFixtureId;
	viz::Transform AddedWorld;

#if WITH_EDITOR
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
	/** In the editor's viewport a fixture shows its look and moves too, not only while playing. */
	virtual bool ShouldTickIfViewportsOnly() const override { return true; }
#endif

private:
	void BuildComponents();
	void ReleaseGates();

	/**
	 * Works out what is in a beam's path this frame. False if nothing is. Otherwise `OutLook` is
	 * what to draw, `OutRoll` how far the light itself is to be turned about its beam, in degrees,
	 * and `OutSpread` how many times wider than the plain beam the light's cone must be.
	 */
	bool BuildGateLook(int32 Beam, float DeltaSeconds, double FieldAngle, double CoreShare, FLvGateLook& OutLook, double& OutRoll, double& OutSpread);

	int32 GateSerial = 0;

	UPROPERTY(Transient)
	TArray<TObjectPtr<USceneComponent>> GeometryComponents;

	UPROPERTY(Transient)
	TArray<TObjectPtr<USpotLightComponent>> BeamLights;

	UPROPERTY(Transient)
	TArray<TObjectPtr<UStaticMeshComponent>> BodyMeshes;

	/** One per beam, in the model's order like BeamLights; null where the beam has no light. */
	UPROPERTY(Transient)
	TArray<TObjectPtr<UStaticMeshComponent>> BeamCones;

	/** One per beam: the lens or LED cell that glows with the beam's colour; null if it has no size. */
	UPROPERTY(Transient)
	TArray<TObjectPtr<UStaticMeshComponent>> BeamSurfaces;

	/** Break number to universe (X) and address (Y), for breaks other than the first. */
	TMap<int32, FIntPoint> BreakPatch;

	bool bUnplaced = false;
	int32 LitBeams = 0;
	int32 ShadedBeams = 0;
	bool bConeBeams = true;
	bool bBeamShadows = true;
	float ConeHaze = 0.00036f;
	float HazeDrift = 0.0f;

	FString BuiltMode;                 // the mode the components were last built for
	float DefaultLookLevel = -1.0f;    // the level SetDefaultLook was last given, or below 0 if not in force

	struct FRuntime;
	TSharedPtr<FRuntime> Runtime;
};
