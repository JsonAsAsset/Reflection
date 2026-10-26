/* Copyright Reflection Contributors 2024-2026 */

#include "Importers/Types/Blueprint/GraphTidy.h"

#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "K2Node_CallFunction.h"
#include "K2Node_EnumEquality.h"
#include "K2Node_EnumInequality.h"

DECLARE_LOG_CATEGORY_CLASS(LogReflectionTidyByteEnum, All, All);

namespace {
	/* The enum wired into one of a node's inputs, which is the side that says what these bytes are */
	UEnum* EnumWiredInto(const UK2Node* Node) {
		if (Node == nullptr) return nullptr;

		for (const UEdGraphPin* Pin : Node->Pins) {
			if (Pin == nullptr || Pin->Direction != EGPD_Input) continue;
			if (Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Byte) continue;

			for (const UEdGraphPin* Linked : Pin->LinkedTo) {
				if (Linked == nullptr) continue;

				if (UEnum* Held = Cast<UEnum>(Linked->PinType.PinSubCategoryObject.Get())) return Held;
			}
		}

		return nullptr;
	}

	/* Whether a node of this kind is the one the call was written as.
	 *
	 * Each of these says which function it compiles down to, so it is asked rather than matched by
	 * name: whatever the engine turns one into is what a call of that name came from. */
	bool CompilesTo(const UClass* Kind, const FName Called, const UClass* Upon) {
		const UK2Node_EnumEquality* Asking = Kind != nullptr ? Cast<UK2Node_EnumEquality>(Kind->GetDefaultObject()) : nullptr;

		if (Asking == nullptr) return false;

		FName Named;
		UClass* Owner = nullptr;

		Asking->GetConditionalFunction(Named, &Owner);

		return Named == Called && (Upon == nullptr || Owner == nullptr || Owner == Upon);
	}

	/* Everything reaching one pin, reaching another instead.
	 *
	 * Named for this file rather than for what it does. Every tidying is built into one file with
	 * every other, so a helper kept to itself here is kept beside theirs, and two of a name is two
	 * of a name however private each one was meant to be. */
	void RewireByteEnum(UEdGraphPin* From, UEdGraphPin* To) {
		if (From == nullptr || To == nullptr) return;

		for (UEdGraphPin* Held : From->LinkedTo) {
			if (Held != nullptr) To->MakeLinkTo(Held);
		}
	}
}

/* Two of an enum compared, which the script keeps as two bytes.
 *
 * A byte is a byte to the machine. The enum lives on the pin, and the call the compiler wrote takes
 * two plain bytes, so it comes back with the enum on the side something is wired into and a bare
 * number on the side nobody wired. The value shows as a number with no name on it, and picking
 * another one means knowing what the numbers stand for.
 *
 * The name cannot be written onto the call. A call grows its pins from the function it calls, and a
 * function taking a byte says byte: anything put on the pin that the signature does not agree with
 * is taken back the next time the node is built, which is every time the asset is loaded. The name
 * left on it afterwards is a name where a number belongs.
 *
 * The editor does not use a call for this at all. It has a node of its own whose inputs take
 * whatever they are wired to and whose enum is therefore its own to keep, and that node compiles
 * back down to the very call that was read. So the call is laid down as the node it was written as,
 * and it survives being built again because nothing about it disagrees with anything. */
struct FByteEnumTidy final : FGraphTidy {
	virtual const TCHAR* GetName() const override { return TEXT("ByteEnum"); }

	virtual int32 Apply(UEdGraph* Graph) const override {
		if (Graph == nullptr) return 0;

		int32 Named = 0;

		TArray<UEdGraphNode*> Nodes = Graph->Nodes;

		for (UEdGraphNode* Node : Nodes) {
			UK2Node_CallFunction* Calling = Cast<UK2Node_CallFunction>(Node);

			if (Calling == nullptr || !Calling->IsNodePure()) continue;

			UEnum* Enum = EnumWiredInto(Calling);

			if (Enum == nullptr) continue;

			const FName Called = Calling->FunctionReference.GetMemberName();
			const UClass* Upon = Calling->FunctionReference.GetMemberParentClass();

			UClass* Kind = nullptr;

			if (CompilesTo(UK2Node_EnumEquality::StaticClass(), Called, Upon)) Kind = UK2Node_EnumEquality::StaticClass();
			else if (CompilesTo(UK2Node_EnumInequality::StaticClass(), Called, Upon)) Kind = UK2Node_EnumInequality::StaticClass();

			if (Kind == nullptr) continue;

			/* The two it compares and what it answers, by the names the call gives them */
			UEdGraphPin* First = Calling->FindPin(TEXT("A"), EGPD_Input);
			UEdGraphPin* Second = Calling->FindPin(TEXT("B"), EGPD_Input);
			UEdGraphPin* Answers = Calling->GetReturnValuePin();

			if (First == nullptr || Second == nullptr || Answers == nullptr) continue;

			UK2Node_EnumEquality* Made = NewObject<UK2Node_EnumEquality>(Graph, Kind, NAME_None, RF_Transactional);

			Graph->AddNode(Made, false, false);

			Made->CreateNewGuid();
			Made->PostPlacedNewNode();
			Made->AllocateDefaultPins();

			Made->NodePosX = Calling->NodePosX;
			Made->NodePosY = Calling->NodePosY;

			UEdGraphPin* Takes = Made->GetInput1Pin();
			UEdGraphPin* Against = Made->GetInput2Pin();
			UEdGraphPin* Gives = Made->GetReturnValuePin();

			if (Takes == nullptr || Against == nullptr || Gives == nullptr) {
				Graph->RemoveNode(Made);

				continue;
			}

			/* Wired first, since the two it compares start as neither kind and take whatever they
			 * are wired to. Said before that, the name would be a name on a pin of no kind. */
			RewireByteEnum(First, Takes);
			RewireByteEnum(Second, Against);
			RewireByteEnum(Answers, Gives);

			Made->NotifyPinConnectionListChanged(Takes);
			Made->NotifyPinConnectionListChanged(Against);

			/* And the one nobody wired says what it was picked as */
			for (const TPair<UEdGraphPin*, UEdGraphPin*>& Pair : { TPair<UEdGraphPin*, UEdGraphPin*>(First, Takes), TPair<UEdGraphPin*, UEdGraphPin*>(Second, Against) }) {
				if (Pair.Key->LinkedTo.Num() > 0 || Pair.Key->DefaultValue.IsEmpty() || !Pair.Key->DefaultValue.IsNumeric()) continue;

				const FString Says = Enum->GetNameStringByValue(FCString::Atoi64(*Pair.Key->DefaultValue));

				/* A number the enum has no entry for is not one of its values */
				if (Says.IsEmpty()) continue;

				Pair.Value->DefaultValue = Says;
			}

			First->BreakAllPinLinks();
			Second->BreakAllPinLinks();
			Answers->BreakAllPinLinks();

			Graph->RemoveNode(Calling);

			Named++;
		}

		if (Named > 0) {
			UE_LOG(LogReflectionTidyByteEnum, Display, TEXT("\"%s\" had %d enum(s) compared as two bytes where the editor draws a node of its own"), *Graph->GetName(), Named);
		}

		return Named;
	}
};

REGISTER_TIDY(FByteEnumTidy)
