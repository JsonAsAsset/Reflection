/* Copyright Reflection Contributors 2024-2026 */

#pragma once

#include "CoreMinimal.h"
#include "Engine/Compatibility.h"
#include "Toolbar.generated.h"

class FMenuBuilder;

/* 4.25 and below build this module without the engine's shared PCH (see Reflection.Build.cs),
 * which is where the menu types below used to come in from. FToolMenuSection is only named at
 * all on the versions that have ToolMenus, so 4.24 and 4.25 are the ones that need it. */
#if UE4_25_BELOW
class FToolBarBuilder;

#if !UE4_23_BELOW
struct FToolMenuSection;
#endif
#endif

UCLASS()
class REFLECTION_API UReflectionToolbar : public UObject {
	GENERATED_BODY()
public:
	void Register();

	/* FToolMenuSection arrived with ToolMenus, which replaced the FExtender toolbar in 4.24 ~~~ */
#if !UE4_23_BELOW
	void AddReflectionButtons(FToolMenuSection& Section);
	void AddCloudButtons(FToolMenuSection& Section);
#endif

	/* Validation is UE5 only, it has no place to live on UE4's menu bar ~~~~~~~~~~~~~~~~~~~ */
#if ENGINE_UE5
	/* Adds Reflection's own menu to the editor's main menu bar, alongside the project's
	 * other tool menus rather than buried in the Content Browser toolbar */
	void RegisterMainMenu();

	/* Fills the Validation menu with everything in the validator registry */
	static void PopulateValidationMenu(FMenuBuilder& MenuBuilder);

	/* Fills the Asset Tools menu with the tools that work over what is already in the project */
	static void PopulateAssetToolsMenu(FMenuBuilder& MenuBuilder);
#endif

	/* Adds Reflect to the Content Browser's right click menu, so an asset already in the project
	 * can be reflected without retyping its path. 4.26 is where both menus became tool menus */
#if !UE4_25_BELOW
	void RegisterAssetContextMenu();
#endif

#if ENGINE_UE4
	void UE4Register(FToolBarBuilder& Builder);
	void UE4CloudRegister(FToolBarBuilder& Builder);
#endif

	/* Checks if Reflection is fit to function */
	void IsFitToFunction(TFunction<void(bool)> OnResponse);
	
	/* Checks if Reflection is fit to function, then called Import */
	void ImportAction();

	/* Asks for asset paths, or for a JSON file when bypassed */
	void Import();

	/* UI Display ~~~~~~~~~~~~~~ */
	static TSharedRef<SWidget> CreateMenuDropdown();
	static TSharedRef<SWidget> CreateCloudMenuDropdown();

	static bool IsToolBarVisible();

protected:
	/* Wait for Cloud to Initialize */
	void HandleCloudWaiting();

	FTimerHandle WaitForCloudTimer;
	int32 CloudDotCount = 0;

	void WaitForCloudTimerCallback();
	void CancelWaitForCloudTimer();
};
