#pragma once

#include "CoreMinimal.h"

class AActor;
class ALvFixtureActor;
class UMaterialInstanceDynamic;
class UTexture2D;
class UWorld;

/**
 * Light levels on stage as a light meter would read them, while watching (the visualizer's M key,
 * or Openviz's Light Levels): what the beams give now, in lux, on the floor and on whatever stands
 * on it (a deck, a riser), drawn over it in false colour, and read under the pointer.
 *
 * The area is the stage's and a little more, sampled at the points of a grid of at most 161 by
 * 161. Where each point lies is found once, when the meter is switched on, by looking down from
 * 3.5 m above the floor (so flown truss is passed over, and people standing about are not taken
 * for surfaces). Each beam counts by its candela: full within its beam angle and fading to
 * nothing at its field angle, by the inverse square of the distance and by the cosine of the
 * angle the light meets the surface at. Each beam is followed to every point it reaches, so what
 * stands in the way (scenery, a set piece, a person) shades it; fixtures' own bodies, gobos and
 * prisms are not allowed for. The work is spread over frames, a few thousand traces each, and the
 * overlay shows each pass when it is whole.
 */
class LVFIXTURES_API FLvLightMeter
{
public:
	~FLvLightMeter();

	bool IsOn() const { return Overlay.IsValid(); }
	/** Lays the grid over `Stage` (Unreal centimetres) and shows the overlay. `NotSurfaces`: actors that stand on the floor but are not to be read on (people). */
	void Start(UWorld& World, const FBox& Stage, const TArray<AActor*>& NotSurfaces);
	void Stop();
	/** Does a share of a pass over the lit beams of `Fixtures`; when a pass is whole, the overlay shows it. */
	void Tick(UWorld& World, const TArray<TWeakObjectPtr<ALvFixtureActor>>& Fixtures);

	/** Lux at a place within the grid, from the last whole pass, between its points; negative outside it or before the first pass. */
	double ReadingAt(const FVector& Where) const;
	/** The top of the colour scale (white): 1, 2 or 5 times a power of ten, at or above the light on 98 in 100 of the lit points (more is white too). */
	double ScaleTop() const { return Top; }
	double Highest() const { return Peak; }
	bool HasReading() const { return Passes > 0; }
	int32 GetAcross() const { return Across; }
	int32 GetDown() const { return Down; }
	double GetCell() const { return Cell; }

private:
	void Show();

	TWeakObjectPtr<AActor> Overlay;
	TWeakObjectPtr<UTexture2D> Texture;
	TWeakObjectPtr<UMaterialInstanceDynamic> Material;
	FVector2D Origin = FVector2D::ZeroVector;    // the first point, Unreal centimetres
	double Cell = 50.0;                          // between points
	int32 Across = 0;
	int32 Down = 0;
	TArray<FVector> Points;                      // where each point lies, on the surface found there
	TArray<FVector> Normals;
	TArray<double> Values;                       // lux, from the last whole pass
	TArray<double> Next;                         // the pass under way
	TArray<TWeakObjectPtr<ALvFixtureActor>> Queue;    // the fixtures still to do in this pass
	bool bPass = false;                          // a pass is under way
	double PassStarted = -1.0;                   // the world's time it began
	double Lowest = 0.0;                         // the lowest point found, Unreal centimetres
	double Top = 100.0;
	double Peak = 0.0;
	int64 Passes = 0;
public:
	/** What the last whole pass did: beams counted, points they reached, traces, traces stopped by something in the way. */
	int32 LastBeams = 0;
	int32 LastReached = 0;
	int32 LastTraces = 0;
	int32 LastBlocked = 0;
private:
	int32 Beams = 0;
	int32 Reached = 0;
	int32 TracesMade = 0;
	int32 Blocked = 0;
};
