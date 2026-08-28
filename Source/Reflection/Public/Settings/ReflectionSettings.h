/* Copyright Reflection Contributors 2024-2026 */

#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"

/* Settings Substructures */
#include "Types/AnimationBlueprintSettings.h"
#include "Types/MeshSettings.h"
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
		AnimationBlueprint = FRAnimationBlueprintSettings();
		Texture = FRTextureSettings();
		Mesh = FRMeshSettings();
	}

	UPROPERTY(EditAnywhere, Config, Category = Settings)
	FRAnimationBlueprintSettings AnimationBlueprint;

	UPROPERTY(EditAnywhere, Config, Category = Settings)
	FRTextureSettings Texture;

	UPROPERTY(EditAnywhere, Config, Category = Settings)
	FRMeshSettings Mesh;

	/* Whether a file holding several assets comes out as several files.
	 *
	 * Most packages hold one asset and this changes nothing for them. Some hold a set: an HLOD proxy
	 * keeps the mesh it stands in for, the material that draws it and the texture that material
	 * samples, all under the one name, and every reference between them is written as a step inside
	 * that one file.
	 *
	 * Left off they come out the way the game keeps them, in the one package, which is what those
	 * references describe. Turned on each becomes a file of its own beside the others, which reads
	 * more plainly in the content browser and is not what was shipped. */
	UPROPERTY(EditAnywhere, DisplayName = "Separate Packaged Assets", Config, Category = Settings)
	bool SeparatePackagedAssets = false;

	UPROPERTY(EditAnywhere, Config, Category = Settings)
	bool SaveAssets = false;
};

/* Reconstruction Toolkit for Unreal Engine */
UCLASS(Config = EditorPerProjectUserSettings, DefaultConfig)
class REFLECTION_API UReflectionSettings : public UDeveloperSettings {
	GENERATED_BODY()
public:
	UReflectionSettings();

	virtual FText GetSectionText() const override;

public:
	UPROPERTY(EditAnywhere, Config, Category = Redirectors, meta = (TitleProperty = "Name"))
	TArray<FRRedirector> Redirectors;

	UPROPERTY(EditAnywhere, Config, Category = Settings)
	FRSettings AssetSettings;

	/* On, the reflect button opens a file dialog and imports a json you exported yourself. Off, it
	 * asks for an asset path and pulls the data from Cloud. */
	UPROPERTY(
		#if !REFLECTION_CLOUD_SERVER
			EditAnywhere,
			DisplayName = "Use Local Json instead of Cloud",
			Category = Settings,
			Config
		#endif
	)
	bool UseLocalJson = false;

	/* Enables experimental/developing features. Features may not work as intended. */
	UPROPERTY(EditAnywhere, Config, DisplayName = "Experiments", Category = Settings, AdvancedDisplay)
	bool EnableExperiments = false;
};