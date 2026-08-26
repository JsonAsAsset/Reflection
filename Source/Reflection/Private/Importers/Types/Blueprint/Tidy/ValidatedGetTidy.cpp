/* Copyright Reflection Contributors 2024-2026 */

#include "Importers/Types/Blueprint/GraphTidy.h"

#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "K2Node_CallFunction.h"
#include "K2Node_IfThenElse.h"
#include "K2Node_VariableGet.h"
#include "Kismet/KismetSystemLibrary.h"

DECLARE_LOG_CATEGORY_CLASS(LogReflectionTidy, All, All);

/* A get that checks what it read.
 *
 * One node in a graph, three in a script: the value is read, IsValid is asked of it, and the answer
 * is branched on. Nothing in the bytecode says it was ever one node, but the three only appear in
 * that shape together, so the one can be put back.
 *
 * The engine calls it a Validated Get, and it is the same UK2Node_VariableGet wearing a different
 * variation: an execution pin in, Is Valid and Is Not Valid out, and the value alongside them. */
struct FValidatedGetTidy final : FGraphTidy {
	virtual const TCHAR* GetName() const override { return TEXT("Validated Get"); }

	virtual int32 Apply(UEdGraph* Graph) const override {
		if (Graph == nullptr) return 0;

		int32 PutBack = 0;
		int32 Seen = 0;

		/* Over a copy, since the graph is being taken apart as it is read */
		TArray<UEdGraphNode*> Nodes = Graph->Nodes;

		for (UEdGraphNode* Node : Nodes) {
			UK2Node_CallFunction* Asking = Cast<UK2Node_CallFunction>(Node);

			if (Asking == nullptr || Asking->FunctionReference.GetMemberName() != TEXT("IsValid")) continue;

			Seen++;

			/* The one that asks of an object, and not any of the others by that name */
			if (Asking->FunctionReference.GetMemberParentClass() != UKismetSystemLibrary::StaticClass()) {
				UE_LOG(LogReflectionTidy, Warning, TEXT("IsValid on %s, not the one asked of an object"), *GetNameSafe(Asking->FunctionReference.GetMemberParentClass()));

				continue;
			}

			UEdGraphPin* Object = Asking->FindPin(TEXT("Object"), EGPD_Input);
			UEdGraphPin* Answer = Asking->GetReturnValuePin();

			if (Object == nullptr || Answer == nullptr) {
				UE_LOG(LogReflectionTidy, Warning, TEXT("%s"), TEXT("no Object pin or no answer"));

				continue;
			}

			/* Read from one place and answered to one place, or taking it away would leave whoever
			 * else was reading it with nothing */
			if (Object->LinkedTo.Num() != 1 || Answer->LinkedTo.Num() != 1) {
				UE_LOG(LogReflectionTidy, Warning, TEXT("IsValid is read from %d place(s) and answered to %d"), Object->LinkedTo.Num(), Answer->LinkedTo.Num());

				continue;
			}

			UK2Node_VariableGet* Get = Cast<UK2Node_VariableGet>(Object->LinkedTo[0]->GetOwningNode());
			UK2Node_IfThenElse* Branch = Cast<UK2Node_IfThenElse>(Answer->LinkedTo[0]->GetOwningNode());

			if (Get == nullptr || Branch == nullptr) {
				UE_LOG(LogReflectionTidy, Warning, TEXT("%s"), TEXT("not a get feeding a branch"));

				continue;
			}
			if (Answer->LinkedTo[0] != Branch->GetConditionPin()) {
				UE_LOG(LogReflectionTidy, Warning, TEXT("%s"), TEXT("the answer does not go to the branch condition"));

				continue;
			}

			/* Already wearing a variation, so it is not a plain get to put back */
			if (!Get->IsNodePure()) {
				UE_LOG(LogReflectionTidy, Warning, TEXT("the get already wears a variation"));

				continue;
			}

			/* What the branch was wired to, taken before any of it is pulled apart */
			UEdGraphPin* Entered = Branch->FindPin(UEdGraphSchema_K2::PN_Execute, EGPD_Input);

			const TArray<UEdGraphPin*> Enters = Entered != nullptr ? Entered->LinkedTo : TArray<UEdGraphPin*>();
			const TArray<UEdGraphPin*> WhenValid = Branch->GetThenPin() != nullptr ? Branch->GetThenPin()->LinkedTo : TArray<UEdGraphPin*>();
			const TArray<UEdGraphPin*> WhenNot = Branch->GetElsePin() != nullptr ? Branch->GetElsePin()->LinkedTo : TArray<UEdGraphPin*>();

			/* Everything else that was reading the value, which the get goes on handing out */
			TArray<UEdGraphPin*> Readers;

			if (UEdGraphPin* Value = Get->FindPin(Get->GetVarName(), EGPD_Output)) {
				for (UEdGraphPin* Reader : Value->LinkedTo) {
					if (Reader != Object) Readers.Add(Reader);
				}
			}

			if (!SetVariableGetVariation(Get, EGetNodeVariation::ValidatedObject)) {
				UE_LOG(LogReflectionTidy, Warning, TEXT("the get would not take the variation"));

				continue;
			}

			/* Wired back up as the one node it was written as. Whoever else was reading the value
			 * is given a reading of its own, since this one is now part of the run. */
			GiveReadersTheirOwn(Graph, Get, Readers);

			if (UEdGraphPin* In = Get->FindPin(UEdGraphSchema_K2::PN_Execute, EGPD_Input)) {
				for (UEdGraphPin* Enter : Enters) Enter->MakeLinkTo(In);
			}

			if (UEdGraphPin* Valid = Get->FindPin(UEdGraphSchema_K2::PN_Then, EGPD_Output)) {
				for (UEdGraphPin* Next : WhenValid) Valid->MakeLinkTo(Next);
			}

			if (UEdGraphPin* NotValid = Get->FindPin(UEdGraphSchema_K2::PN_Else, EGPD_Output)) {
				for (UEdGraphPin* Next : WhenNot) NotValid->MakeLinkTo(Next);
			}

			Graph->RemoveNode(Asking);
			Graph->RemoveNode(Branch);

			PutBack++;
		}

		UE_LOG(LogReflectionTidy, Display, TEXT("\"%s\": %d call(s) to IsValid, %d put back"), *Graph->GetName(), Seen, PutBack);

		if (PutBack > 0) {
			UE_LOG(LogReflectionTidy, Display, TEXT("\"%s\" had %d get(s) that check what they read"), *Graph->GetName(), PutBack);
		}

		return PutBack;
	}

};

REGISTER_TIDY(FValidatedGetTidy)
