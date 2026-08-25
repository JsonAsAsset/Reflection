/* Copyright Reflection Contributors 2024-2026 */

#pragma once

#include "Importers/Constructor/Importer.h"

class IFontImporter final : public IImporter {
public:
	virtual UObject* CreateAsset(UObject* CreatedAsset) override;
	virtual bool Import() override;
};

REGISTER_IMPORTER(IFontImporter, TArray<FString>{
	TEXT("Font")
}, TEXT("Font Assets"));
