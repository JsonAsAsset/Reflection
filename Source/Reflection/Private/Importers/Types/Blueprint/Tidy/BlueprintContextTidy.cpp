/* Copyright Reflection Contributors 2024-2026 */

#include "Importers/Types/Blueprint/GraphTidy.h"

#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "K2Node_CallFunction.h"
#include "K2Node_DynamicCast.h"

DECLARE_LOG_CATEGORY_CLASS(LogReflectionTidyContext, All, All);

namespace {
	/* The two nodes this puts back, neither of which is named here at build time.
	 *
	 * One comes from a game's own editor module, so it is asked for by name and everything set on
	 * it is set through the property system. A build without that module finds nothing and the
	 * tidying leaves what it covers alone, which is the right answer: the call and the cast are
	 * what the bytecode says and they work. */
	UClass* ContextNodeClass() {
		static UClass* Kind = FindObject<UClass>(nullptr, TEXT("/Script/BlueprintContextEditor.K2Node_GetBlueprintContext"));

		return Kind;
	}

	UClass* SubsystemNodeClass() {
		static UClass* Kind = FindObject<UClass>(nullptr, TEXT("/Script/BlueprintGraph.K2Node_GetSubsystem"));

		return Kind;
	}

	/* What a context is, asked for rather than linked against.
	 *
	 * This is the question, and it is asked first. A game may keep its contexts as subsystems, so
	 * being one says nothing: everything that is a context would be a subsystem as well, and asking
	 * that first sends every one of them to the wrong node. */
	bool IsBlueprintContext(const UClass* Class) {
		static UClass* Kind = FindObject<UClass>(nullptr, TEXT("/Script/BlueprintContext.BlueprintContextBase"));

		return Class != nullptr && Kind != nullptr && Class->IsChildOf(Kind);
	}

	/* And a subsystem, for what is left */
	bool IsSubsystem(const UClass* Class) {
		static UClass* Kind = FindObject<UClass>(nullptr, TEXT("/Script/Engine.Subsystem"));

		return Class != nullptr && Kind != nullptr && Class->IsChildOf(Kind);
	}

	/* What the cast was asked to reach, which is what the node is for */
	UEdGraphPin* CastResult(UK2Node_DynamicCast* Cast) {
		for (UEdGraphPin* Pin : Cast->Pins) {
			if (Pin->Direction == EGPD_Output && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Object) return Pin;
		}

		return nullptr;
	}
}

/* A context asked for, and then asked what it is.
 *
 * A game that keeps its own per player state reaches it through one library call handed a class,
 * and the call is declared as giving back the base of them all. So whoever wrote it wrote one node,
 * and what the compiler wrote down is that node followed by a cast to the class they asked for.
 *
 * Read back statement by statement the cast comes with it, and the graph gains a pair of nodes
 * saying what one said. Worse, the pair is not what the editor offers: the node it gives has the
 * class on the node itself, so a graph rebuilt as a call and a cast cannot be edited into the shape
 * it came from.
 *
 * Which node depends on what was asked for. A class the game keeps as a subsystem is reached with
 * the engine own Get Subsystem; anything else is the game own Get Blueprint Context, and that one
 * refuses a class that is not one of its contexts. Handed the wrong one it clears what it was given
 * and says it has no class, which is worse than the call it replaced, so what it took is read back
 * before the call is thrown away.
 *
 * The one call may be read by several casts, since the value it gives is worth nothing until it is
 * cast and whoever wrote two of these wrote a node each. */
struct FBlueprintContextTidy final : FGraphTidy {
	virtual const TCHAR* GetName() const override { return TEXT("BlueprintContext"); }

	virtual int32 Apply(UEdGraph* Graph) const override {
		if (Graph == nullptr) return 0;

		int32 Put = 0;

		TArray<UEdGraphNode*> Nodes = Graph->Nodes;

		for (UEdGraphNode* Node : Nodes) {
			UK2Node_CallFunction* Calling = Cast<UK2Node_CallFunction>(Node);

			if (Calling == nullptr) continue;

			/* Which node wrote the call. GetContext only comes from the game's context node, whatever the
			 * class; GetLocalPlayerSubsystem is what the engine's Get Subsystem writes for a local player
			 * subsystem. Both hand back the base and leave the narrowing to the node, so both come back
			 * from the script as a call and a cast. */
			const UClass* Library = Calling->FunctionReference.GetMemberParentClass();
			const FName Called = Calling->FunctionReference.GetMemberName();

			UClass* Kind = nullptr;

			if (Library != nullptr && Called == TEXT("GetContext") && Library->GetName() == TEXT("BlueprintContextLibrary")) Kind = ContextNodeClass();
			else if (Library != nullptr && Called == TEXT("GetLocalPlayerSubsystem") && Library->GetName() == TEXT("SubsystemBlueprintLibrary")) Kind = SubsystemNodeClass();

			if (Kind == nullptr) continue;

			UEdGraphPin* Gives = Calling->GetReturnValuePin();

			if (Gives == nullptr || Gives->LinkedTo.Num() == 0) continue;

			/* Only where the casts are the whole of what reads it. Anything else holding the base
			 * still wants the call, and taking it out would leave them reading nothing. */
			TArray<UK2Node_DynamicCast*> Casts;

			bool bOnlyCasts = true;

			for (UEdGraphPin* Reader : Gives->LinkedTo) {
				UK2Node_DynamicCast* Held = Reader != nullptr ? Cast<UK2Node_DynamicCast>(Reader->GetOwningNode()) : nullptr;

				/* A cast that is asked whether it worked is a cast somebody wrote, and it is
				 * answering a question the one node cannot */
				if (Held == nullptr || Held->TargetType == nullptr || !Held->IsNodePure() || CastResult(Held) == nullptr) {
					bOnlyCasts = false;

					break;
				}

				UEdGraphPin* Worked = Held->GetBoolSuccessPin();

				if (Worked != nullptr && Worked->LinkedTo.Num() > 0) {
					bOnlyCasts = false;

					break;
				}

				Casts.Add(Held);
			}

			if (!bOnlyCasts || Casts.Num() == 0) continue;

			int32 Stood = 0;

			/* The node already standing for each class asked for. The script asks once and reads the
			 * answer as often as it likes, and every read comes back with a cast of its own, so a call
			 * read three times is three casts to the one class and one node, not three */
			TMap<UClass*, UEdGraphPin*> Standing;

			for (UK2Node_DynamicCast* Casting : Casts) {
				UClass* Wanted = Casting->TargetType.Get();

				if (UEdGraphPin* const* Already = Standing.Find(Wanted)) {
					if (UEdGraphPin* Reached = CastResult(Casting)) {
						for (UEdGraphPin* Reader : Reached->LinkedTo) {
							(*Already)->MakeLinkTo(Reader);
						}

						Reached->BreakAllPinLinks();

						Graph->RemoveNode(Casting);

						Stood++;
					}

					continue;
				}

				FObjectProperty* Says = FindFProperty<FObjectProperty>(Kind, TEXT("CustomClass"));

				if (Says == nullptr) continue;

				UEdGraphNode* Made = NewObject<UEdGraphNode>(Graph, Kind, NAME_None, RF_Transactional);

				Graph->AddNode(Made, false, false);

				Made->CreateNewGuid();
				Made->PostPlacedNewNode();

				Made->NodePosX = Calling->NodePosX;
				Made->NodePosY = Casting->NodePosY;

				/* Said before the pins are made, since the pin it hands out is of whatever this says */
				Says->SetObjectPropertyValue_InContainer(Made, Wanted);

				Made->AllocateDefaultPins();

				/* And read back after, since a node that will not hold the class clears it, and
				 * what is left then says less than the call it was going to replace */
				if (Says->GetObjectPropertyValue_InContainer(Made) != Wanted) {
					Graph->RemoveNode(Made);

					UE_LOG(LogReflectionTidyContext, Warning, TEXT("\"%s\" asks for a \"%s\", which %s will not hold, so it was left as the call it was written as"),
						*Graph->GetName(), *Wanted->GetName(), *Kind->GetName());

					continue;
				}

				UEdGraphPin* Hands = nullptr;

				for (UEdGraphPin* Pin : Made->Pins) {
					if (Pin->Direction == EGPD_Output) { Hands = Pin; break; }
				}

				UEdGraphPin* Reached = CastResult(Casting);

				if (Hands == nullptr || Reached == nullptr) {
					Graph->RemoveNode(Made);

					continue;
				}

				for (UEdGraphPin* Reader : Reached->LinkedTo) {
					Hands->MakeLinkTo(Reader);
				}

				Reached->BreakAllPinLinks();

				Graph->RemoveNode(Casting);

				Standing.Add(Wanted, Hands);

				Stood++;
			}

			/* And the call goes once nothing is left reading it */
			if (Stood == Casts.Num()) Graph->RemoveNode(Calling);

			Put += Stood;
		}

		if (Put > 0) {
			UE_LOG(LogReflectionTidyContext, Display, TEXT("\"%s\" had %d context(s) asked for and cast in two nodes where the editor draws one"), *Graph->GetName(), Put);
		}

		return Put;
	}
};

REGISTER_TIDY(FBlueprintContextTidy)
