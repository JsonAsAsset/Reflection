/* Copyright Reflection Contributors 2024-2026 */

#pragma once

#include "CoreMinimal.h"
#include "Containers/JsonValueExport.h"
#include "Importers/Types/Blueprint/MacroPattern.h"

class UEdGraph;
class UEdGraphPin;
class UK2Node;
class UK2Node_Variable;
class UK2Node_CallFunction;
struct FUObjectExportContainer;

/* Builds a graph back out of the bytecode a function was cooked as.
 *
 * The bytecode is what the compiler made of a graph, and most of it maps straight back. A call is a
 * node. A constant is a value sat on a pin. The locals a function declares are mostly not variables
 * at all: the compiler makes one for every output pin it has to hold onto, names it after the node
 * and the pin it came from, and reading those names back is what says which pin fed which.
 *
 * What it cannot read back is anything the compiler invented and did not name, so control flow is
 * handled apart from this: the push and pop pairs that carry loops and branches are the shape of a
 * macro after expansion rather than anything a single node stands for. */
class REFLECTION_API FBytecodeGraph {
public:
	FBytecodeGraph(UEdGraph* InGraph, const TArray<FUObjectJsonValueExport>& InStatements, const TArray<FUObjectJsonValueExport>& InDeclared, FUObjectExportContainer* InContainer)
		: Graph(InGraph)
		, Statements(InStatements)
		, Declared(InDeclared)
		, Container(InContainer)
	{
	}

	/* Brings the run into Node at an address, for a graph entered in more than one place.
	 *
	 * A function is entered once, at its entry node. The event graph is every event's body laid out
	 * one after another, entered once per event at the address that event's body begins, and which
	 * address that is is what the event's own function passes to the ubergraph. */
	void EnterAt(int32 Address, UK2Node* Node, FName Through = NAME_None);

	/* Says that what the graph reads under one name is what an event was handed under another */
	void HandOver(const FString& Frame, const FString& Parameter);

	/* Says that what the graph reads under one name is a pin of a node it already has.
	 *
	 * A timeline keeps each of its tracks in a property of its own and the script reads those
	 * properties by name, but nobody ever wrote one: what was drawn is the timeline's own pin. */
	void HandOverTrack(const FString& Name, UK2Node* Node, FName Pin);

	/* Only the stretch of statements between these addresses, and nothing else.
	 *
	 * A function is not always one graph. The compiler writes everything a blueprint runs into the
	 * one ubergraph, and some of what it writes there was never drawn in the event graph: a
	 * transition rule is a graph of its own, decided in the ubergraph and entered only by the
	 * handler made for it. Laid out with the rest, it lands among the events as a run nothing
	 * reaches. */
	void Only(int32 From, int32 To);

	/* Everything but the stretch between these addresses, which is being laid out somewhere else */
	void LeaveOut(int32 From, int32 To);

	/* And one statement on its own, where what to leave out was worked out a statement at a time */
	void LeaveOutAt(int32 Address);

	/* Where a write into one of the class's own properties goes, rather than being written.
	 *
	 * A rule ends by setting a member of the node it decides. That is not a node writing a
	 * variable: the graph answers through the node, the way a function answers through its return,
	 * so what would have been written is wired to the pin instead. */
	void HandsInto(const FString& Owner, FName Member, UEdGraphPin* Pin);

	/* Says which node a timeline is, by the name the class keeps it under */

	/* Takes out whatever a previous run left in the graph, before the blueprint is compiled */
	void Clear();

	/* Declares what the function keeps of its own, which has to settle before anything is laid out.
	 * Answers how many it added. */
	int32 DeclareLocals();

	/* Lays the statements out as nodes. Answers how many it could place. */
	int32 Build();

	/* The statements it could make nothing of, for reporting what is still missing */
	const TArray<FString>& GetUnhandled() const { return Unhandled; }

private:
	/* One statement, as whatever it turns out to be: a node, a value, or nothing at all */
	struct FValue {
		UEdGraphPin* Pin = nullptr;
		FString Literal;

		bool IsSet() const { return Pin != nullptr || !Literal.IsEmpty(); }
	};

	/* What an expression says, spelled the same way every time, so the same one is recognised */
	static FString Canonical(const FUObjectJsonValueExport& Expression);

	/* The class a reference stands for, where it names a class's default object rather than an asset */
	const UClass* ClassNamedBy(const FUObjectJsonValueExport& Expression);

	/* Fetches the asset a class was written in, where a call names one the project hasn't got */
	void BringInClass(const FUObjectJsonValueExport& Named);

	/* Words filled in, laid back out as the one node that fills them in. Answers what it hands
	 * back, or nothing where this was somebody calling Format themselves. */
	UK2Node* FormatText(const FUObjectJsonValueExport& Expression);

	/* A struct filled a member at a time, laid back out as the one node that makes one */
	bool FillStruct(const FUObjectJsonValueExport& Statement, const FUObjectJsonValueExport& Variable);

	/* A struct written out in full, read back as the text a default is kept in */
	FString ReadStructConst(const FUObjectJsonValueExport& Expression);

	/* Keeps what was worked out, where reaching it again costs nothing */
	void Remember(const FString& Same, const FValue& Value);

	/* Reads the macros back out of what they compile to, before anything is placed */
	void FindMacros();

	/* Works out which of the compiler's own names carry a value that never changes */
	void FindConstants();

	/* Makes the node every matched macro was written as, before anything is laid out */
	void MakeMacros();

	/* Lays a macro out as the one node it was written as */
	bool PlaceMacro(const FMacroMatch& Match);

	/* One of the engine's own macros, by name */
	static UEdGraph* FindStandardMacro(const TCHAR* Named);

	/* Places the node a statement stands for, and chains it after the one before it */
	bool Place(const FUObjectJsonValueExport& Statement);

	/* Reads an expression as something that can feed a pin, sharing what has been read before */
	FValue Read(const FUObjectJsonValueExport& Expression);

	/* The reading itself, of whatever kind of expression it turns out to be */
	FValue ReadExpression(const FUObjectJsonValueExport& Expression);

	/* A call, of any of the four spellings the bytecode has for one */
	UK2Node* PlaceCall(const FUObjectJsonValueExport& Expression, UEdGraphPin* Target, const UClass* Against = nullptr);

	/* The function a call names, looked up rather than guessed at */
	static UFunction* ResolveFunction(const FUObjectJsonValueExport& Reference);

	/* A function on a class, by the name the game called it. Renames since are the engine's to
	 * know, so one it no longer answers to is looked up through them. */
	static UFunction* FindFunctionOn(const UClass* Class, const FString& Member);

	/* A variable read, as the pin that carries it: a local the compiler made for a pin resolves to
	 * that pin, anything else is a variable node */
	FValue ReadVariable(const FUObjectJsonValueExport& Expression);

	/* Brings in the component a call copies, and answers what kind it is */
	UClass* EnsureComponentTemplate(const FString& Name);

	/* Whether the function already keeps a local of that name */
	bool HasLocal(const FString& Name) const;

	/* Points a delegate node at the delegate it is about, and answers what it is asked against */
	UEdGraphPin* PointAtDelegate(class UK2Node_BaseMCDelegate* Node, const FUObjectJsonValueExport& Named);

	/* The pin a function answers through, by the name of the answer. Nothing where the function
	 * gives nothing back, or where the name is not one of its answers. */
	UEdGraphPin* FindAnswer(const FString& Name) const;

	/* Whether the graph can keep a local at all. Only a function can: a local lives on the entry
	 * node, and the event graph has no entry node to live on. */
	bool HoldsLocals() const;

	/* Whether a name is the compiler's rather than the graph's, and so not the graph's to keep */
	bool IsMade(const FString& Name) const;

	/* Declares a local the function keeps, where it does not have one already */
	bool EnsureLocal(const FString& Name, const FUObjectJsonValueExport& Property);

	/* Points a variable node at a local the function keeps rather than at the class */
	void PointAtLocal(UK2Node_Variable* Node, const FString& Name);

	/* Links two pins, casting where the value arrives wider than the pin taking it */
	void Connect(UEdGraphPin* From, UEdGraphPin* To);

	/* Writes a constant onto a pin, spelled the way that pin spells its values */
	static void ApplyLiteral(UEdGraphPin* Pin, const FString& Literal);

	/* Links one way out of a node to one way in, where that way out is still free */
	void LinkExecution(UEdGraphPin* From, UEdGraphPin* To);

	/* Puts the nodes somewhere sensible: the run reads left to right, and what feeds a node sits to
	 * the left of it. Nothing about this changes what is compiled, it is for reading. */
	void Arrange();

	/* The way into a node, whatever that node calls it */
	static UEdGraphPin* WayIn(UEdGraphNode* Node);

	/* A pin of a macro, by the name the pattern gives it rather than by how it is spaced */
	static UEdGraphPin* MacroPin(UK2Node* Node, FName Named, EEdGraphPinDirection Direction);

	/* Brings the run into Node, without saying anything about where it goes afterwards */
	void EnterNode(UK2Node* Node);

	/* Hooks Node into the run of execution, where it is the kind of node that has one */
	void ChainExecution(UK2Node* Node);

	/* The kind of node a call is written as, which is the engine's own choice rather than ours */
	static TSubclassOf<UK2Node_CallFunction> NodeClassFor(const UFunction* Function);

	template <typename T>
	T* AddNode();

	/* Same, where which kind it is was worked out rather than known when this was written */
	template <typename T>
	T* AddNodeOfClass(const TSubclassOf<T>& Class);

	UEdGraph* Graph = nullptr;
	TArray<FUObjectJsonValueExport> Statements;

	/* The locals the function declares, which the graph has to declare too */
	TArray<FUObjectJsonValueExport> Declared;

	/* Everything the asset was read as, for reaching what a call names */
	FUObjectExportContainer* Container = nullptr;

	/* The pin every compiler made local stands for, by the name it was given */
	TMap<FString, UEdGraphPin*> Locals;

	/* The last node with an execution pin, which the next one follows */
	UEdGraphPin* Flow = nullptr;

	/* The execution pin the run should begin at */
	UEdGraphPin* Start = nullptr;

	/* What the graph is entered through, by the address the run begins at. Held as nodes rather
	 * than as pins, since settling the locals lays the graph out again and any pin taken before
	 * that would be a pin of a node that no longer exists. */
	TMap<int32, TArray<TPair<TWeakObjectPtr<UK2Node>, FName>>> Starts;


	/* Statements that only say where a run begins, and stand for nothing in a graph */
	TSet<int32> Ignored;

	/* The stretch this graph is, where it is only part of a function */
	TArray<TPair<int32, int32>> Stretch;

	/* The stretches that belong to some other graph */
	TArray<TPair<int32, int32>> Elsewhere;

	/* And the statements that do, said one at a time */
	TSet<int32> ElsewhereAt;

	/* What a write answers into, against the property it would have been written to */
	TMap<FString, UEdGraphPin*> Decided;

	/* The name the graph reads a value under, against the name whoever entered it calls it */
	TMap<FString, FString> Handed;

	/* The same, where what it stands for is a pin of a node rather than another name. Held as the
	 * node and the pin's name, since settling the locals lays the graph out again. */
	TMap<FString, TPair<TWeakObjectPtr<UK2Node>, FName>> Tracks;


	/* Where each address in the script begins, for anything that jumps to one */
	TMap<int32, UEdGraphPin*> Entries;

	/* Jumps waiting on an address that may not have been laid down yet */
	TArray<TPair<UEdGraphPin*, int32>> Jumps;

	TArray<FString> Unhandled;

	/* Ways out that something tried to claim twice */
	TArray<FString> Contested;

	/* Whether the graph is being made for somebody to read.
	 *
	 * A run with nobody in front of it is checking the bytecode comes back the same, and every
	 * tidying up is one more thing between what was read and what is compared. A run with somebody
	 * in front of it wants the graph they would have written. */
	const bool bTidy = !IsRunningCommandlet();

	/* Something already worked out, and where it was worked out */
	struct FShared {
		UEdGraphPin* Pin = nullptr;
		int32 Made = 0;
	};

	struct FTakenApart {
		UK2Node* Node = nullptr;
		int32 Made = 0;
	};

	/* How far a reading carries before it is worth reading again rather than reaching back for it.
	 * Counted in statements, which is what the graph is laid out along: a value wanted by the next
	 * thing along is the same value, and one wanted much later is a wire across the whole graph. */
	static constexpr int32 SharingReach = 4;

	/* What has already been worked out, by what it says */
	TMap<FString, FShared> Reused;

	/* The node that takes a struct apart, by the struct it takes apart rather than by the field
	 * somebody wanted out of it */
	TMap<FString, FTakenApart> Apart;

	/* Which statement is being laid down, for telling how near anything else is */
	int32 Placing = 0;

	/* Where a macro begins, and what it accounts for */
	TMap<int32, FMacroMatch> Macros;

	/* The node each of them was written as, made before any of them is placed */
	TMap<int32, class UK2Node_MacroInstance*> Written;

	/* Statements that are a macro's own workings and stand for nothing on their own */
	TSet<int32> Skipped;

	/* The compiler's names for values a matched macro hands out. Nothing else has to hold one of
	 * these, and until a macro has actually matched, nothing may drop one either. */
	TSet<FString> Owned;

	/* Whether the macros have been looked for, since both settling the locals and laying the graph
	 * out want to know and the answer does not change */
	bool bLookedForMacros = false;

	/* Names the compiler made to carry a value that never changes.
	 *
	 * A pin that takes a struct cannot hold one in the script, so the compiler makes a local, writes
	 * the value into it once, and passes that along. That is not a variable anybody declared: it is
	 * what was typed into the pin, and it belongs on the pin rather than in a node of its own.
	 *
	 * Told apart from a macro's scratch by being written once and only ever from a constant. A
	 * loop's counter is written again every time round, so it is never one of these. */
	TSet<FString> Constants;

	/* What each of those carries, once the statement that writes it has been read */
	TMap<FString, FString> Carried;

	bool bLookedForConstants = false;

	/* Macros whose bodies are still being laid out, innermost last */
	TArray<TPair<const FMacroMatch*, UK2Node*>> Open;

	int32 Placed = 0;

	int32 Chained = 0;
	int32 Orphaned = 0;

	int32 LastChainedAddress = -1;
};
