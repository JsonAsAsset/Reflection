/* Copyright Reflection Contributors 2024-2026 */

#pragma once

#include "Importers/Constructor/ObjectImporter.h"

class IDataAssetImporter : public IObjectImporter {
public:
	virtual void Repair(UObject* Asset) const override;
};

REGISTER_IMPORTER(IDataAssetImporter, {
	TEXT("DataAsset")
}, "Data Assets");
