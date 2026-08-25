/* Copyright Reflection Contributors 2024-2026 */

#include "Importers/Constructor/ObjectImporter.h"

UObject* IObjectImporter::CreateAsset(UObject* CreatedAsset) {
	/* The class the export names rather than one written in here, so an asset of a class derived
	 * from the one this importer was registered for still comes out as itself */
	return IImporter::CreateAsset(NewObject<UObject>(GetPackage(), GetAssetClass(), StringToName(GetAssetName()), RF_Public | RF_Standalone));
}

bool IObjectImporter::Import() {
	UObject* Asset = Create<UObject>();
	if (Asset == nullptr) return false;

	auto _ = Asset->MarkPackageDirty();

	/* Subobjects first: a property that points at one has nothing to point at until it exists */
	DeserializeExports(Asset);
	GetObjectSerializer()->DeserializeObjectProperties(GetAssetData(), Asset);

	Repair(Asset);

	Asset->PostEditChange();

	return OnAssetCreation(Asset);
}
