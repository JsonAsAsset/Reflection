/* Copyright Reflection Contributors 2024-2026 */

#pragma once

#include "CoreMinimal.h"
#include "Modules/Tools/SelectedAssetsBase.h"

class REFLECTION_API TToolFontData : public TSelectedAssetsBase {
public:
	virtual void Process(UObject* Object, const TArray<TSharedPtr<FJsonValue>>& Exports) override;

	virtual FName GetSupportedClass() const override { return FName("FontFace"); }

	virtual FText GetDisplayName() const override { return FText::FromString("Fonts"); }
	virtual FText GetTooltip() const override { return FText::FromString("Reflects font properties (not vectorized data)"); }
	virtual FSlateIcon GetIcon() const override { return FSlateIcon(FAppStyle::GetAppStyleSetName(), "ClassIcon.FontFace"); }
};

REGISTER_TOOL(TToolFontData)
