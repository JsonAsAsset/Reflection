/* Copyright Reflection Contributors 2024-2026 */

#include "BlueprintImportCommandlet.h"

#include "FileHelpers.h"
#include "Misc/FileHelper.h"
#include "Serialization/JsonSerializer.h"
#include "JsonObjectConverter.h"
#include "Animation/AnimClassInterface.h"
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
#include "Engine/Blueprint.h"
#include "Importers/Types/Blueprint/MacroPattern.h"
#include "Importers/Constructor/Asset.h"
#include "Importers/Constructor/ImportReader.h"
#include "Importers/Constructor/Importer.h"
#include "Modules/Cloud/Cloud.h"
#include "Settings/Runtime.h"
/* 5.0 gathered what a save is told into a header of its own */
#if ENGINE_UE5
#include "UObject/SavePackage.h"
#endif
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

	/* Only the one blueprint, with what it names left to whatever the project already has.
	 *
	 * A function library names every ability its functions check against, and fetched along with
	 * it those come across as blueprints of their own, each with its own references, whether or
	 * not anything here could compile them. */
	if (FParse::Param(*Params, TEXT("norefs"))) {
		FAssetUtilities::bImportReferences = false;

		UE_LOG(LogBlueprintImport, Display, TEXT("not reflecting referenced assets"));
	}

	/* The class without what it does: its parent, its variables and what its default object holds.
	 *
	 * Enough for a blueprint that only reads another's defaults to compile against it, when the
	 * graphs of that other one are a system of their own that is not being brought across. */
	if (FParse::Param(*Params, TEXT("nocode"))) {
		GetSettings()->AssetSettings.ImportBlueprintCode = false;

		UE_LOG(LogBlueprintImport, Display, TEXT("leaving the graphs empty"));
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

	/* Built under the path given instead, so the copy at the real location is neither read back nor
	 * written over. Spelled the way the cloud path is, since the class is named after it. */
	FString Into = Path;

	if (FString Elsewhere; FParse::Value(*Params, TEXT("into="), Elsewhere) && !Elsewhere.IsEmpty()) {
		Into = Elsewhere;

		UE_LOG(LogBlueprintImport, Display, TEXT("made at \"%s\" rather than where the cloud has it"), *Into);
	}

	/* Notifications are for somebody watching, and the path is already the one the cloud spells */
	const bool bImported = IImportReader::ReadExportsAndImport(Exports, Into, Importer, true, false);

	if (!bImported || Importer == nullptr) {
		UE_LOG(LogBlueprintImport, Error, TEXT("nothing was made from \"%s\""), *Path);

		return 1;
	}

	/* Saved, because the asset on disk is what the comparison reads */
	TArray<UPackage*> Dirty;
	FEditorFileUtils::GetDirtyContentPackages(Dirty);
	FEditorFileUtils::GetDirtyWorldPackages(Dirty);

	int32 Saved = 0;

	const UPackage* Imported = Importer->GetPackage();

	for (UPackage* Package : Dirty) {
		const FString FileName = FPackageName::LongPackageNameToFilename(Package->GetName(), FPackageName::GetAssetPackageExtension());

		/* Loading dirties packages nobody asked to change, so only what this run made is written */
		if (Package != Imported && FPaths::FileExists(FileName)) {
			UE_LOG(LogBlueprintImport, Display, TEXT("left alone %s, it was already on disk"), *FileName);

			continue;
		}

		/* 5.0 gathered what a save is told into one struct. Before that it is told the same
		 * things one argument at a time. */
#if ENGINE_UE5
		/* A package saved empty once keeps PKG_ContainsNoAsset through every later save, which only ever
		 * adds it; the save sets it again if this one really holds no asset. */
		Package->ClearPackageFlags(PKG_ContainsNoAsset);

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

	/* The class the compile made, written out the way the cloud writes the shipped one.
	 *
	 * An animation blueprint does most of its work without any script at all: what feeds each node
	 * is a list of copies on the class, its state machines are baked onto it, and the values
	 * somebody typed into a pin are folded into its sparse data. None of that is in the graphs or
	 * the disassembly, and the graphs leave out every pin that is hidden, so neither says whether a
	 * copy the shipped class makes is made here too. Written as the same kind of json, the two can
	 * be read side by side. */
	if (FString ClassFile; FParse::Value(*Params, TEXT("dumpclass="), ClassFile) && !ClassFile.IsEmpty()) {
		UPackage* Package = Importer->GetPackage();

		TArray<UObject*> Inside;

		if (Package != nullptr) {
			GetObjectsWithOuter(Package, Inside, false);
		}

		const TSharedRef<FJsonObject> Written = MakeShared<FJsonObject>();

		for (UObject* Object : Inside) {
			UBlueprintGeneratedClass* Compiled = Cast<UBlueprintGeneratedClass>(Object);

			if (Compiled == nullptr) continue;

			/* The skeleton class sits in the same package, and is the outline the editor checks
			 * against: no nodes, no copies and nothing folded, which is none of what is wanted */
			if (const UBlueprint* Made = Cast<UBlueprint>(Compiled->ClassGeneratedBy); Made != nullptr && Made->SkeletonGeneratedClass == Compiled) continue;

			/* What the class itself holds, which is the baked machines, the node data and the rest */
			if (const TSharedRef<FJsonObject> Holds = MakeShared<FJsonObject>(); FJsonObjectConverter::UStructToJsonObject(Compiled->GetClass(), Compiled, Holds, 0, 0)) {
				Written->SetObjectField(TEXT("Properties"), Holds);
			}

			/* Which node each node index is, and what kind of node it is.
			 *
			 * The node data and the baked machines both say a node by its place in this list rather
			 * than by name, and the shipped class names its nodes differently, so without the list
			 * neither can be read as anything. */
			if (const IAnimClassInterface* Animated = IAnimClassInterface::GetFromClass(Compiled)) {
				TArray<TSharedPtr<FJsonValue>> Nodes;

				const TSharedRef<FJsonObject> Kinds = MakeShared<FJsonObject>();

				for (const FStructProperty* Node : Animated->GetAnimNodeProperties()) {
					Nodes.Add(MakeShared<FJsonValueString>(Node->GetName()));
					Kinds->SetStringField(Node->GetName(), Node->Struct->GetName());
				}

				Written->SetArrayField(TEXT("Nodes"), Nodes);
				Written->SetObjectField(TEXT("Kinds"), Kinds);

				/* And what each node holds on the default object, which is where a value nothing feeds
				 * and nothing folded is kept: a rule that is a ticked box, for one */
				if (const UObject* Defaults = Compiled->GetDefaultObject(false)) {
					const TSharedRef<FJsonObject> Held = MakeShared<FJsonObject>();

					for (const FStructProperty* Node : Animated->GetAnimNodeProperties()) {
						if (const TSharedRef<FJsonObject> Says = MakeShared<FJsonObject>(); FJsonObjectConverter::UStructToJsonObject(Node->Struct, Node->ContainerPtrToValuePtr<void>(Defaults), Says, 0, 0)) {
							Held->SetObjectField(Node->GetName(), Says);
						}
					}

					Written->SetObjectField(TEXT("Defaults"), Held);
				}
			}

			/* And the slots of what changes at run time, in the order a node's entry counts them */
			if (const FStructProperty* Mutables = FindFProperty<FStructProperty>(Compiled, TEXT("__AnimBlueprintMutables"))) {
				const TSharedRef<FJsonObject> Slots = MakeShared<FJsonObject>();

				for (TFieldIterator<FProperty> It(Mutables->Struct); It; ++It) {
					Slots->SetStringField(It->GetName(), It->GetCPPType());
				}

				Written->SetObjectField(TEXT("Mutables"), Slots);
			}

			/* And what it keeps beside itself, which is where the copies and the folded values are */
			if (const UScriptStruct* Sparse = Compiled->GetSparseClassDataStruct()) {
				if (const void* Data = Compiled->GetSparseClassData(EGetSparseClassDataMethod::ReturnIfNull)) {
					if (const TSharedRef<FJsonObject> Holds = MakeShared<FJsonObject>(); FJsonObjectConverter::UStructToJsonObject(Sparse, Data, Holds, 0, 0)) {
						Written->SetObjectField(TEXT("SerializedSparseClassData"), Holds);
					}
				}
			}

			break;
		}

		FString Said;

		const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Said);

		if (FJsonSerializer::Serialize(Written, Writer) && FFileHelper::SaveStringToFile(Said, *ClassFile)) {
			UE_LOG(LogBlueprintImport, Display, TEXT("the class as compiled was written to \"%s\""), *ClassFile);
		} else {
			UE_LOG(LogBlueprintImport, Error, TEXT("could not write the class to \"%s\""), *ClassFile);
		}
	}

	/* Every graph as it was saved, since what the importer says is said before the jumps are tied up */
	if (FString GraphsFile; FParse::Value(*Params, TEXT("graphs="), GraphsFile) && !GraphsFile.IsEmpty()) {
		FString Written;

		UPackage* Package = Importer->GetPackage();
		UBlueprint* Blueprint = Package != nullptr ? FindObject<UBlueprint>(Package, *FPackageName::GetShortName(Package)) : nullptr;

		if (Blueprint != nullptr) {
			TArray<UEdGraph*> Graphs;
			Blueprint->GetAllGraphs(Graphs);

			for (const UEdGraph* Graph : Graphs) {
				if (Graph == nullptr) continue;

				Written += FString::Printf(TEXT("== %s (%d node(s))") LINE_TERMINATOR, *Graph->GetName(), Graph->Nodes.Num());

				for (const UEdGraphNode* Node : Graph->Nodes) {
					if (Node == nullptr) continue;

					Written += FString::Printf(TEXT("  %s [%s]") LINE_TERMINATOR, *Node->GetName(), *Node->GetNodeTitle(ENodeTitleType::ListView).ToString().Replace(TEXT("\n"), TEXT(" ")).Replace(TEXT("\r"), TEXT("")));

					for (const UEdGraphPin* Pin : Node->Pins) {
						if (Pin == nullptr || Pin->bHidden) continue;

						FString Leads;

						for (const UEdGraphPin* To : Pin->LinkedTo) {
							if (To != nullptr) Leads += FString::Printf(TEXT("%s.%s "), *To->GetOwningNode()->GetName(), *To->PinName.ToString());
						}

						Written += FString::Printf(TEXT("      %s %s (%s)%s -> %s") LINE_TERMINATOR,
							Pin->Direction == EGPD_Input ? TEXT("in ") : TEXT("out"),
							*Pin->PinName.ToString(),
							*Pin->PinType.PinCategory.ToString(),
							Pin->DefaultValue.IsEmpty() && Pin->DefaultObject == nullptr ? TEXT("") : *FString::Printf(TEXT(" = %s"), Pin->DefaultObject != nullptr ? *Pin->DefaultObject->GetName() : *Pin->DefaultValue),
							Leads.IsEmpty() ? TEXT("-") : *Leads);
					}
				}
			}
		}

		if (FFileHelper::SaveStringToFile(Written, *GraphsFile)) {
			UE_LOG(LogBlueprintImport, Display, TEXT("the graphs as saved were written to \"%s\""), *GraphsFile);
		} else {
			UE_LOG(LogBlueprintImport, Error, TEXT("could not write the graphs to \"%s\""), *GraphsFile);
		}
	}

	return Saved > 0 ? 0 : 1;
}
