/* Copyright Reflection Contributors 2024-2026 */

#include "Importers/Types/Texture/TextureImporter.h"

#include "Importers/Types/Texture/TextureCreator.h"
#include "Importers/Types/Texture/TextureTypes.h"
#include "Importers/Constructor/Asset.h"

#include "Engine/TextureLightProfile.h"
#include "Engine/EngineUtilities.h"
#include "Engine/Log.h"
#include "HttpModule.h"
#include "Interfaces/IHttpResponse.h"
#include "Modules/Cloud/Cloud.h"
#include "Modules/Cloud/Remote.h"
#include "Settings/ReflectionSettings.h"
#include "Utilities/ContentBrowser.h"

/* AssetRegistryModule.h only moved under an AssetRegistry/ folder later on */
#if UE4_25_BELOW
#include "AssetRegistryModule.h"
#else
#include "AssetRegistry/AssetRegistryModule.h"
#endif

/* Explicit instantiation of ITextureImporter for UObject */
template class ITextureImporter<UTexture>;
template class ITextureImporter<UTextureLightProfile>;

template <typename AssetType>
bool ITextureImporter<AssetType>::Import() {
	TObjectPtr<AssetType> Texture = nullptr;
	DownloadWrapper<AssetType>(Texture, GetAssetType(), GetAssetName(), GetPackage()->GetPathName());

	return true;
}

/* The pixels come from the export endpoint too, with the content type of the request picking the
 * encoding: an image the texture factory can take, or the raw bytes of the first mip. */
static bool DownloadPixels(const FString& FetchPath, const FString& Type, const TSharedPtr<FJsonObject>& Export, TArray<uint8>& OutData) {
	const bool UseRawMipData = FTextureTypes::RequiresRawMipData(Type, FTextureTypes::IsVectorDisplacementMap(Export));

	const FReflectionHttpRequest HttpRequest = FHttpModule::Get().CreateRequest();

	/* Which export of the package is wanted, said outright.
	 *
	 * The Cloud reads a path as far as the first dot and no further, so a name written on the end of
	 * one is not a name it ever sees: it answers with whatever the package leads with. That is the
	 * texture itself for almost every package, and for an HLOD proxy it is a body setup, which comes
	 * back as properties rather than pixels. */
	FString Named;

	Export->TryGetStringField(TEXT("Name"), Named);

	HttpRequest->SetURL(Cloud::URL + Cloud::ExportURL + "?path=" + FetchPath
		+ (Named.IsEmpty() ? TEXT("") : TEXT("&export_name=") + Named));
	HttpRequest->SetHeader("content-type", UseRawMipData ? "application/octet-stream" : "image/png");
	HttpRequest->SetVerb(TEXT("GET"));

	const FReflectionHttpResponse HttpResponse = FRemoteUtilities::ExecuteRequestBlocking(HttpRequest);

	if (!HttpResponse.IsValid() || HttpResponse->GetResponseCode() != 200) {
		return false;
	}

	/* Cloud answers with json when it couldn't decode the texture, never with pixels */
	if (HttpResponse->GetContentType().StartsWith("application/json")) {
		return false;
	}

	OutData = HttpResponse->GetContent();

	return OutData.Num() > 0;
}

bool FTextureImport::FromCloud(const FString& Path, const FString& FetchPath, UTexture*& OutTexture) {
	if (Path.IsEmpty()) {
		return false;
	}

	const TSharedPtr<FJsonObject> Response = Cloud::Export::GetRawBlocking(FetchPath);
	if (Response == nullptr) {
		return false;
	}

	const TArray<TSharedPtr<FJsonValue>> Exports = Response->GetArrayField(TEXT("exports"));
	if (Exports.Num() == 0) {
		return false;
	}

	/* Which of the package's exports is the one wanted.
	 *
	 * A package holding a single asset has the one export and it is the first. A package holding
	 * several does not: an HLOD proxy keeps its mesh, the material that draws it and the texture
	 * that material samples all in the one file, and taking the first hands back whatever happened
	 * to be written first, which there is a body setup.
	 *
	 * The path says which. A reference names it by the index on the end, and a path typed in names
	 * it outright, so both spellings are tried and the search falls back on the first export that
	 * is a texture at all, since a texture is what was asked for. */
	int32 Wanted = INDEX_NONE;

	if (FString Leaf; Path.Split(TEXT("."), nullptr, &Leaf, ESearchCase::CaseSensitive, ESearchDir::FromEnd)) {
		if (Leaf.IsNumeric()) {
			if (const int32 At = FCString::Atoi(*Leaf); Exports.IsValidIndex(At)) {
				Wanted = At;
			}
		} else {
			for (int32 At = 0; At < Exports.Num() && Wanted == INDEX_NONE; ++At) {
				const TSharedPtr<FJsonObject> One = Exports[At].IsValid() ? Exports[At]->AsObject() : nullptr;

				FString Named;

				if (One.IsValid() && One->TryGetStringField(TEXT("Name"), Named) && Named == Leaf) {
					Wanted = At;
				}
			}
		}
	}

	for (int32 At = 0; At < Exports.Num() && Wanted == INDEX_NONE; ++At) {
		const TSharedPtr<FJsonObject> One = Exports[At].IsValid() ? Exports[At]->AsObject() : nullptr;

		FString Named;

		if (One.IsValid() && One->TryGetStringField(TEXT("Type"), Named) && FTextureTypes::IsSupported(Named)) {
			Wanted = At;
		}
	}

	const TSharedPtr<FJsonObject> Export = Exports[Wanted == INDEX_NONE ? 0 : Wanted]->AsObject();
	const FString Type = Export->GetStringField(TEXT("Type"));

	TArray<uint8> Data;

	if (FTextureTypes::HasPixelPayload(Type) && !DownloadPixels(FetchPath, Type, Export, Data)) {
		return false;
	}

	return FromExport(Export, Path, Type, Data, OutTexture);
}

bool FTextureImport::FromExport(const TSharedPtr<FJsonObject>& Export, const FString& Path, const FString& Type, TArray<uint8> Data, UTexture*& OutTexture) {
	const UReflectionSettings* Settings = GetSettings();

	FString PackagePath;
	FString AssetName; {
		Path.Split(".", &PackagePath, &AssetName);
	}

	/* A reference into a package holding several assets says which one it wants by index, and an
	 * index is no name to give a texture. The export carries the name it was cooked under. */
	if (FString Named; AssetName.IsNumeric() && Export->TryGetStringField(TEXT("Name"), Named)) {
		AssetName = Named;
	}

	/* And it lands beside whatever else that package held, unless they are being split up */
	const FString PackageName = Settings->AssetSettings.SeparatePackagedAssets
		? AssetName
		: FPaths::GetBaseFilename(PackagePath);

	/* Where an asset lands is one decision, made in one place: the redirects, the plugin whatever
	 * root it names, and the paths that arrive relative to the export directory */
	FString FailureReason;
	UPackage* Package = FAssetUtilities::CreateAssetPackage(PackageName, PackagePath, FailureReason);

	if (Package == nullptr) {
		UE_LOG(LogReflection, Error, TEXT("No package for \"%s\": %s"), *Path, *FailureReason);

		return false;
	}

	const bool UseRawMipData = FTextureTypes::RequiresRawMipData(Type, FTextureTypes::IsVectorDisplacementMap(Export));

	FTextureCreator TextureCreator = FTextureCreator(AssetName, Path, Package, UseRawMipData);

	UTexture* Texture = nullptr;
	if (!TextureCreator.Create(Type, Export, Data, Texture) || Texture == nullptr) {
		return false;
	}

	FAssetRegistryModule::AssetCreated(Texture);
	if (!Texture->MarkPackageDirty()) {
		return false;
	}

	Package->SetDirtyFlag(true);
	Texture->PostEditChange();
	Texture->AddToRoot();
	Package->FullyLoad();

	/* Textures don't go through HandleAssetCreation, so the jump to the asset has to happen here */
	BrowseToAsset(Texture);

	if (Settings->AssetSettings.SaveAssets) {
		SavePackage(Package);
	}

	OutTexture = Texture;

	return true;
}
