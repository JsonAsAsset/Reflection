/* Copyright Reflection Contributors 2024-2026 */

#pragma once

#include "Importers/Constructor/ImportIssues.h"

#include "IContentBrowserSingleton.h"
#include "ContentBrowserModule.h"
#include "Utilities/Dialog.h"
#include "Engine/Compatibility.h"

/* AssetRegistryModule.h only moved under an AssetRegistry/ folder later on */
#if UE4_25_BELOW
#include "AssetRegistryModule.h"
#else
#include "AssetRegistry/AssetRegistryModule.h"
#endif
#include "Engine/Log.h"
#include "Framework/Docking/TabManager.h"

/*
 * Whether the editor has a Content Browser open to jump to.
 *
 * Asked only on the versions that need it. When SyncBrowserToAssets finds no browser it summons
 * one, and up to 4.25 that lands in FTabManager::InvokeTab, which uses whatever InvokeTab_Internal
 * returns without checking it first: 4.23 dereferences null outright and 4.25 trips a check. 4.26
 * split the null-safe TryInvokeTab out and routed InvokeTab through it. Nothing on this side can
 * reach the engine's summon path, so the jump is given up instead of taking the editor with it.
 *
 * FContentBrowserSingleton registers its browsers as the nomad tabs ContentBrowserTab1 through
 * MAX_CONTENT_BROWSERS, which is 4 on every version this branch covers.
 */
#if UE4_25_BELOW
inline bool HasLiveContentBrowser() {
	for (int32 BrowserIndex = 1; BrowserIndex <= 4; ++BrowserIndex) {
		const FTabId TabId = FTabId(*FString::Printf(TEXT("ContentBrowserTab%d"), BrowserIndex));

		if (FGlobalTabmanager::Get()->FindExistingLiveTab(TabId).IsValid()) {
			return true;
		}
	}

	return false;
}
#endif

inline void BrowseToAsset(UObject* Asset) {
	if (Asset == nullptr) {
		return;
	}

#if UE4_25_BELOW
	if (!HasLiveContentBrowser()) {
		UE_LOG(LogReflection, Verbose, TEXT("No Content Browser open, skipping the jump to \"%s\""), *Asset->GetName());

		return;
	}
#endif

	/* Browse to newly added Asset in the Content Browser.
	 *
	 * The folder it sits in was registered back when its package was made (CreateAssetPackage), so
	 * by now the browser has had the length of the import to build it and there is something here
	 * to navigate to. */
	const TArray<FAssetData>& Assets = { Asset };
	const FContentBrowserModule& ContentBrowserModule = FModuleManager::Get().LoadModuleChecked<FContentBrowserModule>("ContentBrowser");

	/* Locked browsers included, which is what the engine's own import does at the end of
	 * UAssetToolsImpl::ImportAssetsInternal. Left out, a locked Content Browser means
	 * FindContentBrowserToSync hands back nothing and the jump is dropped without a word. */
	ContentBrowserModule.Get().SyncBrowserToAssets(Assets, /*bAllowLockedBrowsers=*/true);
}

/* Get the asset currently selected in the Content Browser. */
template <typename T>
T* GetSelectedAsset(const bool SuppressErrors = false, FString OptionalAssetNameCheck = "") {
	const FContentBrowserModule& ContentBrowserModule = FModuleManager::LoadModuleChecked<FContentBrowserModule>("ContentBrowser");
	TArray<FAssetData> SelectedAssets;
	ContentBrowserModule.Get().GetSelectedAssets(SelectedAssets);

	if (SelectedAssets.Num() == 0) {
		if (SuppressErrors == false) {
			FImportIssues::Report(
				EImportIssue::Setting,
				TEXT("Nothing selected in the Content Browser"),
				FString::Printf(TEXT("This reflects onto a %s that is already in the project, so one has to be selected."), *T::StaticClass()->GetName())
			);
		}

		return nullptr;
	}

	UObject* SelectedAsset = SelectedAssets[0].GetAsset();
	T* CastedAsset = Cast<T>(SelectedAsset);

	if (!CastedAsset) {
		if (SuppressErrors == false) {
			FImportIssues::Report(
				EImportIssue::Setting,
				TEXT("The selected asset is the wrong type"),
				FString::Printf(TEXT("\"%s\" is a %s, and this reflects onto a %s."), *SelectedAsset->GetName(), *SelectedAsset->GetClass()->GetName(), *T::StaticClass()->GetName())
			);
		}

		return nullptr;
	}

	/* Reflecting onto whatever happens to be selected is how the wrong asset gets overwritten */
	if (OptionalAssetNameCheck != "" && !CastedAsset->GetName().Equals(OptionalAssetNameCheck)) {
		if (SuppressErrors == false) {
			FImportIssues::Report(
				EImportIssue::Setting,
				TEXT("The selected asset is not the one being reflected"),
				FString::Printf(TEXT("\"%s\" is selected, and this reflects onto \"%s\"."), *CastedAsset->GetName(), *OptionalAssetNameCheck)
			);
		}

		return nullptr;
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