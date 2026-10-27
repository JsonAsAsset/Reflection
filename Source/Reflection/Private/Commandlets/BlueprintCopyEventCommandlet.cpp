/* Copyright Reflection Contributors 2024-2026 */

#include "Commandlets/BlueprintCopyEventCommandlet.h"

#include "EdGraphSchema_K2.h"
#include "EdGraphUtilities.h"
#include "Engine/Blueprint.h"
#include "K2Node_BaseMCDelegate.h"
#include "K2Node_CallFunction.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_Event.h"
#include "K2Node_FunctionTerminator.h"
#include "K2Node_Variable.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "UObject/SavePackage.h"

DEFINE_LOG_CATEGORY_STATIC(LogBlueprintCopyEvent, Display, All);

namespace BlueprintCopyEvent {
	bool Answers(const UEdGraphNode* Node, const FName Event) {
		if (const UK2Node_CustomEvent* Custom = Cast<UK2Node_CustomEvent>(Node)) {
			return Custom->CustomFunctionName == Event;
		}

		/* A gameplay cue event is named after its tag, and refers only to the interface function every cue is routed through */
		if (const UK2Node_Event* EventNode = Cast<UK2Node_Event>(Node)) {
			return EventNode->EventReference.GetMemberName() == Event || EventNode->CustomFunctionName == Event;
		}

		return false;
	}

	UK2Node_Event* FindEvent(UBlueprint* Blueprint, const FName Event) {
		for (UEdGraph* Graph : Blueprint->UbergraphPages) {
			if (!Graph) continue;

			for (UEdGraphNode* Node : Graph->Nodes) {
				if (Answers(Node, Event)) {
					return Cast<UK2Node_Event>(Node);
				}
			}
		}

		return nullptr;
	}

	/* Everything the event runs, by following each way out, and everything those read, by following each value in. Another
	 * event is where a run starts, never something this one runs into, so the walk stops at one. */
	void Gather(UEdGraphNode* Start, TSet<UEdGraphNode*>& Into) {
		TArray<UEdGraphNode*> Open;
		Open.Add(Start);

		while (Open.Num() > 0) {
			UEdGraphNode* Node = Open.Pop(false);

			if (!Node || Into.Contains(Node)) continue;

			Into.Add(Node);

			for (UEdGraphPin* Pin : Node->Pins) {
				if (!Pin) continue;

				const bool bExec = Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec;
				const bool bFollow = (bExec && Pin->Direction == EGPD_Output) || (!bExec && Pin->Direction == EGPD_Input);

				if (!bFollow) continue;

				for (UEdGraphPin* Linked : Pin->LinkedTo) {
					UEdGraphNode* Other = Linked ? Linked->GetOwningNode() : nullptr;

					if (Other && Other != Start && !Other->IsA<UK2Node_Event>()) {
						Open.Add(Other);
					}
				}
			}
		}
	}

	/* A member the source names by its own class is the target's own member once it is pasted there */
	bool IsSourceClass(const UClass* Class, const UBlueprint* Source) {
		/* By package rather than by class: what a node names can be the skeleton, the generated class or one the compiler
		 * has since replaced, and all of them live in the source's package */
		return Class && Class->GetOutermost() == Source->GetOutermost();
	}

	void Rehome(UEdGraphNode* Node, const UBlueprint* Source) {
		if (UK2Node_Variable* Variable = Cast<UK2Node_Variable>(Node)) {
			if (IsSourceClass(Variable->VariableReference.GetMemberParentClass(), Source)) {
				Variable->VariableReference.SetSelfMember(Variable->VariableReference.GetMemberName());
			}
		} else if (UK2Node_CallFunction* Call = Cast<UK2Node_CallFunction>(Node)) {
			if (IsSourceClass(Call->FunctionReference.GetMemberParentClass(), Source)) {
				Call->FunctionReference.SetSelfMember(Call->FunctionReference.GetMemberName());
			}
		} else if (UK2Node_BaseMCDelegate* Delegate = Cast<UK2Node_BaseMCDelegate>(Node)) {
			if (IsSourceClass(Delegate->DelegateReference.GetMemberParentClass(), Source)) {
				Delegate->DelegateReference.SetSelfMember(Delegate->DelegateReference.GetMemberName());
			}
		}
	}
}

int32 UBlueprintCopyEventCommandlet::Main(const FString& Params) {
	using namespace BlueprintCopyEvent;

	FString From, To, Event;
	if (!FParse::Value(*Params, TEXT("from="), From) || !FParse::Value(*Params, TEXT("to="), To) || !FParse::Value(*Params, TEXT("event="), Event)) {
		UE_LOG(LogBlueprintCopyEvent, Error, TEXT("usage: -run=BlueprintCopyEvent -from=/Game/Scratch/Some/Blueprint -to=/Game/Some/Blueprint -event=OnSomething"));
		return 1;
	}

	UBlueprint* Source = LoadObject<UBlueprint>(nullptr, *From);
	UBlueprint* Target = LoadObject<UBlueprint>(nullptr, *To);

	if (!Source || !Target) {
		UE_LOG(LogBlueprintCopyEvent, Error, TEXT("no blueprint at %s or %s"), *From, *To);
		return 1;
	}

	/* The target as it stood before anything was changed, since a paste that clashes says only the name it clashed with */
	if (FString GraphsFile; FParse::Value(*Params, TEXT("graphs="), GraphsFile) && !GraphsFile.IsEmpty()) {
		FString Written;

		TArray<UEdGraph*> Graphs;
		Target->GetAllGraphs(Graphs);

		for (const UEdGraph* Graph : Graphs) {
			if (!Graph) continue;

			Written += FString::Printf(TEXT("== %s (%d node(s))") LINE_TERMINATOR, *Graph->GetName(), Graph->Nodes.Num());

			for (const UEdGraphNode* Node : Graph->Nodes) {
				if (!Node) continue;

				Written += FString::Printf(TEXT("  %s [%s]") LINE_TERMINATOR, *Node->GetName(), *Node->GetNodeTitle(ENodeTitleType::ListView).ToString().Replace(TEXT("\n"), TEXT(" ")).Replace(TEXT("\r"), TEXT("")));

				for (const UEdGraphPin* Pin : Node->Pins) {
					if (!Pin || Pin->bHidden) continue;

					FString Leads;
					for (const UEdGraphPin* Linked : Pin->LinkedTo) {
						if (Linked) Leads += FString::Printf(TEXT("%s.%s "), *Linked->GetOwningNode()->GetName(), *Linked->PinName.ToString());
					}

					Written += FString::Printf(TEXT("      %s %s (%s) -> %s") LINE_TERMINATOR, Pin->Direction == EGPD_Input ? TEXT("in ") : TEXT("out"), *Pin->PinName.ToString(), *Pin->PinType.PinCategory.ToString(), Leads.IsEmpty() ? TEXT("-") : *Leads);
				}
			}
		}

		FFileHelper::SaveStringToFile(Written, *GraphsFile);
		UE_LOG(LogBlueprintCopyEvent, Display, TEXT("%s as it was loaded was written to %s"), *Target->GetName(), *GraphsFile);

		if (FParse::Param(*Params, TEXT("dumponly"))) {
			return 0;
		}
	}

	const FName EventName(*Event);

	UK2Node_Event* SourceEvent = FindEvent(Source, EventName);
	if (!SourceEvent) {
		UE_LOG(LogBlueprintCopyEvent, Error, TEXT("%s does not answer %s"), *Source->GetName(), *Event);
		return 1;
	}

	/* Run twice it would answer the event twice, which the blueprint cannot compile */
	if (UK2Node_Event* TargetEvent = FindEvent(Target, EventName)) {
		if (!FParse::Param(*Params, TEXT("replace"))) {
			UE_LOG(LogBlueprintCopyEvent, Error, TEXT("%s already answers %s, nothing changed"), *Target->GetName(), *Event);
			return 1;
		}

		TSet<UEdGraphNode*> Old;
		Gather(TargetEvent, Old);

		/* A node something else also runs or reads from belongs to that too, and taking it would break it */
		for (UEdGraphNode* Node : Old) {
			for (UEdGraphPin* Pin : Node->Pins) {
				for (UEdGraphPin* Linked : Pin ? Pin->LinkedTo : TArray<UEdGraphPin*>()) {
					if (Linked && !Old.Contains(Linked->GetOwningNode())) {
						UE_LOG(LogBlueprintCopyEvent, Error, TEXT("%s.%s is shared with %s, which %s does not run, nothing changed"), *Node->GetName(), *Pin->PinName.ToString(), *Linked->GetOwningNode()->GetName(), *Event);
						return 1;
					}
				}
			}
		}

		UE_LOG(LogBlueprintCopyEvent, Display, TEXT("replacing %s's own answer to %s, %d node(s)"), *Target->GetName(), *Event, Old.Num());

		for (UEdGraphNode* Node : Old) {
			FBlueprintEditorUtils::RemoveNode(Target, Node, true);
		}
	}

	UEdGraph* TargetGraph = FBlueprintEditorUtils::FindEventGraph(Target);
	if (!TargetGraph) {
		UE_LOG(LogBlueprintCopyEvent, Error, TEXT("%s has no event graph"), *Target->GetName());
		return 1;
	}

	/* Functions the event calls on the blueprint itself that the target has not got, brought whole, since a call to a
	 * function that is not there keeps none of its pins */
	if (FString FunctionList; FParse::Value(*Params, TEXT("functions="), FunctionList, false)) {
		TArray<FString> Functions;
		FunctionList.ParseIntoArray(Functions, TEXT(","), true);

		for (const FString& Function : Functions) {
			const FName FunctionName(*Function.TrimStartAndEnd());

			const TObjectPtr<UEdGraph>* SourceGraph = Source->FunctionGraphs.FindByPredicate([FunctionName](const UEdGraph* Graph) { return Graph && Graph->GetFName() == FunctionName; });

			if (!SourceGraph) {
				UE_LOG(LogBlueprintCopyEvent, Error, TEXT("%s has no function %s"), *Source->GetName(), *FunctionName.ToString());
				return 1;
			}

			if (const TObjectPtr<UEdGraph>* Existing = Target->FunctionGraphs.FindByPredicate([FunctionName](const UEdGraph* Graph) { return Graph && Graph->GetFName() == FunctionName; })) {
				/* Only on -replace, since what the target has may be somebody's own work rather than a stub an earlier import left */
				if (!FParse::Param(*Params, TEXT("replace"))) {
					UE_LOG(LogBlueprintCopyEvent, Error, TEXT("%s already has a function %s, nothing changed"), *Target->GetName(), *FunctionName.ToString());
					return 1;
				}

				UE_LOG(LogBlueprintCopyEvent, Display, TEXT("replacing function %s, which had %d node(s)"), *FunctionName.ToString(), (*Existing)->Nodes.Num());
				FBlueprintEditorUtils::RemoveGraph(Target, *Existing, EGraphRemoveFlags::None);
			}

			UEdGraph* Copied = DuplicateObject<UEdGraph>(*SourceGraph, Target, FunctionName);
			Target->FunctionGraphs.Add(Copied);

			for (UEdGraphNode* Node : Copied->Nodes) {
				Rehome(Node, Source);

				if (UK2Node_FunctionTerminator* Terminator = Cast<UK2Node_FunctionTerminator>(Node)) {
					Terminator->FunctionReference.SetSelfMember(FunctionName);
				}
			}

			UE_LOG(LogBlueprintCopyEvent, Display, TEXT("brought function %s, %d node(s)"), *FunctionName.ToString(), Copied->Nodes.Num());
		}

		/* The class has to know the functions before anything calling them is pasted, or the calls come in without their pins */
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Target);

		/* Their nodes still carry pins typed against the source's class until they are built again against this one */
		for (UEdGraph* Graph : Target->FunctionGraphs) {
			if (!Graph || !Functions.Contains(Graph->GetName())) continue;

			for (UEdGraphNode* Node : Graph->Nodes) {
				Node->ReconstructNode();
			}
		}
	}

	TSet<UEdGraphNode*> Nodes;
	Gather(SourceEvent, Nodes);

	/* The copy keeps only links between the nodes it carries, so anything still reaching outside is said */
	for (UEdGraphNode* Node : Nodes) {
		for (UEdGraphPin* Pin : Node->Pins) {
			for (UEdGraphPin* Linked : Pin ? Pin->LinkedTo : TArray<UEdGraphPin*>()) {
				if (Linked && !Nodes.Contains(Linked->GetOwningNode()) && Pin->Direction == EGPD_Input) {
					UE_LOG(LogBlueprintCopyEvent, Warning, TEXT("%s.%s is fed from %s, which another event runs, and comes across unlinked"), *Node->GetName(), *Pin->PinName.ToString(), *Linked->GetOwningNode()->GetName());
				}
			}
		}
	}

	TSet<UObject*> ToExport;
	for (UEdGraphNode* Node : Nodes) {
		Node->PrepareForCopying();
		ToExport.Add(Node);
	}

	FString Text;
	FEdGraphUtilities::ExportNodesToText(ToExport, Text);

	TSet<UEdGraphNode*> Pasted;
	FEdGraphUtilities::ImportNodesFromText(TargetGraph, Text, Pasted);

	if (Pasted.Num() != Nodes.Num()) {
		UE_LOG(LogBlueprintCopyEvent, Error, TEXT("%d node(s) went out and %d came in, nothing saved"), Nodes.Num(), Pasted.Num());
		return 1;
	}

	/* Laid out well below whatever the graph already holds, keeping the shape it had */
	int32 Below = 0;
	for (const UEdGraphNode* Node : TargetGraph->Nodes) {
		if (!Pasted.Contains(const_cast<UEdGraphNode*>(Node))) {
			Below = FMath::Max(Below, Node->NodePosY + 600);
		}
	}

	const int32 ShiftX = -SourceEvent->NodePosX;
	const int32 ShiftY = Below - SourceEvent->NodePosY;

	for (UEdGraphNode* Node : Pasted) {
		Node->CreateNewGuid();
		Node->NodePosX += ShiftX;
		Node->NodePosY += ShiftY;

		Rehome(Node, Source);
		Node->ReconstructNode();
	}

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Target);
	FKismetEditorUtilities::CompileBlueprint(Target, EBlueprintCompileOptions::SkipGarbageCollection);

	if (Target->Status == BS_Error) {
		UE_LOG(LogBlueprintCopyEvent, Error, TEXT("%s did not compile, nothing saved"), *Target->GetName());
		return 1;
	}

	UPackage* Package = Target->GetOutermost();
	const FString FileName = FPackageName::LongPackageNameToFilename(Package->GetName(), FPackageName::GetAssetPackageExtension());

	FSavePackageArgs SaveArgs;
	SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;

	if (!UPackage::SavePackage(Package, nullptr, *FileName, SaveArgs)) {
		UE_LOG(LogBlueprintCopyEvent, Error, TEXT("could not save %s"), *FileName);
		return 1;
	}

	UE_LOG(LogBlueprintCopyEvent, Display, TEXT("%s now answers %s with %d node(s) from %s, saved %s"), *Target->GetName(), *Event, Pasted.Num(), *Source->GetName(), *FileName);
	return 0;
}
