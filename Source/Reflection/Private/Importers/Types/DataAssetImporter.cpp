/* Copyright Reflection Contributors 2024-2026 */

#include "Importers/Types/DataAssetImporter.h"
#include "Importers/Types/ItemDefinitionData.h"

#include "Engine/Log.h"

void IDataAssetImporter::Repair(UObject* Asset) const {
	if (const int32 Wrote = FItemDefinitionData::Apply(Asset, GetAssetData(), GetObjectSerializer()); Wrote > 0) {
		UE_LOG(LogReflection, Display, TEXT("%d thing(s) the data list stood for were read onto \"%s\""), Wrote, *GetAssetName());
	}
}
