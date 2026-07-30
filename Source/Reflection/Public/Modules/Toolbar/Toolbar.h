/* Copyright Reflection Contributors 2024-2026 */

#pragma once

#include "CoreMinimal.h"
#include "Engine/Compatibility.h"
#include "Toolbar.generated.h"

class FMenuBuilder;

UCLASS()
class REFLECTION_API UReflectionToolbar : public UObject {
	GENERATED_BODY()
public:
	void Register();
	void AddReflectionButtons(FToolMenuSection& Section);
	void AddCloudButtons(FToolMenuSection& Section);

	/* Validation is UE5 only, it has no place to live on UE4's menu bar ~~~~~~~~~~~~~~~~~~~ */
#if ENGINE_UE5
	/* Adds Reflection's own menu to the editor's main menu bar, alongside the project's
	 * other tool menus rather than buried in the Content Browser toolbar */
	void RegisterMainMenu();

	/* Fills the Validation menu with everything in the validator registry */
	static void PopulateValidationMenu(FMenuBuilder& MenuBuilder);
#endif

#if ENGINE_UE4
	void UE4Register(FToolBarBuilder& Builder);
	void UE4CloudRegister(FToolBarBuilder& Builder);
#endif

	/* Checks if Reflection is fit to function */
	void IsFitToFunction(TFunction<void(bool)> OnResponse);
	
	/* Checks if Reflection is fit to function, then called Import */
	void ImportAction();

	/* Opens a JSON file dialog */
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
