/* Copyright Reflection Contributors 2024-2026 */

#pragma once

#include "Importers/Constructor/Importer.h"

class IPoseAssetImporter final : public IImporter {
public:
	UPoseAsset* PoseAsset;

	virtual UObject* CreateAsset(UObject* CreatedAsset) override;
	virtual bool Import() override;

protected:
	void ReverseCookLocalSpacePose(USkeleton* Skeleton);
	static UAnimSequence* CreateAnimSequenceFromPose(USkeleton* Skeleton, const FString& SequenceName, const TSharedPtr<FJsonObject>& PoseContainer, UPackage* Outer);

	/* The reference pose of the skeleton this asset was cooked against, keyed by bone name.
	 *
	 * Read from Cloud rather than from the skeleton in the project. A pose stores only its
	 * difference from the reference pose, so the wrong reference bends every bone it touches, and
	 * the parent skeleton moves around enough between builds that the one sitting in the project
	 * is rarely the one the pose was built on. Names survive that, bone order doesn't. */
	TMap<FName, FTransform> GetSourceMeshBindPose();
	TMap<FName, FTransform> GetCookedReferencePose() const;
};

REGISTER_IMPORTER(IPoseAssetImporter, TArray<FString>{ 
	TEXT("PoseAsset")
}, TEXT("Animation Assets"));