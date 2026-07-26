/* Copyright Reflection Contributors 2024-2026 */

#pragma once

#include "Importers/Constructor/Importer.h"

/* Basic template importer using Asset Class. */
template <typename AssetType>
class ITextureImporter : public IImporter {
public:
	virtual bool Import() override;
};