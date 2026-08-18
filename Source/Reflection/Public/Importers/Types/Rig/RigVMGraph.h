/* Copyright Reflection Contributors 2024-2026 */

#pragma once

#include "Engine/Compatibility.h"

#if REFLECTION_CONTROL_RIG

#include "CoreMinimal.h"
#include "Containers/JsonValueExport.h"

class UControlRigBlueprint;
class UPropertySerializer;
class URigVMController;
class URigVMGraph;
class URigVMNode;
class URigVMPin;

struct FRigVMFunction;

/* What came back out of a rig's bytecode, reported once the graph has been rebuilt */
struct FRigVMGraphStats {
	/* Instructions the bytecode holds, and the ones that turned into a node */
	int32 Instructions = 0;
	int32 Nodes = 0;

	int32 Links = 0;
	int32 Defaults = 0;
	int32 VariableNodes = 0;

	/* Functions this engine build doesn't have, so their nodes could not be made */
	TArray<FString> MissingFunctions;

	/* Functions this engine build spells differently to the one the rig was cooked from, and how
	 * many pins were dropped because only the game's version has them */
	TArray<FString> MismatchedFunctions;
	int32 DroppedPins = 0;

	/* Nodes the graph refused to take, and links it refused to make */
	int32 FailedNodes = 0;
	int32 FailedLinks = 0;
};

/* Rebuilds a rig's graph out of the RigVM its graph was compiled into.
 *
 * A cook keeps the VM and drops the graph. What the VM still carries is enough to put most of it
 * back: the bytecode names a function per instruction and the registers its operands read and
 * write, and every register is named after the pin it was made for, down to the node it sits on.
 * Reading the operands back through the function's own argument list turns each instruction into a
 * node, each shared register into a link, each literal into a pin default, and the byte code's
 * branch table into the execution the graph is wired up with.
 *
 * What it cannot put back is anything the compiler flattened. Collapsed nodes and function
 * references are gone by the time the VM exists, so their contents come back as nodes in the graph
 * they were compiled into. */
class REFLECTION_API FRigVMGraphReconstruction {
public:
	FRigVMGraphReconstruction(UControlRigBlueprint* InBlueprint, UPropertySerializer* InPropertySerializer);

	/* Rebuilds the graph from a cooked RigVM export. False when there is no bytecode to read. */
	bool Build(const FUObjectJsonValueExport& VirtualMachine);

	const FRigVMGraphStats& GetStats() const {
		return Stats;
	}

private:
	/* One entry of one of the VM's memory storages.
	 *
	 * A register is named after the pin it was made for and is shared by every pin linked to it,
	 * which is what makes the links readable: the node that owns the name wrote it, and every other
	 * instruction naming the same register reads it. */
	struct FRegister {
		FString Name;
		TSharedPtr<FJsonValue> Value;

		/* The node and pin the register is named after, once an instruction has claimed it */
		int32 Owner = INDEX_NONE;
		FString OwnerPin;
	};

	struct FNode {
		int32 Instruction = INDEX_NONE;

		const FRigVMFunction* Function = nullptr;

		/* The name the graph took, and the one the registers call it by. They differ when two
		 * graphs compiled into the same VM each held a node of the same name. */
		FString Name;
		FString DerivedName;

		/* Memory type and register index per operand, in the function's argument order */
		TArray<TPair<int32, int32>> Operands;

		/* The pin each operand belongs to, and whether the node writes it rather than reads it */
		TArray<FString> Arguments;
		TArray<bool> ArgumentIsOutput;

		URigVMNode* Node = nullptr;
	};

	/* An execute block: a range of instructions hanging off one pin of a control flow node */
	struct FBlock {
		int32 First = INDEX_NONE;
		int32 Last = INDEX_NONE;

		/* The instruction the block belongs to, and the pin on it the block runs from */
		int32 Owner = INDEX_NONE;
		FString Label;

		/* Lazy blocks are values computed on demand rather than a run of execution */
		bool bLazy = false;
	};

	void ReadMemory(const FUObjectJsonValueExport& VirtualMachine);
	void CollectNodes(const TArray<FUObjectJsonValueExport>& Instructions, const TArray<FString>& FunctionNames);

	/* Gives every register to the pin it is named after, written pins first */
	void ClaimRegisters();

	/* The pin path an operand's register offset adds, empty when it names the register whole */
	FString GetSegmentPath(int32 MemoryType, int32 RegisterOffset) const;
	void CreateNodes();
	void ApplyDefaults();

	/* Folds leaf defaults back into the pin that owns them, which is where saving reads from */
	void ConsolidateDefaults();

	/* Lays the graph out left to right: each node one column right of everything feeding it */
	void UpdateLayout();

	/* Turns collapsed graphs' local variables into rig variables so their first writer has a
	 * source. Runs last: making a variable compiles the blueprint, which may collect garbage. */
	void PromoteLocalVariables();
	void CreateLinks(const TArray<FUObjectJsonValueExport>& Instructions);
	void CreateExecution(const TArray<FUObjectJsonValueExport>& Instructions, const FUObjectJsonValueExport& ByteCode);

	/* The register a memory type and index name, or null when the export doesn't have one */
	FRegister* FindRegister(int32 MemoryType, int32 RegisterIndex);

	/* Reads the node name out of a register named after one of its pins. False when the register
	 * is named after a pin of some other node, which is what a link looks like from here. */
	static bool SplitRegisterName(const FString& RegisterName, const FString& PinName, FString& OutNodeName);

	/* The same, for a register whose pin isn't known in advance: the tail of the name is the pin
	 * and what comes before it is the node. Only reached where the two versions of a node disagree
	 * on its pins, since a pin name of its own can hold an underscore. */
	static bool SplitRegisterTail(const FString& RegisterName, FString& OutNodeName, FString& OutPinName);

	/* Writes a value out of the VM's memory onto a pin, walking into struct and array pins so the
	 * fields the pin doesn't have are left behind rather than refused as a whole */
	bool SetPinDefault(URigVMPin* Pin, const TSharedPtr<FJsonValue>& Value);

	/* The pin execution leaves a node through, and the one it comes back in on */
	static URigVMPin* FindExecuteOutput(URigVMNode* Node, const FString& Label);
	static URigVMPin* FindExecuteInput(URigVMNode* Node);

	bool Link(int32 SourceNode, const FString& SourcePin, int32 TargetNode, const FString& TargetPin);

	/* A getter for one of the rig's own variables, one per node reading it so the links stay short */
	URigVMNode* FindOrAddVariableGetter(const FString& VariableName, int32 ConsumerNode);

	/* Reads the variable an external register stands for */
	FString GetExternalVariable(int32 RegisterIndex) const;

	UControlRigBlueprint* Blueprint;
	UPropertySerializer* PropertySerializer;

	URigVMController* Controller = nullptr;
	URigVMGraph* Graph = nullptr;

	TArray<FRegister> Work;
	TArray<FRegister> Literals;

	/* The sub property a register offset stands for, per memory storage: an operand reaching into
	 * one field of a struct rather than the whole of it */
	TArray<FString> WorkPropertyPaths;
	TArray<FString> LiteralPropertyPaths;

	TArray<FNode> Nodes;
	TMap<int32, int32> InstructionToNode;

	TMap<FString, URigVMNode*> VariableGetters;
	TSet<FString> NodeNames;

	FRigVMGraphStats Stats;
};

#endif
