/* Copyright Reflection Contributors 2024-2026 */

#include "Importers/Types/Curves/CurveLinearColorImporter.h"
#include "Curves/CurveLinearColor.h"
#include "Factories/CurveFactory.h"
#include "Utilities/JsonUtilities.h"

UObject* ICurveLinearColorImporter::CreateAsset(UObject* CreatedAsset) {
	UCurveLinearColorFactory* CurveFactory = NewObject<UCurveLinearColorFactory>();
	UCurveLinearColor* CurveLinearColor = Cast<UCurveLinearColor>(CurveFactory->FactoryCreateNew(UCurveLinearColor::StaticClass(), GetPackage(), *GetAssetName(), RF_Standalone | RF_Public, nullptr, GWarn));

	return IImporter::CreateAsset(CurveLinearColor);
}

bool ICurveLinearColorImporter::Import() {
	/* Array of containers */
	TArray<TSharedPtr<FJsonValue>> FloatCurves = GetAssetData()->GetArrayField(TEXT("FloatCurves"));

	UCurveLinearColor* CurveLinearColor = Create<UCurveLinearColor>();

	/* For each container, get keys */
	for (int i = 0; i < FloatCurves.Num(); i++) {
		TArray<TSharedPtr<FJsonValue>> Keys = FloatCurves[i]->AsObject()->GetArrayField(TEXT("Keys"));
		CurveLinearColor->FloatCurves[i].Keys.Empty();

		/* Add keys to the array */
		for (int j = 0; j < Keys.Num(); j++) {
			CurveLinearColor->FloatCurves[i].Keys.Add(ObjectToRichCurveKey(Keys[j]->AsObject()));
		}
	}

	/* Handle edit changes, and add it to the content browser */
	return OnAssetCreation(CurveLinearColor);
}
