/* Copyright Reflection Contributors 2024-2026 */

#pragma once

#include "PluginUtils.h"
#include "Interfaces/IPluginManager.h"

#include "Engine/Notifications.h"

inline TSharedPtr<IPlugin> GetPlugin(const FString& Name) {
	return IPluginManager::Get().FindPlugin(Name);
}

/* Creates a plugin in the name (may result in bugs if inputted wrong) */
static void CreatePlugin(FString PluginName) {
	/* Plugin creation is different between UE5 and UE4 */
#if ENGINE_UE5
	FPluginUtils::FNewPluginParamsWithDescriptor CreationParams;
	CreationParams.Descriptor.bCanContainContent = true;

	CreationParams.Descriptor.FriendlyName = PluginName;
	CreationParams.Descriptor.Version = 1;
	CreationParams.Descriptor.VersionName = TEXT("1.0");
	CreationParams.Descriptor.Category = TEXT("Other");

	FText FailReason;
	FPluginUtils::FLoadPluginParams LoadParams;
	LoadParams.bEnablePluginInProject = true;
	LoadParams.bUpdateProjectPluginSearchPath = true;
	LoadParams.bSelectInContentBrowser = false;

	FPluginUtils::CreateAndLoadNewPlugin(PluginName, FPaths::ProjectPluginsDir(), CreationParams, LoadParams);
#else
	FPluginUtils::FNewPluginParams CreationParams;
	CreationParams.bCanContainContent = true;

	FText FailReason;
	FPluginUtils::FMountPluginParams LoadParams;
	LoadParams.bEnablePluginInProject = true;
	LoadParams.bUpdateProjectPluginSearchPath = true;
	LoadParams.bSelectInContentBrowser = false;

	FPluginUtils::CreateAndMountNewPlugin(PluginName, FPaths::ProjectPluginsDir(), CreationParams, LoadParams, FailReason);
#endif

#define LOCTEXT_NAMESPACE "UMG"
#if WITH_EDITOR
	/* Setup notification's arguments */
	FFormatNamedArguments Args;
	Args.Add(TEXT("PluginName"), FText::FromString(PluginName));

	/* Create notification */
	FNotificationInfo Info(FText::Format(LOCTEXT("PluginCreated", "Plugin Created: {PluginName}"), Args));
	Info.ExpireDuration = 10.0f;
	Info.bUseLargeFont = true;
	Info.bUseSuccessFailIcons = false;
	Info.WidthOverride = FOptionalSize(350);
	SetNotificationSubText(Info, FText::FromString(FString("Created successfully")));

	AddNotificationWhenSafe(Info, SNotificationItem::CS_Success);
#endif
#undef LOCTEXT_NAMESPACE
}
