/* Copyright Reflection Contributors 2024-2026 */

#pragma once

#include "CoreMinimal.h"
#include "Modules/Tools/SelectedAssetsBase.h"

class REFLECTION_API TSkeletalMeshData : public TSelectedAssetsBase {
public:
	virtual void Process(UObject* Object, const TArray<TSharedPtr<FJsonValue>>& Exports) override;

	virtual FName GetSupportedClass() const override { return FName("SkeletalMesh"); }

	virtual FText GetDisplayName() const override { return FText::FromString("Skeletal Meshes"); }
	virtual FText GetTooltip() const override { return FText::FromString("Reflects sockets and other properties"); }
	virtual FSlateIcon GetIcon() const override { return FSlateIcon(FAppStyle::GetAppStyleSetName(), "ClassIcon.SkeletalMeshComponent"); }

protected:
	/* By reference: the caller writes slots back into the mesh */
	static TArray<FSkeletalMaterial>& GetMaterials(USkeletalMesh* Mesh);

	/* Ports the per section flags onto the mesh's sections, returning how many it reached. Cloth's
	 * section fields are left to binding. */
	static int32 ApplySectionUserData(USkeletalMesh* Mesh, const TSharedPtr<FJsonObject>& MeshExport);

	/* Rebuilds the weights behind the profile entries, returning how many profiles got any */
	static int32 ApplySkinWeightProfiles(USkeletalMesh* Mesh);

	/* Logs profiles that only made it half way: an entry with no weights, or weights with no entry */
	static void ReportSkinWeightProfiles(USkeletalMesh* Mesh);
};

REGISTER_TOOL(TSkeletalMeshData)
