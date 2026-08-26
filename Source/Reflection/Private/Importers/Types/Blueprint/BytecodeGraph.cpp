/* Copyright Reflection Contributors 2024-2026 */

#include "Importers/Types/Blueprint/BytecodeGraph.h"

#include "EdGraphSchema_K2.h"
#include "Engine/Compatibility.h"
#include "K2Node_AddComponent.h"
#include "Importers/Types/Blueprint/GraphTidy.h"
#include "Importers/Types/Blueprint/MacroPattern.h"
#include "K2Node_BreakStruct.h"
#include "K2Node_MacroInstance.h"
#include "K2Node_MacroInstance.h"
#include "K2Node_CallFunction.h"
#include "K2Node_DynamicCast.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_FunctionResult.h"
#include "K2Node_Event.h"
#include "K2Node_ExecutionSequence.h"
#include "K2Node_IfThenElse.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Components/ActorComponent.h"
#include "Containers/ExportContainer.h"
#include "Importers/Constructor/Asset.h"
#include "Importers/Constructor/ImportIssues.h"
#include "Utilities/AssetPaths.h"
#include "Engine/Package.h"
#include "UObject/CoreRedirects.h"
#include "Importers/Types/Blueprint/BlueprintGraphs.h"
#include "Importers/Types/Blueprint/BlueprintVariables.h"
#include "K2Node_CommutativeAssociativeBinaryOperator.h"
#include "K2Node_CallMaterialParameterCollectionFunction.h"
#include "K2Node_CallDataTableFunction.h"
#include "K2Node_CallArrayFunction.h"
#include "Kismet/KismetMathLibrary.h"
#include "K2Node_AddDelegate.h"
#include "K2Node_RemoveDelegate.h"
#include "K2Node_CallDelegate.h"
#include "K2Node_CreateDelegate.h"
#include "K2Node_BaseMCDelegate.h"
#include "K2Node_MakeArray.h"
#include "K2Node_Select.h"
#include "K2Node_Timeline.h"
#include "K2Node_Self.h"
#include "K2Node_VariableGet.h"
#include "K2Node_VariableSet.h"

DECLARE_LOG_CATEGORY_CLASS(LogReflectionBytecode, All, All);

namespace {
	/* An object reference, as the two halves the bytecode spells it in: a name that carries the
	 * class and the member, and a path that carries the package they live in */
	void SplitReference(const FUObjectJsonValueExport& Reference, FString& OutOwner, FString& OutMember) {
		FString Name = Reference.Has(TEXT("ObjectName")) ? Reference.GetString(TEXT("ObjectName")) : FString();

		/* Class'Owner:Member' */
		if (Name.Contains(TEXT("'"))) {
			Name.Split(TEXT("'"), nullptr, &Name);
			Name.Split(TEXT("'"), &Name, nullptr, ESearchCase::CaseSensitive, ESearchDir::FromEnd);
		}

		if (!Name.Split(TEXT(":"), &OutOwner, &OutMember)) {
			OutOwner = Name;
			OutMember = FString();
		}
	}

	/* The name a local carries when the compiler made it for a pin rather than the graph declaring
	 * it. They read as the node and the pin they came from, with a number where more than one net
	 * wanted the same name. */
	/* A name the compiler made for a pin rather than one the graph declared.
	 *
	 * Temp_<kind>_Variable is what UK2Node_TemporaryVariable calls itself, so it belongs to a macro
	 * rather than to the function. Whether it can be dropped is not settled by the name though: it
	 * can only be dropped once some macro has been recognised and claimed it. Read statement by
	 * statement, or where nothing matched the shape, there is no macro to own it and it has to be a
	 * local like any other, or the loop it counts has nothing to count with. */
	bool IsCompilerLocal(const FString& Name) {
		return Name.StartsWith(TEXT("CallFunc_")) || Name.StartsWith(TEXT("K2Node_"));
	}

	/* What a property is called in the graph, for declaring a local of the same kind */
	bool TypeOfProperty(const FUObjectJsonValueExport& Property, FEdGraphPinType& OutType) {
		if (!Property.Has(TEXT("Type"))) return false;

		const FString Kind = Property.GetString(TEXT("Type"));

		if (Kind == TEXT("IntProperty")) OutType.PinCategory = UEdGraphSchema_K2::PC_Int;
		else if (Kind == TEXT("Int64Property")) OutType.PinCategory = UEdGraphSchema_K2::PC_Int64;
		else if (Kind == TEXT("BoolProperty")) OutType.PinCategory = UEdGraphSchema_K2::PC_Boolean;
		else if (Kind == TEXT("FloatProperty") || Kind == TEXT("DoubleProperty")) {
			OutType.PinCategory = UEdGraphSchema_K2::PC_Real;
			OutType.PinSubCategory = Kind == TEXT("FloatProperty") ? UEdGraphSchema_K2::PC_Float : UEdGraphSchema_K2::PC_Double;
		}
		else if (Kind == TEXT("ByteProperty")) OutType.PinCategory = UEdGraphSchema_K2::PC_Byte;
		else if (Kind == TEXT("NameProperty")) OutType.PinCategory = UEdGraphSchema_K2::PC_Name;
		else if (Kind == TEXT("StrProperty")) OutType.PinCategory = UEdGraphSchema_K2::PC_String;
		else if (Kind == TEXT("StructProperty")) {
			/* A struct says which one it is, and a pin of that kind has to say the same */
			FString Owner, Member;
			SplitReference(Property.GetObject(TEXT("Struct")), Owner, Member);

			UScriptStruct* Struct = FindFirstObject<UScriptStruct>(*Owner);

			if (Struct == nullptr) return false;

			OutType.PinCategory = UEdGraphSchema_K2::PC_Struct;
			OutType.PinSubCategoryObject = Struct;
		}
		else if (Kind == TEXT("ObjectProperty")) {
			FString Owner, Member;
			SplitReference(Property.GetObject(TEXT("PropertyClass")), Owner, Member);

			UClass* Class = const_cast<UClass*>(FindClassByType(Owner));

			if (Class == nullptr) return false;

			OutType.PinCategory = UEdGraphSchema_K2::PC_Object;
			OutType.PinSubCategoryObject = Class;
		}
		else return false;

		return true;
	}

}

namespace {
	/* The name a call is made to, where the statement is a call at all */
	FString Calls(const FUObjectJsonValueExport& Expression) {
		if (!Expression.Has(TEXT("Function"))) return FString();

		if (Expression.JsonObject.IsValid() && Expression.JsonObject->HasTypedField<EJson::String>(TEXT("Function"))) {
			return Expression.GetString(TEXT("Function"));
		}

		FString Owner, Member;
		SplitReference(Expression.GetObject(TEXT("Function")), Owner, Member);

		return Member;
	}
}

template <typename T>
T* FBytecodeGraph::AddNode() {
	T* Node = NewObject<T>(Graph);

	Graph->AddNode(Node, false, false);

	Node->CreateNewGuid();
	Node->PostPlacedNewNode();

	return Node;
}

template <typename T>
T* FBytecodeGraph::AddNodeOfClass(const TSubclassOf<T>& Class) {
	T* Node = NewObject<T>(Graph, Class != nullptr ? *Class : T::StaticClass());

	Graph->AddNode(Node, false, false);

	Node->CreateNewGuid();
	Node->PostPlacedNewNode();

	return Node;
}

TSubclassOf<UK2Node_CallFunction> FBytecodeGraph::NodeClassFor(const UFunction* Function) {
	/* A call is not always a plain call node. An array function carries wildcards that only take a
	 * type because the array node hands one down from whatever the array pin was given; a data
	 * table function fills a name pin from the table it is pointed at. Read as a plain call, those
	 * pins stay as the compiler left them and the graph will not compile.
	 *
	 * Which kind a function wants is said by the function itself, in its metadata, so the rule here
	 * is the engine's rule rather than a list of names. Promotable operators are left out: whether
	 * one is used at all is an editor setting rather than anything the function says. */
	if (Function == nullptr) return UK2Node_CallFunction::StaticClass();

	if (Function->HasMetaData(FBlueprintMetadata::MD_CommutativeAssociativeBinaryOperator) && Function->HasAnyFunctionFlags(FUNC_BlueprintPure)) {
		return UK2Node_CommutativeAssociativeBinaryOperator::StaticClass();
	}

	if (Function->HasMetaData(FBlueprintMetadata::MD_MaterialParameterCollectionFunction)) {
		return UK2Node_CallMaterialParameterCollectionFunction::StaticClass();
	}

	if (Function->HasMetaData(FBlueprintMetadata::MD_DataTablePin)) {
		return UK2Node_CallDataTableFunction::StaticClass();
	}

	if (Function->HasMetaData(FBlueprintMetadata::MD_ArrayParam)) {
		return UK2Node_CallArrayFunction::StaticClass();
	}

	return UK2Node_CallFunction::StaticClass();
}

UFunction* FBytecodeGraph::FindFunctionOn(const UClass* Class, const FString& Member) {
	if (Class == nullptr || Member.IsEmpty()) return nullptr;

	if (UFunction* Found = Class->FindFunctionByName(*Member)) return Found;

	/* Called by a name the engine has since moved on from. What a float math node was called is
	 * what a double one is called now, and the engine is the one that knows which became which,
	 * so it is asked rather than guessed at. */
	const FCoreRedirectObjectName Called(*Member, *Class->GetName(), NAME_None);
	const FCoreRedirectObjectName Now = FCoreRedirects::GetRedirectedName(ECoreRedirectFlags::Type_Function, Called);

	if (Now == Called) return nullptr;

	/* A redirect may move it onto another class as well as rename it */
	const UClass* On = Now.OuterName != Called.OuterName ? FindClassByType(Now.OuterName.ToString()) : Class;

	return On != nullptr ? On->FindFunctionByName(Now.ObjectName) : nullptr;
}

FString FBytecodeGraph::ReadStructConst(const FUObjectJsonValueExport& Expression) {
	if (!Expression.Has(TEXT("Struct"))) return FString();

	FString Owner, Member;
	SplitReference(Expression.GetObject(TEXT("Struct")), Owner, Member);

	/* Named the way everything else in the script is, and a struct is the type on its own */
	const UScriptStruct* Struct = FindStructByType(Member.IsEmpty() ? Owner : Member);

	if (Struct == nullptr) return FString();

	const TArray<FUObjectJsonValueExport> Listed = Expression.Has(TEXT("Properties"))
		? Expression.GetArray(TEXT("Properties"))
		: TArray<FUObjectJsonValueExport>();

	FString Spelled = TEXT("(");

	int32 At = 0;

	/* Walked the way the compiler walked it. What it wrote out, it wrote by asking the struct for
	 * its members one after another, so asking the same question again pairs each value with the
	 * member it came from however the struct happens to hand them out. */
	for (TFieldIterator<FProperty> It(Struct); It && At < Listed.Num(); ++It, ++At) {
		const FString Held = MacroReading::TokenOf(Listed[At]) == TEXT("EX_StructConst")
			? ReadStructConst(Listed[At])
			: ReadExpression(Listed[At]).Literal;

		if (At > 0) Spelled += TEXT(",");

		/* Quoted where the member holds words, which is how a default is written out */
		const bool bWords = It->IsA<FStrProperty>() || It->IsA<FNameProperty>() || It->IsA<FTextProperty>();

		Spelled += It->GetName() + TEXT("=") + (bWords ? TEXT("\"") + Held + TEXT("\"") : Held);
	}

	return Spelled + TEXT(")");
}

UFunction* FBytecodeGraph::ResolveFunction(const FUObjectJsonValueExport& Reference) {
	FString Owner, Member;
	SplitReference(Reference, Owner, Member);

	if (Owner.IsEmpty() || Member.IsEmpty()) return nullptr;

	/* The class the function is on, wherever it happens to live */
	const UClass* Class = FindClassByType(Owner);

	if (Class == nullptr) return nullptr;

	return FindFunctionOn(Class, Member);
}

UEdGraphPin* FBytecodeGraph::PointAtDelegate(UK2Node_BaseMCDelegate* Node, const FUObjectJsonValueExport& Named) {
	if (Node == nullptr) return nullptr;

	/* Kept by something else, which the run says by reaching it through that thing. What is named
	 * is the delegate; what it is reached through is the target the node is asked against. */
	if (Named.Has(TEXT("ObjectExpression"))) {
		const FValue On = Read(Named.GetObject(TEXT("ObjectExpression")));

		FUObjectJsonValueExport Points;

		if (Named.Has(TEXT("RValuePointer"))) {
			Points = Named.GetObject(TEXT("RValuePointer"));
		} else if (Named.Has(TEXT("ContextExpression"))) {
			const FUObjectJsonValueExport Inner = Named.GetObject(TEXT("ContextExpression"));

			if (Inner.Has(TEXT("Variable"))) Points = Inner.GetObject(TEXT("Variable"));
		}

		const FString Called = MacroReading::NamedProperty(Points);

		if (!Called.IsEmpty()) {
			UClass* Owner = nullptr;

			if (Points.Has(TEXT("ResolvedOwner"))) {
				FString Spelled = Points.GetObject(TEXT("ResolvedOwner")).GetString(TEXT("ObjectName"));

				if (Spelled.Contains(TEXT("'"))) {
					Spelled.Split(TEXT("'"), nullptr, &Spelled);
					Spelled.Split(TEXT("'"), &Spelled, nullptr, ESearchCase::CaseSensitive, ESearchDir::FromEnd);
				}

				Owner = FindClassByType(Spelled);
			}

			if (Owner != nullptr) Node->DelegateReference.SetExternalMember(FName(*Called), Owner);
			else Node->DelegateReference.SetSelfMember(FName(*Called));
		}

		return On.Pin;
	}

	/* One of the blueprint's own, which is asked of itself */
	if (Named.Has(TEXT("Variable"))) {
		if (const FString Called = MacroReading::NamedProperty(Named.GetObject(TEXT("Variable"))); !Called.IsEmpty()) {
			Node->DelegateReference.SetSelfMember(FName(*Called));
		}
	}

	return nullptr;
}

UEdGraphPin* FBytecodeGraph::FindAnswer(const FString& Name) const {
	if (Graph == nullptr || Name.IsEmpty()) return nullptr;

	for (UEdGraphNode* Node : Graph->Nodes) {
		if (!Node->IsA<UK2Node_FunctionResult>()) continue;

		for (UEdGraphPin* Pin : Node->Pins) {
			if (Pin == nullptr || Pin->Direction != EGPD_Input) continue;
			if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec) continue;

			if (Pin->PinName.ToString() == Name) return Pin;
		}
	}

	return nullptr;
}

bool FBytecodeGraph::HoldsLocals() const {
	return Graph != nullptr && Graph->GetSchema() != nullptr && Graph->GetSchema()->GetGraphType(Graph) == GT_Function;
}

bool FBytecodeGraph::IsMade(const FString& Name) const {
	/* Named after the node and the pin it was made for, so it is that pin and nothing else */
	if (IsCompilerLocal(Name)) return true;

	/* Made to carry what was typed into a pin, which belongs on that pin */
	if (Constants.Contains(Name)) return true;

	/* Handed out by a macro that was recognised, so the macro holds it */
	if (Owned.Contains(Name)) return true;

	/* A macro's own scratch that no macro claimed. It would be a local of its own, which is the
	 * honest reading, but the event graph cannot keep one: a local lives on a function's entry
	 * node and there is no entry node here. Dropped rather than written as something that does not
	 * exist, and said out loud, since what it really means is a macro nobody has read back yet. */
	if (Name.StartsWith(TEXT("Temp_")) && !HoldsLocals()) return true;

	return false;
}

bool FBytecodeGraph::HasLocal(const FString& Name) const {
	UBlueprint* Blueprint = Graph != nullptr ? Graph->GetTypedOuter<UBlueprint>() : nullptr;

	return Blueprint != nullptr && FBlueprintEditorUtils::FindLocalVariableGuidByName(Blueprint, Graph, *Name).IsValid();
}

bool FBytecodeGraph::EnsureLocal(const FString& Name, const FUObjectJsonValueExport& Property) {
	UBlueprint* Blueprint = Graph != nullptr ? Graph->GetTypedOuter<UBlueprint>() : nullptr;

	if (Blueprint == nullptr) return false;

	if (FBlueprintEditorUtils::FindLocalVariableGuidByName(Blueprint, Graph, *Name).IsValid()) return true;

	FEdGraphPinType Type;

	if (!TypeOfProperty(Property, Type)) return false;

	return FBlueprintEditorUtils::AddLocalVariable(Blueprint, Graph, *Name, Type);
}

void FBytecodeGraph::PointAtLocal(UK2Node_Variable* Node, const FString& Name) {
	UBlueprint* Blueprint = Graph->GetTypedOuter<UBlueprint>();

	Node->VariableReference.SetLocalMember(*Name, Graph->GetName(), FBlueprintEditorUtils::FindLocalVariableGuidByName(Blueprint, Graph, *Name));
}

FBytecodeGraph::FValue FBytecodeGraph::ReadVariable(const FUObjectJsonValueExport& Expression) {
	FValue Value;

	const FUObjectJsonValueExport Variable = Expression.GetObject(TEXT("Variable"));

	const FString Name = MacroReading::NamedProperty(Variable);

	if (Name.IsEmpty()) return Value;

	/* A local the compiler made is the pin it was made for, so it reads back as that pin rather
	 * than as anything the graph has to hold */
	if (UEdGraphPin** Known = Locals.Find(Name)) {
		Value.Pin = *Known;

		return Value;
	}

	/* One the compiler made to carry what was typed into a pin, which reads as that value */
	if (const FString* Carries = Carried.Find(Name)) {
		Value.Literal = *Carries;

		return Value;
	}

	if (IsMade(Name)) return Value;

	/* Anything else the blueprint declared, which is a node that reads it. A local the function
	 * keeps is reached by its own name rather than through the class. */
	UK2Node_VariableGet* Node = AddNode<UK2Node_VariableGet>();

	if (HasLocal(Name)) {
		PointAtLocal(Node, Name);
	} else {
		Node->VariableReference.SetSelfMember(*Name);
	}

	Node->AllocateDefaultPins();

	Value.Pin = Node->FindPin(*Name, EGPD_Output);

	return Value;
}

FString FBytecodeGraph::Canonical(const FUObjectJsonValueExport& Expression) {
	if (!Expression.JsonObject.IsValid()) return FString();

	/* The same expression twice reads the same twice, so what it says is what identifies it. Keys
	 * are put in order rather than taken as they come, since two readings of the same thing need
	 * to spell it the same way for either to be recognised as the other. */
	TArray<FString> Keys;

	for (const TPair<FString, TSharedPtr<FJsonValue>>& Field : Expression.JsonObject->Values) {
		Keys.Add(Field.Key);
	}

	Keys.Sort();

	FString Spelled = TEXT("{");

	for (const FString& Key : Keys) {
		const TSharedPtr<FJsonValue> Value = Expression.JsonObject->Values[Key];

		Spelled += Key + TEXT(":");

		if (!Value.IsValid()) continue;

		if (Value->Type == EJson::Object) {
			Spelled += Canonical(FUObjectJsonValueExport(Value->AsObject()));
		} else if (Value->Type == EJson::Array) {
			Spelled += TEXT("[");

			for (const TSharedPtr<FJsonValue>& Held : Value->AsArray()) {
				if (Held.IsValid() && Held->Type == EJson::Object) {
					Spelled += Canonical(FUObjectJsonValueExport(Held->AsObject()));
				} else if (Held.IsValid()) {
					Spelled += Held->AsString();
				}

				Spelled += TEXT(",");
			}

			Spelled += TEXT("]");
		} else {
			Spelled += Value->AsString();
		}

		Spelled += TEXT(";");
	}

	return Spelled + TEXT("}");
}

FBytecodeGraph::FValue FBytecodeGraph::Read(const FUObjectJsonValueExport& Expression) {
	/* Something worked out once is worth reading twice. The same struct member read three times is
	 * three of the same node laid side by side, which is nothing anybody wants to look at.
	 *
	 * Asked and answered here rather than at each way out of the reading below, so every kind of
	 * expression is shared on the same terms. */
	const FString Same = bTidy ? Canonical(Expression) : FString();

	if (!Same.IsEmpty()) {
		if (const FShared* Known = Reused.Find(Same)) {
			/* Only what is still nearby. A value read once and wanted again a moment later is the
			 * same read, and drawing it twice is clutter; wanted again much later it is a wire
			 * dragged the length of the graph, and reading it again where it is wanted is tidier
			 * than following that wire back. */
			if (Placing - Known->Made <= SharingReach) {
				FValue Shared;
				Shared.Pin = Known->Pin;

				return Shared;
			}
		}
	}

	const FValue Value = ReadExpression(Expression);

	Remember(Same, Value);

	return Value;
}

FBytecodeGraph::FValue FBytecodeGraph::ReadExpression(const FUObjectJsonValueExport& Expression) {
	FValue Value;

	if (!Expression.JsonObject.IsValid() || !Expression.Has(TEXT("Token"))) return Value;

	const FString Token = Expression.GetString(TEXT("Token"));


	if (Token == TEXT("EX_LocalVariable") || Token == TEXT("EX_InstanceVariable") || Token == TEXT("EX_LocalOutVariable") || Token == TEXT("EX_DefaultVariable")) {
		return ReadVariable(Expression);
	}

	/* A constant is a value sat on the pin that takes it, which is how the graph carries one */
	if (Token == TEXT("EX_IntConst") || Token == TEXT("EX_ByteConst") || Token == TEXT("EX_FloatConst") || Token == TEXT("EX_DoubleConst") || Token == TEXT("EX_Int64Const") || Token == TEXT("EX_UInt64Const")) {
		Value.Literal = LexToString(Expression.GetNumber(TEXT("Value")));

		return Value;
	}

	if (Token == TEXT("EX_True")) {
		Value.Literal = TEXT("true");

		return Value;
	}

	if (Token == TEXT("EX_False")) {
		Value.Literal = TEXT("false");

		return Value;
	}

	if (Token == TEXT("EX_NameConst") || Token == TEXT("EX_StringConst")) {
		Value.Literal = Expression.GetString(TEXT("Value"));

		return Value;
	}

	/* Nothing, said outright.
	 *
	 * An object pin left empty is null, and the compiler writes this for it. There is no node and
	 * no value: the pin taking it keeps the nothing it already had, and writes the same back. */
	if (Token == TEXT("EX_NoObject") || Token == TEXT("EX_NoInterface")) {
		return Value;
	}

	/* A struct written out in full, which is a value sat on the pin that takes it.
	 *
	 * The compiler writes the members in the order the struct hands them out, so they are read back
	 * in that same order and named from the struct rather than counted off against anything here. */
	if (Token == TEXT("EX_StructConst")) {
		Value.Literal = ReadStructConst(Expression);

		return Value;
	}

	if (Token == TEXT("EX_Self")) {
		UK2Node_Self* Node = AddNode<UK2Node_Self>();

		Node->AllocateDefaultPins();

		Value.Pin = Node->FindPin(UEdGraphSchema_K2::PN_Self, EGPD_Output);

		return Value;
	}

	/* A widening or narrowing the compiler wrote in, which nobody wrote in a graph.
	 *
	 * A pin that carries a real number carries whichever width it is given, and the compiler puts
	 * the conversion in on the way to whatever it feeds. There is no node for it, so the cast is
	 * read through to the value it was applied to. */
	if (Token == TEXT("EX_Cast")) {
		return Read(Expression.GetObject(TEXT("Target")));
	}

	/* An object named outright.
	 *
	 * A call to a function that belongs to a class rather than to an object is still made against
	 * something, and what the compiler makes it against is that class's default object. Nobody
	 * wired that in a graph, and the node for such a call has no target to wire it to, so naming
	 * one stands for nothing. Anything else is an asset the graph pointed at. */
	if (Token == TEXT("EX_ObjectConst")) {
		const FUObjectJsonValueExport Named = Expression.GetObject(TEXT("Value"));

		FString Owner, Member;
		SplitReference(Named, Owner, Member);

		/* A reference with nothing in front of it is the whole name, not the outer of one */
		const FString Called = Member.IsEmpty() ? Owner : Member;

		if (Called.StartsWith(DEFAULT_OBJECT_PREFIX)) return Value;

		/* Named where the game kept it, which is not where the editor keeps it */
		FString Where = Named.Has(TEXT("ObjectPath")) ? ToEditorPackagePath(Named.GetString(TEXT("ObjectPath"))) : FString();

		/* The cloud names an export by the package it is in and the number it sits at, and the
		 * number is no part of where the editor keeps it. Left on, the name is written after it and
		 * the whole thing points at nothing. Only a number is taken off: a path that already ends
		 * in the object's own name is the same path either way. */
		if (int32 Sits; Where.FindLastChar(TEXT('.'), Sits)) {
			const FString After = Where.RightChop(Sits + 1);

			if (!After.IsEmpty() && After.IsNumeric()) {
				Where.LeftInline(Sits);
			}
		}

		Value.Literal = Where.IsEmpty() ? Called : Where + TEXT(".") + Called;

		/* Brought in where the project hasn't got it.
		 *
		 * A reference written into the script names something the game had, and it is a reference
		 * like any other: everything else an import reaches is fetched the same way. Left alone,
		 * the pin points at nothing and the graph is quietly missing whatever it named.
		 *
		 * What kind of thing to ask for is taken from the reference itself rather than from the pin
		 * that will hold it. A pin says the widest thing it will accept, which is not something that
		 * can be fetched: a material pin takes any material interface, and no asset is one of those. */
		if (!Value.Literal.IsEmpty() && LoadObjectByPath<UObject>(Value.Literal) == nullptr) {
			FString Kind;

			if (Named.Has(TEXT("ObjectName"))) {
				Named.GetString(TEXT("ObjectName")).Split(TEXT("'"), &Kind, nullptr);
			}

			FString Package = Value.Literal;

			if (int32 Sits; Package.FindLastChar(TEXT('.'), Sits)) {
				Package.LeftInline(Sits);
			}

			if (!Kind.IsEmpty() && !Package.IsEmpty()) {
				TObjectPtr<UObject> Brought = nullptr;
				bool bBrought = false;

				FAssetUtilities::ConstructAsset<UObject>(Package, Value.Literal, Kind, Brought, bBrought);

				if (Brought == nullptr) {
					UE_LOG(LogReflectionBytecode, Warning, TEXT("\"%s\" is a %s the project hasn't got and the cloud would not give"), *Value.Literal, *Kind);
				}
			}
		}

		return Value;
	}

	/* An array written out one element at a time, which is a Make Array.
	 *
	 * The script has no way to say a whole array at once, so it says what it is assigning to and
	 * then every element in turn. A Make Array node is the same thing with the elements as pins,
	 * and it grows a pin for each one. */
	if (Token == TEXT("EX_SetArray")) {
		const TArray<FUObjectJsonValueExport> Elements = Expression.Has(TEXT("Elements")) ? Expression.GetArray(TEXT("Elements")) : TArray<FUObjectJsonValueExport>();

		UK2Node_MakeArray* Node = AddNode<UK2Node_MakeArray>();

		Node->NumInputs = FMath::Max(1, Elements.Num());
		Node->AllocateDefaultPins();

		TArray<UEdGraphPin*> Into;

		for (UEdGraphPin* Pin : Node->Pins) {
			if (Pin != nullptr && Pin->Direction == EGPD_Input) Into.Add(Pin);
		}

		for (int32 Which = 0; Which < Elements.Num(); ++Which) {
			if (!Into.IsValidIndex(Which)) break;

			const FValue Held = Read(Elements[Which]);

			if (Held.Pin != nullptr) Connect(Held.Pin, Into[Which]);
			else if (!Held.Literal.IsEmpty()) ApplyLiteral(Into[Which], Held.Literal);
		}

		/* What it hands back settles the kind of array it is, once something has been put in it */
		Node->PostReconstructNode();

		Placed++;

		Value.Pin = Node->GetOutputPin();

		return Value;
	}

	/* One value picked out of several, which is a Select.
	 *
	 * The script spells it as a switch: something to look at, a case for each thing it might be,
	 * and a value to take in each case. A Select node is the same thing with the cases as pins, and
	 * the compiler numbers those pins in the order the cases are written, so they line up one for
	 * one. What it calls the default is a local of its own rather than anything anybody wired. */
	if (Token == TEXT("EX_SwitchValue")) {
		UK2Node_Select* Node = AddNode<UK2Node_Select>();

		Node->AllocateDefaultPins();

		/* What is looked at, wired first: the node works out what kind of thing it is picking
		 * between from this, and lays its case pins out again to match */
		if (UEdGraphPin* Index = Node->GetIndexPin()) {
			const FValue Looked = Read(Expression.GetObject(TEXT("IndexTerm")));

			if (Looked.Pin != nullptr) Connect(Looked.Pin, Index);
			else if (!Looked.Literal.IsEmpty()) ApplyLiteral(Index, Looked.Literal);
		}

		/* Laid out again now it knows what it is looking at.
		 *
		 * A node names its own pins, and it names the ones it picks between after the thing that
		 * decides between them: picking on a truth, it calls them False and True rather than Option
		 * 0 and Option 1. It only does that while laying itself out, and it had nothing to go on the
		 * first time, so it is asked to do it once more now the index is wired. Left as it was, the
		 * pins keep the names it gives a node that does not yet know. */
		Node->ReconstructNode();

		/* Asked for after the index is wired, since that is what decides how many there are */
		TArray<UEdGraphPin*> Options;
		Node->GetOptionPins(Options);

		/* What kind of thing it is picking between.
		 *
		 * The node starts out picking between wildcards and only learns better when something is
		 * wired to it, and a value cannot be written onto a wildcard: an asset put there would be
		 * dropped for not fitting a pin that has no type yet. The script says what it is outright,
		 * in the local the compiler keeps the untaken case in, so the pins are told before they are
		 * given anything. Telling one tells the rest, which is the node's own doing. */
		if (const FUObjectJsonValueExport Untaken = Expression.Has(TEXT("DefaultTerm")) ? Expression.GetObject(TEXT("DefaultTerm")) : FUObjectJsonValueExport(); Untaken.Has(TEXT("Variable")) && Options.Num() > 0) {
			const FUObjectJsonValueExport Held = Untaken.GetObject(TEXT("Variable"));

			if (FEdGraphPinType Picking; Held.Has(TEXT("Property")) && FBlueprintVariables::GetPinType(Held.GetObject(TEXT("Property")).JsonObject, Picking)) {
				Options[0]->PinType = Picking;

				Node->PinTypeChanged(Options[0]);

				/* Asked for again, since being told laid them out afresh */
				Node->GetOptionPins(Options);
			}
		}

		const TArray<FUObjectJsonValueExport> Cases = Expression.Has(TEXT("Cases")) ? Expression.GetArray(TEXT("Cases")) : TArray<FUObjectJsonValueExport>();

		for (int32 Which = 0; Which < Cases.Num(); ++Which) {
			if (!Options.IsValidIndex(Which) || !Cases[Which].Has(TEXT("CaseTerm"))) continue;

			const FValue Taken = Read(Cases[Which].GetObject(TEXT("CaseTerm")));

			if (Taken.Pin != nullptr) Connect(Taken.Pin, Options[Which]);
			else if (!Taken.Literal.IsEmpty()) ApplyLiteral(Options[Which], Taken.Literal);
		}

		Placed++;

		Value.Pin = Node->GetReturnValuePin();

		return Value;
	}

	/* A call reached for its value, which is the pin it returns on */
	if (Token == TEXT("EX_CallMath") || Token == TEXT("EX_FinalFunction") || Token == TEXT("EX_LocalFinalFunction") || Token == TEXT("EX_VirtualFunction") || Token == TEXT("EX_LocalVirtualFunction")) {
		if (UK2Node* Node = PlaceCall(Expression, nullptr)) {
			Value.Pin = Node->FindPin(UEdGraphSchema_K2::PN_ReturnValue, EGPD_Output);
		}

		return Value;
	}

	/* A call on something, where the something is the target the call is made against */
	if (Token == TEXT("EX_Context") || Token == TEXT("EX_Context_FailSilent")) {
		const FValue Target = Read(Expression.GetObject(TEXT("ObjectExpression")));
		const FUObjectJsonValueExport Inner = Expression.GetObject(TEXT("ContextExpression"));

		if (Inner.Has(TEXT("Token")) && Inner.GetString(TEXT("Token")).Contains(TEXT("Function"))) {
			if (UK2Node* Node = PlaceCall(Inner, Target.Pin)) {
				Value.Pin = Node->FindPin(UEdGraphSchema_K2::PN_ReturnValue, EGPD_Output);
			}

			return Value;
		}

		/* Reading a member off something, which the graph spells as a read made against that target */
		if (Inner.Has(TEXT("Token")) && Inner.GetString(TEXT("Token")).EndsWith(TEXT("Variable"))) {
			const FUObjectJsonValueExport Held = Inner.GetObject(TEXT("Variable"));

			FString Name = MacroReading::NamedProperty(Held);
			FString Owner;

			/* Whose member it is, which says where the graph looks the name up */
			if (Held.Has(TEXT("ResolvedOwner"))) {
				FString Member;
				SplitReference(Held.GetObject(TEXT("ResolvedOwner")), Owner, Member);
			} else if (Held.Has(TEXT("ObjectName"))) {
				/* An older asset points at the property rather than saying who owns it separately,
				 * and a reference carries its owner: Class'Owner:Name' */
				FString Member;
				SplitReference(Held, Owner, Member);
			}

			UClass* On = Owner.IsEmpty() ? nullptr : const_cast<UClass*>(FindClassByType(Owner));

			if (!Name.IsEmpty() && On != nullptr && Target.Pin != nullptr) {
				UK2Node_VariableGet* Node = AddNode<UK2Node_VariableGet>();

				Node->VariableReference.SetExternalMember(*Name, On);
				Node->AllocateDefaultPins();

				Connect(Target.Pin, Node->FindPin(UEdGraphSchema_K2::PN_Self, EGPD_Input));

				Value.Pin = Node->FindPin(*Name, EGPD_Output);

				return Value;
			}
		}

		return Read(Inner);
	}

	/* One field out of a struct, which the graph takes apart to reach */
	if (Token == TEXT("EX_StructMemberContext")) {
		const FValue Held = Read(Expression.GetObject(TEXT("StructExpression")));

		const FUObjectJsonValueExport Property = Expression.GetObject(TEXT("Property"));

		FString Member;

		if (Property.Has(TEXT("Name"))) {
			Member = Property.GetString(TEXT("Name"));
		} else if (Property.JsonObject.IsValid()) {
			const TArray<TSharedPtr<FJsonValue>>* Path;

			if (Property.JsonObject->TryGetArrayField(TEXT("Path"), Path) && Path->Num() > 0) {
				Member = (*Path)[Path->Num() - 1]->AsString();
			}
		}

		FString Owner, Unused;

		if (Property.Has(TEXT("ResolvedOwner"))) {
			SplitReference(Property.GetObject(TEXT("ResolvedOwner")), Owner, Unused);
		}

		UScriptStruct* Struct = Owner.IsEmpty() ? nullptr : FindFirstObject<UScriptStruct>(*Owner);

		if (Held.Pin != nullptr && Struct != nullptr && !Member.IsEmpty()) {
			/* Taking a struct apart is one node however many of its fields are wanted: reading three
			 * of them is three reads of the same node, not three of the node. What identifies it is
			 * the struct it takes apart, so the field is left out of what it is remembered by. */
			const FString SameBreak = bTidy
				? Canonical(Expression.GetObject(TEXT("StructExpression"))) + TEXT("|apart|") + Struct->GetName()
				: FString();

			UK2Node_BreakStruct* Node = nullptr;

			if (!SameBreak.IsEmpty()) {
				if (const FTakenApart* Known = Apart.Find(SameBreak)) {
					if (Placing - Known->Made <= SharingReach) {
						Node = Cast<UK2Node_BreakStruct>(Known->Node);
					}
				}
			}

			const bool bMade = Node == nullptr;

			if (bMade) {
				Node = AddNode<UK2Node_BreakStruct>();

				Node->StructType = Struct;
				Node->AllocateDefaultPins();

				Connect(Held.Pin, Node->FindPinChecked(Struct->GetFName(), EGPD_Input));
			}

			Value.Pin = Node->FindPin(*Member, EGPD_Output);

			if (Value.Pin != nullptr) {
				if (bMade && !SameBreak.IsEmpty()) {
					Apart.Add(SameBreak, FTakenApart{ Node, Placing });
				}

				return Value;
			}

			if (bMade) {
				Graph->RemoveNode(Node);
			}
		}

		Unhandled.AddUnique(FString::Printf(TEXT("%s off %s"), *Member, *Owner));

		return Value;
	}

	/* A transform constant is a value like any other: a pin that takes one spells it out, and
	 * building a node to make it would put a statement in the script that the game never had */
	if (Token == TEXT("EX_TransformConst")) {
		const FUObjectJsonValueExport Held = Expression.GetObject(TEXT("Value"));

		const FUObjectJsonValueExport Translation = Held.GetObject(TEXT("Translation"));
		const FUObjectJsonValueExport Rotation = Held.GetObject(TEXT("Rotation"));
		const FUObjectJsonValueExport Scale = Held.GetObject(TEXT("Scale3D"));

		/* A transform pin spells itself as where, which way and how big, in that order, and as the
		 * numbers alone rather than as the struct prints itself */
		const FQuat Turn(Rotation.GetNumber(TEXT("X")), Rotation.GetNumber(TEXT("Y")), Rotation.GetNumber(TEXT("Z")), Rotation.GetNumber(TEXT("W")));
		const FRotator Facing = Turn.Rotator();

		Value.Literal = FString::Printf(
			TEXT("%f,%f,%f|%f,%f,%f|%f,%f,%f"),
			Translation.GetNumber(TEXT("X")), Translation.GetNumber(TEXT("Y")), Translation.GetNumber(TEXT("Z")),
			Facing.Pitch, Facing.Yaw, Facing.Roll,
			Scale.GetNumber(TEXT("X")), Scale.GetNumber(TEXT("Y")), Scale.GetNumber(TEXT("Z"))
		);

		return Value;
	}

	Unhandled.AddUnique(Token);

	return Value;
}

UClass* FBytecodeGraph::EnsureComponentTemplate(const FString& Name) {
	UBlueprint* Blueprint = Graph != nullptr ? Graph->GetTypedOuter<UBlueprint>() : nullptr;

	if (Blueprint == nullptr || Name.IsEmpty() || Container == nullptr) return nullptr;

	/* One the blueprint already keeps */
	for (UActorComponent* Existing : Blueprint->ComponentTemplates) {
		if (Existing != nullptr && Existing->GetName() == Name) {
			return Existing->GetClass();
		}
	}

	/* The component a call names is an export of its own, kept beside the class rather than in the
	 * construction script: it is the thing the node is a copy of every time it runs. */
	FUObjectExport* Export = nullptr;

	for (FUObjectExport* Candidate : Container->Exports) {
		if (Candidate != nullptr && Candidate->IsJsonValid() && Candidate->GetName().ToString() == Name) {
			Export = Candidate;

			break;
		}
	}

	if (Export == nullptr) return nullptr;

	UClass* Class = Export->GetClass();

	if (Class == nullptr || !Class->IsChildOf(UActorComponent::StaticClass())) return nullptr;

	/* Kept under the class rather than under the blueprint.
	 *
	 * A template is what the node copies every time it runs, and the class is what carries it: the
	 * engine checks every template it has been given is kept there and refuses the class outright
	 * when one is not, so a template made anywhere else takes the whole blueprint down with it. */
	UObject* Keeps = Blueprint->GeneratedClass != nullptr ? static_cast<UObject*>(Blueprint->GeneratedClass) : nullptr;

	if (Keeps == nullptr) return nullptr;

	UActorComponent* Template = NewObject<UActorComponent>(Keeps, Class, *Name, RF_ArchetypeObject | RF_Public | RF_Transactional);

	if (Template == nullptr) return nullptr;

	Export->Object = Template;

	Blueprint->ComponentTemplates.Add(Template);

	UE_LOG(LogReflectionBytecode, Display, TEXT("\"%s\" is a %s the blueprint now keeps a copy of"), *Name, *Class->GetName());

	return Class;
}

void FBytecodeGraph::Remember(const FString& Same, const FValue& Value) {
	if (Same.IsEmpty() || Value.Pin == nullptr) return;

	const UEdGraphNode* MadeOwningNode = Value.Pin->GetOwningNode();

	/* Only what the run goes around is worth keeping. A call made for what it does has to be made
	 * everywhere it was written, and sharing one would drop all but the first. */
	if (MadeOwningNode == nullptr || MadeOwningNode->FindPin(UEdGraphSchema_K2::PN_Execute, EGPD_Input) != nullptr) return;

	/* Kept against where it was read, so how far away it is can be asked later */
	Reused.Add(Same, FShared{ Value.Pin, Placing });
}

UK2Node* FBytecodeGraph::PlaceCall(const FUObjectJsonValueExport& Expression, UEdGraphPin* Target) {
	if (!Expression.Has(TEXT("Function"))) return nullptr;

	UFunction* Function = nullptr;

	/* A call spells its function either as a reference that carries the class it is on, or as a
	 * bare name, which only means anything against whatever the call is made on */
	if (Expression.JsonObject.IsValid() && Expression.JsonObject->HasTypedField<EJson::String>(TEXT("Function"))) {
		const FString Named = Expression.GetString(TEXT("Function"));

		const UClass* On = Target != nullptr ? Cast<UClass>(Target->PinType.PinSubCategoryObject.Get()) : nullptr;

		/* Called on nothing means called on the blueprint itself. Its own functions are reached
		 * through the skeleton: that is the class the editor keeps up to date as graphs are added,
		 * and one added a moment ago is not on the compiled class yet. */
		if (const UBlueprint* Own = On == nullptr && Graph != nullptr ? Graph->GetTypedOuter<UBlueprint>() : nullptr) {
			On = Own->SkeletonGeneratedClass != nullptr ? Own->SkeletonGeneratedClass : Own->GeneratedClass;

			if (On != nullptr && On->FindFunctionByName(*Named) == nullptr && Own->GeneratedClass != nullptr) {
				On = Own->GeneratedClass;
			}
		}

		Function = FindFunctionOn(On, Named);
	} else {
		Function = ResolveFunction(Expression.GetObject(TEXT("Function")));
	}

	if (Function == nullptr) {
		Unhandled.AddUnique(FString::Printf(TEXT("unresolved function in %s"), *Expression.GetString(TEXT("Token"))));

		return nullptr;
	}

	/* A value a macro already hands out, worked out over again.
	 *
	 * The compiler reads an array element once for every place the body uses it, and every one of
	 * those reads writes the same local. The macro is where that value comes from now, so reading
	 * it again is a node that stands for nothing: whatever wanted it is already wired to the
	 * macro's own pin.
	 *
	 * Only a call that does nothing else is passed over. It has to work out a value and no more,
	 * and every value it works out has to be one the macro already answers for, or it is doing
	 * something of its own and belongs in the graph. */
	if (Function->HasAnyFunctionFlags(FUNC_BlueprintPure)) {
		const TArray<FUObjectJsonValueExport> Given = Expression.Has(TEXT("Parameters")) ? Expression.GetArray(TEXT("Parameters")) : TArray<FUObjectJsonValueExport>();

		bool bWorksOutAValue = false;
		bool bAllHandedOut = true;

		int32 Which = 0;

		for (TFieldIterator<FProperty> It(Function); It && (It->PropertyFlags & CPF_Parm); ++It) {
			if (It->HasAnyPropertyFlags(CPF_ReturnParm)) continue;
			if (!Given.IsValidIndex(Which)) break;

			const FUObjectJsonValueExport& Argument = Given[Which++];

			/* Handed back, rather than merely handed over by reference.
			 *
			 * Anything a function takes by reference is marked as something it writes, whether or
			 * not it may write to it: an array passed for reading is const and is still marked. What
			 * a call actually works out is what it is allowed to change. */
			if (!It->HasAnyPropertyFlags(CPF_OutParm) || It->HasAnyPropertyFlags(CPF_ConstParm)) continue;

			bWorksOutAValue = true;

			const FString Into = Argument.Has(TEXT("Variable")) ? MacroReading::NamedProperty(Argument.GetObject(TEXT("Variable"))) : FString();

			if (Into.IsEmpty() || !Owned.Contains(Into) || !Locals.Contains(Into)) {
				bAllHandedOut = false;

				UE_LOG(LogReflectionBytecode, Display, TEXT("%s works out \"%s\", which no macro answers for (owned %d, known %d)"),
					*Function->GetName(), *Into, Owned.Contains(Into) ? 1 : 0, Locals.Contains(Into) ? 1 : 0);
			}
		}

		if (bWorksOutAValue && bAllHandedOut) {
			UE_LOG(LogReflectionBytecode, Display, TEXT("%s was passed over, since a macro already hands out everything it works out"), *Function->GetName());

			return nullptr;
		}
	}

	/* Adding a component is not a plain call in a graph: the editor has a node of its own for it,
	 * and that node is what gives the call back a component of the right kind. Reached as a plain
	 * call the result is only ever an actor component, and everything made from it has to be cast
	 * back down, which puts statements in the script the game's own never had. */
	UK2Node_CallFunction* Node = nullptr;

	if (Function->GetFName() == TEXT("AddComponent")) {
		const TArray<FUObjectJsonValueExport> Arguments = Expression.Has(TEXT("Parameters")) ? Expression.GetArray(TEXT("Parameters")) : TArray<FUObjectJsonValueExport>();

		/* Which component, said by the call itself: it names the template it copies */
		UClass* Kind = Arguments.Num() > 0 && Arguments[0].Has(TEXT("Value")) ? EnsureComponentTemplate(Arguments[0].GetString(TEXT("Value"))) : nullptr;

		if (Kind != nullptr) {
			UK2Node_AddComponent* Adding = AddNode<UK2Node_AddComponent>();

			Adding->TemplateType = Kind;

			Node = Adding;
		}
	}

	if (Node == nullptr) {
		Node = AddNodeOfClass<UK2Node_CallFunction>(NodeClassFor(Function));
	}

	Node->SetFromFunction(Function);
	Node->AllocateDefaultPins();

	/* The component node works out what it hands back from the template it is pointed at, and it is
	 * pointed at one by name. Said before anything is wired to it, since saying it lays the node
	 * out again and what was wired would be lost. */
	if (UK2Node_AddComponent* Adding = Cast<UK2Node_AddComponent>(Node)) {
		const TArray<FUObjectJsonValueExport> Arguments = Expression.Has(TEXT("Parameters")) ? Expression.GetArray(TEXT("Parameters")) : TArray<FUObjectJsonValueExport>();

		if (Arguments.Num() > 0 && Arguments[0].Has(TEXT("Value"))) {
			for (UEdGraphPin* Pin : Adding->Pins) {
				if (Pin != nullptr && Pin->Direction == EGPD_Input && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Name) {
					Pin->DefaultValue = Arguments[0].GetString(TEXT("Value"));

					break;
				}
			}
		}

		Adding->ReconstructNode();
	}

	/* Named the way the compiler names one, so the locals it makes for the pins come back with the
	 * names the cooked function already carries */
	const FString Name = FString::Printf(TEXT("CallFunc_%s"), *Function->GetName());

	Node->Rename(*MakeUniqueObjectName(Graph, Node->GetClass(), *Name).ToString(), nullptr, REN_DontCreateRedirectors | REN_NonTransactional);

	/* What the call is made against, where it is made against anything */
	if (Target != nullptr) {
		if (UEdGraphPin* Self = Node->FindPin(UEdGraphSchema_K2::PN_Self, EGPD_Input)) {
			Connect(Target, Self);
		}
	}

	/* The arguments, in the order the call carries them, onto the pins that take them */
	const TArray<FUObjectJsonValueExport> Parameters = Expression.Has(TEXT("Parameters")) ? Expression.GetArray(TEXT("Parameters")) : TArray<FUObjectJsonValueExport>();

	/* Whether it is one that waits, which decides whether the run carries on past it */
	bool bWaits = false;

	int32 Argument = 0;

	for (TFieldIterator<FProperty> It(Function); It && (It->PropertyFlags & CPF_Parm); ++It) {
		if (It->HasAnyPropertyFlags(CPF_ReturnParm)) continue;
		if (!Parameters.IsValidIndex(Argument)) break;

		UEdGraphPin* Pin = Node->FindPin(It->GetFName());
		const FUObjectJsonValueExport& Value = Parameters[Argument++];

		/* A call that does not finish where it started.
		 *
		 * A latent call hands the engine a note saying where to pick the run up again once it is
		 * done waiting, and the number in that note is an address in the graph that called it. The
		 * note is the engine's own business and there is no pin for it, but where it points is the
		 * one thing that says what runs after: without it the node is left with nothing after it,
		 * which is what a Delay with nothing on its output is.
		 *
		 * The run does not carry on past a latent call either. It stops there and begins again at
		 * the address, so nothing is chained after it in the ordinary way. */
		if (MacroReading::TokenOf(Value) == TEXT("EX_StructConst") && Value.Has(TEXT("Struct"))
		 && Value.GetObject(TEXT("Struct")).GetString(TEXT("ObjectName")).Contains(TEXT("LatentActionInfo"))) {
			for (const FUObjectJsonValueExport& Held : Value.Has(TEXT("Properties")) ? Value.GetArray(TEXT("Properties")) : TArray<FUObjectJsonValueExport>()) {
				if (MacroReading::TokenOf(Held) != TEXT("EX_SkipOffsetConst")) continue;

				if (const int32 Resumes = Held.GetInteger(TEXT("Value"), INDEX_NONE); Resumes >= 0) {
					if (UEdGraphPin* Then = Node->FindPin(UEdGraphSchema_K2::PN_Then, EGPD_Output)) {
						Jumps.Add({ Then, Resumes });
					}
				}

				break;
			}

			bWaits = true;

			continue;
		}

		if (Pin == nullptr) continue;

		/* Some pins are the node's own business: an add component node works out for itself which
		 * actor it is adding to, and keeps the pin that says so hidden. Passed over before the
		 * value is read rather than after, since reading one lays down the node that works it out
		 * and nothing would ever be wired to it. */
		if (Pin->bHidden || Pin->bNotConnectable) continue;

		/* An output argument is a pin the call writes, and the local it writes into is that pin */
		if (Pin->Direction == EGPD_Output) {
			if (Value.Has(TEXT("Variable"))) {
				const FUObjectJsonValueExport Variable = Value.GetObject(TEXT("Variable"));

				if (const FString Named = MacroReading::NamedProperty(Variable); !Named.IsEmpty()) {
					Locals.Add(Named, Pin);
				}
			}

			continue;
		}

		const FValue Read = this->Read(Value);

		if (Read.Pin != nullptr) {
			Connect(Read.Pin, Pin);
		} else if (!Read.Literal.IsEmpty()) {
			ApplyLiteral(Pin, Read.Literal);
		}
	}

	ChainExecution(Node);

	/* Waiting is where the run stops. What comes after was said by the note it handed over, and is
	 * linked once whatever is at that address has been laid down. */
	if (bWaits) Flow = nullptr;

	Placed++;

	return Node;
}

void FBytecodeGraph::ApplyLiteral(UEdGraphPin* Pin, const FString& Literal) {
	if (Pin == nullptr || Literal.IsEmpty()) return;

	const FName Category = Pin->PinType.PinCategory;

	FString Value = Literal;

	/* A whole number, which the bytecode carries as a number and a pin wants without a fraction */
	if (Category == UEdGraphSchema_K2::PC_Int || Category == UEdGraphSchema_K2::PC_Int64) {
		Value = FString::Printf(TEXT("%lld"), static_cast<int64>(FCString::Atod(*Literal)));
	} else if (Category == UEdGraphSchema_K2::PC_Byte) {
		/* An enum is spelled by the name of the entry rather than by what it is worth */
		if (const UEnum* Enum = Cast<UEnum>(Pin->PinType.PinSubCategoryObject.Get())) {
			Value = Enum->GetNameStringByValue(static_cast<int64>(FCString::Atod(*Literal)));
		} else {
			Value = FString::Printf(TEXT("%lld"), static_cast<int64>(FCString::Atod(*Literal)));
		}
	}

	const UEdGraphSchema_K2* Schema = GetDefault<UEdGraphSchema_K2>();

	/* An object is pointed at rather than spelled out. The pin holds the object itself, and what
	 * the bytecode carries is only where it was kept. */
	if (Category == UEdGraphSchema_K2::PC_Object || Category == UEdGraphSchema_K2::PC_Class
	 || Category == UEdGraphSchema_K2::PC_SoftObject || Category == UEdGraphSchema_K2::PC_SoftClass
	 || Category == UEdGraphSchema_K2::PC_Interface) {
		UObject* Pointed = LoadObjectByPath<UObject>(Value);

		/* Brought in where the project hasn't got it.
		 *
		 * A reference written into the script names something the game had, and it is a reference
		 * like any other: everything else an import reaches is fetched the same way, and a value
		 * sat on a pin is no different for being written rather than wired. Left alone, the pin
		 * points at nothing and the graph is quietly missing whatever it named.
		 *
		 * What kind of thing to ask for is what the pin takes, since the pin is the one thing here
		 * that knows. */
		if (Pointed == nullptr) {
			const UClass* Takes = Cast<UClass>(Pin->PinType.PinSubCategoryObject.Get());

			FString Package = Value;

			if (int32 Named; Package.FindLastChar(TEXT('.'), Named)) {
				Package.LeftInline(Named);
			}

			if (Takes != nullptr && !Package.IsEmpty()) {
				TObjectPtr<UObject> Brought = nullptr;
				bool bBrought = false;

				FAssetUtilities::ConstructAsset<UObject>(Package, Value, Takes->GetName(), Brought, bBrought);

				Pointed = Brought;
			}
		}

		if (Pointed != nullptr) {
			Schema->TrySetDefaultObject(*Pin, Pointed);
		} else {
			UE_LOG(LogReflectionBytecode, Warning, TEXT("nothing at \"%s\" for pin \"%s\" on \"%s\""), *Value, *Pin->PinName.ToString(), *Pin->GetOwningNode()->GetName());
		}

		return;
	}

	/* Set through the schema, which is what decides whether a pin will take a value at all */
	const FString Refused = Schema->IsPinDefaultValid(Pin, Value, nullptr, FText::GetEmpty());

	if (!Refused.IsEmpty()) {
		UE_LOG(LogReflectionBytecode, Warning, TEXT("pin \"%s\" on \"%s\" (%s) would not take \"%s\": %s"), *Pin->PinName.ToString(), *Pin->GetOwningNode()->GetName(), *Category.ToString(), *Value, *Refused);
	}

	Schema->TrySetDefaultValue(*Pin, Value);
}

void FBytecodeGraph::Connect(UEdGraphPin* From, UEdGraphPin* To) {
	if (From == nullptr || To == nullptr) return;

	/* Some pins are the schema's own business: a call carries what it passes through them, and the
	 * graph is not meant to reach them */
	if (To->bHidden || To->bNotConnectable || From->bHidden || From->bNotConnectable) return;

	const UEdGraphSchema_K2* Schema = GetDefault<UEdGraphSchema_K2>();

	/* A self pin left alone already stands for the blueprint itself, so a self reference wired into
	 * one says what the empty pin said anyway.
	 *
	 * Except on a call, where the two are not the same thing. What a call is made against is the
	 * call's context, and the compiler only writes one where something was wired: connected, the
	 * call is written as a context around it; left empty, it is written as a plain call on the
	 * blueprint. Both run the same and neither is longer by accident, so the wire stays on a call
	 * and goes everywhere else. */
	if (Schema->IsSelfPin(*To) && !To->GetOwningNode()->IsA<UK2Node_CallFunction>()) {
		if (UK2Node_Self* Itself = Cast<UK2Node_Self>(From->GetOwningNode())) {
			/* Unless something else is reading it, in which case it is drawn for their sake */
			if (From->LinkedTo.Num() == 0) Graph->RemoveNode(Itself);

			return;
		}
	}

	/* Asked of the schema rather than forced, so a pair that wants a conversion between them gets
	 * one instead of a link the compiler will refuse later */
	if (Schema->TryCreateConnection(From, To)) return;

	/* The bytecode says what a call was made against and not what it was read as, so a value that
	 * arrives wider than the pin taking it was cast in the graph and has to be cast again here.
	 * Which class it is cast to is the one the pin asks for. */
	UClass* Target = Cast<UClass>(To->PinType.PinSubCategoryObject.Get());

	if (Target != nullptr && From->PinType.PinCategory == UEdGraphSchema_K2::PC_Object && To->PinType.PinCategory == UEdGraphSchema_K2::PC_Object) {
		UK2Node_DynamicCast* Node = AddNode<UK2Node_DynamicCast>();

		Node->TargetType = Target;
		Node->AllocateDefaultPins();

		/* Pure, so it sits in the flow of the value rather than in the run of execution. Asked for
		 * after the pins exist, since it is the pins it rebuilds. */
		Node->SetPurity(true);

		UEdGraphPin* Source = Node->GetCastSourcePin();
		UEdGraphPin* Result = Node->GetCastResultPin();

		if (Source != nullptr && Result != nullptr && Schema->TryCreateConnection(From, Source) && Schema->TryCreateConnection(Result, To)) {
			return;
		}

		Graph->RemoveNode(Node);
	}

	/* A pin the schema keeps for itself, which nothing in the graph is meant to reach */
	Unhandled.AddUnique(FString::Printf(TEXT("link %s -> %s"), *From->PinName.ToString(), *To->PinName.ToString()));
}

void FBytecodeGraph::LinkExecution(UEdGraphPin* From, UEdGraphPin* To) {
	if (From == nullptr || To == nullptr) return;

	/* A way out of a node leads to one place. Claiming one that is already spoken for would quietly
	 * take the run off whatever was there before, which is how a whole stretch of the graph goes
	 * missing without anything saying so. */
	if (From->LinkedTo.Num() > 0) {
		Contested.AddUnique(FString::Printf(TEXT("%s.%s"), *From->GetOwningNode()->GetName(), *From->PinName.ToString()));

		return;
	}

	From->MakeLinkTo(To);
}

UEdGraphPin* FBytecodeGraph::WayIn(UEdGraphNode* Node) {
	if (Node == nullptr) return nullptr;

	/* Almost everything spells it the one way */
	if (UEdGraphPin* Named = Node->FindPin(UEdGraphSchema_K2::PN_Execute, EGPD_Input)) return Named;

	/* A macro is entered through whatever its own tunnel was called, which is a name somebody chose
	 * rather than the schema's. There is only ever one way into one, so it is found by being the
	 * way in rather than by being called anything in particular. */
	for (UEdGraphPin* Pin : Node->Pins) {
		if (Pin != nullptr && Pin->Direction == EGPD_Input && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec) return Pin;
	}

	return nullptr;
}

void FBytecodeGraph::EnterNode(UK2Node* Node) {
	/* Which statement the run had reached when this one was placed, for saying where it broke */
	UEdGraphPin* In = WayIn(Node);

	if (In == nullptr) return;

	if (Flow != nullptr) {
		LinkExecution(Flow, In);

		/* The first thing the run reaches, kept so the entry can be tied to it once everything is
		 * laid down: reconstructing a node along the way drops what was linked to it */
		if (Start == nullptr) {
			Start = In;
		}

		Chained++;
	} else {
		UE_LOG(LogReflectionBytecode, Warning, TEXT("nothing leads into \"%s\""), *Node->GetName());

		Orphaned++;
	}
}

void FBytecodeGraph::ChainExecution(UK2Node* Node) {
	/* A node with nowhere to come in has nothing to do with the run: a value it works out is used
	 * wherever it is wanted, and the run goes around it */
	if (WayIn(Node) == nullptr) return;

	EnterNode(Node);

	/* Where the run carries on. Most nodes call it "then", and one that spells it otherwise still
	 * has exactly one way out, so the way out is what is asked for rather than the name. A node
	 * with several, a sequence among them, is handled where it is placed. */
	Flow = Node->FindPin(UEdGraphSchema_K2::PN_Then, EGPD_Output);

	if (Flow == nullptr) {
		for (UEdGraphPin* Pin : Node->Pins) {
			if (Pin != nullptr && Pin->Direction == EGPD_Output && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec) {
				Flow = Pin;

				break;
			}
		}

		/* The node a function answers through is the end of the run and has no way out by design */
		if (Flow == nullptr && !Node->IsA<UK2Node_FunctionResult>()) {
			Unhandled.AddUnique(FString::Printf(TEXT("no way out of %s"), *Node->GetName()));
		}
	}
}

bool FBytecodeGraph::Place(const FUObjectJsonValueExport& Statement) {
	if (!Statement.Has(TEXT("Token"))) return false;

	const FString Token = Statement.GetString(TEXT("Token"));

	/* Storing into a local the compiler made is not a node: it is where a call put what it
	 * returned, so the local stands for that pin from here on */
	if (Token == TEXT("EX_Let") || Token == TEXT("EX_LetBool") || Token == TEXT("EX_LetObj") || Token == TEXT("EX_LetWeakObjPtr")) {
		const FUObjectJsonValueExport Variable = Statement.GetObject(TEXT("Variable"));
		const FValue Expression = Read(Statement.GetObject(TEXT("Expression")));

		FString Name;

		if (Variable.Has(TEXT("Variable"))) {
			Name = MacroReading::NamedProperty(Variable.GetObject(TEXT("Variable")));
		}

		if (!Name.IsEmpty() && (IsMade(Name))) {
			if (Expression.Pin != nullptr) {
				Locals.Add(Name, Expression.Pin);
			} else if (Constants.Contains(Name) && !Expression.Literal.IsEmpty()) {
				/* Kept as what it says rather than laid down as a node. Whoever reads it is a pin
				 * that was given this value, and a pin holds its own value. */
				Carried.Add(Name, Expression.Literal);
			}

			return true;
		}

		/* Written back to whoever called, which a function does through the node its run ends at
		 * rather than by keeping the answer anywhere */
		if (UEdGraphPin* Answer = FindAnswer(Name)) {
			if (Expression.Pin != nullptr) {
				Connect(Expression.Pin, Answer);
			} else if (!Expression.Literal.IsEmpty()) {
				ApplyLiteral(Answer, Expression.Literal);
			}

			return true;
		}

		/* Anything else is the blueprint being written to, which is a node that writes it */
		if (!Name.IsEmpty()) {
			UK2Node_VariableSet* Node = AddNode<UK2Node_VariableSet>();

			const FUObjectJsonValueExport Inner = Variable.GetObject(TEXT("Variable"));

			if (HasLocal(Name)) {
				PointAtLocal(Node, Name);
			} else {
				Node->VariableReference.SetSelfMember(*Name);
			}

			Node->AllocateDefaultPins();

			if (UEdGraphPin* Pin = Node->FindPin(*Name, EGPD_Input)) {
				if (Expression.Pin != nullptr) {
					Connect(Expression.Pin, Pin);
				} else if (!Expression.Literal.IsEmpty()) {
					ApplyLiteral(Pin, Expression.Literal);
				}
			}

			ChainExecution(Node);

			Placed++;

			return true;
		}

		return false;
	}

	/* A call made for what it does rather than what it returns */
	if (Token == TEXT("EX_Context") || Token == TEXT("EX_Context_FailSilent") || Token == TEXT("EX_CallMath") || Token == TEXT("EX_FinalFunction") || Token == TEXT("EX_LocalFinalFunction") || Token == TEXT("EX_VirtualFunction") || Token == TEXT("EX_LocalVirtualFunction")) {
		return Read(Statement).IsSet() || Placed > 0;
	}

	/* The flow control the compiler wrote, put back as the nodes that write it.
	 *
	 * A push and its pop are the two halves of a sequence: the compiler remembers where the second
	 * output starts, runs the first, and pops back to it. A pop that is conditional is a branch
	 * whose other way out leads nowhere, which is how the compiler spells a branch with one side
	 * left open. A jump is an execution wire to somewhere already laid down, which is what a loop
	 * is once it has been compiled. */
	if (Token == TEXT("EX_PushExecutionFlow")) {
		UK2Node_ExecutionSequence* Node = AddNode<UK2Node_ExecutionSequence>();

		Node->AllocateDefaultPins();

		EnterNode(Node);

		/* What the sequence runs first carries on from here. What it runs after is where the push
		 * said it would be, which is not the same as whatever happens to be written next: the pop
		 * that ends the first thread goes to that address, and the statements in between belong to
		 * whichever thread pushed them. */
		Flow = Node->GetThenPinGivenIndex(0);

		Jumps.Add({ Node->GetThenPinGivenIndex(1), Statement.GetInteger(TEXT("PushingAddress"), -1) });

		Placed++;

		return true;
	}

	if (Token == TEXT("EX_PopExecutionFlow")) {
		/* The thread ends here, and what was pushed carries on from where it was pushed to */
		Flow = nullptr;

		return true;
	}

	if (Token == TEXT("EX_PopExecutionFlowIfNot") || Token == TEXT("EX_JumpIfNot")) {
		UK2Node_IfThenElse* Node = AddNode<UK2Node_IfThenElse>();

		Node->AllocateDefaultPins();

		EnterNode(Node);

		const FValue Condition = Read(Statement.GetObject(TEXT("BooleanExpression")));

		if (Condition.Pin != nullptr) {
			Connect(Condition.Pin, Node->GetConditionPin());
		} else if (!Condition.Literal.IsEmpty()) {
			ApplyLiteral(Node->GetConditionPin(), Condition.Literal);
		}

		/* Carrying on is the way the condition holds. Where it does not, a conditional pop ends the
		 * thread and a conditional jump goes where it says. */
		Flow = Node->GetThenPin();

		if (Token == TEXT("EX_JumpIfNot")) {
			Jumps.Add({ Node->GetElsePin(), Statement.GetInteger(TEXT("CodeOffset"), -1) });
		}

		Placed++;

		return true;
	}

	if (Token == TEXT("EX_Jump")) {
		Jumps.Add({ Flow, Statement.GetInteger(TEXT("CodeOffset"), -1) });

		/* The run carries on wherever it was jumped to, and not here */
		Flow = nullptr;

		return true;
	}

	/* The end of the run. Where the function answers with something, the node it answers through
	 * is the last thing the run reaches; where it answers with nothing, there is no node at all. */
	if (Token == TEXT("EX_Return")) {
		for (UEdGraphNode* Node : Graph->Nodes) {
			if (UK2Node_FunctionResult* Answering = Cast<UK2Node_FunctionResult>(Node)) {
				ChainExecution(Answering);

				break;
			}
		}

		return true;
	}

	if (Token == TEXT("EX_EndOfScript") || Token == TEXT("EX_Nothing")) {
		return true;
	}

	/* A delegate made to stand for a function, which is a Create Event.
	 *
	 * The script names the function and what it is a function of, and puts the result in a local of
	 * its own. Whoever binds it later reads that local, so the local stands for what this hands out. */
	if (Token == TEXT("EX_BindDelegate")) {
		UK2Node_CreateDelegate* Node = AddNode<UK2Node_CreateDelegate>();

		Node->AllocateDefaultPins();

		if (Statement.Has(TEXT("ObjectTerm"))) {
			const FValue Of = Read(Statement.GetObject(TEXT("ObjectTerm")));

			if (Of.Pin != nullptr && Node->GetObjectInPin() != nullptr) Connect(Of.Pin, Node->GetObjectInPin());
		}

		/* Said after the object, since which functions it may name depends on what it is a
		 * function of.
		 *
		 * And said without laying the node out again. A node asked to settle itself forgets the
		 * name it was given unless something is already reading what it hands out, and what reads
		 * this one is a statement further along that has not been laid down yet. Left alone, the
		 * name keeps until that statement wires it up, and settling it then finds the name still
		 * there and something to check it against. */
		if (Statement.Has(TEXT("FunctionName"))) {
			Node->SetFunction(FName(*Statement.GetString(TEXT("FunctionName"))));
		}

		if (Statement.Has(TEXT("Delegate"))) {
			const FUObjectJsonValueExport Into = Statement.GetObject(TEXT("Delegate"));

			if (Into.Has(TEXT("Variable"))) {
				if (const FString Name = MacroReading::NamedProperty(Into.GetObject(TEXT("Variable"))); !Name.IsEmpty() && Node->GetDelegateOutPin() != nullptr) {
					Locals.Add(Name, Node->GetDelegateOutPin());
				}
			}
		}

		Placed++;

		return true;
	}

	/* A delegate added to one that keeps several, which is a Bind Event */
	if (Token == TEXT("EX_AddMulticastDelegate") || Token == TEXT("EX_RemoveMulticastDelegate")) {
		UK2Node_BaseMCDelegate* Node = Token == TEXT("EX_AddMulticastDelegate")
			? static_cast<UK2Node_BaseMCDelegate*>(AddNode<UK2Node_AddDelegate>())
			: static_cast<UK2Node_BaseMCDelegate*>(AddNode<UK2Node_RemoveDelegate>());

		UEdGraphPin* Against = PointAtDelegate(Node, Statement.GetObject(TEXT("MulticastDelegate")));

		Node->AllocateDefaultPins();

		if (Against != nullptr && Node->FindPin(UEdGraphSchema_K2::PN_Self, EGPD_Input) != nullptr) {
			Connect(Against, Node->FindPin(UEdGraphSchema_K2::PN_Self, EGPD_Input));
		}

		if (Statement.Has(TEXT("Delegate"))) {
			const FValue Bound = Read(Statement.GetObject(TEXT("Delegate")));

			if (Bound.Pin != nullptr && Node->GetDelegatePin() != nullptr) Connect(Bound.Pin, Node->GetDelegatePin());
		}

		ChainExecution(Node);

		Placed++;

		return true;
	}

	/* A delegate that keeps several, told to run them all, which is a Call node */
	if (Token == TEXT("EX_CallMulticastDelegate")) {
		UK2Node_CallDelegate* Node = AddNode<UK2Node_CallDelegate>();

		UEdGraphPin* Against = PointAtDelegate(Node, Statement.GetObject(TEXT("Delegate")));

		Node->AllocateDefaultPins();

		if (Against != nullptr && Node->FindPin(UEdGraphSchema_K2::PN_Self, EGPD_Input) != nullptr) {
			Connect(Against, Node->FindPin(UEdGraphSchema_K2::PN_Self, EGPD_Input));
		}

		/* What it hands each of them, in the order the signature takes them */
		const TArray<FUObjectJsonValueExport> Given = Statement.Has(TEXT("Parameters")) ? Statement.GetArray(TEXT("Parameters")) : TArray<FUObjectJsonValueExport>();

		if (const UFunction* Signature = Node->GetDelegateSignature()) {
			int32 Which = 0;

			for (TFieldIterator<FProperty> It(Signature); It && (It->PropertyFlags & CPF_Parm); ++It) {
				if (It->HasAnyPropertyFlags(CPF_ReturnParm)) continue;
				if (!Given.IsValidIndex(Which)) break;

				UEdGraphPin* Pin = Node->FindPin(It->GetFName());
				const FValue Held = Read(Given[Which++]);

				if (Pin == nullptr) continue;

				if (Held.Pin != nullptr) Connect(Held.Pin, Pin);
				else if (!Held.Literal.IsEmpty()) ApplyLiteral(Pin, Held.Literal);
			}
		}

		ChainExecution(Node);

		Placed++;

		return true;
	}

	/* An array said all at once, into the local the compiler made to hold it.
	 *
	 * It reads as a statement rather than as a value because there is nowhere else to put it: the
	 * script has no way to write a whole array inside an expression. What it becomes is the one
	 * node that makes an array, and the local it was written into stands for that node's answer. */
	if (Token == TEXT("EX_SetArray")) {
		const FValue Built = Read(Statement);

		if (Built.Pin != nullptr && Statement.Has(TEXT("AssigningProperty"))) {
			const FUObjectJsonValueExport Into = Statement.GetObject(TEXT("AssigningProperty"));

			if (Into.Has(TEXT("Variable"))) {
				if (const FString Name = MacroReading::NamedProperty(Into.GetObject(TEXT("Variable"))); !Name.IsEmpty()) {
					Locals.Add(Name, Built.Pin);
				}
			}
		}

		return true;
	}

	Unhandled.AddUnique(Token);

	return false;
}

void FBytecodeGraph::EnterAt(const int32 Address, UK2Node* Node, const FName Through) {
	if (Node == nullptr || Address < 0) return;

	int32 Begins = Address;

	/* What an event's function passes is where the run is entered, and the compiler lays those
	 * entries out after the bodies, so the one at that address is a jump to the body itself. It
	 * stands for nothing in a graph, so the run is taken to begin where it lands. */
	const int32 Stub = MacroReading::IndexOfAddress(Statements, Address);

	if (Statements.IsValidIndex(Stub) && MacroReading::TokenOf(Statements[Stub]) == TEXT("EX_Jump")) {
		Ignored.Add(Address);

		Begins = Statements[Stub].GetInteger(TEXT("CodeOffset"), Address);
	}

	/* Kept as a list. Two events can be written over the same run: one of them is entered at the
	 * address the other jumps to, and both are ways into it. Held one to an address, the second of
	 * them takes the first's place and whichever lost comes out an event with nothing under it. */
	Starts.FindOrAdd(Begins).Add(TPair<TWeakObjectPtr<UK2Node>, FName>(Node, Through));
}

void FBytecodeGraph::HandOverTrack(const FString& Name, UK2Node* Node, const FName Pin) {
	if (Name.IsEmpty() || Node == nullptr || Pin.IsNone()) return;

	Tracks.Add(Name, TPair<TWeakObjectPtr<UK2Node>, FName>(Node, Pin));
}

void FBytecodeGraph::HandOver(const FString& Frame, const FString& Parameter) {
	if (Frame.IsEmpty() || Parameter.IsEmpty()) return;

	Handed.Add(Frame, Parameter);
}

int32 FBytecodeGraph::DeclareLocals() {
	if (Graph == nullptr) return 0;

	/* Looked for before anything is declared, since neither what a macro accounts for nor what the
	 * compiler made to carry a pin's value is the function's to declare */
	FindConstants();

	if (bTidy) {
		FindMacros();
	}

	int32 Added = 0;

	/* What a previous run declared goes first. The entry node outlives a re-import, and the locals
	 * it carries outlive it too, so anything declared wrongly once would stay declared for good and
	 * shadow the class variable of the same name. */
	for (UEdGraphNode* Node : Graph->Nodes) {
		if (UK2Node_FunctionEntry* Entry = Cast<UK2Node_FunctionEntry>(Node)) {
			Entry->LocalVariables.Empty();

			break;
		}
	}

	/* Declaring a local marks the blueprint changed, which lays the graph out again. Done on its
	 * own pass, before any node is placed, so nothing that is placed is pulled apart afterwards. */
	for (const FUObjectJsonValueExport& Local : Declared) {
		if (!Local.Has(TEXT("Name"))) continue;

		/* What a function takes and gives back is its signature, written on the entry node when the
		 * graph was made. Declaring it again would shadow the parameter with a local of that name. */
		if (bool bGivenBack; FBlueprintGraphs::IsParameter(Local.JsonObject, bGivenBack)) continue;

		const FString Name = Local.GetString(TEXT("Name"));

		/* What the compiler makes for a pin is not the function's to declare: it makes those again */
		if (IsMade(Name)) continue;
		if (HasLocal(Name)) continue;

		/* Nowhere to keep it, which is only ever a macro's scratch left over from a macro that was
		 * not recognised. Said once here rather than as a broken node further along. */
		if (!HoldsLocals()) {
			Unhandled.AddUnique(FString::Printf(TEXT("\"%s\" belongs to a macro that was not read back, and %s cannot keep one"), *Name, *Graph->GetName()));

			continue;
		}

		if (EnsureLocal(Name, Local)) {
			Added++;
		} else {
			Unhandled.AddUnique(FString::Printf(TEXT("local %s"), *Name));
		}
	}

	return Added;
}

UEdGraph* FBytecodeGraph::FindStandardMacro(const TCHAR* Named) {
	return MacroReading::StandardMacro(Named);
}

void FBytecodeGraph::FindConstants() {
	if (bLookedForConstants) return;

	bLookedForConstants = true;

	/* Both ways round, and not only where somebody is going to read the graph.
	 *
	 * These look like something being put back, but they are the opposite: the local was never in
	 * the graph to begin with. A switch has to be told to look at a local and a struct cannot sit
	 * in the script as a literal, so the compiler makes one and writes the value into it. Reading
	 * that back as a node means the compiler makes a second one for the same value, and the script
	 * comes back a statement longer than the one it was read from.
	 *
	 * Left as the pin it was, the compiler makes the same local it made the first time, and the two
	 * scripts agree. */

	TMap<FString, int32> Writes;
	TMap<FString, FString> Says;

	for (const FUObjectJsonValueExport& Statement : Statements) {
		if (!MacroReading::IsLet(MacroReading::TokenOf(Statement))) continue;

		const FString Name = MacroReading::WrittenTo(Statement);

		/* Only the compiler's own scratch is ever one of these. A variable the graph declared is
		 * the graph's however seldom it is written. */
		if (!Name.StartsWith(TEXT("Temp_"))) continue;

		Writes.FindOrAdd(Name)++;
		Says.Add(Name, MacroReading::TokenOf(Statement.GetObject(TEXT("Expression"))));
	}

	for (const TPair<FString, int32>& Wrote : Writes) {
		/* Written more than once is something that changes, whatever it was first given */
		if (Wrote.Value != 1) continue;

		const FString What = Says.FindRef(Wrote.Key);

		/* Said outright rather than worked out. Everything the bytecode spells as a constant ends
		 * that way, and the two truths are the only ones that do not. */
		const bool bSaidOutright = What.EndsWith(TEXT("Const")) || What == TEXT("EX_True") || What == TEXT("EX_False");

		/* Or copied out of something that could have been read where it was wanted.
		 *
		 * A switch has to be told to look at a local, so the compiler makes one and copies the
		 * value into it. Nobody drew that copy: what was drawn is the thing copied, wired straight
		 * to where the copy was read.
		 *
		 * Only a plain read counts. Anything worked out has to stay where it was worked out, or it
		 * would be worked out again at every place the value is used. */
		const bool bCopied = What == TEXT("EX_LocalVariable")
			|| What == TEXT("EX_InstanceVariable")
			|| What == TEXT("EX_DefaultVariable")
			|| What == TEXT("EX_LocalOutVariable");

		if (bSaidOutright || bCopied) {
			Constants.Add(Wrote.Key);
		}
	}
}

void FBytecodeGraph::FindMacros() {
	/* Asked for once from the locals and once from the laying out, and the answer is the same */
	if (bLookedForMacros) return;

	bLookedForMacros = true;

	for (int32 Index = 0; Index < Statements.Num(); ++Index) {
		/* Already accounted for by one that matched earlier */
		if (Skipped.Contains(Index)) continue;

		for (const TSharedRef<FMacroPattern>& Pattern : GetMacroPatterns()) {
			FMacroMatch Match;

			if (!Pattern->Match(Statements, Index, Match) || !Match.IsValid()) continue;

			/* One that would take statements another has already taken is that macro read a second
			 * time rather than a second macro */
			bool bTaken = false;

			for (const int32 Inside : Match.Internal) {
				if (Skipped.Contains(Inside)) {
					bTaken = true;

					break;
				}
			}

			if (bTaken) continue;

			Match.Pattern = &Pattern.Get();

			Macros.Add(Match.First, Match);

			for (const int32 Inside : Match.Internal) {
				Skipped.Add(Inside);
			}

			/* What the macro hands out is the macro's to hold, so nothing else has to. So is
			 * anything its own workings write: a loop's counter is never read outside them, and
			 * declaring it would put a variable in the graph that the macro already is. */
			for (const TPair<FString, FName>& Handout : Match.Handouts) {
				Owned.Add(Handout.Key);
			}

			for (const int32 Inside : Match.Internal) {
				if (!Statements.IsValidIndex(Inside)) continue;

				if (const FString Kept = MacroReading::WrittenTo(Statements[Inside]); !Kept.IsEmpty()) {
					Owned.Add(Kept);
				}
			}

			UE_LOG(LogReflectionBytecode, Display, TEXT("statements %d to %d are one %s"), Match.First, Match.Last, Pattern->GetName());

			/* Read on from the next statement rather than from where this macro ends. What a macro
			 * covers is not what it accounts for: a loop reaches from the first thing it does to
			 * the last, and everything in between is whatever the loop runs, which can be another
			 * loop. Skipping to the end would step straight over it. */
			break;
		}
	}
}

UEdGraphPin* FBytecodeGraph::MacroPin(UK2Node* Node, const FName Named, const EEdGraphPinDirection Direction) {
	if (Node == nullptr) return nullptr;

	if (UEdGraphPin* Exact = Node->FindPin(Named, Direction)) return Exact;

	/* A macro's pins are named for whoever reads the graph, so one of them carries spaces where a
	 * pattern spells it without. What the name says is the same either way. */
	const FString Wanted = Named.ToString().Replace(TEXT(" "), TEXT(""));

	for (UEdGraphPin* Pin : Node->Pins) {
		if (Pin == nullptr || Pin->Direction != Direction) continue;

		if (Pin->PinName.ToString().Replace(TEXT(" "), TEXT("")).Equals(Wanted, ESearchCase::IgnoreCase)) return Pin;
	}

	return nullptr;
}

void FBytecodeGraph::MakeMacros() {
	/* Made before anything is laid out, and not where the run reaches them.
	 *
	 * What a macro hands out is read by the body it runs, and the body is not always written after
	 * the macro: an event graph is written as blocks, so a loop's body can come first. Made up
	 * front, what it hands out is known by the time anything reads it, whichever order they were
	 * written in. Where it goes in the run is settled later, when the run gets there. */
	for (const TPair<int32, FMacroMatch>& Match : Macros) {
		UEdGraph* Definition = FindStandardMacro(Match.Value.Pattern->GetName());

		if (Definition == nullptr) {
			Unhandled.AddUnique(FString::Printf(TEXT("no %s macro to read back into"), Match.Value.Pattern->GetName()));

			continue;
		}

		UK2Node_MacroInstance* Node = AddNode<UK2Node_MacroInstance>();

		Node->SetMacroGraph(Definition);
		Node->AllocateDefaultPins();

		/* What the macro hands out, which is what the compiler's own names in the body stand for */
		for (const TPair<FString, FName>& Handout : Match.Value.Handouts) {
			if (UEdGraphPin* Pin = MacroPin(Node, Handout.Value, EGPD_Output)) {
				Locals.Add(Handout.Key, Pin);
			}
		}

		Written.Add(Match.Key, Node);
	}
}

bool FBytecodeGraph::PlaceMacro(const FMacroMatch& Match) {
	UK2Node_MacroInstance* Node = Written.FindRef(Match.First);

	if (Node == nullptr) return false;

	/* Where the macro begins, for anything that jumps into it.
	 *
	 * Every other node is laid down as its statement comes round, and what begins at an address is
	 * worked out from what that statement put there. A macro is made before any of that, so it puts
	 * nothing there and has to say where it begins outright, or the run that enters it arrives at
	 * an address nothing answers to. */
	if (Statements.IsValidIndex(Match.First)) {
		const int32 Address = MacroReading::AddressOf(Statements[Match.First]);

		if (Address >= 0 && !Entries.Contains(Address)) {
			if (UEdGraphPin* In = WayIn(Node)) {
				Entries.Add(Address, In);
			}
		}
	}

	EnterNode(Node);

	/* What the caller wrote for the macro's inputs */
	for (const TPair<FName, FUObjectJsonValueExport>& Input : Match.Inputs) {
		if (UEdGraphPin* Pin = MacroPin(Node, Input.Key, EGPD_Input)) {
			const FValue Given = Read(Input.Value);

			if (Given.Pin != nullptr) Connect(Given.Pin, Pin);
			else if (!Given.Literal.IsEmpty()) ApplyLiteral(Pin, Given.Literal);
		}
	}

	Placed++;

	/* Held open either way, since an input a macro works out after it begins is wired once the
	 * statements that work it out have been laid down, and that is true however it runs */
	Open.Push({ &Match, Node });

	/* A macro that said where each of its ways out goes is not a run of statements, so the run does
	 * not carry on into it: each way out is linked to the address it leads to, once whatever is
	 * there has been laid down. Where it goes nowhere, nothing runs from it, which is what the
	 * compiler means by ending a thread. */
	if (Match.Leads.Num() > 0) {
		for (const TPair<FName, int32>& Lead : Match.Leads) {
			if (UEdGraphPin* Pin = MacroPin(Node, Lead.Key, EGPD_Output)) {
				Jumps.Add({ Pin, Lead.Value });
			}
		}

		Flow = nullptr;

		return true;
	}

	/* The run carries on into the first thing the macro runs */
	if (Match.Bodies.Num() > 0) {
		Flow = MacroPin(Node, Match.Bodies[0].Key, EGPD_Output);
	}

	return true;
}

void FBytecodeGraph::Arrange() {
	if (Graph == nullptr) return;

	/* How far apart nodes sit. A column is wide enough for a call with its pins, and a row deep
	 * enough that what feeds a node does not land on top of the node beside it. */
	constexpr int32 Column = 420;
	constexpr int32 Row = 200;

	TSet<UEdGraphNode*> Settled;

	/* What each spot is taken by, so two nodes never land on the same one */
	TSet<TPair<int32, int32>> Taken;

	/* How far down anything has been put, so the next run starts below the last rather than over it */
	int32 Deepest = 0;

	/* Puts a node down at the first free spot at or below the one asked for */
	auto PutDown = [&Taken, &Deepest, Row](UEdGraphNode* Node, const int32 X, int32 Y) {
		while (Taken.Contains(TPair<int32, int32>(X, Y))) {
			Y += Row;
		}

		Taken.Add(TPair<int32, int32>(X, Y));

		Node->NodePosX = X;
		Node->NodePosY = Y;

		Deepest = FMath::Max(Deepest, Y);

		return Y;
	};

	/* What feeds a node is laid out to the left of it, in the order its pins take them, and each
	 * one is followed back through whatever feeds it in turn */
	TFunction<int32(UEdGraphNode*, int32, int32)> Feeders = [&](UEdGraphNode* Node, int32 X, int32 Y) {
		int32 Used = 0;

		for (UEdGraphPin* Pin : Node->Pins) {
			if (Pin == nullptr || Pin->Direction != EGPD_Input || Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec) continue;

			for (UEdGraphPin* Linked : Pin->LinkedTo) {
				UEdGraphNode* Feeding = Linked != nullptr ? Linked->GetOwningNode() : nullptr;

				if (Feeding == nullptr || Settled.Contains(Feeding)) continue;

				Settled.Add(Feeding);

				const int32 Landed = PutDown(Feeding, X - Column, Y + Used * Row);

				Used += FMath::Max(1, Feeders(Feeding, X - Column, Landed));
			}
		}

		return Used;
	};

	/* How far right a node has to sit for everything feeding it to sit to its left.
	 *
	 * A graph reads left to right: what feeds a node is drawn to the left of it, and the run passes
	 * through afterwards. Placed by the run alone that does not hold, because a value can be worked
	 * out from something the run has already gone past, and the working then has to fit between the
	 * two. Laid out to the left regardless it is drawn behind the node it was read from, with the
	 * wires running back the way they came.
	 *
	 * So each node is asked how deep what feeds it goes, counting from wherever those readings come
	 * from, and moved right far enough for all of it to fit in front. */
	TFunction<int32(UEdGraphNode*, TSet<UEdGraphNode*>&)> Needed = [&](UEdGraphNode* Node, TSet<UEdGraphNode*>& Asking) {
		if (Node == nullptr || Asking.Contains(Node)) return 0;

		Asking.Add(Node);

		int32 Need = 0;

		for (UEdGraphPin* Pin : Node->Pins) {
			if (Pin == nullptr || Pin->Direction != EGPD_Input || Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec) continue;

			for (UEdGraphPin* Linked : Pin->LinkedTo) {
				UEdGraphNode* Feeding = Linked != nullptr ? Linked->GetOwningNode() : nullptr;

				if (Feeding == nullptr) continue;

				/* One already down is as far right as it stands. One still to come is asked the
				 * same question in turn, since its own feeders have to fit in front of it too. */
				const int32 Sits = Settled.Contains(Feeding) ? Feeding->NodePosX : Needed(Feeding, Asking);

				Need = FMath::Max(Need, Sits + Column);
			}
		}

		Asking.Remove(Node);

		return Need;
	};

	int32 Across = 0;
	int32 Down = 0;

	/* Where the lines are counted from, which is the top of the graph for a run of its own and the
	 * run it meets for one that joins another */
	int32 Base = 0;

	/* The run itself reads left to right, and every way out of a node that leads somewhere else
	 * starts a line of its own below */
	TFunction<void(UEdGraphNode*, int32)> Run = [&](UEdGraphNode* Node, int32 Line) {
		if (Node == nullptr || Settled.Contains(Node)) return;

		Settled.Add(Node);

		{
			TSet<UEdGraphNode*> Asking;

			Across = FMath::Max(Across, Needed(Node, Asking));
		}

		PutDown(Node, Across, Base + Line * Row * 3);

		Feeders(Node, Across, Node->NodePosY + Row);

		/* Where this node ended up, since every way out of it starts from there.
		 *
		 * A node stands in one column and its ways out lead from that one column, so each of them
		 * begins alongside the next. Carried on from wherever the way out before it finished, the
		 * second way out of a branch starts beyond everything the first one ran through, and the
		 * one node on the short way out is drawn the whole length of the long one away from the
		 * branch it comes off. */
		const int32 Here = Node->NodePosX;

		int32 Branch = 0;

		for (UEdGraphPin* Pin : Node->Pins) {
			if (Pin == nullptr || Pin->Direction != EGPD_Output || Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec) continue;

			for (UEdGraphPin* Linked : Pin->LinkedTo) {
				if (Linked != nullptr) {
					Across = Here + Column;

					Run(Linked->GetOwningNode(), Line + Branch);
				}
			}

			Branch++;
		}
	};

	/* Every way into the graph starts a run of its own, laid out below the one before it. A
	 * function is entered in one place, its entry node; the event graph once per event. */
	int32 Line = 0;

	TArray<UEdGraphNode*> Ways;

	for (UEdGraphNode* Node : Graph->Nodes) {
		if (Node != nullptr && (Node->IsA<UK2Node_FunctionEntry>() || Node->IsA<UK2Node_Event>())) {
			Ways.Add(Node);
		}
	}

	/* And whatever else the run was entered through, which is asked of the graph rather than worked
	 * out from what the nodes are. A timeline is entered twice over and is not an event either
	 * time, so read by type it would be left out and everything running from it left at the origin. */
	for (const TPair<int32, TArray<TPair<TWeakObjectPtr<UK2Node>, FName>>>& Entered : Starts) {
		for (const TPair<TWeakObjectPtr<UK2Node>, FName>& One : Entered.Value) {
			if (One.Key.IsValid()) Ways.AddUnique(One.Key.Get());
		}
	}

	for (UEdGraphNode* Way : Ways) {
		if (Settled.Contains(Way)) continue;

		/* Where a run joins one already laid out, it is put where it joins rather than on a line of
		 * its own below everything.
		 *
		 * Runs meet. Two events can run the same thing, and one event can do a little of its own
		 * before carrying on into what another already runs. Laid out as a run of its own, what
		 * joins comes out sat alone at the bottom with one long wire back up to the node it meets.
		 *
		 * So the run is followed forward until it reaches something already down, and started back
		 * from there by as many columns as it took to get there, which lands its last node beside
		 * the one it runs into. */
		UEdGraphNode* Joins = nullptr;

		int32 Depth = 0;

		{
			TArray<TPair<UEdGraphNode*, int32>> Ahead;
			TSet<UEdGraphNode*> Seen;

			Ahead.Add(TPair<UEdGraphNode*, int32>(Way, 0));
			Seen.Add(Way);

			for (int32 At = 0; At < Ahead.Num() && Joins == nullptr; ++At) {
				for (UEdGraphPin* Pin : Ahead[At].Key->Pins) {
					if (Pin == nullptr || Pin->Direction != EGPD_Output || Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec) continue;

					for (UEdGraphPin* Linked : Pin->LinkedTo) {
						UEdGraphNode* Next = Linked != nullptr ? Linked->GetOwningNode() : nullptr;

						if (Next == nullptr || Seen.Contains(Next)) continue;

						if (Settled.Contains(Next)) {
							Joins = Next;
							Depth = Ahead[At].Value + 1;

							break;
						}

						Seen.Add(Next);
						Ahead.Add(TPair<UEdGraphNode*, int32>(Next, Ahead[At].Value + 1));
					}

					if (Joins != nullptr) break;
				}
			}
		}

		if (Joins != nullptr) {
			Across = FMath::Max(0, Joins->NodePosX - Depth * Column);

			/* A row under the run it joins, and further down again wherever that is already taken */
			Base = Joins->NodePosY + Row;

			Run(Way, 0);

			Base = 0;

			continue;
		}

		Across = 0;

		Run(Way, Line);

		/* Two rows of clear air between one run and the next */
		Line = Deepest / (Row * 3) + 2;
	}

	/* Straightened along the run.
	 *
	 * A node is placed by its top corner, but a wire leaves and enters a pin, and the pins sit under
	 * the node's title rather than at the top of it. A call made on a target reads as two lines of
	 * title where a plain one reads as one, so two nodes left at the same height have their pins a
	 * line apart and the wire between them slopes.
	 *
	 * This is what the editor's own Straighten Connections does, except that it measures the pins,
	 * which it can only do once the graph is open and every pin has been given somewhere to be. Here
	 * there is nothing to measure, so the height is worked out from what decides it: how many lines
	 * of title the node carries. */
	auto HeaderOf = [Row](UEdGraphNode* Node) {
		/* Drawn as an operator rather than as a node, so there is no title above the pins */
		if (const UK2Node* Written = Cast<UK2Node>(Node); Written != nullptr && Written->ShouldDrawCompact()) {
			return 0;
		}

		const FString Title = Node->GetNodeTitle(ENodeTitleType::FullTitle).ToString();

		int32 Lines = 1;

		for (const TCHAR Letter : Title) {
			if (Letter == TEXT('\n')) Lines++;
		}

		/* One step of the grid to a line, and one for the title's own top and bottom */
		return (Row / 12) * (Lines + 1);
	};

	/* Only where the two were meant to sit level. A branch puts what runs from it on lines of its
	 * own, and pulling those up to meet the node they run from would undo the run's shape. */
	TSet<UEdGraphNode*> Levelled;

	for (int32 At = 0; At < Ways.Num(); ++At) {
		UEdGraphNode* From = Ways[At];

		for (UEdGraphPin* Pin : From->Pins) {
			if (Pin == nullptr || Pin->Direction != EGPD_Output || Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec) continue;

			for (UEdGraphPin* Linked : Pin->LinkedTo) {
				UEdGraphNode* To = Linked != nullptr ? Linked->GetOwningNode() : nullptr;

				if (To == nullptr || To == From || Levelled.Contains(To)) continue;

				Levelled.Add(To);
				Ways.Add(To);

				if (To->NodePosY != From->NodePosY) continue;

				To->NodePosY = From->NodePosY + HeaderOf(From) - HeaderOf(To);
			}
		}
	}

	/* Anything the run never reaches is put underneath rather than left sat at the origin */
	Down = 0;

	for (UEdGraphNode* Node : Graph->Nodes) {
		if (Node == nullptr || Settled.Contains(Node)) continue;

		Settled.Add(Node);

		PutDown(Node, 0, Row * 3 * (6 + Down++));

		Feeders(Node, 0, Node->NodePosY + Row);
	}
}

void FBytecodeGraph::Clear() {
	if (Graph == nullptr) return;

	/* Whatever a previous run left behind goes, so importing twice lays the same graph out rather
	 * than laying a second one over it. What the graph is entered through stays.
	 *
	 * Done before the blueprint is compiled and not as the laying out begins. A compile reads the
	 * graphs as they stand, so one run's nodes would be compiled again at the start of the next:
	 * where those nodes no longer answer to anything the compile fails, the class never picks up
	 * what the construction script gave it, and everything laid out afterwards is read against a
	 * class that is behind. That is what makes the same asset come back differently each time. */
	for (int32 Index = Graph->Nodes.Num() - 1; Index >= 0; --Index) {
		UEdGraphNode* Node = Graph->Nodes[Index];

		if (Node == nullptr) continue;

		/* What the graph is entered through stays, and so does anything that was not made from the
		 * bytecode in the first place. A timeline is built from the template the class carries
		 * rather than from any statement, so taking it out here would take out the one node the
		 * statements are about to be wired to. */
		if (Node->IsA<UK2Node_FunctionEntry>() || Node->IsA<UK2Node_FunctionResult>() || Node->IsA<UK2Node_Event>() || Node->IsA<UK2Node_Timeline>()) {
			Node->BreakAllNodeLinks();

			continue;
		}

		Graph->RemoveNode(Node);
	}
}

int32 FBytecodeGraph::Build() {
	if (Graph == nullptr) return 0;

	if (bTidy) {
		FindMacros();
	}

	Clear();

	/* What the graph was handed comes out of whatever it is entered through, so a read of a
	 * parameter is a read of that pin rather than of anything the graph keeps of its own */
	for (UEdGraphNode* Node : Graph->Nodes) {
		if (Node == nullptr || !(Node->IsA<UK2Node_FunctionEntry>() || Node->IsA<UK2Node_Event>())) continue;

		for (UEdGraphPin* Pin : Node->Pins) {
			if (Pin == nullptr || Pin->Direction != EGPD_Output) continue;
			if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec) continue;

			Locals.Add(Pin->PinName.ToString(), Pin);
		}
	}

	/* What a timeline hands out, read under the name it keeps it in rather than as the pin it is */
	for (const TPair<FString, TPair<TWeakObjectPtr<UK2Node>, FName>>& Track : Tracks) {
		if (!Track.Value.Key.IsValid()) continue;

		if (UEdGraphPin* Pin = Track.Value.Key->FindPin(Track.Value.Value, EGPD_Output)) {
			Locals.Add(Track.Key, Pin);
		}
	}

	/* An event's parameter is read under whatever the frame keeps it as, which is not its own name */
	for (const TPair<FString, FString>& Given : Handed) {
		UEdGraphPin** Handing = Locals.Find(Given.Value);

		if (Handing == nullptr) continue;

		UEdGraphPin* Pin = *Handing;

		Locals.Add(Given.Key, Pin);
	}

	/* The run starts wherever the graph is entered from. A graph entered at named addresses is
	 * entered once for each of them instead, as those addresses come round. */
	if (Starts.Num() == 0) {
		for (UEdGraphNode* Node : Graph->Nodes) {
			if (UEdGraphPin* Then = Node->FindPin(UEdGraphSchema_K2::PN_Then, EGPD_Output)) {
				Flow = Then;

				UE_LOG(LogReflectionBytecode, Display, TEXT("starting the run from \"%s\""), *Node->GetName());

				break;
			}
		}
	}

	FindConstants();

	MakeMacros();

	/* Before anything a graph says, the compiler puts the function's own end on the flow stack, so
	 * that ending a thread has somewhere to land. Nobody wrote it: it is not a sequence, and read
	 * as one it leaves a node at the top of every graph with nothing on its second way out.
	 *
	 * Told apart by being the first thing the function does and by what it puts there, which is the
	 * statement the function returns from. A function that never ends a thread has none of this. */
	if (Statements.Num() > 0 && MacroReading::TokenOf(Statements[0]) == TEXT("EX_PushExecutionFlow")) {
		const int32 Ends = MacroReading::IndexOfAddress(Statements, Statements[0].GetInteger(TEXT("PushingAddress"), INDEX_NONE));

		if (Statements.IsValidIndex(Ends) && MacroReading::TokenOf(Statements[Ends]) == TEXT("EX_Return")) {
			Ignored.Add(MacroReading::AddressOf(Statements[0]));
		}
	}

	/* The ubergraph is entered by being told where in itself to go, and the statements that do the
	 * telling are the compiler's way in rather than anything somebody wrote */
	for (int32 Index = 0; Index < Statements.Num(); ++Index) {
		if (MacroReading::TokenOf(Statements[Index]) != TEXT("EX_ComputedJump")) continue;

		Ignored.Add(MacroReading::AddressOf(Statements[Index]));

		if (Index > 0 && MacroReading::TokenOf(Statements[Index - 1]) == TEXT("EX_PushExecutionFlow")) {
			Ignored.Add(MacroReading::AddressOf(Statements[Index - 1]));
		}
	}

	/* Walked by number rather than one at a time, since a macro accounts for a run of them */
	for (int32 Index = 0; Index < Statements.Num(); ++Index) {
		const FUObjectJsonValueExport& Statement = Statements[Index];

		const int32 Address = Statement.GetInteger(TEXT("StatementIndex"), -1);
		const int32 Before = Graph->Nodes.Num();

		Placing = Index;

		/* An address the graph is entered at begins a run of its own, from the node it is entered
		 * through rather than from wherever the statement before it left off */
		if (const TArray<TPair<TWeakObjectPtr<UK2Node>, FName>>* Entered = Starts.Find(Address)) {
			Flow = nullptr;

			for (const TPair<TWeakObjectPtr<UK2Node>, FName>& One : *Entered) {
				if (!One.Key.IsValid()) continue;

				UEdGraphPin* Out = One.Key->FindPin(One.Value.IsNone() ? UEdGraphSchema_K2::PN_Then : One.Value, EGPD_Output);

				if (Out == nullptr) continue;

				/* The run carries on from one of them. Anything else entering here is tied up the
				 * way a jump to this address is, since the run can only be carried on from one. */
				if (Flow == nullptr) {
					Flow = Out;
				} else {
					Jumps.Add({ Out, Address });
				}

				UE_LOG(LogReflectionBytecode, Display, TEXT("entering at %d, through \"%s\""), Address, *One.Key->GetName());
			}
		}

		const int32 WasChained = Chained;

		/* A macro stands for a run of statements, so it is placed once and its own workings passed
		 * over. What runs from it is placed as it comes, with the run threaded through its outputs. */
		if (Ignored.Contains(Address)) {
			/* Nothing: it only says where a run begins */
		} else if (const FMacroMatch* Match = Macros.Find(Index)) {
			PlaceMacro(*Match);
		} else if (!Skipped.Contains(Index)) {
			Place(Statement);
		} else {
			/* A macro's own workings stand for nothing, but one of them ending a thread still ends
			 * it: the run does not carry on past it into whatever happens to be written next */
			const FString Token = MacroReading::TokenOf(Statement);

			if (Token == TEXT("EX_Jump") || Token == TEXT("EX_PopExecutionFlow")) {
				Flow = nullptr;
			}
		}

		/* Where a macro's inputs were worked out after it, they are wired once they exist */
		for (const TPair<const FMacroMatch*, UK2Node*>& Waiting : Open) {
			for (const TPair<FName, FUObjectJsonValueExport>& Late : Waiting.Key->Deferred) {
				if (Index != Waiting.Key->Last) continue;

				if (UEdGraphPin* Pin = MacroPin(Waiting.Value, Late.Key, EGPD_Input)) {
					if (Pin->LinkedTo.Num() == 0 && Pin->DefaultValue.IsEmpty()) {
						const FValue Given = Read(Late.Value);

						if (Given.Pin != nullptr) Connect(Given.Pin, Pin);
						else if (!Given.Literal.IsEmpty()) ApplyLiteral(Pin, Given.Literal);
					}
				}
			}
		}

		/* Once everything a macro runs is placed, the run carries on past it */
		while (Open.Num() > 0 && Index >= Open.Last().Key->Last) {
			const TPair<const FMacroMatch*, UK2Node*> Done = Open.Pop();

			/* One that named where its ways out go was wired to them outright, and the run carries
			 * on from wherever those led rather than from here */
			if (Done.Key->Leads.Num() > 0) continue;

			Flow = MacroPin(Done.Value, TEXT("Completed"), EGPD_Output);
		}

		if (Chained > WasChained && Address >= 0) {
			LastChainedAddress = Address;
		}

		/* Where this address begins, for anything that jumps to it. The first node laid down for a
		 * statement that can be entered at all is what the address names. */
		if (Address >= 0 && !Entries.Contains(Address)) {
			for (int32 Laid = Before; Laid < Graph->Nodes.Num(); ++Laid) {
				/* Asked for the way in rather than for a pin of that name: a macro is entered
				 * through whatever its own tunnel was called, and one that is not asked for
				 * properly is a macro nothing can jump to */
				if (UEdGraphPin* In = WayIn(Graph->Nodes[Laid])) {
					Entries.Add(Address, In);

					break;
				}
			}
		}
	}

	/* Tied up at the end, since a jump can name an address that had not been laid down yet */
	for (const TPair<UEdGraphPin*, int32>& Jump : Jumps) {
		if (Jump.Key == nullptr) continue;

		/* A jump can name a statement that does nothing on its own: a value worked out along the
		 * way has no place in the run of execution, so what is meant is the next thing that does.
		 *
		 * Which one that is, is found by carrying on the way the run would have: a jump goes where
		 * it says, and anything else falls through to the address after it. Taking the nearest
		 * address above instead would be wrong wherever the run doubles back, and a loop laid out
		 * as the compiler lays one out doubles back every time round. */
		UEdGraphPin* Entry = nullptr;

		/* Whether the run ends where the jump leads, rather than the jump leading nowhere findable.
		 * A branch whose other way out returns, or a sequence whose second thread is the end of the
		 * function, leads to the end of the run: an unconnected way out is what that looks like in
		 * a graph, and there is nothing missing to say anything about. */
		bool bEnds = false;

		int32 Address = Jump.Value;
		TSet<int32> Walked;

		while (Address >= 0 && !Walked.Contains(Address)) {
			Walked.Add(Address);

			if (UEdGraphPin** Begins = Entries.Find(Address)) {
				Entry = *Begins;

				break;
			}

			const int32 At = MacroReading::IndexOfAddress(Statements, Address);

			if (At == INDEX_NONE) break;

			/* Landed among a macro's own workings, which stand for nothing on their own. The
			 * macro is the one node all of them became, and it is entered where it begins: coming
			 * in anywhere else means the macro carries on by itself from there, which in a graph
			 * is a way out that leads nowhere. */
			if (Skipped.Contains(At)) {
				bEnds = true;

				break;
			}

			const FString Token = MacroReading::TokenOf(Statements[At]);

			if (Token == TEXT("EX_Jump")) {
				Address = Statements[At].GetInteger(TEXT("CodeOffset"), -1);

				continue;
			}

			/* The thread ends here, and where it carries on is not this jump's business */
			if (Token == TEXT("EX_PopExecutionFlow") || Token == TEXT("EX_Return") || Token == TEXT("EX_EndOfScript")) {
				bEnds = true;

				break;
			}

			if (!Statements.IsValidIndex(At + 1)) break;

			Address = MacroReading::AddressOf(Statements[At + 1]);
		}

		if (Entry != nullptr) {
			LinkExecution(Jump.Key, Entry);
		} else if (!bEnds) {
			Unhandled.AddUnique(FString::Printf(TEXT("jump to %d, which nothing begins"), Jump.Value));
		}
	}

	/* Put back whatever was written as one node before anything is laid out, so what is laid out is
	 * what the reader will see */
	if (bTidy) {
		for (const TSharedRef<FGraphTidy>& Tidy : GetGraphTidies()) {
			Tidy->Apply(Graph);
		}
	}

	Arrange();

	/* Tied on last. A node that is rebuilt while the graph is being laid out takes its links with
	 * it, and the entry is the one link nothing else can put back. */
	if (Start != nullptr) {
		for (UEdGraphNode* Node : Graph->Nodes) {
			if (Node == nullptr || !Node->IsA<UK2Node_FunctionEntry>()) continue;

			if (UEdGraphPin* Then = Node->FindPin(UEdGraphSchema_K2::PN_Then, EGPD_Output)) {
				LinkExecution(Then, Start);
			}

			break;
		}
	}

	/* What the run actually starts from, checked rather than assumed */
	for (UEdGraphNode* Node : Graph->Nodes) {
		if (Node == nullptr || !Node->IsA<UK2Node_FunctionEntry>()) continue;

		UEdGraphPin* Then = Node->FindPin(UEdGraphSchema_K2::PN_Then, EGPD_Output);

		UE_LOG(LogReflectionBytecode, Display, TEXT("entry \"%s\" carries %d link(s) out of it"), *Node->GetName(), Then != nullptr ? Then->LinkedTo.Num() : -1);
	}

	UE_LOG(LogReflectionBytecode, Display, TEXT("\"%s\": entered from %s, %d node(s) chained, %d left out of the run"), *Graph->GetName(), Flow != nullptr ? TEXT("a pin") : TEXT("nothing"), Chained, Orphaned);

	if (Contested.Num() > 0) {
		UE_LOG(LogReflectionBytecode, Warning, TEXT("%d way(s) out were already spoken for: %s"), Contested.Num(), *FString::Join(Contested, TEXT(", ")));
	}

	if (Unhandled.Num() > 0) {
		UE_LOG(LogReflectionBytecode, Warning, TEXT("\"%s\" placed %d node(s), and read nothing from: %s"), *Graph->GetName(), Placed, *FString::Join(Unhandled, TEXT(", ")));

		/* Said where somebody will see it and not only in the log. A graph that came back with
		 * something missing still opens and still compiles, and what it does then is not what the
		 * game did, which is worth knowing before it is trusted. */
		FImportIssues::Report(
			EImportIssue::Data,
			FString::Printf(TEXT("Part of \"%s\" could not be read back"), *Graph->GetName()),
			FString::Printf(TEXT("%d node(s) were laid out. Nothing was read from: %s"), Placed, *FString::Join(Unhandled, TEXT(", ")))
		);
	} else {
		UE_LOG(LogReflectionBytecode, Display, TEXT("\"%s\" placed %d node(s) from %d statement(s)"), *Graph->GetName(), Placed, Statements.Num());
	}

	return Placed;
}
