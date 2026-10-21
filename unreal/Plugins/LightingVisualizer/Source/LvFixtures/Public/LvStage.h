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

	/**
	 * The floor of a stage that Build made: its colour as a share of the light it gives back (0
	 * black, 1 white; 0.14 unless set) and how rough it is (0 a mirror, 1 matte; 0.8 unless set).
	 * A smooth floor shows what is lit and the lenses in it, as a glossy stage floor does.
	 */
	LVFIXTURES_API void SetFloor(UWorld& World, float Shade, float Roughness);

	/**
	 * How bright parts of the picture glow into what is around them (the engine's bloom: 0 none,
	 * 0.675 its own default), and whether what moves fast is blurred along its path. Without
	 * `bLens`, nothing a camera's lens adds: no darkening toward the corners and no flares, which
	 * would show as seams where views taken one beside the other are joined.
	 */
	LVFIXTURES_API void SetLook(UWorld& World, float Bloom, bool bMotionBlur, bool bLens = true);

	/** Points the camera and makes it the player's view; hides the default pawn. */
	LVFIXTURES_API void Aim(UWorld& World, ACameraActor* Camera, const FVector& Location, const FVector& Target);
}
