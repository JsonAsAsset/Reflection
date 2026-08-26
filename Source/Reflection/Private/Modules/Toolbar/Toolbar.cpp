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
#include "Settings/Runtime.h"
#include "Modules/Toolbar/Dropdowns/GeneralDropdownBuilder.h"
#include "Modules/Toolbar/Dropdowns/DonateDropdownBuilder.h"
#include "Modules/Toolbar/Dropdowns/ParentDropdownBuilder.h"
#include "Modules/Toolbar/Dropdowns/ToolsDropdownBuilder.h"
#include "Modules/Toolbar/Dropdowns/VersioningDropdownBuilder.h"
#include "Settings/Static.h"
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

static TWeakPtr<SNotificationItem> WaitingForCloud;

/* When the press that is still waiting on an answer went in, or zero when none is. */
static double ImportActionStartedAt = 0.0;

/* An answer that hasn't arrived by now isn't coming, and a button that never works again is worse than a second window */
static constexpr double ImportActionTimeoutSeconds = 60.0;

/* Whether the wait loop below has a status request out. The loop asks every fifth of a second,
 * which is faster than a starting Cloud answers, so without this the answers stack up and each one
 * starts the run again. */
static bool ReadyCheckPending = false;

/* The button is a readout, not a switch: nothing about it turns the Cloud on or off, because
 * whether the app is running is not this editor's decision to make. Pressed while it is down, the
 * useful thing to do is say where to get one. */
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
		Section.AddSeparator(NAME_None);
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
		EmbeddedSection.AddSeparator(NAME_None);
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
			if (!Cloud::EnsureMetadataBlocking()) {
				SpawnPrompt("Reflection", "Cloud didn't say which project it has loaded, so paths can't be resolved.");

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
			/* The dialog fills itself in from the Content Browser selection */
			TToolImportFolder().Execute();
		}
	);
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
}

void UReflectionToolbar::PopulateValidationMenu(FMenuBuilder& MenuBuilder) {
	IValidationDropdownBuilder().Build(MenuBuilder);
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

	ActionButton.StyleNameOverride = "CalloutToolbar";

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
#if ENGINE_UE5
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

void UReflectionToolbar::WaitForCloudTimerCallback() {
	if (WaitingForCloud.IsValid()) {
		CloudDotCount = (CloudDotCount + 1) % 4;

		/* Appended rather than formatted: no dots leaves an FString that never allocated, and the
		 * pointer %s gets out of one of those is null, which prints as "(null)" once every four
		 * ticks */
		WaitingForCloud.Pin()->SetText(
			FText::FromString(TEXT("Establishing Cloud") + FString::ChrN(CloudDotCount, TEXT('.')))
		);
	}
	
	/* The Cloud was running when the wait started, so it going away now means it stopped or
	 * restarted underneath us. Dropping the wait without saying so looks exactly like the reflect
	 * button doing nothing at all. */
	if (!Cloud::Status::IsOpened()) {
		CancelWaitForCloudTimer();

		AppendNotification(
			FText::FromString("Cloud Stopped"),
			FText::FromString("It went away while starting up. Reflect again once it is running."),
			4.0f,
			FReflectionStyle::Get().GetBrush("Toolbar.Icon"),
			SNotificationItem::CS_Fail,
			false,
			310.0f
		);

		return;
	}

	if (ReadyCheckPending) {
		return;
	}

	ReadyCheckPending = true;

	Cloud::Status::IsReady([this](const bool bReady) {
		ReadyCheckPending = false;

		if (!bReady) {
			return;
		}

		CancelWaitForCloudTimer();
		ImportAction();
	});
}

void UReflectionToolbar::CancelWaitForCloudTimer() {
	RemoveNotification(WaitingForCloud);
	GEditor->GetTimerManager()->ClearTimer(WaitForCloudTimer);
}

void UReflectionToolbar::IsFitToFunction(TFunction<void(bool)> OnResponse) {
	/* A json file is imported from disk whether or not Cloud is up, so nothing has to be running
	 * for the run to go ahead. Cloud still answers references while it is open. */
	if (GetSettings()->UseLocalJson) {
		OnResponse(true);

		return;
	}

	Cloud::Status::Check([this, OnResponse](const bool bStatusOk) {
		if (!bStatusOk) {
			OnResponse(false);

			return;
		}

		Cloud::Update([OnResponse](const bool bUpdated) {
			OnResponse(bUpdated);
		});
	});
}

void UReflectionToolbar::ImportAction() {
	if (WaitingForCloud.IsValid()) return;

	const double Now = FPlatformTime::Seconds();

	/* A press already on its way in */
	if (ImportActionStartedAt != 0.0 && Now - ImportActionStartedAt < ImportActionTimeoutSeconds) {
		return;
	}

	ImportActionStartedAt = Now;

	IsFitToFunction([this](const bool bAllowed) {
		ImportActionStartedAt = 0.0;

		if (!bAllowed) {
			HandleCloudWaiting();
			
			return;
		}

		Import();
	});
}

void UReflectionToolbar::Import() {
	/* Update Runtime */
	GReflectionRuntime.Update();

	/* Redirect history is what lets a path be turned back into the one the Cloud knows, so
	 * clearing it under a job that is still running would strand its remaining references */
	if (!FImportJob::IsRunning()) {
		FRRedirects::Clear();
	}

	CancelWaitForCloudTimer();

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

void UReflectionToolbar::HandleCloudWaiting() {
	if (!Cloud::Status::ShouldWaitUntilInitialized() || WaitingForCloud.IsValid()) return;
	
	WaitingForCloud =
		AppendNotificationWithHandler(
			FText::FromString("Establishing Cloud"),
			FText::FromString(""),
			999.0f,
			FReflectionStyle::Get().GetBrush("Toolbar.Icon"),
			SNotificationItem::CS_Pending,
			false,
			0.0f);

	GEditor->GetTimerManager()->SetTimer(
		WaitForCloudTimer,
		FTimerDelegate::CreateUObject(this, &UReflectionToolbar::WaitForCloudTimerCallback),
		0.2f,
		true);
}

TSharedRef<SWidget> UReflectionToolbar::CreateMenuDropdown() {
	FMenuBuilder MenuBuilder(false, nullptr);

	TArray<TSharedRef<IParentDropdownBuilder>> Dropdowns = {
		MakeShared<IVersioningDropdownBuilder>(),
		MakeShared<IParentDropdownBuilder>(),
		MakeShared<IToolsDropdownBuilder>(),
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

