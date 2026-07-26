/* Copyright Reflection Contributors 2024-2026 */

#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "AnimationBlueprintSettings.generated.h"

/* Settings for animation blueprints */
USTRUCT()
struct FRAnimationBlueprintSettings {
	GENERATED_BODY()
public:
	/* Saves IDs in Node's comment. */
	UPROPERTY(EditAnywhere, Config, AdvancedDisplay, Category = AnimationBlueprintSettings)
	bool NodeIDComments = false;
};