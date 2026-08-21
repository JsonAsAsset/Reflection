/* Copyright Reflection Contributors 2024-2026 */

#include "Modules/Toolbar/Dropdowns/ToolsDropdownBuilder.h"

#include "Importers/Constructor/Importer.h"

#if ENGINE_UE4
#include "Modules/Toolbar/Dropdowns/CloudToolsDropdownBuilder.h"
#endif

#include "Engine/EngineUtilities.h"

#include "Modules/Toolbar/Tools/ClearImportData.h"
#include "Modules/Toolbar/Tools/FixUpAssetData.h"
#include "Utilities/Dialog.h"

void IToolsDropdownBuilder::Build(FMenuBuilder& MenuBuilder) const {
	MenuBuilder.BeginSection("ReflectionAssetToolsSection", FText::FromString("Tools"));

	MenuBuilder.AddSubMenu(
		FText::FromString("Asset Tools"),
		FText::FromString("Tools bundled"),
		FNewMenuDelegate::CreateLambda([this](FMenuBuilder& InnerMenuBuilder) {
			InnerMenuBuilder.BeginSection("ReflectionToolsSection", FText::FromString("Tools"));
			{
				InnerMenuBuilder.AddMenuEntry(
					FText::FromString("Clear Import Data"),
					FText::FromString(""),
					FSlateIcon(FAppStyle::GetAppStyleSetName(), "LevelEditor.BspMode"),

					FUIAction(
						FExecuteAction::CreateLambda([] {
							TToolClearImportData* Tool = new TToolClearImportData();
							Tool->Execute();
						})
					),
					NAME_None
				);

				InnerMenuBuilder.AddMenuEntry(
					FText::FromString("Fixup Asset Data"),
					FText::FromString(""),
					FSlateIcon(FAppStyle::GetAppStyleSetName(), "LevelEditor.BspMode"),

					FUIAction(
						FExecuteAction::CreateLambda([] {
							TToolFixUpAssetData* Tool = new TToolFixUpAssetData();
							Tool->Execute();
						})
					),
					NAME_None
				);

				InnerMenuBuilder.EndSection();
			}
		}),
		false,
		FSlateIcon(FAppStyle::GetAppStyleSetName(), "DeveloperTools.MenuIcon")
	);

	MenuBuilder.EndSection();
}
