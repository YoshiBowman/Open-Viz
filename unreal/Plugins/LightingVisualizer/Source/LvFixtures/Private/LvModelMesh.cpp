#include "LvModelMesh.h"

#include "Engine/StaticMesh.h"
#include "Materials/MaterialInterface.h"
#include "PhysicsEngine/BodySetup.h"
#include "MeshDescription.h"
#include "StaticMeshAttributes.h"

UStaticMesh* LvBuildStaticMesh(const viz::Mesh& Mesh, UObject* Outer, UMaterialInterface* Material, bool bCollide, const TArray<UMaterialInterface*>& Materials)
{
	const int32 VertexCount = static_cast<int32>(Mesh.vertex_count());
	const int32 TriangleCount = static_cast<int32>(Mesh.triangle_count());
	if (VertexCount == 0 || TriangleCount == 0)
	{
		return nullptr;
	}

	FMeshDescription Description;
	FStaticMeshAttributes Attributes(Description);
	Attributes.Register();
	TVertexAttributesRef<FVector3f> Positions = Attributes.GetVertexPositions();
	TVertexInstanceAttributesRef<FVector3f> Normals = Attributes.GetVertexInstanceNormals();
	TVertexInstanceAttributesRef<FVector3f> Tangents = Attributes.GetVertexInstanceTangents();
	TVertexInstanceAttributesRef<float> BinormalSigns = Attributes.GetVertexInstanceBinormalSigns();
	TVertexInstanceAttributesRef<FVector2f> UVs = Attributes.GetVertexInstanceUVs();

	Description.ReserveNewVertices(VertexCount);
	Description.ReserveNewVertexInstances(TriangleCount * 3);
	Description.ReserveNewPolygons(TriangleCount);
	Description.ReserveNewEdges(TriangleCount * 3);

	// One section per material the mesh wears and the caller gave something for, in the file's
	// order, and after them one for whatever is drawn plain: triangles that wear nothing, or a
	// material the caller gave nothing for. Only sections that have triangles are made, so a
	// model painted throughout has its paint as its first material.
	const bool bWorn = Mesh.triangle_material.size() == static_cast<size_t>(TriangleCount);
	TArray<int32> SectionOf;    // per mesh material: its section, or -1 for the plain one
	TArray<UMaterialInterface*> SectionMaterials;
	TArray<FPolygonGroupID> Groups;
	auto Section = [&](UMaterialInterface* With, const FString& Name) -> int32
	{
		const FPolygonGroupID Group = Description.CreatePolygonGroup();
		Attributes.GetPolygonGroupMaterialSlotNames()[Group] = FName(*Name);
		Groups.Add(Group);
		SectionMaterials.Add(With);
		return Groups.Num() - 1;
	};
	TArray<bool> Used;
	Used.Init(false, static_cast<int32>(Mesh.materials.size()));
	bool bAnyPlain = !bWorn;
	for (int32 Triangle = 0; bWorn && Triangle < TriangleCount; ++Triangle)
	{
		const uint32 Worn = Mesh.triangle_material[static_cast<size_t>(Triangle)];
		if (Worn != viz::kNoMaterial && Used.IsValidIndex(static_cast<int32>(Worn)) && Materials.IsValidIndex(static_cast<int32>(Worn)) && Materials[static_cast<int32>(Worn)])
		{
			Used[static_cast<int32>(Worn)] = true;
		}
		else
		{
			bAnyPlain = true;
		}
	}
	for (int32 Index = 0; Index < Used.Num(); ++Index)
	{
		SectionOf.Add(Used[Index] ? Section(Materials[Index], FString::Printf(TEXT("Material%d"), Index)) : -1);
	}
	const int32 Plain = bAnyPlain ? Section(Material, TEXT("Body")) : -1;
	for (int32& Index : SectionOf)
	{
		Index = Index < 0 ? Plain : Index;
	}
	const bool bHasUVs = Mesh.uvs.size() == static_cast<size_t>(VertexCount) * 2;

	// Metres to centimetres, and Y mirrored, as everything from GDTF is brought across.
	TArray<FVertexID> Vertices;
	Vertices.Reserve(VertexCount);
	for (int32 Index = 0; Index < VertexCount; ++Index)
	{
		const float* P = &Mesh.positions[static_cast<size_t>(Index) * 3];
		const FVertexID Vertex = Description.CreateVertex();
		Positions[Vertex] = FVector3f(P[0] * 100.0f, -P[1] * 100.0f, P[2] * 100.0f);
		Vertices.Add(Vertex);
	}
	for (int32 Triangle = 0; Triangle < TriangleCount; ++Triangle)
	{
		FVertexInstanceID Corners[3];
		for (int32 Corner = 0; Corner < 3; ++Corner)
		{
			const uint32 Index = Mesh.triangles[static_cast<size_t>(Triangle) * 3 + Corner];
			const float* N = &Mesh.normals[static_cast<size_t>(Index) * 3];
			const FVector3f Normal(N[0], -N[1], N[2]);
			// Any direction in the surface will do for a tangent: nothing here is normal-mapped.
			FVector3f Tangent = FVector3f::CrossProduct(Normal, FMath::Abs(Normal.Z) < 0.9f ? FVector3f::UpVector : FVector3f::ForwardVector);
			Tangent.Normalize();
			Corners[Corner] = Description.CreateVertexInstance(Vertices[static_cast<int32>(Index)]);
			Normals[Corners[Corner]] = Normal;
			Tangents[Corners[Corner]] = Tangent;
			BinormalSigns[Corners[Corner]] = 1.0f;
			UVs.Set(Corners[Corner], 0, bHasUVs ? FVector2f(Mesh.uvs[static_cast<size_t>(Index) * 2], Mesh.uvs[static_cast<size_t>(Index) * 2 + 1]) : FVector2f::ZeroVector);
		}
		int32 InSection = Plain;
		if (bWorn)
		{
			const uint32 Worn = Mesh.triangle_material[static_cast<size_t>(Triangle)];
			InSection = Worn != viz::kNoMaterial && SectionOf.IsValidIndex(static_cast<int32>(Worn)) ? SectionOf[static_cast<int32>(Worn)] : Plain;
		}
		// Mirroring turns every triangle inside out, so its corners are given in the opposite order.
		Description.CreatePolygon(Groups[InSection], {Corners[0], Corners[2], Corners[1]});
	}

	UStaticMesh* StaticMesh = NewObject<UStaticMesh>(Outer);
	for (int32 Index = 0; Index < SectionMaterials.Num(); ++Index)
	{
		StaticMesh->GetStaticMaterials().Add(FStaticMaterial(SectionMaterials[Index], Attributes.GetPolygonGroupMaterialSlotNames()[Groups[Index]]));
	}
	UStaticMesh::FBuildMeshDescriptionsParams Params;
	Params.bBuildSimpleCollision = false;
	Params.bFastBuild = true;    // the path that also exists outside the editor
	Params.bMarkPackageDirty = false;
	Params.bAllowCpuAccess = bCollide;    // collision is cooked from the triangles the mesh is drawn with
	StaticMesh->BuildFromMeshDescriptions({&Description}, Params);
	if (bCollide)
	{
		if (UBodySetup* Body = StaticMesh->GetBodySetup())
		{
			Body->CollisionTraceFlag = CTF_UseComplexAsSimple;
			// From either side: models are not reliably wound one way, and a drape has no inside.
			Body->bDoubleSidedGeometry = true;
			Body->CreatePhysicsMeshes();
		}
	}
	return StaticMesh;
}
