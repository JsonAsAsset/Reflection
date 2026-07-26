/* Copyright Reflection Contributors 2024-2026 */

#pragma once

#include "Importers/Constructor/Graph/MaterialGraph.h"

class IMaterialFunctionImporter : public IMaterialGraph {
public:
	virtual UObject* CreateAsset(UObject* CreatedAsset) override;
	virtual bool Import() override;
};

REGISTER_IMPORTER(IMaterialFunctionImporter, {
	"MaterialFunction"
}, "Material Assets");