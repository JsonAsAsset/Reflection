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
	/* Packages with an import open further down the stack, innermost last. */
	TArray<FString> GAssetsUnderConstruction;

	/* The file a path names, without the asset inside it.
	 *
	 * Kept by the file rather than by the asset because a file is imported whole: every export in it
	 * is built by the one run. An HLOD proxy keeps four meshes, four materials and four textures
	 * under the one name, and each mesh reaches for the others while it is being built. Asked for by
	 * asset, those are four separate keys and none of them matches the run already open, so each one
	 * fetches the file again and builds all four again, four levels deeper every time. */
	FString FileOf(const FString& Path) {
		FString Named;

		return Path.Split(TEXT("."), &Named, nullptr, ESearchCase::CaseSensitive, ESearchDir::FromStart) ? Named : Path;
	}

	/* Marks a file as being built for as long as the call constructing it is running, and reports
	 * whether that call is the one that opened it. */
	struct FConstructionScope {
		explicit FConstructionScope(const FString& InPath)
			: Path(FileOf(InPath))
			, bOwned(!GAssetsUnderConstruction.Contains(Path))
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

	/* The asset a reference names, wherever it ended up.
	 *
	 * A reference says the file the asset was cooked in and the asset inside it. Kept together that
	 * is where it lands and the path reads straight through. Split apart it lands in a file of its
	 * own beside the others, and the same reference has to be read a second way: the folder that
	 * file is in, and the asset's own name. */
	template <typename T>
	TObjectPtr<T> FindByReference(const FString& Path) {
		FString Redirected = Path;
		FRRedirects::Redirect(Redirected);

		if (TObjectPtr<T> Found = LoadObjectByPath<T>(Redirected)) {
			return Found;
		}

		FString Folder, File, Asset;

		if (!Redirected.Split(TEXT("."), &Folder, &Asset, ESearchCase::CaseSensitive, ESearchDir::FromEnd)) return nullptr;
		if (!Folder.Split(TEXT("/"), &Folder, &File, ESearchCase::IgnoreCase, ESearchDir::FromEnd)) return nullptr;

		/* The file is already named for the asset, so there is no second spelling to try */
		if (File == Asset) return nullptr;

		/* Nor for a part of an asset. A subobject is named for the thing that holds it and never
		 * becomes a file of its own, so reading it as one would ask for a path nothing is at. */
		if (Asset.Contains(TEXT(":"))) return nullptr;

		return LoadObjectByPath<T>(Folder + TEXT("/") + Asset + TEXT(".") + Asset);
	}
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

	const bool IsTexture = FTextureTypes::IsSupported(Type);

	const FConstructionScope ConstructionScope(Path);

	/* Handed back unresolved only where the run that already has this file open is going to build it.
	 *
	 * That run builds the exports the reader takes and turns the rest down, and what it turns down
	 * is fetched here instead, one asset at a time, as each is referenced. A texture is the standing
	 * example: no package import has ever built one. Declining any of those waits on the open run to
	 * produce something it is never going to produce, and whatever asked comes out empty.
	 *
	 * So the question is not whether the file is open. It is whether the run holding it open builds
	 * this, which is the same question the reader asks of every export it is offered. Asked the same
	 * way here, a type nobody has taught the reader about still gets fetched rather than dropped. */
	if (!ConstructionScope.bOwned && CanImport(Type)) {
		OutObject = FindByReference<T>(RealPath);
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
			OutObject = FindByReference<T>(RealPath);

			return OutObject != nullptr;
		}
	}

	return false;
}

/* Textures live in FTextureImport, this is the seam other tools still reach through */
bool FAssetUtilities::Fast_Construct_TypeTexture(const TSharedPtr<FJsonObject>& JsonExport, const FString& Path, const FString& Type, TArray<uint8> Data, UTexture*& OutTexture) {
	return FTextureImport::FromExport(JsonExport, Path, Type, Data, OutTexture);
}
