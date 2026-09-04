/* Copyright Reflection Contributors 2024-2026 */

#pragma once


#include "Engine/Compatibility.h"
#include "Framework/Application/SlateApplication.h"
#include "Engine/FontFace.h"

#include "Engine/StaticMesh.h"

/* Static meshes only build asynchronously from UE5 on */
#if ENGINE_UE5
#include "StaticMeshCompiler.h"

/* Skeletal meshes build asynchronously the same way static ones do, from 5.0 on */
#if ENGINE_UE5
#include "SkinnedAssetCompiler.h"
#include "Engine/SkeletalMesh.h"
#endif
#endif

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
T* LoadObjectByPath(const FString& InPath) {
	/* Named, since a path with nothing after the package names the package.
	 *
	 * An asset's path and the asset's own path are two different strings: the package is
	 * /Game/Some/Thing and the asset in it is /Game/Some/Thing.Thing. Handed the first, the loader
	 * resolves no further than the package, so what comes back is a UPackage where it is in memory
	 * and nothing at all where it is not, and neither is ever the asset. Callers with the asset's
	 * name already say it; the ones reading a reference out of an export have only the package. */
	FString Path = InPath;

	if (!Path.Contains(TEXT("."))) {
		FString Named = Path;

		if (Named.Split(TEXT("/"), nullptr, &Named, ESearchCase::CaseSensitive, ESearchDir::FromEnd) && !Named.IsEmpty()) {
			Path += TEXT(".") + Named;
		}
	}

	/* Already in memory, including a package this session created and never saved */
	if (UObject* Found = FindObject<UObject>(nullptr, *Path)) {
		return Cast<T>(ResolveRedirector(Found));
	}

	const FString PackageName = FPackageName::ObjectPathToPackageName(Path);

	/* DoesPackageExist asserts rather than answering when it dislikes what it is handed, in two
	 * separate ways: a name holding a character a package name cannot hold trips its ensure, and
	 * one that reads as a package but belongs to no mounted content root reaches a Fatal log
	 * through LongPackageNameToFilename.
	 *
	 * IsValidLongPackageName is the same pair of questions asked in a form that answers false. It
	 * covers the characters, '.' and ':' and '\' among them, which is what a subobject path still
	 * carries when it gets this far, and it covers the roots. Read only roots are left out on
	 * purpose: /Config is a valid root by that list and has no content path to resolve to, which
	 * is the Fatal.
	 *
	 * Paths arrive here off Cloud responses and out of json exports, so none of this is under the
	 * plugin's control and none of it is worth trusting. */
	if (!FPackageName::IsValidLongPackageName(PackageName, /*bIncludeReadOnlyRoots=*/false)) {
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

/* bBrowse says whether to jump the content browser to what was made.
 *
 * Only ever true of the asset somebody asked for. An import reaches through whatever the asset
 * references and builds those too, and jumping to each of them walks the asset registry over every
 * object in memory while the ones further up this same stack are still half built. */
inline bool HandleAssetCreation(UObject* Asset, UPackage* Package, const bool bBrowse = true) {
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

	/* A font face tells Slate to drop the font it had cached, and asking for Slate where it was
	 * never started is an assert rather than an answer. A commandlet has none, and nothing has the
	 * old font cached there either. */
	if (!Asset->IsA<UFontFace>() || FSlateApplication::IsInitialized()) {
		Asset->PostEditChange();
	}

	Asset->AddToRoot();

	Package->FullyLoad();

	if (bBrowse) BrowseToAsset(Asset);

	if (UVectorFieldStatic* VectorFieldStatic = Cast<UVectorFieldStatic>(Asset)) {
		VectorFieldStatic->Resource = nullptr;
	}

	/* PostEditChange above already built the static mesh, resources and all. PostLoad builds it a
	 * second time and frees the render data the first build handed to the render thread. */
	if (UStaticMesh* StaticMesh = Cast<UStaticMesh>(Asset)) {
#if ENGINE_UE5
		if (StaticMesh->IsCompiling()) {
			FStaticMeshCompilingManager::Get().FinishCompilation({ StaticMesh });
		}
#endif

		return true;
	}

	/* Same story for a skeletal mesh, and worse: its PostLoad starts a second async build while
	 * the first is still running, which trips the build task's own idle check */
#if ENGINE_UE5
	if (USkeletalMesh* SkeletalMesh = Cast<USkeletalMesh>(Asset)) {
		if (SkeletalMesh->IsCompiling()) {
			FSkinnedAssetCompilingManager::Get().FinishCompilation({ SkeletalMesh });
		}

		return true;
	}
#endif

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
