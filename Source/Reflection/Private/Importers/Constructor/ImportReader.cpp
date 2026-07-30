/* Copyright Reflection Contributors 2024-2026 */

#include "Importers/Constructor/ImportReader.h"

#include "Importers/Constructor/Importer.h"
#include "Importers/Constructor/TemplatedImporter.h"
#include "Importers/Types/DataAssetImporter.h"
#include "Importers/Types/Texture/TextureImporter.h"
#include "Settings/Runtime.h"
#include "Styling/SlateIconFinder.h"
#include "Utilities/AssetUtilities.h"
#include "Engine/EngineUtilities.h"
#include "Utilities/JsonUtilities.h"
#include "Utilities/RemoteUtilities.h"

bool IImportReader::ReadExportsAndImport(const TArray<TSharedPtr<FJsonValue>>& Exports, const FString& File, IImporter*& OutImporter, const bool HideNotifications) {
	/* Importers resolve references through the Cloud while they deserialize, and those requests
	 * have nowhere to put a callback, so they get waited on. The scope is what keeps the editor
	 * drawn and cancellable for as long as this import needs the Cloud. */
	const FBlockingRequestScope BlockingScope(FText::Format(
		NSLOCTEXT("Reflection", "CloudImporting", "Reflecting {0}"),
		FText::FromString(FPaths::GetCleanFilename(File))
	));

	FUObjectExportContainer* Container = new FUObjectExportContainer(Exports);

	const bool IsBlueprint = Container->FindByType(FString("BlueprintGeneratedClass"))->IsJsonValid();
	
	for (FUObjectExport* Export : Container->Exports) {
		if (IsBlueprint) {
			if (Export->GetType() != "BlueprintGeneratedClass") continue;
		}
		
		if (IImporter* Importer = ReadExportAndImport(Container, Export, File, HideNotifications)) OutImporter = Importer;
	}

	return true;
}

IImporter* IImportReader::ReadExportAndImport(FUObjectExportContainer* Container, FUObjectExport* Export, FString File, const bool HideNotifications) {
	const FString Type = Export->GetType().ToString();
	FString Name = Export->GetName().ToString();

	const bool IsBlueprint = Type.Contains("BlueprintGeneratedClass");

	/* BlueprintGeneratedClass is post-fixed with _C */
	if (IsBlueprint) {
		Name.Split("_C", &Name, nullptr, ESearchCase::CaseSensitive, ESearchDir::FromEnd);
	}

	const UClass* Class = FindClassByType(Type);
	
	if (Class == nullptr) return nullptr;

	/* Check if this export can be imported */
	const bool InheritsDataAsset = Class->IsChildOf(UDataAsset::StaticClass());
	if (!CanImport(Type, false, Class)) return nullptr;

	/* Convert from relative path to full path */
	if (FPaths::IsRelative(File)) File = FPaths::ConvertRelativePathToFull(File);

	FString FailureReason;
	UPackage* LocalPackage = FAssetUtilities::CreateAssetPackage(Name, File, FailureReason);

	if (LocalPackage == nullptr) {
		/* Try fixing our Export Directory Settings using the provided File directory if local package not found */
        UReflectionSettings* PluginSettings = GetSettings();

		GReflectionRuntime.Update();
		LocalPackage = FAssetUtilities::CreateAssetPackage(Name, File, FailureReason);

		if (LocalPackage == nullptr) {
			FString ExportDirectoryCache = GReflectionRuntime.ExportDirectory.Path;
		
			if (FString DirectoryPathFix; File.Split(TEXT("Output/Exports/"), &DirectoryPathFix, nullptr, ESearchCase::IgnoreCase, ESearchDir::FromEnd)) {
				DirectoryPathFix = DirectoryPathFix + TEXT("Output/Exports");

				GReflectionRuntime.ExportDirectory.Path = DirectoryPathFix;
				SavePluginSettings(PluginSettings);

				/* Retry creating the asset package */
				LocalPackage = FAssetUtilities::CreateAssetPackage(Name, File, FailureReason);

				/* Undo the change if unsuccessful */
				if (LocalPackage == nullptr) {
					GReflectionRuntime.ExportDirectory.Path = ExportDirectoryCache;

					SavePluginSettings(PluginSettings);
				}
			}
		}
	}

	if (LocalPackage == nullptr) {
		AppendNotification(
			FText::FromString("Failed: " + Type),
			FText::FromString(FailureReason),
			4.0f,
			FSlateIconFinder::FindCustomIconBrushForClass(FindObject<UClass>(nullptr, *("/Script/Engine." + Type)), TEXT("ClassThumbnail")),
			SNotificationItem::CS_Fail,
			false,
			350.0f
		);

		return nullptr;
	}

	/* Importer ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ */
	IImporter* Importer = nullptr;
	
	/* Try to find the importer using a factory delegate */
	if (const FImporterFactoryDelegate* Factory = FindFactoryForAssetType(Type)) {
		Importer = (*Factory)();
	}

	/* If it inherits DataAsset, use the data asset importer */
	if (Importer == nullptr && InheritsDataAsset) {
		Importer = new IDataAssetImporter();
	}

	/* By default, (with no existing importer) use the templated importer with the asset class. */
	if (Importer == nullptr) {
		Importer = new ITemplatedImporter<UObject>();
	}

	/* TODO: Don't hardcode this. */
	if (ImportTypes::Cloud::Extra.Contains(Type)) {
		Importer = new ITextureImporter<UTextureLightProfile>();
	}

	Export->Package = LocalPackage;
	Importer->Initialize(Export, Container);

	/* Import the asset ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ */
	bool Successful = false; {
		try {
			Successful = Importer->Import();
		} catch (const char* Exception) {
			UE_LOG(LogReflection, Error, TEXT("Importer exception: %s"), *FString(Exception));
		}
	}

	if (HideNotifications) {
		return Importer;
	}

	FString ClassIconType = Type;

	if (Type.Contains("GeneratedClass")) {
		Type.Split("GeneratedClass", &ClassIconType, nullptr);
	}

	if (Successful) {
		UE_LOG(LogReflection, Log, TEXT("Successfully reflected \"%s\" as \"%s\""), *Name, *Type);

		/* Successful Notification */
		AppendNotification(
			FText::FromString("Reflected: " + Name),
			FText::FromString(Type),
			2.0f,
			FSlateIconFinder::FindCustomIconBrushForClass(FindObject<UClass>(nullptr, *("/Script/Engine." + ClassIconType)), TEXT("ClassThumbnail")),
			SNotificationItem::CS_Success,
			false,
			350.0f
		);
	} else {
		/* Failed Notification */
		AppendNotification(
			FText::FromString("Failed: " + Name),
			FText::FromString(Type),
			2.0f,
			FSlateIconFinder::FindCustomIconBrushForClass(FindObject<UClass>(nullptr, *("/Script/Engine." + ClassIconType)), TEXT("ClassThumbnail")),
			SNotificationItem::CS_Fail,
			false,
			350.0f
		);
	}

	return Importer;
}

IImporter* IImportReader::ImportReference(const FString& File) {
	FString FilePath = File;
	if (FilePath.Contains("\\")) {
		FilePath = File.Replace(TEXT("\\"), TEXT("/"));
	}
	
	TArray<TSharedPtr<FJsonValue>> DataObjects; {
		DeserializeJSON(FilePath, DataObjects);
	}

	IImporter* Importer = nullptr;
	ReadExportsAndImport(DataObjects, FilePath, Importer);
	
	return Importer;
}