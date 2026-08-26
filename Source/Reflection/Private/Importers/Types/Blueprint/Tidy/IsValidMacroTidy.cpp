/* Copyright Reflection Contributors 2024-2026 */

#include "Importers/Types/Blueprint/GraphTidy.h"
#include "Importers/Types/Blueprint/MacroPattern.h"

#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "K2Node_CallFunction.h"
#include "K2Node_IfThenElse.h"
#include "K2Node_MacroInstance.h"
#include "K2Node_VariableGet.h"
#include "Kismet/KismetSystemLibrary.h"

DECLARE_LOG_CATEGORY_CLASS(LogReflectionTidyValid, All, All);

namespace {
	/* A pin of the macro, by the name the macro graph gives it rather than by how it is spaced */
	UEdGraphPin* Named(UEdGraphNode* Node, const TCHAR* Wanted, const EEdGraphPinDirection Direction) {
		if (Node == nullptr) return nullptr;

		const FString Asked = FString(Wanted).Replace(TEXT(" "), TEXT(""));

		for (UEdGraphPin* Pin : Node->Pins) {
			if (Pin == nullptr || Pin->Direction != Direction) continue;

			if (Pin->PinName.ToString().Replace(TEXT(" "), TEXT("")).Equals(Asked, ESearchCase::IgnoreCase)) return Pin;
		}

		return nullptr;
	}

	/* The way into a node, whatever that node calls it */
	UEdGraphPin* WayInto(UEdGraphNode* Node) {
		for (UEdGraphPin* Pin : Node->Pins) {
			if (Pin != nullptr && Pin->Direction == EGPD_Input && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec) return Pin;
		}

		return nullptr;
	}
}

/* Asking whether something is valid and branching on the answer.
 *
 * Two nodes in a graph, and the engine offers one that is both: the IsValid macro, which takes the
 * object and runs from Is Valid or Is Not Valid. Compiled, the two are indistinguishable from the
 * one, since the macro is copied in wherever it was used.
 *
 * A validated get is the same thing again where the object came from reading a variable, and that
 * is one node rather than two, so it is left to whatever puts those back and only the rest is
 * taken here. Which means neither has to run before the other. */
struct FIsValidMacroTidy final : FGraphTidy {
	virtual const TCHAR* GetName() const override { return TEXT("IsValid"); }

	virtual int32 Apply(UEdGraph* Graph) const override {
		if (Graph == nullptr) return 0;

		int32 PutBack = 0;

		/* Over a copy, since the graph is being taken apart as it is read */
		TArray<UEdGraphNode*> Nodes = Graph->Nodes;

		for (UEdGraphNode* Node : Nodes) {
			UK2Node_CallFunction* Asking = Cast<UK2Node_CallFunction>(Node);

			if (Asking == nullptr || Asking->FunctionReference.GetMemberName() != TEXT("IsValid")) continue;

			/* The one that asks of an object, and not any of the others by that name */
			if (Asking->FunctionReference.GetMemberParentClass() != UKismetSystemLibrary::StaticClass()) continue;

			UEdGraphPin* Object = Asking->FindPin(TEXT("Object"), EGPD_Input);
			UEdGraphPin* Answer = Asking->GetReturnValuePin();

			if (Object == nullptr || Answer == nullptr) continue;

			/* Asked of one thing and answered to one place, or taking it away would leave whoever
			 * else was reading it with nothing */
			if (Object->LinkedTo.Num() != 1 || Answer->LinkedTo.Num() != 1) continue;

			UK2Node_IfThenElse* Branch = Cast<UK2Node_IfThenElse>(Answer->LinkedTo[0]->GetOwningNode());

			if (Branch == nullptr || Answer->LinkedTo[0] != Branch->GetConditionPin()) continue;

			UEdGraphPin* Source = Object->LinkedTo[0];

			/* Read straight out of a variable, which is one node and not this one */
			if (const UK2Node_VariableGet* Reading = Cast<UK2Node_VariableGet>(Source->GetOwningNode())) {
				if (Reading->IsNodePure() && Source->LinkedTo.Num() == 1) continue;
			}

			UEdGraph* Definition = MacroReading::StandardMacro(TEXT("IsValid"));

			if (Definition == nullptr) {
				UE_LOG(LogReflectionTidyValid, Warning, TEXT("%s"), TEXT("no IsValid macro to put back into"));

				continue;
			}

			/* What the branch was wired to, taken before any of it is pulled apart */
			UEdGraphPin* Entered = WayInto(Branch);

			const TArray<UEdGraphPin*> Enters = Entered != nullptr ? Entered->LinkedTo : TArray<UEdGraphPin*>();
			const TArray<UEdGraphPin*> WhenValid = Branch->GetThenPin() != nullptr ? Branch->GetThenPin()->LinkedTo : TArray<UEdGraphPin*>();
			const TArray<UEdGraphPin*> WhenNot = Branch->GetElsePin() != nullptr ? Branch->GetElsePin()->LinkedTo : TArray<UEdGraphPin*>();

			UK2Node_MacroInstance* Asks = NewObject<UK2Node_MacroInstance>(Graph);

			Graph->AddNode(Asks, false, false);

			Asks->CreateNewGuid();
			Asks->PostPlacedNewNode();

			Asks->SetMacroGraph(Definition);
			Asks->AllocateDefaultPins();

			/* Where the branch stood, since that is where the run reads it */
			Asks->NodePosX = Branch->NodePosX;
			Asks->NodePosY = Branch->NodePosY;

			const UEdGraphSchema_K2* Schema = GetDefault<UEdGraphSchema_K2>();

			if (UEdGraphPin* Given = Named(Asks, TEXT("InputObject"), EGPD_Input)) {
				Schema->TryCreateConnection(Source, Given);
			}

			if (UEdGraphPin* In = WayInto(Asks)) {
				for (UEdGraphPin* Enter : Enters) Schema->TryCreateConnection(Enter, In);
			}

			if (UEdGraphPin* Valid = Named(Asks, TEXT("Is Valid"), EGPD_Output)) {
				for (UEdGraphPin* Next : WhenValid) Schema->TryCreateConnection(Valid, Next);
			}

			if (UEdGraphPin* NotValid = Named(Asks, TEXT("Is Not Valid"), EGPD_Output)) {
				for (UEdGraphPin* Next : WhenNot) Schema->TryCreateConnection(NotValid, Next);
			}

			Graph->RemoveNode(Branch);
			Graph->RemoveNode(Asking);

			PutBack++;
		}

		if (PutBack > 0) {
			UE_LOG(LogReflectionTidyValid, Display, TEXT("\"%s\" had %d check(s) asked and branched on at once"), *Graph->GetName(), PutBack);
		}

		return PutBack;
	}
};

REGISTER_TIDY(FIsValidMacroTidy)
