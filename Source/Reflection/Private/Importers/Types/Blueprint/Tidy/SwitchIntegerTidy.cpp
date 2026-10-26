/* Copyright Reflection Contributors 2024-2026 */

#include "Importers/Types/Blueprint/GraphTidy.h"

#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "K2Node_CallFunction.h"
#include "K2Node_IfThenElse.h"
#include "K2Node_SwitchInteger.h"

DECLARE_LOG_CATEGORY_CLASS(LogReflectionTidySwitchInteger, All, All);

namespace {
	/* One rung of the ladder: the number it compares against, where it goes when it matches, and the
	 * nodes it was written as */
	struct FIntegerRung {
		UK2Node_IfThenElse* Asks = nullptr;
		UK2Node* Compares = nullptr;
		int32 Case = 0;
		TArray<UEdGraphPin*> Matches;
	};

	/* The two sides of an integer inequality */
	bool IntegerSidesOf(UK2Node* Node, UEdGraphPin*& OutFirst, UEdGraphPin*& OutSecond) {
		UK2Node_CallFunction* Calling = Cast<UK2Node_CallFunction>(Node);

		if (Calling == nullptr || !Calling->IsNodePure()) return false;
		if (Calling->FunctionReference.GetMemberName() != TEXT("NotEqual_IntInt")) return false;

		OutFirst = Calling->FindPin(TEXT("A"), EGPD_Input);
		OutSecond = Calling->FindPin(TEXT("B"), EGPD_Input);

		return OutFirst != nullptr && OutSecond != nullptr;
	}

	/* Whatever feeds a pin, where exactly one thing does */
	UEdGraphPin* IntegerFedBy(const UEdGraphPin* Pin) {
		return Pin != nullptr && Pin->LinkedTo.Num() == 1 ? Pin->LinkedTo[0] : nullptr;
	}

	/* The branch a run carries on into, where that is the next rung rather than a body */
	UK2Node_IfThenElse* NextIntegerRung(const UK2Node_IfThenElse* Asks) {
		UEdGraphPin* Carries = Asks != nullptr ? Asks->GetThenPin() : nullptr;

		if (Carries == nullptr || Carries->LinkedTo.Num() != 1) return nullptr;

		return Cast<UK2Node_IfThenElse>(Carries->LinkedTo[0]->GetOwningNode());
	}
}

/* A switch over an integer, which the script writes as a ladder.
 *
 * The same shape as the enum and name switches: one question per case, whether the value is not this
 * number, going to the case where it is and asking the next where it is not. Read back a question at
 * a time it is a comparison and a branch per case, and every branch is a node the value is worked out
 * again for, where the switch works it out once.
 *
 * An integer switch has no list of cases. It counts up from where it starts, a pin per number, so
 * only a ladder that asks after consecutive numbers in rising order is one: that is the only order
 * the compiler writes them in. */
struct FSwitchIntegerTidy final : FGraphTidy {
	virtual const TCHAR* GetName() const override { return TEXT("SwitchInteger"); }

	virtual int32 Apply(UEdGraph* Graph) const override {
		if (Graph == nullptr) return 0;

		int32 Put = 0;

		TArray<UEdGraphNode*> Nodes = Graph->Nodes;

		TSet<UK2Node_IfThenElse*> Spent;

		for (UEdGraphNode* Node : Nodes) {
			UK2Node_IfThenElse* Head = Cast<UK2Node_IfThenElse>(Node);

			if (Head == nullptr || Spent.Contains(Head)) continue;

			UEdGraphPin* Asked = nullptr;

			TArray<FIntegerRung> Ladder;

			for (UK2Node_IfThenElse* Asks = Head; Asks != nullptr; Asks = NextIntegerRung(Asks)) {
				if (Spent.Contains(Asks)) break;

				UEdGraphPin* Says = IntegerFedBy(Asks->GetConditionPin());
				UK2Node* Compares = Says != nullptr ? Cast<UK2Node>(Says->GetOwningNode()) : nullptr;

				UEdGraphPin* First = nullptr;
				UEdGraphPin* Second = nullptr;

				if (!IntegerSidesOf(Compares, First, Second)) break;

				/* The value every rung asks about, which is the one the switch is over */
				UEdGraphPin* Over = IntegerFedBy(First);

				if (Over == nullptr) break;
				if (Asked != nullptr && Over != Asked) break;

				/* Only a number written into the node is a case; one worked out at run time is a
				 * question somebody drew */
				if (Second->LinkedTo.Num() > 0 || !Second->DefaultValue.IsNumeric()) break;

				const int32 Case = FCString::Atoi(*Second->DefaultValue);

				/* Counting up one at a time from the first, or it is not the one switch */
				if (Ladder.Num() > 0 && Case != Ladder.Last().Case + 1) break;

				UEdGraphPin* Matches = Asks->GetElsePin();

				if (Matches == nullptr) break;

				Asked = Over;

				FIntegerRung Rung;

				Rung.Asks = Asks;
				Rung.Compares = Compares;
				Rung.Case = Case;
				Rung.Matches = Matches->LinkedTo;

				Ladder.Add(Rung);
			}

			if (Ladder.Num() < 2 || Asked == nullptr) continue;

			/* Where the last question falls through to, which is the switch's default */
			UEdGraphPin* Falls = Ladder.Last().Asks->GetThenPin();

			UK2Node_SwitchInteger* Switch = NewObject<UK2Node_SwitchInteger>(Graph, UK2Node_SwitchInteger::StaticClass(), NAME_None, RF_Transactional);

			/* Kept the way the editor makes it, with a default whether or not anything is wired to it */
			Switch->bHasDefaultPin = true;
			Switch->StartIndex = Ladder[0].Case;

			Graph->AddNode(Switch, false, false);

			Switch->CreateNewGuid();
			Switch->PostPlacedNewNode();

			if (Switch->Pins.Num() == 0) Switch->AllocateDefaultPins();

			/* A pin a case, counted up from where it starts */
			for (int32 Index = 0; Index < Ladder.Num(); ++Index) {
				Switch->AddPinToSwitchNode();
			}

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

			for (const FIntegerRung& Rung : Ladder) {
				UEdGraphPin* Way = Switch->FindPin(FName(*LexToString(Rung.Case)), EGPD_Output);

				if (Way == nullptr || Way->LinkedTo.Num() > 0) continue;

				for (UEdGraphPin* To : Rung.Matches) Way->MakeLinkTo(To);

				Wired++;
			}

			if (UEdGraphPin* Otherwise = Switch->GetDefaultPin()) {
				for (UEdGraphPin* To : Falls->LinkedTo) {
					if (To->GetOwningNode() != Ladder.Last().Asks) Otherwise->MakeLinkTo(To);
				}
			}

			if (Wired != Ladder.Num()) {
				Graph->RemoveNode(Switch);

				continue;
			}

			/* Sat where the first question was, so the cases are written where the asset wrote them */
			const int32 HeadAt = Graph->Nodes.IndexOfByKey(Head);

			if (HeadAt != INDEX_NONE && Graph->Nodes.Remove(Switch) > 0) {
				Graph->Nodes.Insert(Switch, HeadAt);
			}

			for (const FIntegerRung& Rung : Ladder) {
				Spent.Add(Rung.Asks);

				Graph->RemoveNode(Rung.Asks);

				/* The comparison was only there for the branch, and the switch makes its own */
				if (Rung.Compares != nullptr) Graph->RemoveNode(Rung.Compares);
			}

			Put++;
		}

		if (Put > 0) {
			UE_LOG(LogReflectionTidySwitchInteger, Display, TEXT("\"%s\" had %d integer switch(es) written as a ladder of comparisons"), *Graph->GetName(), Put);
		}

		return Put;
	}
};

REGISTER_TIDY(FSwitchIntegerTidy)
