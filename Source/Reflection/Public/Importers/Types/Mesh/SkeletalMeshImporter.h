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
	/* Gives up on a mesh that was made but never finished, leaving nothing behind that can be
	 * drawn. Called on the way out of a failed import, before it reports what went wrong. */
	void Abandon(USkeletalMesh* SkeletalMesh);

	/* The skeleton the mesh is skinned to, imported through the Cloud when the project doesn't
	 * have it. Null when the export names none, or when the one it names could not be reached. */
	USkeleton* ResolveSkeleton();

	/* A skeleton made out of the mesh's own reference pose, saved beside it. For a mesh whose
	 * export names no skeleton at all: there is nothing to go and fetch, and the bones the mesh
	 * carries are the whole of what a skeleton for it would hold anyway. */
	USkeleton* BuildSkeletonFromMesh(USkeletalMesh* SkeletalMesh) const;

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

	/* Rewrites a DNA so the RigLogic anim node reads it correctly. The node is the only thing that
	 * ever reads one, and it reads a joint's numbers in axes this data is not written in, so the
	 * data is put into the node's axes rather than the node into the data's. */
	TArray<uint8> RewriteDnaForAnimNode(USkeletalMesh* SkeletalMesh, const TArray<uint8>& Dna);

	/* Hands the mesh its DNA, the bit stream RigLogic reads a face rig out of. It is written after
	 * the DNA asset's properties rather than as any of them, so it comes down on its own. */
	bool ApplyDna(USkeletalMesh* SkeletalMesh, const FString& FetchPath);

	/* Binds the mesh at the pose the cooked package says it was built at, rather than at the one
	 * its skeleton carries. A skeleton is shared between characters and each is built at its own
	 * proportions, so the two are not the same pose. False when the Cloud has none to give. */
	bool ApplyCookedBindPose(USkeletalMesh* SkeletalMesh, USkeleton* Skeleton, const FString& FetchPath);

	/* Puts the mesh's bind pose where the DNA's neutral is. RigLogic sets a bone rather than adding
	 * to it, so a mesh bound anywhere else wears the difference as a deformation the whole time the
	 * rig runs. False when the skeleton can't carry the DNA's joints. */
	bool AlignBindPoseToDna(USkeletalMesh* SkeletalMesh);

	/* Flattens the face rig into a pose asset beside the mesh, one pose per control the DNA names.
	 * Each control is evaluated on its own and the joints it moves are kept as they are, which is
	 * what RigLogic hands back in the first place. Null when the DNA has nothing to pose with. */
	bool BuildBackportedPosePlan(const TSharedPtr<class IDNAReader>& Behavior, TArray<struct FDnaPosePlan>& OutPlan);
	UPoseAsset* BakeDnaPoseAsset(USkeletalMesh* SkeletalMesh);

	/* The same poses for an engine with no RigLogic, worked out by the Cloud instead of here */
	UPoseAsset* BakeDnaPoseAssetFromCloud(USkeletalMesh* SkeletalMesh, const FString& FetchPath);
};

REGISTER_IMPORTER(ISkeletalMeshImporter, {
	"SkeletalMesh"
}, "Mesh Assets");
