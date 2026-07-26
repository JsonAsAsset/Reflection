/* Copyright Reflection Contributors 2024-2026 */

#include "Modules/Cloud/Tools/FontData.h"

#include "Engine/FontFace.h"
#include "Modules/Cloud/Cloud.h"
#include "Engine/EngineUtilities.h"
#include "Utilities/JsonUtilities.h"

void TToolFontData::Execute() {
	const FString TargetType = "FontFace";
	
	TArray<FAssetData> AssetDataList = GetAssetsInSelectedFolder();

	if (AssetDataList.Num() == 0) {
		return;
	}

	for (const FAssetData& AssetData : AssetDataList) {
		if (!AssetData.IsValid()) continue;
		if (GetAssetDataClass(AssetData) != FName(*TargetType)) continue;
		
		UObject* Asset = AssetData.GetAsset();
		if (Asset == nullptr) continue;
		
		UFontFace* FontFace = Cast<UFontFace>(Asset);
		if (FontFace == nullptr) continue;

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

			if (Type == TargetType) {
				/* Create an object serializer */
				GetObjectSerializer()->ExportsToNotDeserialize.Empty();
				GetObjectSerializer()->SetExportForDeserialization(JsonObject, FontFace);
				GetObjectSerializer()->Parent = FontFace;

				FUObjectExportContainer* Container = new FUObjectExportContainer(Exports);
				GetObjectSerializer()->DeserializeExports(Container);

				GetObjectSerializer()->DeserializeObjectProperties(Properties, FontFace);
				
				FontFace->Modify();
				
				const TArray<FAssetData>& Assets = { Asset };
				const FContentBrowserModule& ContentBrowserModule = FModuleManager::Get().LoadModuleChecked<FContentBrowserModule>("ContentBrowser");
				ContentBrowserModule.Get().SyncBrowserToAssets(Assets);

				/* Notification */
				AppendNotification(
					FText::FromString("Imported Data: " + FontFace->GetName()),
					FText::FromString(FontFace->GetName()),
					3.5f,
					FAppStyle::GetBrush("ClassIcon.FontFace"),
					SNotificationItem::CS_Success,
					false,
					310.0f
				);
			}
		}
	}
}
