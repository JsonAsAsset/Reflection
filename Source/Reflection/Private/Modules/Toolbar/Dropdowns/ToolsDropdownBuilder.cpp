/* Copyright Reflection Contributors 2024-2026 */

#include "Modules/Toolbar/Dropdowns/ToolsDropdownBuilder.h"

#include "Importers/Constructor/Importer.h"
#include "Importers/Constructor/ImportJob.h"
#include "Importers/Constructor/ImportReader.h"

#if ENGINE_UE4
#include "Modules/Toolbar/Dropdowns/CloudToolsDropdownBuilder.h"
#endif

#include "Engine/EngineUtilities.h"

#include "Modules/Toolbar/Tools/ClearImportData.h"
#include "Modules/Toolbar/Tools/FixUpAssetData.h"
#include "Utilities/DialogUtilities.h"

void IToolsDropdownBuilder::Build(FMenuBuilder& MenuBuilder) const {
	UReflectionSettings* Settings = GetSettings();
	
	MenuBuilder.AddSubMenu(
		FText::FromString("Asset Tools"),
		FText::FromString("Tools bundled"),
		FNewMenuDelegate::CreateLambda([this, Settings](FMenuBuilder& InnerMenuBuilder) {
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

				InnerMenuBuilder.AddMenuEntry(
					FText::FromString("Import Folder of JSON Files"),
					FText::FromString(""),
					FSlateIcon(FAppStyle::GetAppStyleSetName(), "LevelEditor.BspMode"),

					FUIAction(
						FExecuteAction::CreateLambda([] {
							TArray<FString> JsonFiles;

							for (const FString& Folder : OpenFolderDialog("Folder of JSON files")) {
								IFileManager::Get().FindFilesRecursive(
									JsonFiles,
									*Folder,
									TEXT("*.json"),
									true,
									true,
									/* bClearFileNames */ false
								);
							}

							/* A folder of JSON is exactly the case that used to lock the editor up
							 * for minutes, so it goes through the sliced job */
							FImportJob::Enqueue(JsonFiles);
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
}
