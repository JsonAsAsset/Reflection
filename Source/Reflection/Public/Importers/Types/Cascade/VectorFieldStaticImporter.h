/* Copyright Reflection Contributors 2024-2026 */

#pragma once

#include "Importers/Constructor/Importer.h"

/* Rebuilds a static vector field.
 *
 * The export carries the grid size and the bounds and nothing else: the vectors live in bulk data
 * serialized after the properties, so they come down from the Cloud as raw bytes the same way a
 * texture's first mip does. */
class IVectorFieldStaticImporter : public IImporter {
public:
	virtual UObject* CreateAsset(UObject* CreatedAsset) override;
	virtual bool Import() override;

private:
	/* Fetches the volume, one FFloat16Color per voxel. False if the Cloud has nothing usable. */
	bool DownloadVolumeData(int32 ExpectedSize, TArray<uint8>& OutData) const;
};

REGISTER_IMPORTER(IVectorFieldStaticImporter, {
	"VectorFieldStatic"
}, "Cascade Assets");
