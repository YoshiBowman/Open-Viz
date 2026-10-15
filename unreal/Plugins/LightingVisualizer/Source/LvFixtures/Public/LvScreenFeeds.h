#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "LvScreenFeeds.generated.h"

class UMediaPlayer;
class UMediaTexture;

USTRUCT()
struct FLvScreenFeed
{
	GENERATED_BODY()

	UPROPERTY(Transient)
	TObjectPtr<UMediaPlayer> Player;

	UPROPERTY(Transient)
	TObjectPtr<UMediaTexture> Texture;

	int32 Users = 0;
};

/**
 * The films and NDI streams that screens show, each opened once however many screens and sections
 * of screens show it or a part of it. One stream spread over eight sections is then received and
 * decoded once, not eight times, and the sections of a film stay in step.
 */
UCLASS()
class LVFIXTURES_API ULvScreenFeeds : public UWorldSubsystem
{
	GENERATED_BODY()

public:
	/**
	 * A film ("film", a file's path) or a stream ("ndi", its name) as it plays. False if it cannot
	 * even be asked for: no such file, no name. Whoever takes one gives it back when done with it.
	 */
	bool Take(const FString& Kind, const FString& Value, UMediaPlayer*& OutPlayer, UMediaTexture*& OutTexture);
	void GiveBack(const FString& Kind, const FString& Value);

	/** How many films and streams are open. For checks. */
	int32 Count() const { return Feeds.Num(); }

	virtual void Deinitialize() override;

private:
	UPROPERTY(Transient)
	TMap<FString, FLvScreenFeed> Feeds;
};
