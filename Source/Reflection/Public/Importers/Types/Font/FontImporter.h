/* Copyright Reflection Contributors 2024-2026 */

#pragma once

#include "Importers/Constructor/ObjectImporter.h"

class IFontImporter final : public IObjectImporter {
public:
	/* A font holding its own faces holds them empty: the typeface is bulk data beside the package
	 * rather than a property, so it is fetched and put in once the faces exist. */
	virtual void Repair(UObject* Asset) const override;

	virtual void Validate(UObject* Asset) const override;
};

REGISTER_IMPORTER(IFontImporter, TArray<FString>{
	TEXT("Font")
}, TEXT("Font Assets"));
