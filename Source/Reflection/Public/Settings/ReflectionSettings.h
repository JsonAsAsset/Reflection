/* Copyright Reflection Contributors 2024-2026 */

#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"

/* Settings Substructures */
#include "Types/AnimationBlueprintSettings.h"
#include "Types/MaterialSettings.h"
#include "Types/TextureSettings.h"
#include "Redirector.h"

#include "ReflectionSettings.generated.h"

extern FName GReflectionSettingsCategoryName;
extern FName GReflectionInternalName;

USTRUCT()
struct FRSettings
{
	GENERATED_BODY()
public:
	/* Constructor to initialize default values */
	FRSettings() {
		Material = FRMaterialSettings();
		Texture = FRTextureSettings();
		AnimationBlueprint = FRAnimationBlueprintSettings();
	}

	UPROPERTY(EditAnywhere, Config, Category = Settings)
	FRAnimationBlueprintSettings AnimationBlueprint;

	UPROPERTY(EditAnywhere, Config, Category = Settings)
	FRTextureSettings Texture;
	
	UPROPERTY(EditAnywhere, Config, Category = Settings)
	FRMaterialSettings Material;

	UPROPERTY(EditAnywhere, Config, Category = Settings)
	FString ProjectName;
	
	UPROPERTY(EditAnywhere, Config, Category = Settings)
	bool SaveAssets = false;
};

USTRUCT()
struct FJVersioningSettings
{
	GENERATED_BODY()
public:
	/* Disable checking for newer updates of Reflection. */
	UPROPERTY(EditAnywhere, Config, Category = VersioningSettings)
	bool Disable = false;
};

/* Reconstruction Toolkit for Unreal Engine */
UCLASS(Config = EditorPerProjectUserSettings, DefaultConfig)
class REFLECTION_API UReflectionSettings : public UDeveloperSettings {
	GENERATED_BODY()
public:
	UReflectionSettings();

	/* Overriden to stop the Editor spacing the words between Reflection */
	virtual FText GetSectionText() const override;
	
public:
	UPROPERTY(EditAnywhere, Config, Category = Settings)
	FJVersioningSettings Versioning;
	
	UPROPERTY(EditAnywhere, Config, Category = Settings)
	FRSettings AssetSettings;

	UPROPERTY(EditAnywhere, Config, Category = Redirectors, meta = (TitleProperty = "Name"))
	TArray<FRRedirector> Redirectors;

	/* Retrieves assets from an API and imports references directly into your project. */
	UPROPERTY(EditAnywhere, Config, Category = Cloud, DisplayName = "Enable Cloud")
	bool EnableCloudServer = true;

	/* Enables experimental/developing features of Reflection. Features may not work as intended. */
	UPROPERTY(EditAnywhere, Config, Category = Settings, AdvancedDisplay)
	bool EnableExperiments = false;
};