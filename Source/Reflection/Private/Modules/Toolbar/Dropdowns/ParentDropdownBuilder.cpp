/* Copyright Reflection Contributors 2024-2026 */

#include "Modules/Toolbar/Dropdowns/ParentDropdownBuilder.h"

#include "Reflection.h"
#include "Modules/Cloud/Cloud.h"
#include "Modules/Toolbar/Tools/ImportFolder.h"
#include "Engine/Compatibility.h"
#include "Engine/EngineUtilities.h"

void IParentDropdownBuilder::Build(FMenuBuilder& MenuBuilder) const {
	MenuBuilder.BeginSection(
		"ReflectionSection",
		FText::FromString("Plugin")
	);

	/* The button reflects one path, this reflects all of them under a folder. Same way in, so it
	 * sits with the button rather than off in the Cloud menu. */
	MenuBuilder.AddMenuEntry(
		FText::FromString("Folder"),
		FText::FromString("Reflect assets under a folder of the game files, not one in this project"),
		FSlateIcon(FAppStyle::GetAppStyleSetName(), "ContentBrowser.AssetTreeFolderOpen"),

		FUIAction(
			FExecuteAction::CreateLambda([] {
				TToolImportFolder Tool;
				Tool.Execute();
			}),

			FCanExecuteAction::CreateLambda([] {
				return Cloud::Status::IsOpened();
			})
		),
		NAME_None
	);

	MenuBuilder.EndSection();
}
