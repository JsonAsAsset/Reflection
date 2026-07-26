/* Copyright Reflection Contributors 2024-2026 */

#pragma once

#include "Importers/Constructor/Importer.h"
#include "Engine/CurveTable.h"

class ICurveTableImporter : public IImporter {
public:
	virtual UObject* CreateAsset(UObject* CreatedAsset) override;
	virtual bool Import() override;
};

REGISTER_IMPORTER(ICurveTableImporter, {
	"CurveTable"
}, "Table Assets");