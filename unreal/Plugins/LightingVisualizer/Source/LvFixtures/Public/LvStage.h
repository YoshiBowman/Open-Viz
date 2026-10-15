#pragma once

#include "CoreMinimal.h"

class ACameraActor;
class UWorld;

/** A bare stage for benchmarks and checks: floor, haze, fixed exposure and a camera. */
namespace LvStage
{
	/**
	 * `ExposureCompensation` is in stops. Fixtures built from GDTF data are photometric (a
	 * 46,500 lumen profile really is that bright), so they need about -8; the engine's stock
	 * fixtures use small arbitrary units and need 0.
	 *
	 * Also reads -LvBeamRes=<25..100> from the command line: the resolution beams in haze are
	 * drawn at, as a percentage of the picture's (default 100).
	 */
	LVFIXTURES_API ACameraActor* Build(UWorld& World, float ExposureCompensation);

	/** Sets the exposure compensation of a stage that Build made, in stops. */
	LVFIXTURES_API void SetExposure(UWorld& World, float ExposureCompensation);

	/** Points the camera and makes it the player's view; hides the default pawn. */
	LVFIXTURES_API void Aim(UWorld& World, ACameraActor* Camera, const FVector& Location, const FVector& Target);
}
