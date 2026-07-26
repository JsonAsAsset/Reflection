/* Copyright Reflection Contributors 2024-2026 */

#pragma once

#include "Importers/Constructor/Importer.h"

class IPoseAssetImporter final : public IImporter {
public:
	UPoseAsset* PoseAsset;

	virtual UObject* CreateAsset(UObject* CreatedAsset) override;
	virtual bool Import() override;

protected:
	void ReverseCookLocalSpacePose(USkeleton* Skeleton) const;
	static UAnimSequence* CreateAnimSequenceFromPose(USkeleton* Skeleton, const FString& SequenceName, const TSharedPtr<FJsonObject>& PoseContainer, UPackage* Outer);
};

REGISTER_IMPORTER(IPoseAssetImporter, TArray<FString>{ 
	TEXT("PoseAsset")
}, TEXT("Animation Assets"));