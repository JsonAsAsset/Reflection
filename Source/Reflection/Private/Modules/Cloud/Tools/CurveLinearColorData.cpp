/* Copyright Reflection Contributors 2024-2026 */

#include "Modules/Cloud/Tools/CurveLinearColorData.h"
#include "Curves/CurveLinearColor.h"
#include "Engine/EngineUtilities.h"
#include "Utilities/JsonHelpers.h"

void TCurveLinearColorData::Process(UObject* Object, const TArray<TSharedPtr<FJsonValue>>& Exports) {
	UCurveLinearColor* CurveLinearColor = Cast<UCurveLinearColor>(Object);
	if (!CurveLinearColor) return;

	FUObjectExportContainer* Container = new FUObjectExportContainer(Exports);
	FUObjectExport* Export = Container->FindByType(FString("CurveLinearColor"));

	/* FindByType hands back an empty export rather than null when there is no match, and
	 * Initialize reads straight through its JSON */
	if (!Export->IsJsonValid()) return;

	Initialize(Export, Container);

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
