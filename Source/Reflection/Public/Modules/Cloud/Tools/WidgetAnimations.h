/* Copyright Reflection Contributors 2024-2026 */

#pragma once

#include "CoreMinimal.h"
#include "Modules/Toolbar/Tools/SelectedAssetsBase.h"

class REFLECTION_API TWidgetAnimations : public TSelectedAssetsBase {
public:
	virtual void Process(UObject* Object, const TArray<TSharedPtr<FJsonValue>>& Exports) override;

	virtual FName GetSupportedClass() const override { return FName("WidgetBlueprint"); }

	virtual FText GetDisplayName() const override { return FText::FromString("Widget Animations"); }
	virtual FText GetTooltip() const override { return FText::FromString("Reflects widget animations"); }
	virtual FSlateIcon GetIcon() const override { return FSlateIcon(FAppStyle::GetAppStyleSetName(), "ClassIcon.WidgetBlueprint"); }
};

REGISTER_TOOL(TWidgetAnimations)
