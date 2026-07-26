/* Copyright Reflection Contributors 2024-2026 */

#include "Modules/Cloud/Tools/AnimationData.h"
#include "Engine/EngineUtilities.h"

#include "Dom/JsonObject.h"
#include "Animation/AnimSequence.h"
#include "Modules/Cloud/Cloud.h"

void TToolAnimationData::Execute() {
	TArray<FAssetData> AssetDataList = GetAssetsInSelectedFolder();

	if (AssetDataList.Num() == 0) {
		return;
	}

	for (const FAssetData& AssetData : AssetDataList) {
		if (!AssetData.IsValid()) continue;
		if (GetAssetDataClass(AssetData) != "AnimSequence") continue;
		
		UObject* Asset = AssetData.GetAsset();
		if (Asset == nullptr) continue;

		UAnimSequence* AnimSequence = Cast<UAnimSequence>(Asset);
		if (AnimSequence == nullptr) continue;

		/* Request to API ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ */
		FString ObjectPath = GetAssetObjectPath(AssetData);

		const TSharedPtr<FJsonObject> Response = Cloud::Export::GetRaw(ObjectPath);
		if (Response == nullptr || ObjectPath.IsEmpty()) continue;

		/* Not found */
		if (Response->HasField(TEXT("errored"))) {
			continue;
		}

		TArray<TSharedPtr<FJsonValue>> Exports = Response->GetArrayField(TEXT("exports"));
		
		for (const TSharedPtr<FJsonValue>& Export : Exports) {
			if (!Export.IsValid() || !Export->AsObject().IsValid()) {
				continue;
			}

			const TSharedPtr<FJsonObject> JsonObject = Export->AsObject();
			if (!IsProperExportData(JsonObject)) continue;

			TSharedPtr<FJsonObject> Properties = JsonObject->GetObjectField(TEXT("Properties"));
			const FString Type = JsonObject->GetStringField(TEXT("Type"));
			const FString Name = JsonObject->GetStringField(TEXT("Name"));

			if (Name != Asset->GetName()) continue;

			if (Type == "AnimSequence") {
				FUObjectExportContainer* Container = new FUObjectExportContainer(Exports);

				for (FUObjectExport* ExportInContainer : Container->Exports) {
					if (ExportInContainer->GetClass() == UAnimSequence::StaticClass()) {
						ExportInContainer->Object = AnimSequence;
						Initialize(ExportInContainer, Container);
					}
				}
					
				ReadAnimationData(this, false);
				
				/* Notification */
				AppendNotification(
					FText::FromString("Imported Animation Data: " + AnimSequence->GetName()),
					FText::FromString(AnimSequence->GetName()),
					3.5f,
					FAppStyle::GetBrush("PhysicsAssetEditor.EnableCollision.Small"),
					SNotificationItem::CS_Success,
					false,
					310.0f
				);
			}
		}
	}
}
