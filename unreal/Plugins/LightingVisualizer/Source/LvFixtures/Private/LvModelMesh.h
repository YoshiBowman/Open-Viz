#pragma once

#include "CoreMinimal.h"
#include "vizcore/mesh.h"

class UMaterialInterface;
class UStaticMesh;

/**
 * Turns a fixture part's mesh, as the core read it from the GDTF (metres, right-handed, Z up),
 * into a static mesh Unreal can draw (centimetres, Y mirrored). Built once per part of a fixture
 * type and shared by every fixture of that type. Works at runtime, in a packaged build too.
 *
 * With bCollide the mesh can be hit by line traces, triangle for triangle: for scenery, which
 * beams have to land on. (In a packaged build that needs the engine's runtime collision cooking;
 * it has only been run from the editor's build.)
 */
/**
 * `Materials` are the materials to draw the mesh's own materials with, one per entry of
 * viz::Mesh::materials (null for any that is to be drawn plain); triangles that wear none, and
 * a mesh without any, are drawn with `Material`. Texture coordinates come across where the file
 * has them.
 */
UStaticMesh* LvBuildStaticMesh(const viz::Mesh& Mesh, UObject* Outer, UMaterialInterface* Material, bool bCollide = false,
	const TArray<UMaterialInterface*>& Materials = TArray<UMaterialInterface*>());
