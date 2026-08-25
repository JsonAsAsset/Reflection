/* Copyright Reflection Contributors 2024-2026 */

#pragma once

#include "Importers/Constructor/ObjectImporter.h"

class IDataAssetImporter : public IObjectImporter {
};

REGISTER_IMPORTER(IDataAssetImporter, {
	TEXT("DataAsset")
}, "Data Assets");
