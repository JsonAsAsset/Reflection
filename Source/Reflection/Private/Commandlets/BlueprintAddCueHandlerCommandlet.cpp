/* Copyright Reflection Contributors 2024-2026 */

#include "Commandlets/BlueprintAddCueHandlerCommandlet.h"

#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "K2Node_CallFunction.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_EnumInequality.h"
#include "K2Node_IfThenElse.h"
#include "K2Node_VariableGet.h"
#include "K2Node_VariableSet.h"
#include "Kismet/GameplayStatics.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/PackageName.h"
#include "Particles/ParticleSystem.h"
#include "Engine/Compatibility.h"
#if ENGINE_UE5
#include "UObject/SavePackage.h"
#endif

DEFINE_LOG_CATEGORY_STATIC(LogBlueprintAddCueHandler, Display, All);

namespace BlueprintAddCueHandler {
	template <typename T>
	T* Place(UEdGraph* Graph, const FVector2D& At) {
		T* Node = NewObject<T>(Graph);
		Node->CreateNewGuid();
		Node->NodePosX = At.X;
		Node->NodePosY = At.Y;

		Graph->AddNode(Node, false, false);

		return Node;
	}

	bool Link(UEdGraphNode* From, const FName& Out, UEdGraphNode* To, const FName& In) {
		UEdGraphPin* OutPin = From->FindPin(Out, EGPD_Output);
		UEdGraphPin* InPin = To->FindPin(In, EGPD_Input);

		if (!OutPin || !InPin) {
			UE_LOG(LogBlueprintAddCueHandler, Error, TEXT("no pin %s on %s or %s on %s"), *Out.ToString(), *From->GetName(), *In.ToString(), *To->GetName());
			return false;
		}

		return GetDefault<UEdGraphSchema_K2>()->TryCreateConnection(OutPin, InPin);
	}

	bool SetDefault(UEdGraphNode* Node, const FName& In, const FString& Value) {
		UEdGraphPin* Pin = Node->FindPin(In, EGPD_Input);

		if (!Pin) {
			UE_LOG(LogBlueprintAddCueHandler, Error, TEXT("no pin %s on %s"), *In.ToString(), *Node->GetName());
			return false;
		}

		GetDefault<UEdGraphSchema_K2>()->TrySetDefaultValue(*Pin, Value);
		return true;
	}

	UK2Node_CustomEvent* FindCustomEvent(UEdGraph* Graph, const FName& Name) {
		for (UEdGraphNode* Node : Graph->Nodes) {
			UK2Node_CustomEvent* Event = Cast<UK2Node_CustomEvent>(Node);

			if (Event && Event->CustomFunctionName == Name) {
				return Event;
			}
		}

		return nullptr;
	}
}

int32 UBlueprintAddCueHandlerCommandlet::Main(const FString& Params) {
	using namespace BlueprintAddCueHandler;

	FString Path, Tag, Like;
	if (!FParse::Value(*Params, TEXT("path="), Path) || !FParse::Value(*Params, TEXT("tag="), Tag) || !FParse::Value(*Params, TEXT("like="), Like)) {
		UE_LOG(LogBlueprintAddCueHandler, Error, TEXT("usage: -run=BlueprintAddCueHandler -path=/Game/Some/Pawn -tag=GameplayCue.Some.Cue -like=GameplayCue.Other.Cue (-spawn=Variable [-socket=root] [-skipdedicated] | -setbool=Variable)"));
		return 1;
	}

	FString SpawnVariable, BoolVariable;
	const bool bSpawn = FParse::Value(*Params, TEXT("spawn="), SpawnVariable) && !SpawnVariable.IsEmpty();
	const bool bSetBool = FParse::Value(*Params, TEXT("setbool="), BoolVariable) && !BoolVariable.IsEmpty();

	if (bSpawn == bSetBool) {
		UE_LOG(LogBlueprintAddCueHandler, Error, TEXT("give it one of -spawn= or -setbool="));
		return 1;
	}

	UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *Path);
	if (!Blueprint) {
		UE_LOG(LogBlueprintAddCueHandler, Error, TEXT("no blueprint at %s"), *Path);
		return 1;
	}

	UEdGraph* EventGraph = FBlueprintEditorUtils::FindEventGraph(Blueprint);
	if (!EventGraph) {
		UE_LOG(LogBlueprintAddCueHandler, Error, TEXT("%s has no event graph"), *Blueprint->GetName());
		return 1;
	}

	/* Run twice it would answer the cue twice, which the blueprint cannot compile */
	if (FindCustomEvent(EventGraph, FName(*Tag))) {
		UE_LOG(LogBlueprintAddCueHandler, Error, TEXT("%s already answers %s, nothing changed"), *Blueprint->GetName(), *Tag);
		return 1;
	}

	/* The cue interface calls a handler by the tag's name with the signature every handler shares, so the new one copies
	 * it from one the blueprint already has rather than spelling it out */
	const UK2Node_CustomEvent* Template = FindCustomEvent(EventGraph, FName(*Like));
	if (!Template) {
		UE_LOG(LogBlueprintAddCueHandler, Error, TEXT("%s has no handler %s to copy the signature from"), *Blueprint->GetName(), *Like);
		return 1;
	}

	FString AddVariable, VariableDefault;
	if (FParse::Value(*Params, TEXT("addvar="), AddVariable) && !AddVariable.IsEmpty() && FBlueprintEditorUtils::FindNewVariableIndex(Blueprint, FName(*AddVariable)) == INDEX_NONE) {
		FParse::Value(*Params, TEXT("vardefault="), VariableDefault);

		FEdGraphPinType PinType;
		PinType.PinCategory = UEdGraphSchema_K2::PC_Object;
		PinType.PinSubCategoryObject = UParticleSystem::StaticClass();

		if (!FBlueprintEditorUtils::AddMemberVariable(Blueprint, FName(*AddVariable), PinType, VariableDefault)) {
			UE_LOG(LogBlueprintAddCueHandler, Error, TEXT("%s would not take variable %s"), *Blueprint->GetName(), *AddVariable);
			return 1;
		}

		UE_LOG(LogBlueprintAddCueHandler, Display, TEXT("added %s, defaulting to %s"), *AddVariable, *VariableDefault);
	}

	/* Laid out well below whatever the graph already holds */
	int32 Below = 0;
	for (const UEdGraphNode* Node : EventGraph->Nodes) {
		Below = FMath::Max(Below, Node->NodePosY + 400);
	}

	bool bLinked = true;

	UK2Node_CustomEvent* EventNode = Place<UK2Node_CustomEvent>(EventGraph, FVector2D(0, Below));
	EventNode->CustomFunctionName = FName(*Tag);
	EventNode->FunctionFlags = Template->FunctionFlags;

	for (const TSharedPtr<FUserPinInfo>& Pin : Template->UserDefinedPins) {
		EventNode->UserDefinedPins.Add(MakeShared<FUserPinInfo>(*Pin));
	}

	EventNode->AllocateDefaultPins();

	/* Only an executed cue does anything, the same test the handlers beside it make */
	UK2Node_EnumInequality* NotExecuted = Place<UK2Node_EnumInequality>(EventGraph, FVector2D(250, Below + 150));
	NotExecuted->AllocateDefaultPins();

	bLinked &= Link(EventNode, TEXT("EventType"), NotExecuted, TEXT("A"));
	bLinked &= SetDefault(NotExecuted, TEXT("B"), TEXT("Executed"));

	UK2Node_IfThenElse* ExecutedBranch = Place<UK2Node_IfThenElse>(EventGraph, FVector2D(500, Below));
	ExecutedBranch->AllocateDefaultPins();

	bLinked &= Link(EventNode, UEdGraphSchema_K2::PN_Then, ExecutedBranch, UEdGraphSchema_K2::PN_Execute);
	bLinked &= Link(NotExecuted, UEdGraphSchema_K2::PN_ReturnValue, ExecutedBranch, UEdGraphSchema_K2::PN_Condition);

	/* Where the run stands, as the node and the way out it carries on from */
	UEdGraphNode* RunNode = ExecutedBranch;
	FName RunPin = UEdGraphSchema_K2::PN_Else;

	if (bSetBool) {
		UK2Node_VariableSet* SetNode = Place<UK2Node_VariableSet>(EventGraph, FVector2D(800, Below));
		SetNode->VariableReference.SetSelfMember(FName(*BoolVariable));
		SetNode->AllocateDefaultPins();

		bLinked &= Link(RunNode, RunPin, SetNode, UEdGraphSchema_K2::PN_Execute);
		bLinked &= SetDefault(SetNode, FName(*BoolVariable), TEXT("true"));
	} else {
		UFunction* IsDedicatedServer = UKismetSystemLibrary::StaticClass()->FindFunctionByName(TEXT("IsDedicatedServer"));
		UFunction* SpawnEmitterAttached = UGameplayStatics::StaticClass()->FindFunctionByName(TEXT("SpawnEmitterAttached"));

		if (!IsDedicatedServer || !SpawnEmitterAttached) {
			UE_LOG(LogBlueprintAddCueHandler, Error, TEXT("missing IsDedicatedServer or SpawnEmitterAttached"));
			return 1;
		}

		if (FParse::Param(*Params, TEXT("skipdedicated"))) {
			UK2Node_CallFunction* DedicatedNode = Place<UK2Node_CallFunction>(EventGraph, FVector2D(750, Below + 150));
			DedicatedNode->SetFromFunction(IsDedicatedServer);
			DedicatedNode->AllocateDefaultPins();

			UK2Node_IfThenElse* DedicatedBranch = Place<UK2Node_IfThenElse>(EventGraph, FVector2D(1000, Below));
			DedicatedBranch->AllocateDefaultPins();

			bLinked &= Link(RunNode, RunPin, DedicatedBranch, UEdGraphSchema_K2::PN_Execute);
			bLinked &= Link(DedicatedNode, UEdGraphSchema_K2::PN_ReturnValue, DedicatedBranch, UEdGraphSchema_K2::PN_Condition);

			RunNode = DedicatedBranch;
			RunPin = UEdGraphSchema_K2::PN_Else;
		}

		UK2Node_VariableGet* EffectNode = Place<UK2Node_VariableGet>(EventGraph, FVector2D(1000, Below + 250));
		EffectNode->VariableReference.SetSelfMember(FName(*SpawnVariable));
		EffectNode->AllocateDefaultPins();

		UK2Node_VariableGet* MeshNode = Place<UK2Node_VariableGet>(EventGraph, FVector2D(1000, Below + 350));
		MeshNode->VariableReference.SetSelfMember(TEXT("Mesh"));
		MeshNode->AllocateDefaultPins();

		UK2Node_CallFunction* SpawnNode = Place<UK2Node_CallFunction>(EventGraph, FVector2D(1300, Below));
		SpawnNode->SetFromFunction(SpawnEmitterAttached);
		SpawnNode->AllocateDefaultPins();

		bLinked &= Link(RunNode, RunPin, SpawnNode, UEdGraphSchema_K2::PN_Execute);
		bLinked &= Link(EffectNode, FName(*SpawnVariable), SpawnNode, TEXT("EmitterTemplate"));
		bLinked &= Link(MeshNode, TEXT("Mesh"), SpawnNode, TEXT("AttachToComponent"));

		/* Everything after the socket keeps the function's own defaults, which are what retail passes */
		if (FString Socket; FParse::Value(*Params, TEXT("socket="), Socket) && !Socket.IsEmpty()) {
			bLinked &= SetDefault(SpawnNode, TEXT("AttachPointName"), Socket);
		}
	}

	if (!bLinked) {
		UE_LOG(LogBlueprintAddCueHandler, Error, TEXT("%s: the nodes did not all wire up, nothing saved"), *Blueprint->GetName());
		return 1;
	}

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
	FKismetEditorUtilities::CompileBlueprint(Blueprint, EBlueprintCompileOptions::SkipGarbageCollection);

	if (Blueprint->Status == BS_Error) {
		UE_LOG(LogBlueprintAddCueHandler, Error, TEXT("%s did not compile, nothing saved"), *Blueprint->GetName());
		return 1;
	}

	UPackage* Package = Blueprint->GetOutermost();
	const FString FileName = FPackageName::LongPackageNameToFilename(Package->GetName(), FPackageName::GetAssetPackageExtension());

#if ENGINE_UE5
	FSavePackageArgs SaveArgs;
	SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;

	if (!UPackage::SavePackage(Package, nullptr, *FileName, SaveArgs)) {
#else
	if (!UPackage::SavePackage(Package, nullptr, RF_Public | RF_Standalone, *FileName)) {
#endif
		UE_LOG(LogBlueprintAddCueHandler, Error, TEXT("could not save %s"), *FileName);
		return 1;
	}

	UE_LOG(LogBlueprintAddCueHandler, Display, TEXT("%s now answers %s, saved %s"), *Blueprint->GetName(), *Tag, *FileName);
	return 0;
}
