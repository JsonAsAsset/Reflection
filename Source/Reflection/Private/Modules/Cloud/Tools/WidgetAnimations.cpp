/* Copyright Reflection Contributors 2024-2026 */

#include "Modules/Cloud/Tools/WidgetAnimations.h"
#include "WidgetBlueprint.h"
#include "Animation/WidgetAnimation.h"
#include "Engine/EngineUtilities.h"

void TWidgetAnimations::Process(UObject* Object, const TArray<TSharedPtr<FJsonValue>>& Exports) {
	UWidgetBlueprint* WidgetBlueprint = Cast<UWidgetBlueprint>(Object);
	if (!WidgetBlueprint) return;

	FUObjectExportContainer* Container = new FUObjectExportContainer(Exports);
	FUObjectExport* Export = Container->FindByType(FString("WidgetBlueprintGeneratedClass"));

	if (!Export->IsJsonValid()) return;
	if (!Export->JsonObject.Get()->HasField(TEXT("Properties"))) return;

	UWidgetBlueprintGeneratedClass* GeneratedClass = Cast<UWidgetBlueprintGeneratedClass>(WidgetBlueprint->GeneratedClass);
	if (!GeneratedClass) return;

	/* Empty all animations. Only once the Cloud has come back with something to replace them
	 * with, so a failed request leaves the asset alone. */
	for (UObject* Animation : WidgetBlueprint->Animations) {
		MoveToTransientPackageAndRename(Animation);
	}

	WidgetBlueprint->Animations.Empty();

	for (UObject* AnimationObject : GeneratedClass->Animations) {
		MoveToTransientPackageAndRename(AnimationObject);
	}

	GetObjectSerializer()->WhitelistedTypes.Add("MovieScene");
	GetObjectSerializer()->WhitelistedTypes.Add("WidgetAnimation");

	Initialize(Export, Container);
	DeserializeExports(WidgetBlueprint, true);

	for (FUObjectExport* AnimationExport : GetPropertySerializer()->ExportsContainer->Exports) {
		if (AnimationExport->Object) {
			UWidgetAnimation* WidgetAnimation = AnimationExport->Get<UWidgetAnimation>();
			if (!WidgetAnimation) continue;

			const FString AnimationName = WidgetAnimation->GetName();
			if (AnimationName.EndsWith(TEXT("_INST"))) {
				WidgetAnimation->Rename(*AnimationName.Mid(0, AnimationName.Len() - 5), WidgetBlueprint);
			}

			WidgetBlueprint->Animations.Add(WidgetAnimation);
		}
	}
}
