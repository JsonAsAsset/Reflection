/* Copyright Reflection Contributors 2024-2026 */

#pragma once

#include "CoreMinimal.h"
#include "Modules/Toolbar/Tools/SelectedAssetsBase.h"

class REFLECTION_API TToolConvexCollision : public TSelectedAssetsBase {
public:
	virtual void Process(UObject* Object, const TArray<TSharedPtr<FJsonValue>>& Exports) override;

	virtual FName GetSupportedClass() const override { return FName("StaticMesh"); }

	virtual FText GetDisplayName() const override { return FText::FromString("Static Meshes"); }
	virtual FText GetTooltip() const override { return FText::FromString("Reflects collision and other properties"); }
	virtual FSlateIcon GetIcon() const override { return FSlateIcon(FAppStyle::GetAppStyleSetName(), "ClassIcon.StaticMeshActor"); }
};

REGISTER_TOOL(TToolConvexCollision)
