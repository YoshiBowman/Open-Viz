#pragma once

#include "CoreMinimal.h"
#include "Subsystems/EngineSubsystem.h"
#include "vizcore/fixture_state.h"
#include "vizcore/gdtf.h"
#include "vizcore/mesh.h"
#include "LvFixtureLibrary.generated.h"

class UMaterialInstanceDynamic;
class UMaterialInterface;
class UStaticMesh;
class UTexture2D;

/** A parsed GDTF fixture type. Shared by every fixture of that type and kept alive by them. */
struct LVFIXTURES_API FLvFixtureDefinition
{
	/** Where it came from: a file path, or "<mvr path>|<entry>" for one packed in an MVR. */
	FString Source;
	viz::FixtureType Type;
	viz::Diagnostics Diagnostics;

	/**
	 * The 3D shape of each of Type.models, in the same order: the mesh packed in the GDTF, or null
	 * for a model that names no file or whose file could not be read. The library owns them.
	 */
	TArray<TWeakObjectPtr<UStaticMesh>> ModelMeshes;

	/**
	 * The picture of each wheel slot, by wheel and slot in Type.wheels' order, as a texture of
	 * what it lets through; null for a slot without a picture. The library owns them.
	 */
	TArray<TArray<TWeakObjectPtr<UTexture2D>>> WheelPictures;

	/** The picture of a slot of one of Type's wheels; `Slot` counts from 1. Null if it has none. */
	UTexture2D* GetPicture(const viz::Wheel* Wheel, int32 Slot) const;

	/** Index of the named DMX mode, or -1. */
	int32 FindMode(const FString& ModeName) const;

	/** The model for one DMX mode, built the first time it is asked for. Null for a bad index. */
	const viz::FixtureModel* GetModel(int32 ModeIndex) const;

private:
	mutable TArray<TSharedPtr<viz::FixtureModel>> Models;
};

/** Loads GDTF files at runtime and hands out one shared definition per file. */
UCLASS()
class LVFIXTURES_API ULvFixtureLibrary : public UEngineSubsystem
{
	GENERATED_BODY()

public:
	static ULvFixtureLibrary* Get();

	/**
	 * Null, with a reason in OutError, if the file is not a usable GDTF.
	 * `Path` may also name a GDTF packed inside an MVR, as "<mvr path>|<entry name>".
	 */
	TSharedPtr<const FLvFixtureDefinition> LoadGdtfFile(const FString& Path, FString& OutError);

	/** The same for a GDTF already in memory. `Key` identifies it so it is only parsed once. */
	TSharedPtr<const FLvFixtureDefinition> LoadGdtfBytes(const FString& Key, const TArray<uint8>& Bytes, FString& OutError);

	/** What fixture bodies are made of: a dark, slightly glossy housing. */
	UMaterialInterface* GetBodyMaterial();

	/**
	 * The shape a beam of light is drawn on: a cone 1 cm long and 1 cm in radius at its far end,
	 * apex at its origin, along +X, with the beam material on it. Null if the material is not in
	 * this build, in which case beams are left to the engine's volumetric fog.
	 */
	UStaticMesh* GetBeamCone();

	/**
	 * The beam material for a cone with something standing in its beam (M_LvBeamShadow). The cone's
	 * mesh carries M_LvBeam, which has no shadow path and is the cheaper for it. Null if not in this build.
	 */
	UMaterialInterface* GetShadowBeam();

	/**
	 * A glowing surface 1 cm across facing +X, round or square, with the emissive material on it:
	 * a lens, an LED cell. Null if the material is not in this build.
	 */
	UStaticMesh* GetGlowSurface(bool bRound);

	/** What scenery is made of: a plain mid grey that shows the light falling on it. */
	UMaterialInterface* GetSceneryMaterial();

	/**
	 * The materials to draw a model's own materials with: its colour, its picture, from both
	 * sides where it says so. Fixture parts are lightened by -LvBodyShade as the plain body is.
	 * The same look gives the same material, so a rig of one grey shares one.
	 */
	TArray<UMaterialInterface*> GetModelMaterials(const viz::Mesh& Mesh, bool bFixturePart);

	/**
	 * The mesh of one model file of a rig, built the first time it is asked for. `Key` names the
	 * file for good ("<mvr path>|<file name>"); `Load` is only called if it is not known yet.
	 * Null if the file cannot be read as a mesh.
	 */
	UStaticMesh* GetSceneryMesh(const FString& Key, TFunctionRef<viz::MeshResult()> Load);

	/** A wheel's empty slot as a picture: a clear hole in the dark wheel. For a wheel that is turning. */
	UTexture2D* GetOpenHole();

	/**
	 * Two colours side by side, the first on the picture's left half and the second on its right,
	 * with a soft line between: a colour wheel standing between two of its slots. Linear colours,
	 * the largest part of each 1. Shared by every fixture that splits those two colours.
	 */
	UTexture2D* GetSplitColours(const FLinearColor& Left, const FLinearColor& Right);

private:
	TMap<FString, TSharedPtr<const FLvFixtureDefinition>> Loaded;

	/** Keeps the meshes built from GDTF models alive; fixture definitions point into this. */
	UPROPERTY(Transient)
	TArray<TObjectPtr<UStaticMesh>> ModelMeshes;

	/** Likewise the textures made from wheel pictures. */
	UPROPERTY(Transient)
	TArray<TObjectPtr<UTexture2D>> Pictures;

	UPROPERTY(Transient)
	TObjectPtr<UTexture2D> OpenHole;

	/** By the two colours, rounded. */
	UPROPERTY(Transient)
	TMap<FString, TObjectPtr<UTexture2D>> SplitColours;

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> SceneryMaterial;

	/** By key; null for a file that was tried and could not be read. */
	UPROPERTY(Transient)
	TMap<FString, TObjectPtr<UStaticMesh>> SceneryMeshes;

	/** Materials by look (colour, picture, sides), and the pictures by their bytes' checksum. */
	UPROPERTY(Transient)
	TMap<FString, TObjectPtr<UMaterialInstanceDynamic>> SurfaceMaterials;

	UPROPERTY(Transient)
	TMap<uint32, TObjectPtr<UTexture2D>> SurfacePictures;

	bool bSurfaceTried = false;
	UPROPERTY(Transient)
	TObjectPtr<UMaterialInterface> SurfaceBase;

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> BodyMaterial;

	UPROPERTY(Transient)
	TObjectPtr<UStaticMesh> BeamCone;

	bool bBeamConeTried = false;

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInterface> ShadowBeam;

	bool bShadowBeamTried = false;

	UPROPERTY(Transient)
	TObjectPtr<UStaticMesh> GlowDisc;

	UPROPERTY(Transient)
	TObjectPtr<UStaticMesh> GlowSquare;

	bool bGlowTried = false;
};
