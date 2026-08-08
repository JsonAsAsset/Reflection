/* Copyright Reflection Contributors 2024-2026 */

#include "Modules/Toolbar/Dropdowns/GeneralDropdownBuilder.h"

#include "Engine/Compatibility.h"
#include "Engine/EngineUtilities.h"

#include "Modules/Metadata.h"
#include "Modules/UI/SupportedAssets/SupportedAssetsTab.h"

void IGeneralDropdownBuilder::Build(FMenuBuilder& MenuBuilder) const {
	MenuBuilder.BeginSection("ReflectionPluginSection", FText::FromString(FRMetadata::Version));

	MenuBuilder.AddMenuEntry(
		FText::FromString("Supported Assets"),
		FText::FromString("Every asset type Reflection can build"),
#if ENGINE_UE5
		FSlateIcon(FAppStyle::GetAppStyleSetName(), "LevelEditor.Tabs.ContentBrowser"),
#else
		FSlateIcon(FEditorStyle::GetStyleSetName(), "LevelEditor.Tabs.ContentBrowser"),
#endif
		FUIAction(
			FExecuteAction::CreateLambda([]() {
				FSupportedAssetsTab::Open();
			})
		),
		NAME_None
	);

	MenuBuilder.AddMenuEntry(
		FText::FromString("Settings"),
		FText::FromString("Navigate to Settings"),
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

	MenuBuilder.EndSection();
}
