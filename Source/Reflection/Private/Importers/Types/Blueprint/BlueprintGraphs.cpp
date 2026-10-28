/* Copyright Reflection Contributors 2024-2026 */

#include "Importers/Types/Blueprint/BlueprintGraphs.h"
#include "AnimationGraphSchema.h"
#include "AnimationGraph.h"
/* Renamed from the sub input node in 4.24 */
#if UE4_23_BELOW
#include "AnimGraphNode_SubInput.h"
#else
#include "AnimGraphNode_LinkedInputPose.h"
#endif
#include "Importers/Types/Blueprint/BlueprintVariables.h"
#include "Importers/Types/Blueprint/MacroPattern.h"

#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_Event.h"
#include "Importers/Types/Blueprint/BytecodeGraph.h"
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


	/* How a parameter is handed over. A struct or interface the game passes as const& is a reference
	 * pin here too, or a delegate bound to it no longer matches and the compiler wraps it in a proxy */
	void PassedAs(const TSharedPtr<FJsonObject>& Parameter, FEdGraphPinType& Type) {
		const FString Spelled = Parameter->GetStringField(TEXT("PropertyFlags"));

		Type.bIsReference = Spelled.Contains(TEXT("ReferenceParm"));
		Type.bIsConst = Spelled.Contains(TEXT("ConstParm"));
	}

	/* Puts a node in a graph, the way the editor would have put it there */
	template <typename T>
	T* Place(UEdGraph* Graph) {
		/* Transactional like a node the editor placed, or the next open flags them all and asks for a resave */
		T* Node = NewObject<T>(Graph, T::StaticClass(), NAME_None, RF_Transactional);

		Graph->AddNode(Node, false, false);

		Node->CreateNewGuid();
		Node->PostPlacedNewNode();

		return Node;
	}

	/* The name an event node answers to, however it came to be one. */
	FName CalledBy(const UK2Node_Event* Event) {
		if (!Event->bOverrideFunction && !Event->CustomFunctionName.IsNone()) {
			return Event->CustomFunctionName;
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
	bool bOut = false;
	bool bByReference = false;
	bool bReturned = false;

	for (FString& Flag : Named) {
		Flag.TrimStartAndEndInline();

		/* Read whole rather than looked for inside another, since a value the compiler holds onto
		 * is a ConstParm and is nobody's parameter */
		if (Flag == TEXT("Parm")) bParameter = true;
		if (Flag == TEXT("OutParm")) bOut = true;
		if (Flag == TEXT("ReferenceParm")) bByReference = true;
		if (Flag == TEXT("ReturnParm")) bReturned = true;
	}

	/* Out and by reference together is something handed in, not handed back.
	 *
	 * A parameter taken as a const reference is marked out because that is how the compiler passes
	 * it, by address rather than by copy, and the caller writes the address in. It is still the
	 * caller telling the function something. Read as an output it disappears off the front of the
	 * node and the event ends up with one pin where the game had two, which is a different function
	 * to anything that calls it.
	 *
	 * This is the engine's own rule, from K2Node_FunctionEntry: given back only where it is out and
	 * not by reference, or where it is the return. */
	bOutGivenBack = bReturned || (bOut && !bByReference);

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

	/* The one graph everything a blueprint runs was written into.
	 *
	 * It says so in its flags, where the flags were kept. Not every build writes them out some
	 * come back saying nothing at all and read without them the ubergraph is taken for an
	 * ordinary function: a graph is made under its name, which the class already has one of, and
	 * the blueprint will not compile for having the same function twice. Every event that was
	 * written in it is left with nowhere to go besides.
	 *
	 * So the name settles it where the flags do not. The compiler builds that name rather than
	 * anybody typing it, and it is the engine's own, asked for here rather than spelled out. */
	FString Named;

	if (Function.JsonObject.IsValid()) {
		Function.JsonObject->TryGetStringField(TEXT("Name"), Named);
	}

	if (Says(Function, TEXT("FUNC_UbergraphFunction")) || Named.StartsWith(UEdGraphSchema_K2::FN_ExecuteUbergraphBase.ToString())) {
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

UEdGraph* FBlueprintGraphs::MakeLayer(UBlueprint* Blueprint, const FString& Name, const TArray<TSharedPtr<FJsonValue>>& Declared) {
	if (Blueprint == nullptr || Name.IsEmpty()) return nullptr;

	UEdGraph* Graph = FBlueprintEditorUtils::CreateNewGraph(Blueprint, FName(*Name), UAnimationGraph::StaticClass(), UAnimationGraphSchema::StaticClass());

	if (Graph == nullptr) return nullptr;

	/* Added the way the editor adds a layer, which fills the graph in once.
	 *
	 * Added as a function instead it is filled in twice: once for the graph, which is where an
	 * animation graph gets its output pose, and again for the signature. The second one makes a
	 * second output pose, and the schema insists there is only ever one. */
	FBlueprintEditorUtils::AddDomainSpecificGraph(Blueprint, Graph);

	TArray<TSharedPtr<FJsonObject>> Takes;
	TArray<TSharedPtr<FJsonObject>> GivesBack;

	ReadSignature(Declared, Takes, GivesBack);

	/* What a layer takes is not pins on an entry node.
	 *
	 * Each pose it takes is a node of its own standing for the pose whoever calls it hands in, and
	 * anything else it takes is drawn on that node. The compiler reads a layer's signature back the
	 * same way round a node, then whatever else that node carries so a value belongs with the
	 * pose it was declared after, and they come out in the order they went in. */
	UAnimGraphNode_LinkedInputPose* Handing = nullptr;

	int32 Which = 0;

	for (const TSharedPtr<FJsonObject>& Parameter : Takes) {
		FEdGraphPinType Type;

		if (!FBlueprintVariables::GetPinType(Parameter, Type)) continue;

		const FName Named(*Parameter->GetStringField(TEXT("Name")));

		if (UAnimationGraphSchema::IsPosePin(Type)) {
			Handing = NewObject<UAnimGraphNode_LinkedInputPose>(Graph);

			Graph->AddNode(Handing, false, false);

			Handing->CreateNewGuid();
			Handing->PostPlacedNewNode();

			/* 4.23 is where the node grew a name, a place in the order and inputs of its own.
			 * Before that it carries a pose and nothing else, so there is nothing to say. */
#if !UE4_22_BELOW
			Handing->Node.Name = Named;
			Handing->InputPoseIndex = Which;
#endif
			Which++;

			Handing->AllocateDefaultPins();

			continue;
		}

		/* A value handed in before any pose has nowhere to be drawn */
		if (Handing == nullptr) continue;

#if !UE4_22_BELOW
		Handing->Inputs.Add(FAnimBlueprintFunctionPinInfo(Named, Type));
#endif
	}

	/* Laid out again once, since each of them grows a pin for everything it carries */
#if !UE4_22_BELOW
	for (UEdGraphNode* Node : Graph->Nodes) {
		if (UAnimGraphNode_LinkedInputPose* Takes_Pose = Cast<UAnimGraphNode_LinkedInputPose>(Node); Takes_Pose != nullptr && Takes_Pose->Inputs.Num() > 0) {
			Takes_Pose->ReconstructNode();
		}
	}
#endif

	return Graph;
}

UEdGraph* FBlueprintGraphs::Make(UBlueprint* Blueprint, const FString& Name, const FUObjectJsonValueExport& Function, const TArray<TSharedPtr<FJsonValue>>& Declared) {
	if (Blueprint == nullptr || Name.IsEmpty()) return nullptr;

	/* A function of the blueprint's own that a native parent also has under that name, callable but
	 * not overridable. The cooked asset keeps both, but this compiler refuses a function whose name the parent
	 * already uses, and refusing it fails the whole class. So the blueprint's one is made under a name
	 * of its own, and its callers are pointed at it by FBytecodeGraph::FindFunctionOn. */
	FString Making = Name;

	if (!Function.Has(TEXT("SuperStruct")) && Blueprint->ParentClass != nullptr) {
		if (const UFunction* Taken = Blueprint->ParentClass->FindFunctionByName(*Name); Taken != nullptr && !Taken->HasAnyFunctionFlags(FUNC_BlueprintEvent)) {
			Making = Name + FBlueprintGraphs::ClashSuffix;

			UE_LOG(LogTemp, Warning, TEXT("\"%s\" is a name %s already uses for a function of its own, so the blueprint's is made as \"%s\""),
				*Name, *Taken->GetOwnerClass()->GetName(), *Making);
		}
	}

	UEdGraph* Graph = FBlueprintEditorUtils::CreateNewGraph(Blueprint, FName(*Making), UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());

	if (Graph == nullptr) return nullptr;


	/* Answering one that already exists is that function's signature rather than a new one, and the
	 * entry node is laid out from it.
	 *
	 * Which one it answers is said outright: a function that overrides another carries it as its
	 * super struct, naming the class it was declared on and what it is called there. Looked for on
	 * the parent by name alone instead, one declared somewhere the parent chain does not reach in
	 * this engine comes back as nothing, and what gets made is a new function under a name the
	 * class already has: the compiler then has two of them, disagreeing about their parameters and
	 * about who may call them, and says so four times over.
	 *
	 * Not user created, whatever else it is. That is what tells the editor this is an override, and
	 * a graph marked as made by hand is given public access on top of whatever the parent said,
	 * which the compiler refuses against a parent that is protected. */
	UFunction* Answered = nullptr;

	if (Function.Has(TEXT("SuperStruct"))) {
		Answered = FBytecodeGraph::ResolveFunction(Function.GetObject(TEXT("SuperStruct")));
	}

	if (Answered == nullptr && Blueprint->ParentClass != nullptr) {
		Answered = Blueprint->ParentClass->FindFunctionByName(*Name);

		/* Only something a blueprint may override. A parent can carry a plain callable of the same
		 * name the blueprint's own function was written beside, and taken as an
		 * override it is refused for not being one */
		if (Answered != nullptr && !Answered->HasAnyFunctionFlags(FUNC_BlueprintEvent)) {
			Answered = nullptr;
		}
	}

	/* Said as the class it was declared on rather than as the function.
	 *
	 * Both are taken, and they do not mean the same thing. Given the function, the entry names it
	 * as one of the blueprint's own and the reference is left without a class behind it; the
	 * compiler then goes looking for anything of that name on the parent, finds the one being
	 * overridden, and calls the name taken. Given the class, the entry names it as that class's,
	 * which is what an override is and what the editor's own Override Function hands over. */
	if (Answered != nullptr) {
		FBlueprintEditorUtils::AddFunctionGraph<UClass>(Blueprint, Graph, false, Answered->GetOwnerClass());

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

		PassedAs(Parameter, Type);

		Entry->CreateUserDefinedPin(FName(*Parameter->GetStringField(TEXT("Name"))), Type, EGPD_Output);
	}

	if (GivesBack.Num() == 0) return Graph;

	UK2Node_FunctionResult* Result = FBlueprintEditorUtils::FindOrCreateFunctionResultNode(Entry);

	if (Result == nullptr) return Graph;

	for (const TSharedPtr<FJsonObject>& Parameter : GivesBack) {
		FEdGraphPinType Type;

		if (!FBlueprintVariables::GetPinType(Parameter, Type)) continue;

		PassedAs(Parameter, Type);

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

	/* Or something an interface the blueprint answers for declares, reached through the interface.
	 *
	 * An interface function that hands nothing back is never given a graph: implementing the
	 * interface leaves one for everything that returns something and nothing at all for the rest,
	 * because the blueprint is meant to answer those with an event.
	 *
	 * Read off the parent alone there is nothing declaring it the parent does not, the interface
	 * does so it is taken for something the blueprint made up and written as an event of its
	 * own. That is a second function under a name the class already carries from the interface, and
	 * the blueprint stops compiling: the name is spoken for. */
	for (const FBPInterfaceDescription& Implemented : Blueprint->ImplementedInterfaces) {
		const UClass* Interface = Implemented.Interface;
		const UFunction* Declares = Interface != nullptr ? Interface->FindFunctionByName(FName(*Name)) : nullptr;

		if (Declares == nullptr) continue;

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

		PassedAs(Parameter, Type);

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

			PassedAs(Parameter, Type);

			Entry->CreateUserDefinedPin(FName(*Parameter->GetStringField(TEXT("Name"))), Type, EGPD_Output);
		}
	}

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);

	return Graph;
}
