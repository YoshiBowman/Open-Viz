#pragma once

#include "CoreMinimal.h"
#include "vizcore/types.h"

#include <string>

/**
 * GDTF and MVR are right-handed with Z up; Unreal is left-handed with Z up. Both are brought
 * across by mirroring Y, which is also what Epic's own MVR import does to positions, so a rig
 * spawned here lines up with scene geometry imported through the engine.
 *
 * GDTF lengths are metres, MVR lengths millimetres, Unreal's centimetres.
 */
namespace LvCoordinates
{
	/** A GDTF geometry's matrix, relative to its parent. */
	LVFIXTURES_API FTransform FromGdtf(const viz::Matrix4& Matrix);

	/** An MVR object's transform. */
	LVFIXTURES_API FTransform FromMvr(const viz::Transform& Transform);
	/** The other way, for writing a placement back out. Scale is not carried. */
	LVFIXTURES_API viz::Transform ToMvr(const FTransform& Transform);

	/** A turn of `Degrees` about a geometry's own GDTF axis: 'Z' for pan, 'X' for tilt. */
	LVFIXTURES_API FQuat AxisRotation(char Axis, double Degrees);
}

LVFIXTURES_API std::string LvToStd(const FString& Text);
LVFIXTURES_API FString LvToFString(const std::string& Text);
