/* Copyright Reflection Contributors 2024-2026 */

#include "Importers/Types/Curves/CurveVectorImporter.h"
#include "Factories/CurveFactory.h"
#include "Curves/CurveVector.h"
#include "Utilities/JsonHelpers.h"

UObject* ICurveVectorImporter::CreateAsset(UObject* CreatedAsset) {
	UCurveVectorFactory* CurveVectorFactory = NewObject<UCurveVectorFactory>();
	UCurveVector* CurveVector = Cast<UCurveVector>(CurveVectorFactory->FactoryCreateNew(UCurveVector::StaticClass(), GetPackage(), *GetAssetName(), RF_Standalone | RF_Public, nullptr, GWarn));

	return IImporter::CreateAsset(CurveVector);
}

bool ICurveVectorImporter::Import() {
	/* Array of containers */
	TArray<TSharedPtr<FJsonValue>> FloatCurves = GetAssetData()->GetArrayField(TEXT("FloatCurves"));

	UCurveVector* CurveVectorAsset = Create<UCurveVector>();

	/* For each container, get keys */
	for (int i = 0; i < FloatCurves.Num(); i++) {
		TArray<TSharedPtr<FJsonValue>> Keys = FloatCurves[i]->AsObject()->GetArrayField(TEXT("Keys"));
		CurveVectorAsset->FloatCurves[i].Keys.Empty();

		/* Add keys to the array */
		for (int j = 0; j < Keys.Num(); j++) {
			CurveVectorAsset->FloatCurves[i].Keys.Add(ObjectToRichCurveKey(Keys[j]->AsObject()));
		}
	}

	/* Handle edit changes, and add it to the content browser */
	return OnAssetCreation(CurveVectorAsset);
}
