#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "LvScreenActor.generated.h"

class UMaterialInstanceDynamic;
class UMediaPlayer;
class UMediaTexture;
class UStaticMeshComponent;
class UTexture;
class UTexture2D;

/**
 * The picture on a video screen or an LED wall: a glowing rectangle that shows a test pattern, a
 * still picture, a film from a file, a live NDI stream, or what arrives pixel by pixel over DMX.
 *
 * The actor sits in the middle of the picture: its X runs to the picture's right, its Y down it,
 * and its Z out of it toward whoever is looking. Like everything else of a show it is rebuilt
 * each time the show is opened; what it shows is chosen in Openviz.
 */
UCLASS()
class LVFIXTURES_API ALvScreenActor : public AActor
{
	GENERATED_BODY()

public:
	ALvScreenActor();

	/** What the rig file or Openviz calls it, the id it is known by, and where the rig file files it. For reading. */
	UPROPERTY(VisibleAnywhere, Category = "Openviz")
	FString ScreenName;

	UPROPERTY(VisibleAnywhere, Category = "Openviz")
	FString ScreenId;

	UPROPERTY(VisibleAnywhere, Category = "Openviz")
	FString Layer;

	UPROPERTY(VisibleAnywhere, Category = "Openviz")
	FString Class;

	/**
	 * "pattern", "picture", "film", "ndi", "pixels" or "off", and what goes with it: a file, a
	 * stream's name, or for pixels where they are patched and how many there are ("20.1 40x10 GRB
	 * snake": see vizcore/screens.h). Two more draw nothing at all and stop no beam: "fixtures",
	 * for a screen that stands where fixtures hang and leaves the place to them, and "sections",
	 * for a screen whose sections are screens of their own.
	 */
	UPROPERTY(VisibleAnywhere, Category = "Openviz")
	FString Source;

	UPROPERTY(VisibleAnywhere, Category = "Openviz")
	FString SourceValue;

	/** The luminance of the picture's white, in nits. */
	UPROPERTY(VisibleAnywhere, Category = "Openviz")
	float Nits = 1000.0f;

	/** Width and height of the picture, in centimetres. */
	UPROPERTY(VisibleAnywhere, Category = "Openviz")
	FVector2D Size = FVector2D::ZeroVector;

	/** Makes the rectangle. A screen that is not in the rig file as a model stops beams itself. */
	void Build(double WidthCm, double HeightCm, bool bStopsBeams);

	/** Puts a picture on it. An unknown kind, or a file that cannot be shown, leaves the test pattern up. */
	void Show(const FString& Kind, const FString& Value, float InNits);

	/**
	 * Which part of its picture it shows: the left, top, right and bottom edge as shares of the
	 * whole, (0, 0, 1, 1) for all of it. So one film or stream can be spread over several screens
	 * or over the sections of one. The test pattern and DMX pixels are always shown whole.
	 */
	void SetPart(const FVector4& Edges);
	FVector4 GetPart() const { return Part; }

	/**
	 * How its picture is doing, in words for the log: what it shows, or why it does not. A film or
	 * a stream takes a moment to open, so this changes after Show.
	 */
	FString Describe() const;

	/** True once Describe will not change by itself any more: the picture is up, or it failed. */
	bool IsSettled() const;

	/**
	 * Levels to show instead of what arrives on a universe, for a screen lit pixel by pixel: for
	 * checks, and for lighting one up without a console. An empty array takes them away again.
	 */
	void SetPixelOverride(int32 Universe, TConstArrayView<uint8> Slots);

	/** For a screen lit pixel by pixel: how many pixels it has each way, and their colours as last drawn (four bytes each, from the top left). */
	FIntPoint GetPixelCount() const;
	const TArray<uint8>& GetPixelLevels() const { return PixelLevels; }

	virtual void Tick(float DeltaSeconds) override;

	/** The texture it shows, or null while dark. */
	UTexture* GetPicture() const { return Picture; }

	/** A test pattern for a picture of this shape: bars, a grid, a circle, and a mark in each corner that tells them apart. */
	static UTexture2D* MakeTestPattern(UObject* Outer, double Aspect);

	virtual void EndPlay(const EEndPlayReason::Type Reason) override;

private:
	UPROPERTY(Transient)
	TObjectPtr<UStaticMeshComponent> Surface;

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> Material;

	UPROPERTY(Transient)
	TObjectPtr<UTexture> Picture;

	UPROPERTY(Transient)
	TObjectPtr<UMediaPlayer> Player;

	UPROPERTY(Transient)
	TObjectPtr<UMediaTexture> Film;

	/** Lit pixel by pixel: the picture the levels are drawn into, the map from slots to pixels, and what was drawn last. */
	UPROPERTY(Transient)
	TObjectPtr<UTexture2D> PixelTexture;

	struct FPixels;
	TSharedPtr<FPixels> Pixels;
	TArray<uint8> PixelLevels;

	/** The film or stream it has taken from ULvScreenFeeds, to give back. */
	FString HeldKind;
	FString HeldValue;
	void GiveFeedBack();

	FVector4 Part = FVector4(0.0, 0.0, 1.0, 1.0);
	bool bStops = false;    // whether its own rectangle is what stops beams

	/** Why the picture asked for is not up, if it is not. */
	FString Trouble;

	void SetPicture(UTexture* Texture, float Brightness);
};
