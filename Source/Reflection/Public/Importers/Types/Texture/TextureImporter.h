/* Copyright Reflection Contributors 2024-2026 */

#pragma once

#include "Importers/Constructor/Importer.h"

/* Brings a texture over from Cloud.
 *
 * The export and the pixels come from the same endpoint: the export as json, the pixels as
 * whatever encoding the class needs, picked by FTextureTypes. FTextureCreator turns the pair
 * into an asset, this just does the fetching and the surrounding packaging. */
struct REFLECTION_API FTextureImport {
	/* Fetches an export and its pixels from Cloud, then builds the asset */
	static bool FromCloud(const FString& Path, const FString& FetchPath, UTexture*& OutTexture);

	/* Builds the asset from an export and a payload already in hand */
	static bool FromExport(const TSharedPtr<FJsonObject>& Export, const FString& Path, const FString& Type, TArray<uint8> Data, UTexture*& OutTexture);
};

/* Basic template importer using Asset Class. */
template <typename AssetType>
class ITextureImporter : public IImporter {
public:
	virtual bool Import() override;
};
