#include "LvFixtureActor.h"

#include "Components/SceneComponent.h"
#include "Components/SpotLightComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "Engine/Texture2D.h"
#include "HAL/IConsoleManager.h"
#include "LvCoordinates.h"
#include "LvDmxSubsystem.h"
#include "LvGate.h"
#include "Materials/MaterialInterface.h"
#include "UObject/ConstructorHelpers.h"

DEFINE_LOG_CATEGORY_STATIC(LogLvFixture, Log, All);

namespace
{
	// Where across a beam it is felt along for things standing in it: the middle, six places on a
	// ring halfway out and twelve near the rim. In cone radii; counted anticlockwise from the
	// first of the two directions across the beam. The beam material (tools/build_materials.py,
	// SHADE_STRUCT) reads them in this order.
	constexpr int32 kShadePlaces = 19;
	constexpr int32 kShadePerFrame = 6;

	// The cone a frost filter at full scatters light into, in degrees: GDTF gives frost only as an
	// amount, and this is about what a heavy frost does to a beam fixture.
	constexpr double kFrostDegrees = 20.0;

	const FVector2D* ShadePlaces()
	{
		static FVector2D Places[kShadePlaces];
		static bool bMade = false;
		if (!bMade)
		{
			Places[0] = FVector2D::ZeroVector;
			for (int32 Index = 0; Index < 6; ++Index)
			{
				const double Angle = UE_DOUBLE_PI * 2.0 * Index / 6.0;
				Places[1 + Index] = FVector2D(FMath::Cos(Angle), FMath::Sin(Angle)) * 0.5;
			}
			for (int32 Index = 0; Index < 12; ++Index)
			{
				const double Angle = UE_DOUBLE_PI * 2.0 * Index / 12.0;
				Places[7 + Index] = FVector2D(FMath::Cos(Angle), FMath::Sin(Angle)) * 0.9;
			}
			bMade = true;
		}
		return Places;
	}
}

namespace
{
	// Geometry names can hold characters an Unreal object name cannot ("Cell1.Lens", "DMX IN").
	FName ComponentNameFor(UObject* Outer, const std::string& GeometryName)
	{
		FString Name = LvToFString(GeometryName);
		for (TCHAR& Character : Name)
		{
			if (!FChar::IsAlnum(Character) && Character != TEXT('_'))
			{
				Character = TEXT('_');
			}
		}
		return MakeUniqueObjectName(Outer, USceneComponent::StaticClass(), FName(*Name));
	}
}

struct ALvFixtureActor::FRuntime
{
	TSharedPtr<const FLvFixtureDefinition> Definition;
	const viz::FixtureModel* Model = nullptr;

	std::vector<viz::ChannelValue> Values;
	viz::FixtureState State;

	TArray<FTransform> Rest;             // per geometry, relative to its parent
	TArray<viz::MotorAxis> Motors;       // per axis
	TArray<bool> MotorStarted;
	TArray<double> StrobePhase;          // per beam, in cycles
	TArray<double> StrobeChance;         // per beam: drawn anew each cycle, for the random kinds of strobe
	TArray<double> WheelTurns;           // per beam: how far a spinning colour wheel has turned, in revolutions

	// Last values pushed to each light, so unchanged ones cost nothing.
	struct FApplied
	{
		float Intensity = -1.0f;
		FLinearColor Color = FLinearColor::Black;
		float Inner = -1.0f;
		float Outer = -1.0f;
		// And to its cone.
		FVector4f Glow = FVector4f(-1.0f, -1.0f, -1.0f, -1.0f);
		FVector4f ConeLight = FVector4f(-1.0f, -1.0f, -1.0f, -1.0f);
		FVector4f ConeShape = FVector4f(-1.0f, -1.0f, -1.0f, -1.0f);
		float ConeLength = -1.0f;
		float ConeRadius = -1.0f;
		// How far along the beam light gets at each of the places it is felt at, from the cone's
		// apex in centimetres; what was last told to the cone about that; and which places are
		// next to be felt again.
		float Reach[kShadePlaces];
		FVector4f Shade[5];
		int32 ShadeNext = 0;
		bool bShadeKnown = false;
		// Whether something stands in the beam now, and whether its cone wears the material that
		// shades it (M_LvBeamShadow) rather than the plain one: only while it has no gate's material.
		bool bWantsShadow = false;
		bool bShadowMaterial = false;
		FApplied()
		{
			for (float& Place : Reach)
			{
				Place = 0.0f;
			}
			for (FVector4f& Told : Shade)
			{
				Told = FVector4f(-1.0f, -1.0f, -1.0f, -1.0f);
			}
		}
	};
	TArray<FApplied> Applied;

	// What is in each beam's path: where its turning parts have got to, and the look last asked for.
	struct FGate
	{
		int32 Handle = INDEX_NONE;
		bool bOpen = true;
		bool bUnavailable = false;    // asked for, but the materials are not in this build
		FLvGateLook Look;
		double Spin[viz::kGateLayers] = {};          // degrees each slot has turned in its holder
		double Shake[viz::kGateLayers] = {};         // where each is in its shake, in cycles
		double WheelTurns[viz::kGateLayers] = {};    // revolutions each wheel itself has made
		double PrismSpin[viz::kPrisms] = {};
		double IrisPhase = 0.0;
		double IrisChance = 0.0;
		float Roll = 0.0f;                           // how far the light is turned about its beam now
		bool bSplit = false;                         // the look shows a colour wheel's two colours side by side
		bool bGated = false;                         // its cone wears the gate's material
	};
	TArray<FGate> Gates;

	// Lenses side by side that are drawn as one beam (vizcore, BeamCluster). Per beam: the beam
	// that draws for it (itself for a beam alone or a cluster's leader). Per beam that draws: how
	// far across the opening it shines through is, in centimetres (0: its own lens), and where the
	// middle of that opening is from its own light, in the light's frame.
	TArray<int32> DrawnBy;
	TArray<float> Opening;
	TArray<FVector> OpeningAt;
	// This frame's light of each beam before it is added up: candela, and its colour.
	TArray<float> Candela;
	TArray<FLinearColor> Tint;
	TArray<FLinearColor> Clear;    // the colour without a colour wheel that stands between two slots, whose two colours the gate then shows

	TArray<uint8> Override;
	TMap<int32, TArray<uint8>> BreakOverride;     // the same for other breaks
	std::vector<viz::DmxBreakData> BreakData;     // this frame's DMX, break by break, when there is more than one
	bool bReceiving = false;
};

ALvFixtureActor::ALvFixtureActor()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.bStartWithTickEnabled = true;
	RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	RootComponent->SetMobility(EComponentMobility::Movable);
}

bool ALvFixtureActor::InitializeFromGdtfFile(const FString& GdtfPath, const FString& ModeName, int32 InUniverse, int32 InAddress)
{
	ULvFixtureLibrary* Library = ULvFixtureLibrary::Get();
	FString Error;
	TSharedPtr<const FLvFixtureDefinition> Definition = Library ? Library->LoadGdtfFile(GdtfPath, Error) : nullptr;
	if (!Definition)
	{
		UE_LOG(LogLvFixture, Error, TEXT("%s"), *Error);
		return false;
	}
	const int32 ModeIndex = Definition->FindMode(ModeName);
	if (ModeIndex < 0)
	{
		UE_LOG(LogLvFixture, Error, TEXT("%s has no DMX mode named '%s'"), *GdtfPath, *ModeName);
		return false;
	}
	return Initialize(Definition, ModeIndex, InUniverse, InAddress);
}

bool ALvFixtureActor::Initialize(TSharedPtr<const FLvFixtureDefinition> InDefinition, int32 ModeIndex, int32 InUniverse, int32 InAddress)
{
	const viz::FixtureModel* Model = InDefinition ? InDefinition->GetModel(ModeIndex) : nullptr;
	if (!Model || !Model->valid())
	{
		return false;
	}
	ReleaseGates();
	Runtime = MakeShared<FRuntime>();
	Runtime->Definition = InDefinition;
	Runtime->Model = Model;
	Universe = FMath::Clamp(InUniverse, 0, 32767);
	Address = FMath::Clamp(InAddress, 1, 512);
	Mode = LvToFString(InDefinition->Type.modes[static_cast<size_t>(ModeIndex)].name);
	BuiltMode = Mode;
	FixtureType = LvToFString(InDefinition->Type.manufacturer + " " + InDefinition->Type.name);

	// Start from the fixture's defaults, so it is posed sensibly before any DMX arrives.
	Model->evaluator().evaluate(nullptr, 0, Runtime->Values);
	Model->resolve(Runtime->Values, Runtime->State);
	Runtime->Motors.SetNum(static_cast<int32>(Model->axis_count()));
	Runtime->MotorStarted.Init(false, static_cast<int32>(Model->axis_count()));
	Runtime->StrobePhase.Init(0.0, static_cast<int32>(Model->beam_count()));
	Runtime->StrobeChance.Init(0.0, static_cast<int32>(Model->beam_count()));
	Runtime->WheelTurns.Init(0.0, static_cast<int32>(Model->beam_count()));
	Runtime->Applied.SetNum(static_cast<int32>(Model->beam_count()));
	Runtime->Gates.SetNum(static_cast<int32>(Model->beam_count()));
	const int32 BeamCount = static_cast<int32>(Model->beam_count());
	Runtime->DrawnBy.SetNum(BeamCount);
	Runtime->Opening.Init(0.0f, BeamCount);
	Runtime->OpeningAt.Init(FVector::ZeroVector, BeamCount);
	Runtime->Candela.Init(0.0f, BeamCount);
	Runtime->Tint.Init(FLinearColor::Black, BeamCount);
	Runtime->Clear.Init(FLinearColor::Black, BeamCount);
	for (int32 Beam = 0; Beam < BeamCount; ++Beam)
	{
		const int32 Cluster = Model->cluster_of(static_cast<size_t>(Beam));
		Runtime->DrawnBy[Beam] = Cluster >= 0 ? Model->clusters()[static_cast<size_t>(Cluster)].leader : Beam;
	}

	BuildComponents();
	SetBeamStyle(bConeBeams, ConeHaze);
	if (DefaultLookLevel >= 0.0f)
	{
		SetDefaultLook(DefaultLookLevel);    // a rebuilt fixture keeps the look it was given
	}
	return true;
}

TArray<FString> ALvFixtureActor::GetModeNames() const
{
	TArray<FString> Names;
	if (Runtime && Runtime->Definition)
	{
		for (const viz::DmxMode& Each : Runtime->Definition->Type.modes)
		{
			Names.Add(LvToFString(Each.name));
		}
	}
	return Names;
}

bool ALvFixtureActor::SetMode(const FString& ModeName)
{
	if (!Runtime || !Runtime->Definition)
	{
		return false;
	}
	if (ModeName == BuiltMode)
	{
		Mode = BuiltMode;
		return true;
	}
	// Initialize replaces Runtime, so the definition is held here while it does.
	const TSharedPtr<const FLvFixtureDefinition> Definition = Runtime->Definition;
	const int32 ModeIndex = Definition->FindMode(ModeName);
	if (ModeIndex < 0 || !Initialize(Definition, ModeIndex, Universe, Address))
	{
		Mode = BuiltMode;
		return false;
	}
	return true;
}

#if WITH_EDITOR
void ALvFixtureActor::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);
	const FName Changed = PropertyChangedEvent.GetPropertyName();
	if (Changed == GET_MEMBER_NAME_CHECKED(ALvFixtureActor, Mode))
	{
		SetMode(Mode);
	}
	else if (Changed == GET_MEMBER_NAME_CHECKED(ALvFixtureActor, bCastShadows))
	{
		SetCastShadows(bCastShadows);
	}
	Universe = FMath::Clamp(Universe, 0, 32767);
	Address = FMath::Clamp(Address, 1, 512);
}
#endif

void ALvFixtureActor::BuildComponents()
{
	// The boxes hang off the geometry components but are not destroyed with them.
	for (UStaticMeshComponent* Old : BodyMeshes)
	{
		if (Old)
		{
			Old->DestroyComponent();
		}
	}
	for (USceneComponent* Old : GeometryComponents)
	{
		if (Old)
		{
			Old->DestroyComponent();
		}
	}
	for (UStaticMeshComponent* Old : BeamCones)
	{
		if (Old)
		{
			Old->DestroyComponent();
		}
	}
	GeometryComponents.Reset();
	BeamLights.Reset();
	BodyMeshes.Reset();
	for (UStaticMeshComponent* Old : BeamSurfaces)
	{
		if (Old)
		{
			Old->DestroyComponent();
		}
	}
	BeamCones.Reset();
	BeamSurfaces.Reset();
	ULvFixtureLibrary* Library = ULvFixtureLibrary::Get();
	UStaticMesh* ConeMesh = Library ? Library->GetBeamCone() : nullptr;

	const std::vector<viz::GeometryInstance>& Tree = Runtime->Model->geometry();
	UStaticMesh* Cube = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));

	// Which geometries turn: their boxes are centred on the pivot instead of hanging below it.
	TSet<int32> Pivots;
	for (const viz::AxisState& Axis : Runtime->State.axes)
	{
		if (Axis.motion != viz::Motion::Slide)
		{
			Pivots.Add(Axis.geometry);
		}
	}

	Runtime->Rest.SetNum(static_cast<int32>(Tree.size()));
	for (int32 Index = 0; Index < static_cast<int32>(Tree.size()); ++Index)
	{
		const viz::GeometryInstance& Instance = Tree[Index];
		const FName ComponentName = ComponentNameFor(this, Instance.name);
		const bool bIsBeam = Instance.geometry->beam.has_value();
		// BeamType "None" means the geometry only glows, the pixel of an LED wall for one: it
		// gets no light of its own. "Glow" lights its surroundings but draws no beam in haze.
		// Nor does a lens that is one of a cluster drawn as a single beam, unless it is the one
		// in the middle that draws for all of them.
		const int32 BeamIndex = bIsBeam ? BeamLights.Num() : INDEX_NONE;
		const bool bDrawsItself = bIsBeam && Runtime->DrawnBy.IsValidIndex(BeamIndex) && Runtime->DrawnBy[BeamIndex] == BeamIndex;
		const bool bHasLight = bIsBeam && bDrawsItself && Instance.geometry->beam->beam_type != viz::BeamType::None;

		USceneComponent* Component = nullptr;
		if (bHasLight)
		{
			USpotLightComponent* Light = NewObject<USpotLightComponent>(this, ComponentName);
#if WITH_EDITORONLY_DATA
			Light->bVisualizeComponent = false;    // no editor icon per beam: a rig has thousands
#endif
			Light->SetMobility(EComponentMobility::Movable);
			Light->SetIntensityUnits(ELightUnits::Candelas);
			Light->SetIntensity(0.0f);
			Light->SetAttenuationRadius(6000.0f);
			Light->SetSourceRadius(static_cast<float>(Instance.geometry->beam->beam_radius * 100.0));
			Light->SetCastShadows(bCastShadows);
			Light->SetVolumetricScatteringIntensity(Instance.geometry->beam->beam_type == viz::BeamType::Glow ? 0.0f : HazeScattering);
			Component = Light;
		}
		else
		{
			Component = NewObject<USceneComponent>(this, ComponentName);
			Component->SetMobility(EComponentMobility::Movable);
		}
		if (bIsBeam)
		{
			// One entry per beam of the model, in the model's order; null where there is no light.
			BeamLights.Add(Cast<USpotLightComponent>(Component));
			BeamCones.Add(nullptr);
			BeamSurfaces.Add(nullptr);
		}

		USceneComponent* Parent = Instance.parent >= 0 ? GeometryComponents[Instance.parent].Get() : RootComponent.Get();
		Component->SetupAttachment(Parent);
		Component->RegisterComponent();
		AddInstanceComponent(Component);

		FTransform Rest = LvCoordinates::FromGdtf(Instance.local);
		if (bIsBeam)
		{
			// A GDTF beam shines along its own -Z; an Unreal spot light along its +X.
			Rest = FTransform(FRotator(-90.0, 0.0, 0.0)) * Rest;
		}
		Runtime->Rest[Index] = Rest;
		Component->SetRelativeTransform(Rest);
		GeometryComponents.Add(Component);

		// The cone the beam is drawn on in haze. It rides on the light, so it points where the
		// light points; its size is set each frame. A "Glow" beam lights things but shows no shaft.
		if (bHasLight && ConeMesh && Instance.geometry->beam->beam_type != viz::BeamType::Glow)
		{
			UStaticMeshComponent* Cone = NewObject<UStaticMeshComponent>(this);
			Cone->SetMobility(EComponentMobility::Movable);
			Cone->SetStaticMesh(ConeMesh);
			Cone->SetCollisionEnabled(ECollisionEnabled::NoCollision);
			Cone->SetCastShadow(false);
			Cone->SetReceivesDecals(false);
			Cone->SetVisibleInRayTracing(false);
			Cone->bAffectDynamicIndirectLighting = false;
			Cone->bAffectDistanceFieldLighting = false;
			Cone->SetVisibility(false);
			Cone->SetupAttachment(Component);
			Cone->RegisterComponent();
			BeamCones.Last() = Cone;
		}

		// What glows: the lens of a spot, the cell of an LED wall. Round with the beam's radius,
		// unless the part is described as a box, as LED cells are; then a rectangle of its size.
		if (bIsBeam && Library)
		{
			const viz::Model* Described = nullptr;
			for (const viz::Model& Candidate : Runtime->Definition->Type.models)
			{
				if (Candidate.name == Instance.geometry->model)
				{
					Described = &Candidate;
				}
			}
			const bool bBox = Described && Described->file.empty() && Described->primitive_type != "Cylinder" && Described->primitive_type != "Sphere" &&
				Described->length > 0.0 && Described->width > 0.0;
			const double Radius = Instance.geometry->beam->beam_radius * 100.0;
			if (UStaticMesh* Surface = (bBox || Radius > 0.05) ? Library->GetGlowSurface(!bBox) : nullptr)
			{
				UStaticMeshComponent* Glow = NewObject<UStaticMeshComponent>(this);
				Glow->SetMobility(EComponentMobility::Movable);
				Glow->SetStaticMesh(Surface);
				Glow->SetCollisionEnabled(ECollisionEnabled::NoCollision);
				Glow->SetCastShadow(false);
				Glow->SetupAttachment(Component);
				Glow->RegisterComponent();
				// A hair in front of the part, so that it is not hidden in its surface. The surface
				// is 1 cm across; the part's local +X is where the light goes. A box is centred on
				// its origin (the LED wall in grandMA3's demo rig only fits together that way), so
				// its front face is half its height further on.
				Glow->SetRelativeLocation(FVector(bBox ? Described->height * 50.0 + 0.1 : 0.3, 0.0, 0.0));
				Glow->SetRelativeScale3D(bBox ? FVector(1.0, Described->width * 100.0, Described->length * 100.0) : FVector(1.0, Radius * 2.0, Radius * 2.0));
				Glow->SetCustomPrimitiveDataVector4(0, FVector4(1.0, 1.0, 1.0, 0.0));
				BeamSurfaces.Last() = Glow;
			}
		}

		// The part's own 3D model where the GDTF packs one; otherwise a box of the size it states.
		const viz::Model* Model = nullptr;
		UStaticMesh* Shape = nullptr;
		const std::vector<viz::Model>& Models = Runtime->Definition->Type.models;
		for (size_t Candidate = 0; Candidate < Models.size(); ++Candidate)
		{
			if (Models[Candidate].name == Instance.geometry->model)
			{
				Model = &Models[Candidate];
				Shape = Runtime->Definition->ModelMeshes.IsValidIndex(static_cast<int32>(Candidate))
					? Runtime->Definition->ModelMeshes[static_cast<int32>(Candidate)].Get() : nullptr;
			}
		}
		if (Shape && !bIsBeam)
		{
			UStaticMeshComponent* Body = NewObject<UStaticMeshComponent>(this);
			Body->SetMobility(EComponentMobility::Movable);
			Body->SetStaticMesh(Shape);
			Body->SetCollisionEnabled(ECollisionEnabled::NoCollision);
			Body->SetCastShadow(false);
			Body->SetupAttachment(Component);
			Body->RegisterComponent();    // the mesh carries its own place relative to the part's origin
			BodyMeshes.Add(Body);
		}
		else if (Cube && Model && !bIsBeam && Model->length > 0.0 && Model->width > 0.0 && Model->height > 0.0)
		{
			UStaticMeshComponent* Box = NewObject<UStaticMeshComponent>(this);
			Box->SetMobility(EComponentMobility::Movable);
			Box->SetStaticMesh(Cube);
			Box->SetCollisionEnabled(ECollisionEnabled::NoCollision);
			Box->SetCastShadow(false);
			Box->SetupAttachment(Component);
			Box->RegisterComponent();
			// The engine cube is one metre across. GDTF's plain shapes (cube, cylinder, sphere) are
			// centred on the part's origin; the fixture-shaped ones hang from it.
			const bool bCentred = Model->primitive_type == "Cube" || Model->primitive_type == "Cylinder" || Model->primitive_type == "Sphere";
			Box->SetRelativeScale3D(FVector(Model->length, Model->width, Model->height));
			Box->SetRelativeLocation(FVector(0.0, 0.0, Pivots.Contains(Index) || bCentred ? 0.0 : -Model->height * 50.0));
			BodyMeshes.Add(Box);
		}
	}

	// A cluster's one beam leaves from the middle of its lenses and is as wide there as all of them.
	for (const viz::BeamCluster& Cluster : Runtime->Model->clusters())
	{
		USpotLightComponent* Light = BeamLights.IsValidIndex(Cluster.leader) ? BeamLights[Cluster.leader].Get() : nullptr;
		if (!Light)
		{
			continue;
		}
		FVector Middle = FVector::ZeroVector;
		for (int32 Member : Cluster.members)
		{
			const int32 Part = Runtime->State.beams[static_cast<size_t>(Member)].geometry;
			Middle += GeometryComponents[Part]->GetComponentLocation() / static_cast<double>(Cluster.members.size());
		}
		Runtime->OpeningAt[Cluster.leader] = Light->GetComponentTransform().InverseTransformPosition(Middle);
		Runtime->Opening[Cluster.leader] = static_cast<float>(Cluster.radius * 100.0);
		Light->SetSourceRadius(static_cast<float>(Cluster.radius * 100.0));
	}
}

void ALvFixtureActor::SetTicking(bool bTick)
{
	// Both: an actor that has not begun play yet is given its tick back when it does, unless it is also told not to start with one.
	PrimaryActorTick.bStartWithTickEnabled = bTick;
	SetActorTickEnabled(bTick);
	if (!bTick)
	{
		LitBeams = 0;
		ShadedBeams = 0;
	}
}

int32 ALvFixtureActor::GetDrawnBeamCount() const
{
	int32 Drawn = 0;
	for (const TObjectPtr<USpotLightComponent>& Light : BeamLights)
	{
		Drawn += Light ? 1 : 0;
	}
	return Drawn;
}

void ALvFixtureActor::SetUnplaced(bool bInUnplaced)
{
	if (bUnplaced == bInUnplaced)
	{
		return;
	}
	bUnplaced = bInUnplaced;
	SetTicking(!bUnplaced);
	if (bUnplaced)
	{
		// Dark, whatever it was showing: nothing of it ticks again until it has a place.
		for (int32 Index = 0; Index < BeamLights.Num(); ++Index)
		{
			if (USpotLightComponent* Light = BeamLights[Index])
			{
				Light->SetIntensity(0.0f);
			}
			if (UStaticMeshComponent* Cone = BeamCones.IsValidIndex(Index) ? BeamCones[Index].Get() : nullptr)
			{
				Cone->SetVisibility(false);
			}
			if (UStaticMeshComponent* Glow = BeamSurfaces.IsValidIndex(Index) ? BeamSurfaces[Index].Get() : nullptr)
			{
				Glow->SetCustomPrimitiveDataVector4(0, FVector4(1.0, 1.0, 1.0, 0.0));
			}
			if (Runtime && Runtime->Applied.IsValidIndex(Index))
			{
				Runtime->Applied[Index] = FRuntime::FApplied();
			}
		}
	}
}

void ALvFixtureActor::SetDmxOverride(TConstArrayView<uint8> Slots)
{
	DefaultLookLevel = -1.0f;
	if (Runtime)
	{
		Runtime->Override = TArray<uint8>(Slots.GetData(), Slots.Num());
	}
}

void ALvFixtureActor::SetDefaultLook(float DimmerLevel)
{
	DefaultLookLevel = DimmerLevel;
	if (!Runtime)
	{
		return;
	}
	std::vector<uint8_t> Frame = Runtime->Model->evaluator().default_frame();
	Runtime->Model->set_dimmers(Frame, DimmerLevel);
	Runtime->Override = TArray<uint8>(Frame.data(), static_cast<int32>(Frame.size()));
}

void ALvFixtureActor::SetBreakPatch(int32 Break, int32 InUniverse, int32 InAddress)
{
	if (Break == 1)
	{
		Universe = FMath::Clamp(InUniverse, 0, 32767);
		Address = FMath::Clamp(InAddress, 1, 512);
	}
	else if (Break > 1 && InUniverse > 0)
	{
		BreakPatch.Add(Break, FIntPoint(FMath::Clamp(InUniverse, 1, 32767), FMath::Clamp(InAddress, 1, 512)));
	}
	else
	{
		BreakPatch.Remove(Break);
	}
}

bool ALvFixtureActor::GetBreakPatch(int32 Break, int32& OutUniverse, int32& OutAddress) const
{
	if (Break == 1)
	{
		OutUniverse = Universe;
		OutAddress = Address;
		return true;
	}
	if (const FIntPoint* Patch = BreakPatch.Find(Break))
	{
		OutUniverse = Patch->X;
		OutAddress = Patch->Y;
		return true;
	}
	return false;
}

void ALvFixtureActor::SetBreakOverride(int32 Break, TConstArrayView<uint8> Slots)
{
	if (Break == 1)
	{
		SetDmxOverride(Slots);
	}
	else if (Runtime)
	{
		if (Slots.Num() > 0)
		{
			Runtime->BreakOverride.Add(Break, TArray<uint8>(Slots.GetData(), Slots.Num()));
		}
		else
		{
			Runtime->BreakOverride.Remove(Break);
		}
	}
}

void ALvFixtureActor::SetLookSlot(int32 Slot, uint8 Value)
{
	if (Runtime && Runtime->Override.IsValidIndex(Slot - 1))
	{
		Runtime->Override[Slot - 1] = Value;
	}
}

void ALvFixtureActor::SetCastShadows(bool bInCastShadows)
{
	bCastShadows = bInCastShadows;
	for (USpotLightComponent* Light : BeamLights)
	{
		if (Light)
		{
			Light->SetCastShadows(bCastShadows);
		}
	}
}

int32 ALvFixtureActor::GetFootprint() const
{
	return Runtime ? static_cast<int32>(Runtime->Model->evaluator().footprint()) : 0;
}

bool ALvFixtureActor::IsReceivingDmx() const
{
	return Runtime && Runtime->bReceiving;
}

const viz::FixtureState* ALvFixtureActor::GetState() const
{
	return Runtime ? &Runtime->State : nullptr;
}

USceneComponent* ALvFixtureActor::GetGeometryComponent(const FString& GeometryName) const
{
	if (!Runtime)
	{
		return nullptr;
	}
	const std::string Wanted = LvToStd(GeometryName);
	const std::vector<viz::GeometryInstance>& Tree = Runtime->Model->geometry();
	for (int32 Index = 0; Index < static_cast<int32>(Tree.size()); ++Index)
	{
		if (Tree[Index].name == Wanted)
		{
			return GeometryComponents[Index];
		}
	}
	return nullptr;
}

USceneComponent* ALvFixtureActor::GetCarrier(const FString& ChildPosition) const
{
	if (!Runtime || !Runtime->Model)
	{
		return nullptr;
	}
	// An MVR links to a geometry by its path from the top, "Base.Hook": the last name is the geometry.
	FString Named = ChildPosition;
	int32 Dot = INDEX_NONE;
	if (Named.FindLastChar(TEXT('.'), Dot))
	{
		Named.RightChopInline(Dot + 1);
	}
	if (USceneComponent* Linked = Named.IsEmpty() ? nullptr : GetGeometryComponent(Named))
	{
		return Linked;
	}
	const int32 Carrier = Runtime->Model->carrier();
	return GeometryComponents.IsValidIndex(Carrier) ? GeometryComponents[Carrier].Get() : nullptr;
}

USpotLightComponent* ALvFixtureActor::GetBeamLight(const FString& GeometryName) const
{
	return Cast<USpotLightComponent>(GetGeometryComponent(GeometryName));
}

UStaticMeshComponent* ALvFixtureActor::GetBeamCone(const FString& GeometryName) const
{
	const USpotLightComponent* Light = GetBeamLight(GeometryName);
	for (int32 Index = 0; Light && Index < BeamLights.Num(); ++Index)
	{
		if (BeamLights[Index] == Light)
		{
			return BeamCones.IsValidIndex(Index) ? BeamCones[Index].Get() : nullptr;
		}
	}
	return nullptr;
}

bool ALvFixtureActor::HasFogControl() const
{
	return Runtime && Runtime->Model && Runtime->Model->has_fog();
}

float ALvFixtureActor::GetFogOutput() const
{
	return Runtime ? static_cast<float>(Runtime->State.fog) : 0.0f;
}

bool ALvFixtureActor::GetAxisAngle(const FString& GeometryName, double& OutDegrees) const
{
	if (!Runtime)
	{
		return false;
	}
	const std::string Wanted = LvToStd(GeometryName);
	const std::vector<viz::GeometryInstance>& Tree = Runtime->Model->geometry();
	for (int32 Index = 0; Index < Runtime->Motors.Num(); ++Index)
	{
		if (Tree[static_cast<size_t>(Runtime->State.axes[Index].geometry)].name == Wanted)
		{
			OutDegrees = Runtime->Motors[Index].position();
			return true;
		}
	}
	return false;
}

void ALvFixtureActor::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	if (!Runtime || !Runtime->Model)
	{
		return;
	}
	FRuntime& R = *Runtime;

	// 1. This fixture's slice of DMX.
	const uint8* Data = nullptr;
	int32 Count = 0;
	if (R.Override.Num() > 0)
	{
		Data = R.Override.GetData();
		Count = R.Override.Num();
	}
	else if (ULvDmxSubsystem* Dmx = GetWorld() ? GetWorld()->GetSubsystem<ULvDmxSubsystem>() : nullptr)
	{
		if (const uint8* Slots = Dmx->GetUniverse(Universe))
		{
			Data = Slots + (Address - 1);
			Count = 512 - (Address - 1);
		}
	}
	R.bReceiving = Data != nullptr;

	// 2. DMX to physical values to what each axis and beam should do.
	if (BreakPatch.Num() == 0 && R.BreakOverride.Num() == 0)
	{
		R.Model->evaluator().evaluate(Data, static_cast<size_t>(Count), R.Values);
	}
	else
	{
		// Each further break comes from its own place: its override if it has one, else its patch.
		R.BreakData.clear();
		R.BreakData.push_back({1, Data, static_cast<size_t>(FMath::Max(Count, 0))});
		ULvDmxSubsystem* Dmx = GetWorld() ? GetWorld()->GetSubsystem<ULvDmxSubsystem>() : nullptr;
		for (const TPair<int32, TArray<uint8>>& Fed : R.BreakOverride)
		{
			R.BreakData.push_back({Fed.Key, Fed.Value.GetData(), static_cast<size_t>(Fed.Value.Num())});
		}
		for (const TPair<int32, FIntPoint>& Patch : BreakPatch)
		{
			if (R.BreakOverride.Contains(Patch.Key))
			{
				continue;
			}
			if (const uint8* Slots = Dmx ? Dmx->GetUniverse(Patch.Value.X) : nullptr)
			{
				R.BreakData.push_back({Patch.Key, Slots + (Patch.Value.Y - 1), static_cast<size_t>(512 - (Patch.Value.Y - 1))});
			}
		}
		R.Model->evaluator().evaluate(R.BreakData, R.Values);
	}
	R.Model->resolve(R.Values, R.State, static_cast<viz::DimmerCurve>(TypeDimmerCurve));

	// 3. Axes: the motor chases the target. Several on one geometry combine: the part is turned
	// about its own origin, then slid along its own axes as they stand at rest. Pan and tilt turn;
	// the XYZ and Rot channels of a winch, a hoist or a turntable slide and turn likewise.
	struct FMove
	{
		int32 Geometry;
		FQuat Turn;
		FVector Slide;
	};
	TArray<FMove, TInlineAllocator<4>> Moves;
	for (int32 Index = 0; Index < R.Motors.Num(); ++Index)
	{
		const viz::AxisState& Axis = R.State.axes[Index];
		double Target = Axis.target;
		// Mirrored about the middle of its range by the fixture's own inversion or by its type's
		// (both together are no mirroring), then turned by the type's offset.
		if ((Axis.motion == viz::Motion::Pan && bInvertPan != bTypeInvertPan) || (Axis.motion == viz::Motion::Tilt && bInvertTilt != bTypeInvertTilt))
		{
			Target = 2.0 * Axis.center - Target;
		}
		if (Axis.motion == viz::Motion::Pan)
		{
			Target += PanOffset;
		}
		else if (Axis.motion == viz::Motion::Tilt)
		{
			Target += TiltOffset;
		}
		viz::MotorAxis& Motor = R.Motors[Index];
		Motor.configure(Axis.range, Axis.real_fade, Axis.real_acceleration);
		if (!R.MotorStarted[Index])
		{
			Motor.reset(Target);
			R.MotorStarted[Index] = true;
		}
		const FTransform Step = LvCoordinates::FromGdtf(viz::axis_matrix(Axis, Motor.step(Target, DeltaSeconds)));
		FMove* Move = Moves.FindByPredicate([&Axis](const FMove& Entry) { return Entry.Geometry == Axis.geometry; });
		if (!Move)
		{
			Move = &Moves.Add_GetRef(FMove{Axis.geometry, FQuat::Identity, FVector::ZeroVector});
		}
		Move->Turn = Step.GetRotation() * Move->Turn;
		Move->Slide += Step.GetTranslation();
	}
	for (const FMove& Move : Moves)
	{
		// Moved in the geometry's own frame, then placed where the file says.
		GeometryComponents[Move.Geometry]->SetRelativeTransform(FTransform(Move.Turn, Move.Slide) * R.Rest[Move.Geometry]);
	}

	// 4. Beams.
	for (int32 Index = 0; Index < BeamLights.Num(); ++Index)
	{
		const viz::BeamState& Beam = R.State.beams[Index];
		const viz::BeamProperties& Properties = *R.Model->geometry()[static_cast<size_t>(Beam.geometry)].geometry->beam;

		// A strobe flashes the way its kind does: on and off, a short pulse, a ramp, or at random.
		double Gate = 1.0;
		if (Beam.strobe_hz > 0.0)
		{
			const double Next = R.StrobePhase[Index] + Beam.strobe_hz * DeltaSeconds;
			if (Next >= 1.0)
			{
				R.StrobeChance[Index] = FMath::FRand();    // a new cycle: the random kinds decide again
			}
			R.StrobePhase[Index] = FMath::Fmod(Next, 1.0);
			Gate = viz::strobe_level(Beam.strobe_shape, R.StrobePhase[Index], R.StrobeChance[Index]);
		}

		// A colour wheel that is spinning, or held between two colours, tints the beam as it turns.
		viz::ColorRGB Tint = Beam.color;
		R.Clear[Index] = FLinearColor(static_cast<float>(Tint.r), static_cast<float>(Tint.g), static_cast<float>(Tint.b));
		if (Beam.wheel)
		{
			R.WheelTurns[Index] = FMath::Fmod(R.WheelTurns[Index] + Beam.wheel_degrees_per_second * DeltaSeconds / 360.0, 1.0);
			const viz::ColorRGB Glass = viz::wheel_color_at(*Beam.wheel, Beam.wheel_degrees / 360.0 + R.WheelTurns[Index]);
			Tint.r *= Glass.r;
			Tint.g *= Glass.g;
			Tint.b *= Glass.b;
		}

		// The lens or cell glows: its light spread evenly over its own area, in nits. A spot's lens
		// is really a thousand times brighter than an LED wall; it is held to a level that blooms
		// without washing out the picture.
		if (UStaticMeshComponent* Glow = BeamSurfaces.IsValidIndex(Index) ? BeamSurfaces[Index].Get() : nullptr)
		{
			const FVector Size = Glow->GetRelativeScale3D();
			const double Area = FMath::Max(Size.Y * Size.Z * 1e-4, 1e-5);    // square metres; near enough for a disc too
			const double Nits = FMath::Min(Properties.luminous_flux * Beam.intensity * IntensityScale / (UE_DOUBLE_PI * Area), 40000.0) * Gate;
			const FVector4f GlowData(static_cast<float>(Tint.r), static_cast<float>(Tint.g), static_cast<float>(Tint.b), static_cast<float>(Nits));
			if (!R.Applied[Index].Glow.Equals(GlowData, 1e-3f))
			{
				Glow->SetCustomPrimitiveDataVector4(0, FVector4(GlowData));
				R.Applied[Index].Glow = GlowData;
			}
		}

		// Luminous flux spread over the field angle's cone gives candela. Some files state a
		// field angle smaller than the beam angle; the wider of the two is the cone's edge.
		{
			const double FieldAngle = FMath::Clamp(FMath::Max(Beam.field_angle, Beam.beam_angle), 1.0, 160.0);
			const double SolidAngle = 2.0 * UE_DOUBLE_PI * (1.0 - FMath::Cos(FMath::DegreesToRadians(FieldAngle * 0.5)));
			R.Candela[Index] = static_cast<float>(Properties.luminous_flux / SolidAngle * Beam.intensity * IntensityScale * Gate);
			R.Tint[Index] = FLinearColor(static_cast<float>(Tint.r), static_cast<float>(Tint.g), static_cast<float>(Tint.b));
		}
	}

	// Lenses drawn as one beam: their light is added up in the one that draws for them, and its
	// colour is theirs weighed by how bright each is.
	for (const viz::BeamCluster& Cluster : R.Model->clusters())
	{
		if (!R.Candela.IsValidIndex(Cluster.leader))
		{
			continue;
		}
		float Sum = 0.0f;
		FLinearColor Mixed(0.0f, 0.0f, 0.0f, 1.0f);
		for (int32 Member : Cluster.members)
		{
			const float Share = R.Candela[Member];
			Sum += Share;
			Mixed.R += R.Tint[Member].R * Share;
			Mixed.G += R.Tint[Member].G * Share;
			Mixed.B += R.Tint[Member].B * Share;
		}
		if (Sum > 0.0f)
		{
			R.Tint[Cluster.leader] = FLinearColor(Mixed.R / Sum, Mixed.G / Sum, Mixed.B / Sum);
		}
		R.Candela[Cluster.leader] = Sum;
	}

	LitBeams = 0;
	ShadedBeams = 0;
	for (int32 Index = 0; Index < BeamLights.Num(); ++Index)
	{
		USpotLightComponent* Light = BeamLights[Index];
		if (!Light)
		{
			continue;
		}
		const viz::BeamState& Beam = R.State.beams[Index];
		const viz::BeamProperties& Properties = *R.Model->geometry()[static_cast<size_t>(Beam.geometry)].geometry->beam;
		const double FieldAngle = FMath::Clamp(FMath::Max(Beam.field_angle, Beam.beam_angle), 1.0, 160.0);
		const double BeamAngle = FMath::Clamp(FMath::Min(Beam.field_angle, Beam.beam_angle), 0.5, FieldAngle);
		const float Intensity = R.Candela[Index];
		FLinearColor Color = R.Tint[Index];     // the light's
		FLinearColor ConeColor = Color;         // the beam's in haze
		float Outer = static_cast<float>(FieldAngle * 0.5);
		float Inner = static_cast<float>(BeamAngle * 0.5);

		// What is in the beam's path: gobos, prism, iris, frost, blades. The materials for it are
		// asked for when it changes and arrive later this frame; the light is turned here.
		{
			FRuntime::FGate& Shaping = R.Gates[Index];
			FLvGateLook Look;
			double Roll = 0.0;
			double Spread = 1.0;
			const double CoreShare = FMath::Tan(FMath::DegreesToRadians(BeamAngle * 0.5)) / FMath::Tan(FMath::DegreesToRadians(FieldAngle * 0.5));
			const bool bShaped = BuildGateLook(Index, DeltaSeconds, FieldAngle, CoreShare, Look, Roll, Spread);
			if (bShaped == Shaping.bOpen || (bShaped && Look != Shaping.Look))
			{
				if (ULvGateSubsystem* Gates = GetWorld() ? GetWorld()->GetSubsystem<ULvGateSubsystem>() : nullptr)
				{
					Gates->Request(this, Index, Look, !bShaped);
					Shaping.bOpen = !bShaped;
					Shaping.Look = Look;
				}
			}
			const bool bDrawn = bShaped && !Shaping.bOpen && !Shaping.bUnavailable;
			const float RollNow = bDrawn ? static_cast<float>(Roll) : 0.0f;
			if (FMath::Abs(RollNow - Shaping.Roll) > 1e-3f)
			{
				// About the beam, which is the light's own X, before the light is put where the file has it.
				Light->SetRelativeTransform(FTransform(FQuat(FVector::XAxisVector, FMath::DegreesToRadians(RollNow))) * R.Rest[Beam.geometry]);
				Shaping.Roll = RollNow;
			}
			if (bDrawn && Shaping.bSplit && R.Clear.IsValidIndex(Index))
			{
				// The gate shows the wheel's two colours, so the beam in haze is as without the wheel. The
				// light on surfaces too, where the engine's light function atlas keeps colour; by default it
				// keeps only how bright, and there the light keeps the two colours mixed and the pattern
				// says how bright each side is.
				static const IConsoleVariable* AtlasFormat = IConsoleManager::Get().FindConsoleVariable(TEXT("r.LightFunctionAtlas.Format"));
				ConeColor = R.Clear[Index];
				if (AtlasFormat && AtlasFormat->GetInt() == 1)
				{
					Color = R.Clear[Index];
				}
			}
			if (bDrawn)
			{
				// The gate material draws the beam's edge itself, over a cone wide enough for all it shows.
				Outer = static_cast<float>(FMath::RadiansToDegrees(FMath::Atan(FMath::Tan(FMath::DegreesToRadians(FieldAngle * 0.5)) * Spread)));
				Inner = Outer;
			}
		}

		FRuntime::FApplied& Applied = R.Applied[Index];
		if (Applied.Intensity != Intensity)
		{
			Light->SetIntensity(Intensity);
			Applied.Intensity = Intensity;
		}
		if (!Applied.Color.Equals(Color, 1e-4f))
		{
			Light->SetLightColor(Color, false);
			Applied.Color = Color;
		}
		if (Applied.Outer != Outer)
		{
			Light->SetOuterConeAngle(Outer);
			Applied.Outer = Outer;
		}
		if (Applied.Inner != Inner)
		{
			Light->SetInnerConeAngle(Inner);
			Applied.Inner = Inner;
		}

		UStaticMeshComponent* Cone = BeamCones.IsValidIndex(Index) ? BeamCones[Index].Get() : nullptr;
		if (!Cone)
		{
			continue;
		}
		const bool bShow = bConeBeams && Intensity > 0.0f;
		if (Cone->IsVisible() != bShow)
		{
			Cone->SetVisibility(bShow);
		}
		if (!bShow)
		{
			continue;
		}
		// The light leaves through a lens, not from a point: the cone's apex sits behind the lens,
		// where the cone is as wide as the lens at the lens.
		// A cluster's beam leaves through all its lenses: as wide as they are together, from their middle.
		const double TanHalf = FMath::Tan(FMath::DegreesToRadians(static_cast<double>(Outer)));
		const double LensRadius = R.Opening[Index] > 0.0f ? static_cast<double>(R.Opening[Index]) : FMath::Max(Properties.beam_radius * 100.0, 1.0);
		const FVector LensAt = R.OpeningAt[Index];
		const double Back = LensRadius / TanHalf;
		// It ends where the beam lands, so that it does not show through walls and floors. The
		// beam is felt along at several places across it: the middle every frame, and the others
		// a few at a time. The cone reaches as far as the furthest of them, and the shader cuts
		// it short wherever one of them stopped sooner: the shadow of what stands in the beam.
		const FVector Lens = Light->GetComponentTransform().TransformPosition(LensAt);
		const FVector Along = Light->GetForwardVector();
		const FVector Apex = Lens - Along * Back;
		const double Furthest = Back + 6000.0;    // as far as the light itself reaches
		// Across the beam, from directions that do not turn when the light is rolled for a gobo. The shader takes the same two.
		const FVector Reference = FMath::Abs(Along.X) > 0.999 ? FVector(0.0, 0.0, 1.0) : FVector(1.0, 0.0, 0.0);
		const FVector AcrossY = FVector::CrossProduct(Reference, Along).GetSafeNormal();
		const FVector AcrossZ = FVector::CrossProduct(Along, AcrossY);
		FCollisionQueryParams Query(SCENE_QUERY_STAT(LvBeamThrow), false, this);
		UWorld* World = GetWorld();
		auto Feel = [&](int32 Place) -> float
		{
			// A step of one along the beam for every step of this: so a distance along it is a distance from the apex along the beam's axis.
			const FVector2D& Across = ShadePlaces()[Place];
			const FVector Direction = Along + (AcrossY * Across.X + AcrossZ * Across.Y) * TanHalf;
			FHitResult Hit;
			if (World->LineTraceSingleByChannel(Hit, Apex + Direction * (Back + 2.0), Apex + Direction * Furthest, ECC_Visibility, Query))
			{
				return static_cast<float>(FVector::DotProduct(Hit.ImpactPoint - Apex, Along) + 4.0);
			}
			return static_cast<float>(Furthest);
		};
		Applied.Reach[0] = Feel(0);
		float Length = Applied.Reach[0];
		// Light that spreads as widely as a strobe's or a blinder's is a glow about the fixture, not a
		// shaft that things stand in: it is not felt along for shadows.
		const bool bFeel = bBeamShadows && Outer < 50.0f;
		if (bFeel)
		{
			// All of them the first time, so that a beam does not come on whole and then lose its shadow; after that six a frame.
			const int32 Felt = Applied.bShadeKnown ? kShadePerFrame : kShadePlaces - 1;
			for (int32 Step = 0; Step < Felt; ++Step)
			{
				const int32 Place = 1 + Applied.ShadeNext;
				Applied.ShadeNext = (Applied.ShadeNext + 1) % (kShadePlaces - 1);
				Applied.Reach[Place] = Feel(Place);
			}
			Applied.bShadeKnown = true;
			for (int32 Place = 1; Place < kShadePlaces; ++Place)
			{
				Length = FMath::Max(Length, Applied.Reach[Place]);
			}
		}
		else
		{
			Applied.bShadeKnown = false;
		}
		// Where all that a beam lands on is one flat thing (the floor, a wall, a backdrop), nothing
		// stands in it, and where it ends is already seen: the surface hides what lies behind it.
		// Only a beam with something in its way needs its shadow worked out, which is the dear
		// part of drawing it. Flat is told from the reaches themselves: along a plane, one over
		// the reach changes evenly across the beam. Places where the beam lands on nothing say
		// nothing either way; one or two hits alone might be a truss chord, and count as in the way.
		bool bShade = bFeel;
		if (bFeel)
		{
			const FVector2D* Places = ShadePlaces();
			double M[3][3] = {{0.0, 0.0, 0.0}, {0.0, 0.0, 0.0}, {0.0, 0.0, 0.0}};
			double V[3] = {0.0, 0.0, 0.0};
			int32 Hits = 0;
			for (int32 Place = 0; Place < kShadePlaces; ++Place)
			{
				if (Applied.Reach[Place] >= Furthest - 1.0)
				{
					continue;
				}
				const double Row[3] = {1.0, Places[Place].X, Places[Place].Y};
				const double Inverse = 1.0 / FMath::Max(static_cast<double>(Applied.Reach[Place]), 1.0);
				for (int32 I = 0; I < 3; ++I)
				{
					V[I] += Row[I] * Inverse;
					for (int32 J = 0; J < 3; ++J)
					{
						M[I][J] += Row[I] * Row[J];
					}
				}
				++Hits;
			}
			bool bFlat = Hits == 0;
			if (Hits >= 3)
			{
				// One over the reach as (a + b x + c y), fitted to the hits by least squares.
				const double Det = M[0][0] * (M[1][1] * M[2][2] - M[1][2] * M[2][1]) - M[0][1] * (M[1][0] * M[2][2] - M[1][2] * M[2][0]) +
					M[0][2] * (M[1][0] * M[2][1] - M[1][1] * M[2][0]);
				if (FMath::Abs(Det) > 1e-6)    // else the hits lie in a line across the beam: a plane cannot be told from them
				{
					const double A = (V[0] * (M[1][1] * M[2][2] - M[1][2] * M[2][1]) - M[0][1] * (V[1] * M[2][2] - M[1][2] * V[2]) +
						M[0][2] * (V[1] * M[2][1] - M[1][1] * V[2])) / Det;
					const double B = (M[0][0] * (V[1] * M[2][2] - M[1][2] * V[2]) - V[0] * (M[1][0] * M[2][2] - M[1][2] * M[2][0]) +
						M[0][2] * (M[1][0] * V[2] - V[1] * M[2][0])) / Det;
					const double C = (M[0][0] * (M[1][1] * V[2] - V[1] * M[2][1]) - M[0][1] * (M[1][0] * V[2] - V[1] * M[2][0]) +
						V[0] * (M[1][0] * M[2][1] - M[1][1] * M[2][0])) / Det;
					bFlat = true;
					for (int32 Place = 0; Place < kShadePlaces && bFlat; ++Place)
					{
						const double Reach = Applied.Reach[Place];
						if (Reach >= Furthest - 1.0)
						{
							continue;
						}
						const double Fitted = 1.0 / FMath::Max(A + B * Places[Place].X + C * Places[Place].Y, 1e-7);
						bFlat = FMath::Abs(Reach - Fitted) <= FMath::Max(6.0, 0.02 * Reach);
					}
				}
			}
			bShade = !bFlat;
		}
		++LitBeams;
		ShadedBeams += bShade ? 1 : 0;
		// The plain beam material has no shadow path, which makes it the cheaper: a cone wears the
		// one that has only while something stands in it. A gate's material has its own.
		Applied.bWantsShadow = bShade;
		const bool bGated = R.Gates.IsValidIndex(Index) && R.Gates[Index].bGated;
		if (!bGated && Applied.bShadowMaterial != bShade)
		{
			UMaterialInterface* Shadow = nullptr;
			if (bShade)
			{
				ULvFixtureLibrary* Library = ULvFixtureLibrary::Get();
				Shadow = Library ? Library->GetShadowBeam() : nullptr;
			}
			if (!bShade || Shadow)
			{
				Cone->SetMaterial(0, Shadow);    // none: the plain beam the cone's mesh carries
				Applied.bShadowMaterial = bShade;
			}
		}
		const float Radius = static_cast<float>(Length * TanHalf);
		if (FMath::Abs(Applied.ConeLength - Length) > 0.5f || FMath::Abs(Applied.ConeRadius - Radius) > 0.05f)
		{
			Cone->SetRelativeLocation(LensAt + FVector(-Back, 0.0, 0.0));
			Cone->SetRelativeScale3D(FVector(Length, Radius, Radius));
			Applied.ConeLength = Length;
			Applied.ConeRadius = Radius;
		}
		// Each place's reach as a share of the cone's length, and in the last number whether the shader is to look at them at all.
		float Shares[20];
		for (int32 Place = 0; Place < kShadePlaces; ++Place)
		{
			Shares[Place] = bShade ? FMath::Clamp(Applied.Reach[Place] / FMath::Max(Length, 1.0f), 0.0f, 1.0f) : 1.0f;
		}
		Shares[19] = bShade ? 1.0f : 0.0f;
		for (int32 Group = 0; Group < 5; ++Group)
		{
			const FVector4f Told(Shares[Group * 4], Shares[Group * 4 + 1], Shares[Group * 4 + 2], Shares[Group * 4 + 3]);
			if (!Applied.Shade[Group].Equals(Told, 5e-4f))
			{
				Cone->SetCustomPrimitiveDataVector4(8 + Group * 4, FVector4(Told));
				Applied.Shade[Group] = Told;
			}
		}
		const FVector4f LightData(ConeColor.R, ConeColor.G, ConeColor.B, Intensity);
		if (!Applied.ConeLight.Equals(LightData, 1e-4f))
		{
			Cone->SetCustomPrimitiveDataVector4(0, FVector4(LightData));
			Applied.ConeLight = LightData;
		}
		// Where the fade to the edge starts, as a share of the cone's width.
		const float Core = static_cast<float>(FMath::Tan(FMath::DegreesToRadians(static_cast<double>(Inner))) / TanHalf);
		// With it: how much haze the air holds, and how unevenly it hangs there.
		const FVector4f ShapeData(static_cast<float>(Back), FMath::Clamp(Core, 0.0f, 0.98f), ConeHaze, HazeDrift);
		if (!Applied.ConeShape.Equals(ShapeData, 1e-5f))
		{
			Cone->SetCustomPrimitiveDataVector4(4, FVector4(ShapeData));
			Applied.ConeShape = ShapeData;
		}
	}
}

void ALvFixtureActor::SetBeamStyle(bool bCones, float Haze)
{
	bConeBeams = bCones;
	ConeHaze = FMath::Max(Haze, 0.0f);
	for (int32 Index = 0; Index < BeamLights.Num(); ++Index)
	{
		USpotLightComponent* Light = BeamLights[Index];
		if (!Light)
		{
			continue;
		}
		// A beam drawn on a cone must not also light the fog, or it would show twice.
		const bool bOnCone = bConeBeams && BeamCones.IsValidIndex(Index) && BeamCones[Index];
		const bool bGlow = Runtime && Runtime->Model && Runtime->Model->geometry()[static_cast<size_t>(Runtime->State.beams[Index].geometry)].geometry->beam->beam_type == viz::BeamType::Glow;
		Light->SetVolumetricScatteringIntensity(bOnCone || bGlow ? 0.0f : HazeScattering);
		if (!bConeBeams && BeamCones.IsValidIndex(Index) && BeamCones[Index])
		{
			BeamCones[Index]->SetVisibility(false);
		}
	}
}

int32 ALvFixtureActor::TakeGateHandle(int32 Beam)
{
	if (!Runtime || !Runtime->Gates.IsValidIndex(Beam))
	{
		return INDEX_NONE;
	}
	const int32 Handle = Runtime->Gates[Beam].Handle;
	Runtime->Gates[Beam].Handle = INDEX_NONE;
	return Handle;
}

bool ALvFixtureActor::ApplyGate(int32 Beam, int32 Handle, UMaterialInterface* LightFunction, UMaterialInterface* BeamMaterial)
{
	if (!Runtime || !Runtime->Gates.IsValidIndex(Beam) || !BeamLights.IsValidIndex(Beam))
	{
		return false;
	}
	FRuntime::FGate& Shaping = Runtime->Gates[Beam];
	Shaping.Handle = Handle;
	Shaping.bUnavailable = !Shaping.bOpen && !LightFunction;
	if (USpotLightComponent* Light = BeamLights[Beam])
	{
		if (Light->LightFunctionMaterial != LightFunction)
		{
			Light->SetLightFunctionMaterial(LightFunction);
		}
	}
	Shaping.bGated = BeamMaterial != nullptr;
	if (UStaticMeshComponent* Cone = BeamCones.IsValidIndex(Beam) ? BeamCones[Beam].Get() : nullptr)
	{
		// None: back to the plain beam the cone's mesh carries, or the one that shades it if something stands in it.
		UMaterialInterface* Wear = BeamMaterial;
		bool bShadowMaterial = false;
		if (!Wear && Runtime->Applied.IsValidIndex(Beam) && Runtime->Applied[Beam].bWantsShadow)
		{
			ULvFixtureLibrary* Library = ULvFixtureLibrary::Get();
			Wear = Library ? Library->GetShadowBeam() : nullptr;
			bShadowMaterial = Wear != nullptr;
		}
		Cone->SetMaterial(0, Wear);
		if (Runtime->Applied.IsValidIndex(Beam))
		{
			Runtime->Applied[Beam].bShadowMaterial = bShadowMaterial;
		}
	}
	return true;
}

void ALvFixtureActor::ReleaseGates()
{
	++GateSerial;
	if (!Runtime)
	{
		return;
	}
	ULvGateSubsystem* Gates = GetWorld() ? GetWorld()->GetSubsystem<ULvGateSubsystem>() : nullptr;
	for (FRuntime::FGate& Shaping : Runtime->Gates)
	{
		if (Gates)
		{
			Gates->Release(Shaping.Handle);
		}
		Shaping.Handle = INDEX_NONE;
		Shaping.bOpen = true;
	}
}

void ALvFixtureActor::Destroyed()
{
	ReleaseGates();
	Super::Destroyed();
}

void ALvFixtureActor::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	ReleaseGates();
	Super::EndPlay(EndPlayReason);
}

bool ALvFixtureActor::BuildGateLook(int32 Index, float DeltaSeconds, double FieldAngle, double CoreShare, FLvGateLook& Look, double& OutRoll, double& OutSpread)
{
	FRuntime& R = *Runtime;
	const viz::BeamState& Beam = R.State.beams[Index];
	FRuntime::FGate& Shaping = R.Gates[Index];
	ULvFixtureLibrary* Library = ULvFixtureLibrary::Get();
	OutRoll = 0.0;
	OutSpread = 1.0;

	// Whatever turns goes on turning, whether or not anything is in the beam just now.
	struct FLayer
	{
		UTexture2D* Picture = nullptr;
		double Angle = 0.0;      // degrees the picture is turned
		double Lateral = 0.0;    // how far it sits off the beam's centre, in beam radii: shaking, or passing through
		double Radius = 0.35;    // an animation wheel: the beam's size on the wheel, the wheel's own radius being 1
	};
	FLayer Layers[viz::kGateLayers + 1];    // the wheels' pictures, then a colour wheel's two colours
	for (int32 L = 0; L < viz::kGateLayers; ++L)
	{
		const viz::GateLayer& In = Beam.layers[L];
		Shaping.Spin[L] = In.degrees_per_second != 0.0 ? FMath::Fmod(Shaping.Spin[L] + In.degrees_per_second * DeltaSeconds, 360.0) : 0.0;
		Shaping.Shake[L] = In.shake_hz > 0.0 ? FMath::Fmod(Shaping.Shake[L] + In.shake_hz * DeltaSeconds, 1.0) : 0.0;
		int32 Slot = In.slot;
		if (In.wheel && Slot == 0)
		{
			// The wheel itself is turning or held between slots: whichever slot is nearest shows, off-centre.
			Shaping.WheelTurns[L] = FMath::Fmod(Shaping.WheelTurns[L] + In.wheel_degrees_per_second * DeltaSeconds / 360.0, 1.0);
			const viz::WheelPlace Place = viz::wheel_place_at(*In.wheel, In.wheel_degrees / 360.0 + Shaping.WheelTurns[L]);
			Slot = Place.slot;
			Layers[L].Lateral = Place.offset * 2.4;    // neighbouring slots sit a little more than their own width apart
		}
		else
		{
			Shaping.WheelTurns[L] = 0.0;
		}
		if (!In.wheel || Slot < 1 || static_cast<size_t>(Slot) > In.wheel->slots.size())
		{
			continue;
		}
		UTexture2D* Picture = R.Definition->GetPicture(In.wheel, Slot);
		if (!Picture && In.slot == 0 && Library)
		{
			Picture = Library->GetOpenHole();    // an empty slot going past is still a hole with dark wheel either side
		}
		if (!Picture)
		{
			continue;
		}
		Layers[L].Picture = Picture;
		Layers[L].Angle = In.degrees + Shaping.Spin[L];
		if (In.shake_hz > 0.0)
		{
			Layers[L].Lateral += 0.12 * FMath::Sin(2.0 * UE_DOUBLE_PI * Shaping.Shake[L]);
		}
		const viz::WheelSlot& Described = In.wheel->slots[static_cast<size_t>(Slot - 1)];
		if (Described.animation && Described.animation->radius > 0.0)
		{
			Layers[L].Radius = FMath::Clamp(Described.animation->radius, 0.1, 0.9);
		}
	}

	const viz::PrismState* Prism = nullptr;
	double PrismAngle = 0.0;
	const viz::PrismState* Second = nullptr;    // a second prism in at the same time: copies of the first's copies
	double SecondAngle = 0.0;
	for (int32 P = 0; P < viz::kPrisms; ++P)
	{
		const viz::PrismState& In = Beam.prisms[P];
		Shaping.PrismSpin[P] = In.degrees_per_second != 0.0 ? FMath::Fmod(Shaping.PrismSpin[P] + In.degrees_per_second * DeltaSeconds, 360.0) : 0.0;
		if (!In.slot)
		{
			continue;
		}
		if (!Prism)
		{
			Prism = &In;
			PrismAngle = In.degrees + Shaping.PrismSpin[P];
		}
		else if (!Second)
		{
			Second = &In;
			SecondAngle = In.degrees + Shaping.PrismSpin[P];
		}
	}

	double Iris = Beam.iris;
	if (Beam.iris_hz > 0.0)
	{
		const double Next = Shaping.IrisPhase + Beam.iris_hz * DeltaSeconds;
		if (Next >= 1.0)
		{
			Shaping.IrisChance = FMath::FRand();
		}
		Shaping.IrisPhase = FMath::Fmod(Next, 1.0);
		Iris = FMath::Lerp(Beam.iris, 1.0, viz::strobe_level(Beam.iris_shape, Shaping.IrisPhase, Shaping.IrisChance));
	}
	else
	{
		Shaping.IrisPhase = 0.0;
	}
	const bool bIris = Iris < 0.999;

	viz::BladeEdge Edges[4];
	bool bBlades = false;
	for (int32 Blade = 0; Blade < 4; ++Blade)
	{
		Edges[Blade] = viz::blade_edge(Blade, Beam.blades[Blade], Beam.shaper_degrees);
		bBlades = bBlades || Edges[Blade].distance < 1.5;
	}

	// How far the pictures and edges are smeared, in beam radii. Frost does it more than a soft focus.
	const double Blur = FMath::Max(Beam.frost * 0.45, Beam.defocus * 0.2);

	// The materials take two pictures: the first two that are in, gobo wheels before the animation wheel.
	int32 Active[2] = {0, 0};
	int32 ActiveCount = 0;
	for (int32 L = 0; L < viz::kGateLayers && ActiveCount < 2; ++L)
	{
		if (Layers[L].Picture)
		{
			Active[ActiveCount++] = L;
		}
	}

	// A colour wheel standing between two slots, or turning, shows both colours side by side, the line
	// between them crossing the beam as the wheel turns: the "split colour" a fixture's wheel gives.
	// Slots move across the beam from its right; neighbouring ones sit 2.4 beam radii apart, as gobos do.
	Shaping.bSplit = false;
	const int32 SplitLayer = viz::kGateLayers;
	if (Beam.wheel && Beam.wheel->slots.size() >= 2 && ActiveCount < 2 && Library && R.Model->cluster_of(static_cast<size_t>(Index)) < 0 &&
		R.WheelTurns.IsValidIndex(Index))
	{
		const viz::Wheel& Wheel = *Beam.wheel;
		const double Turns = Beam.wheel_degrees / 360.0 + R.WheelTurns[Index];
		const viz::WheelPlace Place = viz::wheel_place_at(Wheel, Turns);
		const double Edge = Place.offset >= 0.0 ? (0.5 - Place.offset) * 2.4 : -(0.5 + Place.offset) * 2.4;
		if (FMath::Abs(Edge) < 1.0)
		{
			const int32 Count = static_cast<int32>(Wheel.slots.size());
			const int32 Left = Place.offset >= 0.0 ? Place.slot : (Place.slot + Count - 2) % Count + 1;
			const int32 Right = Place.offset >= 0.0 ? Place.slot % Count + 1 : Place.slot;
			// Each colour as the beam has it with that slot in the middle.
			auto SlotColour = [&Wheel, Count](int32 Slot)
			{
				const viz::ColorRGB Glass = viz::wheel_color_at(Wheel, static_cast<double>(Slot - 1) / Count);
				return FLinearColor(static_cast<float>(Glass.r), static_cast<float>(Glass.g), static_cast<float>(Glass.b));
			};
			Layers[SplitLayer].Picture = Library->GetSplitColours(SlotColour(Left), SlotColour(Right));
			Layers[SplitLayer].Angle = 0.0;
			Layers[SplitLayer].Lateral = Edge;
			if (Layers[SplitLayer].Picture)
			{
				Active[ActiveCount++] = SplitLayer;
				Shaping.bSplit = true;
			}
		}
	}
	if (ActiveCount == 0 && !Prism && !bIris && !bBlades && Blur < 0.002)
	{
		return false;
	}

	// The light is rolled with the first gobo, so a gobo that only turns never changes the look;
	// failing a gobo, with the prism. Everything else is then placed relative to that.
	const int32 AnimationLayer = viz::kGateLayers - 1;
	if (ActiveCount > 0 && Active[0] != AnimationLayer)
	{
		OutRoll = Layers[Active[0]].Angle;
	}
	else if (Prism)
	{
		OutRoll = PrismAngle;
	}
	OutRoll = FMath::Fmod(OutRoll, 360.0);
	// Angles are rounded to a quarter of a degree, so that fixtures doing the same thing share a look.
	auto Quarter = [](double Degrees) { return FMath::DegreesToRadians(FMath::RoundToDouble(Degrees * 4.0) / 4.0); };

	for (int32 K = 0; K < ActiveCount; ++K)
	{
		const FLayer& Shown = Layers[Active[K]];
		Look.Texture[K] = Shown.Picture;
		// A place p in the rolled beam is Rot(roll) p in the fixture's own frame, and the picture is turned by its angle.
		const double Delta = Quarter(OutRoll - Shown.Angle);
		const double C = FMath::Cos(Delta);
		const double S = FMath::Sin(Delta);
		if (Active[K] != AnimationLayer)
		{
			// A gobo fills the beam; shaking or passing through moves it sideways in the fixture's frame.
			const double Sideways = FMath::RoundToDouble(Shown.Lateral * 256.0) / 256.0;
			const double Turned = Sideways != 0.0 ? Quarter(Shown.Angle) : 0.0;
			Look.Set(FLvGateLook::Layer0A + K * 2, C, -S, S, C);
			Look.Set(FLvGateLook::Layer0B + K * 2, -Sideways * FMath::Cos(Turned), Sideways * FMath::Sin(Turned), 1.0, 0.0);
		}
		else
		{
			// An animation wheel is far larger than the beam, which passes through it near its rim.
			const double Turned = Quarter(Shown.Angle);
			const double Out = 1.0 - Shown.Radius;
			Look.Set(FLvGateLook::Layer0A + K * 2, Shown.Radius * C, -Shown.Radius * S, Shown.Radius * S, Shown.Radius * C);
			Look.Set(FLvGateLook::Layer0B + K * 2, FMath::Sin(Turned) * Out, FMath::Cos(Turned) * Out, 1.0, 0.0);
		}
	}

	// Frost spreads the beam as well as softening it. A frost filter scatters what passes it into a cone
	// of its own, which adds to the beam's angle as two blurs add (in quadrature): a narrow beam becomes
	// a soft wash of about that cone, a wide one changes little. The gate's pictures, iris and blades
	// are spread with it (the materials divide by the widening), and the light is spread as thinly.
	const double TanOwn = FMath::Tan(FMath::DegreesToRadians(FieldAngle * 0.5));
	const double TanFrost = FMath::Tan(FMath::DegreesToRadians(kFrostDegrees * 0.5 * Beam.frost));
	const double Widen = FMath::RoundToDouble(FMath::Sqrt(TanOwn * TanOwn + TanFrost * TanFrost) / TanOwn * 64.0) / 64.0;
	double Spread = Widen;
	int32 Facets = 0;
	if (Prism && Second)
	{
		// Two prisms: each copy of the first is copied again by the second, each prism turned by its own
		// angle. Their places are worked out here, turned as they stand, and the materials told not to turn them.
		const std::vector<viz::PrismImage> First = viz::prism_images(*Prism->slot);
		const std::vector<viz::PrismImage> Then = viz::prism_images(*Second->slot);
		const double A = Quarter(PrismAngle - OutRoll), B = Quarter(SecondAngle - OutRoll);
		const int32 Total = static_cast<int32>(First.size() * Then.size());
		const int32 Step = FMath::Max(1, FMath::DivideAndRoundUp(Total, 16));
		double Reach = 1.0;
		for (int32 Image = 0; Image < Total && Facets < 16; Image += Step)
		{
			const viz::PrismImage& One = First[static_cast<size_t>(Image) / Then.size()];
			const viz::PrismImage& Two = Then[static_cast<size_t>(Image) % Then.size()];
			const double X = FMath::Cos(A) * One.x - FMath::Sin(A) * One.y + FMath::Cos(B) * Two.x - FMath::Sin(B) * Two.y;
			const double Y = FMath::Sin(A) * One.x + FMath::Cos(A) * One.y + FMath::Sin(B) * Two.x + FMath::Cos(B) * Two.y;
			const double ScaleX = One.scale_x * Two.scale_x, ScaleY = One.scale_y * Two.scale_y;
			Look.Set(FLvGateLook::Facet0 + Facets++, FMath::RoundToDouble(X * 64.0) / 64.0, FMath::RoundToDouble(Y * 64.0) / 64.0, 1.0 / ScaleX, 1.0 / ScaleY);
			Reach = FMath::Max(Reach, FMath::Sqrt(X * X + Y * Y) + FMath::Max(ScaleX, ScaleY) * Widen);
		}
		Spread = Reach;
		Look.Set(FLvGateLook::Turn, 1.0, 0.0, 0.0, 0.0);
	}
	else if (Prism)
	{
		const std::vector<viz::PrismImage> Images = viz::prism_images(*Prism->slot);
		// The materials take sixteen copies; of a prism with more, evenly chosen ones are drawn.
		const int32 Step = FMath::Max(1, FMath::DivideAndRoundUp(static_cast<int32>(Images.size()), 16));
		double Reach = 1.0;
		for (int32 Image = 0; Image < static_cast<int32>(Images.size()) && Facets < 16; Image += Step)
		{
			const viz::PrismImage& Copy = Images[static_cast<size_t>(Image)];
			Look.Set(FLvGateLook::Facet0 + Facets++, Copy.x, Copy.y, 1.0 / Copy.scale_x, 1.0 / Copy.scale_y);
			Reach = FMath::Max(Reach, FMath::Sqrt(Copy.x * Copy.x + Copy.y * Copy.y) + FMath::Max(Copy.scale_x, Copy.scale_y) * Widen);
		}
		Spread = Reach;
		const double Turned = Quarter(PrismAngle - OutRoll);
		Look.Set(FLvGateLook::Turn, FMath::Cos(Turned), FMath::Sin(Turned), 0.0, 0.0);
	}
	// A spot light's cone can be 80 degrees to a side at most; what falls outside is cut off.
	const double TanField = FMath::Tan(FMath::DegreesToRadians(FieldAngle * 0.5));
	Spread = FMath::Min(Spread, FMath::Tan(FMath::DegreesToRadians(78.0)) / TanField);
	Spread = FMath::Max(1.0, FMath::RoundToDouble(Spread * 64.0) / 64.0);
	OutSpread = Spread;

	const double Soft = FMath::RoundToDouble((0.015 + Blur) * 128.0) / 128.0;
	const double Mip = Blur > 0.002 ? FMath::RoundToDouble(FMath::Clamp(FMath::Log2(256.0 * Blur), 0.0, 8.0) * 8.0) / 8.0 : 0.0;
	Look.Set(FLvGateLook::Shape, bIris ? FMath::RoundToDouble(Iris * 128.0) / 128.0 : 4.0, Soft, Mip, Spread);
	Look.Set(FLvGateLook::Shape2, FMath::RoundToDouble(FMath::Clamp(CoreShare * (1.0 - Beam.frost), 0.0, 0.98) * 64.0) / 64.0, Widen, Facets, 0.0);

	if (bBlades)
	{
		const double Rolled = Quarter(OutRoll);
		for (int32 Blade = 0; Blade < 4; ++Blade)
		{
			if (Edges[Blade].distance >= 1.5)
			{
				continue;    // clear of the beam: left as it is by default, whichever way the light is rolled
			}
			Look.Set(FLvGateLook::Blade0 + Blade, FMath::Cos(Rolled) * Edges[Blade].nx + FMath::Sin(Rolled) * Edges[Blade].ny,
				FMath::Cos(Rolled) * Edges[Blade].ny - FMath::Sin(Rolled) * Edges[Blade].nx, FMath::RoundToDouble(Edges[Blade].distance * 128.0) / 128.0, 0.0);
		}
	}
	return true;
}
