#include "LvFixtureLibrary.h"

#include "Engine/Engine.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture2D.h"
#include "ImageUtils.h"
#include "Misc/Crc.h"
#include "LvCoordinates.h"
#include "LvModelMesh.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "TextureResource.h"
#include "vizcore/archive.h"
#include "vizcore/image.h"
#include "vizcore/mesh.h"

DEFINE_LOG_CATEGORY_STATIC(LogLvFixtures, Log, All);

namespace
{
	const int32 PictureSide = 256;    // what GDTF recommends for a gobo, and what the engine's light function atlas can show

	float SrgbToLinear(float Encoded)
	{
		return Encoded <= 0.04045f ? Encoded / 12.92f : FMath::Pow((Encoded + 0.055f) / 1.055f, 2.4f);
	}

	uint8 LinearToSrgb(float Linear)
	{
		const float Clamped = FMath::Clamp(Linear, 0.0f, 1.0f);
		const float Encoded = Clamped <= 0.0031308f ? Clamped * 12.92f : 1.055f * FMath::Pow(Clamped, 1.0f / 2.4f) - 0.055f;
		return static_cast<uint8>(FMath::RoundToInt(Encoded * 255.0f));
	}

	/**
	 * A texture from a square of linear-light RGB, with every smaller version of itself down to one
	 * pixel: frost and focus blur a gobo by looking it up in a smaller version.
	 */
	UTexture2D* BuildPictureTexture(TArray<FVector3f> Pixels, int32 Side)
	{
		UTexture2D* Texture = UTexture2D::CreateTransient(Side, Side, PF_B8G8R8A8);
		if (!Texture)
		{
			return nullptr;
		}
		Texture->SRGB = true;
		Texture->AddressX = TA_Clamp;
		Texture->AddressY = TA_Clamp;
		Texture->Filter = TF_Trilinear;
		Texture->NeverStream = true;
		FTexturePlatformData* Data = Texture->GetPlatformData();
		for (int32 Level = 0; Side >= 1; ++Level)
		{
			if (Level > 0)
			{
				Data->Mips.Add(new FTexture2DMipMap(Side, Side));
			}
			FTexture2DMipMap& Mip = Data->Mips[Level];
			Mip.BulkData.Lock(LOCK_READ_WRITE);
			uint8* Out = static_cast<uint8*>(Mip.BulkData.Realloc(static_cast<int64>(Side) * Side * 4));
			for (int32 Index = 0; Index < Side * Side; ++Index)
			{
				Out[Index * 4 + 0] = LinearToSrgb(Pixels[Index].Z);
				Out[Index * 4 + 1] = LinearToSrgb(Pixels[Index].Y);
				Out[Index * 4 + 2] = LinearToSrgb(Pixels[Index].X);
				Out[Index * 4 + 3] = 255;
			}
			Mip.BulkData.Unlock();
			if (Side == 1)
			{
				break;
			}
			// The next one down: each pixel the average of four.
			const int32 Half = Side / 2;
			TArray<FVector3f> Smaller;
			Smaller.SetNumUninitialized(Half * Half);
			for (int32 Y = 0; Y < Half; ++Y)
			{
				for (int32 X = 0; X < Half; ++X)
				{
					Smaller[Y * Half + X] = (Pixels[(Y * 2) * Side + X * 2] + Pixels[(Y * 2) * Side + X * 2 + 1] + Pixels[(Y * 2 + 1) * Side + X * 2] +
						Pixels[(Y * 2 + 1) * Side + X * 2 + 1]) * 0.25f;
				}
			}
			Pixels = MoveTemp(Smaller);
			Side = Half;
		}
		Texture->UpdateResource();
		return Texture;
	}

	/** What a gobo picture lets through, brought to one size whatever size the file has it in. */
	UTexture2D* BuildGoboTexture(const viz::Image& Picture)
	{
		const viz::Image Through = viz::gobo_transmission(Picture);
		if (Through.width < 1 || Through.height < 1)
		{
			return nullptr;
		}
		float Table[256];
		for (int32 Value = 0; Value < 256; ++Value)
		{
			Table[Value] = SrgbToLinear(Value / 255.0f);
		}
		auto At = [&Through, &Table](int32 X, int32 Y)
		{
			const uint8* Pixel = &Through.rgba[(static_cast<size_t>(FMath::Clamp(Y, 0, Through.height - 1)) * Through.width + FMath::Clamp(X, 0, Through.width - 1)) * 4];
			return FVector3f(Table[Pixel[0]], Table[Pixel[1]], Table[Pixel[2]]);
		};
		// Sixteen looks at the picture for each pixel made, each between the four pixels nearest it:
		// right for a picture four times the size, and smooth for one that is smaller.
		TArray<FVector3f> Pixels;
		Pixels.SetNumUninitialized(PictureSide * PictureSide);
		const int32 Looks = 4;
		for (int32 Y = 0; Y < PictureSide; ++Y)
		{
			for (int32 X = 0; X < PictureSide; ++X)
			{
				FVector3f Sum(0.0f, 0.0f, 0.0f);
				for (int32 SubY = 0; SubY < Looks; ++SubY)
				{
					for (int32 SubX = 0; SubX < Looks; ++SubX)
					{
						const float U = (X + (SubX + 0.5f) / Looks) / PictureSide * Through.width - 0.5f;
						const float V = (Y + (SubY + 0.5f) / Looks) / PictureSide * Through.height - 0.5f;
						const int32 X0 = FMath::FloorToInt(U);
						const int32 Y0 = FMath::FloorToInt(V);
						const float Fx = U - X0;
						const float Fy = V - Y0;
						Sum += FMath::Lerp(FMath::Lerp(At(X0, Y0), At(X0 + 1, Y0), Fx), FMath::Lerp(At(X0, Y0 + 1), At(X0 + 1, Y0 + 1), Fx), Fy);
					}
				}
				Pixels[Y * PictureSide + X] = Sum / static_cast<float>(Looks * Looks);
			}
		}
		return BuildPictureTexture(MoveTemp(Pixels), PictureSide);
	}
}

UTexture2D* FLvFixtureDefinition::GetPicture(const viz::Wheel* Wheel, int32 Slot) const
{
	if (!Wheel || Type.wheels.empty() || Wheel < Type.wheels.data() || Wheel >= Type.wheels.data() + Type.wheels.size())
	{
		return nullptr;
	}
	const int32 WheelIndex = static_cast<int32>(Wheel - Type.wheels.data());
	if (!WheelPictures.IsValidIndex(WheelIndex) || !WheelPictures[WheelIndex].IsValidIndex(Slot - 1))
	{
		return nullptr;
	}
	return WheelPictures[WheelIndex][Slot - 1].Get();
}

UTexture2D* ULvFixtureLibrary::GetOpenHole()
{
	if (!OpenHole)
	{
		TArray<FVector3f> Pixels;
		Pixels.SetNumUninitialized(PictureSide * PictureSide);
		for (int32 Y = 0; Y < PictureSide; ++Y)
		{
			for (int32 X = 0; X < PictureSide; ++X)
			{
				const float U = (X + 0.5f) / PictureSide * 2.0f - 1.0f;
				const float V = (Y + 0.5f) / PictureSide * 2.0f - 1.0f;
				const float Open = 1.0f - FMath::SmoothStep(0.92f, 0.96f, FMath::Sqrt(U * U + V * V));
				Pixels[Y * PictureSide + X] = FVector3f(Open, Open, Open);
			}
		}
		OpenHole = BuildPictureTexture(MoveTemp(Pixels), PictureSide);
	}
	return OpenHole;
}

int32 FLvFixtureDefinition::FindMode(const FString& ModeName) const
{
	return Type.find_mode(LvToStd(ModeName));
}

const viz::FixtureModel* FLvFixtureDefinition::GetModel(int32 ModeIndex) const
{
	if (ModeIndex < 0 || ModeIndex >= static_cast<int32>(Type.modes.size()))
	{
		return nullptr;
	}
	if (Models.Num() == 0)
	{
		Models.SetNum(static_cast<int32>(Type.modes.size()));
	}
	if (!Models[ModeIndex])
	{
		Models[ModeIndex] = MakeShared<viz::FixtureModel>(Type, static_cast<size_t>(ModeIndex));
	}
	return Models[ModeIndex].Get();
}

UMaterialInterface* ULvFixtureLibrary::GetBodyMaterial()
{
	if (!BodyMaterial)
	{
		if (UMaterialInterface* Base = LoadObject<UMaterialInterface>(nullptr, TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial")))
		{
			// Fixture housings are black. -LvBodyShade=<0..1> makes them lighter, to see their shape.
			float Shade = 0.02f;
			FParse::Value(FCommandLine::Get(), TEXT("LvBodyShade="), Shade);
			Shade = FMath::Clamp(Shade, 0.0f, 1.0f);
			BodyMaterial = UMaterialInstanceDynamic::Create(Base, this);
			BodyMaterial->SetVectorParameterValue(TEXT("Color"), FLinearColor(Shade, Shade, Shade * 1.1f));
		}
	}
	return BodyMaterial;
}

UMaterialInterface* ULvFixtureLibrary::GetSceneryMaterial()
{
	if (!SceneryMaterial)
	{
		if (UMaterialInterface* Base = LoadObject<UMaterialInterface>(nullptr, TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial")))
		{
			SceneryMaterial = UMaterialInstanceDynamic::Create(Base, this);
			SceneryMaterial->SetVectorParameterValue(TEXT("Color"), FLinearColor(0.22f, 0.22f, 0.22f));
		}
	}
	return SceneryMaterial;
}

TArray<UMaterialInterface*> ULvFixtureLibrary::GetModelMaterials(const viz::Mesh& Mesh, bool bFixturePart)
{
	TArray<UMaterialInterface*> Out;
	if (Mesh.materials.empty())
	{
		return Out;
	}
	if (!bSurfaceTried)
	{
		bSurfaceTried = true;
		SurfaceBase = LoadObject<UMaterialInterface>(nullptr, TEXT("/LightingVisualizer/Materials/M_LvSurface.M_LvSurface"));
		if (!SurfaceBase)
		{
			UE_LOG(LogLvFixtures, Warning, TEXT("the surface material M_LvSurface is missing (run tools/build_materials.py): models are drawn plain"));
		}
	}
	float Shade = 0.02f;
	FParse::Value(FCommandLine::Get(), TEXT("LvBodyShade="), Shade);
	Shade = FMath::Clamp(Shade, 0.0f, 1.0f);
	for (const viz::MeshMaterial& Described : Mesh.materials)
	{
		if (!SurfaceBase)
		{
			Out.Add(nullptr);
			continue;
		}
		FLinearColor Colour(Described.color[0], Described.color[1], Described.color[2], 1.0f);
		if (bFixturePart)
		{
			Colour = FMath::Lerp(Colour, FLinearColor::White, Shade);    // -LvBodyShade lightens black housings, to see their shape
		}
		UTexture2D* Picture = nullptr;
		if (Described.textured())
		{
			const uint32 Sum = FCrc::MemCrc32(Described.image.data(), static_cast<int32>(Described.image.size()));
			if (const TObjectPtr<UTexture2D>* Known = SurfacePictures.Find(Sum))
			{
				Picture = Known->Get();
			}
			else
			{
				Picture = FImageUtils::ImportBufferAsTexture2D(TArrayView64<const uint8>(Described.image.data(), static_cast<int64>(Described.image.size())));
				if (!Picture)
				{
					UE_LOG(LogLvFixtures, Warning, TEXT("a model's picture %s (%s, %d bytes) cannot be read: its colour alone is used"),
						*LvToFString(Described.texture_file.empty() ? Described.name : Described.texture_file), *LvToFString(Described.image_type),
						static_cast<int32>(Described.image.size()));
				}
				SurfacePictures.Add(Sum, Picture);
			}
		}
		const FString Key = FString::Printf(TEXT("%.4f %.4f %.4f %s %s"), Colour.R, Colour.G, Colour.B, Picture ? *Picture->GetName() : TEXT("-"),
			Described.double_sided ? TEXT("2") : TEXT("1"));
		TObjectPtr<UMaterialInstanceDynamic>& Made = SurfaceMaterials.FindOrAdd(Key);
		if (!Made)
		{
			Made = UMaterialInstanceDynamic::Create(SurfaceBase, this);
			Made->SetVectorParameterValue(TEXT("Color"), Colour);
			if (Picture)
			{
				Made->SetTextureParameterValue(TEXT("Picture"), Picture);
			}
		}
		Out.Add(Made);
	}
	return Out;
}

UStaticMesh* ULvFixtureLibrary::GetSceneryMesh(const FString& Key, TFunctionRef<viz::MeshResult()> Load)
{
	if (const TObjectPtr<UStaticMesh>* Known = SceneryMeshes.Find(Key))
	{
		return Known->Get();
	}
	const viz::MeshResult Shape = Load();
	for (const viz::Diagnostic& Item : Shape.diagnostics.items)
	{
		UE_LOG(LogLvFixtures, Verbose, TEXT("%s: %s"), *Key, *LvToFString(Item.message));
	}
	UStaticMesh* Built = Shape.ok ? LvBuildStaticMesh(Shape.mesh, this, GetSceneryMaterial(), true, GetModelMaterials(Shape.mesh, false)) : nullptr;
	SceneryMeshes.Add(Key, Built);
	return Built;
}

UStaticMesh* ULvFixtureLibrary::GetBeamCone()
{
	if (bBeamConeTried)
	{
		return BeamCone;
	}
	bBeamConeTried = true;
	UMaterialInterface* Material = LoadObject<UMaterialInterface>(nullptr, TEXT("/LightingVisualizer/Materials/M_LvBeam.M_LvBeam"));
	if (!Material)
	{
		UE_LOG(LogLvFixtures, Warning, TEXT("The beam material is missing (tools/build_materials.py builds it); beams fall back to volumetric fog"));
		return nullptr;
	}

	// The core's mesh is in metres, so 0.01 is the 1 cm this cone measures in Unreal.
	const int32 Sides = 40;
	const float Unit = 0.01f;
	viz::Mesh Cone;
	auto Add = [&Cone](float X, float Y, float Z, const FVector3f& Normal)
	{
		Cone.positions.insert(Cone.positions.end(), {X, Y, Z});
		Cone.normals.insert(Cone.normals.end(), {Normal.X, Normal.Y, Normal.Z});
		return static_cast<uint32>(Cone.positions.size() / 3 - 1);
	};
	for (int32 Side = 0; Side < Sides; ++Side)
	{
		const float A0 = 2.0f * UE_PI * Side / Sides;
		const float A1 = 2.0f * UE_PI * (Side + 1) / Sides;
		const FVector3f P0(Unit, Unit * FMath::Cos(A0), Unit * FMath::Sin(A0));
		const FVector3f P1(Unit, Unit * FMath::Cos(A1), Unit * FMath::Sin(A1));
		// Each triangle is wound so that it faces away from the axis: the material draws the far
		// side of the cone only, and which side is far is decided by this.
		const FVector3f Outward = FVector3f(-1.0f, FMath::Cos((A0 + A1) * 0.5f), FMath::Sin((A0 + A1) * 0.5f)).GetSafeNormal();
		const bool bFlip = FVector3f::DotProduct(FVector3f::CrossProduct(P0, P1), Outward) < 0.0f;
		const uint32 Apex = Add(0.0f, 0.0f, 0.0f, Outward);
		const uint32 V0 = Add(P0.X, P0.Y, P0.Z, Outward);
		const uint32 V1 = Add(P1.X, P1.Y, P1.Z, Outward);
		Cone.triangles.insert(Cone.triangles.end(), {Apex, bFlip ? V1 : V0, bFlip ? V0 : V1});
		// The far end, facing along +X.
		const FVector3f Along(1.0f, 0.0f, 0.0f);
		const FVector3f Centre(Unit, 0.0f, 0.0f);
		const bool bFlipEnd = FVector3f::DotProduct(FVector3f::CrossProduct(P0 - Centre, P1 - Centre), Along) < 0.0f;
		const uint32 C = Add(Centre.X, Centre.Y, Centre.Z, Along);
		const uint32 E0 = Add(P0.X, P0.Y, P0.Z, Along);
		const uint32 E1 = Add(P1.X, P1.Y, P1.Z, Along);
		Cone.triangles.insert(Cone.triangles.end(), {C, bFlipEnd ? E1 : E0, bFlipEnd ? E0 : E1});
	}
	BeamCone = LvBuildStaticMesh(Cone, this, Material);
	return BeamCone;
}

UStaticMesh* ULvFixtureLibrary::GetGlowSurface(bool bRound)
{
	if (!bGlowTried)
	{
		bGlowTried = true;
		if (UMaterialInterface* Material = LoadObject<UMaterialInterface>(nullptr, TEXT("/LightingVisualizer/Materials/M_LvEmissive.M_LvEmissive")))
		{
			// In the core's terms (metres): flat in the plane X = 0, half a centimetre from its
			// centre to its edge, facing +X.
			const float Half = 0.005f;
			auto Build = [&](int32 Sides, float Turn)
			{
				viz::Mesh Surface;
				Surface.positions = {0.0f, 0.0f, 0.0f};
				Surface.normals = {1.0f, 0.0f, 0.0f};
				for (int32 Side = 0; Side < Sides; ++Side)
				{
					const float Angle = Turn + 2.0f * UE_PI * Side / Sides;
					// A square's corners lie further out than its sides: half a side times root two.
					const float Reach = Sides == 4 ? Half * UE_SQRT_2 : Half;
					Surface.positions.insert(Surface.positions.end(), {0.0f, Reach * FMath::Cos(Angle), Reach * FMath::Sin(Angle)});
					Surface.normals.insert(Surface.normals.end(), {1.0f, 0.0f, 0.0f});
				}
				for (int32 Side = 0; Side < Sides; ++Side)
				{
					// Both windings, so it shows whichever way the mirroring leaves it facing.
					const uint32 A = static_cast<uint32>(1 + Side);
					const uint32 B = static_cast<uint32>(1 + (Side + 1) % Sides);
					Surface.triangles.insert(Surface.triangles.end(), {0u, A, B, 0u, B, A});
				}
				return LvBuildStaticMesh(Surface, this, Material);
			};
			GlowDisc = Build(32, 0.0f);
			GlowSquare = Build(4, UE_PI / 4.0f);
		}
		else
		{
			UE_LOG(LogLvFixtures, Warning, TEXT("The emissive material is missing (tools/build_materials.py builds it); lenses and LED cells will not glow"));
		}
	}
	return bRound ? GlowDisc : GlowSquare;
}

ULvFixtureLibrary* ULvFixtureLibrary::Get()
{
	return GEngine ? GEngine->GetEngineSubsystem<ULvFixtureLibrary>() : nullptr;
}

TSharedPtr<const FLvFixtureDefinition> ULvFixtureLibrary::LoadGdtfFile(const FString& Path, FString& OutError)
{
	if (const TSharedPtr<const FLvFixtureDefinition>* Known = Loaded.Find(Path))
	{
		return *Known;
	}
	FString MvrPath;
	FString Entry;
	if (Path.Split(TEXT("|"), &MvrPath, &Entry))
	{
		viz::Archive Mvr;
		std::string ArchiveError;
		std::vector<uint8_t> Packed;
		if (!Mvr.open_file(LvToStd(MvrPath), ArchiveError) || !Mvr.read(LvToStd(Entry), Packed))
		{
			OutError = FString::Printf(TEXT("cannot read %s from %s"), *Entry, *MvrPath);
			return nullptr;
		}
		return LoadGdtfBytes(Path, TArray<uint8>(Packed.data(), static_cast<int32>(Packed.size())), OutError);
	}

	TArray<uint8> Bytes;
	if (!FFileHelper::LoadFileToArray(Bytes, *Path))
	{
		OutError = FString::Printf(TEXT("cannot read %s"), *Path);
		return nullptr;
	}
	return LoadGdtfBytes(Path, Bytes, OutError);
}

TSharedPtr<const FLvFixtureDefinition> ULvFixtureLibrary::LoadGdtfBytes(const FString& Key, const TArray<uint8>& Bytes, FString& OutError)
{
	if (const TSharedPtr<const FLvFixtureDefinition>* Known = Loaded.Find(Key))
	{
		return *Known;
	}

	viz::Archive Archive;
	std::string ArchiveError;
	if (!Archive.open_memory(std::vector<uint8_t>(Bytes.GetData(), Bytes.GetData() + Bytes.Num()), ArchiveError))
	{
		OutError = FString::Printf(TEXT("%s: %s"), *Key, *LvToFString(ArchiveError));
		return nullptr;
	}
	viz::GdtfResult Result = viz::load_gdtf(Archive);
	for (const viz::Diagnostic& Item : Result.diagnostics.items)
	{
		if (Item.severity == viz::Severity::Error)
		{
			UE_LOG(LogLvFixtures, Error, TEXT("%s: %s: %s"), *Key, *LvToFString(Item.where), *LvToFString(Item.message));
		}
		else
		{
			UE_LOG(LogLvFixtures, Verbose, TEXT("%s: %s: %s"), *Key, *LvToFString(Item.where), *LvToFString(Item.message));
		}
	}
	if (!Result.ok)
	{
		OutError = FString::Printf(TEXT("%s is not a usable GDTF"), *Key);
		if (!Result.diagnostics.items.empty())
		{
			OutError += TEXT(": ") + LvToFString(Result.diagnostics.items.front().message);
		}
		return nullptr;
	}

	TSharedPtr<FLvFixtureDefinition> Definition = MakeShared<FLvFixtureDefinition>();
	Definition->Source = Key;
	Definition->Type = MoveTemp(Result.fixture);
	Definition->Diagnostics = MoveTemp(Result.diagnostics);

	// The parts' 3D models, where the file packs them. A part without one is drawn as a box.
	int32 Shapes = 0;
	int32 Triangles = 0;
	Definition->ModelMeshes.SetNum(static_cast<int32>(Definition->Type.models.size()));
	for (int32 Index = 0; Index < Definition->ModelMeshes.Num(); ++Index)
	{
		const viz::Model& Model = Definition->Type.models[static_cast<size_t>(Index)];
		viz::MeshResult Shape = viz::load_model_mesh(Archive, Model);
		for (const viz::Diagnostic& Item : Shape.diagnostics.items)
		{
			UE_LOG(LogLvFixtures, Verbose, TEXT("%s: model %s: %s"), *Key, *LvToFString(Model.name), *LvToFString(Item.message));
		}
		if (!Shape.ok)
		{
			continue;
		}
		if (UStaticMesh* Built = LvBuildStaticMesh(Shape.mesh, this, GetBodyMaterial(), false, GetModelMaterials(Shape.mesh, true)))
		{
			ModelMeshes.Add(Built);
			Definition->ModelMeshes[Index] = Built;
			++Shapes;
			Triangles += static_cast<int32>(Shape.mesh.triangle_count());
		}
	}
	// The pictures on its wheels: gobos, animation wheels.
	int32 PictureCount = 0;
	Definition->WheelPictures.SetNum(static_cast<int32>(Definition->Type.wheels.size()));
	for (int32 WheelIndex = 0; WheelIndex < Definition->WheelPictures.Num(); ++WheelIndex)
	{
		const viz::Wheel& Wheel = Definition->Type.wheels[static_cast<size_t>(WheelIndex)];
		Definition->WheelPictures[WheelIndex].SetNum(static_cast<int32>(Wheel.slots.size()));
		for (int32 Slot = 0; Slot < Definition->WheelPictures[WheelIndex].Num(); ++Slot)
		{
			const viz::ImageResult Picture = viz::load_wheel_image(Archive, Wheel.slots[static_cast<size_t>(Slot)]);
			for (const viz::Diagnostic& Item : Picture.diagnostics.items)
			{
				UE_LOG(LogLvFixtures, Verbose, TEXT("%s: wheel %s: %s: %s"), *Key, *LvToFString(Wheel.name), *LvToFString(Item.where), *LvToFString(Item.message));
			}
			if (!Picture.ok)
			{
				continue;
			}
			if (UTexture2D* Texture = BuildGoboTexture(Picture.image))
			{
				Pictures.Add(Texture);
				Definition->WheelPictures[WheelIndex][Slot] = Texture;
				++PictureCount;
			}
		}
	}
	Loaded.Add(Key, Definition);
	UE_LOG(LogLvFixtures, Log, TEXT("%s: %d of %d models have a 3D shape, %d triangles; %d wheel pictures"), *Key, Shapes, Definition->ModelMeshes.Num(), Triangles, PictureCount);
	UE_LOG(LogLvFixtures, Log, TEXT("Loaded GDTF %s / %s with %d modes from %s"),
		*LvToFString(Definition->Type.manufacturer), *LvToFString(Definition->Type.name),
		static_cast<int32>(Definition->Type.modes.size()), *Key);
	return Definition;
}
