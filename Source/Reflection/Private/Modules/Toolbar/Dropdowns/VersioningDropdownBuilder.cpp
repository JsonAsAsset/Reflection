/* Copyright Reflection Contributors 2024-2026 */

#include "Modules/Toolbar/Dropdowns/VersioningDropdownBuilder.h"

#include "Modules/Versioning.h"
#include "Engine/Compatibility.h"
#include "Engine/EngineUtilities.h"

void IVersioningDropdownBuilder::Build(FMenuBuilder& MenuBuilder) const {
	if (!GReflectionVersioning.IsValid) {
		return;
	}

	if (!GReflectionVersioning.IsNewVersionAvailable() && !GReflectionVersioning.IsFutureVersion()) return;
	
	MenuBuilder.BeginSection("ReflectionVersioningSection", FText::FromString("Version"));
	
	FText Text, Tooltip;
	FSlateIcon Icon =
#if ENGINE_UE5
		FSlateIcon(FAppStyle::GetAppStyleSetName(), "Blueprint.CompileStatus.Background", NAME_None);
#else
		FSlateIcon(FEditorStyle::GetStyleSetName(), "MainFrame.CreditsUnrealEd");
#endif

	/* A new release is available */
	if (GReflectionVersioning.IsNewVersionAvailable()) {
		Text = FText::FromString("New Version Available");
		
		Tooltip = FText::FromString("Update your installation to version " + GReflectionVersioning.VersionName);

		Icon = FSlateIcon(FAppStyle::GetAppStyleSetName(), "Cascade.AddLODBeforeCurrent.Small");
	} else if (GReflectionVersioning.IsFutureVersion()) {
		Text = FText::FromString("Developmental");
		
		Tooltip = FText::FromString("You are currently running a developmental build");
		
	} else {
		Text = FText::FromString("Latest");
		
		Tooltip = FText::FromString("You are currently using the latest version");
	}

	MenuBuilder.AddMenuEntry(
		Text,
		Tooltip,
		Icon,
		FUIAction(
			FExecuteAction::CreateLambda([this]() {
				if (GReflectionVersioning.IsNewVersionAvailable()) {
					LaunchURL(GReflectionVersioning.HTMLUrl);
				}
			})
		),
		NAME_None
	);
	
	MenuBuilder.EndSection();
}
