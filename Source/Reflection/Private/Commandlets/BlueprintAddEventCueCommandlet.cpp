/* Copyright Reflection Contributors 2024-2026 */

#include "Commandlets/BlueprintAddEventCueCommandlet.h"

#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "K2Node_CallFunction.h"
#include "K2Node_CallParentFunction.h"
#include "K2Node_Event.h"
#include "K2Node_Self.h"
#include "K2Node_VariableGet.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/PackageName.h"
#include "Engine/Compatibility.h"
/* 5.0 gathered what a save is told into a header of its own */
#if ENGINE_UE5
#include "UObject/SavePackage.h"
#endif

DEFINE_LOG_CATEGORY_STATIC(LogBlueprintAddEventCue, Display, All);

namespace BlueprintAddEventCue {
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
			UE_LOG(LogBlueprintAddEventCue, Error, TEXT("no pin %s on %s or %s on %s"), *Out.ToString(), *From->GetName(), *In.ToString(), *To->GetName());
			return false;
		}

		return GetDefault<UEdGraphSchema_K2>()->TryCreateConnection(OutPin, InPin);
	}
}

int32 UBlueprintAddEventCueCommandlet::Main(const FString& Params) {
	using namespace BlueprintAddEventCue;

	FString Path, Event, Variable, Tag;
	if (!FParse::Value(*Params, TEXT("path="), Path) || !FParse::Value(*Params, TEXT("event="), Event) || !FParse::Value(*Params, TEXT("variable="), Variable) || !FParse::Value(*Params, TEXT("tag="), Tag)) {
		UE_LOG(LogBlueprintAddEventCue, Error, TEXT("usage: -run=BlueprintAddEventCue -path=/Game/Some/Blueprint -event=OnDeathPlayEffects -variable=\"Some Cue\" -tag=GameplayCue.Some.Cue"));
		return 1;
	}

	UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *Path);
	if (!Blueprint || !Blueprint->ParentClass) {
		UE_LOG(LogBlueprintAddEventCue, Error, TEXT("no blueprint at %s"), *Path);
		return 1;
	}

	const UFunction* Declares = Blueprint->ParentClass->FindFunctionByName(*Event);
	UFunction* MakeParameters = FindObject<UFunction>(nullptr, TEXT("/Script/GameplayAbilities.AbilitySystemBlueprintLibrary:MakeGameplayCueParameters"));
	UFunction* ExecuteCue = FindObject<UFunction>(nullptr, TEXT("/Script/FortniteGame.FortGameplayCueFunctionLibrary:ExecuteGameplayCueLocal"));
	UScriptStruct* GameplayTag = FindObject<UScriptStruct>(nullptr, TEXT("/Script/GameplayTags.GameplayTag"));

	if (!Declares || !MakeParameters || !ExecuteCue || !GameplayTag) {
		UE_LOG(LogBlueprintAddEventCue, Error, TEXT("missing %s on the parent, MakeGameplayCueParameters, ExecuteGameplayCueLocal or FGameplayTag"), *Event);
		return 1;
	}

	UEdGraph* EventGraph = FBlueprintEditorUtils::FindEventGraph(Blueprint);
	if (!EventGraph) {
		UE_LOG(LogBlueprintAddEventCue, Error, TEXT("%s has no event graph"), *Blueprint->GetName());
		return 1;
	}

	/* Run twice it would answer the event twice, which the blueprint cannot compile */
	for (const UEdGraphNode* Node : EventGraph->Nodes) {
		const UK2Node_Event* Existing = Cast<UK2Node_Event>(Node);

		if (Existing && Existing->bOverrideFunction && Existing->EventReference.GetMemberName() == FName(*Event)) {
			UE_LOG(LogBlueprintAddEventCue, Error, TEXT("%s already answers %s, nothing changed"), *Blueprint->GetName(), *Event);
			return 1;
		}
	}

	const FName VariableName(*Variable);

	if (FBlueprintEditorUtils::FindNewVariableIndex(Blueprint, VariableName) == INDEX_NONE) {
		FEdGraphPinType PinType;
		PinType.PinCategory = UEdGraphSchema_K2::PC_Struct;
		PinType.PinSubCategoryObject = GameplayTag;

		if (!FBlueprintEditorUtils::AddMemberVariable(Blueprint, VariableName, PinType, FString::Printf(TEXT("(TagName=\"%s\")"), *Tag))) {
			UE_LOG(LogBlueprintAddEventCue, Error, TEXT("%s would not take variable %s"), *Blueprint->GetName(), *Variable);
			return 1;
		}
	}

	/* Laid out well below whatever the graph already holds */
	int32 Below = 0;
	for (const UEdGraphNode* Node : EventGraph->Nodes) {
		Below = FMath::Max(Below, Node->NodePosY + 400);
	}

	UK2Node_Event* EventNode = Place<UK2Node_Event>(EventGraph, FVector2D(0, Below));
	EventNode->EventReference.SetExternalMember(FName(*Event), Declares->GetOwnerClass());
	EventNode->bOverrideFunction = true;
	EventNode->AllocateDefaultPins();

	bool bLinked = true;

	/* Where the run stands, as the node and the way out it carries on from */
	UEdGraphNode* RunNode = EventNode;
	FName RunPin = UEdGraphSchema_K2::PN_Then;

	/* An event a blueprint only implements has no parent to call */
	if (!FParse::Param(*Params, TEXT("noparent"))) {
		UK2Node_CallParentFunction* ParentNode = Place<UK2Node_CallParentFunction>(EventGraph, FVector2D(400, Below));
		ParentNode->SetFromFunction(Declares);
		ParentNode->AllocateDefaultPins();

		bLinked &= Link(EventNode, UEdGraphSchema_K2::PN_Then, ParentNode, UEdGraphSchema_K2::PN_Execute);

		/* The parent is handed everything the event was */
		for (UEdGraphPin* Pin : EventNode->Pins) {
			if (Pin->Direction == EGPD_Output && Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec && Pin->PinName != UK2Node_Event::DelegateOutputName) {
				bLinked &= Link(EventNode, Pin->PinName, ParentNode, Pin->PinName);
			}
		}

		RunNode = ParentNode;
	}

	/* A pawn that teleports in hides its mesh, children too, for a moment first, and shows it as the cue plays */
	if (float HideFor = 0.0f; FParse::Value(*Params, TEXT("hidemesh="), HideFor)) {
		UFunction* SetHidden = FindObject<UFunction>(nullptr, TEXT("/Script/Engine.SceneComponent:SetHiddenInGame"));
		UFunction* Delay = FindObject<UFunction>(nullptr, TEXT("/Script/Engine.KismetSystemLibrary:Delay"));

		if (!SetHidden || !Delay) {
			UE_LOG(LogBlueprintAddEventCue, Error, TEXT("missing SetHiddenInGame or Delay"));
			return 1;
		}

		const auto HideMesh = [&](const bool bHidden, const FVector2D& At) {
			UK2Node_VariableGet* MeshNode = Place<UK2Node_VariableGet>(EventGraph, At + FVector2D(0, 150));
			MeshNode->VariableReference.SetSelfMember(TEXT("Mesh"));
			MeshNode->AllocateDefaultPins();

			UK2Node_CallFunction* HideNode = Place<UK2Node_CallFunction>(EventGraph, At);
			HideNode->SetFromFunction(SetHidden);
			HideNode->AllocateDefaultPins();

			bLinked &= Link(MeshNode, TEXT("Mesh"), HideNode, UEdGraphSchema_K2::PN_Self);
			GetDefault<UEdGraphSchema_K2>()->TrySetDefaultValue(*HideNode->FindPin(TEXT("NewHidden"), EGPD_Input), bHidden ? TEXT("true") : TEXT("false"));
			GetDefault<UEdGraphSchema_K2>()->TrySetDefaultValue(*HideNode->FindPin(TEXT("bPropagateToChildren"), EGPD_Input), TEXT("true"));

			bLinked &= Link(RunNode, RunPin, HideNode, UEdGraphSchema_K2::PN_Execute);

			RunNode = HideNode;
			RunPin = UEdGraphSchema_K2::PN_Then;
		};

		HideMesh(true, FVector2D(400, Below - 200));

		UK2Node_CallFunction* DelayNode = Place<UK2Node_CallFunction>(EventGraph, FVector2D(700, Below - 200));
		DelayNode->SetFromFunction(Delay);
		DelayNode->AllocateDefaultPins();
		GetDefault<UEdGraphSchema_K2>()->TrySetDefaultValue(*DelayNode->FindPin(TEXT("Duration"), EGPD_Input), LexToString(HideFor));

		bLinked &= Link(RunNode, RunPin, DelayNode, UEdGraphSchema_K2::PN_Execute);

		/* A latent call carries on from Completed once the time is up */
		RunNode = DelayNode;
		RunPin = UEdGraphSchema_K2::PN_Then;

		HideMesh(false, FVector2D(1000, Below - 200));
	}

	UK2Node_CallFunction* MakeNode = Place<UK2Node_CallFunction>(EventGraph, FVector2D(400, Below + 300));
	MakeNode->SetFromFunction(MakeParameters);
	MakeNode->AllocateDefaultPins();

	UK2Node_Self* SelfNode = Place<UK2Node_Self>(EventGraph, FVector2D(700, Below + 200));
	SelfNode->AllocateDefaultPins();

	UK2Node_VariableGet* TagNode = Place<UK2Node_VariableGet>(EventGraph, FVector2D(700, Below + 250));
	TagNode->VariableReference.SetSelfMember(VariableName);
	TagNode->AllocateDefaultPins();

	UK2Node_CallFunction* ExecuteNode = Place<UK2Node_CallFunction>(EventGraph, FVector2D(1300, Below));
	ExecuteNode->SetFromFunction(ExecuteCue);
	ExecuteNode->AllocateDefaultPins();

	bLinked &= Link(RunNode, RunPin, ExecuteNode, UEdGraphSchema_K2::PN_Execute);
	bLinked &= Link(SelfNode, UEdGraphSchema_K2::PN_Self, ExecuteNode, TEXT("Target"));
	bLinked &= Link(TagNode, VariableName, ExecuteNode, TEXT("GameplayCueTag"));
	bLinked &= Link(MakeNode, UEdGraphSchema_K2::PN_ReturnValue, ExecuteNode, TEXT("CueParameters"));

	if (UEdGraphPin* OnlyNotifies = ExecuteNode->FindPin(TEXT("bOnlyNotifies"), EGPD_Input)) {
		GetDefault<UEdGraphSchema_K2>()->TrySetDefaultValue(*OnlyNotifies, TEXT("true"));
	}

	if (!bLinked) {
		UE_LOG(LogBlueprintAddEventCue, Error, TEXT("%s: the nodes did not all wire up, nothing saved"), *Blueprint->GetName());
		return 1;
	}

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
	FKismetEditorUtilities::CompileBlueprint(Blueprint, EBlueprintCompileOptions::SkipGarbageCollection);

	if (Blueprint->Status == BS_Error) {
		UE_LOG(LogBlueprintAddEventCue, Error, TEXT("%s did not compile, nothing saved"), *Blueprint->GetName());
		return 1;
	}

	UPackage* Package = Blueprint->GetOutermost();
	const FString FileName = FPackageName::LongPackageNameToFilename(Package->GetName(), FPackageName::GetAssetPackageExtension());

	/* 5.0 gathered what a save is told into one struct. Before that it is told the same
	 * things one argument at a time. */
#if ENGINE_UE5
	FSavePackageArgs SaveArgs;
	SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;

	if (!UPackage::SavePackage(Package, nullptr, *FileName, SaveArgs)) {
#else
	if (!UPackage::SavePackage(Package, nullptr, RF_Public | RF_Standalone, *FileName)) {
#endif
		UE_LOG(LogBlueprintAddEventCue, Error, TEXT("could not save %s"), *FileName);
		return 1;
	}

	UE_LOG(LogBlueprintAddEventCue, Display, TEXT("%s now answers %s with %s, saved %s"), *Blueprint->GetName(), *Event, *Tag, *FileName);
	return 0;
}
