/* Copyright Reflection Contributors 2024-2026 */

#include "Importers/Types/Blueprint/GraphTidy.h"

#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "K2Node_IfThenElse.h"
#include "K2Node_VariableGet.h"

DECLARE_LOG_CATEGORY_CLASS(LogReflectionTidyBranch, All, All);

/* A boolean read and immediately branched on.
 *
 * Two nodes in a graph, and there is no reason for the first: a get can be a branch itself, with
 * True and False coming off the same node that read the value. The compiler writes it out as a
 * read and a branch either way, so nothing in the bytecode says which was written, but the two
 * only appear in this shape together.
 *
 * The get keeps handing the value out, so anything else reading it goes on reading it. */
struct FBooleanBranchTidy final : FGraphTidy {
	virtual const TCHAR* GetName() const override { return TEXT("Boolean Branch"); }

	virtual int32 Apply(UEdGraph* Graph) const override {
		if (Graph == nullptr) return 0;

		int32 PutBack = 0;

		/* Over a copy, since the graph is being taken apart as it is read */
		TArray<UEdGraphNode*> Nodes = Graph->Nodes;

		for (UEdGraphNode* Node : Nodes) {
			UK2Node_IfThenElse* Branch = Cast<UK2Node_IfThenElse>(Node);

			if (Branch == nullptr) continue;

			UEdGraphPin* Condition = Branch->GetConditionPin();

			/* Branched on one thing, and that thing read from somewhere rather than sat on the pin */
			if (Condition == nullptr || Condition->LinkedTo.Num() != 1) continue;

			UEdGraphPin* Read = Condition->LinkedTo[0];
			UK2Node_VariableGet* Get = Cast<UK2Node_VariableGet>(Read->GetOwningNode());

			if (Get == nullptr || !Get->IsNodePure()) continue;

			/* A boolean, since anything else has nothing to branch on */
			if (Read->PinType.PinCategory != UEdGraphSchema_K2::PC_Boolean) continue;

			/* Read by this branch and nothing else. Taking the branch away would put the get into
			 * the run of execution, and anything else reading it would be read from there instead
			 * of where it was written. */
			if (Read->LinkedTo.Num() != 1) continue;

			/* What the branch was wired to, taken before any of it is pulled apart */
			UEdGraphPin* Entered = Branch->FindPin(UEdGraphSchema_K2::PN_Execute, EGPD_Input);

			const TArray<UEdGraphPin*> Enters = Entered != nullptr ? Entered->LinkedTo : TArray<UEdGraphPin*>();
			const TArray<UEdGraphPin*> WhenTrue = Branch->GetThenPin() != nullptr ? Branch->GetThenPin()->LinkedTo : TArray<UEdGraphPin*>();
			const TArray<UEdGraphPin*> WhenFalse = Branch->GetElsePin() != nullptr ? Branch->GetElsePin()->LinkedTo : TArray<UEdGraphPin*>();

			if (!SetVariableGetVariation(Get, EGetNodeVariation::Branch)) continue;

			/* Wired back up as the one node it was written as */
			if (UEdGraphPin* In = Get->FindPin(UEdGraphSchema_K2::PN_Execute, EGPD_Input)) {
				for (UEdGraphPin* Enter : Enters) Enter->MakeLinkTo(In);
			}

			if (UEdGraphPin* True = Get->FindPin(UEdGraphSchema_K2::PN_Then, EGPD_Output)) {
				for (UEdGraphPin* Next : WhenTrue) True->MakeLinkTo(Next);
			}

			if (UEdGraphPin* False = Get->FindPin(UEdGraphSchema_K2::PN_Else, EGPD_Output)) {
				for (UEdGraphPin* Next : WhenFalse) False->MakeLinkTo(Next);
			}

			Graph->RemoveNode(Branch);

			PutBack++;
		}

		if (PutBack > 0) {
			UE_LOG(LogReflectionTidyBranch, Display, TEXT("\"%s\" had %d boolean(s) read and branched on at once"), *Graph->GetName(), PutBack);
		}

		return PutBack;
	}
};

REGISTER_TIDY(FBooleanBranchTidy)
