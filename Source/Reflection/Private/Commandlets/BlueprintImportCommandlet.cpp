/* Copyright Reflection Contributors 2024-2026 */

#include "BlueprintImportCommandlet.h"

#include "FileHelpers.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "ScriptDisassembler.h"
/* Given a header of its own after 5.6, and part of the string header before that */
#if __has_include("Misc/StringOutputDevice.h")
#include "Misc/StringOutputDevice.h"
#else
#include "Containers/UnrealString.h"
#endif
#include "Importers/Constructor/Asset.h"
#include "Importers/Constructor/ImportReader.h"
#include "Importers/Constructor/Importer.h"
#include "Modules/Cloud/Cloud.h"
#include "Settings/Runtime.h"
#include "UObject/SavePackage.h"
#include "Interfaces/ITargetPlatformManagerModule.h"
#include "Interfaces/ITargetPlatform.h"
#include "Serialization/ArchiveCookData.h"
#include "UObject/ArchiveCookContext.h"

DECLARE_LOG_CATEGORY_CLASS(LogBlueprintImport, All, All);

int32 UBlueprintImportCommandlet::Main(const FString& Params) {
	UE_LOG(LogBlueprintImport, Display, TEXT("asked for: %s"), *Params);

	FString Path;

	if (!FParse::Value(*Params, TEXT("path="), Path) || Path.IsEmpty()) {
		UE_LOG(LogBlueprintImport, Error, TEXT("no asset to import: pass -path=<cloud asset path>"));

		return 1;
	}

	/* The runtime carries the project name a cloud path is turned into an editor one with, and
	 * nothing has fetched it in a run with no editor around it */
	Cloud::EnsureMetadataBlocking();

	const TSharedPtr<FJsonObject> Response = Cloud::Export::GetRawBlocking(Path);

	if (!Response.IsValid() || !Response->HasField(TEXT("exports"))) {
		UE_LOG(LogBlueprintImport, Error, TEXT("the cloud had nothing at \"%s\""), *Path);

		return 1;
	}

	const TArray<TSharedPtr<FJsonValue>> Exports = Response->GetArrayField(TEXT("exports"));

	UE_LOG(LogBlueprintImport, Display, TEXT("importing \"%s\", %d export(s)"), *Path, Exports.Num());

	IImporter* Importer = nullptr;

	/* Notifications are for somebody watching, and the path is already the one the cloud spells */
	const bool bImported = IImportReader::ReadExportsAndImport(Exports, Path, Importer, true, false);

	if (!bImported || Importer == nullptr) {
		UE_LOG(LogBlueprintImport, Error, TEXT("nothing was made from \"%s\""), *Path);

		return 1;
	}

	/* Saved, because the asset on disk is what the comparison reads */
	TArray<UPackage*> Dirty;
	FEditorFileUtils::GetDirtyContentPackages(Dirty);
	FEditorFileUtils::GetDirtyWorldPackages(Dirty);

	int32 Saved = 0;

	for (UPackage* Package : Dirty) {
		const FString FileName = FPackageName::LongPackageNameToFilename(Package->GetName(), FPackageName::GetAssetPackageExtension());

		FSavePackageArgs SaveArgs;
		SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;

		if (UPackage::SavePackage(Package, nullptr, *FileName, SaveArgs)) {
			Saved++;

			UE_LOG(LogBlueprintImport, Display, TEXT("saved %s"), *FileName);
		} else {
			UE_LOG(LogBlueprintImport, Warning, TEXT("could not save %s"), *FileName);
		}
	}

	UE_LOG(LogBlueprintImport, Display, TEXT("%d package(s) saved"), Saved);

	/* What the compile made of it, written out instruction by instruction.
	 *
	 * The compiler prints this itself, but only where somebody is sat in front of it: the check it
	 * does before printing is that this is not a commandlet, which is exactly where the comparison
	 * against the shipped script is made. So it is asked for again here.
	 *
	 * Cooking would be the other way to get at it, and a cook save wants a package writer that
	 * nothing here has, so this is the one that works. */
	if (FParse::Param(*Params, TEXT("disasm"))) {
		UPackage* Package = Importer->GetPackage();

		TArray<UObject*> Inside;

		if (Package != nullptr) {
			GetObjectsWithOuter(Package, Inside, false);
		}

		for (UObject* Object : Inside) {
			UBlueprintGeneratedClass* Compiled = Cast<UBlueprintGeneratedClass>(Object);

			if (Compiled == nullptr) continue;

			for (TFieldIterator<UFunction> It(Compiled, EFieldIteratorFlags::ExcludeSuper); It; ++It) {
				if (It->Script.Num() == 0) continue;

				/* Caught rather than logged as it goes: the disassembler writes a line at a time,
				 * and a log the run is already filling is no place to read one of these out of */
				FStringOutputDevice Written;

				FKismetBytecodeDisassembler Disassembler(Written);
				Disassembler.DisassembleStructure(*It);

				UE_LOG(LogBlueprintImport, Display, TEXT("[disasm %s] %d byte(s)") LINE_TERMINATOR TEXT("%s"), *It->GetName(), It->Script.Num(), *Written);
			}
		}
	}

	return Saved > 0 ? 0 : 1;
}
