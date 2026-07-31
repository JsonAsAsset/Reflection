/* Copyright Reflection Contributors 2024-2026 */

#include "Modules/Toolbar/Tools/FixUpAssetData.h"

#include "WidgetBlueprint.h"
#include "Animation/WidgetAnimation.h"
#include "PhysicsEngine/BodySetup.h"
#include "Engine/EngineUtilities.h"

void TToolFixUpAssetData::Execute() {
	TArray<FAssetData> AssetDataList = GetAssetsInSelectedFolder();

	if (AssetDataList.Num() == 0) {
		return;
	}

	static const TArray<FName> SupportedClasses = {
		"AnimSequence",
		"WidgetBlueprint",
		"StaticMesh"
	};

	for (const FAssetData& AssetData : AssetDataList) {
		if (!AssetData.IsValid() || !SupportedClasses.Contains(GetAssetDataClass(AssetData))) {
			continue;
		}
		
		UObject* Asset = AssetData.GetAsset();
		if (Asset == nullptr) continue;

		if (UAnimSequence* AnimSequence = Cast<UAnimSequence>(Asset)) {
			/* Bone compression moved behind a settings asset in 4.23; before that the sequence
			 * points straight at the compression scheme */
#if UE4_22_BELOW
			AnimSequence->CompressionScheme = nullptr;
#else
			AnimSequence->BoneCompressionSettings = nullptr;
#endif
			AnimSequence->CurveCompressionSettings = nullptr;

#if ENGINE_UE5 && ENGINE_MINOR_VERSION >= 2
			AnimSequence->BeginCacheDerivedDataForCurrentPlatform();
#endif
			AnimSequence->Modify();
		}
		
		if (UStaticMesh* StaticMesh = Cast<UStaticMesh>(Asset)) {
#if ENGINE_UE5
			StaticMesh->GetBodySetup()->CollisionTraceFlag = CTF_UseComplexAsSimple;
			StaticMesh->Modify();
			StaticMesh->GetBodySetup()->PostEditChange();

			/* Update physics data */
			StaticMesh->GetBodySetup()->InvalidatePhysicsData();
			StaticMesh->GetBodySetup()->CreatePhysicsMeshes();

			StaticMesh->MarkPackageDirty();
			StaticMesh->Modify(true);
#endif
		}

		if (UWidgetBlueprint* WidgetBlueprint = Cast<UWidgetBlueprint>(Asset)) {
			WidgetBlueprint->Animations.RemoveAll(
				[](const UWidgetAnimation* Animation) {
					return Animation == nullptr;
				}
			);
		}

		Asset->Modify();
	}
}
