/* Copyright Reflection Contributors 2024-2026 */

#pragma once

#include "Importers/Constructor/Importer.h"

class IBlendSpaceImporter : public IImporter {
public:
	virtual UObject* CreateAsset(UObject* CreatedAsset) override;
	virtual bool Import() override;
};

REGISTER_IMPORTER(IBlendSpaceImporter, (TArray<FString>{
	"BlendSpace",
	"BlendSpace1D",

	"AimOffsetBlendSpace",
	"AimOffsetBlendSpace1D"
}), TEXT("Animation Assets"));