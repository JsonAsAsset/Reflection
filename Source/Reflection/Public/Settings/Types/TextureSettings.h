/* Copyright Reflection Contributors 2024-2026 */

#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "TextureSettings.generated.h"

/* Settings for textures */
USTRUCT()
struct FRTextureSettings {
	GENERATED_BODY()
public:
	/* Re-downloads textures *that already exist*. Significantly worse for import time.
	 * Do not use this unless you are intentionally reimporting textures that were updated since last updated. */
	UPROPERTY(EditAnywhere, Config, AdvancedDisplay, Category = TextureSettings)
	bool UpdateExisingTextures = false;
};