#include "LvStage.h"

#include "Camera/CameraActor.h"
#include "Components/ExponentialHeightFogComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/ExponentialHeightFog.h"
#include "Engine/PostProcessVolume.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"

ACameraActor* LvStage::Build(UWorld& World, float ExposureCompensation)
{
	// Nothing spawned here belongs in a saved level: the stage is built again each time.
	FActorSpawnParameters Spawn;
	Spawn.ObjectFlags |= RF_Transient;

	// A floor for the beams to land on.
	if (UStaticMesh* Plane = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Plane.Plane")))
	{
		AStaticMeshActor* Floor = World.SpawnActor<AStaticMeshActor>(FVector::ZeroVector, FRotator::ZeroRotator, Spawn);
		Floor->GetStaticMeshComponent()->SetMobility(EComponentMobility::Movable);
		Floor->GetStaticMeshComponent()->SetStaticMesh(Plane);
		Floor->SetActorScale3D(FVector(400.0, 400.0, 1.0));
		// A plain dark floor. Left without a material the engine draws its grey test pattern on
		// it, which at this size shows as blotches in every pool of light.
		if (UMaterialInterface* Base = LoadObject<UMaterialInterface>(nullptr, TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial")))
		{
			UMaterialInstanceDynamic* Surface = UMaterialInstanceDynamic::Create(Base, Floor);
			Surface->SetVectorParameterValue(TEXT("Color"), FLinearColor(0.14f, 0.14f, 0.14f));
			Floor->GetStaticMeshComponent()->SetMaterial(0, Surface);
		}
	}

	// Haze, so lights that scatter into volumetric fog have something to light.
	AExponentialHeightFog* Fog = World.SpawnActor<AExponentialHeightFog>(FVector(0.0, 0.0, 100.0), FRotator::ZeroRotator, Spawn);
	Fog->GetComponent()->SetFogDensity(0.06f);
	Fog->GetComponent()->SetFogHeightFalloff(0.01f);    // haze fills the room, it does not hug the floor
	Fog->GetComponent()->SetVolumetricFog(true);

	// Fixed exposure everywhere, so the look does not drift as a chase changes brightness.
	APostProcessVolume* PostProcess = World.SpawnActor<APostProcessVolume>(FVector::ZeroVector, FRotator::ZeroRotator, Spawn);
	PostProcess->bUnbound = true;
	PostProcess->Settings.bOverride_AutoExposureMethod = true;
	PostProcess->Settings.AutoExposureMethod = AEM_Manual;
	// Without this, manual exposure uses the camera's daylight aperture/shutter/ISO and a dark
	// stage renders black. With it off, exposure is the compensation value alone.
	PostProcess->Settings.bOverride_AutoExposureApplyPhysicalCameraExposure = true;
	PostProcess->Settings.AutoExposureApplyPhysicalCameraExposure = false;
	PostProcess->Settings.bOverride_AutoExposureBias = true;
	PostProcess->Settings.AutoExposureBias = ExposureCompensation;

	// -LvBeamRes=<25..100>: beams in haze are drawn at this percentage of the picture's resolution
	// and scaled up. They are soft, so half loses little, and a stage full of beams costs a
	// quarter as much to draw. (Beams are translucent; this is the engine's setting for all such.)
	// The engine moves such translucency back into the full-resolution pass by itself wherever it
	// lies behind a camera's focus distance (r.Translucency.AutoBeforeDOF): not wanted here, where
	// the resolution is the point and nothing is out of focus.
	if (IConsoleVariable* Variable = IConsoleManager::Get().FindConsoleVariable(TEXT("r.Translucency.AutoBeforeDOF")))
	{
		Variable->Set(-1.0f, ECVF_SetByCode);
	}
	float BeamResolution = 100.0f;
	if (FParse::Value(FCommandLine::Get(), TEXT("LvBeamRes="), BeamResolution))
	{
		if (IConsoleVariable* Variable = IConsoleManager::Get().FindConsoleVariable(TEXT("r.SeparateTranslucencyScreenPercentage")))
		{
			Variable->Set(FMath::Clamp(BeamResolution, 25.0f, 100.0f), ECVF_SetByCode);
		}
	}

	return World.SpawnActor<ACameraActor>(FVector::ZeroVector, FRotator::ZeroRotator, Spawn);
}

void LvStage::SetExposure(UWorld& World, float ExposureCompensation)
{
	for (TActorIterator<APostProcessVolume> Volume(&World); Volume; ++Volume)
	{
		if (Volume->bUnbound && Volume->Settings.bOverride_AutoExposureBias)
		{
			Volume->Settings.AutoExposureBias = ExposureCompensation;
		}
	}
}

void LvStage::Aim(UWorld& World, ACameraActor* Camera, const FVector& Location, const FVector& Target)
{
	if (!Camera)
	{
		return;
	}
	Camera->SetActorLocationAndRotation(Location, (Target - Location).Rotation());
	if (APlayerController* Controller = World.GetFirstPlayerController())
	{
		Controller->SetViewTarget(Camera);
		// The default pawn is a visible sphere that would sit in the middle of the stage.
		if (APawn* Pawn = Controller->GetPawn())
		{
			Pawn->SetActorHiddenInGame(true);
		}
	}
}
