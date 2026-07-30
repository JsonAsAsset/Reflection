/* Copyright Reflection Contributors 2024-2026 */

#pragma once

#include "CoreMinimal.h"
#include "Modules/Toolbar/Tools/SelectedAssetsBase.h"

class REFLECTION_API TCurveLinearColorData : public TSelectedAssetsBase {
public:
	virtual void Process(UObject* Object, const TArray<TSharedPtr<FJsonValue>>& Exports) override;

	virtual FName GetSupportedClass() const override { return FName("CurveLinearColor"); }
};
