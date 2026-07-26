/* Copyright Reflection Contributors 2024-2026 */

#include "Modules/Toolbar/Tools/SelectedAssetsBase.h"

#include "Modules/Cloud/Cloud.h"
#include "Engine/EngineUtilities.h"

void TSelectedAssetsBase::Execute() {
	TArray<FAssetData> AssetDataList = GetAssetsInSelectedFolder();

	const UObject* SelectedAsset = GetSelectedAsset<UObject>(true);
	
	if (AssetDataList.Num() == 0 && SelectedAsset == nullptr) {
		return;
	}

	if (SelectedAsset) {
		AssetDataList.Empty();
		
		FAssetData AssetData(SelectedAsset);
		AssetDataList.Add(AssetData);
	}

	for (const FAssetData& AssetData : AssetDataList) {
		if (!AssetData.IsValid()) continue;
		
		UObject* Asset = AssetData.GetAsset();
		if (Asset == nullptr) continue;

		Process(Asset);
	}
}

TArray<TSharedPtr<FJsonValue>> TSelectedAssetsBase::SendToCloudForExports(const FString& ObjectPath) {
	const auto Exports = Cloud::Export::Array::Get(ObjectPath, true);

	return Exports;
}
