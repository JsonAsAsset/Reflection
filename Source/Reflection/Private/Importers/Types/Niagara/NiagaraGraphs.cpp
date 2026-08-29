/* Copyright Reflection Contributors 2024-2026 */

#include "Importers/Types/Niagara/NiagaraGraphs.h"

#include "Engine/EngineUtilities.h"
#include "Serializers/PropertySerializer.h"

#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"

#include "NiagaraGraph.h"
#include "NiagaraScript.h"
#include "NiagaraNodeOutput.h"
#include "NiagaraNodeFunctionCall.h"
#include "NiagaraScriptSource.h"

namespace {
	/* The object a reference names, by the whole path under the package rather than its leaf.
	 *
	 * A script carries a graph per version of itself and they are copies, so the same node name
	 * appears once per graph. Told apart by the leaf they are the same node, and a link then lands
	 * on whichever graph was read last, which is a wire between two graphs. */
	FString ReferencedPath(const TSharedPtr<FJsonObject>& Reference) {
		FString Named;

		if (!Reference.IsValid() || !Reference->TryGetStringField(TEXT("ObjectName"), Named)) {
			return FString();
		}

		return StripObjectOuter(Named);
	}

	/* What a pin says of itself, given either as the text or as the pieces it was made from */
	FText TextOf(const TSharedPtr<FJsonValue>& Value) {
		if (!Value.IsValid()) return FText::GetEmpty();

		if (Value->Type == EJson::String) return FText::FromString(Value->AsString());

		const TSharedPtr<FJsonObject> Object = Value->AsObject();

		if (!Object.IsValid()) return FText::GetEmpty();

		FString Said;

		if (Object->TryGetStringField(TEXT("CultureInvariantString"), Said)
			|| Object->TryGetStringField(TEXT("SourceString"), Said)) {
			return FText::FromString(Said);
		}

		return FText::GetEmpty();
	}

	/* The pins an export lists, or nothing where it lists none */
	const TArray<TSharedPtr<FJsonValue>>* PinsOf(const FUObjectExport* Export) {
		if (Export == nullptr || !Export->IsJsonValid()) return nullptr;

		const TArray<TSharedPtr<FJsonValue>>* Listed = nullptr;

		return Export->JsonObject->TryGetArrayField(TEXT("Pins"), Listed) ? Listed : nullptr;
	}

#if WITH_EDITORONLY_DATA
	/* The graph a function call node names.
	 *
	 * UE4 declares the node MinimalAPI, so the call that answers this is there to read and not
	 * there to link against. Walked by hand instead, the same two steps the engine takes: the
	 * script the node names, and the graph its source holds. */
	const UNiagaraGraph* CalledGraphOf(const UNiagaraNodeFunctionCall* Call) {
#if ENGINE_UE4
		if (Call->FunctionScript == nullptr) return nullptr;

		const UNiagaraScriptSource* Source = Cast<UNiagaraScriptSource>(Call->FunctionScript->GetSource());

		return Source != nullptr ? Source->NodeGraph : nullptr;
#else
		return Call->GetCalledGraph();
#endif
	}
#endif
}

void FNiagaraGraphs::Rebuild(FUObjectExportContainer* Container, UPackage* Package,
	UPropertySerializer* Serializer, FNiagaraGraphStats& OutStats) {
	if (Container == nullptr) return;

	/* What each graph holds, taken from the nodes sitting under it rather than from the list it
	 * carries.
	 *
	 * A script keeps a graph per version of itself and they are copies, so the same node name is
	 * in each. The list names them by a reference that has to be resolved back to one of them, and
	 * one resolved by name alone lands in whichever copy was read last: the graphs come out with
	 * the right number of nodes between them and the wrong ones in each. What is under a graph is
	 * not open to that, so the list is made again from that. */
	for (FUObjectExport* Export : Container->Exports) {
		UEdGraph* Graph = Cast<UEdGraph>(Export->Object);

		if (Graph == nullptr) continue;

		Graph->Nodes.Reset();

		for (FUObjectExport* Under : Container->Exports) {
			UEdGraphNode* Beneath = Cast<UEdGraphNode>(Under->Object);

			if (Beneath != nullptr && Beneath->GetOuter() == Graph) {
				Graph->Nodes.Add(Beneath);

				OutStats.Placed++;
			}
		}
	}

	/* Every pin the export lists, made on the node that owns it. Kept against the node and the id
	 * it was written with, because that pair is what a link names. */
	TMap<TPair<FString, FGuid>, UEdGraphPin*> Made;

	for (FUObjectExport* Export : Container->Exports) {
		UEdGraphNode* Node = Cast<UEdGraphNode>(Export->Object);

		if (Node == nullptr) continue;

		/* A node holding pins already is one this has been over before, and it is gone over again
		 * whenever something asks for the asset a second time.
		 *
		 * Every pin is made from the export afresh, so a node kept as it was ends up with each of
		 * its pins twice under the one id. A link names that id against the node that owns it, so
		 * on the way back in only one of the two answers to the name and everything wired to the
		 * other reaches nothing. Taking off what is there first leaves a second pass where the
		 * first one left off. */
		TArray<UEdGraphPin*> Standing = MoveTemp(Node->Pins);

		Node->Pins.Reset();

		for (UEdGraphPin* Pin : Standing) {
			if (Pin == nullptr) continue;

			Pin->BreakAllPinLinks();

			/* Thrown away rather than removed. Removing one tells the node it lost a pin, which
			 * is a thing a person does to it and not a thing this is doing: a Niagara node hearing
			 * that goes and makes itself again from what it is a call to, and what it makes is
			 * built on whatever it can find under that name. Nothing is told anything here. */
			UEdGraphNode::DestroyPin(Pin);
		}

		const TArray<TSharedPtr<FJsonValue>>* Listed = PinsOf(Export);

		if (Listed == nullptr) continue;

		for (const TSharedPtr<FJsonValue>& Value : *Listed) {
			const TSharedPtr<FJsonObject> Entry = Value.IsValid() ? Value->AsObject() : nullptr;

			if (!Entry.IsValid()) continue;

			FString Named;
			FString Way;

			Entry->TryGetStringField(TEXT("PinName"), Named);
			Entry->TryGetStringField(TEXT("Direction"), Way);

			/* What the pin carries, read as the struct it is rather than picked apart by hand, so
			 * the type object it names is resolved the same way every other reference is */
			FEdGraphPinType Type;

			if (const TSharedPtr<FJsonObject>* Described = nullptr;
				Serializer != nullptr && Entry->TryGetObjectField(TEXT("PinType"), Described)) {
				Serializer->DeserializeStruct(FEdGraphPinType::StaticStruct(), Described->ToSharedRef(), &Type, nullptr);
			}

			UEdGraphPin* Pin = Node->CreatePin(Way == TEXT("EGPD_Output") ? EGPD_Output : EGPD_Input, Type, FName(*Named));

			if (Pin == nullptr) continue;

			/* Kept as it was written, since a link names the id and not the name */
			if (FString Id; Entry->TryGetStringField(TEXT("PinId"), Id)) {
				Pin->PinId = StringToGuid(Id);
			}

			/* The name a pin keeps for good, which is not the one a link uses.
			 *
			 * A node that pairs its pins up records the pairs by this name rather than by the
			 * other: a parameter map's get node says which of its inputs stands for the default
			 * of which of its outputs, and a Niagara if node its own pairs. Left unset the pairs
			 * name nothing, and the node comes out with the pins it should have and no idea what
			 * any of them are to each other. */
#if WITH_EDITORONLY_DATA
			if (FString Persistent; Entry->TryGetStringField(TEXT("PersistentGuid"), Persistent)) {
				Pin->PersistentGuid = StringToGuid(Persistent);
			}
#endif

			/* What the pin is on the node, beside what it carries. The engine packs these into
			 * one number in an order it keeps fixed on purpose, and reads them back out of the
			 * same places. A pin made without them shows where the original hid it and takes a
			 * connection where the original refused one. */
			if (int32 Flags = 0; Entry->TryGetNumberField(TEXT("BitField"), Flags)) {
				Pin->bHidden = (Flags & 1 << 0) != 0;
				Pin->bNotConnectable = (Flags & 1 << 1) != 0;
				Pin->bDefaultValueIsReadOnly = (Flags & 1 << 2) != 0;
				Pin->bDefaultValueIsIgnored = (Flags & 1 << 3) != 0;
				Pin->bAdvancedView = (Flags & 1 << 4) != 0;
			}

			if (const TSharedPtr<FJsonValue> Friendly = Entry->TryGetField(TEXT("PinFriendlyName")); Friendly.IsValid()) {
				Pin->PinFriendlyName = TextOf(Friendly);
			}

			Entry->TryGetStringField(TEXT("DefaultValue"), Pin->DefaultValue);
			Entry->TryGetStringField(TEXT("AutogeneratedDefaultValue"), Pin->AutogeneratedDefaultValue);
			Entry->TryGetStringField(TEXT("PinToolTip"), Pin->PinToolTip);

			Made.Add(TPair<FString, FGuid>(Node->GetPathName(Package), Pin->PinId), Pin);

			OutStats.Pins++;
		}
	}

	/* What a node made of its own pins.
	 *
	 * Some nodes pair their pins off: a parameter map's get node says which of its inputs stands
	 * for the default of which of its outputs, and it says so by the name a pin keeps for good
	 * rather than the one a link uses. Both halves have to be a pin that is there, or the node
	 * drops the pairing the first time it looks at itself and the pin it named goes back to
	 * standing for nothing.
	 *
	 * Which of them does this is not asked. A pairing is a map of one such name to another, so
	 * that is what is looked for, and a node that keeps none has none to check. */
#if WITH_EDITORONLY_DATA
	for (FUObjectExport* Export : Container->Exports) {
		const UEdGraphNode* Node = Cast<UEdGraphNode>(Export->Object);

		if (Node == nullptr) continue;

		TSet<FGuid> Kept;

		for (const UEdGraphPin* Pin : Node->Pins) {
			if (Pin != nullptr && Pin->PersistentGuid.IsValid()) Kept.Add(Pin->PersistentGuid);
		}

		for (TFieldIterator<FMapProperty> Named(Node->GetClass()); Named; ++Named) {
			const FStructProperty* Says = CastField<FStructProperty>(Named->KeyProp);
			const FStructProperty* Stands = CastField<FStructProperty>(Named->ValueProp);

			if (Says == nullptr || Stands == nullptr
				|| Says->Struct != TBaseStructure<FGuid>::Get()
				|| Stands->Struct != TBaseStructure<FGuid>::Get()) {
				continue;
			}

			FScriptMapHelper Held(*Named, Named->ContainerPtrToValuePtr<void>(Node));

			for (int32 Index = 0; Index < Held.GetMaxIndex(); Index++) {
				if (!Held.IsValidIndex(Index)) continue;

				const FGuid& One = *reinterpret_cast<const FGuid*>(Held.GetKeyPtr(Index));
				const FGuid& Other = *reinterpret_cast<const FGuid*>(Held.GetValuePtr(Index));

				if (!Kept.Contains(One) || !Kept.Contains(Other)) OutStats.Unpaired++;
			}
		}
	}
#endif

	/* Where a call goes.
	 *
	 * A function call node stands for a module, and walking the graph means walking into the one
	 * it names. The engine reaches for that graph's output and goes through it without asking
	 * whether it found one, so a call naming a script that has no module output to it takes the
	 * editor down the moment anything walks the graph, which opening the asset does.
	 *
	 * Said here with both names in it, because a call is only wrong by what it points at. */
#if WITH_EDITORONLY_DATA
	for (FUObjectExport* Export : Container->Exports) {
		const UNiagaraNodeFunctionCall* Call = Cast<UNiagaraNodeFunctionCall>(Export->Object);

		if (Call == nullptr) continue;

		const UNiagaraGraph* Called = CalledGraphOf(Call);

		/* A call naming nothing is one the engine leaves alone */
		if (Called == nullptr) continue;

		bool bWalkable = false;

		for (const UEdGraphNode* Node : Called->Nodes) {
			const UNiagaraNodeOutput* Output = Cast<UNiagaraNodeOutput>(Node);

			if (Output == nullptr) continue;

			if (Output->ScriptType == ENiagaraScriptUsage::Function
				|| Output->ScriptType == ENiagaraScriptUsage::Module
				|| Output->ScriptType == ENiagaraScriptUsage::DynamicInput) {
				bWalkable = true;

				break;
			}
		}

		if (bWalkable) continue;

		OutStats.Uncalled++;

		UE_LOG(LogReflection, Warning, TEXT("\"%s\" calls \"%s\", whose graph \"%s\" holds %d node(s) and no module output to walk into"),
			*Call->GetPathName(Package), *GetPathNameSafe(Call->FunctionScript),
			*GetPathNameSafe(Called), Called->Nodes.Num());
	}
#endif

	/* Joined up once every pin exists, since a link reaches across nodes and there is no order
	 * that has both ends of all of them made first */
	for (FUObjectExport* Export : Container->Exports) {
		const TArray<TSharedPtr<FJsonValue>>* Listed = PinsOf(Export);

		if (Cast<UEdGraphNode>(Export->Object) == nullptr || Listed == nullptr) continue;

		for (const TSharedPtr<FJsonValue>& Value : *Listed) {
			const TSharedPtr<FJsonObject> Entry = Value.IsValid() ? Value->AsObject() : nullptr;

			FString Id;

			if (!Entry.IsValid() || !Entry->TryGetStringField(TEXT("PinId"), Id)) continue;

			UEdGraphPin** From = Made.Find(TPair<FString, FGuid>(Export->Object->GetPathName(Package), StringToGuid(Id)));

			const TArray<TSharedPtr<FJsonValue>>* Joined = nullptr;

			if (From == nullptr || !Entry->TryGetArrayField(TEXT("LinkedTo"), Joined)) continue;

			for (const TSharedPtr<FJsonValue>& One : *Joined) {
				const TSharedPtr<FJsonObject> Link = One.IsValid() ? One->AsObject() : nullptr;

				if (!Link.IsValid()) continue;

				FString Other;

				const TSharedPtr<FJsonObject>* Owner = nullptr;

				if (!Link->TryGetStringField(TEXT("PinId"), Other) || !Link->TryGetObjectField(TEXT("OwningNode"), Owner)) {
					continue;
				}

				UEdGraphPin** To = Made.Find(TPair<FString, FGuid>(ReferencedPath(*Owner), StringToGuid(Other)));

				/* A link onto a pin that isn't here, which is a node the build hasn't got */
				if (To == nullptr) {
					OutStats.Dangling++;

					continue;
				}

				/* Both ends list each other, so the second time round the pair is already joined */
				if (!(*From)->LinkedTo.Contains(*To)) {
					(*From)->MakeLinkTo(*To);

					OutStats.Links++;
				}
			}
		}
	}
}
