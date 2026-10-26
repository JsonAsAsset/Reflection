/* Copyright Reflection Contributors 2024-2026 */

#include "Importers/Types/Blueprint/GraphTidy.h"

#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "K2Node_CallFunction.h"
#include "K2Node_VariableGet.h"
#include "K2Node_VariableSet.h"

DECLARE_LOG_CATEGORY_CLASS(LogReflectionTidyAccessor, All, All);

namespace {
	/* Whether a graph may read it at all */
	bool CanRead(const FProperty* Property) {
		return Property != nullptr && Property->HasAnyPropertyFlags(CPF_BlueprintVisible);
	}

	/* And whether it may write it, which is the same permission without the lock on top */
	bool CanWrite(const FProperty* Property) {
		return CanRead(Property) && !Property->HasAnyPropertyFlags(CPF_BlueprintReadOnly);
	}

	/* Whether a function is shaped like the way in to that member.
	 *
	 * A setter takes the one thing and answers nothing; a getter takes nothing and answers the one
	 * thing. Anything else of that name is a function that happens to be called that. */
	bool Fits(const UFunction* Function, const FProperty* Property, const bool bWriting) {
		if (Function == nullptr || Property == nullptr) return false;
		if (!Function->HasAnyFunctionFlags(FUNC_BlueprintCallable | FUNC_BlueprintPure)) return false;

		const FProperty* Answers = Function->GetReturnProperty();

		int32 Takes = 0;

		const FProperty* Took = nullptr;

		for (TFieldIterator<FProperty> It(Function); It && It->HasAnyPropertyFlags(CPF_Parm); ++It) {
			if (It->HasAnyPropertyFlags(CPF_ReturnParm)) continue;

			Takes++;

			Took = *It;
		}

		if (bWriting) return Answers == nullptr && Takes == 1 && Took != nullptr && Took->SameType(Property);

		return Takes == 0 && Answers != nullptr && Answers->SameType(Property);
	}

	/* The way in the class gives, where it keeps the member to itself.
	 *
	 * A class that means one to be reached through a function says so on the member, which is the
	 * one place that is not a guess. Where it does not, the function is looked for by the name such
	 * a one is given, and taken only if it is shaped like one. */
	UFunction* WayIn(const FProperty* Property, const bool bWriting) {
		UClass* Owner = Property != nullptr ? Property->GetOwnerClass() : nullptr;

		if (Owner == nullptr) return nullptr;

		static const FName Reading(TEXT("BlueprintGetter"));
		static const FName Writing(TEXT("BlueprintSetter"));

		const FString Said = Property->GetMetaData(bWriting ? Writing : Reading);

		if (!Said.IsEmpty()) {
			if (UFunction* Named = Owner->FindFunctionByName(*Said); Fits(Named, Property, bWriting)) return Named;
		}

		const FString Called = (bWriting ? TEXT("Set") : TEXT("Get")) + Property->GetName();

		if (UFunction* Found = Owner->FindFunctionByName(*Called); Fits(Found, Property, bWriting)) return Found;

		return nullptr;
	}

	/* Everything reaching one pin, reaching another instead */
	void HandOver(UEdGraphPin* From, UEdGraphPin* To) {
		if (From == nullptr || To == nullptr) return;

		for (UEdGraphPin* Held : From->LinkedTo) {
			if (Held != nullptr) To->MakeLinkTo(Held);
		}
	}
}

/* A member the class keeps to itself, reached the way it means to be reached.
 *
 * The script reads and writes a member by name, and nothing in it says whether a graph was allowed
 * to. Most of the time it was. Where it was not, what the graph had was the function the class
 * gives for it, and the compiler wrote that out as the member it ends up touching.
 *
 * Read back as the member, the node is one the editor complains about and refuses to compile in the
 * end, so where the class has a way in and the member itself is shut, the way in is what goes down.
 * Where the member is open it is left alone: a member anybody may read is a member somebody drew. */
struct FAccessorTidy final : FGraphTidy {
	virtual const TCHAR* GetName() const override { return TEXT("Accessor"); }

	virtual int32 Apply(UEdGraph* Graph) const override {
		if (Graph == nullptr) return 0;

		int32 Through = 0;

		TArray<UEdGraphNode*> Nodes = Graph->Nodes;

		for (UEdGraphNode* Node : Nodes) {
			UK2Node_VariableGet* Reading = Cast<UK2Node_VariableGet>(Node);
			UK2Node_VariableSet* Writing = Cast<UK2Node_VariableSet>(Node);

			if (Reading == nullptr && Writing == nullptr) continue;

			UK2Node_Variable* About = Reading != nullptr ? static_cast<UK2Node_Variable*>(Reading) : static_cast<UK2Node_Variable*>(Writing);

			FProperty* Member = About->GetPropertyForVariable();

			if (Member == nullptr) continue;

			const bool bWrites = Writing != nullptr;

			/* Open to a graph, so what was drawn is what is here */
			if (bWrites ? CanWrite(Member) : CanRead(Member)) continue;

			UFunction* Gives = WayIn(Member, bWrites);

			if (Gives == nullptr) continue;

			UK2Node_CallFunction* Call = NewObject<UK2Node_CallFunction>(Graph, UK2Node_CallFunction::StaticClass(), NAME_None, RF_Transactional);

			Graph->AddNode(Call, false, false);

			Call->CreateNewGuid();
			Call->SetFromFunction(Gives);
			Call->PostPlacedNewNode();
			Call->AllocateDefaultPins();

			Call->NodePosX = Node->NodePosX;
			Call->NodePosY = Node->NodePosY;

			/* A read sits in the flow of the value and has no way in or out. A function that has
			 * one cannot stand where it stood, and the member is left as it was rather than the run
			 * being rearranged around it. */
			if (!bWrites && !Call->IsNodePure()) {
				Graph->RemoveNode(Call);

				continue;
			}

			HandOver(About->FindPin(UEdGraphSchema_K2::PN_Self, EGPD_Input), Call->FindPin(UEdGraphSchema_K2::PN_Self, EGPD_Input));

			bool bStood = false;

			if (bWrites) {
				/* What it is given, which is the one thing it takes */
				UEdGraphPin* Takes = nullptr;

				for (UEdGraphPin* Pin : Call->Pins) {
					if (Pin == nullptr || Pin->Direction != EGPD_Input) continue;
					if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec) continue;
					if (Pin->PinName == UEdGraphSchema_K2::PN_Self) continue;

					Takes = Pin;

					break;
				}

				/* Found by name rather than asked for. What a variable node hands out is an output
				 * and is asked for as one, and on a write the pin of that name is what goes in. */
				UEdGraphPin* Held = Writing->FindPin(Writing->GetVarName(), EGPD_Input);

				if (Takes != nullptr && Held != nullptr) {
					HandOver(Held, Takes);

					Takes->DefaultValue = Held->DefaultValue;
					Takes->DefaultObject = Held->DefaultObject;
					Takes->DefaultTextValue = Held->DefaultTextValue;

					/* And the run through it, which a write is part of */
					HandOver(Writing->GetExecPin(), Call->GetExecPin());
					HandOver(Writing->FindPin(UEdGraphSchema_K2::PN_Then, EGPD_Output), Call->GetThenPin());

					bStood = true;
				}
			} else {
				UEdGraphPin* Answers = Call->GetReturnValuePin();
				UEdGraphPin* Held = Reading->GetValuePin();

				if (Answers != nullptr && Held != nullptr) {
					HandOver(Held, Answers);

					bStood = true;
				}
			}

			if (!bStood) {
				Graph->RemoveNode(Call);

				continue;
			}

			Graph->RemoveNode(Node);

			Through++;
		}

		if (Through > 0) {
			UE_LOG(LogReflectionTidyAccessor, Display, TEXT("\"%s\" reached %d member(s) the way the class means them to be reached"), *Graph->GetName(), Through);
		}

		return Through;
	}
};

REGISTER_TIDY(FAccessorTidy)
