/* Copyright Reflection Contributors 2024-2026 */

#include "Reflection.h"

/* ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ */
#if ENGINE_UE4
#if !UE4_23_BELOW
#include "ToolMenus.h"
#endif
#include "LevelEditor.h"
#endif

#include "Http.h"
#include "Modules/Versioning.h"
#include "Modules/Support.h"

#include "Modules/UI/StyleModule.h"
#include "Modules/UI/SupportedAssets/SupportedAssetsTab.h"
#include "Modules/UI/Validation/ValidationTab.h"
#include "Modules/Toolbar/Toolbar.h"
#include "Engine/EngineUtilities.h"

#include "Logging/LogVerbosity.h"
#include "JsonGlobals.h"
#include "Settings/Runtime.h"
/* ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ */

#ifdef _MSC_VER
#undef GetObject
#endif

void FReflectionModule::StartupModule() {
	LogHttp.SetVerbosity(ELogVerbosity::Error);

	/* Exports are read by asking for fields that may not be there, which is how the reader tells
	 * an absent one from an empty one. Json warns on every miss, so one import can bury the log
	 * in complaints about something working as intended. */
	LogJson.SetVerbosity(ELogVerbosity::NoLogging);

	FRMetadata::Initialize();
	
    /* Initialize plugin style, reload textures */
    FReflectionStyle::Initialize();
    FReflectionStyle::ReloadTextures();

	/* Register tabs, the style has to exist first for their icons */
	FSupportedAssetsTab::Register();

#if ENGINE_UE5
	FValidationTab::Register();
#endif

    /* Register Toolbar */
	Toolbar = NewObject<UReflectionToolbar>();
	Toolbar->AddToRoot();
	
	/* UE4 needs this too, its Register is what puts Reflect in the Content Browser's right click menu */
#if !UE4_23_BELOW
	UToolMenus::RegisterStartupCallback(FSimpleMulticastDelegate::FDelegate::CreateUObject(Toolbar, &UReflectionToolbar::Register));
#endif

#if ENGINE_UE4
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
	
	GReflectionVersioning.Update();
	GReflectionRuntime.Update();

	FReflectionSupport::Register();
}

void FReflectionModule::ShutdownModule() {
	/* Unregister startup callback and tool menus */
#if !UE4_23_BELOW
	UToolMenus::UnRegisterStartupCallback(this);
	UToolMenus::UnregisterOwner(this);
#endif

	FSupportedAssetsTab::Unregister();

#if ENGINE_UE5
	/* The main menu bar entry is owned by the toolbar, not the module */
	if (Toolbar) {
		UToolMenus::UnregisterOwner(Toolbar);
	}

	FValidationTab::Unregister();
#endif

	FReflectionSupport::Unregister();

	/* Shutdown the plugin style */
	FReflectionStyle::Shutdown();

	if (Toolbar) {
#if UE4_23_BELOW
		if (!GIsRequestingExit) {
#else
		if (!IsEngineExitRequested()) {
#endif
			Toolbar->RemoveFromRoot();
		}
		Toolbar = nullptr;
	}
}

IMPLEMENT_MODULE(FReflectionModule, Reflection)
