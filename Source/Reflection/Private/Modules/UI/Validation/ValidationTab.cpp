/* Copyright Reflection Contributors 2024-2026 */

#include "Modules/UI/Validation/ValidationTab.h"

/* Validation is UE5 only */
#if ENGINE_UE5

#include "Engine/Compatibility.h"
#include "Engine/EngineUtilities.h"

#include "Modules/Validation/ValidatorRegistry.h"
#include "Modules/UI/Validation/SValidationPanel.h"
#include "Modules/UI/StyleModule.h"

#include "Framework/Docking/TabManager.h"
#include "Widgets/Docking/SDockTab.h"

#define LOCTEXT_NAMESPACE "Reflection.Validation"

const FName FValidationTab::TabId(TEXT("ReflectionValidation"));

TWeakPtr<SValidationPanel> FValidationTab::ActivePanel;

void FValidationTab::Register() {
	FGlobalTabmanager::Get()->RegisterNomadTabSpawner(TabId, FOnSpawnTab::CreateStatic(&FValidationTab::Spawn))
		.SetDisplayName(LOCTEXT("TabTitle", "Validation"))
		.SetTooltipText(LOCTEXT("TabTooltip", "Check this project's content against the real game files."))
		.SetIcon(FSlateIcon(FReflectionStyle::GetStyleSetName(), "Toolbar.Icon"))
		.SetMenuType(ETabSpawnerMenuType::Hidden);

	FReflectionValidator Validator; {
		Validator.Id = TEXT("Validation");
		Validator.Label = LOCTEXT("ValidationLabel", "Validation");
		Validator.Description = LOCTEXT("ValidationDescription", "Find assets that don't exist in the game, or that sit in the wrong folder.");
		Validator.Icon = FSlateIcon(FReflectionStyle::GetStyleSetName(), "Toolbar.Icon");
		Validator.OnOpen = FExecuteAction::CreateStatic(&FValidationTab::Open);
	}

	FReflectionValidatorRegistry::Register(Validator);
}

void FValidationTab::Unregister() {
	FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(TabId);

	ActivePanel.Reset();
}

void FValidationTab::Open() {
	OpenAt(GetSelectedContentBrowserFolder(), false);
}

void FValidationTab::OpenAt(const FString RootPath, const bool bRunImmediately) {
	FGlobalTabmanager::Get()->TryInvokeTab(TabId);

	const TSharedPtr<SValidationPanel> Panel = ActivePanel.Pin();
	if (!Panel.IsValid()) {
		return;
	}

	if (!RootPath.IsEmpty()) {
		Panel->SetRootPath(RootPath);
	}

	if (bRunImmediately) {
		Panel->StartValidation();
	}
}

void FValidationTab::OpenAtSelectedFolder() {
	const FString SelectedFolder = GetSelectedContentBrowserFolder();

	if (SelectedFolder.IsEmpty()) {
		SpawnPrompt(TEXT("Validation"), TEXT("Select a folder in the Content Browser first."));

		return;
	}

	OpenAt(SelectedFolder, true);
}

TSharedRef<SDockTab> FValidationTab::Spawn(const FSpawnTabArgs& Args) {
	const TSharedRef<SValidationPanel> Panel = SNew(SValidationPanel);
	ActivePanel = Panel;

	return SNew(SDockTab)
		.TabRole(NomadTab)
		[
			Panel
		];
}

#undef LOCTEXT_NAMESPACE

#endif
