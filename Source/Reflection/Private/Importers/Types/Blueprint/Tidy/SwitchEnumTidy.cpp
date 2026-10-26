/* Copyright Reflection Contributors 2024-2026 */

#include "Importers/Types/Blueprint/GraphTidy.h"

#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "K2Node_CallFunction.h"
#include "K2Node_EnumInequality.h"
#include "K2Node_IfThenElse.h"
#include "K2Node_SwitchEnum.h"
#include "K2Node_VariableGet.h"

DECLARE_LOG_CATEGORY_CLASS(LogReflectionTidySwitchEnum, All, All);

namespace {
	/* One rung of the ladder: what it compares against, which way it goes when it matches, and the
	 * nodes it was written as */
	struct FRung {
		UK2Node_IfThenElse* Asks = nullptr;
		UK2Node* Compares = nullptr;
		int32 Index = INDEX_NONE;
		TArray<UEdGraphPin*> Matches;
	};

	/* The two sides of an inequality, whichever node was written for it */
	bool SidesOf(UK2Node* Node, UEdGraphPin*& OutFirst, UEdGraphPin*& OutSecond) {
		if (Node == nullptr || !Node->IsNodePure()) return false;

		if (UK2Node_EnumInequality* Against = Cast<UK2Node_EnumInequality>(Node)) {
			OutFirst = Against->GetInput1Pin();
			OutSecond = Against->GetInput2Pin();

			return OutFirst != nullptr && OutSecond != nullptr;
		}

		UK2Node_CallFunction* Calling = Cast<UK2Node_CallFunction>(Node);

		if (Calling == nullptr || Calling->FunctionReference.GetMemberName() != TEXT("NotEqual_ByteByte")) return false;

		OutFirst = Calling->FindPin(TEXT("A"), EGPD_Input);
		OutSecond = Calling->FindPin(TEXT("B"), EGPD_Input);

		return OutFirst != nullptr && OutSecond != nullptr;
	}

	/* Whatever feeds a pin, where exactly one thing does */
	UEdGraphPin* FedBy(const UEdGraphPin* Pin) {
		return Pin != nullptr && Pin->LinkedTo.Num() == 1 ? Pin->LinkedTo[0] : nullptr;
	}

	/* Whether two pins hand over the same value. Read a read at a time, every rung gets a getter of
	 * its own, and two getters of one variable are never equal as pins, so a switch of five cases
	 * ends at the third with the rest becoming a second switch. */
	bool SameValue(const UEdGraphPin* One, const UEdGraphPin* Other) {
		if (One == Other) return true;
		if (One == nullptr || Other == nullptr || One->PinName != Other->PinName) return false;

		const UK2Node_VariableGet* OneGet = Cast<UK2Node_VariableGet>(One->GetOwningNode());
		const UK2Node_VariableGet* OtherGet = Cast<UK2Node_VariableGet>(Other->GetOwningNode());

		if (OneGet == nullptr || OtherGet == nullptr) return false;
		if (OneGet->VariableReference.GetMemberName() != OtherGet->VariableReference.GetMemberName()) return false;

		const UEdGraphPin* OneSelf = OneGet->FindPin(UEdGraphSchema_K2::PN_Self, EGPD_Input);
		const UEdGraphPin* OtherSelf = OtherGet->FindPin(UEdGraphSchema_K2::PN_Self, EGPD_Input);

		const UEdGraphPin* OneOn = OneSelf != nullptr ? FedBy(OneSelf) : nullptr;
		const UEdGraphPin* OtherOn = OtherSelf != nullptr ? FedBy(OtherSelf) : nullptr;

		return (OneOn == nullptr && OtherOn == nullptr) || (OneOn != nullptr && OtherOn != nullptr && SameValue(OneOn, OtherOn));
	}

	/* Which entry of the enum a side was picked as, said either way round */
	int32 EntryOf(const UEnum* Enum, const UEdGraphPin* Pin) {
		if (Enum == nullptr || Pin == nullptr || Pin->LinkedTo.Num() > 0 || Pin->DefaultValue.IsEmpty()) return INDEX_NONE;

		if (Pin->DefaultValue.IsNumeric()) return Enum->GetIndexByValue(FCString::Atoi64(*Pin->DefaultValue));

		return Enum->GetIndexByNameString(Pin->DefaultValue);
	}

	/* The branch a run carries on into, where that is the next rung rather than a body */
	UK2Node_IfThenElse* NextRung(const UK2Node_IfThenElse* Asks) {
		UEdGraphPin* Carries = Asks != nullptr ? Asks->GetThenPin() : nullptr;

		if (Carries == nullptr || Carries->LinkedTo.Num() != 1) return nullptr;

		return Cast<UK2Node_IfThenElse>(Carries->LinkedTo[0]->GetOwningNode());
	}
}

/* A switch over an enum, which the script writes as a ladder.
 *
 * There is no switching in the bytecode. What the compiler writes is one comparison per case: ask
 * whether the value is not this one, go to that case where it is, and carry on asking where it is
 * not. The last question falling through is the default.
 *
 * Read back a question at a time, an enum of six cases comes out as six comparisons and six
 * branches stepping down the graph, where what was drawn is one node with a way out per entry. The
 * ladder only appears in this one shape, and every rung asks the same value, so it is put back.
 *
 * Two cases may leave by the same way out, which is a ladder whose rungs lead to the same place and
 * a switch with two of its pins wired to the one node. */
struct FSwitchEnumTidy final : FGraphTidy {
	virtual const TCHAR* GetName() const override { return TEXT("SwitchEnum"); }

	virtual int32 Apply(UEdGraph* Graph) const override {
		if (Graph == nullptr) return 0;

		int32 Put = 0;

		TArray<UEdGraphNode*> Nodes = Graph->Nodes;

		TSet<UK2Node_IfThenElse*> Spent;

		for (UEdGraphNode* Node : Nodes) {
			UK2Node_IfThenElse* Head = Cast<UK2Node_IfThenElse>(Node);

			if (Head == nullptr || Spent.Contains(Head)) continue;

			UEnum* Enum = nullptr;
			UEdGraphPin* Asked = nullptr;

			TArray<FRung> Ladder;

			TSet<int32> Already;

			for (UK2Node_IfThenElse* Asks = Head; Asks != nullptr; Asks = NextRung(Asks)) {
				if (Spent.Contains(Asks)) break;

				UEdGraphPin* Says = FedBy(Asks->GetConditionPin());

				UK2Node* Compares = Says != nullptr ? Cast<UK2Node>(Says->GetOwningNode()) : nullptr;

				UEdGraphPin* First = nullptr;
				UEdGraphPin* Second = nullptr;

				if (!SidesOf(Compares, First, Second)) break;

				/* The value every rung asks about, which is the one the switch is over */
				UEdGraphPin* Over = FedBy(First);

				if (Over == nullptr) break;
				if (Asked != nullptr && !SameValue(Over, Asked)) break;

				UEnum* Held = Cast<UEnum>(Over->PinType.PinSubCategoryObject.Get());

				if (Held == nullptr || (Enum != nullptr && Held != Enum)) break;

				const int32 Entry = EntryOf(Held, Second);

				if (Entry == INDEX_NONE) break;

				/* A switch has one way out per entry, so a ladder asking after one twice is not one
				 * switch. Where an entry comes round again the ladder ends there, and the rest of it
				 * stays as it was written. */
				if (Already.Contains(Entry)) break;

				/* Where it goes when the answer is no, which is where it goes when it matches:
				 * the question written down is whether it is not this one */
				UEdGraphPin* Matches = Asks->GetElsePin();

				if (Matches == nullptr) break;

				Enum = Held;
				Asked = Over;

				FRung Rung;

				Rung.Asks = Asks;
				Rung.Compares = Compares;
				Rung.Index = Entry;
				Rung.Matches = Matches->LinkedTo;

				Ladder.Add(Rung);

				Already.Add(Entry);
			}

			/* One question is a branch somebody drew. Two in a row asking the same value against
			 * two entries of the one enum is a switch. */
			if (Ladder.Num() < 2 || Enum == nullptr || Asked == nullptr) continue;

			/* Where the last question falls through to, which is the switch's default.
			 *
			 * The node is made without a way out for it, since a switch drawn by hand need not
			 * answer for the entries it does not name. One read back from a ladder has a place the
			 * last question falls through to whenever anything is written after it, and that place
			 * is lost unless the node is told to grow the pin before it grows any. */
			UEdGraphPin* Falls = Ladder.Last().Asks->GetThenPin();

			UK2Node_SwitchEnum* Switch = NewObject<UK2Node_SwitchEnum>(Graph, UK2Node_SwitchEnum::StaticClass(), NAME_None, RF_Transactional);

			Switch->bHasDefaultPin = Falls != nullptr && Falls->LinkedTo.Num() > 0;

			Graph->AddNode(Switch, false, false);

			Switch->CreateNewGuid();
			Switch->PostPlacedNewNode();

			/* The enum is put on the node and its entries listed beside it, rather than asked for
			 * through the one call that does both. That call is not exported from every build of
			 * the editor, and the list is what the ways out are made from: a node given the enum
			 * and nothing else grows none of them.
			 *
			 * An entry the enum keeps to itself is skipped, the same as the editor skips it, or the
			 * ways out and the entries stop lining up. */
			Switch->Enum = Enum;
			Switch->EnumEntries.Empty();
			Switch->EnumFriendlyNames.Empty();

			for (int32 Entry = 0; Entry < Enum->NumEnums() - 1; ++Entry) {
				if (Enum->HasMetaData(TEXT("Hidden"), Entry) || Enum->HasMetaData(TEXT("Spacer"), Entry)) continue;

				Switch->EnumEntries.Add(FName(*Enum->GetNameStringByIndex(Entry)));
				Switch->EnumFriendlyNames.Add(Enum->GetDisplayNameTextByIndex(Entry));
			}

			if (Switch->Pins.Num() == 0) Switch->AllocateDefaultPins();

			Switch->NodePosX = Head->NodePosX;
			Switch->NodePosY = Head->NodePosY;

			UEdGraphPin* Over = Switch->GetSelectionPin();
			UEdGraphPin* In = Switch->GetExecPin();

			if (Over == nullptr || In == nullptr) {
				Graph->RemoveNode(Switch);

				continue;
			}

			Over->MakeLinkTo(Asked);

			/* Whatever ran into the first question runs into the switch */
			if (UEdGraphPin* Entered = Head->GetExecPin()) {
				for (UEdGraphPin* From : Entered->LinkedTo) In->MakeLinkTo(From);
			}

			int32 Wired = 0;

			for (const FRung& Rung : Ladder) {
				/* Asked for by name rather than by text: the text lookup turns away any name that reads
				 * as None, and an enum with an entry called None then loses that way out */
				UEdGraphPin* Way = Switch->FindPin(FName(*Enum->GetNameStringByIndex(Rung.Index)), EGPD_Output);

				/* One way out leads one place. Wiring a second onto it is the error the editor puts
				 * on the node rather than a second route. */
				if (Way == nullptr || Way->LinkedTo.Num() > 0) continue;

				for (UEdGraphPin* To : Rung.Matches) Way->MakeLinkTo(To);

				Wired++;
			}

			/* And falling off the end of the ladder is the default */
			if (UEdGraphPin* Otherwise = Switch->GetDefaultPin()) {
				for (UEdGraphPin* To : Falls->LinkedTo) {
					if (To->GetOwningNode() != Ladder.Last().Asks) Otherwise->MakeLinkTo(To);
				}
			}

			if (Wired == 0) {
				Graph->RemoveNode(Switch);

				continue;
			}

			for (const FRung& Rung : Ladder) {
				Spent.Add(Rung.Asks);

				Graph->RemoveNode(Rung.Asks);
			}

			Put++;
		}

		if (Put > 0) {
			UE_LOG(LogReflectionTidySwitchEnum, Display, TEXT("\"%s\" had %d switch(es) written as a ladder of comparisons"), *Graph->GetName(), Put);
		}

		return Put;
	}
};

REGISTER_TIDY(FSwitchEnumTidy)
