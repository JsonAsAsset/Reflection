/* Copyright Reflection Contributors 2024-2026 */

#include "Modules/Cloud/Tools/AnimationData.h"
#include "Engine/EngineUtilities.h"

#include "Dom/JsonObject.h"
#include "Animation/AnimSequence.h"

void TToolAnimationData::Process(UObject* Object, const TArray<TSharedPtr<FJsonValue>>& Exports) {
	UAnimSequence* AnimSequence = Cast<UAnimSequence>(Object);
	if (AnimSequence == nullptr) return;

	for (const TSharedPtr<FJsonValue>& Export : Exports) {
		if (!Export.IsValid() || !Export->AsObject().IsValid()) {
			continue;
		}

		const TSharedPtr<FJsonObject> JsonObject = Export->AsObject();
		if (!IsProperExportData(JsonObject)) continue;

		const FString Type = JsonObject->GetStringField(TEXT("Type"));
		const FString Name = JsonObject->GetStringField(TEXT("Name"));

		if (Name != Object->GetName()) continue;
		if (Type != "AnimSequence") continue;

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
			FText::FromString("Reflected Animation Data: " + AnimSequence->GetName()),
			FText::FromString(AnimSequence->GetName()),
			3.5f,
			FAppStyle::Get().GetBrush("PhysicsAssetEditor.EnableCollision.Small"),
			SNotificationItem::CS_Success,
			false,
			310.0f
		);
	}
}
