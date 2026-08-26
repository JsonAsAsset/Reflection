/* Copyright Reflection Contributors 2024-2026 */

#include "Importers/Types/Blueprint/GraphTidy.h"

#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "K2Node_CallFunction.h"
#include "K2Node_Self.h"

DECLARE_LOG_CATEGORY_CLASS(LogReflectionTidySelf, All, All);

/* A call made against the blueprint itself, said twice.
 *
 * What a call is made against is its target, and a target left empty is the blueprint. Somebody who
 * writes it out anyway gets the same call with a reference wired into it, and the compiler keeps
 * the difference: wired, the call is written as a context around it; empty, as a plain call. So the
 * two are told apart in the bytecode, and reading one back draws the node that was drawn.
 *
 * Which is right, and reads as clutter. Every call in a blueprint is on itself unless it says
 * otherwise, and a node saying so is a node saying nothing. So it comes out here, where somebody is
 * going to read the graph, and stays in where the bytecode is what is being compared.
 *
 * This is the one tidying that costs the reading: the call is a little shorter without it. */
struct FSelfTargetTidy final : FGraphTidy {
	virtual const TCHAR* GetName() const override { return TEXT("SelfTarget"); }

	virtual int32 Apply(UEdGraph* Graph) const override {
		if (Graph == nullptr) return 0;

		const UEdGraphSchema_K2* Schema = GetDefault<UEdGraphSchema_K2>();

		int32 Dropped = 0;

		/* Over a copy, since the nodes are being taken out as they are read */
		TArray<UEdGraphNode*> Nodes = Graph->Nodes;

		for (UEdGraphNode* Node : Nodes) {
			UK2Node_CallFunction* Calling = Cast<UK2Node_CallFunction>(Node);

			if (Calling == nullptr) continue;

			UEdGraphPin* Target = Schema->FindSelfPin(*Calling, EGPD_Input);

			if (Target == nullptr || Target->LinkedTo.Num() != 1) continue;

			UK2Node_Self* Itself = Cast<UK2Node_Self>(Target->LinkedTo[0]->GetOwningNode());

			if (Itself == nullptr) continue;

			/* Only where nothing else is reading it, or taking it away would leave them with
			 * nothing to read */
			if (Target->LinkedTo[0]->LinkedTo.Num() != 1) continue;

			Graph->RemoveNode(Itself);

			Dropped++;
		}

		if (Dropped > 0) {
			UE_LOG(LogReflectionTidySelf, Display, TEXT("\"%s\" had %d call(s) made against itself in so many words"), *Graph->GetName(), Dropped);
		}

		return Dropped;
	}
};

REGISTER_TIDY(FSelfTargetTidy)
