/* Copyright Reflection Contributors 2024-2026 */

#include "Importers/Types/Blueprint/BlueprintGraphs.h"
#include "Importers/Types/Blueprint/BlueprintVariables.h"
#include "Importers/Types/Blueprint/MacroPattern.h"

#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_Event.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_FunctionResult.h"
#include "Kismet2/BlueprintEditorUtils.h"

namespace {
	/* Whether a function says something about itself, the way the asset spells it */
	bool Says(const FUObjectJsonValueExport& Function, const TCHAR* Flag) {
		return Function.Has(TEXT("FunctionFlags")) && Function.GetString(TEXT("FunctionFlags")).Contains(Flag);
	}

	/* The parameters a function takes and gives back, in the order it declares them */
	void ReadSignature(const TArray<TSharedPtr<FJsonValue>>& Declared, TArray<TSharedPtr<FJsonObject>>& Takes, TArray<TSharedPtr<FJsonObject>>& GivesBack) {
		for (const TSharedPtr<FJsonValue>& Value : Declared) {
			const TSharedPtr<FJsonObject> Property = Value.IsValid() ? Value->AsObject() : nullptr;

			bool bGivenBack = false;

			if (!FBlueprintGraphs::IsParameter(Property, bGivenBack)) continue;

			(bGivenBack ? GivesBack : Takes).Add(Property);
		}
	}


	/* Puts a node in a graph, the way the editor would have put it there */
	template <typename T>
	T* Place(UEdGraph* Graph) {
		T* Node = NewObject<T>(Graph);

		Graph->AddNode(Node, false, false);

		Node->CreateNewGuid();
		Node->PostPlacedNewNode();

		return Node;
	}

	/* The name an event node answers to, however it came to be one */
	FName CalledBy(const UK2Node_Event* Event) {
		if (const UK2Node_CustomEvent* Custom = Cast<UK2Node_CustomEvent>(Event)) {
			return Custom->CustomFunctionName;
		}

		return Event->EventReference.GetMemberName();
	}
}

bool FBlueprintGraphs::IsParameter(const TSharedPtr<FJsonObject>& Property, bool& bOutGivenBack) {
	bOutGivenBack = false;

	FString Spelled;

	if (!Property.IsValid() || !Property->TryGetStringField(TEXT("PropertyFlags"), Spelled)) return false;

	TArray<FString> Named;
	Spelled.ParseIntoArray(Named, TEXT("|"), true);

	bool bParameter = false;

	for (FString& Flag : Named) {
		Flag.TrimStartAndEndInline();

		/* Read whole rather than looked for inside another, since a value the compiler holds onto
		 * is a ConstParm and is nobody's parameter */
		if (Flag == TEXT("Parm")) bParameter = true;
		if (Flag == TEXT("OutParm") || Flag == TEXT("ReturnParm")) bOutGivenBack = true;
	}

	return bParameter;
}

UK2Node_FunctionEntry* FBlueprintGraphs::EntryOf(const UEdGraph* Graph) {
	for (UEdGraphNode* Node : Graph->Nodes) {
		if (UK2Node_FunctionEntry* Entry = Cast<UK2Node_FunctionEntry>(Node)) return Entry;
	}

	return nullptr;
}

FBlueprintGraphs::FWritten FBlueprintGraphs::Reads(const FUObjectJsonValueExport& Function) {
	FWritten Written;

	if (Says(Function, TEXT("FUNC_UbergraphFunction"))) {
		Written.Kind = EWritten::Ubergraph;

		return Written;
	}

	/* An event's body is not in the event's own function. What is there is a call into the
	 * ubergraph saying where to begin, and nothing else it does is work of its own: an event that
	 * takes anything copies what it was given into the frame the ubergraph reads from first. So a
	 * function that only hands its arguments over and says where to begin is an event, and the
	 * number it passes is where its body starts. */
	const TArray<FUObjectJsonValueExport> Statements = Function.Has(TEXT("ScriptBytecode")) ? Function.GetArray(TEXT("ScriptBytecode")) : TArray<FUObjectJsonValueExport>();

	int32 Entered = INDEX_NONE;

	for (const FUObjectJsonValueExport& Statement : Statements) {
		const FString Token = MacroReading::TokenOf(Statement);

		/* Handing an argument over to the frame the ubergraph reads it from */
		if (Token == TEXT("EX_LetValueOnPersistentFrame")) {
			const FString Kept = MacroReading::NamedProperty(Statement.GetObject(TEXT("DestinationProperty")));
			const FString Given = MacroReading::ReadFrom(Statement.GetObject(TEXT("AssignmentExpression")));

			if (!Kept.IsEmpty() && !Given.IsEmpty()) {
				Written.Frame.Add(Kept, Given);
			}

			continue;
		}

		/* The end of it, which says nothing either way */
		if (Token == TEXT("EX_Return") || Token == TEXT("EX_Nothing") || Token == TEXT("EX_EndOfScript")) continue;

		/* Anything other than the one call into the ubergraph is work of its own, and a function
		 * that does work of its own was written as a function */
		if (Entered != INDEX_NONE) return Written;

		if (!MacroReading::CallsTo(Statement).StartsWith(TEXT("ExecuteUbergraph"))) return Written;

		const TArray<FUObjectJsonValueExport> Arguments = Statement.Has(TEXT("Parameters")) ? Statement.GetArray(TEXT("Parameters")) : TArray<FUObjectJsonValueExport>();

		if (Arguments.Num() != 1 || MacroReading::TokenOf(Arguments[0]) != TEXT("EX_IntConst")) return Written;

		Entered = Arguments[0].GetInteger(TEXT("Value"), INDEX_NONE);
	}

	if (Entered == INDEX_NONE) return Written;

	Written.Kind = EWritten::Event;
	Written.EntryPoint = Entered;

	return Written;
}

UEdGraph* FBlueprintGraphs::Events(UBlueprint* Blueprint) {
	if (Blueprint == nullptr) return nullptr;

	for (UEdGraph* Page : Blueprint->UbergraphPages) {
		if (Page != nullptr) return Page;
	}

	return nullptr;
}

UEdGraph* FBlueprintGraphs::Make(UBlueprint* Blueprint, const FString& Name, const FUObjectJsonValueExport& Function, const TArray<TSharedPtr<FJsonValue>>& Declared) {
	if (Blueprint == nullptr || Name.IsEmpty()) return nullptr;

	UEdGraph* Graph = FBlueprintEditorUtils::CreateNewGraph(Blueprint, FName(*Name), UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());

	if (Graph == nullptr) return nullptr;

	/* Answering one the parent already declares is that function's signature rather than a new one,
	 * and the entry node is laid out from it */
	if (UFunction* Answered = Blueprint->ParentClass != nullptr ? Blueprint->ParentClass->FindFunctionByName(*Name) : nullptr) {
		FBlueprintEditorUtils::AddFunctionGraph<UFunction>(Blueprint, Graph, true, Answered);

		return Graph;
	}

	FBlueprintEditorUtils::AddFunctionGraph<UClass>(Blueprint, Graph, true, static_cast<UClass*>(nullptr));

	UK2Node_FunctionEntry* Entry = EntryOf(Graph);

	if (Entry == nullptr) return Graph;

	/* Said the way the game said it. What a function is callable as is part of what it is, and a
	 * pure one has no run through it at all. */
	int32 Extra = 0;

	if (Says(Function, TEXT("FUNC_BlueprintPure"))) Extra |= FUNC_BlueprintPure;
	if (Says(Function, TEXT("FUNC_Const"))) Extra |= FUNC_Const;

	if (Extra != 0) {
		Entry->AddExtraFlags(Extra);
	}

	/* Who may call it, which is one answer and not several.
	 *
	 * A new entry node is public to begin with, so saying it is private on top of that leaves it
	 * saying both, and the compiler refuses a function that cannot say which it is. What it was is
	 * put in place of what it started as rather than added to it. */
	int32 Access = FUNC_Public;

	if (Says(Function, TEXT("FUNC_Private"))) Access = FUNC_Private;
	else if (Says(Function, TEXT("FUNC_Protected"))) Access = FUNC_Protected;

	Entry->SetExtraFlags((Entry->GetExtraFlags() & ~(FUNC_Public | FUNC_Protected | FUNC_Private)) | Access);

	TArray<TSharedPtr<FJsonObject>> Takes;
	TArray<TSharedPtr<FJsonObject>> GivesBack;

	ReadSignature(Declared, Takes, GivesBack);

	/* What a function takes comes out of its entry, since that is where the run hands it over */
	for (const TSharedPtr<FJsonObject>& Parameter : Takes) {
		FEdGraphPinType Type;

		if (!FBlueprintVariables::GetPinType(Parameter, Type)) continue;

		Entry->CreateUserDefinedPin(FName(*Parameter->GetStringField(TEXT("Name"))), Type, EGPD_Output);
	}

	if (GivesBack.Num() == 0) return Graph;

	UK2Node_FunctionResult* Result = FBlueprintEditorUtils::FindOrCreateFunctionResultNode(Entry);

	if (Result == nullptr) return Graph;

	for (const TSharedPtr<FJsonObject>& Parameter : GivesBack) {
		FEdGraphPinType Type;

		if (!FBlueprintVariables::GetPinType(Parameter, Type)) continue;

		Result->CreateUserDefinedPin(FName(*Parameter->GetStringField(TEXT("Name"))), Type, EGPD_Input);
	}

	return Graph;
}

int32 FBlueprintGraphs::RemoveUnwrittenEvents(UEdGraph* EventGraph, const TSet<FString>& Written) {
	if (EventGraph == nullptr) return 0;

	int32 Taken = 0;

	for (int32 Index = EventGraph->Nodes.Num() - 1; Index >= 0; --Index) {
		UK2Node_Event* Event = Cast<UK2Node_Event>(EventGraph->Nodes[Index]);

		/* Only the ones the editor offered: anything the blueprint made up itself was made here */
		if (Event == nullptr || !Event->bOverrideFunction) continue;

		if (Written.Contains(CalledBy(Event).ToString())) continue;

		EventGraph->RemoveNode(Event);

		Taken++;
	}

	return Taken;
}

UK2Node* FBlueprintGraphs::MakeEvent(UBlueprint* Blueprint, UEdGraph* EventGraph, const FString& Name, const TArray<TSharedPtr<FJsonValue>>& Declared) {
	if (Blueprint == nullptr || EventGraph == nullptr || Name.IsEmpty()) return nullptr;

	/* One the graph already carries, which is how a blueprint made from an actor arrives: the
	 * events its parent offers are put there waiting to be used */
	for (UEdGraphNode* Node : EventGraph->Nodes) {
		UK2Node_Event* Event = Cast<UK2Node_Event>(Node);

		if (Event != nullptr && CalledBy(Event) == FName(*Name)) return Event;
	}

	/* Answering something the parent declares is that event, reached through the parent */
	if (const UFunction* Declares = Blueprint->ParentClass != nullptr ? Blueprint->ParentClass->FindFunctionByName(*Name) : nullptr) {
		UK2Node_Event* Event = Place<UK2Node_Event>(EventGraph);

		Event->EventReference.SetExternalMember(FName(*Name), Declares->GetOwnerClass());
		Event->bOverrideFunction = true;

		Event->AllocateDefaultPins();

		return Event;
	}

	/* Anything else the blueprint made up itself, which carries its own parameters */
	UK2Node_CustomEvent* Event = Place<UK2Node_CustomEvent>(EventGraph);

	Event->CustomFunctionName = FName(*Name);
	Event->bIsEditable = true;

	Event->AllocateDefaultPins();

	TArray<TSharedPtr<FJsonObject>> Takes;
	TArray<TSharedPtr<FJsonObject>> GivesBack;

	ReadSignature(Declared, Takes, GivesBack);

	for (const TSharedPtr<FJsonObject>& Parameter : Takes) {
		FEdGraphPinType Type;

		if (!FBlueprintVariables::GetPinType(Parameter, Type)) continue;

		Event->CreateUserDefinedPin(FName(*Parameter->GetStringField(TEXT("Name"))), Type, EGPD_Output);
	}

	return Event;
}

UEdGraph* FBlueprintGraphs::MakeDispatcher(UBlueprint* Blueprint, const FString& Name, const TArray<TSharedPtr<FJsonValue>>& Declared) {
	if (Blueprint == nullptr || Name.IsEmpty()) return nullptr;

	/* One it already keeps, which is how a second import arrives */
	for (UEdGraph* Kept : Blueprint->DelegateSignatureGraphs) {
		if (Kept != nullptr && Kept->GetName() == Name) return Kept;
	}

	/* A dispatcher is two things, and one of them is a variable.
	 *
	 * The class carries the delegate itself the way it carries any other value, and the graph below
	 * only says what whoever binds to it will be handed. Made without the variable, there is nothing
	 * to bind to and nothing to broadcast: the class simply has no dispatcher of that name, however
	 * complete the graph beside it looks. */
	if (FBlueprintEditorUtils::FindNewVariableIndex(Blueprint, FName(*Name)) == INDEX_NONE) {
		FEdGraphPinType Carried;
		Carried.PinCategory = UEdGraphSchema_K2::PC_MCDelegate;

		if (!FBlueprintEditorUtils::AddMemberVariable(Blueprint, FName(*Name), Carried)) return nullptr;
	}

	UEdGraph* Graph = FBlueprintEditorUtils::CreateNewGraph(Blueprint, FName(*Name), UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());

	if (Graph == nullptr) {
		FBlueprintEditorUtils::RemoveMemberVariable(Blueprint, FName(*Name));

		return nullptr;
	}

	/* Made the way the editor makes one. A dispatcher is a graph that is never opened: it is there
	 * to say what those who bind to it are handed, and nothing else. */
	const UEdGraphSchema_K2* Schema = GetDefault<UEdGraphSchema_K2>();

	Graph->bEditable = false;

	Schema->CreateDefaultNodesForGraph(*Graph);
	Schema->CreateFunctionGraphTerminators(*Graph, static_cast<UClass*>(nullptr));
	Schema->AddExtraFunctionFlags(Graph, FUNC_BlueprintCallable | FUNC_BlueprintEvent | FUNC_Public);
	Schema->MarkFunctionEntryAsEditable(Graph, true);

	Blueprint->DelegateSignatureGraphs.Add(Graph);

	/* What it hands over, which is what its signature declares */
	if (UK2Node_FunctionEntry* Entry = EntryOf(Graph)) {
		TArray<TSharedPtr<FJsonObject>> Takes;
		TArray<TSharedPtr<FJsonObject>> GivesBack;

		ReadSignature(Declared, Takes, GivesBack);

		for (const TSharedPtr<FJsonObject>& Parameter : Takes) {
			FEdGraphPinType Type;

			if (!FBlueprintVariables::GetPinType(Parameter, Type)) continue;

			Entry->CreateUserDefinedPin(FName(*Parameter->GetStringField(TEXT("Name"))), Type, EGPD_Output);
		}
	}

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);

	return Graph;
}
