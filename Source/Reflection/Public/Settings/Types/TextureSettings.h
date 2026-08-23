/* Copyright Reflection Contributors 2024-2026 */

#pragma once

#include "CoreMinimal.h"
#include "TextureSettings.generated.h"

/* Settings for textures */
USTRUCT()
struct FRTextureSettings {
	GENERATED_BODY()
public:
	/* Reflects textures that already exist. */
	UPROPERTY(EditAnywhere, Config, AdvancedDisplay, Category = TextureSettings)
	bool ReflectExistingTextures = false;
};