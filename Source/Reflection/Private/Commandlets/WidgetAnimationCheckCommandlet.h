/* Copyright Reflection Contributors 2024-2026 */

/* Instantiates a widget blueprint and resolves every binding of every animation against the live widget, the
 * way the editor preview and the game do:
 *
 *     -run=WidgetAnimationCheck -path=/Game/Some/Widget */

#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"
#include "WidgetAnimationCheckCommandlet.generated.h"

UCLASS()
class UWidgetAnimationCheckCommandlet : public UCommandlet {
	GENERATED_BODY()

public:
	virtual int32 Main(const FString& Params) override;
};
