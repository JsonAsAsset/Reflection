/* Copyright Reflection Contributors 2024-2026 */

#include "Importers/Constructor/Graph/RigVMGraphBuilder.h"

#include "Modules/Log.h"
#include "Engine/Compatibility.h"
#include "Serializers/PropertySerializer.h"

#include "RigVMBlueprint.h"
#include "RigVMCore/RigVMDispatchFactory.h"
#include "RigVMCore/RigVMExecuteContext.h"
#include "RigVMCore/RigVMRegistry.h"
#include "RigVMDeveloperTypeUtils.h"
#include "RigVMTypeUtils.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "RigVMModel/RigVMController.h"
#include "RigVMModel/RigVMGraph.h"
#include "RigVMModel/RigVMNode.h"
#include "RigVMModel/RigVMPin.h"
#include "RigVMModel/Nodes/RigVMLibraryNode.h"

namespace {
	/* An operand's RegisterOffset when it addresses the whole register rather than a slice of it */
	constexpr int32 NoRegisterOffset = 0xFFFF;

	/* ERigVMMemoryType, as the operands store it */
	constexpr int32 WorkMemory = 0;
	constexpr int32 LiteralMemory = 1;
	constexpr int32 ExternalMemory = 2;

	/* The compiler separates the graph a register belongs to from the node with a triple underscore */
	const FString GraphSeparator = TEXT("___");

	/* Suffixes the compiler appends to a register depending on how the pin is used */
	const TArray<FString> RegisterSuffixes = { TEXT("__Const"), TEXT("__IO") };

	int32 GetNumber(const TSharedPtr<FJsonObject>& Object, const FString& Field, const int32 Default = 0) {
		double Value = 0.0;
		return Object.IsValid() && Object->TryGetNumberField(Field, Value) ? static_cast<int32>(Value) : Default;
	}
}

FRigVMGraphBuilder::FRigVMGraphBuilder(UBlueprint* InBlueprint, FUObjectExportContainer* InContainer, UPropertySerializer* InPropertySerializer)
	: Blueprint(Cast<URigVMBlueprint>(InBlueprint))
	, Container(InContainer)
	, PropertySerializer(InPropertySerializer)
{
}

bool FRigVMGraphBuilder::Build() {
	if (Blueprint == nullptr || Container == nullptr) return false;
	if (!ReadVirtualMachine()) return false;

	URigVMGraph* RootGraph = Blueprint->GetDefaultModel();
	if (RootGraph == nullptr) return false;

	Controller = Blueprint->GetOrCreateController(RootGraph);
	if (Controller == nullptr) return false;

	/* The rig's own graph keeps the name it already has; every other one the registers mention was a function */
	RootGraphName = RootGraph->GetName();

	Blueprint->SetAutoVMRecompile(false);

	BuildVariables();
	BuildNodes();
	BuildLinks();
	BuildExecutionChain();

	Blueprint->SetAutoVMRecompile(true);
	Blueprint->RecompileVM();

	UE_LOG(LogReflection, Log, TEXT("Rebuilt %d RigVM nodes from %d instructions across %d graph(s)"), NodesByName.Num(), Instructions.Num(), Controllers.Num() + 1);

	return NodesByName.Num() > 0;
}

/* ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ Reading the virtual machine ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ */

bool FRigVMGraphBuilder::ReadVirtualMachine() {
	/* The VM is found by what it carries rather than by its type name, so this doesn't care which engine
	 * version cooked it or what the host asset calls its machine. */
	FUObjectExport* VirtualMachine = nullptr;

	for (FUObjectExport* Export : Container->Exports) {
		if (Export->JsonObject.IsValid() && Export->JsonObject->HasField(TEXT("ByteCodeStorage"))) {
			VirtualMachine = Export;
			break;
		}
	}

	if (VirtualMachine == nullptr) return false;

	const TSharedPtr<FJsonObject>& Machine = VirtualMachine->JsonObject;

	for (const TSharedPtr<FJsonValue>& FunctionName : Machine->GetArrayField(TEXT("FunctionNamesStorage"))) {
		FunctionNames.Add(FunctionName->AsString());
	}

	const TSharedPtr<FJsonObject> ByteCode = Machine->GetObjectField(TEXT("ByteCodeStorage"));
	if (!ByteCode.IsValid() || !ByteCode->HasField(TEXT("Instructions"))) return false;

	Instructions = ByteCode->GetArrayField(TEXT("Instructions"));

	ReadRegisterNames(Machine, TEXT("DefaultWorkMemoryStorage"), WorkRegisters, WorkSegmentPaths);
	ReadRegisterNames(Machine, TEXT("LiteralMemoryStorage"), LiteralRegisters, LiteralSegmentPaths);

	/* Literal values live alongside the descriptors that name them */
	const TSharedPtr<FJsonObject>* LiteralStorage;
	if (Machine->TryGetObjectField(TEXT("LiteralMemoryStorage"), LiteralStorage)) {
		ReadLiteralValues(*LiteralStorage);
	}

	/* A variable can be read a member at a time, and the paths into it are kept by the machine rather than by
	 * either block of memory - a rig whose variables are structs addresses them almost entirely this way */
	const TArray<TSharedPtr<FJsonValue>>* ExternalPaths;
	if (Machine->TryGetArrayField(TEXT("ExternalPropertyPathDescriptions"), ExternalPaths)) {
		for (const TSharedPtr<FJsonValue>& Description : *ExternalPaths) {
			ExternalSegmentPaths.Add(Description->AsObject()->GetStringField(TEXT("SegmentPath")));
		}
	}

	/* External variables are the generated class' own properties, in declaration order */
	for (FUObjectExport* Export : Container->Exports) {
		if (!Export->JsonObject.IsValid() || !IsGeneratedClassType(Export->GetType().ToString())) continue;

		const TArray<TSharedPtr<FJsonValue>>* ChildProperties;
		if (!Export->JsonObject->TryGetArrayField(TEXT("ChildProperties"), ChildProperties)) break;

		for (const TSharedPtr<FJsonValue>& ChildProperty : *ChildProperties) {
			const TSharedPtr<FJsonObject> Description = ChildProperty->AsObject();

			ExternalVariables.Add(Description->GetStringField(TEXT("Name")));
			ExternalVariableDescriptions.Add(Description);
		}

		break;
	}

	return Instructions.Num() > 0 && FunctionNames.Num() > 0;
}

/*
 * The authored values are exported as property tags rather than a plain map: each entry names the register it
 * belongs to and carries the value itself under its tag. Exports that write a straight name-to-value object are
 * read too, since the tag list is the only part of this that depends on how the values were written out.
 */
void FRigVMGraphBuilder::ReadLiteralValues(const TSharedPtr<FJsonObject>& Storage) {
	const TArray<TSharedPtr<FJsonValue>>* Tags;

	if (Storage->TryGetArrayField(TEXT("Properties"), Tags)) {
		for (const TSharedPtr<FJsonValue>& Entry : *Tags) {
			const TSharedPtr<FJsonObject> Tag = Entry->AsObject();
			if (!Tag.IsValid()) continue;

			FString Name;
			if (!Tag->TryGetStringField(TEXT("Name"), Name)) continue;

			if (const TSharedPtr<FJsonValue>* Value = Tag->Values.Find(TEXT("Tag"))) {
				LiteralValues.Add(Name, *Value);
			}
		}

		return;
	}

	const TSharedPtr<FJsonObject>* Values;
	if (Storage->TryGetObjectField(TEXT("Properties"), Values)) {
		LiteralValues = (*Values)->Values;
	}
}

void FRigVMGraphBuilder::ReadRegisterNames(const TSharedPtr<FJsonObject>& Storage, const FString& StorageName, TArray<FString>& OutNames, TArray<FString>& OutSegmentPaths) const {
	const auto ReadSegmentPaths = [&OutSegmentPaths](const TSharedPtr<FJsonObject>& Source) {
		const TArray<TSharedPtr<FJsonValue>>* Descriptions;
		if (!Source->TryGetArrayField(TEXT("PropertyPathDescriptions"), Descriptions)) return;

		for (const TSharedPtr<FJsonValue>& Description : *Descriptions) {
			OutSegmentPaths.Add(Description->AsObject()->GetStringField(TEXT("SegmentPath")));
		}
	};

	/* From 5.1 on the VM carries its own registers as a property bag, and that copy is the one the bytecode
	 * was compiled against. The generated memory classes are kept in sync only up to a point - a rig can ship
	 * with a stale class whose register list has drifted - so the bag wins wherever it exists. */
	const TSharedPtr<FJsonObject>* Bag;

	if (Storage->TryGetObjectField(StorageName, Bag)) {
		const TArray<TSharedPtr<FJsonValue>>* Descriptors;

		if ((*Bag)->TryGetArrayField(TEXT("PropertyDescs"), Descriptors) && Descriptors->Num() > 0) {
			for (const TSharedPtr<FJsonValue>& Descriptor : *Descriptors) {
				OutNames.Add(Descriptor->AsObject()->GetStringField(TEXT("Name")));
			}

			ReadSegmentPaths(*Bag);

			return;
		}
	}

	/* Before that the registers were the properties of a generated memory class */
	for (FUObjectExport* Export : Container->Exports) {
		if (!Export->JsonObject.IsValid()) continue;

		double MemoryType = -1.0;
		if (!Export->JsonObject->TryGetNumberField(TEXT("MemoryType"), MemoryType)) continue;

		const bool bIsLiteral = static_cast<int32>(MemoryType) == LiteralMemory;
		if (bIsLiteral != StorageName.Contains(TEXT("Literal"))) continue;

		const TArray<TSharedPtr<FJsonValue>>* ChildProperties;
		if (!Export->JsonObject->TryGetArrayField(TEXT("ChildProperties"), ChildProperties)) continue;

		for (const TSharedPtr<FJsonValue>& ChildProperty : *ChildProperties) {
			OutNames.Add(ChildProperty->AsObject()->GetStringField(TEXT("Name")));
		}

		ReadSegmentPaths(Export->JsonObject);

		return;
	}
}

/* ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ Variables ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ */

namespace {
	/*
	 * An exported property says what it is by naming its property class; RigVM states the same thing as a C++
	 * type name. Both are the engine's own vocabulary and this is only a translation between them - anything
	 * with a type of its own carries that separately, as the object the description points at.
	 */
	const TMap<FString, FString> ScalarCPPTypes = {
		{ TEXT("BoolProperty"),   RigVMTypeUtils::BoolType },
		{ TEXT("FloatProperty"),  RigVMTypeUtils::FloatType },
		{ TEXT("DoubleProperty"), RigVMTypeUtils::DoubleType },
		{ TEXT("IntProperty"),    RigVMTypeUtils::Int32Type },
		{ TEXT("UInt32Property"), RigVMTypeUtils::UInt32Type },
		{ TEXT("ByteProperty"),   RigVMTypeUtils::UInt8Type },
		{ TEXT("NameProperty"),   RigVMTypeUtils::FNameType },
		{ TEXT("StrProperty"),    RigVMTypeUtils::FStringType },
		{ TEXT("TextProperty"),   RigVMTypeUtils::FTextType },

		/* RigVM only names the widths it has always supported; the 64 bit ones are spelled out so a variable
		 * that uses them still resolves on the engine versions that do know them */
		{ TEXT("Int64Property"),  TEXT("int64") },
		{ TEXT("UInt64Property"), TEXT("uint64") },
	};

	/* Type references export as an object path; the name inside it is enough for reflection to find the type */
	UObject* ResolveTypeObject(const TSharedPtr<FJsonObject>& Description, const FString& FieldName) {
		const TSharedPtr<FJsonObject>* Reference;
		if (!Description->TryGetObjectField(FieldName, Reference)) return nullptr;

		FString ObjectName;
		if (!(*Reference)->TryGetStringField(TEXT("ObjectName"), ObjectName)) return nullptr;

		ObjectName = GetObjectNameFromOuter(ObjectName);
		if (ObjectName.IsEmpty()) return nullptr;

#if UE5_1_BEYOND
		return FindFirstObjectSafe<UField>(*ObjectName);
#else
		return FindObject<UField>(ANY_PACKAGE, *ObjectName);
#endif
	}
}

bool FRigVMGraphBuilder::ReadVariableType(const TSharedPtr<FJsonObject>& Description, FVariableType& OutType) const {
	FString PropertyType;
	if (!Description->TryGetStringField(TEXT("Type"), PropertyType)) return false;

	/* An array is its element's type, wrapped */
	if (PropertyType == TEXT("ArrayProperty")) {
		const TSharedPtr<FJsonObject>* Inner;
		if (!Description->TryGetObjectField(TEXT("Inner"), Inner)) return false;

		if (!ReadVariableType(*Inner, OutType)) return false;

		OutType.CPPType = RigVMTypeUtils::ArrayTypeFromBaseType(OutType.CPPType);

		return true;
	}

	/* Anything with a type of its own names it, and the name is what states the C++ type */
	if (UObject* Struct = ResolveTypeObject(Description, TEXT("Struct"))) {
		OutType.CPPType = TEXT("F") + Struct->GetName();
		OutType.CPPTypeObject = Struct;

		return true;
	}

	if (UObject* Enum = ResolveTypeObject(Description, TEXT("Enum"))) {
		OutType.CPPType = Enum->GetName();
		OutType.CPPTypeObject = Enum;

		return true;
	}

	if (UObject* Class = ResolveTypeObject(Description, TEXT("PropertyClass"))) {
		OutType.CPPType = FString::Printf(TEXT("TObjectPtr<U%s>"), *Class->GetName());
		OutType.CPPTypeObject = Class;

		return true;
	}

	const FString* ScalarType = ScalarCPPTypes.Find(PropertyType);
	if (ScalarType == nullptr) return false;

	OutType.CPPType = *ScalarType;

	return true;
}

/*
 * A cooked rig's variables live on its generated class, which is thrown away and rebuilt from scratch when the
 * asset is recreated - so before any getter or setter can be spawned, the variables themselves have to exist on
 * the blueprint again. Their names and types come straight out of the class that was exported.
 */
void FRigVMGraphBuilder::BuildVariables() {
	for (const TSharedPtr<FJsonObject>& Description : ExternalVariableDescriptions) {
		const FString VariableName = Description->GetStringField(TEXT("Name"));

		FVariableType VariableType;
		if (!ReadVariableType(Description, VariableType)) {
			UE_LOG(LogReflection, Warning, TEXT("Could not work out the type of rig variable \"%s\""), *VariableName);
			continue;
		}

		VariableTypes.Add(VariableName, VariableType);

		const FName Name = FName(*VariableName);

		/* A blueprint made from a parent that already declares it needs nothing adding */
		const bool bAlreadyDeclared = Blueprint->NewVariables.ContainsByPredicate(
			[&Name](const FBPVariableDescription& Variable) { return Variable.VarName == Name; }
		);

		if (bAlreadyDeclared) {
			UE_LOG(LogReflection, Log, TEXT("Rig variable \"%s\" is already declared"), *VariableName);
			continue;
		}

		const FEdGraphPinType PinType = RigVMTypeUtils::PinTypeFromCPPType(*VariableType.CPPType, VariableType.CPPTypeObject);

		if (FBlueprintEditorUtils::AddMemberVariable(Blueprint, Name, PinType)) {
			UE_LOG(LogReflection, Log, TEXT("Added rig variable \"%s\" of type %s"), *VariableName, *VariableType.CPPType);
		} else {
			UE_LOG(LogReflection, Warning, TEXT("Could not add rig variable \"%s\" of type %s"), *VariableName, *VariableType.CPPType);
		}
	}
}

/* ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ Nodes ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ */

FRigVMGraphBuilder::FCallTarget FRigVMGraphBuilder::ResolveCallTarget(const FString& FunctionName) const {
	FCallTarget Target;

	/* Unit functions read "FRigUnit_Something::Execute"; dispatches read
	 * "DISPATCH_RigVMDispatch_If::Condition:bool,True:float,..." */
	FString Head, Tail;
	if (!FunctionName.Split(TEXT("::"), &Head, &Tail)) return Target;

	static const FString DispatchPrefix = TEXT("DISPATCH_");

	if (Head.StartsWith(DispatchPrefix)) {
		/* A factory registers itself under its dispatch prefix and answers to nothing else - and the registry
		 * only bothers looking one up on demand for names carrying that prefix, so the head goes in whole */
		if (const FRigVMDispatchFactory* Factory = FRigVMRegistry::Get().FindDispatchFactory(FName(*Head))) {
			Target.TemplateNotation = Factory->GetTemplateNotation();
		}

		if (Target.TemplateNotation.IsNone()) {
			UE_LOG(LogReflection, Warning, TEXT("No dispatch factory for RigVM function \"%s\""), *FunctionName);
		}

		/* The remainder of the signature spells out every argument as "Name:Type" */
		TArray<FString> Arguments;
		Tail.ParseIntoArray(Arguments, TEXT(","), true);

		for (const FString& Argument : Arguments) {
			FString ArgumentName;
			Target.PinNames.Add(Argument.Split(TEXT(":"), &ArgumentName, nullptr) ? ArgumentName : Argument);
		}

		return Target;
	}


	/* Struct names are exported with their C++ prefix, which reflection doesn't use */
	const FString StructName = Head.RightChop(1);

#if UE5_1_BEYOND
	Target.ScriptStruct = FindFirstObjectSafe<UScriptStruct>(*StructName);
#else
	Target.ScriptStruct = FindObject<UScriptStruct>(ANY_PACKAGE, *StructName);
#endif

	Target.MethodName = FName(*Tail);

	if (Target.ScriptStruct == nullptr) {
		UE_LOG(LogReflection, Warning, TEXT("No script struct for RigVM function \"%s\""), *FunctionName);
		return Target;
	}

	/* Operands arrive in memory layout order, which puts anything inherited ahead of the struct's own
	 * properties - the opposite of how TFieldIterator walks a struct, hence the chain being reversed first. */
	TArray<const UStruct*> Chain;

	for (const UStruct* Struct = Target.ScriptStruct; Struct != nullptr; Struct = Struct->GetSuperStruct()) {
		Chain.Insert(Struct, 0);
	}

	for (const UStruct* Struct : Chain) {
		for (TFieldIterator<FProperty> PropertyIterator(Struct, EFieldIteratorFlags::ExcludeSuper); PropertyIterator; ++PropertyIterator) {
			/* The execution wire is a property like any other, but it never becomes an operand - leaving it in
			 * would shift every pin after it out of step with the instruction's arguments */
			const FStructProperty* StructProperty = CastField<FStructProperty>(*PropertyIterator);

			if (StructProperty != nullptr && StructProperty->Struct != nullptr && StructProperty->Struct->IsChildOf(FRigVMExecuteContext::StaticStruct())) {
				continue;
			}

			Target.PinNames.Add(PropertyIterator->GetName());
		}
	}

	return Target;
}

FString FRigVMGraphBuilder::GetRegisterGraph(const FString& RegisterName) {
	const int32 Separator = RegisterName.Find(GraphSeparator, ESearchCase::CaseSensitive, ESearchDir::FromEnd);

	return Separator == INDEX_NONE ? FString() : RegisterName.Left(Separator);
}

/*
 * Every graph other than the one holding the rig's event was a function that the compiler inlined, so it goes
 * back into the local function library and its nodes are built through its own controller. That turns a few
 * hundred nodes in one line back into the handful of graphs they were authored as.
 */
URigVMController* FRigVMGraphBuilder::GetController(const FString& GraphName) {
	if (GraphName.IsEmpty() || GraphName == RootGraphName) return Controller;

	if (URigVMController** Existing = Controllers.Find(GraphName)) return *Existing;

	URigVMController* FunctionController = nullptr;

	if (URigVMLibraryNode* Function = Controller->AddFunctionToLibrary(FName(*GraphName), true, FVector2D::ZeroVector, false)) {
		if (URigVMGraph* ContainedGraph = Function->GetContainedGraph()) {
			FunctionController = Blueprint->GetOrCreateController(ContainedGraph);
		}
	}

	if (FunctionController == nullptr) {
		UE_LOG(LogReflection, Warning, TEXT("Could not add the function \"%s\"; its nodes go to the root graph"), *GraphName);
		FunctionController = Controller;
	} else {
		UE_LOG(LogReflection, Log, TEXT("Added the function \"%s\" to the library"), *GraphName);
	}

	Controllers.Add(GraphName, FunctionController);

	return FunctionController;
}

bool FRigVMGraphBuilder::MatchRegisterToPin(const FString& RegisterName, const FString& PinName, FString& OutNodeName) {
	FString Name = RegisterName;

	for (const FString& Suffix : RegisterSuffixes) {
		if (Name.EndsWith(Suffix)) {
			Name = Name.LeftChop(Suffix.Len());
			break;
		}
	}

	/* Drop the graph the register was compiled for; what's left is "<Node>_<Pin>" */
	const int32 Separator = Name.Find(GraphSeparator, ESearchCase::CaseSensitive, ESearchDir::FromEnd);

	if (Separator != INDEX_NONE) {
		Name = Name.RightChop(Separator + GraphSeparator.Len());
	}

	if (Name.Len() <= PinName.Len() + 1) return false;
	if (!Name.EndsWith(PinName, ESearchCase::CaseSensitive)) return false;
	if (Name[Name.Len() - PinName.Len() - 1] != TEXT('_')) return false;

	OutNodeName = Name.LeftChop(PinName.Len() + 1);

	return !OutNodeName.IsEmpty();
}

/*
 * The compiler shares identical literals between nodes, so a register can perfectly well be named after some
 * other node that needed the same value first - and on a rig with several copies of the same unit, those
 * borrowed names can outnumber the node's own. What separates them is position: a register only belongs to
 * this instruction if it sits at the operand for the pin it is named after. Work registers weigh far more than
 * literals on top of that, because a literal is exactly the kind of thing that gets shared and a node's own
 * scratch and output registers never are.
 */
FRigVMGraphBuilder::FNodeReference FRigVMGraphBuilder::ResolveNode(const FCallTarget& Target, const TArray<TSharedPtr<FJsonValue>>& Operands) const {
	constexpr int32 WorkWeight = 10;
	constexpr int32 LiteralWeight = 1;

	TMap<FString, int32> Votes;

	FNodeReference Best;
	int32 BestVote = 0;

	for (int32 Index = 0; Index < Operands.Num() && Index < Target.PinNames.Num(); Index++) {
		const TSharedPtr<FJsonObject> Operand = Operands[Index]->AsObject();
		const int32 MemoryType = GetNumber(Operand, TEXT("MemoryType"), INDEX_NONE);

		if (MemoryType != WorkMemory && MemoryType != LiteralMemory) continue;

		const FString RegisterName = GetRegisterName(Operand);

		FString NodeName;
		if (!MatchRegisterToPin(RegisterName, Target.PinNames[Index], NodeName)) continue;

		/* A node's graph and its name are decided together - the register that identifies one names the other */
		const FString Key = GetRegisterGraph(RegisterName) + GraphSeparator + NodeName;

		const int32 Vote = Votes.FindOrAdd(Key) + (MemoryType == WorkMemory ? WorkWeight : LiteralWeight);
		Votes[Key] = Vote;

		if (Vote > BestVote) {
			BestVote = Vote;
			Best = { GetRegisterGraph(RegisterName), NodeName };
		}
	}

	return Best;
}

URigVMNode* FRigVMGraphBuilder::CreateNode(const FCallTarget& Target, const FNodeReference& Node) {
	const FString Key = Node.Graph + GraphSeparator + Node.Name;

	if (URigVMNode** Existing = NodesByName.Find(Key)) return *Existing;

	URigVMController* GraphController = GetController(Node.Graph);

	/* Each graph lays its own nodes out, so a function doesn't inherit the root graph's column */
	int32& Column = NodeColumns.FindOrAdd(Node.Graph);
	const FVector2D Position(Column * 420.0f, (Column % 2) * 60.0f);
	Column++;

	URigVMNode* CreatedNode = Target.ScriptStruct != nullptr
		? Cast<URigVMNode>(GraphController->AddUnitNodeFromStructPath(Target.ScriptStruct->GetPathName(), Target.MethodName, Position, Node.Name, false))
		: Cast<URigVMNode>(GraphController->AddTemplateNode(Target.TemplateNotation, Position, Node.Name, false));

	if (CreatedNode == nullptr) {
		UE_LOG(LogReflection, Warning, TEXT("Failed to create RigVM node \"%s\""), *Node.Name);
		return nullptr;
	}

	/*
	 * A node whose every pin is an execution wire holds nothing but the shape of the branching, and that shape
	 * is the one thing compiling destroys - a sequence driving two subtrees emits the same bytecode as the two
	 * subtrees run one after the other. Rebuilding it would mean inventing a grouping the file doesn't record,
	 * so it's dropped and the execution simply flows on. Entry points are exempt: they carry no pins either,
	 * but which event a graph answers to is real information.
	 */
	if (!CreatedNode->IsEvent() && !CreatedNode->GetPins().ContainsByPredicate([](const URigVMPin* Pin) { return !Pin->IsExecuteContext(); })) {
		GraphController->RemoveNode(CreatedNode, false);

		return nullptr;
	}

	NodesByName.Add(Key, CreatedNode);
	NodeGraphs.Add(CreatedNode, Node.Graph);

	return CreatedNode;
}

void FRigVMGraphBuilder::BuildNodes() {
	/*
	 * Which graph the registers call the root isn't something to assume - the blueprint names its own model
	 * however the engine version felt like, and the compiler wrote down whatever it was called when the rig was
	 * authored. Instructions run in order and a rig starts in its own graph, so the first one that names a graph
	 * names the root, and every other graph after it was a function that got inlined into this same stream.
	 */
	for (int32 Index = 0; Index < Instructions.Num(); Index++) {
		const TSharedPtr<FJsonObject> Instruction = Instructions[Index]->AsObject();
		if (!Instruction.IsValid() || !Instruction->HasField(TEXT("FunctionIndex"))) continue;

		const int32 FunctionIndex = GetNumber(Instruction, TEXT("FunctionIndex"), INDEX_NONE);
		if (!FunctionNames.IsValidIndex(FunctionIndex)) continue;

		const TArray<TSharedPtr<FJsonValue>>* Operands;
		if (!Instruction->TryGetArrayField(TEXT("Arguments"), Operands)) continue;

		const FNodeReference NodeReference = ResolveNode(ResolveCallTarget(FunctionNames[FunctionIndex]), *Operands);

		if (NodeReference.IsValid() && !NodeReference.Graph.IsEmpty()) {
			RootGraphName = NodeReference.Graph;
			break;
		}
	}

	for (int32 Index = 0; Index < Instructions.Num(); Index++) {
		const TSharedPtr<FJsonObject> Instruction = Instructions[Index]->AsObject();

		/* Only the execute instructions carry a function; everything else moves data around */
		if (!Instruction.IsValid() || !Instruction->HasField(TEXT("FunctionIndex"))) continue;

		const int32 FunctionIndex = GetNumber(Instruction, TEXT("FunctionIndex"), INDEX_NONE);
		if (!FunctionNames.IsValidIndex(FunctionIndex)) continue;

		const FCallTarget Target = ResolveCallTarget(FunctionNames[FunctionIndex]);

		if (!Target.IsValid()) {
			UE_LOG(LogReflection, Warning, TEXT("Skipping instruction %d, nothing to build \"%s\" from"), Index, *FunctionNames[FunctionIndex]);
			continue;
		}

		const TArray<TSharedPtr<FJsonValue>>* Operands;
		static const TArray<TSharedPtr<FJsonValue>> NoOperands;

		if (!Instruction->TryGetArrayField(TEXT("Arguments"), Operands)) {
			Operands = &NoOperands;
		}

		FNodeReference NodeReference = ResolveNode(Target, *Operands);

		/* Nodes with no operands at all (an entry point, a sequence) leave nothing to name them after, and
		 * nothing to place them by either - the rig's own event is what the root graph is for */
		if (!NodeReference.IsValid()) {
			FString BaseName = Target.ScriptStruct != nullptr ? Target.ScriptStruct->GetName() : Target.TemplateNotation.ToString();

			/* A template notation carries its arguments, which have no business being in a node name */
			int32 ArgumentList = INDEX_NONE;
			if (BaseName.FindChar(TEXT('('), ArgumentList)) {
				BaseName.LeftInline(ArgumentList);
			}

			NodeReference = { RootGraphName, FString::Printf(TEXT("%s_%d"), *BaseName, Index) };
		}

		URigVMNode* Node = CreateNode(Target, NodeReference);
		if (Node == nullptr) continue;

		InstructionNodes.Add(Index, Node);

		/* Operands come in the order the node declares its pins, minus the execution wire, so the two line up
		 * positionally. That beats matching names, which the shared literals would get wrong. */
		TArray<URigVMPin*> Pins;

		for (URigVMPin* Pin : Node->GetPins()) {
			if (!Pin->IsExecuteContext()) Pins.Add(Pin);
		}

		if (Pins.Num() != Operands->Num()) {
			UE_LOG(LogReflection, Warning, TEXT("\"%s\" has %d pins for %d operands, its values may be off"), *NodeReference.Name, Pins.Num(), Operands->Num());
		}

		for (int32 OperandIndex = 0; OperandIndex < Operands->Num() && OperandIndex < Pins.Num(); OperandIndex++) {
			const TSharedPtr<FJsonObject> Operand = (*Operands)[OperandIndex]->AsObject();
			const FString RegisterName = GetRegisterName(Operand);

			if (RegisterName.IsEmpty()) continue;

			const FString SegmentSuffix = GetSegmentSuffix(Operand);
			const FPinReference Pin = { NodeReference.Graph, Pins[OperandIndex]->GetPinPath() + SegmentSuffix };

			/* Remembering the property behind a whole pin is what lets its value be written out by its real type
			 * later on, whichever instruction turns out to supply it */
			if (SegmentSuffix.IsEmpty() && Target.ScriptStruct != nullptr) {
				PinProperties.Add(Pin.Graph + GraphSeparator + Pin.Path, Target.ScriptStruct->FindPropertyByName(Pins[OperandIndex]->GetFName()));
			}

			if (GetNumber(Operand, TEXT("MemoryType")) == LiteralMemory) {
				SetPinDefaultValue(Pin, RegisterName);
				continue;
			}

			/* Whichever node reaches a work register first is the one that produces it. An operand addressing
			 * only part of the register isn't that claim - it's a wire into one of its sub-pins. */
			if (SegmentSuffix.IsEmpty()) {
				RegisterOwners.FindOrAdd(RegisterName, Pin);
			}
		}
	}
}

/* ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ Links ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ */

FString FRigVMGraphBuilder::GetRegisterName(const TSharedPtr<FJsonObject>& Operand) const {
	if (!Operand.IsValid()) return FString();

	const int32 RegisterIndex = GetNumber(Operand, TEXT("RegisterIndex"), INDEX_NONE);

	switch (GetNumber(Operand, TEXT("MemoryType"), INDEX_NONE)) {
		case WorkMemory:     return WorkRegisters.IsValidIndex(RegisterIndex) ? WorkRegisters[RegisterIndex] : FString();
		case LiteralMemory:  return LiteralRegisters.IsValidIndex(RegisterIndex) ? LiteralRegisters[RegisterIndex] : FString();
		case ExternalMemory: return ExternalVariables.IsValidIndex(RegisterIndex) ? ExternalVariables[RegisterIndex] : FString();
		default:             return FString();
	}
}

FString FRigVMGraphBuilder::GetSegmentSuffix(const TSharedPtr<FJsonObject>& Operand) const {
	const int32 Offset = GetNumber(Operand, TEXT("RegisterOffset"), NoRegisterOffset);
	if (Offset == NoRegisterOffset) return FString();

	/* Each block of memory numbers its own paths, and a variable's live with the machine. Segment paths are
	 * already written the way a pin path spells its sub-pins, so they need no translating. */
	const TArray<FString>* SegmentPaths;

	switch (GetNumber(Operand, TEXT("MemoryType"), INDEX_NONE)) {
		case LiteralMemory:  SegmentPaths = &LiteralSegmentPaths; break;
		case ExternalMemory: SegmentPaths = &ExternalSegmentPaths; break;
		default:             SegmentPaths = &WorkSegmentPaths; break;
	}

	return SegmentPaths->IsValidIndex(Offset) ? TEXT(".") + (*SegmentPaths)[Offset] : FString();
}

FRigVMGraphBuilder::FPinReference FRigVMGraphBuilder::GetPin(const TSharedPtr<FJsonObject>& Operand) const {
	const FPinReference* Pin = RegisterOwners.Find(GetRegisterName(Operand));

	return Pin != nullptr ? FPinReference{ Pin->Graph, Pin->Path + GetSegmentSuffix(Operand) } : FPinReference();
}

void FRigVMGraphBuilder::Link(const FPinReference& Output, const FPinReference& Input) {
	if (!Output.IsValid() || !Input.IsValid() || Output.Path == Input.Path) return;

	/* A wire can only exist inside one graph. Two ends in different graphs were a function's arguments, and
	 * those crossed a boundary the compiler dissolved when it inlined the call. */
	if (Output.Graph != Input.Graph) {
		UE_LOG(LogReflection, Verbose, TEXT("%s and %s sit in different graphs; that link was a function argument"), *Output.Path, *Input.Path);
		return;
	}

	URigVMController* GraphController = GetController(Output.Graph);

	/* Which end is the output is decided by the pins themselves, so both orders get a try */
	if (GraphController->AddLink(Output.Path, Input.Path, false)) return;
	if (GraphController->AddLink(Input.Path, Output.Path, false)) return;

	UE_LOG(LogReflection, Warning, TEXT("Could not link %s to %s"), *Output.Path, *Input.Path);
}

void FRigVMGraphBuilder::LinkVariable(const int32 InstructionIndex, const FString& VariableName, const FString& ValueSegment, const FPinReference& Pin, const bool bIsGetter) {
	const TCHAR* Access = bIsGetter ? TEXT("getter") : TEXT("setter");

	/* No pin means the register this variable feeds was never claimed by a node, so there is nothing to attach
	 * to - which points at the node that should own it having failed rather than at the variable */
	if (!Pin.IsValid()) {
		UE_LOG(LogReflection, Warning, TEXT("No pin for the \"%s\" %s; the node that owns it is missing"), *VariableName, Access);
		return;
	}

	const FVariableType* VariableType = VariableTypes.Find(VariableName);

	if (VariableType == nullptr) {
		UE_LOG(LogReflection, Warning, TEXT("No declared type for rig variable \"%s\""), *VariableName);
		return;
	}

	/* The getter or setter belongs beside whatever it feeds, which is what decides the graph it goes in */
	URigVMController* GraphController = GetController(Pin.Graph);

	int32& Column = NodeColumns.FindOrAdd(Pin.Graph);
	const FVector2D Position(Column * 420.0f, -220.0f);
	Column++;

	URigVMNode* Node = GraphController->AddVariableNodeFromObjectPath(
		FName(*VariableName),
		VariableType->CPPType,
		VariableType->CPPTypeObject != nullptr ? VariableType->CPPTypeObject->GetPathName() : FString(),
		bIsGetter,
		FString(),
		Position,
		FString(),
		false
	);

	if (Node == nullptr) {
		UE_LOG(LogReflection, Warning, TEXT("Could not spawn the \"%s\" %s node"), *VariableName, Access);
		return;
	}

	/* Writing a variable is an action rather than a value, so a setter has to sit in the run of execution or it
	 * simply never happens. The copy instruction that stood for it says where: between whichever nodes ran on
	 * either side of it. Getters have no execution wire and want no place in that run. */
	NodeGraphs.Add(Node, Pin.Graph);

	if (!bIsGetter) {
		InstructionNodes.Add(InstructionIndex, Node);
	}

	for (URigVMPin* ValuePin : Node->GetPins()) {
		if (ValuePin->GetName() != TEXT("Value")) continue;

		/* Reading or writing one member of a struct variable addresses that member's sub-pin, not the whole thing */
		const FPinReference Value = { Pin.Graph, ValuePin->GetPinPath() + ValueSegment };

		if (bIsGetter) {
			Link(Value, Pin);
		} else {
			Link(Pin, Value);
		}

		return;
	}
}

void FRigVMGraphBuilder::BuildLinks() {
	for (int32 Index = 0; Index < Instructions.Num(); Index++) {
		const TSharedPtr<FJsonObject> Instruction = Instructions[Index]->AsObject();
		if (!Instruction.IsValid()) continue;

		/* A copy is a wire the compiler turned into an instruction */
		if (Instruction->HasField(TEXT("Source")) && Instruction->HasField(TEXT("Target"))) {
			const TSharedPtr<FJsonObject> Source = Instruction->GetObjectField(TEXT("Source"));
			const TSharedPtr<FJsonObject> Target = Instruction->GetObjectField(TEXT("Target"));

			const int32 SourceMemory = GetNumber(Source, TEXT("MemoryType"), INDEX_NONE);
			const int32 TargetMemory = GetNumber(Target, TEXT("MemoryType"), INDEX_NONE);

			/* One end being a class property means a variable node stood there */
			if (SourceMemory == ExternalMemory) {
				LinkVariable(Index, GetRegisterName(Source), GetSegmentSuffix(Source), GetPin(Target), true);
			} else if (TargetMemory == ExternalMemory) {
				LinkVariable(Index, GetRegisterName(Target), GetSegmentSuffix(Target), GetPin(Source), false);
			} else if (SourceMemory == LiteralMemory) {
				SetPinDefaultValue(GetPin(Target), GetRegisterName(Source));
			} else {
				Link(GetPin(Source), GetPin(Target));
			}

			continue;
		}

		/* Two instructions naming the same work register are two ends of the same wire */
		if (!InstructionNodes.Contains(Index)) continue;

		const URigVMNode* Node = InstructionNodes[Index];

		const TArray<TSharedPtr<FJsonValue>>* Operands;
		if (!Instruction->TryGetArrayField(TEXT("Arguments"), Operands)) continue;

		const FString GraphName = NodeGraphs.FindRef(Node);

		TArray<URigVMPin*> Pins;

		for (URigVMPin* Pin : Node->GetPins()) {
			if (!Pin->IsExecuteContext()) Pins.Add(Pin);
		}

		for (int32 OperandIndex = 0; OperandIndex < Operands->Num() && OperandIndex < Pins.Num(); OperandIndex++) {
			const TSharedPtr<FJsonObject> Operand = (*Operands)[OperandIndex]->AsObject();

			if (GetNumber(Operand, TEXT("MemoryType"), INDEX_NONE) != WorkMemory) continue;

			Link(GetPin(Operand), { GraphName, Pins[OperandIndex]->GetPinPath() + GetSegmentSuffix(Operand) });
		}
	}
}

void FRigVMGraphBuilder::BuildExecutionChain() {
	/* The authored execution wiring is gone - the compiler flattened it into instruction order and the branch
	 * boundaries went with it. Chaining the nodes in the order the VM ran them reproduces the behaviour even
	 * where it doesn't reproduce the drawing. Each graph runs its own chain, since the compiler interleaves the
	 * instructions of an inlined function with those of whatever called it. */
	TMap<FString, URigVMPin*> PreviousOutputs;

	for (int32 Index = 0; Index < Instructions.Num(); Index++) {
		URigVMNode** Node = InstructionNodes.Find(Index);
		if (Node == nullptr) continue;

		const FString GraphName = NodeGraphs.FindRef(*Node);
		URigVMPin*& PreviousOutput = PreviousOutputs.FindOrAdd(GraphName);

		URigVMPin* Input = nullptr;
		URigVMPin* Output = nullptr;
		URigVMPin* SharedPin = nullptr;

		for (URigVMPin* Pin : (*Node)->GetPins()) {
			if (!Pin->IsExecuteContext()) continue;

			switch (Pin->GetDirection()) {
				case ERigVMPinDirection::Input:
					if (Input == nullptr) Input = Pin;
					break;

				/* A node that branches offers several outputs; the first free one continues the chain, so a
				 * sequence fills its slots in order rather than leaving the earlier ones empty */
				case ERigVMPinDirection::Output:
					if (Output == nullptr) Output = Pin;
					break;

				/* Most nodes just pass execution through a single pin that serves as both ends */
				default:
					if (SharedPin == nullptr) SharedPin = Pin;
					break;
			}
		}

		if (Input == nullptr) Input = SharedPin;
		if (Output == nullptr) Output = SharedPin;

		if (Input != nullptr && PreviousOutput != nullptr) {
			GetController(GraphName)->AddLink(PreviousOutput->GetPinPath(), Input->GetPinPath(), false);
		}

		if (Output != nullptr) PreviousOutput = Output;
	}
}

/* ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ Values ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ */

/*
 * Values are turned into text by the property they belong to rather than by reading the json's shape, because
 * an export writes whatever its own types expose - a quaternion arrives with its length and normalized flag
 * alongside X, Y, Z and W, and none of those are fields the engine will accept back. Handing the json to the
 * property serialiser sidesteps all of that: it looks each field up by name on the real type and ignores
 * anything that doesn't belong, and the engine then writes the value out in exactly the form it expects to read.
 */
FString FRigVMGraphBuilder::ToPinDefaultValue(const FPinReference& Pin, const TSharedPtr<FJsonValue>& Value) const {
	FProperty* const* Property = PinProperties.Find(Pin.Graph + GraphSeparator + Pin.Path);

	if (Property == nullptr || *Property == nullptr || PropertySerializer == nullptr || !Value.IsValid()) {
		return ToLiteralText(Value, false);
	}

	UScriptStruct* OwnerStruct = Cast<UScriptStruct>((*Property)->GetOwnerStruct());
	if (OwnerStruct == nullptr) return ToLiteralText(Value, false);

	/* The property needs a container to live in, so a throwaway instance of the struct that declares it */
	void* Storage = FMemory::Malloc(OwnerStruct->GetStructureSize(), OwnerStruct->GetMinAlignment());
	OwnerStruct->InitializeStruct(Storage);

	void* PropertyValue = (*Property)->ContainerPtrToValuePtr<void>(Storage);

	FString Text;
	PropertySerializer->DeserializePropertyValue(*Property, Value.ToSharedRef(), PropertyValue);

#if UE5_1_BEYOND
	(*Property)->ExportTextItem_Direct(Text, PropertyValue, nullptr, nullptr, PPF_None);
#else
	(*Property)->ExportTextItem(Text, PropertyValue, nullptr, nullptr, PPF_None);
#endif

	OwnerStruct->DestroyStruct(Storage);
	FMemory::Free(Storage);

	return Text;
}

void FRigVMGraphBuilder::SetPinDefaultValue(const FPinReference& Pin, const FString& RegisterName) {
	if (!Pin.IsValid()) return;

	const TSharedPtr<FJsonValue>* Value = LiteralValues.Find(RegisterName);
	if (Value == nullptr) return;

	GetController(Pin.Graph)->SetPinDefaultValue(Pin.Path, ToPinDefaultValue(Pin, *Value), true, false);
}

FString FRigVMGraphBuilder::ToLiteralText(const TSharedPtr<FJsonValue>& Value, const bool bNested) {
	if (!Value.IsValid()) return FString();

	switch (Value->Type) {
		case EJson::Boolean:
			return Value->AsBool() ? TEXT("True") : TEXT("False");

		case EJson::Number: {
			const double Number = Value->AsNumber();

			return FMath::IsNearlyEqual(Number, FMath::RoundToDouble(Number)) && FMath::Abs(Number) < static_cast<double>(TNumericLimits<int64>::Max())
				? FString::Printf(TEXT("%lld"), static_cast<int64>(FMath::RoundToDouble(Number)))
				: FString::SanitizeFloat(Number);
		}

		case EJson::String: {
			FString Text = Value->AsString();

			/* Enums export fully qualified, but a default value only wants the entry */
			FString EnumEntry;
			if (Text.Split(TEXT("::"), nullptr, &EnumEntry, ESearchCase::CaseSensitive, ESearchDir::FromEnd)) {
				return EnumEntry;
			}

			return bNested ? FString::Printf(TEXT("\"%s\""), *Text) : Text;
		}

		case EJson::Array: {
			TArray<FString> Elements;

			for (const TSharedPtr<FJsonValue>& Element : Value->AsArray()) {
				Elements.Add(ToLiteralText(Element, true));
			}

			return FString::Printf(TEXT("(%s)"), *FString::Join(Elements, TEXT(",")));
		}

		case EJson::Object: {
			const TSharedPtr<FJsonObject> Object = Value->AsObject();

			/* An exported object reference is a path, not a struct */
			if (Object->HasField(TEXT("ObjectPath")) && Object->HasField(TEXT("ObjectName"))) {
				return Object->GetStringField(TEXT("ObjectPath"));
			}

			TArray<FString> Fields;

			for (const TPair<FString, TSharedPtr<FJsonValue>>& Field : Object->Values) {
				Fields.Add(FString::Printf(TEXT("%s=%s"), *Field.Key, *ToLiteralText(Field.Value, true)));
			}

			return FString::Printf(TEXT("(%s)"), *FString::Join(Fields, TEXT(",")));
		}

		default:
			return FString();
	}
}
