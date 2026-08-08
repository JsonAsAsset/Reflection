/* Copyright Reflection Contributors 2024-2026 */

#include "Modules/Toolbar/Dropdowns/ReflectFromPathDropdownBuilder.h"

#include "Modules/Cloud/Cloud.h"
#include "Modules/Toolbar/Tools/ImportFolder.h"

void IReflectFromPathDropdownBuilder::Build(FMenuBuilder& MenuBuilder) const {
	MenuBuilder.BeginSection("ReflectionFromPathSection", FText::FromString("From Path"));

	MenuBuilder.AddMenuEntry(
		FText::FromString("Reflect Folder"),
		FText::FromString("Reflect every asset under a folder of the game files, not one in this project"),
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
