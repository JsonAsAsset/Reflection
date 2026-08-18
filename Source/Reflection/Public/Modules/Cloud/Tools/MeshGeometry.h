/* Copyright Reflection Contributors 2024-2026 */

#pragma once

#include "CoreMinimal.h"
#include "Modules/Tools/SelectedAssetsBase.h"

/* Replaces a skeletal mesh's geometry with the cooked geometry it was exported with, vertex for
 * vertex. An exchange format re-derives vertices on import, which breaks cloth binding, skin
 * weight profiles and morph deltas, since all of them key off the original indices. */
class REFLECTION_API TMeshGeometry : public TSelectedAssetsBase {
public:
	virtual void Process(UObject* Object, const TArray<TSharedPtr<FJsonValue>>& Exports) override;

	virtual FName GetSupportedClass() const override { return FName("SkeletalMesh"); }

	virtual FText GetDisplayName() const override { return FText::FromString("Mesh Geometry"); }
	virtual FText GetTooltip() const override { return FText::FromString("Rebuilds the selected mesh's LOD models from the cooked geometry"); }
	virtual FSlateIcon GetIcon() const override { return FSlateIcon(FAppStyle::GetAppStyleSetName(), "ClassIcon.SkeletalMeshComponent"); }

	/* Describes every LOD the payload carries into the mesh's imported model, returning how many
	 * were rebuilt. Shared with the skeletal mesh importer, which has just made the asset the tool
	 * is otherwise pointed at, so both reach the geometry the same way. */
	static int32 RebuildLodModels(USkeletalMesh* SkeletalMesh, const TSharedPtr<FJsonObject>& Response);
};

REGISTER_TOOL(TMeshGeometry)
