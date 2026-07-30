/* Copyright Reflection Contributors 2024-2026 */

#include "Modules/Toolbar/Toolbar.h"

#include "Engine/Compatibility.h"
#include "Engine/EngineUtilities.h"

#include "Modules/UI/StyleModule.h"
#include "Importers/Constructor/ImportJob.h"
#include "Importers/Constructor/ImportReader.h"
#include "Modules/Metadata.h"
#include "Modules/Cloud/Cloud.h"
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
#include "Utilities/DialogUtilities.h"

static TWeakPtr<SNotificationItem> WaitingForCloud;

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

	FToolMenuSection& Section =
		UseToolbar
		? Menu->FindOrAddSection(GReflectionName)
		: Menu->FindOrAddSection("New");

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
		
		FText::FromString(FRMetadata::Version),
		
		FText::FromString("Execute Reflection"),
		
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

	AddCloudButtons(Section);

	/* Validation lives on the main menu bar, not in here */
	RegisterMainMenu();
#endif
}

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

void UReflectionToolbar::AddCloudButtons(FToolMenuSection& Section) {
#if ENGINE_UE5
	/* Adds the Cloud button to the toolbar */
	FToolMenuEntry& ActionButton = Section.AddEntry(FToolMenuEntry::InitToolBarButton(
		"ReflectionCloud",
		FToolUIActionChoice(
			FUIAction(
				FExecuteAction::CreateLambda([this] {
					UReflectionSettings* Settings = GetSettings();
					
					Settings->EnableCloudServer = !Settings->EnableCloudServer;
					SavePluginSettings(Settings);
				}),
				FCanExecuteAction(),
				FGetActionCheckState(),
				FIsActionButtonVisible::CreateStatic(&IsToolBarVisible)
			)
		),
		TAttribute<FText>::CreateLambda([this] {
			const UReflectionSettings* Settings = GetSettings();
			
			return Settings->EnableCloudServer ? FText::FromString("On") : FText::FromString("Off");
		}),
		FText::FromString(""),
		FSlateIcon(FReflectionStyle::Get().GetStyleSetName(), FName("Toolbar.Cloud")),
		EUserInterfaceActionType::Button
	));
	
	ActionButton.StyleNameOverride = "CalloutToolbar";

	/* Menu dropdown */
	const FToolMenuEntry MenuButton = Section.AddEntry(FToolMenuEntry::InitComboButton(
		"ReflectionCloudMenu",
		FUIAction(
			FExecuteAction(),
			FCanExecuteAction::CreateLambda([] {
				return GetSettings()->EnableCloudServer;
			}),
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
		FText::FromString(FRMetadata::Version),
		FText::FromString("Execute Reflection"),
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
		FText::FromString(FRMetadata::Version),
		FText::FromString(""),
		FSlateIcon(FReflectionStyle::Get().GetStyleSetName(), FName("Toolbar.Icon")),
		true
	);
	
	UE4CloudRegister(Builder);
}

void UReflectionToolbar::UE4CloudRegister(FToolBarBuilder& Builder) {
	Builder.AddToolBarButton(
		FUIAction(
			FExecuteAction::CreateLambda([this] {
				UReflectionSettings* Settings = GetSettings();
						
				Settings->EnableCloudServer = !Settings->EnableCloudServer;
				SavePluginSettings(Settings);
			}),
			FCanExecuteAction(),
			FGetActionCheckState(),
			FIsActionButtonVisible::CreateStatic(&IsToolBarVisible)
		),
		NAME_None,
		TAttribute<FText>::Create([this] {
			const UReflectionSettings* Settings = GetSettings();
			
			return Settings->EnableCloudServer ? FText::FromString("On") : FText::FromString("Off");
		}),
		FText::FromString(""),
		FSlateIcon(FReflectionStyle::Get().GetStyleSetName(), FName("Toolbar.Cloud"))
	);

	Builder.AddComboButton(
		FUIAction(
			FExecuteAction(),
			FCanExecuteAction::CreateLambda([] {
				return GetSettings()->EnableCloudServer;
			}),
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

		FString Dots;
		for (int32 i = 0; i < CloudDotCount; ++i) {
			Dots += TEXT(".");
		}

		WaitingForCloud.Pin()->SetText(
			FText::FromString(FString::Printf(TEXT("Establishing Cloud%s"), *Dots))
		);
	}
	
	if (!GetSettings()->EnableCloudServer || !Cloud::Status::IsOpened()) {
		CancelWaitForCloudTimer();
		
		return;
	}

	Cloud::Status::IsReady([this](const bool bReady) {
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
	const UReflectionSettings* Settings = GetSettings();

	if (!Settings->EnableCloudServer) {
		OnResponse(true);
		
		return;
	}

	Cloud::Status::Check(Settings,[this, OnResponse](const bool bStatusOk) {
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
	
	IsFitToFunction([this](const bool bAllowed) {
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

	/* Dialog for a JSON File */
	const TArray<FString> OutFileNames = OpenFileDialog("Select a JSON File", "JSON Files|*.json");
	if (OutFileNames.Num() == 0) {
		return;
	}

	/* Imported a slice at a time, so the editor keeps drawing for however long it takes */
	FImportJob::Enqueue(OutFileNames);
}

void UReflectionToolbar::HandleCloudWaiting() {
	if (!Cloud::Status::ShouldWaitUntilInitialized(GetSettings()) || WaitingForCloud.IsValid()) return;
	
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

