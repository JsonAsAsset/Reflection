/* Copyright Reflection Contributors 2024-2026 */

#pragma once

#include "CoreMinimal.h"
#include "Modules/Tools/SelectedAssetsBase.h"

class UClothingAssetCommon;

/* Rebuilds the ClothingAssetCommon subobjects a mesh was exported with and binds them to its
 * render sections. Cloth lives inside the mesh rather than in a package of its own, so it cannot
 * go through the importers, and the mesh has to already be in the project with its imported model.
 *
 * Apart from TSkeletalMeshData because binding is destructive: it drops whatever cloth the mesh
 * has and rebuilds the sections. */
class REFLECTION_API TClothingData : public TSelectedAssetsBase {
public:
	virtual void Process(UObject* Object, const TArray<TSharedPtr<FJsonValue>>& Exports) override;

	virtual FName GetSupportedClass() const override { return FName("SkeletalMesh"); }

	virtual FText GetDisplayName() const override { return FText::FromString("Clothing Assets"); }
	virtual FText GetTooltip() const override { return FText::FromString("Reflects cloth data onto the selected mesh, and binds it to its sections"); }
	virtual FSlateIcon GetIcon() const override { return FSlateIcon(FAppStyle::GetAppStyleSetName(), "PhysicsAssetEditor.EnableCollision.Small"); }

protected:
	/* Fills the editor's painted masks in from the exported weight maps */
	static void RebuildParameterMasks(UClothingAssetCommon* ClothingAsset);
};

REGISTER_TOOL(TClothingData)
