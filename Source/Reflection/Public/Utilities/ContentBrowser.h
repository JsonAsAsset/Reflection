/* Copyright Reflection Contributors 2024-2026 */

#pragma once

#include "IContentBrowserSingleton.h"
#include "ContentBrowserModule.h"
#include "Utilities/Dialog.h"
#include "Engine/Compatibility.h"

#include "Editor.h"
#include "TimerManager.h"

#include "AssetToolsModule.h"
#include "IAssetTools.h"

#include "ContentBrowserDataSubsystem.h"
#include "ContentBrowserItem.h"
#include "IContentBrowserDataModule.h"

/* AssetRegistryModule.h only moved under an AssetRegistry/ folder later on */
#if UE4_25_BELOW
#include "AssetRegistryModule.h"
#else
#include "AssetRegistry/AssetRegistryModule.h"
#endif
#include "Engine/Log.h"

/* Holds the Content Browser still for the length of a reflect.
 *
 * Every asset a reflect creates asks to be jumped to, and a reflect creates every reference it
 * reaches, so an asset with a lot of them drags the browser through its whole dependency tree and
 * finishes wherever the last reference happened to land. */
namespace PendingBrowse {
	inline int32 Depth = 0;

	/* References are created while their parent is still deserializing, so the parent is the last
	 * to ask, which is the one worth keeping */
	inline TWeakObjectPtr<UObject> Target;

	inline FTimerHandle Timer;

	/* How often the browser is asked whether it can see the asset yet, and for how long before
	 * the attempt is written off. Any fixed wait is a guess that a slow import loses. */
	constexpr float RetryRate = 0.1f;
	constexpr float Timeout = 10.0f;

	inline double Deadline = 0.0;
}

/* Whether the Content Browser can see Asset yet.
 *
 * Syncing is asynchronous in the sense that matters here: the asset registry is told the moment
 * the asset exists, but the browser only learns of it when its data subsystem next ticks, and a
 * sync to an item it has not heard of is discarded without a word. This is the same lookup
 * SContentBrowser::SyncToItems does, so it answers the only question worth asking. */
inline bool IsAssetInContentBrowser(const UObject* Asset, FContentBrowserItem& OutItem) {
	UContentBrowserDataSubsystem* ContentBrowserData = IContentBrowserDataModule::Get().GetSubsystem();

	if (ContentBrowserData == nullptr) {
		return false;
	}

	const FAssetData AssetData(Asset);

	FName VirtualPath;

	ContentBrowserData->Legacy_TryConvertAssetDataToVirtualPaths(AssetData, /* InUseFolderPaths */ false, [&VirtualPath](const FName InPath) {
		VirtualPath = InPath;

		/* The first is the one the browser would navigate to */
		return false;
	});

	if (VirtualPath.IsNone()) {
		return false;
	}

	OutItem = ContentBrowserData->GetItemAtPath(VirtualPath, EContentBrowserItemTypeFilter::IncludeFiles);

	return OutItem.IsValid();
}

inline void BrowseToAsset(UObject* Asset) {
	if (Asset == nullptr) {
		return;
	}

	if (PendingBrowse::Depth > 0) {
		PendingBrowse::Target = Asset;

		return;
	}

	const FString ObjectPath = Asset->GetPathName();

	UE_LOG(LogReflection, Verbose, TEXT("Browsing to \"%s\""), *ObjectPath);

	/* The two ways SyncBrowserToAssets answers a perfectly good request by doing nothing, checked
	 * with the inputs it uses. Neither of them says a word when it drops the sync.
	 *
	 * FindContentBrowserToSync hands back nothing when there is no browser it is allowed to touch,
	 * and SContentBrowser::SyncToLegacy drops any asset whose path the folder permission list
	 * rejects, which a game project is free to configure however it likes. */
	{
		const FContentBrowserModule& BrowserModule = FModuleManager::Get().LoadModuleChecked<FContentBrowserModule>("ContentBrowser");

		if (!BrowserModule.Get().HasPrimaryContentBrowser()) {
			UE_LOG(LogReflection, Warning, TEXT("No primary Content Browser to sync"));
		}

		FAssetToolsModule& AssetToolsModule = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools");

		if (!AssetToolsModule.Get().GetFolderPermissionList()->PassesStartsWithFilter(*ObjectPath)) {
			UE_LOG(LogReflection, Warning, TEXT("Folder permissions hide \"%s\" from the Content Browser"), *ObjectPath);
		}
	}

	const FContentBrowserModule& ContentBrowserModule = FModuleManager::Get().LoadModuleChecked<FContentBrowserModule>("ContentBrowser");

	/* Syncing to the item the browser itself resolved, rather than to an FAssetData it has to go
	 * and resolve again. By this point it is known to be there. */
	if (FContentBrowserItem Item; IsAssetInContentBrowser(Asset, Item)) {
		ContentBrowserModule.Get().SyncBrowserToItems({ Item });

		return;
	}

	/* Browse to newly added Asset in the Content Browser */
	const TArray<FAssetData>& Assets = { Asset };
	ContentBrowserModule.Get().SyncBrowserToAssets(Assets);
}

/* Jumps to Asset once the Content Browser is in a state to be jumped.
 *
 * Two things have to be true, and neither is when the import finishes. The call stack has to be
 * one of ours: the reflect button imports from a timer (FImportJob) so its jump lands cleanly,
 * while everything reached off a menu runs the import inside the click handler, and a sync issued
 * from in there is dropped by the tick still unwinding. And the browser has to have heard of the
 * asset, which happens on a tick of its own, some time after the asset registry was told.
 *
 * So this waits for the second condition on a timer, which satisfies the first on the way. */
inline void BrowseToAssetWhenSafe(UObject* Asset) {
	if (Asset == nullptr) {
		return;
	}

	if (GEditor == nullptr) {
		BrowseToAsset(Asset);

		return;
	}

	UE_LOG(LogReflection, Verbose, TEXT("Queued browse to \"%s\""), *Asset->GetPathName());

	const TWeakObjectPtr<UObject> WeakAsset = Asset;

	PendingBrowse::Deadline = FPlatformTime::Seconds() + PendingBrowse::Timeout;

	GEditor->GetTimerManager()->SetTimer(
		PendingBrowse::Timer,
		FTimerDelegate::CreateLambda([WeakAsset] {
			UObject* Target = WeakAsset.Get();

			/* Long enough for the asset to have been thrown away underneath this */
			if (Target == nullptr) {
				GEditor->GetTimerManager()->ClearTimer(PendingBrowse::Timer);

				return;
			}

			/* Asking again next time rather than spending the sync on a browser that would
			 * discard it */
			if (FContentBrowserItem Item; !IsAssetInContentBrowser(Target, Item)) {
				if (FPlatformTime::Seconds() < PendingBrowse::Deadline) {
					return;
				}

				UE_LOG(LogReflection, Warning, TEXT("Content Browser never saw \"%s\", jumping to it anyway"), *Target->GetPathName());
			}

			GEditor->GetTimerManager()->ClearTimer(PendingBrowse::Timer);

			BrowseToAsset(Target);
		}),
		PendingBrowse::RetryRate,
		true,
		0.0f
	);
}

/* Marks a whole reflect, so the one jump it earns happens when it is over.
 *
 * Declare it before the FBlockingRequestScope of the same operation: the jump then happens once
 * that scope has closed, rather than into a Content Browser sitting behind a progress dialog. */
struct FScopedBrowseToAsset {
	FScopedBrowseToAsset() {
		PendingBrowse::Depth++;
	}

	~FScopedBrowseToAsset() {
		/* Reflects nest, and only the outermost one is finished */
		if (--PendingBrowse::Depth > 0) {
			return;
		}

		UObject* Target = PendingBrowse::Target.Get();
		PendingBrowse::Target.Reset();

		if (Target == nullptr) {
			UE_LOG(LogReflection, Warning, TEXT("Reflect finished with nothing to browse to"));
		}

		BrowseToAssetWhenSafe(Target);
	}

	FScopedBrowseToAsset(const FScopedBrowseToAsset&) = delete;
	FScopedBrowseToAsset& operator=(const FScopedBrowseToAsset&) = delete;
};

/* Get the asset currently selected in the Content Browser. */
template <typename T>
T* GetSelectedAsset(const bool SuppressErrors = false, FString OptionalAssetNameCheck = "") {
	const FContentBrowserModule& ContentBrowserModule = FModuleManager::LoadModuleChecked<FContentBrowserModule>("ContentBrowser");
	TArray<FAssetData> SelectedAssets;
	ContentBrowserModule.Get().GetSelectedAssets(SelectedAssets);

	if (SelectedAssets.Num() == 0) {
		if (SuppressErrors == true) {
			return nullptr;
		}
		
		GLog->Log("Reflection: [GetSelectedAsset] None selected, returning nullptr.");

		const FText DialogText = FText::Format(
			FText::FromString(TEXT("Reflecting an asset of type '{0}' requires a base asset selected to modify. Select one in your content browser.")),
			FText::FromString(T::StaticClass()->GetName())
		);

		FMessageDialog::Open(EAppMsgType::Ok, DialogText);
		
		return nullptr;
	}

	UObject* SelectedAsset = SelectedAssets[0].GetAsset();
	T* CastedAsset = Cast<T>(SelectedAsset);

	if (!CastedAsset) {
		if (SuppressErrors == true) {
			return nullptr;
		}
		
		GLog->Log("Reflection: [GetSelectedAsset] Selected asset is not of the required class, returning nullptr.");

		const FText DialogText = FText::Format(
			FText::FromString(TEXT("The selected asset is not of type '{0}'. Please select a valid asset.")),
			FText::FromString(T::StaticClass()->GetName())
		);

		FMessageDialog::Open(EAppMsgType::Ok, DialogText);
		
		return nullptr;
	}

	if (CastedAsset && OptionalAssetNameCheck != "" && !CastedAsset->GetName().Equals(OptionalAssetNameCheck)) {
		CastedAsset = nullptr;
	}

	return CastedAsset;
}

/* Package path of the folder selected in the Content Browser, empty when nothing is selected.
 * Ex: "/Game/Characters/Items" */
inline FString GetSelectedContentBrowserFolder() {
	const FContentBrowserModule& ContentBrowserModule = FModuleManager::LoadModuleChecked<FContentBrowserModule>("ContentBrowser");

	TArray<FString> SelectedFolders;
	ContentBrowserModule.Get().GetSelectedPathViewFolders(SelectedFolders);

	if (SelectedFolders.Num() == 0) {
		return FString();
	}

#if ENGINE_UE5
	/* Convert virtual paths to internal package paths */
	const UContentBrowserDataSubsystem* ContentBrowserData = GEditor->GetEditorSubsystem<UContentBrowserDataSubsystem>();

	if (!ContentBrowserData) {
		return FString();
	}

	const TArray<FString> InternalPaths = ContentBrowserData->TryConvertVirtualPathsToInternal(SelectedFolders);

	return InternalPaths.Num() > 0 ? InternalPaths[0] : FString();
#else
	return SelectedFolders[0];
#endif
}

/* Gets all assets in selected folder */
inline TArray<FAssetData> GetAssetsInSelectedFolder() {
	TArray<FAssetData> AssetDataList;

	const FString CurrentFolder = GetSelectedContentBrowserFolder();

	if (CurrentFolder.IsEmpty()) {
		UE_LOG(LogReflection, Warning, TEXT("No folder selected in the Content Browser."));
		return AssetDataList;
	}

	/* Check if the folder is the root folder, and show a prompt if */
	if (CurrentFolder == "/All/Game") {
		bool Continue = false;
		
		SpawnYesNoPrompt(
			TEXT("Large Operation"),
			TEXT("This will stall the editor for a long time. Continue anyway?"),
			[&](const bool Confirmed) {
				Continue = Confirmed;
			}
		);

		if (!Continue) {
			UE_LOG(LogReflection, Warning, TEXT("Action cancelled by user."));
			return AssetDataList;
		}
	}

	/* Get the Asset Registry Module */
	const FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
	AssetRegistryModule.Get().SearchAllAssets(true);

	/* Get all assets in the folder and its subfolders */
	AssetRegistryModule.Get().GetAssetsByPath(FName(*CurrentFolder), AssetDataList, true);

	return AssetDataList;
}