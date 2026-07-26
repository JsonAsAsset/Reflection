/* Copyright Reflection Contributors 2024-2026 */

#pragma once

#include "./Importer.h"

/* Basic template importer using Asset Class. */
template <typename AssetType>
class ITemplatedImporter : public IImporter {
public:
	virtual UObject* CreateAsset(UObject* CreatedAsset) override;
	virtual bool Import() override;
};