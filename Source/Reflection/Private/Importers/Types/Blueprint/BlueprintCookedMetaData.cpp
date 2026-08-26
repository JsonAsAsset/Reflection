/* Copyright Reflection Contributors 2024-2026 */

#include "Importers/Types/Blueprint/BlueprintCookedMetaData.h"

#include "Containers/ExportContainer.h"
#include "Importers/Types/Blueprint/BlueprintGraphs.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_Event.h"
#include "K2Node_FunctionEntry.h"
#include "Kismet2/BlueprintEditorUtils.h"

namespace {
	/* The pairs under a metadata block, which are always written the same way: a list of things
	 * with a Key and a Value */
	TMap<FString, FString> Pairs(const FUObjectJsonValueExport& Holding, const TCHAR* Named) {
		TMap<FString, FString> Read;

		if (!Holding.Has(Named)) return Read;

		for (const FUObjectJsonValueExport& Entry : Holding.GetArray(Named)) {
			if (!Entry.Has(TEXT("Key"))) continue;

			Read.Add(Entry.GetString(TEXT("Key")), Entry.Has(TEXT("Value")) ? Entry.GetString(TEXT("Value")) : FString());
		}

		return Read;
	}


	/* The event a name belongs to, where it belongs to one rather than to a graph */
	UK2Node_Event* EventOf(const UBlueprint* Blueprint, const FString& Named) {
		for (UEdGraph* Page : Blueprint->UbergraphPages) {
			if (Page == nullptr) continue;

			for (UEdGraphNode* Node : Page->Nodes) {
				UK2Node_Event* Event = Cast<UK2Node_Event>(Node);

				if (Event == nullptr) continue;

				const FName Answers = Event->IsA<UK2Node_CustomEvent>()
					? static_cast<UK2Node_CustomEvent*>(Event)->CustomFunctionName
					: Event->EventReference.GetMemberName();

				if (Answers == FName(*Named)) return Event;
			}
		}

		return nullptr;
	}

}


namespace {
	void ApplyToEvent(UBlueprint* Blueprint, const FString& Named, const FUObjectJsonValueExport& Value, int32& Said) {
		UK2Node_Event* Event = EventOf(Blueprint, Named);

		if (Event == nullptr) return;

		const TMap<FString, FString> Told = Value.Has(TEXT("ObjectMetaData"))
			? Pairs(Value.GetObject(TEXT("ObjectMetaData")), TEXT("ObjectMetaData"))
			: TMap<FString, FString>();

		for (const TPair<FString, FString>& One : Told) {
			if (One.Value.IsEmpty()) continue;

			if (UK2Node_CustomEvent* Custom = Cast<UK2Node_CustomEvent>(Event)) {
				Custom->GetUserDefinedMetaData().SetMetaData(FName(*One.Key), One.Value);

				if (One.Key == FBlueprintMetadata::MD_CallInEditor.ToString()) {
					Custom->bCallInEditor = One.Value == TEXT("true");
				}
			}

			Said++;
		}
	}
}

TMap<FString, FString> FBlueprintCookedMetaData::WorldContexts(FUObjectExportContainer* Container) {
	TMap<FString, FString> Said;

	if (Container == nullptr) return Said;

	const FUObjectExport* Kept = Container->FindByType(FString(TEXT("ClassCookedMetaData")));

	if (Kept == nullptr || !Kept->IsJsonValid()) return Said;

	const FUObjectJsonValueExport Metadata(Kept->GetProperties());

	if (!Metadata.Has(TEXT("FunctionsMetaData"))) return Said;

	for (const FUObjectJsonValueExport& Entry : Metadata.GetArray(TEXT("FunctionsMetaData"))) {
		if (!Entry.Has(TEXT("Key")) || !Entry.Has(TEXT("Value"))) continue;

		const FUObjectJsonValueExport Value = Entry.GetObject(TEXT("Value"));

		if (!Value.Has(TEXT("ObjectMetaData"))) continue;

		for (const TPair<FString, FString>& One : Pairs(Value.GetObject(TEXT("ObjectMetaData")), TEXT("ObjectMetaData"))) {
			if (One.Key != FBlueprintMetadata::MD_WorldContext.ToString() || One.Value.IsEmpty()) continue;

			Said.Add(Entry.GetString(TEXT("Key")), One.Value);
		}
	}

	return Said;
}

int32 FBlueprintCookedMetaData::Apply(UBlueprint* Blueprint, FUObjectExportContainer* Container) {
	if (Blueprint == nullptr || Container == nullptr) return 0;

	const FUObjectExport* Kept = Container->FindByType(FString(TEXT("ClassCookedMetaData")));

	if (Kept == nullptr || !Kept->IsJsonValid()) return 0;

	const FUObjectJsonValueExport Metadata(Kept->GetProperties());

	int32 Said = 0;

	/* What each variable was shown as: the name above it, the section it sits under, whether its
	 * box is one line or several. None of it changes what runs, and all of it is the difference
	 * between the asset somebody wrote and a list of variables in Default. */
	if (Metadata.Has(TEXT("ClassMetaData"))) {
		const FUObjectJsonValueExport Class = Metadata.GetObject(TEXT("ClassMetaData"));

		if (Class.Has(TEXT("PropertiesMetaData"))) {
			for (const FUObjectJsonValueExport& Entry : Class.GetArray(TEXT("PropertiesMetaData"))) {
				if (!Entry.Has(TEXT("Key")) || !Entry.Has(TEXT("Value"))) continue;

				const FName Variable(*Entry.GetString(TEXT("Key")));

				/* Only what the blueprint declared itself. A component the construction script
				 * gives it is not a variable anybody can be told about. */
				if (FBlueprintEditorUtils::FindNewVariableIndex(Blueprint, Variable) == INDEX_NONE) continue;

				for (const TPair<FString, FString>& Told : Pairs(Entry.GetObject(TEXT("Value")), TEXT("FieldMetaData"))) {
					FBlueprintEditorUtils::SetBlueprintVariableMetaData(Blueprint, Variable, nullptr, FName(*Told.Key), Told.Value);

					Said++;
				}
			}
		}
	}

	if (!Metadata.Has(TEXT("FunctionsMetaData"))) return Said;

	/* And what each function was shown as. This is kept on the node the graph is entered through,
	 * which is where the editor reads it back from. */
	TArray<UEdGraph*> Graphs;
	Blueprint->GetAllGraphs(Graphs);

	for (const FUObjectJsonValueExport& Entry : Metadata.GetArray(TEXT("FunctionsMetaData"))) {
		if (!Entry.Has(TEXT("Key")) || !Entry.Has(TEXT("Value"))) continue;

		const FString Named = Entry.GetString(TEXT("Key"));

		UEdGraph* Graph = nullptr;

		for (UEdGraph* Candidate : Graphs) {
			if (Candidate != nullptr && Candidate->GetName() == Named) {
				Graph = Candidate;

				break;
			}
		}

		const FUObjectJsonValueExport Value = Entry.GetObject(TEXT("Value"));

		/* Not everything the class carries was written as a function. An event is a node in the
		 * event graph rather than a graph of its own, and it keeps what it is shown as on itself:
		 * whether it can be run from the details panel is the node's own answer, and the compiler
		 * copies it onto the function it builds. */
		if (Graph == nullptr) {
			ApplyToEvent(Blueprint, Named, Value, Said);

			continue;
		}

		UK2Node_FunctionEntry* Entered = FBlueprintGraphs::EntryOf(Graph);

		if (Entered == nullptr) continue;
		const TMap<FString, FString> Told = Value.Has(TEXT("ObjectMetaData"))
			? Pairs(Value.GetObject(TEXT("ObjectMetaData")), TEXT("ObjectMetaData"))
			: TMap<FString, FString>();

		for (const TPair<FString, FString>& One : Told) {
			/* A parameter is written here as a key with nothing against it. There is nothing to
			 * say about it, and it is not something the function itself is shown as. */
			if (One.Value.IsEmpty()) continue;

			/* Kept whole, whatever it is.
			 *
			 * A function carries its metadata as pairs, and the compiler puts every one of them
			 * back onto the function it builds. Picking out the ones known here would drop the
			 * rest on the floor, and what a blueprint can be told about is the engine's to decide
			 * rather than this. */
			Entered->MetaData.SetMetaData(FName(*One.Key), One.Value);

			Said++;
		}

		/* And said again where the editor keeps its own copy.
		 *
		 * A few of these are held twice: once among the pairs, and once as a field of their own
		 * that the editor reads to draw the panel. Written only to the pairs, the function compiles
		 * with everything it had but comes up in the wrong place with its button missing. The keys
		 * are the engine's own rather than spelled out here. */
		const auto Given = [&Told](const FName Key, FString& Out) {
			if (const FString* Against = Told.Find(Key.ToString())) {
				Out = *Against;

				return true;
			}

			return false;
		};

		FString Held;

		if (Given(FBlueprintMetadata::MD_FunctionCategory, Held)) Entered->MetaData.Category = FText::FromString(Held);
		if (Given(FBlueprintMetadata::MD_Tooltip, Held)) Entered->MetaData.ToolTip = FText::FromString(Held);
		if (Given(FBlueprintMetadata::MD_FunctionKeywords, Held)) Entered->MetaData.Keywords = FText::FromString(Held);
		if (Given(FBlueprintMetadata::MD_CompactNodeTitle, Held)) Entered->MetaData.CompactNodeTitle = FText::FromString(Held);
		if (Given(FBlueprintMetadata::MD_DeprecationMessage, Held)) Entered->MetaData.DeprecationMessage = Held;
		if (Given(FBlueprintMetadata::MD_CallInEditor, Held)) Entered->MetaData.bCallInEditor = Held == TEXT("true");
		if (Given(FBlueprintMetadata::MD_ThreadSafe, Held)) Entered->MetaData.bThreadSafe = Held == TEXT("true");
	}

	return Said;
}
