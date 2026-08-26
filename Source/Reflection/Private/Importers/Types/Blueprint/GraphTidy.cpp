/* Copyright Reflection Contributors 2024-2026 */

#include "Importers/Types/Blueprint/GraphTidy.h"

#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "K2Node_VariableGet.h"

namespace {
	TArray<TSharedRef<FGraphTidy>>& Tidies() {
		static TArray<TSharedRef<FGraphTidy>> Held;

		return Held;
	}
}

void AddGraphTidy(const TSharedRef<FGraphTidy>& Tidy) {
	Tidies().Add(Tidy);
}

const TArray<TSharedRef<FGraphTidy>>& GetGraphTidies() {
	return Tidies();
}

bool SetVariableGetVariation(UK2Node_VariableGet* Get, const EGetNodeVariation Variation) {
	if (Get == nullptr) return false;

	FProperty* Wearing = UK2Node_VariableGet::StaticClass()->FindPropertyByName(TEXT("CurrentVariation"));

	if (Wearing == nullptr) return false;

	void* Value = Wearing->ContainerPtrToValuePtr<void>(Get);

	if (const FEnumProperty* AsEnum = CastField<FEnumProperty>(Wearing)) {
		AsEnum->GetUnderlyingProperty()->SetIntPropertyValue(Value, static_cast<int64>(Variation));
	} else if (const FByteProperty* AsByte = CastField<FByteProperty>(Wearing)) {
		AsByte->SetIntPropertyValue(Value, static_cast<int64>(Variation));
	} else {
		return false;
	}

	Get->ReconstructNode();

	return true;
}

void GiveReadersTheirOwn(UEdGraph* Graph, UK2Node_VariableGet* Converted, const TArray<UEdGraphPin*>& Readers) {
	if (Graph == nullptr || Converted == nullptr) return;

	for (UEdGraphPin* Reader : Readers) {
		if (Reader == nullptr) continue;

		UK2Node_VariableGet* Own = NewObject<UK2Node_VariableGet>(Graph);

		Graph->AddNode(Own, false, false);

		Own->CreateNewGuid();
		Own->PostPlacedNewNode();

		Own->VariableReference = Converted->VariableReference;
		Own->AllocateDefaultPins();

		/* Beside whoever wanted it, rather than wherever the branch ended up */
		if (const UEdGraphNode* Wanting = Reader->GetOwningNode()) {
			Own->NodePosX = Wanting->NodePosX - 320;
			Own->NodePosY = Wanting->NodePosY + 40;
		}

		if (UEdGraphPin* Value = Own->FindPin(Own->GetVarName(), EGPD_Output)) {
			/* Asked of the schema rather than forced. Whoever is being given a reading of their own
			 * is still reading the one that was converted, and a pin takes one thing at a time: the
			 * schema is what knows to let go of the old one, and forcing the link leaves both on
			 * and the graph refusing to compile. */
			GetDefault<UEdGraphSchema_K2>()->TryCreateConnection(Value, Reader);
		}
	}
}
