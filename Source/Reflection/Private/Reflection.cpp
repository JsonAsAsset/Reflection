/* Copyright Reflection Contributors 2024-2026 */

#include "Reflection.h"
#include "Utilities/JsonUtilities.h"

/* ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ */
#if ENGINE_UE4
#include "ToolMenus.h"
#include "LevelEditor.h"
#endif

#include "Http.h"
#include "Modules/Versioning.h"

#include "Modules/UI/StyleModule.h"
#include "Modules/UI/Validation/ValidationTab.h"
#include "Modules/Toolbar/Toolbar.h"
#include "Engine/EngineUtilities.h"

#include "Logging/LogVerbosity.h"
#include "Settings/Runtime.h"
/* ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ */

#ifdef _MSC_VER
#undef GetObject
#endif

void FReflectionModule::StartupModule() {
	LogHttp.SetVerbosity(ELogVerbosity::Error);

	FRMetadata::Initialize();
	
    /* Initialize plugin style, reload textures */
    FReflectionStyle::Initialize();
    FReflectionStyle::ReloadTextures();

	/* Register tabs, the style has to exist first for their icons */
#if ENGINE_UE5
	FValidationTab::Register();
#endif

    /* Register Toolbar */
	Toolbar = NewObject<UReflectionToolbar>();
	Toolbar->AddToRoot();
	
#if ENGINE_UE5
	UToolMenus::RegisterStartupCallback(FSimpleMulticastDelegate::FDelegate::CreateUObject(Toolbar, &UReflectionToolbar::Register));
#else
	{
    	const TSharedPtr<FUICommandList> PluginCommands = MakeShareable(new FUICommandList);

    	FLevelEditorModule& LevelEditorModule = FModuleManager::LoadModuleChecked<FLevelEditorModule>("LevelEditor");
    	const TSharedPtr<FExtender> ToolbarExtender = MakeShareable(new FExtender);
    	ToolbarExtender->AddToolBarExtension(
			"Settings",
			EExtensionHook::After,
			PluginCommands,
			FToolBarExtensionDelegate::CreateUObject(Toolbar, &UReflectionToolbar::UE4Register)
		);

    	LevelEditorModule.GetToolBarExtensibilityManager()->AddExtender(ToolbarExtender);
	}
#endif
	
    const UReflectionSettings* Settings = GetSettings();
	
	if (!Settings->Versioning.Disable) {
		GReflectionVersioning.Update();
	}

	GReflectionRuntime.Update();
}

void FReflectionModule::ShutdownModule() {
	/* Unregister startup callback and tool menus */
	UToolMenus::UnRegisterStartupCallback(this);
	UToolMenus::UnregisterOwner(this);

#if ENGINE_UE5
	/* The main menu bar entry is owned by the toolbar, not the module */
	if (Toolbar) {
		UToolMenus::UnregisterOwner(Toolbar);
	}

	FValidationTab::Unregister();
#endif

	/* Shutdown the plugin style */
	FReflectionStyle::Shutdown();

	if (Toolbar) {
		Toolbar->RemoveFromRoot();
		Toolbar = nullptr;
	}
}

IMPLEMENT_MODULE(FReflectionModule, Reflection)
