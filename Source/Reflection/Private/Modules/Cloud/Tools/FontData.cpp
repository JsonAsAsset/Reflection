/* Copyright Reflection Contributors 2024-2026 */

#include "Modules/Cloud/Tools/FontData.h"

#include "Engine/FontFace.h"
#include "Engine/EngineUtilities.h"
#include "Utilities/JsonUtilities.h"

void TToolFontData::Process(UObject* Object, const TArray<TSharedPtr<FJsonValue>>& Exports) {
	const FString TargetType = "FontFace";

	UFontFace* FontFace = Cast<UFontFace>(Object);
	if (FontFace == nullptr) return;

	for (const TSharedPtr<FJsonValue>& Export : Exports) {
		if (!Export.IsValid() || !Export->AsObject().IsValid()) {
			continue;
		}

		const TSharedPtr<FJsonObject> JsonObject = Export->AsObject();
		if (!IsProperExportData(JsonObject)) continue;

		TSharedPtr<FJsonObject> Properties = JsonObject->GetObjectField(TEXT("Properties"));
		const FString Type = JsonObject->GetStringField(TEXT("Type"));
		const FString Name = JsonObject->GetStringField(TEXT("Name"));

		if (Name != Object->GetName()) continue;
		if (Type != TargetType) continue;

		/* Create an object serializer */
		GetObjectSerializer()->ExportsToNotDeserialize.Empty();
		GetObjectSerializer()->SetExportForDeserialization(JsonObject, FontFace);
		GetObjectSerializer()->Parent = FontFace;

		FUObjectExportContainer* Container = new FUObjectExportContainer(Exports);
		GetObjectSerializer()->DeserializeExports(Container);

		GetObjectSerializer()->DeserializeObjectProperties(Properties, FontFace);

		FontFace->Modify();

		BrowseToAsset(FontFace);

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
