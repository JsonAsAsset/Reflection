/* Copyright Reflection Contributors 2024-2026 */

#pragma once

#include "CoreMinimal.h"
#include "Modules/Toolbar/Tools/SelectedAssetsBase.h"

class REFLECTION_API TCurveLinearColorData : public TSelectedAssetsBase {
public:
	virtual void Process(UObject* Object, const TArray<TSharedPtr<FJsonValue>>& Exports) override;

	virtual FName GetSupportedClass() const override { return FName("CurveLinearColor"); }

	virtual FText GetDisplayName() const override { return FText::FromString("Linear Colors"); }
	virtual FText GetTooltip() const override { return FText::FromString("Reflects colors if any changes were made"); }
	virtual FSlateIcon GetIcon() const override { return FSlateIcon(FAppStyle::GetAppStyleSetName(), "ClassIcon.CurveBase"); }
};

REGISTER_TOOL(TCurveLinearColorData)
