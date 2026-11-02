/* Copyright Reflection Contributors 2024-2026 */

#include "Modules/Toolbar/Toolbar.h"

#include "Engine/Compatibility.h"
#include "Engine/EngineUtilities.h"
#include "Utilities/Process.h"

#include "Modules/UI/StyleModule.h"
#include "ContentBrowserModule.h"
#include "Engine/AssetCompatibility.h"
#include "Importers/Constructor/ImportIssues.h"
#include "Importers/Constructor/ImportJob.h"
#include "Importers/Constructor/ImportReader.h"
#include "Modules/Metadata.h"
#include "Modules/Cloud/Cloud.h"
#include "Modules/Toolbar/Tools/ImportFolder.h"
#include "Modules/Toolbar/Tools/ImportFromPath.h"
#if ENGINE_UE5
#include "Modules/Toolbar/Dropdowns/ValidationDropdownBuilder.h"
#endif
#include "Modules/Toolbar/Dropdowns/CloudToolsDropdownBuilder.h"
#include "Modules/Tools/SelectedAssetsBase.h"
#include "Modules/Tools/ToolRegistry.h"

#include "Algo/Sort.h"
#include "Settings/Runtime.h"
#include "Modules/Toolbar/Dropdowns/GeneralDropdownBuilder.h"
#include "Modules/Toolbar/Dropdowns/DonateDropdownBuilder.h"
#include "Modules/Toolbar/Dropdowns/ParentDropdownBuilder.h"
#include "Modules/Toolbar/Dropdowns/ToolsDropdownBuilder.h"
#include "Modules/Toolbar/Dropdowns/VersioningDropdownBuilder.h"
#include "Settings/Static.h"
#include "Utilities/ContentBrowser.h"
#include "Utilities/Dialog.h"
#include "Widgets/Layout/SBox.h"

/* Every menu type below comes from here. Later engines drag it in behind one of the includes
 * above, 5.0 does not, so name it rather than depend on the shared PCH's include graph. */
#if !UE4_23_BELOW
#include "ToolMenus.h"
#endif

/* 4.25 and below build this module without the engine's shared PCH (see Reflection.Build.cs),
 * which is where the multi box builders used to come in from */
#if UE4_25_BELOW
#include "Framework/MultiBox/MultiBoxBuilder.h"
#endif

static void CloudButtonPressed() {
	if (!Cloud::Status::IsOpened()) {
		LaunchURL(GitHub::README::Cloud);
	}
}

static FText GetCloudButtonLabel() {
	return Cloud::Status::IsOpened() ? FText::FromString("On") : FText::FromString("Off");
}

static FText GetCloudButtonTooltip() {
	return Cloud::Status::IsOpened()
		? FText::FromString("Cloud is running.")
		: FText::FromString("Cloud isn't running. Click to read how to set one up.");
}

/* The dropdown is only worth opening when there is something behind it */
static bool CanOpenCloudMenu() {
	return Cloud::Status::IsOpened();
}

#if ENGINE_UE5
/* Reflection ships no validation artwork, so the editor's own stands in */
static FSlateIcon GetValidationIcon() {
	return FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.Validate");
}

/* The same one the tools menu carried while it was a submenu */
static FSlateIcon GetAssetToolsIcon() {
	return FSlateIcon(FAppStyle::GetAppStyleSetName(), "DeveloperTools.MenuIcon");
}
#endif

void UReflectionToolbar::Register() {
#if ENGINE_UE5
	/* false: uses top toolbar. true: uses content browser toolbar */
	static bool UseToolbar = false;

	UToolMenu* Menu;

	if (UseToolbar) {
		Menu = UToolMenus::Get()->ExtendMenu("LevelEditor.LevelEditorToolBar.PlayToolBar");
	} else {
		Menu = UToolMenus::Get()->ExtendMenu("ContentBrowser.Toolbar");
	}

	if (UseToolbar) {
		FToolMenuSection& Section = Menu->FindOrAddSection(GReflectionName);

		AddReflectionButtons(Section);
		AddCloudButtons(Section);
	} else {
		static const FName EmbeddedToolbarName("Reflection.EmbeddedToolbar");

		/* Registering this here, rather than at Reflection's own module startup, is what keeps
		 * it from borrowing the engine's toolbar style before that style exists */
		FReflectionStyle::EnsureEmbeddedToolbarStyleRegistered();

		UToolMenu* EmbeddedToolbar = UToolMenus::Get()->RegisterMenu(EmbeddedToolbarName, NAME_None, EMultiBoxType::SlimHorizontalToolBar);

		/* Pointing the menu at Reflection's own style set is a 5.4 luxury. Below that the set is
		 * private and always the app style, which is where EnsureEmbeddedToolbarStyleRegistered
		 * has just put the style, so the name alone is enough to find it. */
#if UE5_4_BEYOND
		EmbeddedToolbar->SetStyleSet(&FReflectionStyle::Get());
#endif
		EmbeddedToolbar->StyleName = FReflectionStyle::GetEmbeddedToolbarStyleName();

		FToolMenuSection& EmbeddedSection = EmbeddedToolbar->FindOrAddSection("ReflectionEmbeddedSection");

		AddReflectionButtons(EmbeddedSection);
		AddCloudButtons(EmbeddedSection);

		FToolMenuSection& Section = Menu->FindOrAddSection("New");

		Section.AddDynamicEntry("ReflectionEmbeddedToolbar", FNewToolMenuSectionDelegate::CreateLambda([](FToolMenuSection& InSection) {
			InSection.AddEntry(FToolMenuEntry::InitWidget(
				"ReflectionEmbeddedToolbar",
				SNew(SBox)
				.Visibility_Static([]() {
					return UReflectionToolbar::IsToolBarVisible() ? EVisibility::Visible : EVisibility::Collapsed;
				})
				[
					UToolMenus::Get()->GenerateWidget(EmbeddedToolbarName, FToolMenuContext())
				],
				FText::GetEmpty(),
				true,
				false
			));
		}));
	}

	/* Validation lives on the main menu bar, not in here */
	RegisterMainMenu();
#endif

#if !UE4_25_BELOW
	RegisterAssetContextMenu();
#endif
}

#if !UE4_25_BELOW
void UReflectionToolbar::RegisterAssetContextMenu() {
	UToolMenus* ToolMenus = UToolMenus::Get();
	if (ToolMenus == nullptr) return;

	/* Placed after an existing section: found or added lands at the bottom */
	const auto AddReflectSection = [](UToolMenu* Menu, const FName After, const FText& Tooltip, TFunction<void()> OnReflect) {
		if (Menu == nullptr) return;

		FToolMenuSection& Section = Menu->AddSection(
			GReflectionName,
			FText::FromString(GReflectionName.ToString()),
			FToolMenuInsert(After, EToolMenuInsertType::After)
		);

		Section.AddMenuEntry(
			GReflectionName,
			FText::FromString("Reflect"),
			Tooltip,
			FSlateIcon(FReflectionStyle::Get().GetStyleSetName(), "Toolbar.Icon"),

			FUIAction(
				FExecuteAction::CreateLambda(OnReflect),

				FCanExecuteAction::CreateLambda([] {
					return Cloud::Status::IsOpened();
				})
			)
		);
	};

	AddReflectSection(
		ToolMenus->ExtendMenu("ContentBrowser.AssetContextMenu"),
		TEXT("GetAssetActions"),
		FText::FromString("Reflects the selected assets from Cloud, replacing what is in the project."),
		[] {
			const FContentBrowserModule& ContentBrowser = FModuleManager::LoadModuleChecked<FContentBrowserModule>("ContentBrowser");

			TArray<FAssetData> SelectedAssets;
			ContentBrowser.Get().GetSelectedAssets(SelectedAssets);

			/* And reached without going through either reflect tool, so the project name the
			 * exports are resolved against is fetched here too. Asked once for the press rather
			 * than once for each asset, since the answer is the same either way. */
			/* Cancelled, which is the reader saying they are done waiting */
			if (!Cloud::EnsureMetadataBlocking()) {
				return;
			}

			/* Reached without going through either reflect tool, so the report is opened and
			 * closed here as well, or whatever the selection had to say would sit unshown until
			 * the next run cleared it */
			FImportIssues::Begin();

			/* One at a time, so a miss does not take the rest of the selection with it */
			for (const FAssetData& AssetData : SelectedAssets) {
				const FString ObjectPath = GetAssetObjectPath(AssetData);
				if (ObjectPath.IsEmpty()) continue;

				TToolImportFromPath::Import(ObjectPath);
			}

			FImportIssues::Finish();
		}
	);

	AddReflectSection(
		ToolMenus->ExtendMenu("ContentBrowser.FolderContextMenu"),
		TEXT("PathViewFolderOptions"),
		FText::FromString("Opens Reflect Folder listing what the Cloud has under this folder."),
		[] {
			/* Named here rather than left to the dialog to work out: this way in is the folder that
			 * was clicked, so nothing else - the clipboard included - gets a say in it */
			TToolImportFolder().Execute(GetSelectedContentBrowserFolder());
		}
	);

	/* And the tools, under the asset they read rather than behind the Cloud dropdown.
	 *
	 * Reflect above takes the whole asset and puts it in the project's place. These take one part
	 * of it and leave the rest alone, which is what somebody who has already worked on the asset
	 * is after, and right-clicking the asset is where they would look for it. */
	if (UToolMenu* AssetMenu = ToolMenus->ExtendMenu("ContentBrowser.AssetContextMenu")) {
		FToolMenuSection& Section = AssetMenu->FindOrAddSection(GReflectionName);

		Section.AddSubMenu(
			TEXT("ReflectionTools"),
			FText::FromString("Reflect Part Of"),
			FText::FromString("Reads one part of the selected asset out of the game's copy, leaving the rest of it as it is."),

			FNewToolMenuDelegate::CreateStatic(&UReflectionToolbar::BuildToolsSubMenu),
			false,
			FSlateIcon(FReflectionStyle::Get().GetStyleSetName(), "Toolbar.Icon")
		);
	}
}

void UReflectionToolbar::BuildToolsSubMenu(UToolMenu* Menu) {
	if (Menu == nullptr) return;

	const FContentBrowserModule& ContentBrowser = FModuleManager::LoadModuleChecked<FContentBrowserModule>("ContentBrowser");

	TArray<FAssetData> Selected;
	ContentBrowser.Get().GetSelectedAssets(Selected);

	TSet<FName> Classes;

	for (const FAssetData& AssetData : Selected) {
		if (AssetData.IsValid()) Classes.Add(GetAssetDataClass(AssetData));
	}

	TArray<TSelectedAssetsBase*> Tools;

	for (const TPair<FName, FToolInstanceDelegate>& Registration : GetToolRegistry()) {
		TSelectedAssetsBase* Tool = Registration.Value();

		/* A tool with no name isn't meant to be reachable from a menu */
		if (Tool == nullptr || Tool->GetDisplayName().IsEmpty()) continue;

		/* One that takes anything is offered for anything, and the rest only where what was
		 * clicked is what they read */
		const FName Supported = Tool->GetSupportedClass();

		if (!Supported.IsNone() && !Classes.Contains(Supported)) continue;

		Tools.Add(Tool);
	}

	/* Registration order is the linker's business, so the menu sorts itself */
	Algo::Sort(Tools, [](const TSelectedAssetsBase* A, const TSelectedAssetsBase* B) {
		return A->GetDisplayName().CompareTo(B->GetDisplayName()) < 0;
	});

	FToolMenuSection& Section = Menu->AddSection(TEXT("ReflectionTools"), FText::FromString("Selected Assets"));

	for (TSelectedAssetsBase* Tool : Tools) {
		Section.AddMenuEntry(
			FName(*Tool->GetDisplayName().ToString()),
			Tool->GetDisplayName(),
			Tool->GetTooltip(),
			Tool->GetIcon(),

			FUIAction(
				FExecuteAction::CreateLambda([Tool] {
					Tool->Execute();
				}),

				FCanExecuteAction::CreateLambda([Tool] {
					return Cloud::Status::IsOpened() && !Tool->IsRunning();
				})
			)
		);
	}

	/* Said rather than left as an empty menu, which reads as something being broken */
	if (Tools.Num() == 0) {
		Section.AddMenuEntry(
			TEXT("ReflectionToolsNone"),
			FText::FromString("Nothing for this asset"),
			FText::FromString("No tool reads a part of this kind of asset."),
			FSlateIcon(),

			FUIAction(FExecuteAction(), FCanExecuteAction::CreateLambda([] { return false; }))
		);
	}
}
#endif

#if ENGINE_UE5
void UReflectionToolbar::RegisterMainMenu() {
	UToolMenus* ToolMenus = UToolMenus::Get();
	if (ToolMenus == nullptr) {
		return;
	}

	UToolMenu* MenuBar = ToolMenus->ExtendMenu("LevelEditor.MainMenu");
	if (MenuBar == nullptr) {
		return;
	}

	/* Scoped so ShutdownModule can tear the menu back down */
	FToolMenuOwnerScoped OwnerScoped(this);

	/* Sits after Window, which is where this project puts its other tool menus */
	FToolMenuSection& Section = MenuBar->AddSection(
		"ReflectionValidationSection",
		FText::GetEmpty(),
		FToolMenuInsert("WindowLayout", EToolMenuInsertType::After)
	);

	/* FNewToolMenuChoice takes an FMenuBuilder delegate as its legacy form, which lets the
	 * dropdown builder stay the single implementation of the menu's contents */
	Section.AddSubMenu(
		"Validation",
		FText::FromString("Validation"),
		FText::FromString("Validate this project's content against the real game files"),
		FNewToolMenuChoice(FNewMenuDelegate::CreateStatic(&UReflectionToolbar::PopulateValidationMenu)),
		false,
		GetValidationIcon()
	);

	/* Next to Validation, and for the same reason: it acts on what is already in the project
	 * rather than on anything being reflected, so the reflect button's own dropdown was never
	 * where anyone would look for it */
	Section.AddSubMenu(
		"AssetTools",
		FText::FromString("Asset Tools"),
		FText::FromString("Tools that work over the assets already in this project"),
		FNewToolMenuChoice(FNewMenuDelegate::CreateStatic(&UReflectionToolbar::PopulateAssetToolsMenu)),
		false,
		GetAssetToolsIcon()
	);
}

void UReflectionToolbar::PopulateValidationMenu(FMenuBuilder& MenuBuilder) {
	IValidationDropdownBuilder().Build(MenuBuilder);
}

void UReflectionToolbar::PopulateAssetToolsMenu(FMenuBuilder& MenuBuilder) {
	IToolsDropdownBuilder().Build(MenuBuilder);
}
#endif

#if !UE4_23_BELOW
void UReflectionToolbar::AddReflectionButtons(FToolMenuSection& Section) {
#if ENGINE_UE5
	/* Displays Reflection's icon along with the Version */
	FToolMenuEntry& ActionButton = Section.AddEntry(FToolMenuEntry::InitToolBarButton(
		GReflectionName,

		FToolUIActionChoice(
			FUIAction(
				FExecuteAction::CreateUObject(this, &UReflectionToolbar::ImportAction),
				FCanExecuteAction(),
				FGetActionCheckState(),
				FIsActionButtonVisible::CreateStatic(&IsToolBarVisible)
			)
		),

		FText::FromString(""),

		FText::FromString(""),

		FSlateIcon(FReflectionStyle::Get().GetStyleSetName(), FName("Toolbar.Icon")),

		EUserInterfaceActionType::Button
	));

	ActionButton.StyleNameOverride = FReflectionStyle::GetActionButtonStyleName();

	/* Menu dropdown */
	const FToolMenuEntry MenuButton = Section.AddEntry(FToolMenuEntry::InitComboButton(
		"ReflectionMenu",
		FUIAction(
			FExecuteAction(),
			FCanExecuteAction(),
			FGetActionCheckState(),
			FIsActionButtonVisible::CreateStatic(IsToolBarVisible)
		),
		FOnGetContent::CreateStatic(&CreateMenuDropdown),
		FText::FromString(GReflectionName.ToString()),
		FText::FromString(""),
		FSlateIcon(),
		true
	));
#endif
}

void UReflectionToolbar::AddCloudButtons(FToolMenuSection& Section) {
#if 0
	/* Divides these from the reflect button. It belongs to them rather than sitting in the caller:
	 * a slim horizontal toolbar lays its blocks out in a plain box that keeps a separator even when
	 * it is the last one, and this one's brush is the same recessed colour as the toolbar behind
	 * it, so with nothing after it, it reads as empty space off the right edge. */
	Section.AddSeparator(NAME_None);

	/* Adds the Cloud button to the toolbar */
	FToolMenuEntry& ActionButton = Section.AddEntry(FToolMenuEntry::InitToolBarButton(
		"ReflectionCloud",
		FToolUIActionChoice(
			FUIAction(
				FExecuteAction::CreateStatic(&CloudButtonPressed),
				FCanExecuteAction(),
				FGetActionCheckState(),
				FIsActionButtonVisible::CreateStatic(&IsToolBarVisible)
			)
		),
		/* CreateStatic is 5.1+; Create takes the same plain function pointer on every version */
		TAttribute<FText>::Create(&GetCloudButtonLabel),
		TAttribute<FText>::Create(&GetCloudButtonTooltip),
		FSlateIcon(FReflectionStyle::Get().GetStyleSetName(), FName("Toolbar.Cloud")),
		EUserInterfaceActionType::Button
	));
	
	ActionButton.StyleNameOverride = "CalloutToolbar";

	/* Menu dropdown */
	const FToolMenuEntry MenuButton = Section.AddEntry(FToolMenuEntry::InitComboButton(
		"ReflectionCloudMenu",
		FUIAction(
			FExecuteAction(),
			FCanExecuteAction::CreateStatic(&CanOpenCloudMenu),
			FGetActionCheckState(),
			FIsActionButtonVisible::CreateStatic(IsToolBarVisible)
		),
		FOnGetContent::CreateStatic(&CreateCloudMenuDropdown),
		FText::FromString(""),
		FText::FromString(""),
		FSlateIcon(),
		true
	));
#endif
}
#endif

#if ENGINE_UE4
void UReflectionToolbar::UE4Register(FToolBarBuilder& Builder) {
	Builder.AddToolBarButton(
		FUIAction(
			FExecuteAction::CreateUObject(this, &UReflectionToolbar::ImportAction),
			FCanExecuteAction(),
			FGetActionCheckState(),
			FIsActionButtonVisible::CreateStatic(IsToolBarVisible)
		),
		NAME_None,
		FText::FromString(""),
		FText::FromString(""),
		FSlateIcon(FReflectionStyle::Get().GetStyleSetName(), FName("Toolbar.Icon"))
	);

	Builder.AddComboButton(
		FUIAction(
			FExecuteAction(),
			FCanExecuteAction(),
			FGetActionCheckState(),
			FIsActionButtonVisible::CreateStatic(IsToolBarVisible)
		),
		FOnGetContent::CreateStatic(&UReflectionToolbar::CreateMenuDropdown),
		FText::FromString(""),
		FText::FromString(""),
		FSlateIcon(FReflectionStyle::Get().GetStyleSetName(), FName("Toolbar.Icon")),
		true
	);
	
	UE4CloudRegister(Builder);
}

void UReflectionToolbar::UE4CloudRegister(FToolBarBuilder& Builder) {
	Builder.AddToolBarButton(
		FUIAction(
			FExecuteAction::CreateStatic(&CloudButtonPressed),
			FCanExecuteAction(),
			FGetActionCheckState(),
			FIsActionButtonVisible::CreateStatic(&IsToolBarVisible)
		),
		NAME_None,
		TAttribute<FText>::Create(&GetCloudButtonLabel),
		TAttribute<FText>::Create(&GetCloudButtonTooltip),
		FSlateIcon(FReflectionStyle::Get().GetStyleSetName(), FName("Toolbar.Cloud"))
	);

	Builder.AddComboButton(
		FUIAction(
			FExecuteAction(),
			FCanExecuteAction::CreateStatic(&CanOpenCloudMenu),
			FGetActionCheckState(),
			FIsActionButtonVisible::CreateStatic(IsToolBarVisible)
		),
		FOnGetContent::CreateStatic(&CreateCloudMenuDropdown),
		FText::FromString(FRMetadata::Version),
		FText::FromString(""),
		FSlateIcon(FReflectionStyle::Get().GetStyleSetName(), FName("Toolbar.Cloud")),
		true
	);

}
#endif

IConsoleVariable* GReflectionButtonVisibility = nullptr;

bool UReflectionToolbar::IsToolBarVisible() {
	if (!GReflectionRuntime.bEnableToolbarToggling) {
		return true;
	}
	
	bool Visible = true;

	static bool bHasCheckedConsoleVariable = false;

	if (!bHasCheckedConsoleVariable) {
		GReflectionButtonVisibility = IConsoleManager::Get().FindConsoleVariable(TEXT("Toolbar.Tools.FlippedVisibility"));

		bHasCheckedConsoleVariable = true;
	}

	if (GReflectionButtonVisibility) {
		if (GReflectionButtonVisibility->GetInt() == 1) {
			Visible = false;
		}
	}

	if (GEditor) {
		for (const FWorldContext& WorldContext : GEditor->GetWorldContexts()) {
			if (WorldContext.World() && WorldContext.World()->WorldType == EWorldType::PIE) {
				Visible = false;
			}
		}
	}

	return Visible;
}

void UReflectionToolbar::ImportAction() {
	/* Straight to the window. Whether Cloud is up is the reader's question once they have said
	 * what to fetch, so it is asked after Reflect rather than before the window opens. */
	Import();
}

void UReflectionToolbar::Import() {
	/* Update Runtime */
	GReflectionRuntime.Update();

	/* Redirect history is what lets a path be turned back into the one the Cloud knows, so
	 * clearing it under a job that is still running would strand its remaining references */
	if (!FImportJob::IsRunning()) {
		FRRedirects::Clear();
	}

	/* A path is the direct way in, and having to export a json first to import something Cloud
	 * could have fetched is a step that does not need to exist. Built the other way, the file on
	 * disk is what is being imported, so the file dialog is the way in. */
	if (!GetSettings()->UseLocalJson) {
		TToolImportFromPath Tool;
		Tool.Execute();

		return;
	}

	/* Dialog for a JSON File */
	const TArray<FString> OutFileNames = OpenFileDialog("Select a JSON File", "JSON Files|*.json");
	if (OutFileNames.Num() == 0) {
		return;
	}

	/* Imported a slice at a time, so the editor keeps drawing for however long it takes */
	FImportJob::Enqueue(OutFileNames);
}


TSharedRef<SWidget> UReflectionToolbar::CreateMenuDropdown() {
	FMenuBuilder MenuBuilder(false, nullptr);

	TArray<TSharedRef<IParentDropdownBuilder>> Dropdowns = {
		MakeShared<IVersioningDropdownBuilder>(),

		/* UE5 carries these on the menu bar, alongside Validation. UE4 has no Reflection menu up
		 * there to hang them off, so the dropdown stays the way to them. */
#if ENGINE_UE4
		MakeShared<IToolsDropdownBuilder>(),
#endif

		MakeShared<IGeneralDropdownBuilder>(),
		MakeShared<IDonateDropdownBuilder>()
	};

	for (const TSharedRef<IParentDropdownBuilder>& Dropdown : Dropdowns) {
		Dropdown->Build(MenuBuilder);
	}

	return MenuBuilder.MakeWidget();
}

TSharedRef<SWidget> UReflectionToolbar::CreateCloudMenuDropdown() {
	FMenuBuilder MenuBuilder(false, nullptr);

	TArray<TSharedRef<IParentDropdownBuilder>> Dropdowns = {
		MakeShared<ICloudToolsDropdownBuilder>()
	};

	for (const TSharedRef<IParentDropdownBuilder>& Dropdown : Dropdowns) {
		Dropdown->Build(MenuBuilder);
	}

	return MenuBuilder.MakeWidget();
}

