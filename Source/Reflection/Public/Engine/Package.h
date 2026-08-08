/* Copyright Reflection Contributors 2024-2026 */

#pragma once

#include "Engine/Compatibility.h"

/* AssetRegistryModule.h only moved under an AssetRegistry/ folder later on */
#if UE4_25_BELOW
#include "AssetRegistryModule.h"
#else
#include "AssetRegistry/AssetRegistryModule.h"
#endif
#include "VectorField/VectorFieldStatic.h"
#include "UObject/ObjectRedirector.h"

#include "Utilities/ContentBrowser.h"
#include "Utilities/Dialog.h"

#if ENGINE_UE5
#include "UObject/SavePackage.h"
#endif

/* Follows an object redirector to whatever it points at.*/
inline UObject* ResolveRedirector(UObject* Object) {
	/* Redirectors chain. The cap only exists so a self referential one cannot spin forever. */
	for (int32 Depth = 0; Depth < 16; ++Depth) {
		const UObjectRedirector* Redirector = Cast<UObjectRedirector>(Object);

		if (Redirector == nullptr) {
			return Object;
		}

		Object = Redirector->DestinationObject;
	}

	return nullptr;
}

/* Loads an asset by package path, following any redirector left behind by a rename. Every load
 * the importer does by path goes through here, so none of them can hand back a redirector.
 *
 * Null is an answer rather than a failure here: this is also how the importer asks whether an
 * asset exists yet, and most of the time it does not. Asking the loader that question costs a
 * failed package load and two warnings apiece, so the cheap answers are given first. */
template <typename T>
T* LoadObjectByPath(const FString& Path) {
	/* Already in memory, including a package this session created and never saved */
	if (UObject* Found = FindObject<UObject>(nullptr, *Path)) {
		return Cast<T>(ResolveRedirector(Found));
	}

	const FString PackageName = FPackageName::ObjectPathToPackageName(Path);

	/* DoesPackageExist asserts rather than answering when it dislikes what it is handed: a name it
	 * cannot convert trips an ensure, and one that converts but belongs to no mounted content root
	 * reaches a Fatal log through LongPackageNameToFilename. Paths get here from Cloud responses
	 * and from json exports, so what arrives is not this plugin's to trust. Resolving the filename
	 * first is the same question asked in a form that returns false instead. */
	if (FString Unused; !FPackageName::TryConvertLongPackageNameToFilename(PackageName, Unused)) {
		return nullptr;
	}

	/* Not in memory and no file to read it out of, so there is nothing to load and no reason to
	 * have the loader say so twice */
	if (!FPackageName::DoesPackageExist(PackageName)) {
		return nullptr;
	}

	return Cast<T>(ResolveRedirector(StaticLoadObject(T::StaticClass(), nullptr, *Path, nullptr, LOAD_NoWarn | LOAD_Quiet)));
}

inline void SavePackage(UPackage* Package) {
	const FString PackageName = Package->GetName();
	const FString PackageFileName = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());

#if ENGINE_UE5
	FSavePackageArgs SaveArgs; {
		SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
		SaveArgs.Error = GError;
		SaveArgs.SaveFlags = SAVE_NoError;
	}

	UPackage::SavePackage(Package, nullptr, *PackageFileName, SaveArgs);
#else
	UPackage::SavePackage(Package, nullptr, RF_Standalone, *PackageFileName);
#endif
}

inline bool HandleAssetCreation(UObject* Asset, UPackage* Package) {
	{
		/* User Failsafe.... */
		const UPackage* AssetOutermostPackage = Asset->GetOutermost();
		const FString PackageName = AssetOutermostPackage->GetName();

		const FString Path = FPackageName::GetLongPackagePath(PackageName);
		if (!Path.StartsWith(TEXT("/")) || Path.Len() < 2) {
			SpawnPrompt("Failsafe", "Here's some reasons why:\n\n- You didn't export it from FModel\n- Reflected it from a random path, not in Exports/.../\n\nPlease reflect it again next time at the proper location.");

			return false;
		}
	}

	FAssetRegistryModule::AssetCreated(Asset);
	if (!Asset->MarkPackageDirty()) return false;

	Package->SetDirtyFlag(true);

	if (UVectorFieldStatic* VectorFieldStatic = Cast<UVectorFieldStatic>(Asset)) {
		VectorFieldStatic->InitResource();
	}

	Asset->PostEditChange();
	Asset->AddToRoot();

	Package->FullyLoad();

	BrowseToAsset(Asset);

	if (UVectorFieldStatic* VectorFieldStatic = Cast<UVectorFieldStatic>(Asset)) {
		VectorFieldStatic->Resource = nullptr;
	}

	Asset->PostLoad();

	return true;
}

inline FString GetAssetPath(const UObject* Object) {
	if (!Object) {
		return FString();
	}

	if (const UPackage* Package = Object->GetOutermost()) {
		return Package->GetName();
	}

	return FString();
}

inline void MoveToTransientPackageAndRename(UObject* Object) {
	if (Object) {
		Object->Rename(nullptr, GetTransientPackage(), REN_DontCreateRedirectors);
		Object->SetFlags(RF_Transient);
	}
}

inline void MoveToTransientPackagesAndRename(TArray<UObject*> Objects) {
	for (UObject* Object : Objects) {
		MoveToTransientPackageAndRename(Object);
	}
}
