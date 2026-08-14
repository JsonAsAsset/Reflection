/* Copyright Reflection Contributors 2024-2026 */

#pragma once

#include "Importers/Constructor/Importer.h"

/* Builds a static mesh from the cooked geometry.
 *
 * The export carries the properties and the material slots, but the geometry lives in the render
 * buffers rather than in properties, so it comes down from the Cloud alongside them. Vertices are
 * used in the order the game cooked them, splits included. */
class IStaticMeshImporter : public IImporter {
public:
	virtual UObject* CreateAsset(UObject* CreatedAsset) override;
	virtual bool Import() override;

private:
	/* Describes one LOD into the mesh at LodIndex, which counts the LODs actually described rather
	 * than the ones the payload names. False when the payload has nothing usable in it. */
	bool BuildLod(UStaticMesh* StaticMesh, const FUObjectJsonValueExport& Lod, int32 LodIndex);

	/* Converts the legacy Materials array into StaticMaterials, preserving the slot names supplied by
     * the geometry payload while attaching each material reference by index. */
	void NormalizeLegacyStaticMeshMaterials(UStaticMesh* StaticMesh, const TSharedPtr<FJsonObject>& Properties);
	
	/* Slot order comes from the geometry payload, since a polygon group names its slot */
	static void BuildMaterialSlots(UStaticMesh* StaticMesh, const TArray<TSharedPtr<FJsonValue>>& Slots);

	/* Collision and sockets, both of which live in the export's own subobjects rather than in the
	 * geometry the Cloud serves */
	void BuildCollisionAndSockets(UStaticMesh* StaticMesh);

	/* Lowest LOD that came with a colour buffer, or INDEX_NONE when none of them did. Set while
	 * describing the LODs and read once they are all in. */
	int32 FirstColoredLod = INDEX_NONE;
};

REGISTER_IMPORTER(IStaticMeshImporter, {
	"StaticMesh"
}, "Mesh Assets");
