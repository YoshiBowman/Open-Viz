#include "LvLightMeter.h"

#include "CollisionQueryParams.h"
#include "Components/SpotLightComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture2D.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "LvFixtureActor.h"
#include "LvModelMesh.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "RenderUtils.h"

namespace
{
	// The scale's colours from no light to its top value: as the plot's light-level sheet has
	// them, but starting from dark where paper starts from white.
	struct FStop
	{
		double At;
		FLinearColor Colour;
	};
	const FStop Ramp[] = {
		{0.0, FLinearColor(0.02f, 0.02f, 0.05f)}, {0.02, FLinearColor(0.35f, 0.15f, 0.45f)}, {0.15, FLinearColor(0.55f, 0.1f, 0.1f)},
		{0.4, FLinearColor(0.95f, 0.5f, 0.05f)}, {0.75, FLinearColor(1.0f, 0.92f, 0.3f)}, {1.0, FLinearColor(1.0f, 1.0f, 0.85f)}};

	FColor RampColour(double Share)
	{
		Share = FMath::Clamp(Share, 0.0, 1.0);
		for (int32 Stop = 1; Stop < UE_ARRAY_COUNT(Ramp); ++Stop)
		{
			if (Share <= Ramp[Stop].At)
			{
				const double Between = (Share - Ramp[Stop - 1].At) / (Ramp[Stop].At - Ramp[Stop - 1].At);
				return (Ramp[Stop - 1].Colour + (Ramp[Stop].Colour - Ramp[Stop - 1].Colour) * static_cast<float>(Between)).ToFColor(false);
			}
		}
		return Ramp[UE_ARRAY_COUNT(Ramp) - 1].Colour.ToFColor(false);
	}

	// A tidy top for the scale: 1, 2 or 5 times a power of ten, at or above `Value`; never under 10 lux.
	double RoundTop(double Value)
	{
		if (Value <= 10.0)
		{
			return 10.0;
		}
		const double Power = FMath::Pow(10.0, FMath::FloorToDouble(FMath::LogX(10.0, Value)));
		for (const double Step : {1.0, 2.0, 5.0, 10.0})
		{
			if (Step * Power >= Value * 0.999)
			{
				return Step * Power;
			}
		}
		return 10.0 * Power;
	}

	constexpr int32 kTracesPerFrame = 4000;
	constexpr double kLookFrom = 350.0;      // how high above the floor surfaces are looked for, cm
	constexpr double kAbove = 3.0;           // the overlay floats this far above what it lies on, cm
	constexpr double kNits = 1500.0;         // how bright the overlay's white is: about a video screen's
	constexpr double kEvery = 0.25;          // a pass begins at most this often, seconds
}

FLvLightMeter::~FLvLightMeter()
{
	// The overlay is a transient actor of the world: it goes with the world, and a world closing down is not to be asked to destroy it.
	Overlay.Reset();
}

void FLvLightMeter::Start(UWorld& World, const FBox& Stage, const TArray<AActor*>& NotSurfaces)
{
	Stop();
	UMaterialInterface* Base = LoadObject<UMaterialInterface>(nullptr, TEXT("/LightingVisualizer/Materials/M_LvScreen.M_LvScreen"));
	if (!Stage.IsValid || !Base)
	{
		return;
	}
	const double Margin = 300.0;
	const double Left = Stage.Min.X - Margin;
	const double Near = Stage.Min.Y - Margin;
	const double Wide = Stage.Max.X + Margin - Left;
	const double Deep = Stage.Max.Y + Margin - Near;
	Cell = FMath::CeilToDouble(FMath::Max(25.0, FMath::Max(Wide, Deep) / 160.0) / 5.0) * 5.0;
	Across = FMath::Clamp(FMath::CeilToInt32(Wide / Cell) + 1, 2, 161);
	Down = FMath::Clamp(FMath::CeilToInt32(Deep / Cell) + 1, 2, 161);
	Origin = FVector2D(Left, Near);

	// Where each point lies: the first surface below 3.5 m. Fixtures are not surfaces, nor are people.
	FCollisionQueryParams Query(SCENE_QUERY_STAT(LvLightMeterGround), true);
	for (TActorIterator<ALvFixtureActor> Fixture(&World); Fixture; ++Fixture)
	{
		Query.AddIgnoredActor(*Fixture);
	}
	Query.AddIgnoredActors(NotSurfaces);
	const int32 Count = Across * Down;
	Points.SetNumUninitialized(Count);
	Normals.SetNumUninitialized(Count);
	Lowest = TNumericLimits<double>::Max();
	for (int32 Row = 0; Row < Down; ++Row)
	{
		for (int32 Column = 0; Column < Across; ++Column)
		{
			const int32 At = Row * Across + Column;
			const FVector From(Origin.X + Column * Cell, Origin.Y + Row * Cell, kLookFrom);
			FHitResult Hit;
			if (World.LineTraceSingleByChannel(Hit, From, FVector(From.X, From.Y, -100.0), ECC_Visibility, Query) && Hit.ImpactNormal.Z > 0.2)
			{
				Points[At] = Hit.ImpactPoint;
				Normals[At] = Hit.ImpactNormal;
			}
			else
			{
				Points[At] = FVector(From.X, From.Y, 0.0);    // the stage's own floor
				Normals[At] = FVector::UpVector;
			}
			Lowest = FMath::Min(Lowest, Points[At].Z);
		}
	}
	Values.Init(0.0, Count);
	Next.Init(0.0, Count);

	// The overlay: a sheet over those points, its colours a texture of one texel a point.
	viz::Mesh Sheet;
	Sheet.positions.reserve(static_cast<size_t>(Count) * 3);
	Sheet.normals.reserve(static_cast<size_t>(Count) * 3);
	Sheet.uvs.reserve(static_cast<size_t>(Count) * 2);
	for (int32 At = 0; At < Count; ++At)
	{
		// In the rig file's terms, which LvBuildStaticMesh turns back: metres, Y mirrored.
		const FVector Lifted = Points[At] + Normals[At] * kAbove;
		Sheet.positions.insert(Sheet.positions.end(), {static_cast<float>(Lifted.X / 100.0), static_cast<float>(-Lifted.Y / 100.0), static_cast<float>(Lifted.Z / 100.0)});
		Sheet.normals.insert(Sheet.normals.end(), {static_cast<float>(Normals[At].X), static_cast<float>(-Normals[At].Y), static_cast<float>(Normals[At].Z)});
		const int32 Column = At % Across;
		const int32 Row = At / Across;
		Sheet.uvs.insert(Sheet.uvs.end(), {static_cast<float>((Column + 0.5) / Across), static_cast<float>((Row + 0.5) / Down)});
	}
	for (int32 Row = 0; Row + 1 < Down; ++Row)
	{
		for (int32 Column = 0; Column + 1 < Across; ++Column)
		{
			const uint32 A = static_cast<uint32>(Row * Across + Column);
			const uint32 B = A + 1;
			const uint32 C = A + static_cast<uint32>(Across);
			const uint32 D = C + 1;
			// Facing up, as the screen material only glows on its front (worked out by looking: the other way it is black).
			Sheet.triangles.insert(Sheet.triangles.end(), {A, B, C, B, D, C});
		}
	}
	UStaticMesh* Mesh = LvBuildStaticMesh(Sheet, GetTransientPackage(), Base, false, {});
	UTexture2D* Made = UTexture2D::CreateTransient(Across, Down, PF_B8G8R8A8);
	if (!Mesh || !Made || !Made->GetPlatformData() || Made->GetPlatformData()->Mips.Num() == 0)
	{
		return;
	}
	Made->SRGB = true;
	Made->NeverStream = true;
	Made->Filter = TF_Bilinear;
	Made->AddressX = TA_Clamp;
	Made->AddressY = TA_Clamp;
	void* Data = Made->GetPlatformData()->Mips[0].BulkData.Lock(LOCK_READ_WRITE);
	FMemory::Memzero(Data, static_cast<SIZE_T>(Count) * 4);
	Made->GetPlatformData()->Mips[0].BulkData.Unlock();
	Made->UpdateResource();

	FActorSpawnParameters Spawn;
	Spawn.ObjectFlags |= RF_Transient;
	AActor* Actor = World.SpawnActor<AActor>(AActor::StaticClass(), FTransform::Identity, Spawn);
	if (!Actor)
	{
		return;
	}
	USceneComponent* Root = NewObject<USceneComponent>(Actor, TEXT("Root"));
	Actor->SetRootComponent(Root);
	Root->RegisterComponent();
	UStaticMeshComponent* Component = NewObject<UStaticMeshComponent>(Actor, TEXT("Light levels"));
	Component->SetStaticMesh(Mesh);
	Component->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Component->SetCastShadow(false);
	Component->SetupAttachment(Root);
	UMaterialInstanceDynamic* Dynamic = UMaterialInstanceDynamic::Create(Base, Actor);
	Dynamic->SetTextureParameterValue(TEXT("Picture"), Made);
	Dynamic->SetScalarParameterValue(TEXT("Nits"), static_cast<float>(kNits));
	Dynamic->SetVectorParameterValue(TEXT("Part"), FLinearColor(0.0f, 0.0f, 1.0f, 0.0f));
	Dynamic->SetVectorParameterValue(TEXT("PartDown"), FLinearColor(0.0f, 1.0f, 0.0f, 0.0f));
	Component->SetMaterial(0, Dynamic);
	Component->RegisterComponent();
	Overlay = Actor;
	Texture = Made;
	Material = Dynamic;
	bPass = false;
	PassStarted = -1.0;
	Passes = 0;
	Peak = 0.0;
	Top = 100.0;
}

void FLvLightMeter::Stop()
{
	if (Overlay.IsValid())
	{
		Overlay->Destroy();
	}
	Overlay.Reset();
	Texture.Reset();
	Material.Reset();
	Queue.Reset();
	bPass = false;
	Passes = 0;
}

void FLvLightMeter::Tick(UWorld& World, const TArray<TWeakObjectPtr<ALvFixtureActor>>& Fixtures)
{
	if (!IsOn())
	{
		return;
	}
	if (Queue.IsEmpty())
	{
		if (bPass)
		{
			// A pass is whole: it is what the overlay shows from now on.
			Values = Next;
			bPass = false;
			++Passes;
			LastBeams = Beams;
			LastReached = Reached;
			LastTraces = TracesMade;
			LastBlocked = Blocked;
			Show();
		}
		if (PassStarted >= 0.0 && World.GetTimeSeconds() - PassStarted < kEvery)
		{
			return;
		}
		for (const TWeakObjectPtr<ALvFixtureActor>& Fixture : Fixtures)
		{
			if (Fixture.IsValid() && !Fixture->IsHidden())
			{
				Queue.Add(Fixture);
			}
		}
		Next.Init(0.0, Points.Num());
		Beams = Reached = TracesMade = Blocked = 0;
		bPass = true;
		PassStarted = World.GetTimeSeconds();
	}
	// What is in the way of a beam shades it, but other fixtures' bodies are not counted.
	FCollisionQueryParams Query(SCENE_QUERY_STAT(LvLightMeterBeams), false);
	for (const TWeakObjectPtr<ALvFixtureActor>& Fixture : Fixtures)
	{
		if (Fixture.IsValid())
		{
			Query.AddIgnoredActor(Fixture.Get());
		}
	}
	if (Overlay.IsValid())
	{
		Query.AddIgnoredActor(Overlay.Get());
	}
	int32 Traces = 0;
	while (!Queue.IsEmpty() && Traces < kTracesPerFrame)
	{
		const TWeakObjectPtr<ALvFixtureActor> Fixture = Queue.Pop(EAllowShrinking::No);
		if (!Fixture.IsValid() || Fixture->IsHidden())
		{
			continue;
		}
		TInlineComponentArray<USpotLightComponent*> Lights(Fixture.Get());
		for (const USpotLightComponent* Light : Lights)
		{
			if (!Light || !Light->IsVisible() || Light->Intensity <= 0.0f)
			{
				continue;
			}
			++Beams;
			const double Candela = Light->Intensity;    // the fixtures' lights are in candela
			const FVector From = Light->GetComponentLocation();
			const FVector Along = Light->GetForwardVector();
			const double Outer = FMath::DegreesToRadians(FMath::Clamp(static_cast<double>(Light->OuterConeAngle), 0.1, 89.0));
			const double Inner = FMath::Min(FMath::DegreesToRadians(FMath::Max(static_cast<double>(Light->InnerConeAngle), 0.0)), Outer * 0.999);
			const double CosOuter = FMath::Cos(Outer);
			// The points it can reach: within its cone's reach across the floor below it.
			const double Height = From.Z - Lowest;
			if (Height <= 1.0)
			{
				continue;
			}
			const double Lean = FMath::Acos(FMath::Clamp(-Along.Z, -1.0, 1.0));
			const double Reach = Lean + Outer >= FMath::DegreesToRadians(85.0) ? 1e9 : Height * FMath::Tan(Lean + Outer) + 2.0 * Cell;
			const int32 FirstColumn = FMath::Clamp(FMath::FloorToInt32((From.X - Reach - Origin.X) / Cell), 0, Across - 1);
			const int32 LastColumn = FMath::Clamp(FMath::CeilToInt32((From.X + Reach - Origin.X) / Cell), 0, Across - 1);
			const int32 FirstRow = FMath::Clamp(FMath::FloorToInt32((From.Y - Reach - Origin.Y) / Cell), 0, Down - 1);
			const int32 LastRow = FMath::Clamp(FMath::CeilToInt32((From.Y + Reach - Origin.Y) / Cell), 0, Down - 1);
			const FVector Out = From + Along * 20.0;    // clear of the lens
			for (int32 Row = FirstRow; Row <= LastRow; ++Row)
			{
				for (int32 Column = FirstColumn; Column <= LastColumn; ++Column)
				{
					const int32 At = Row * Across + Column;
					const FVector Towards = Points[At] - From;
					const double Squared = Towards.SizeSquared();
					if (Squared < 100.0)
					{
						continue;
					}
					const FVector Way = Towards / FMath::Sqrt(Squared);
					const double Cosine = FVector::DotProduct(Way, Along);
					if (Cosine <= CosOuter)
					{
						continue;
					}
					const double Meets = -FVector::DotProduct(Way, Normals[At]);
					if (Meets <= 0.0)
					{
						continue;    // it reaches the surface from below
					}
					const double Angle = FMath::Acos(FMath::Min(Cosine, 1.0));
					const double Share = Angle <= Inner ? 1.0 : (Outer - Angle) / (Outer - Inner);
					++Traces;
					++TracesMade;
					FHitResult Hit;
					if (World.LineTraceSingleByChannel(Hit, Out, Points[At] + Normals[At] * 5.0, ECC_Visibility, Query))
					{
						++Blocked;
						continue;    // something stands in the way
					}
					++Reached;
					Next[At] += Candela * Share * Meets / (Squared / 10000.0);    // centimetres squared to metres squared
				}
			}
		}
	}
}

void FLvLightMeter::Show()
{
	// White is at nearly the most light there is: the 98th hundredth of the lit points, so that a
	// hot spot under one fixture does not leave the rest of the stage in the scale's bottom colours.
	Peak = 0.0;
	TArray<double> Lit;
	Lit.Reserve(Values.Num());
	for (const double Value : Values)
	{
		Peak = FMath::Max(Peak, Value);
		if (Value > 1.0)
		{
			Lit.Add(Value);
		}
	}
	double Near = Peak;
	if (Lit.Num() >= 20)
	{
		const int32 Index = FMath::Clamp(FMath::FloorToInt32(Lit.Num() * 0.98), 0, Lit.Num() - 1);
		Lit.Sort();
		Near = Lit[Index];
	}
	Top = RoundTop(Near);
	UTexture2D* Made = Texture.Get();
	if (!Made)
	{
		return;
	}
	const int32 Count = Across * Down;
	uint8* Copy = static_cast<uint8*>(FMemory::Malloc(static_cast<SIZE_T>(Count) * 4));
	for (int32 At = 0; At < Count; ++At)
	{
		const FColor Colour = RampColour(Values[At] / Top);
		Copy[At * 4] = Colour.B;
		Copy[At * 4 + 1] = Colour.G;
		Copy[At * 4 + 2] = Colour.R;
		Copy[At * 4 + 3] = 255;
	}
	FUpdateTextureRegion2D* Region = new FUpdateTextureRegion2D(0, 0, 0, 0, Across, Down);
	Made->UpdateTextureRegions(0, 1, Region, Across * 4, 4, Copy, [](uint8* Data, const FUpdateTextureRegion2D* Regions) { FMemory::Free(Data); delete Regions; });
}

double FLvLightMeter::ReadingAt(const FVector& Where) const
{
	if (!IsOn() || Passes == 0 || Across < 2 || Down < 2)
	{
		return -1.0;
	}
	const double X = (Where.X - Origin.X) / Cell;
	const double Y = (Where.Y - Origin.Y) / Cell;
	if (X < 0.0 || Y < 0.0 || X > Across - 1 || Y > Down - 1)
	{
		return -1.0;
	}
	const int32 Column = FMath::Min(FMath::FloorToInt32(X), Across - 2);
	const int32 Row = FMath::Min(FMath::FloorToInt32(Y), Down - 2);
	const double U = X - Column;
	const double V = Y - Row;
	const int32 At = Row * Across + Column;
	return FMath::Lerp(FMath::Lerp(Values[At], Values[At + 1], U), FMath::Lerp(Values[At + Across], Values[At + Across + 1], U), V);
}
