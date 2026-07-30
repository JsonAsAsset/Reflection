/* Copyright Reflection Contributors 2024-2026 */

#include "Importers/Constructor/TypesHelper.h"

#include "Importers/Constructor/Types.h"
#include "Importers/Constructor/Registry/RegistrationInfo.h"
#include "Engine/Compatibility.h"

bool CanImport(const FString& Type, const bool IsCloud, const UClass* Class) {
	if (IsCloud) {
		if (!ImportTypes::Cloud::Allowed(Type)) {
			return false;
		}

		if (Type == "SoundWave") {
			return true;
		}
	}
    
	if (FindFactoryForAssetType(Type)) {
		return true;
	}
    
	for (const TPair<FString, TArray<FString>>& Pair : ImportTypes::Templated) {
		if (Pair.Value.Contains(Type)) {
			return true;
		}
	}

	if (!Class) {
		Class = FindClassByType(Type);
	}

	if (Class == nullptr) {
		return false;
	}

	if (ImportTypes::Cloud::Extra.Contains(Type)) {
		return true;
	}

	if (!ImportTypes::Allowed(Type)) return false;

	/* Cooked blueprints of any family; the blueprint importer resolves which kind through reflection */
	if (IsGeneratedClassType(Type)) {
		return true;
	}

	if (Class->IsChildOf(UDataAsset::StaticClass())) {
		return true;
	}
	
	if (Class->IsChildOf(UTexture::StaticClass())) {
		return false;
	}

	/* Lots of issues happened */
#if 0
	/* If the Class has an asset type action, it's importable */
	const FAssetToolsModule& AssetToolsModule = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools");
	const IAssetTools& AssetTools = AssetToolsModule.Get();
	
	static TArray<TWeakPtr<IAssetTypeActions>> OutAssetTypeActionsList = {};
	AssetTools.GetAssetTypeActionsList(OutAssetTypeActionsList);

	for (TWeakPtr AssetTypeAction : OutAssetTypeActionsList) {
		if (const TSharedPtr<IAssetTypeActions> Action = AssetTypeAction.Pin()) {
			const UClass* SupportedClass = Action->GetSupportedClass();

			if (SupportedClass == Class) return true;
		}
	}
#endif

	return false;
}
