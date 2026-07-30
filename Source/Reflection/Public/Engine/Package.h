/* Copyright Reflection Contributors 2024-2026 */

#pragma once

#include "AssetRegistry/AssetRegistryModule.h"
#include "VectorField/VectorFieldStatic.h"

#include "Utilities/ContentBrowser.h"
#include "Utilities/Dialog.h"

#if ENGINE_UE5
#include "UObject/SavePackage.h"
#endif

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
