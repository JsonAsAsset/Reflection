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
	static TArray<FSkeletalMaterial> GetMaterials(USkeletalMesh* Mesh);
};

REGISTER_TOOL(TSkeletalMeshData)
