/* Copyright Reflection Contributors 2024-2026 */

#pragma once

#include "Importers/Constructor/Types.h"
#include "Engine/EngineUtilities.h"
#include "Settings/ReflectionSettings.h"

class IImporter;

/* The registry keys on TArray<FString>, which TMap cannot hash on its own */
FORCEINLINE uint32 GetTypeHash(const TArray<FString>& Array) {
	uint32 Hash = 0;

	for (const FString& Str : Array) {
		Hash = HashCombine(Hash, GetTypeHash(Str));
	}

	return Hash;
}

/* Easy way to find importers ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ */
using FImporterFactoryDelegate = TFunction<IImporter*()>;

/* Registration info for an importer */
struct FImporterRegistrationInfo {
	FString Category;
	FImporterFactoryDelegate Factory;

	FImporterRegistrationInfo(const FString& InCategory, const FImporterFactoryDelegate& InFactory)
		: Category(InCategory)
		, Factory(InFactory)
	{
	}

	FImporterRegistrationInfo() = default;
};

inline TMap<TArray<FString>, FImporterRegistrationInfo>& GetFactoryRegistry() {
	static TMap<TArray<FString>, FImporterRegistrationInfo> Registry;
        
	return Registry;
}

/* Importers that take a type off whatever else registered it. Empty unless something in this build
 * put itself there. */
inline TMap<TArray<FString>, FImporterRegistrationInfo>& GetFactoryOverrides() {
	static TMap<TArray<FString>, FImporterRegistrationInfo> Overrides;

	return Overrides;
}

inline FImporterFactoryDelegate* FindFactoryForAssetType(const FString& AssetType) {
	const UReflectionSettings* Settings = GetSettings();

	for (auto& Pair : GetFactoryOverrides()) {
		if (Pair.Key.Contains(AssetType)) {
			return &Pair.Value.Factory;
		}
	}

	for (auto& Pair : GetFactoryRegistry()) {
		if (!Settings->EnableExperiments) {
			if (ImportTypes::Experimental.Contains(AssetType)) return nullptr;
		}
            
		if (Pair.Key.Contains(AssetType)) {
			return &Pair.Value.Factory;
		}
	}
        
	return nullptr;
}

template <typename T>
IImporter* CreateImporter() {
	return new T();
}