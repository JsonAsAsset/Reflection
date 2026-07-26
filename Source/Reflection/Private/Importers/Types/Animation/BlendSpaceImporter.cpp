/* Copyright Reflection Contributors 2024-2026 */

#include "Importers/Types/Animation/BlendSpaceImporter.h"

#if ENGINE_UE4
#include "Animation/BlendSpaceBase.h"
#else
#include "Animation/BlendSpace.h"
#endif

UObject* IBlendSpaceImporter::CreateAsset(UObject* CreatedAsset) {
#if ENGINE_UE5
	auto BlendSpace = NewObject<UBlendSpace>(GetPackage(), GetAssetClass(), *GetAssetName(), RF_Public | RF_Standalone);
#else
	auto BlendSpace = NewObject<UBlendSpaceBase>(GetPackage(), GetAssetClass(), *GetAssetName(), RF_Public | RF_Standalone);
#endif
	
	return IImporter::CreateAsset(BlendSpace);
}

bool IBlendSpaceImporter::Import() {
#if ENGINE_UE4
	auto BlendSpace = Create<UBlendSpaceBase>();
#else
	auto BlendSpace = Create<UBlendSpace>();
#endif
	
	BlendSpace->Modify();
	
	GetObjectSerializer()->DeserializeObjectProperties(GetAssetData(), BlendSpace);

	/* Ensure internal state is refreshed after adding all samples */
	BlendSpace->ValidateSampleData();
	BlendSpace->MarkPackageDirty();
	BlendSpace->PostEditChange();
	BlendSpace->PostLoad();

	return OnAssetCreation(BlendSpace);
}