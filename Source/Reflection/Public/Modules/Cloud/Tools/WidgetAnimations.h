/* Copyright Reflection Contributors 2024-2026 */

#pragma once

#include "CoreMinimal.h"
#include "Modules/Toolbar/Tools/SelectedAssetsBase.h"

class REFLECTION_API TWidgetAnimations : public TSelectedAssetsBase {
public:
	virtual void Process(UObject* Object) override;
};