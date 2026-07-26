/* Copyright Reflection Contributors 2024-2026 */

#pragma once

#include "Importers/Constructor/Importer.h"

class IDataAssetImporter : public IImporter {
public:
	virtual UObject* CreateAsset(UObject* CreatedAsset = nullptr) override;
	virtual bool Import() override;
};

REGISTER_IMPORTER(IDataAssetImporter, {
	TEXT("DataAsset")
}, "Data Assets");