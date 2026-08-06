/* Copyright Reflection Contributors 2024-2026 */

#include "Modules/Toolbar/Tools/ClearImportData.h"
#include "EditorFramework/AssetImportData.h"
#include "Engine/FontFace.h"
#include "Factories/FbxAnimSequenceImportData.h"
#include "Engine/EngineUtilities.h"
#include "Sound/SoundWave.h"

void TToolClearImportData::Execute() {
	TArray<FAssetData> AssetDataList = GetAssetsInSelectedFolder();

	if (AssetDataList.Num() == 0) {
		return;
	}

	static const TArray<FName> SupportedClasses = {
		"AnimSequence",
		"SkeletalMesh",
		"StaticMesh",
		"Texture",
		"Texture2D",
		"TextureCube",
		"SoundWave",
		"FontFace",
	};

	for (const FAssetData& AssetData : AssetDataList) {
		if (!AssetData.IsValid() || !SupportedClasses.Contains(GetAssetDataClass(AssetData))) {
			continue;
		}
		
		UObject* Asset = AssetData.GetAsset();
		if (Asset == nullptr) continue;

		if (UAnimSequence* AnimSequence = Cast<UAnimSequence>(Asset)) {
			AnimSequence->AssetImportData->SourceData.SourceFiles.Empty();
			
			if (UFbxAnimSequenceImportData* FbxImportData = Cast<UFbxAnimSequenceImportData>(AnimSequence->AssetImportData)) {
				FbxImportData->ImportUniformScale = 1.0f;
			}
			
			AnimSequence->Modify();
		}

		if (const UStaticMesh* StaticMesh = Cast<UStaticMesh>(Asset)) {
	#if UE4_27_AND_UE5
			/* Direct member access was deprecated in 5.7; the accessor arrived in 4.27 */
			StaticMesh->GetAssetImportData()->SourceData.SourceFiles.Empty();
	#else
			StaticMesh->AssetImportData->SourceData.SourceFiles.Empty();
	#endif
		}

		if (const UTexture* Texture = Cast<UTexture>(Asset)) {
			Texture->AssetImportData->SourceData.SourceFiles.Empty();
		}

		if (UFontFace* FontFace = Cast<UFontFace>(Asset)) {
			FontFace->SourceFilename = FString();
		}

		if (USoundWave* SoundWave = Cast<USoundWave>(Asset)) {
			SoundWave->AssetImportData = nullptr;
		}

		if (USkeletalMesh* SkeletalMesh = Cast<USkeletalMesh>(Asset)) {
			SetAssetImportData(SkeletalMesh, nullptr);
			SkeletalMesh->Modify();
		}

		Asset->Modify();
	}
}
