/* Copyright Reflection Contributors 2024-2026 */

#include "Modules/Cloud/Tools/CurveLinearColorData.h"
#include "Curves/CurveLinearColor.h"
#include "Engine/EngineUtilities.h"
#include "Utilities/JsonUtilities.h"

void TCurveLinearColorData::Process(UObject* Object) {
	UCurveLinearColor* CurveLinearColor = Cast<UCurveLinearColor>(Object);
	if (!CurveLinearColor) return;

	FUObjectExportContainer* Exports = new FUObjectExportContainer(SendToCloudForExports(GetAssetPath(Object)));
	auto Export = Exports->FindByType(FString("CurveLinearColor"));

	Initialize(Export, Exports);
	
	/* Array of containers */
	TArray<TSharedPtr<FJsonValue>> FloatCurves = GetAssetData()->GetArrayField(TEXT("FloatCurves"));

	/* For each container, get keys */
	for (int i = 0; i < FloatCurves.Num(); i++) {
		TArray<TSharedPtr<FJsonValue>> Keys = FloatCurves[i]->AsObject()->GetArrayField(TEXT("Keys"));
		CurveLinearColor->FloatCurves[i].Keys.Empty();

		/* Add keys to the array */
		for (int j = 0; j < Keys.Num(); j++) {
			CurveLinearColor->FloatCurves[i].Keys.Add(ObjectToRichCurveKey(Keys[j]->AsObject()));
		}
	}
	
	HandleAssetCreation(CurveLinearColor, CurveLinearColor->GetPackage());
}
