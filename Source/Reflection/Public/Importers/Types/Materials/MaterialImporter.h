/* Copyright Reflection Contributors 2024-2026 */

#pragma once

#include "Importers/Constructor/Graph/MaterialGraph.h"

class IMaterialImporter final : public IMaterialGraph {
public:
	virtual UObject* CreateAsset(UObject* CreatedAsset) override;
	virtual bool Import() override;
};

REGISTER_IMPORTER(IMaterialImporter, {
	"Material"
}, "Material Assets");