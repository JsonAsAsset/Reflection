/* Copyright Reflection Contributors 2024-2026 */

#include "Importers/Types/Texture/TextureImporter.h"

#include "Importers/Types/Texture/TextureCreator.h"
#include "Importers/Types/Texture/TextureTypes.h"
#include "Importers/Constructor/Asset.h"

#include "Engine/TextureLightProfile.h"
#include "Engine/EngineUtilities.h"
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
	TObjectPtr<AssetType> Texture;
	DownloadWrapper<AssetType>(Texture, GetAssetType(), GetAssetName(), GetPackage()->GetPathName());

	return true;
}

/*
 * The pixels come from the export endpoint too, with the content type of the request picking the
 * encoding: an image the texture factory can take, or the raw bytes of the first mip.
 */
static bool DownloadPixels(const FString& FetchPath, const FString& Type, const TSharedPtr<FJsonObject>& Export, TArray<uint8>& OutData) {
	const bool UseRawMipData = FTextureTypes::RequiresRawMipData(Type, FTextureTypes::IsVectorDisplacementMap(Export));

	const FReflectionHttpRequest HttpRequest = FHttpModule::Get().CreateRequest();

	HttpRequest->SetURL(Cloud::URL + Cloud::ExportURL + "?path=" + FetchPath);
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

	const TSharedPtr<FJsonObject> Export = Exports[0]->AsObject();
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

	FRRedirects::Redirect(PackagePath);

	/* Missing Plugin: Create it */
	if (!PackagePath.StartsWith("/Game/") && !PackagePath.StartsWith("/Engine/")) {
		FString PluginName;
		PackagePath.Split("/", nullptr, &PluginName, ESearchCase::IgnoreCase, ESearchDir::FromStart);
		PluginName.Split("/", &PluginName, nullptr, ESearchCase::IgnoreCase, ESearchDir::FromStart);

		if (GetPlugin(PluginName) == nullptr) {
			CreatePlugin(PluginName);
		}
	}

	UPackage* Package = FAssetUtilities::CreateAssetPackage(*PackagePath);
	Package->FullyLoad();

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
