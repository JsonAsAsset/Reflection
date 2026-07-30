/* Copyright Reflection Contributors 2024-2026 */

#include "Modules/Toolbar/Dropdowns/GeneralDropdownBuilder.h"

#include "Engine/Compatibility.h"
#include "Engine/EngineUtilities.h"

void IGeneralDropdownBuilder::Build(FMenuBuilder& MenuBuilder) const {
	MenuBuilder.AddMenuEntry(
		FText::FromString("Plugin Settings"),
		FText::FromString("Navigate to Plugin Settings"),
#if ENGINE_UE5
		FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.Settings"),
#else
		FSlateIcon(FEditorStyle::GetStyleSetName(), "ProjectSettings.TabIcon"),
#endif
		FUIAction(
			FExecuteAction::CreateLambda([this]() {
				OpenPluginSettings();
			})
		),
		NAME_None
	);
}
