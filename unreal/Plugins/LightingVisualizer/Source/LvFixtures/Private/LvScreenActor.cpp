#include "LvScreenActor.h"

#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture2D.h"
#include "ImageUtils.h"
#include "LvCoordinates.h"
#include "LvDmxSubsystem.h"
#include "LvScreenFeeds.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "MediaPlayer.h"
#include "MediaTexture.h"
#include "Misc/Paths.h"
#include "vizcore/screens.h"

// A screen lit pixel by pixel: which slots are whose, and levels given in place of what arrives.
struct ALvScreenActor::FPixels
{
	viz::PixelMap Map;
	TMap<int32, TArray<uint8>> Override;
	TWeakObjectPtr<ULvDmxSubsystem> Dmx;
	int32 Arriving = 0;

	static const uint8_t* Lookup(int Universe, void* Context)
	{
		FPixels* Self = static_cast<FPixels*>(Context);
		if (const TArray<uint8>* Given = Self->Override.Find(Universe))
		{
			return Given->GetData();
		}
		return Self->Dmx.IsValid() ? Self->Dmx->GetUniverse(Universe) : nullptr;
	}
};

ALvScreenActor::ALvScreenActor()
{
	// Only a screen lit pixel by pixel has something to do each frame.
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.bStartWithTickEnabled = false;
	RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	RootComponent->SetMobility(EComponentMobility::Movable);
}

void ALvScreenActor::Build(double WidthCm, double HeightCm, bool bStopsBeams)
{
	Size = FVector2D(WidthCm, HeightCm);
	bStops = bStopsBeams;
	UStaticMesh* Plane = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Plane.Plane"));
	UMaterialInterface* Base = LoadObject<UMaterialInterface>(nullptr, TEXT("/LightingVisualizer/Materials/M_LvScreen.M_LvScreen"));
	if (!Plane || !Base)
	{
		Trouble = TEXT("the screen material is missing: run tools/build_materials.py");
		return;
	}
	Surface = NewObject<UStaticMeshComponent>(this, TEXT("Picture"));
	Surface->SetMobility(EComponentMobility::Movable);
	Surface->SetStaticMesh(Plane);
	Surface->SetCastShadow(false);
	Surface->SetCollisionProfileName(bStopsBeams ? TEXT("BlockAll") : TEXT("NoCollision"));
	Surface->SetupAttachment(RootComponent);
	// The engine's plane is a metre square, lying in its own X and Y. A hair out from the screen's
	// face, so that the cabinet the rig file draws does not show through.
	Surface->SetRelativeScale3D(FVector(WidthCm / 100.0, HeightCm / 100.0, 1.0));
	Surface->SetRelativeLocation(FVector(0.0, 0.0, 0.3));
	Surface->RegisterComponent();
	AddInstanceComponent(Surface);

	Material = UMaterialInstanceDynamic::Create(Base, this);
	Surface->SetMaterial(0, Material);
	SetPicture(nullptr, 0.0f);
}

void ALvScreenActor::SetPicture(UTexture* Texture, float Brightness)
{
	Picture = Texture;
	if (Material)
	{
		if (Texture)
		{
			Material->SetTextureParameterValue(TEXT("Picture"), Texture);
		}
		Material->SetScalarParameterValue(TEXT("Nits"), Texture ? Brightness : 0.0f);
		// The part of the picture: its left and top edge, its width and its height.
		const bool bWhole = Source == TEXT("pattern") || Source == TEXT("pixels") || !Trouble.IsEmpty();
		Material->SetVectorParameterValue(TEXT("Part"), bWhole ? FLinearColor(0.0f, 0.0f, 1.0f, 1.0f)
			: FLinearColor(static_cast<float>(Part.X), static_cast<float>(Part.Y), static_cast<float>(Part.Z - Part.X), static_cast<float>(Part.W - Part.Y)));
	}
}

void ALvScreenActor::SetPart(const FVector4& Edges)
{
	FVector4 Wanted(FMath::Clamp(Edges.X, 0.0, 1.0), FMath::Clamp(Edges.Y, 0.0, 1.0), FMath::Clamp(Edges.Z, 0.0, 1.0), FMath::Clamp(Edges.W, 0.0, 1.0));
	if (Wanted.Z - Wanted.X < 1e-4 || Wanted.W - Wanted.Y < 1e-4)
	{
		Wanted = FVector4(0.0, 0.0, 1.0, 1.0);    // no part at all: the whole
	}
	Part = Wanted;
	SetPicture(Picture, Nits);
}

void ALvScreenActor::GiveFeedBack()
{
	if (!HeldKind.IsEmpty())
	{
		if (ULvScreenFeeds* Feeds = GetWorld() ? GetWorld()->GetSubsystem<ULvScreenFeeds>() : nullptr)
		{
			Feeds->GiveBack(HeldKind, HeldValue);
		}
		HeldKind.Reset();
		HeldValue.Reset();
	}
	Player = nullptr;
	Film = nullptr;
}

void ALvScreenActor::Show(const FString& Kind, const FString& Value, float InNits)
{
	if (Kind == Source && Value == SourceValue && Trouble.IsEmpty() && Picture && Material && Kind != TEXT("off"))
	{
		// The same picture, perhaps brighter or dimmer: a film is not started again for that.
		Nits = FMath::Clamp(InNits, 0.0f, 20000.0f);
		Material->SetScalarParameterValue(TEXT("Nits"), Nits);
		return;
	}
	Source = Kind;
	SourceValue = Value;
	Nits = FMath::Clamp(InNits, 0.0f, 20000.0f);
	Trouble.Reset();
	GiveFeedBack();
	const TMap<int32, TArray<uint8>> KeptOverride = Pixels ? Pixels->Override : TMap<int32, TArray<uint8>>();
	Pixels.Reset();
	PixelLevels.Reset();
	SetActorTickEnabled(false);
	if (!Material)
	{
		return;
	}
	const double Aspect = Size.Y > 0.0 ? Size.X / Size.Y : 16.0 / 9.0;

	// A screen that leaves its place to the fixtures hanging there, or to its own sections, is not
	// there at all: not drawn, and no beam ends on it.
	const bool bGone = Kind == TEXT("fixtures") || Kind == TEXT("sections");
	if (Surface)
	{
		Surface->SetVisibility(!bGone);
		Surface->SetCollisionProfileName(bStops && !bGone ? TEXT("BlockAll") : TEXT("NoCollision"));
	}
	if (Kind == TEXT("off") || bGone)
	{
		SetPicture(nullptr, 0.0f);
		return;
	}
	if (Kind == TEXT("picture"))
	{
		if (UTexture2D* Still = FPaths::FileExists(Value) ? FImageUtils::ImportFileAsTexture2D(Value) : nullptr)
		{
			SetPicture(Still, Nits);
			return;
		}
		Trouble = FString::Printf(TEXT("the picture %s cannot be read"), *FPaths::GetCleanFilename(Value));
	}
	else if (Kind == TEXT("film") || Kind == TEXT("ndi"))
	{
		// Taken from the films and streams that are open: another screen, or another section of this
		// one, may be showing the same.
		ULvScreenFeeds* Feeds = GetWorld() ? GetWorld()->GetSubsystem<ULvScreenFeeds>() : nullptr;
		UMediaPlayer* Playing = nullptr;
		UMediaTexture* Shown = nullptr;
		if (Feeds && Feeds->Take(Kind, Value, Playing, Shown))
		{
			Player = Playing;
			Film = Shown;
			HeldKind = Kind;
			HeldValue = Value;
			SetPicture(Film, Nits);
			return;
		}
		Trouble = Kind == TEXT("ndi") ? FString::Printf(TEXT("the NDI stream %s cannot be opened"), *Value)
			: FString::Printf(TEXT("the film %s cannot be opened"), *FPaths::GetCleanFilename(Value));
	}
	else if (Kind == TEXT("pixels"))
	{
		viz::PixelMap Map;
		std::string Error;
		if (viz::parse_pixel_map(LvToStd(Value), Map, Error))
		{
			// One texel a pixel, drawn as blocks: an LED wall is not a blurred picture.
			PixelTexture = UTexture2D::CreateTransient(Map.across, Map.down, PF_B8G8R8A8);
			if (PixelTexture && PixelTexture->GetPlatformData() && PixelTexture->GetPlatformData()->Mips.Num() > 0)
			{
				PixelTexture->SRGB = true;
				PixelTexture->NeverStream = true;    // or the engine declines to redraw parts of it: it only says so in its log
				PixelTexture->Filter = TF_Nearest;
				PixelTexture->AddressX = TA_Clamp;
				PixelTexture->AddressY = TA_Clamp;
				void* Data = PixelTexture->GetPlatformData()->Mips[0].BulkData.Lock(LOCK_READ_WRITE);
				FMemory::Memzero(Data, static_cast<SIZE_T>(Map.pixels()) * 4);
				PixelTexture->GetPlatformData()->Mips[0].BulkData.Unlock();
				PixelTexture->UpdateResource();
				Pixels = MakeShared<FPixels>();
				Pixels->Map = Map;
				Pixels->Override = KeptOverride;
				Pixels->Dmx = GetWorld() ? GetWorld()->GetSubsystem<ULvDmxSubsystem>() : nullptr;
				ULvDmxSubsystem::CoverUniverses(Map.universe + Map.universes() - 1);
				SetPicture(PixelTexture, Nits);
				SetActorTickEnabled(true);
				return;
			}
			Trouble = TEXT("the picture for its pixels could not be made");
		}
		else
		{
			Trouble = FString::Printf(TEXT("\"%s\" is not a pixel map: %s"), *Value, *LvToFString(Error));
		}
	}
	else if (Kind != TEXT("pattern"))
	{
		Trouble = FString::Printf(TEXT("\"%s\" is not something a screen can show"), *Kind);
	}
	SetPicture(MakeTestPattern(this, Aspect), Nits);
}

void ALvScreenActor::SetPixelOverride(int32 Universe, TConstArrayView<uint8> Slots)
{
	if (!Pixels)
	{
		return;
	}
	if (Slots.Num() == 0)
	{
		Pixels->Override.Remove(Universe);
		return;
	}
	TArray<uint8>& Kept = Pixels->Override.FindOrAdd(Universe);
	Kept.Init(0, 512);
	FMemory::Memcpy(Kept.GetData(), Slots.GetData(), FMath::Min(Slots.Num(), 512));
}

FIntPoint ALvScreenActor::GetPixelCount() const
{
	return Pixels ? FIntPoint(Pixels->Map.across, Pixels->Map.down) : FIntPoint::ZeroValue;
}

void ALvScreenActor::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	if (!Pixels || !PixelTexture)
	{
		return;
	}
	std::vector<uint8_t> Read;
	Pixels->Arriving = viz::read_pixels(Pixels->Map, &FPixels::Lookup, Pixels.Get(), Read);
	const int32 Count = static_cast<int32>(Read.size());
	if (Count == PixelLevels.Num() && FMemory::Memcmp(Read.data(), PixelLevels.GetData(), Count) == 0)
	{
		return;    // as it was: nothing to draw
	}
	PixelLevels.SetNumUninitialized(Count);
	FMemory::Memcpy(PixelLevels.GetData(), Read.data(), Count);
	// The texture wants blue first; the copy is handed over and freed once it has been drawn.
	uint8* Copy = static_cast<uint8*>(FMemory::Malloc(Count));
	for (int32 At = 0; At + 3 < Count; At += 4)
	{
		Copy[At] = Read[At + 2];
		Copy[At + 1] = Read[At + 1];
		Copy[At + 2] = Read[At];
		Copy[At + 3] = 255;
	}
	const int32 Across = Pixels->Map.across;
	FUpdateTextureRegion2D* Region = new FUpdateTextureRegion2D(0, 0, 0, 0, Across, Pixels->Map.down);
	PixelTexture->UpdateTextureRegions(0, 1, Region, Across * 4, 4, Copy,
		[](uint8* Data, const FUpdateTextureRegion2D* Regions) { FMemory::Free(Data); delete Regions; });
}

bool ALvScreenActor::IsSettled() const
{
	if (!Trouble.IsEmpty() || !Player || (Source != TEXT("film") && Source != TEXT("ndi")))
	{
		return true;
	}
	if (Player->HasError() || (!Player->IsPreparing() && !Player->IsBuffering() && Player->GetUrl().IsEmpty()))
	{
		return true;
	}
	// Playing, and far enough in to know how big the picture is.
	return Player->IsPlaying() && Player->GetVideoTrackDimensions(INDEX_NONE, INDEX_NONE).X > 0;
}

FString ALvScreenActor::Describe() const
{
	if (!Trouble.IsEmpty())
	{
		return Trouble + TEXT(": showing the test pattern");
	}
	if (Source == TEXT("off"))
	{
		return TEXT("dark");
	}
	if (Source == TEXT("fixtures"))
	{
		return TEXT("showing the fixtures that hang there");
	}
	if (Source == TEXT("sections"))
	{
		return TEXT("showing its sections");
	}
	if (Source == TEXT("pattern"))
	{
		return TEXT("showing the test pattern");
	}
	if (Source == TEXT("picture"))
	{
		return FString::Printf(TEXT("showing %s"), *FPaths::GetCleanFilename(SourceValue));
	}
	if (Source == TEXT("pixels") && Pixels)
	{
		const viz::PixelMap& Map = Pixels->Map;
		const int32 Universes = Map.universes();
		const FString Patched = Universes == 1 ? FString::Printf(TEXT("universe %d"), Map.universe)
			: FString::Printf(TEXT("universes %d to %d"), Map.universe, Map.universe + Universes - 1);
		const FString Heard = Pixels->Arriving == 0 ? TEXT("nothing is arriving there yet")
			: Pixels->Arriving == Universes ? TEXT("DMX is arriving") : FString::Printf(TEXT("DMX is arriving on %d of them"), Pixels->Arriving);
		return FString::Printf(TEXT("lit pixel by pixel, %d x %d, from %s: %s"), Map.across, Map.down, *Patched, *Heard);
	}
	const FString What = Source == TEXT("ndi") ? FString::Printf(TEXT("the NDI stream %s"), *SourceValue) : FPaths::GetCleanFilename(SourceValue);
	if (!Player)
	{
		return FString::Printf(TEXT("has no player for %s"), *What);
	}
	if (Player->HasError() || (!Player->IsPreparing() && Player->GetUrl().IsEmpty()))
	{
		return FString::Printf(TEXT("cannot play %s"), *What);
	}
	if (Player->IsPlaying())
	{
		const FIntPoint Frame = Player->GetVideoTrackDimensions(INDEX_NONE, INDEX_NONE);
		return Frame.X > 0 ? FString::Printf(TEXT("playing %s, %d x %d"), *What, Frame.X, Frame.Y) : FString::Printf(TEXT("playing %s"), *What);
	}
	return Source == TEXT("ndi") ? FString::Printf(TEXT("waiting for %s: nobody is sending it yet"), *What) : FString::Printf(TEXT("opening %s"), *What);
}

void ALvScreenActor::EndPlay(const EEndPlayReason::Type Reason)
{
	GiveFeedBack();
	Super::EndPlay(Reason);
}

UTexture2D* ALvScreenActor::MakeTestPattern(UObject* Outer, double Aspect)
{
	Aspect = FMath::Clamp(Aspect, 0.05, 20.0);
	// About a megapixel whatever the shape, and never a side under 64.
	const int32 Width = FMath::Clamp(FMath::RoundToInt32(1024.0 * FMath::Sqrt(Aspect)), 64, 4096);
	const int32 Height = FMath::Clamp(FMath::RoundToInt32(Width / Aspect), 64, 4096);
	TArray<FColor> Pixels;
	Pixels.Init(FColor(22, 22, 26), Width * Height);
	auto Rect = [&](int32 X0, int32 Y0, int32 X1, int32 Y1, FColor Colour)
	{
		for (int32 Y = FMath::Max(Y0, 0); Y < FMath::Min(Y1, Height); ++Y)
		{
			for (int32 X = FMath::Max(X0, 0); X < FMath::Min(X1, Width); ++X)
			{
				Pixels[Y * Width + X] = Colour;
			}
		}
	};
	const int32 Unit = FMath::Max(FMath::Min(Width, Height) / 8, 8);    // the grid's squares
	const int32 Line = FMath::Max(Unit / 40, 1);

	// A grid of squares, counted from the middle so that it is the same on both sides.
	for (int32 X = Width / 2 % Unit; X < Width; X += Unit)
	{
		Rect(X - Line / 2, 0, X - Line / 2 + Line, Height, FColor(120, 120, 128));
	}
	for (int32 Y = Height / 2 % Unit; Y < Height; Y += Unit)
	{
		Rect(0, Y - Line / 2, Width, Y - Line / 2 + Line, FColor(120, 120, 128));
	}

	// Eight bars across the middle: white, yellow, cyan, green, magenta, red, blue, black.
	static const FColor Bars[8] = {FColor(235, 235, 235), FColor(235, 235, 16), FColor(16, 235, 235), FColor(16, 235, 16),
	                               FColor(235, 16, 235), FColor(235, 16, 16), FColor(16, 16, 235), FColor(0, 0, 0)};
	const int32 BarsLeft = Width / 8, BarsWide = Width * 3 / 4;
	for (int32 Bar = 0; Bar < 8; ++Bar)
	{
		Rect(BarsLeft + BarsWide * Bar / 8, Height * 3 / 8, BarsLeft + BarsWide * (Bar + 1) / 8, Height * 5 / 8, Bars[Bar]);
	}
	// Under them a ramp from black to white.
	for (int32 X = 0; X < BarsWide; ++X)
	{
		const uint8 Level = static_cast<uint8>(255 * X / FMath::Max(BarsWide - 1, 1));
		Rect(BarsLeft + X, Height * 5 / 8, BarsLeft + X + 1, Height * 11 / 16, FColor(Level, Level, Level));
	}

	// A circle that is only round if the picture is not squeezed.
	const double Radius = FMath::Min(Width, Height) * 0.46;
	for (int32 Step = 0; Step < 4096; ++Step)
	{
		const double Angle = 2.0 * UE_DOUBLE_PI * Step / 4096.0;
		const int32 X = FMath::RoundToInt32(Width / 2.0 + Radius * FMath::Cos(Angle));
		const int32 Y = FMath::RoundToInt32(Height / 2.0 + Radius * FMath::Sin(Angle));
		Rect(X - Line, Y - Line, X + Line + 1, Y + Line + 1, FColor(235, 235, 235));
	}

	// The edge, and a block in each corner: red top left, green top right, blue bottom left, white
	// bottom right. With them a picture that is mirrored or upside down gives itself away.
	const int32 Edge = Line * 2;
	Rect(0, 0, Width, Edge, FColor::White);
	Rect(0, Height - Edge, Width, Height, FColor::White);
	Rect(0, 0, Edge, Height, FColor::White);
	Rect(Width - Edge, 0, Width, Height, FColor::White);
	const int32 Block = Unit * 3 / 4;
	Rect(Edge, Edge, Edge + Block, Edge + Block, FColor(235, 16, 16));
	Rect(Width - Edge - Block, Edge, Width - Edge, Edge + Block, FColor(16, 235, 16));
	Rect(Edge, Height - Edge - Block, Edge + Block, Height - Edge, FColor(16, 16, 235));
	Rect(Width - Edge - Block, Height - Edge - Block, Width - Edge, Height - Edge, FColor(235, 235, 235));
	// And an arrow that points up, above the bars.
	const int32 Tip = Height / 8, Shaft = Height * 5 / 16;
	for (int32 Y = Tip; Y < Shaft; ++Y)
	{
		const int32 Half = Y < Tip + Unit / 2 ? (Y - Tip) : Unit / 8;
		Rect(Width / 2 - Half, Y, Width / 2 + Half + 1, Y + 1, FColor(235, 235, 235));
	}

	UTexture2D* Texture = UTexture2D::CreateTransient(Width, Height, PF_B8G8R8A8);
	if (!Texture || !Texture->GetPlatformData() || Texture->GetPlatformData()->Mips.Num() == 0)
	{
		return nullptr;
	}
	Texture->SRGB = true;
	void* Data = Texture->GetPlatformData()->Mips[0].BulkData.Lock(LOCK_READ_WRITE);
	FMemory::Memcpy(Data, Pixels.GetData(), Pixels.Num() * sizeof(FColor));
	Texture->GetPlatformData()->Mips[0].BulkData.Unlock();
	Texture->UpdateResource();
	return Texture;
}
