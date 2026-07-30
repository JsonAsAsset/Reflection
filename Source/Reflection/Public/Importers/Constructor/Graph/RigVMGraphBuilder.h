/* Copyright Reflection Contributors 2024-2026 */

#pragma once

#include "CoreMinimal.h"
#include "Containers/ExportContainer.h"

class UBlueprint;
class URigVMBlueprint;
class URigVMController;
class URigVMNode;

/*
 * Rebuilds a RigVM node graph out of the compiled bytecode a cooked asset ships with.
 *
 * Cooking throws the authored graph away and keeps only the virtual machine: a flat instruction list, a table
 * of the functions those instructions call, and the register tables they address. That turns out to be enough
 * to get the graph back, because the compiler leaves its working out in the register names - it generates one
 * register per pin, named "<Graph>___<Node>_<Pin>", and it names every instruction after the script struct or
 * dispatch factory it runs.
 *
 * So the type of every node comes from the engine's own reflection (the struct behind the function name), the
 * pins come from that struct, the values come from literal memory, and a link is nothing more than two
 * instructions addressing the same register. Nothing in here knows about any particular rig unit.
 *
 * Functions are inlined by the compiler into that one stream, but the register names still say which graph
 * each node was authored in, so the graphs are put back: the rig's own event keeps the root graph and every
 * other one becomes a function in the local library rather than a few hundred nodes in a single line.
 *
 * What can't be recovered is the shape of the execution wiring: the compiler flattens it into instruction
 * order, and the branch boundaries a Sequence node used to draw are gone. The nodes are chained in the order
 * the VM runs them, which behaves identically but doesn't necessarily redraw the original layout.
 */
class REFLECTION_API FRigVMGraphBuilder {
public:
	FRigVMGraphBuilder(UBlueprint* InBlueprint, FUObjectExportContainer* InContainer, UPropertySerializer* InPropertySerializer);

	/* Rebuilds the graph. False when the blueprint hosts no RigVM graph, or the package shipped no bytecode. */
	bool Build();

private:
	/* A function an instruction can call: either a script struct's method, or a dispatch factory template */
	struct FCallTarget {
		UScriptStruct* ScriptStruct = nullptr;
		FName MethodName;
		FName TemplateNotation;

		/* The pins the call addresses, in the order its operands arrive */
		TArray<FString> PinNames;

		bool IsValid() const { return ScriptStruct != nullptr || !TemplateNotation.IsNone(); }
	};

	bool ReadVirtualMachine();
	void ReadLiteralValues(const TSharedPtr<FJsonObject>& Storage);
	void ReadRegisterNames(const TSharedPtr<FJsonObject>& Storage, const FString& StorageName, TArray<FString>& OutNames, TArray<FString>& OutSegmentPaths) const;

	/* An operand can address part of a register; that slice is a sub-pin of whatever the register belongs to */
	FString GetSegmentSuffix(const TSharedPtr<FJsonObject>& Operand) const;

	FCallTarget ResolveCallTarget(const FString& FunctionName) const;

	/* Where a node lives: the graph it was authored in, and its name within that graph */
	struct FNodeReference {
		FString Graph;
		FString Name;

		bool IsValid() const { return !Name.IsEmpty(); }
	};

	/* A pin on a node that exists, addressed the way its own graph's controller expects */
	struct FPinReference {
		FString Graph;
		FString Path;

		bool IsValid() const { return !Path.IsEmpty(); }
	};

	/* The graph a register was generated for, which is the one its node was authored in */
	static FString GetRegisterGraph(const FString& RegisterName);

	/* Peels "<Graph>___<Node>_<Pin>__Const" back to the node, given the pin it was generated for */
	static bool MatchRegisterToPin(const FString& RegisterName, const FString& PinName, FString& OutNodeName);

	/* Works out which node an instruction belongs to from the registers it addresses */
	FNodeReference ResolveNode(const FCallTarget& Target, const TArray<TSharedPtr<FJsonValue>>& Operands) const;

	/* The controller that edits a graph; anything other than the root graph is a function, made on demand */
	URigVMController* GetController(const FString& GraphName);

	URigVMNode* CreateNode(const FCallTarget& Target, const FNodeReference& Node);

	/* The type a variable was declared with, in the terms RigVM states types in */
	struct FVariableType {
		FString CPPType;
		UObject* CPPTypeObject = nullptr;
	};

	/* Reads an exported property description back into the type it declares */
	bool ReadVariableType(const TSharedPtr<FJsonObject>& Description, FVariableType& OutType) const;

	void BuildVariables();
	void BuildNodes();
	void BuildLinks();
	void BuildExecutionChain();

	/* Resolves an operand to the pin that owns it, invalid when no node ever claimed the register */
	FPinReference GetPin(const TSharedPtr<FJsonObject>& Operand) const;
	FString GetRegisterName(const TSharedPtr<FJsonObject>& Operand) const;

	void LinkVariable(int32 InstructionIndex, const FString& VariableName, const FString& ValueSegment, const FPinReference& Pin, bool bIsGetter);
	void Link(const FPinReference& Output, const FPinReference& Input);

	/* Turns an exported json value into the text form SetPinDefaultValue expects */
	FString ToPinDefaultValue(const FPinReference& Pin, const TSharedPtr<FJsonValue>& Value) const;

	/* Last resort for pins with no property to go by; builds the text by hand from the json's own shape */
	static FString ToLiteralText(const TSharedPtr<FJsonValue>& Value, bool bNested);

	void SetPinDefaultValue(const FPinReference& Pin, const FString& RegisterName);

	URigVMBlueprint* Blueprint = nullptr;
	FUObjectExportContainer* Container = nullptr;
	UPropertySerializer* PropertySerializer = nullptr;
	URigVMController* Controller = nullptr;

	/* Straight out of the VM export */
	TArray<FString> FunctionNames;
	TArray<TSharedPtr<FJsonValue>> Instructions;
	TArray<FString> WorkRegisters;
	TArray<FString> LiteralRegisters;
	TArray<FString> WorkSegmentPaths;
	TArray<FString> LiteralSegmentPaths;
	TArray<FString> ExternalSegmentPaths;
	TArray<FString> ExternalVariables;
	TArray<TSharedPtr<FJsonObject>> ExternalVariableDescriptions;

	/* Variable name -> the type it was declared with, which is what a getter or setter node needs */
	TMap<FString, FVariableType> VariableTypes;
	TMap<FString, TSharedPtr<FJsonValue>> LiteralValues;

	/* Register name -> the pin of the node that first claimed it, which is the one that owns it */
	TMap<FString, FPinReference> RegisterOwners;

	/* A pin -> the struct property it was generated from, which is what gives a value its type */
	TMap<FString, FProperty*> PinProperties;

	/* Instruction index -> the node it runs, for instructions that create one */
	TMap<int32, URigVMNode*> InstructionNodes;

	/* The graph holding the rig's own event; every other graph the compiler inlined was a function */
	FString RootGraphName;

	TMap<FString, URigVMController*> Controllers;
	TMap<FString, URigVMNode*> NodesByName;
	TMap<FString, int32> NodeColumns;

	/* Which graph each node ended up in, so its pins can be addressed by the controller that owns them */
	TMap<const URigVMNode*, FString> NodeGraphs;
};
