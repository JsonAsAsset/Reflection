/* Copyright Reflection Contributors 2024-2026 */

#include "Importers/Constructor/ImportReader.h"

#include "Importers/Constructor/ImportIssues.h"
#include "Importers/Constructor/Importer.h"
#include "Importers/Constructor/TemplatedImporter.h"
#include "Importers/Constructor/Types.h"
#include "Importers/Types/DataAssetImporter.h"
#include "Importers/Types/Texture/TextureImporter.h"
#include "Settings/Runtime.h"
#include "Styling/SlateIconFinder.h"
#include "Importers/Constructor/Asset.h"
#include "Engine/EngineUtilities.h"
#include "Utilities/JsonHelpers.h"
#include "Modules/Cloud/Remote.h"
#include "UObject/UObjectHash.h"
#include "Settings/SettingsAccess.h"

#if REFLECTION_RIGVM
#include "Engine/ControlRigCompatibility.h"
#endif

namespace {
	/* Every export of this package that one export points at.
	 *
	 * A reference inside a package is written as the package's own path with the index of what it
	 * names on the end, so the index is read straight off it. Anything naming a different package is
	 * somebody else's asset and is fetched rather than ordered around. */
	void ReadSiblings(const TSharedPtr<FJsonValue>& Value, const FString& Package, const int32 Count, TSet<int32>& Out) {
		if (!Value.IsValid()) return;

		if (Value->Type == EJson::Object) {
			const TSharedPtr<FJsonObject> Object = Value->AsObject();

			FString Path;

			if (Object->TryGetStringField(TEXT("ObjectPath"), Path)) {
				FString Named, Index;

				if (Path.Split(TEXT("."), &Named, &Index, ESearchCase::CaseSensitive, ESearchDir::FromEnd)
					&& Named == Package && Index.IsNumeric()) {
					const int32 At = FCString::Atoi(*Index);

					if (At >= 0 && At < Count) Out.Add(At);
				}
			}

			for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Object->Values) {
				ReadSiblings(Pair.Value, Package, Count, Out);
			}

			return;
		}

		if (Value->Type == EJson::Array) {
			for (const TSharedPtr<FJsonValue>& One : Value->AsArray()) {
				ReadSiblings(One, Package, Count, Out);
			}
		}
	}

	/* The order the exports of one package have to be built in.
	 *
	 * A package holding one asset needs none of this. One holding several an HLOD proxy keeps its
	 * mesh, the material that draws it and the texture that material samples, all in the one file
	 * has them pointing at each other, and each becomes an asset of its own here. Built in the order
	 * they happen to be written, a material reaches for a texture that has not been made yet and
	 * comes out without it.
	 *
	 * So whatever an export points at is built before it. Depth first, and a cycle is left in the
	 * order it arrived: two things naming each other cannot both go first, and the pass that fills
	 * references in is not this one. */
	TArray<FUObjectExport*> InDependencyOrder(FUObjectExportContainer* Container) {
		const int32 Count = Container->Exports.Num();

		TArray<FUObjectExport*> Ordered;
		Ordered.Reserve(Count);

		if (Count < 2) {
			Ordered = Container->Exports;

			return Ordered;
		}

		/* The path they are all written under, which is what a reference between them looks like */
		FString Package;

		for (const FUObjectExport* Export : Container->Exports) {
			if (Export->IsJsonValid() && Export->JsonObject->TryGetStringField(TEXT("Package"), Package)) break;
		}

		if (Package.IsEmpty()) {
			Ordered = Container->Exports;

			return Ordered;
		}

		TArray<TSet<int32>> Wants;
		Wants.SetNum(Count);

		for (int32 Index = 0; Index < Count; ++Index) {
			const FUObjectExport* Export = Container->Exports[Index];

			if (!Export->IsJsonValid()) continue;

			ReadSiblings(MakeShared<FJsonValueObject>(Export->JsonObject), Package, Count, Wants[Index]);

			/* Nothing is its own dependency, however it names itself */
			Wants[Index].Remove(Index);
		}

		TArray<uint8> Done;
		Done.SetNumZeroed(Count);

		TArray<int32> Open;

		TFunction<void(int32)> Walk = [&](const int32 Index) {
			/* Already placed, or being placed further up this same walk, which is the cycle */
			if (Done[Index] != 0 || Open.Contains(Index)) return;

			Open.Push(Index);

			for (const int32 Wanted : Wants[Index]) {
				Walk(Wanted);
			}

			Open.Pop();

			Done[Index] = 1;

			Ordered.Add(Container->Exports[Index]);
		};

		for (int32 Index = 0; Index < Count; ++Index) {
			Walk(Index);
		}

		return Ordered;
	}
}

bool IImportReader::ReadExportsAndImport(const TArray<TSharedPtr<FJsonValue>>& Exports, const FString& File, IImporter*& OutImporter, const bool HideNotifications, bool bUseRelativePath) {
	/* Importers resolve references through the Cloud while they deserialize, and those requests
	 * have nowhere to put a callback, so they get waited on. The scope is what keeps the editor
	 * drawn and cancellable for as long as this import needs the Cloud. */
	const FBlockingRequestScope BlockingScope(FText::Format(
		NSLOCTEXT("Reflection", "CloudImporting", "Reflecting {0}"),
		FText::FromString(FPaths::GetCleanFilename(File))
	));

	/* One import, however many references it reaches through */
	const FAssetUtilities::FRunScope RunScope;

	FUObjectExportContainer* Container = new FUObjectExportContainer(Exports);

	const bool IsBlueprint = Container->FindByType(FString("BlueprintGeneratedClass"))->IsJsonValid();
	
	for (FUObjectExport* Export : InDependencyOrder(Container)) {
		if (IsBlueprint) {
			if (Export->GetType() != "BlueprintGeneratedClass") continue;
		}
		
		if (IImporter* Importer = ReadExportAndImport(Container, Export, File, HideNotifications, bUseRelativePath)) OutImporter = Importer;
	}

	/* Nothing matched. Every export gets offered to the reader and most of them are subobjects with
	 * no importer of their own, so this is only worth saying once the whole package has been walked
	 * and come to nothing: otherwise the run counts a file it did not import as one it did. */
	if (OutImporter == nullptr) {
		const FString Name = FPaths::GetBaseFilename(File);

		FString Type;

		for (FUObjectExport* Export : Container->Exports) {
			const FString ExportType = Export->GetType().ToString();

			if (ExportType.IsEmpty()) continue;

			const FString ExportName = Export->GetName().ToString();

			/* The one named after the package is the asset; anything else is a part of it. A
			 * blueprint's class carries a _C the package it lives in does not. */
			if (ExportName == Name || ExportName == Name + TEXT("_C")) {
				Type = ExportType;

				break;
			}

			if (Type.IsEmpty()) Type = ExportType;
		}

		/* An importer switched off rather than absent. It reads the same from here and is not the
		 * same thing at all, so it is worth saying which of the two happened. */
		if (ImportTypes::Experimental.Contains(Type) && !GetSettings()->EnableExperiments) {
			FImportIssues::ReportFor(Name, File, Type, EImportIssue::Setting,
				TEXT("This type is behind Enable Experiments"),
				FString::Printf(
					TEXT("'%s' is a %s, which Reflection imports once Enable Experiments is turned on in its settings."),
					*Name, *Type)
			);

			return false;
		}

		FImportIssues::ReportFor(Name, File, Type, EImportIssue::MissingClass,
			FString::Printf(TEXT("Nothing here that can be imported")),
			FString::Printf(
				TEXT("'%s' is a %s, and this build has no importer for one. The file was read and nothing was made from it."),
				*Name, Type.IsEmpty() ? TEXT("type it doesn't recognise") : *Type)
		);

		return false;
	}

	return true;
}

IImporter* IImportReader::ReadExportAndImport(FUObjectExportContainer* Container, FUObjectExport* Export, FString File, const bool HideNotifications, bool bUseRelativePath) {
	const FString Type = Export->GetType().ToString();
	FString Name = Export->GetName().ToString();

	const bool IsBlueprint = Type.Contains("BlueprintGeneratedClass");

	/* BlueprintGeneratedClass is post-fixed with _C */
	if (IsBlueprint) {
		Name.Split("_C", &Name, nullptr, ESearchCase::CaseSensitive, ESearchDir::FromEnd);
	}

	/* A font's faces are part of the font.
	 *
	 * A face kept inside a font is a subobject of it, and the font's own import builds it along with
	 * everything else it holds: DeserializeExports runs before a single property is read. Offered
	 * here as well it comes out a second time, beside the font, under a name the game never cooked
	 * and so with no typeface to fill it from.
	 *
	 * Skipped before the package is made rather than refused afterwards, since the package is made
	 * whether the importer wants it or not and an empty one left behind shows as a folder.
	 *
	 * Said of font faces rather than of subobjects at large, deliberately. Plenty is held this way
	 * an offline font holds its texture pages exactly so and those are built here today, with
	 * their contents fetched as they are. Nothing would rebuild them if this stopped offering them,
	 * and what came out instead would be the empty subobject this went to the trouble of curing. */
	if (Type == TEXT("FontFace") && Container->Find(Export->GetOuter())->IsJsonValid()) {
		return nullptr;
	}

	const UClass* Class = FindClassByType(Type);

#if REFLECTION_RIGVM
	/* A curve mapping drawn as a rig does not need the class the game kept it in.
	 *
	 * That class comes with a plugin an engine may not have, and where it is missing there is
	 * nothing here to hold the asset. What the mapping is being turned into needs none of it: the
	 * expressions come from the Cloud and what they are drawn into is a rig. So the rig's own class
	 * stands in and the import goes ahead. */
	if (Class == nullptr && Type == TEXT("CurveExpressionsDataAsset") && ImportsCurveMappingAsRig()) {
		Class = UControlRigBlueprint::StaticClass();
	}
#endif

	if (Class == nullptr) return nullptr;

	/* Check if this export can be imported */
	const bool InheritsDataAsset = Class->IsChildOf(UDataAsset::StaticClass());
	if (!CanImport(Type, false, Class)) return nullptr;

	if (bUseRelativePath) {
		/* Convert from relative path to full path */
		if (FPaths::IsRelative(File)) File = FPaths::ConvertRelativePathToFull(File);
	}

	/* Named for the file it came in, or for itself where they are being split apart */
	const FString PackageName = GetSettings()->AssetSettings.PackagedAssets == ERPackagedAssets::Separate
		? Name
		: FPaths::GetBaseFilename(File);

	FString FailureReason;
	UPackage* LocalPackage = FAssetUtilities::CreateAssetPackage(PackageName, File, FailureReason);

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
		FImportIssues::ReportFor(Name, FString(), Type, EImportIssue::Failed, TEXT("Couldn't create a package for this asset"), FailureReason);

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

	Export->Package = LocalPackage;
	Importer->Initialize(Export, Container);

	/* Everything the import reports from here on belongs to this asset, including its references */
	const FImportIssueScope IssueScope(Name, LocalPackage->GetName(), Type);

	/* Import the asset ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ */
	bool Successful = false; {
		try {
			Successful = Importer->Import();
		} catch (const char* Exception) {
			UE_LOG(LogReflection, Error, TEXT("Importer exception: %s"), *FString(Exception));

			FImportIssues::Report(EImportIssue::Failed, TEXT("The importer threw partway through"), FString(Exception));
		}
	}

	if (!Successful) {
		FImportIssues::Report(EImportIssue::Failed, TEXT("The importer couldn't build this asset"));

		/* The package is made before the importer runs, so an import that builds nothing leaves an
		 * empty one behind and the Content Browser shows the folder it would have lived in.
		 *
		 * Only where it is empty. A package kept together is built by several passes through here,
		 * and one of them failing says nothing about the ones that worked: throwing the package away
		 * then would take everything already in it along with the failure. */
		TArray<UObject*> Held;
		GetObjectsWithOuter(LocalPackage, Held, false);

		if (Export->Object == nullptr && Held.Num() == 0) {
			LocalPackage->SetDirtyFlag(false);
			LocalPackage->ClearFlags(RF_Public | RF_Standalone);

#if ENGINE_UE5
			LocalPackage->MarkAsGarbage();
#else
			LocalPackage->MarkPendingKill();
#endif
		}
	}

	if (HideNotifications) {
		return Importer;
	}

	FString ClassIconType = Type;

	if (Type.Contains("GeneratedClass")) {
		Type.Split("GeneratedClass", &ClassIconType, nullptr);
	}

	/* Failures are on the Reflection Errors window rather than on a notification that expires */
	if (Successful) {
		UE_LOG(LogReflection, Log, TEXT("Successfully reflected \"%s\" as \"%s\""), *Name, *Type);

		AppendNotification(
			FText::FromString("Reflected: " + Name),
			FText::FromString(Type),
			2.0f,
			FSlateIconFinder::FindCustomIconBrushForClass(FindObject<UClass>(nullptr, *("/Script/Engine." + ClassIconType)), TEXT("ClassThumbnail")),
			SNotificationItem::CS_Success,
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