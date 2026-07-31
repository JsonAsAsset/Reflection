/* Copyright Reflection Contributors 2024-2026 */

#pragma once

#include "Engine/Compatibility.h"

/* FPluginUtils lives in the PluginUtils plugin, which did not ship before 4.26 */
#if UE4_25_BELOW
#include "Interfaces/IProjectManager.h"
#include "PluginDescriptor.h"
#include "HAL/PlatformFilemanager.h"
#include "Misc/Paths.h"
#else
#include "PluginUtils.h"
#endif

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
#elif UE4_25_BELOW
	/* Without FPluginUtils the content only plugin has to be laid down by hand. These are the
	 * same steps FPluginUtils::CreateAndMountNewPlugin takes once the code paths, icon, and
	 * template folders it also handles are stripped out, none of which apply here. */
	const FString PluginFolder = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectPluginsDir(), PluginName));
	const FString PluginFilePath = FPaths::Combine(PluginFolder, PluginName + TEXT(".uplugin"));
	const FString PluginContentFolder = FPaths::Combine(PluginFolder, TEXT("Content"));

	IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();

	FText FailReason;

	const bool bFoldersReady =
		(PlatformFile.DirectoryExists(*PluginFolder) || PlatformFile.CreateDirectoryTree(*PluginFolder)) &&
		(PlatformFile.DirectoryExists(*PluginContentFolder) || PlatformFile.CreateDirectoryTree(*PluginContentFolder));

	if (bFoldersReady) {
		FPluginDescriptor Descriptor;
		Descriptor.FriendlyName = PluginName;
		Descriptor.Version = 1;
		Descriptor.VersionName = TEXT("1.0");
		Descriptor.Category = TEXT("Other");
		Descriptor.bCanContainContent = true;

		if (Descriptor.Save(PluginFilePath, FailReason)) {
			/* Project/Plugins is already a search path, so registering it is just a refresh */
			IPluginManager::Get().RefreshPluginsList();

			IProjectManager::Get().SetPluginEnabled(PluginName, true, FailReason);

			/* Mounts the content folder and loads any modules, of which there are none here */
			IPluginManager::Get().MountNewlyCreatedPlugin(PluginName);
		}
	}
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
