/* Copyright Reflection Contributors 2024-2026 */

#pragma once

#include "Importers/Constructor/Importer.h"

class IFontFaceImporter final : public IImporter {
public:
	virtual UObject* CreateAsset(UObject* CreatedAsset) override;
	virtual bool Import() override;
};

REGISTER_IMPORTER(IFontFaceImporter, TArray<FString>{
	TEXT("FontFace")
}, TEXT("Font Assets"));
