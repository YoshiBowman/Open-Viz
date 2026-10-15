#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "LvGate.generated.h"

class ALvFixtureActor;
class UMaterialInstanceDynamic;
class UMaterialInterface;
class UTexture2D;

/**
 * What sits in a beam's path, in the numbers the gate materials take (M_LvGate and M_LvBeamGate;
 * tools/build_materials.py describes each row). Two beams with the same look share one pair of
 * materials, so every number in here is rounded before it is stored: Set() does that.
 *
 * Nothing but plain numbers and the two pictures, with no gaps between them, so that looks can be
 * compared and hashed as bytes.
 */
struct LVFIXTURES_API FLvGateLook
{
	enum ERow
	{
		Layer0A, Layer0B, Layer1A, Layer1B, Shape, Shape2, Turn,
		Blade0, Blade1, Blade2, Blade3,
		Facet0,
		RowCount = Facet0 + 16,
	};

	UTexture2D* Texture[2];
	float Values[RowCount][4];

	FLvGateLook();

	/** Rounded to 1/4096, which is finer than anything here can be seen to. */
	void Set(int32 Row, double X, double Y, double Z, double W);

	bool operator==(const FLvGateLook& Other) const { return FMemory::Memcmp(this, &Other, sizeof(FLvGateLook)) == 0; }
	bool operator!=(const FLvGateLook& Other) const { return !(*this == Other); }
	friend uint32 GetTypeHash(const FLvGateLook& Look) { return FCrc::MemCrc32(&Look, sizeof(FLvGateLook)); }
};

/**
 * Hands out the materials that draw a gobo, prism, iris, frost or framing blade: one light function
 * for what lands on surfaces and one material for the beam in haze, per distinct look.
 *
 * Why looks are shared: the engine draws every light function in use into one atlas of at most 256
 * slots, and a light whose function finds no slot loses its pattern. A rig of 500 spots all showing
 * the same gobo needs one slot this way, not 500. (A gobo that turns is turned by rolling the light
 * itself, so that does not make a new look either.)
 *
 * Fixtures ask during their tick; all requests are answered together afterwards, so that fixtures
 * changing in step keep sharing instead of each taking a look of its own for a frame.
 */
UCLASS()
class LVFIXTURES_API ULvGateSubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	/**
	 * Asks for the materials of `Look` for one beam of a fixture, or with bOpen for none. Answered
	 * later this frame through ALvFixtureActor::ApplyGate.
	 */
	void Request(ALvFixtureActor* Fixture, int32 Beam, const FLvGateLook& Look, bool bOpen);

	/** Gives a look back at once: for a fixture that is going away or being rebuilt. */
	void Release(int32 Handle);

	/** How many distinct looks are in use now. */
	int32 GetLookCount() const;

	/** How many looks have ever been made: these stay around to be used again. */
	int32 GetMaterialCount() const { return Entries.Num(); }

	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;
	virtual bool IsTickableInEditor() const override { return true; }

protected:
	virtual bool DoesSupportWorldType(const EWorldType::Type WorldType) const override;

private:
	int32 Acquire(const FLvGateLook& Look);

	struct FRequest
	{
		TWeakObjectPtr<ALvFixtureActor> Fixture;
		int32 Beam = 0;
		int32 Serial = 0;    // the fixture's when it asked: a fixture rebuilt since has other beams
		bool bOpen = true;
		FLvGateLook Look;
	};
	TArray<FRequest> Requests;

	struct FEntry
	{
		FLvGateLook Look;       // what it is asked to show, and what it is filed under
		FLvGateLook Applied;    // what its materials were last set to
		bool bApplied = false;
		int32 Users = 0;
		int32 Material = 0;     // index of its light function in Materials; the beam material follows it
	};
	TArray<FEntry> Entries;
	TMap<FLvGateLook, int32> ByLook;
	TArray<int32> Idle;    // entries nobody uses, most recently freed last

	/** Two per entry: the light function, then the beam material. */
	UPROPERTY(Transient)
	TArray<TObjectPtr<UMaterialInstanceDynamic>> Materials;

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInterface> LightBase;

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInterface> BeamBase;

	bool bBasesTried = false;
};
