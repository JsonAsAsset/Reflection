/* Copyright Reflection Contributors 2024-2026 */

#pragma once

#include "Importers/Constructor/Importer.h"

class USkeletalMesh;
class USkeleton;
class UPoseAsset;

/* Builds a skeletal mesh from the cooked geometry.
 *
 * The export carries the properties, the material slots and what every LOD is made of, and none of
 * the geometry: vertices, indices and skin weights live in the render buffers, which come down from
 * the Cloud the same way a static mesh's do. What the mesh is posed against is a skeleton of its
 * own, imported first, since a mesh without a reference pose has nothing to skin to.
 *
 * Sockets, section flags and skin weight profiles are the Skeletal Mesh Data tool's, and the
 * geometry is the Mesh Geometry tool's. Both run against an asset that already exists, which is
 * what this makes, so the import hands over to them rather than spelling either out again. */
class ISkeletalMeshImporter : public IImporter {
public:
	virtual UObject* CreateAsset(UObject* CreatedAsset) override;
	virtual bool Import() override;

private:
	/* The skeleton the mesh is skinned to, imported through the Cloud when the project doesn't
	 * have it. Null when it could not be reached, which is the end of the import. */
	USkeleton* ResolveSkeleton();

	/* One slot per exported material, named before the geometry arrives: a wedge names the slot it
	 * belongs to, so the names have to be there for the sections to land on the right material.
	 * The references themselves are filled in later, by the skeletal mesh data tool. */
	static void BuildMaterialSlots(USkeletalMesh* SkeletalMesh, const TArray<TSharedPtr<FJsonValue>>& Slots);

	/* Writes the morphs the cook quantized into its GPU buffers back into the imported data,
	 * returning how many were given anything to move. Runs before the build, which is what turns
	 * them into morph targets: anything set on the mesh itself is dropped the next time it builds. */
	static int32 BuildMorphTargets(USkeletalMesh* SkeletalMesh, const TSharedPtr<FJsonObject>& Payload);

	/* True when the export hangs a DNA off the mesh, which is what a MetaHuman head does */
	bool ExportNamesDna();

	/* Hands the mesh its DNA, the bit stream RigLogic reads a face rig out of. It is written after
	 * the DNA asset's properties rather than as any of them, so it comes down on its own. */
	static bool ApplyDna(USkeletalMesh* SkeletalMesh, const FString& FetchPath);

	/* Flattens the face rig into a pose asset beside the mesh, one pose per control the DNA names.
	 * Each control is evaluated on its own and the joints it moves are kept as they are, which is
	 * what RigLogic hands back in the first place. Null when the DNA has nothing to pose with. */
	UPoseAsset* BakeDnaPoseAsset(USkeletalMesh* SkeletalMesh);
};

REGISTER_IMPORTER(ISkeletalMeshImporter, {
	"SkeletalMesh"
}, "Mesh Assets");
