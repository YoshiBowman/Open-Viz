#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "LvDmxSubsystem.generated.h"

/**
 * Raw DMX universes as the engine's DMX Protocol plugin last received them, on any input port.
 * Fixtures read whole universes from here and decode them with the GDTF data, rather than going
 * through DMX Library patches.
 */
UCLASS()
class LVFIXTURES_API ULvDmxSubsystem : public UWorldSubsystem
{
	GENERATED_BODY()

public:
	/**
	 * The 512 slots of a universe, or nullptr if nothing has been received for it.
	 * The pointer is good until the end of the frame.
	 */
	const uint8* GetUniverse(int32 Universe);

	/**
	 * Adds an input port at runtime. Ports normally come from Project Settings; this is for tools
	 * and tests that need one whatever the project has configured. `Protocol` is "Art-Net" or "sACN".
	 */
	static void AddInputPort(FName Protocol, const FString& DeviceAddress, int32 FirstUniverse, int32 NumUniverses);

	/**
	 * Sees to it that universes up to `Last` are received: where the port added last stops short
	 * of it, another is added beside it for the rest, on the same protocol and address. Asked
	 * before any port was added, it is remembered and seen to when one is. For what the fixtures'
	 * patch does not cover, such as a screen lit pixel by pixel on universes of its own.
	 */
	static void CoverUniverses(int32 Last);

	/**
	 * Levels Openviz gives itself in place of what arrives, as when a recording is rendered to
	 * video: a universe given here is read from here and not from the network. With `bOnly`, a
	 * universe not given reads as nothing has arrived, so the network has no say at all. Game
	 * thread only. Clear when done.
	 */
	static void Provide(int32 Universe, const uint8* Levels, int32 Count);
	static void ProvideOnly(bool bOnly);
	static void ClearProvided();

private:
	struct FCache;
	TSharedPtr<FCache> Cache;
};
