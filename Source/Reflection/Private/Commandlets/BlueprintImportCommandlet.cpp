/* Copyright Reflection Contributors 2024-2026 */

#include "BlueprintImportCommandlet.h"

#include "FileHelpers.h"
#include "Misc/FileHelper.h"
#include "Serialization/JsonSerializer.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "ScriptDisassembler.h"
/* Given a header of its own after 5.6, and part of the string header before that */
#if __has_include("Misc/StringOutputDevice.h")
#include "Misc/StringOutputDevice.h"
#else
#include "Containers/UnrealString.h"
#endif
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "Importers/Types/Blueprint/MacroPattern.h"
#include "Importers/Constructor/Asset.h"
#include "Importers/Constructor/ImportReader.h"
#include "Importers/Constructor/Importer.h"
#include "Modules/Cloud/Cloud.h"
#include "Settings/Runtime.h"
#include "UObject/SavePackage.h"
#include "Interfaces/ITargetPlatformManagerModule.h"
#include "Interfaces/ITargetPlatform.h"
/* 5.1 split what a save is told about the cook it belongs to out into these. Nothing here asks
 * for any of it, and before 5.1 there is nothing to ask. */
#if UE5_1_BEYOND
#include "Serialization/ArchiveCookData.h"
#include "UObject/ArchiveCookContext.h"
#endif

DECLARE_LOG_CATEGORY_CLASS(LogBlueprintImport, All, All);

int32 UBlueprintImportCommandlet::Main(const FString& Params) {
	UE_LOG(LogBlueprintImport, Display, TEXT("asked for: %s"), *Params);

	/* What a macro means, read out of the engine's own statement of it.
	 *
	 * A macro is copied into whatever used it before anything is compiled, so what comes back is
	 * the copy rather than the macro. Putting one back means knowing its shape, and the shape is
	 * kept in StandardMacros rather than anywhere it can be guessed from. */
	if (FString Macro; FParse::Value(*Params, TEXT("macro="), Macro) && !Macro.IsEmpty()) {
		UEdGraph* Graph = MacroReading::StandardMacro(*Macro);

		if (Graph == nullptr) {
			UE_LOG(LogBlueprintImport, Error, TEXT("no macro called \"%s\" in StandardMacros"), *Macro);

			return 1;
		}

		UE_LOG(LogBlueprintImport, Display, TEXT("macro \"%s\", %d node(s)"), *Macro, Graph->Nodes.Num());

		for (const UEdGraphNode* Node : Graph->Nodes) {
			if (Node == nullptr) continue;

			UE_LOG(LogBlueprintImport, Display, TEXT("  %s [%s]"), *Node->GetName(), *Node->GetNodeTitle(ENodeTitleType::ListView).ToString());

			for (const UEdGraphPin* Pin : Node->Pins) {
				if (Pin == nullptr) continue;

				FString Leads;

				for (const UEdGraphPin* To : Pin->LinkedTo) {
					if (To != nullptr) Leads += FString::Printf(TEXT("%s.%s "), *To->GetOwningNode()->GetName(), *To->PinName.ToString());
				}

				UE_LOG(LogBlueprintImport, Display, TEXT("      %s %s (%s) -> %s"),
					Pin->Direction == EGPD_Input ? TEXT("in ") : TEXT("out"),
					*Pin->PinName.ToString(),
					*Pin->PinType.PinCategory.ToString(),
					Leads.IsEmpty() ? TEXT("-") : *Leads);
			}
		}

		return 0;
	}

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

	/* What was fetched, written out where it was asked for.
	 *
	 * What comes back from the cloud is what the import is made from, and it is not always what is
	 * sitting in somebody's export folder: the two can be different builds of the same asset. What
	 * came back is the only thing worth checking the result against, so it can be kept. */
	if (FString Dump; FParse::Value(*Params, TEXT("json="), Dump) && !Dump.IsEmpty()) {
		FString Written;

		const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Written);

		if (FJsonSerializer::Serialize(Response.ToSharedRef(), Writer) && FFileHelper::SaveStringToFile(Written, *Dump)) {
			UE_LOG(LogBlueprintImport, Display, TEXT("what the cloud gave back was kept at \"%s\""), *Dump);
		} else {
			UE_LOG(LogBlueprintImport, Error, TEXT("could not keep what the cloud gave back at \"%s\""), *Dump);
		}
	}

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

		/* 5.0 gathered what a save is told into one struct. Before that it is told the same
		 * things one argument at a time. */
#if ENGINE_UE5
		FSavePackageArgs SaveArgs;
		SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;

		if (UPackage::SavePackage(Package, nullptr, *FileName, SaveArgs)) {
#else
		if (UPackage::SavePackage(Package, nullptr, RF_Public | RF_Standalone, *FileName)) {
#endif
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
