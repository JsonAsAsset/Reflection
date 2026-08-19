/* Copyright Reflection Contributors 2024-2026 */

#include "Importers/Constructor/Asset.h"

#include "Animation/Skeleton.h"

#include "Importers/Constructor/Importer.h"

#include "Importers/Types/Texture/TextureImporter.h"
#include "Importers/Types/Texture/TextureTypes.h"

#include "Curves/CurveLinearColor.h"
#include "Engine/TextureLightProfile.h"
#include "Sound/SoundNode.h"
#include "Engine/SubsurfaceProfile.h"
#include "Materials/MaterialParameterCollection.h"
#include "Settings/ReflectionSettings.h"
#include "Dom/JsonObject.h"

#include "Engine/FontFace.h"
#include "Importers/Constructor/ImportIssues.h"
#include "Importers/Constructor/ImportReader.h"
#include "Importers/Constructor/Graph/SoundGraph.h"
#include "Modules/Cloud/Cloud.h"
#include "Settings/Runtime.h"

/* CreateAssetPackage Implementations ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ */
UPackage* FAssetUtilities::CreateAssetPackage(const FString& Path) {
	/* A path off disk rather than a package path gets this far when the export was reflected from
	 * somewhere outside the project, and the asset registry crashes on one that has no leading
	 * slash rather than refusing it. */
	if (!FPackageName::IsValidLongPackageName(Path)) {
		FImportIssues::Report(
			EImportIssue::Failed,
			TEXT("Not a package path"),
			FString::Printf(TEXT("\"%s\" is not one. Reflect from inside the export folder so the path resolves against the game's own."), *Path)
		);

		return nullptr;
	}

	UPackage* Package = CreatePackage(
		/* 4.25, 4.26.0 and below need an Outer */
#if UE4_25_BELOW || (UE4_26_0)
		nullptr,
#endif
		*Path);
	Package->FullyLoad();

	/* Reflected assets land in folders this project has never had, and the Content Browser builds
	 * its folder tree from the asset registry's cached paths: a folder it has not been told about
	 * cannot be navigated to, which is what stops the jump at the end of an import.
	 *
	 * Told here rather than at the jump because being told is not the same as being ready. The
	 * browser builds the folder on a tick of its own, and doing it now gives it the whole length
	 * of the import to get there instead of no time at all. */
	IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry").Get();
	AssetRegistry.AddPath(FPackageName::GetLongPackagePath(Path));

	return Package;
}

UPackage* FAssetUtilities::CreateAssetPackage(const FString& Name, const FString& OutputPath, FString& FailureReason) {
	const FString& ProjectName = GReflectionRuntime.Profile.ProjectName;

	FString ModifiablePath = OutputPath;

	/* References Automatically Formatted */
	if (!ModifiablePath.StartsWith("/Game/") && !ModifiablePath.StartsWith("/Plugins/") && ModifiablePath.Contains("/Content/")) {
		if (!ProjectName.IsEmpty()) {
			ModifiablePath = ModifiablePath.Replace(*(ProjectName + "/Content"), TEXT("/Game"));
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

	/* Null when the path is not a package path, which the overload above reports for itself */
	if (Package == nullptr) {
		FailureReason = "\"" + PathWithGame + "\" is not a package path.\n\nReflect from inside the export folder so the path resolves against the game's own.";

		return nullptr;
	}

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

/* A skeletal mesh is skinned to its skeleton, so an import reaches for one the project may not
 * have yet */
template bool FAssetUtilities::ConstructAsset<USkeleton>(const FString&, const FString&, const FString&, TObjectPtr<USkeleton>&, bool&);

namespace {
	/* Paths with an import open further down the stack, innermost last. */
	TArray<FString> GAssetsUnderConstruction;

	/* Marks a path as being built for as long as the call constructing it is running, and reports
	 * whether that call is the one that opened it. */
	struct FConstructionScope {
		explicit FConstructionScope(const FString& InPath)
			: Path(InPath)
			, bOwned(!GAssetsUnderConstruction.Contains(InPath))
		{
			if (bOwned) {
				GAssetsUnderConstruction.Add(Path);
			}
		}

		~FConstructionScope() {
			if (bOwned) {
				GAssetsUnderConstruction.Remove(Path);
			}
		}

		FString Path;
		bool bOwned;
	};
}

/* Importing assets from Cloud */
template <typename T>
bool FAssetUtilities::ConstructAsset(const FString& Path, const FString& RealPath, const FString& Type, TObjectPtr<T>& OutObject, bool& bSuccess) {
	if (Type.IsEmpty()) {
		return false;
	}

	/* References run both ways between materials and the functions they call, so a reference met
	 * partway through an import can name an asset whose import is already open further down the
	 * stack. Building it a second time is what breaks: the second pass empties the expression list
	 * and rebuilds the same expression names under the same outer, and NewObject on a name already
	 * taken destructs whatever holds it and constructs the replacement in that same allocation.
	 * The pass still running below is left holding those pointers, and finishes by walking freed
	 * memory: a virtual call on a dead vtable as soon as anything traces an input.
	 *
	 * The asset itself exists by this point, since it is created before its graph is filled in, so
	 * what is in memory is what the reference wants. It is handed back half built and the import
	 * that owns it finishes it. */
	if (!bImportReferences && Type != TEXT("Skeleton")) {
		return false;
	}

	const FConstructionScope ConstructionScope(Path);

	if (!ConstructionScope.bOwned) {
		FString InFlightPath = RealPath;
		FRRedirects::Redirect(InFlightPath);

		OutObject = LoadObjectByPath<T>(InFlightPath);
		bSuccess = OutObject != nullptr;

		return true;
	}

	/* Every path out of here is a request. With no Cloud to answer them, each reference would sit
	 * on a connection that is never going to be made, behind a scope announcing it, and end up
	 * exactly where it started. */
	if (!Cloud::Status::IsOpened()) {
		return false;
	}

	/* Reached from the middle of property deserialization, which has nowhere to put a callback,
	 * so the requests below have to be waited on. The scope is what keeps the editor drawn and
	 * cancellable while that happens. */
	const FBlockingRequestScope BlockingScope(FText::Format(
		NSLOCTEXT("Reflection", "CloudReflecting", "Reflecting {0}"),
		FText::FromString(Path)
	));

	const bool IsTexture = FTextureTypes::IsSupported(Type);

	FString GamePath = Path;

	/* Supported Assets */
	if (CanImport(Type, true) || IsTexture) {
		if (IsTexture) {
			UTexture* Texture = nullptr;

			bSuccess = FTextureImport::FromCloud(RealPath, Path, Texture);
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
			OutObject = LoadObjectByPath<T>(RedirectedPath);

			return OutObject != nullptr;
		}
	}

	return false;
}

/* Textures live in FTextureImport, this is the seam other tools still reach through */
bool FAssetUtilities::Fast_Construct_TypeTexture(const TSharedPtr<FJsonObject>& JsonExport, const FString& Path, const FString& Type, TArray<uint8> Data, UTexture*& OutTexture) {
	return FTextureImport::FromExport(JsonExport, Path, Type, Data, OutTexture);
}
