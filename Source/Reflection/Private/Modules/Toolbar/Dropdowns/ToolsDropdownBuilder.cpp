/* Copyright Reflection Contributors 2024-2026 */

#include "Modules/Toolbar/Dropdowns/ToolsDropdownBuilder.h"

#include "Importers/Constructor/Importer.h"

#if ENGINE_UE4
#include "Modules/Toolbar/Dropdowns/CloudToolsDropdownBuilder.h"
#endif

#include "Engine/EngineUtilities.h"

#include "Modules/Toolbar/Tools/ClearImportData.h"
#include "Utilities/Dialog.h"

/* The entries themselves, with nothing wrapped around them: on UE5 this menu is the Asset Tools
 * item on the editor's own menu bar, and a submenu of the same name inside it would only be a
 * second door onto the same room. */
void IToolsDropdownBuilder::Build(FMenuBuilder& MenuBuilder) const {
	MenuBuilder.BeginSection("ReflectionAssetToolsSection", FText::FromString("Tools"));

	MenuBuilder.AddMenuEntry(
		FText::FromString("Clear Import Data"),
		FText::FromString("Strips the record of the file an asset was imported from, across the folder selected in the Content Browser"),
		FSlateIcon(FAppStyle::GetAppStyleSetName(), "LevelEditor.BspMode"),

		FUIAction(
			FExecuteAction::CreateLambda([] {
				TToolClearImportData Tool;
				Tool.Execute();
			})
		),
		NAME_None
	);

	MenuBuilder.EndSection();
}
