/* Copyright Reflection Contributors 2024-2026 */

#include "Importers/Constructor/Asset.h"

#include "Importers/Constructor/Importer.h"

#include "Importers/Types/Texture/TextureCreator.h"

#include "Curves/CurveLinearColor.h"
#include "Sound/SoundNode.h"
#include "Engine/SubsurfaceProfile.h"
#include "Materials/MaterialParameterCollection.h"
#include "Settings/ReflectionSettings.h"
#include "Dom/JsonObject.h"

#include "HttpModule.h"

/* AssetRegistryModule.h only moved under an AssetRegistry/ folder later on */
#if UE4_22_BELOW
#include "AssetRegistryModule.h"
#else
#include "AssetRegistry/AssetRegistryModule.h"
#endif
#include "Engine/FontFace.h"
#include "Importers/Constructor/ImportReader.h"
#include "Importers/Constructor/Graph/SoundGraph.h"
#include "Interfaces/IHttpResponse.h"
#include "Modules/Cloud/Cloud.h"
#include "Settings/Runtime.h"
#include "Modules/Cloud/Remote.h"

/* CreateAssetPackage Implementations ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ */
UPackage* FAssetUtilities::CreateAssetPackage(const FString& Path) {
	UPackage* Package = CreatePackage(
		/* 4.25, 4.26.0 and below need an Outer */
#if UE4_25_BELOW || (UE4_26_0)
		nullptr, 
#endif
		*Path);
	Package->FullyLoad();

	return Package;
}

UPackage* FAssetUtilities::CreateAssetPackage(const FString& Name, const FString& OutputPath, FString& FailureReason) {
	const UReflectionSettings* Settings = GetSettings();
	
	FString ModifiablePath = OutputPath;
	
	/* References Automatically Formatted */
	if (!ModifiablePath.StartsWith("/Game/") && !ModifiablePath.StartsWith("/Plugins/") && ModifiablePath.Contains("/Content/")) {
		if (!Settings->AssetSettings.ProjectName.IsEmpty()) {
			ModifiablePath = ModifiablePath.Replace(*(Settings->AssetSettings.ProjectName + "/Content"), TEXT("/Game"));
			ModifiablePath.Split(*(GReflectionRuntime.ExportDirectory.Path + "/"), nullptr, &ModifiablePath, ESearchCase::IgnoreCase, ESearchDir::FromStart);
			ModifiablePath.Split("/", &ModifiablePath, nullptr, ESearchCase::IgnoreCase, ESearchDir::FromEnd);
			ModifiablePath += "/";
		}

		if (!ModifiablePath.StartsWith("/Game/") && !ModifiablePath.StartsWith("/Plugins/") && ModifiablePath.Contains("/Content/")) {
			ModifiablePath.Split(*(GReflectionRuntime.ExportDirectory.Path + "/"), nullptr, &ModifiablePath, ESearchCase::IgnoreCase, ESearchDir::FromStart);
			ModifiablePath.Split("/", nullptr, &ModifiablePath, ESearchCase::IgnoreCase, ESearchDir::FromStart);
			ModifiablePath.Split("/", &ModifiablePath, nullptr, ESearchCase::IgnoreCase, ESearchDir::FromEnd);
			/* Ex: RestPath: Plugins/Folder/BaseTextures */
			/* Ex: RestPath: Content/SecondaryFolder */
			const bool IsPlugin = ModifiablePath.StartsWith("Plugins");

			/* Plugins/Folder/BaseTextures -> Folder/BaseTextures */
			if (IsPlugin) {
				FString PluginName = ModifiablePath;
				FString RemainingPath;
				/* PluginName = TestName */
				/* RemainingPath = SetupAssets/Materials */
				ModifiablePath.Split("/Content/", &PluginName, &RemainingPath, ESearchCase::IgnoreCase, ESearchDir::FromStart);
				PluginName.Split("/", nullptr, &PluginName, ESearchCase::IgnoreCase, ESearchDir::FromEnd);

				/* /PluginName/Materials */
				ModifiablePath = PluginName + "/" + RemainingPath;
			}
			/* Content/SecondaryFolder -> Game/SecondaryFolder */
			else {
				ModifiablePath = ModifiablePath.Replace(TEXT("Content"), TEXT("Game"));
			}

			ModifiablePath = "/" + ModifiablePath + "/";

			FRRedirects::Redirect(ModifiablePath);

			/* Check if plugin exists */
			if (IsPlugin && !ModifiablePath.StartsWith("/Game/")) {
				FString PluginName;
				ModifiablePath.Split("/", nullptr, &PluginName, ESearchCase::IgnoreCase, ESearchDir::FromStart);
				PluginName.Split("/", &PluginName, nullptr, ESearchCase::IgnoreCase, ESearchDir::FromStart);

				if (GetPlugin(PluginName) == nullptr) {
					CreatePlugin(PluginName);
				}
			}
		}
		else {
			FRRedirects::Redirect(ModifiablePath);

			if (!ModifiablePath.StartsWith("/Game/") && !ModifiablePath.StartsWith("/Engine/")) {
				FString PluginName;
				ModifiablePath.Split("/", nullptr, &PluginName, ESearchCase::IgnoreCase, ESearchDir::FromStart);
				PluginName.Split("/", &PluginName, nullptr, ESearchCase::IgnoreCase, ESearchDir::FromStart);

				if (GetPlugin(PluginName) == nullptr) {
					CreatePlugin(PluginName);
				}
			}
		}
	} else {
		FString RootName; {
			ModifiablePath.Split("/", nullptr, &RootName, ESearchCase::IgnoreCase, ESearchDir::FromStart);
			RootName.Split("/", &RootName, nullptr, ESearchCase::IgnoreCase, ESearchDir::FromStart);
		}

		if (RootName != "Game" && RootName != "Engine" && GetPlugin(RootName) == nullptr) {
			CreatePlugin(RootName);
		}

		ModifiablePath.Split("/", &ModifiablePath, nullptr, ESearchCase::IgnoreCase, ESearchDir::FromEnd);

		ModifiablePath = ModifiablePath + "/";

		FRRedirects::Redirect(ModifiablePath);
	}

	const FString PathWithGame = ModifiablePath + Name;

	if (PathWithGame.Contains(TEXT("//"), ESearchCase::CaseSensitive) || PathWithGame == "None" || PathWithGame.IsEmpty()) {
		FailureReason = "Attempted to create a package with name containing double slashes.\n\nUpdate your configuration to use a valid Export Directory.";
		return nullptr;
	}
	
	UPackage* Package = CreateAssetPackage(*PathWithGame);
	Package->FullyLoad();

	return Package;
}

UPackage* FAssetUtilities::CreateAssetPackage(const FString& Name, const FString& OutputPath) {
	FString StringIgnore = "";
	
	return CreateAssetPackage(Name, OutputPath, StringIgnore);
}

/* ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ */
template bool FAssetUtilities::ConstructAsset<UMaterialInterface>(const FString& Path, const FString& RealPath, const FString& Type, TObjectPtr<UMaterialInterface>& OutObject, bool& bSuccess);
template bool FAssetUtilities::ConstructAsset<USubsurfaceProfile>(const FString& Path, const FString& RealPath, const FString& Type, TObjectPtr<USubsurfaceProfile>& OutObject, bool& bSuccess);
template bool FAssetUtilities::ConstructAsset<UTexture>(const FString& Path, const FString& RealPath, const FString& Type, TObjectPtr<UTexture>& OutObject, bool& bSuccess);
template bool FAssetUtilities::ConstructAsset<UAnimSequence>(const FString& Path, const FString& RealPath, const FString& Type, TObjectPtr<UAnimSequence>& OutObject, bool& bSuccess);
template bool FAssetUtilities::ConstructAsset<UMaterialParameterCollection>(const FString& Path, const FString& RealPath, const FString& Type, TObjectPtr<UMaterialParameterCollection>& OutObject, bool& bSuccess);
template bool FAssetUtilities::ConstructAsset<USoundWave>(const FString& Path, const FString& RealPath, const FString& Type, TObjectPtr<USoundWave>& OutObject, bool& bSuccess);
template bool FAssetUtilities::ConstructAsset<UObject>(const FString& Path, const FString& RealPath, const FString& Type, TObjectPtr<UObject>& OutObject, bool& bSuccess);
template bool FAssetUtilities::ConstructAsset<UMaterialFunctionInterface>(const FString& Path, const FString& RealPath, const FString& Type, TObjectPtr<UMaterialFunctionInterface>& OutObject, bool& bSuccess);
template bool FAssetUtilities::ConstructAsset<USoundNode>(const FString& Path, const FString& RealPath, const FString& Type, TObjectPtr<USoundNode>& OutObject, bool& bSuccess);
template bool FAssetUtilities::ConstructAsset<UCurveLinearColor>(const FString& Path, const FString& RealPath, const FString& Type, TObjectPtr<UCurveLinearColor>& OutObject, bool& bSuccess);
template bool FAssetUtilities::ConstructAsset<UTextureLightProfile>(const FString&, const FString&, const FString&, TObjectPtr<UTextureLightProfile>&, bool&);
template bool FAssetUtilities::ConstructAsset<UFontFace>(const FString&, const FString&, const FString&, TObjectPtr<UFontFace>&, bool&);

/* Importing assets from Cloud */
template <typename T>
bool FAssetUtilities::ConstructAsset(const FString& Path, const FString& RealPath, const FString& Type, TObjectPtr<T>& OutObject, bool& bSuccess) {
	if (Type.IsEmpty()) {
		return false;
	}

	/* Reached from the middle of property deserialization, which has nowhere to put a callback,
	 * so the requests below have to be waited on. The scope is what keeps the editor drawn and
	 * cancellable while that happens. */
	const FBlockingRequestScope BlockingScope(FText::Format(
		NSLOCTEXT("Reflection", "CloudReflecting", "Reflecting {0}"),
		FText::FromString(Path)
	));

	/* Supported Texture Classes */
	const bool IsTexture = Type ==
		"Texture2D" ||
		Type == "TextureRenderTarget2D" ||
		Type == "TextureCube" ||
		Type == "VolumeTexture" ||
		Type == "TextureLightProfile";

	FString GamePath = Path;

	/* Supported Assets */
	if (CanImport(Type, true) || IsTexture) {
		if (IsTexture) {
			UTexture* Texture;
			const FString NewPath = RealPath;

			FString RootName; {
				NewPath.Split("/", nullptr, &RootName, ESearchCase::IgnoreCase, ESearchDir::FromStart);
				RootName.Split("/", &RootName, nullptr, ESearchCase::IgnoreCase, ESearchDir::FromStart);
			}

			/* Missing Plugin: Create it */
			if (RootName != "Game" && RootName != "Engine" && GetPlugin(RootName) == nullptr) {
				CreatePlugin(RootName);
			}

			bSuccess = Construct_TypeTexture(NewPath, Path, Texture);
			if (bSuccess) OutObject = Cast<T>(Texture);

			return true;
		}
		
		const TSharedPtr<FJsonObject> Response = Cloud::Export::GetRawBlocking(Path);
		if (Response == nullptr || Path.IsEmpty()) return true;

		if (Response->HasField(TEXT("errored"))) {
			UE_LOG(LogReflection, Log, TEXT("Error from response \"%s\""), *Path);
			return true;
		}

		if (Type == "SoundWave") {
			const TSharedPtr<FJsonObject> ObjectResponse = Cloud::Export::GetRawBlocking(Path, {
				{
					"save",
					"true"
				}
			});
					
			if (ObjectResponse == nullptr) return true;
					
			ISoundGraph::OnDownloadSoundWave(ObjectResponse->GetStringField(TEXT("file")), Path, nullptr);
			
			return true;
		}

		const TSharedPtr<FJsonObject> JsonObject = Response->GetArrayField(TEXT("exports"))[0]->AsObject();
		FString PackagePath;
		FString AssetName;
		RealPath.Split(".", &PackagePath, &AssetName);

		if (JsonObject) {
			const FString NewPath = PackagePath;

			FString RootName; {
				NewPath.Split("/", nullptr, &RootName, ESearchCase::IgnoreCase, ESearchDir::FromStart);
				RootName.Split("/", &RootName, nullptr, ESearchCase::IgnoreCase, ESearchDir::FromStart);
			}

			if (RootName != "Game" && RootName != "Engine" && GetPlugin(RootName) == nullptr) {
				CreatePlugin(RootName);
			}

			IImporter* OutImporter;
			bSuccess = IImportReader::ReadExportsAndImport(Response->GetArrayField(TEXT("exports")), PackagePath, OutImporter, true);

			/* Define found object */
			FString RedirectedPath = RealPath;
			
			FRRedirects::Redirect(RedirectedPath);
			OutObject = Cast<T>(StaticLoadObject(T::StaticClass(), nullptr, *RedirectedPath));

			return OutObject != nullptr;
		}
	}

	return false;
}

bool FAssetUtilities::Construct_TypeTexture(const FString& Path, const FString& FetchPath, UTexture*& OutTexture) {
	if (Path.IsEmpty()) {
		return false;
	}

	const TSharedPtr<FJsonObject> JsonObject = Cloud::Export::GetRawBlocking(FetchPath);
	if (JsonObject == nullptr) {
		return false;
	}

	TArray<TSharedPtr<FJsonValue>> Response = JsonObject->GetArrayField(TEXT("exports"));
	if (Response.Num() == 0) {
		return false;
	}

	const TSharedPtr<FJsonObject> JsonExport = Response[0]->AsObject();
	const FString Type = JsonExport->GetStringField(TEXT("Type"));

	bool IsVectorDisplacementMap = false;

	if (JsonExport->HasField(TEXT("Properties"))) {
		if (JsonExport->GetObjectField(TEXT("Properties"))->HasField(TEXT("CompressionSettings"))) {
			IsVectorDisplacementMap =
				JsonExport->GetObjectField(TEXT("Properties"))->GetStringField(TEXT("CompressionSettings")).Contains("TC_VectorDisplacementmap")
				|| JsonExport->GetObjectField(TEXT("Properties"))->GetStringField(TEXT("CompressionSettings")).Contains("TC_HDR");
		}
	}
	
	TArray<uint8> Data = TArray<uint8>();
	bool UseOctetStream = ShouldUseOctetStream(Type, IsVectorDisplacementMap);

	/* ~~~~~~~~~~~~~~~ Download Texture Data ~~~~~~~~~~~~ */
	if (Type != "TextureRenderTarget2D") {
		const FReflectionHttpRequest HttpRequest = FHttpModule::Get().CreateRequest();

		HttpRequest->SetURL(Cloud::URL + "/api/export?path=" + FetchPath);
		HttpRequest->SetHeader("content-type", UseOctetStream ? "application/octet-stream" : "image/png");
		HttpRequest->SetVerb(TEXT("GET"));

		const FReflectionHttpResponse HttpResponse = FRemoteUtilities::ExecuteRequestBlocking(HttpRequest);

		if (!HttpResponse.IsValid() || HttpResponse->GetResponseCode() != 200)
			return false;

		if (HttpResponse->GetContentType().StartsWith("application/json; charset=utf-8")) {
			return false;
		}

		Data = HttpResponse->GetContent();
		if (Data.Num() == 0) {
			return false;
		}
	}

	return Fast_Construct_TypeTexture(JsonExport, Path, Type, Data, OutTexture);
}

bool FAssetUtilities::Fast_Construct_TypeTexture(const TSharedPtr<FJsonObject>& JsonExport, const FString& Path, const FString& Type, TArray<uint8> Data, UTexture*& OutTexture) {
	const UReflectionSettings* Settings = GetSettings();
	UTexture* Texture = nullptr;
	
	FString PackagePath;
	FString AssetName; {
		Path.Split(".", &PackagePath, &AssetName);
	}

	bool IsVectorDisplacementMap = false;

	if (JsonExport->HasField(TEXT("Properties"))) {
		if (JsonExport->GetObjectField(TEXT("Properties"))->HasField(TEXT("CompressionSettings"))) {
			IsVectorDisplacementMap = JsonExport->GetObjectField(TEXT("Properties"))->GetStringField(TEXT("CompressionSettings")).Contains("TC_VectorDisplacementmap")
				|| JsonExport->GetObjectField(TEXT("Properties"))->GetStringField(TEXT("CompressionSettings")).Contains("TC_HDR");
		}
	}
	
	bool UseOctetStream = ShouldUseOctetStream(Type, IsVectorDisplacementMap);
	
	FRRedirects::Redirect(PackagePath);

	if (!PackagePath.StartsWith("/Game/") && !PackagePath.StartsWith("/Engine/")) {
		FString PluginName;
		PackagePath.Split("/", nullptr, &PluginName, ESearchCase::IgnoreCase, ESearchDir::FromStart);
		PluginName.Split("/", &PluginName, nullptr, ESearchCase::IgnoreCase, ESearchDir::FromStart);

		if (GetPlugin(PluginName) == nullptr) {
			CreatePlugin(PluginName);
		}
	}
	
	UPackage* Package = CreateAssetPackage(*PackagePath);
	Package->FullyLoad();

	FTextureCreatorUtilities TextureCreator = FTextureCreatorUtilities(AssetName, Path, Package, UseOctetStream);

	if (Type == "Texture2D") {
		TextureCreator.CreateTexture<UTexture2D>(Texture, Data, JsonExport);
	}
	if (Type == "TextureLightProfile") {
		TextureCreator.CreateTexture<UTextureLightProfile>(Texture, Data, JsonExport);
	}
	if (Type == "TextureCube") {
		TextureCreator.CreateTextureCube(Texture, Data, JsonExport);
	}
	if (Type == "VolumeTexture") {
		TextureCreator.CreateVolumeTexture(Texture, Data, JsonExport);
	}
	if (Type == "TextureRenderTarget2D") {
		TextureCreator.CreateRenderTarget2D(Texture, JsonExport->GetObjectField(TEXT("Properties")));
	}

	if (Texture == nullptr) {
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

	/* Save texture */
	if (Settings->AssetSettings.SaveAssets) {
		SavePackage(Package);
	}

	OutTexture = Texture;

	return true;
}
