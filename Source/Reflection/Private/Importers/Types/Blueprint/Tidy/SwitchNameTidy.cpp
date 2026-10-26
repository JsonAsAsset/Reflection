/* Copyright Reflection Contributors 2024-2026 */

#include "Importers/Types/Blueprint/GraphTidy.h"

#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "K2Node_CallFunction.h"
#include "K2Node_IfThenElse.h"
#include "K2Node_SwitchName.h"

DECLARE_LOG_CATEGORY_CLASS(LogReflectionTidySwitchName, All, All);

namespace {
	/* One rung of the ladder: the name it compares against, where it goes when it matches, and the
	 * nodes it was written as */
	struct FNameRung {
		UK2Node_IfThenElse* Asks = nullptr;
		UK2Node* Compares = nullptr;
		FName Case;
		TArray<UEdGraphPin*> Matches;
	};

	/* The two sides of a name inequality */
	bool NameSidesOf(UK2Node* Node, UEdGraphPin*& OutFirst, UEdGraphPin*& OutSecond) {
		UK2Node_CallFunction* Calling = Cast<UK2Node_CallFunction>(Node);

		if (Calling == nullptr || !Calling->IsNodePure()) return false;
		if (Calling->FunctionReference.GetMemberName() != TEXT("NotEqual_NameName")) return false;

		OutFirst = Calling->FindPin(TEXT("A"), EGPD_Input);
		OutSecond = Calling->FindPin(TEXT("B"), EGPD_Input);

		return OutFirst != nullptr && OutSecond != nullptr;
	}

	/* Whatever feeds a pin, where exactly one thing does */
	UEdGraphPin* NameFedBy(const UEdGraphPin* Pin) {
		return Pin != nullptr && Pin->LinkedTo.Num() == 1 ? Pin->LinkedTo[0] : nullptr;
	}

	/* The branch a run carries on into, where that is the next rung rather than a body */
	UK2Node_IfThenElse* NextNameRung(const UK2Node_IfThenElse* Asks) {
		UEdGraphPin* Carries = Asks != nullptr ? Asks->GetThenPin() : nullptr;

		if (Carries == nullptr || Carries->LinkedTo.Num() != 1) return nullptr;

		return Cast<UK2Node_IfThenElse>(Carries->LinkedTo[0]->GetOwningNode());
	}
}

/* A switch over a name, which the script writes as a ladder.
 *
 * The same shape as a switch over an enum: one question per case, whether the value is not this
 * name, going to the case where it is and asking the next where it is not. Read back a question at a
 * time it comes out as a comparison and a branch per case.
 *
 * The node goes back where the first question was, not at the end of the graph. The compiler writes
 * the runs in the order the nodes sit, and a switch added after everything else has its cases laid
 * out somewhere other than where the asset had them. */
struct FSwitchNameTidy final : FGraphTidy {
	virtual const TCHAR* GetName() const override { return TEXT("SwitchName"); }

	virtual int32 Apply(UEdGraph* Graph) const override {
		if (Graph == nullptr) return 0;

		int32 Put = 0;

		TArray<UEdGraphNode*> Nodes = Graph->Nodes;

		TSet<UK2Node_IfThenElse*> Spent;

		for (UEdGraphNode* Node : Nodes) {
			UK2Node_IfThenElse* Head = Cast<UK2Node_IfThenElse>(Node);

			if (Head == nullptr || Spent.Contains(Head)) continue;

			UEdGraphPin* Asked = nullptr;

			TArray<FNameRung> Ladder;
			TSet<FName> Already;

			for (UK2Node_IfThenElse* Asks = Head; Asks != nullptr; Asks = NextNameRung(Asks)) {
				if (Spent.Contains(Asks)) break;

				UEdGraphPin* Says = NameFedBy(Asks->GetConditionPin());
				UK2Node* Compares = Says != nullptr ? Cast<UK2Node>(Says->GetOwningNode()) : nullptr;

				UEdGraphPin* First = nullptr;
				UEdGraphPin* Second = nullptr;

				if (!NameSidesOf(Compares, First, Second)) break;

				/* The value every rung asks about, which is the one the switch is over */
				UEdGraphPin* Over = NameFedBy(First);

				if (Over == nullptr) break;
				if (Asked != nullptr && Over != Asked) break;

				/* Only a name written into the node is a case; one worked out at run time is a question
				 * somebody drew */
				if (Second->LinkedTo.Num() > 0) break;

				const FName Case(*Second->DefaultValue);

				/* A switch has one way out per case, so a name asked after twice ends the ladder */
				if (Already.Contains(Case)) break;

				UEdGraphPin* Matches = Asks->GetElsePin();

				if (Matches == nullptr) break;

				Asked = Over;

				FNameRung Rung;

				Rung.Asks = Asks;
				Rung.Compares = Compares;
				Rung.Case = Case;
				Rung.Matches = Matches->LinkedTo;

				Ladder.Add(Rung);
				Already.Add(Case);
			}

			if (Ladder.Num() < 2 || Asked == nullptr) continue;

			/* Where the last question falls through to, which is the switch's default */
			UEdGraphPin* Falls = Ladder.Last().Asks->GetThenPin();

			UK2Node_SwitchName* Switch = NewObject<UK2Node_SwitchName>(Graph, UK2Node_SwitchName::StaticClass(), NAME_None, RF_Transactional);

			/* Kept the way the editor makes it, with a default whether or not anything is wired to it. The
			 * compiler lays a switch's cases out differently without one, so dropping an empty default
			 * moves the last case's run to the front of the graph. */
			Switch->bHasDefaultPin = true;

			for (const FNameRung& Rung : Ladder) {
				Switch->PinNames.Add(Rung.Case);
			}

			Graph->AddNode(Switch, false, false);

			Switch->CreateNewGuid();
			Switch->PostPlacedNewNode();

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

			for (const FNameRung& Rung : Ladder) {
				UEdGraphPin* Way = Switch->FindPin(Rung.Case, EGPD_Output);

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

			for (const FNameRung& Rung : Ladder) {
				Spent.Add(Rung.Asks);

				Graph->RemoveNode(Rung.Asks);

				/* The comparison was only there for the branch, and the switch makes its own */
				if (Rung.Compares != nullptr) Graph->RemoveNode(Rung.Compares);
			}

			Put++;
		}

		if (Put > 0) {
			UE_LOG(LogReflectionTidySwitchName, Display, TEXT("\"%s\" had %d name switch(es) written as a ladder of comparisons"), *Graph->GetName(), Put);
		}

		return Put;
	}
};

REGISTER_TIDY(FSwitchNameTidy)
