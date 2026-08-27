/* Copyright Reflection Contributors 2024-2026 */

#pragma once

#include "Importers/Types/Blueprint/BlueprintImporter.h"

class UAnimGraphNode_BlendListByEnum;
class UAnimGraphNode_Base;

/* 4.25 and below build this module without the engine's shared PCH (see Reflection.Build.cs),
 * which is where the animation blueprint type used to come in from */
#if UE4_25_BELOW
class UAnimBlueprint;
#endif

class IAnimationBlueprintImporter final : public IBlueprintImporter {
public:
	virtual bool Import() override;

private:
	UAnimBlueprint* CreateAnimBlueprint(UClass* ParentClass);

	/* Says over each graph how it blends in and out, and answers how many were said over */
	int32 ConstructBlendOptions();
	
	void ProcessEvaluateGraphExposedInputs(const TSharedPtr<FJsonObject>& AnimNodeProperties) const;

	/* Finds an Animation Graph in an Animation Blueprint */
	static UEdGraph* FindAnimGraph(UAnimBlueprint* AnimBlueprint);

	/* Using [AnimNodeProperties] that is filled with animation nodes, create new nodes, and the Outer being [AnimGraph], then add it to the [Container] */
	void CreateGraph(const TSharedPtr<FJsonObject>& AnimNodeProperties, UEdGraph* AnimGraph, FUObjectExportContainer* Container);

	/* Create Animation Graph Nodes and create a UObjectExportContainer to hold the data */
	static void CreateAnimGraphNodes(UEdGraph* AnimGraph, const TSharedPtr<FJsonObject>& AnimNodeProperties, FUObjectExportContainer& OutContainer);

	/* Add a container full of nodes to a graph */
	static void AddNodesToGraph(UEdGraph* AnimGraph, FUObjectExportContainer* Container);

	/* Deserializes each node in the node container */
	void HandleNodeDeserialization(FUObjectExportContainer* Container);

	/* Links Animation Graph Nodes together using a container */
	static void ConnectAnimGraphNodes(FUObjectExportContainer* Container, UEdGraph* AnimGraph);

	/* Reads how the class folded its nodes' values away, and where it keeps them */
	void ReadFolded(const TSharedPtr<FJsonObject>& Nodes, const TArray<FString>& Keys);

	/* Puts back the values the class folded out of its nodes and kept once for all of them */
	void Unfold(const TSharedPtr<FJsonObject>& Nodes, const TArray<FString>& Keys) const;

	/* Which of a node's properties reads a given slot, said by the node rather than by the slot */
	FName MemberAt(const FString& Key, int32 Which, uint32 Entry) const;

	/* Which node reads a given slot, where nothing else says */
	FString NodeAt(const TArray<FString>& Keys, uint32 Entry, int32& Which) const;

	/* Reads which pins are handed a variable by the class rather than fed by anything drawn */
	void ReadBindings(const TSharedPtr<FJsonObject>& Nodes, const TArray<FString>& Keys);

public:
	/* Puts those back on a node, drawn or bound.
	 *
	 * Said out here because a transition result is made where the state machine is laid out rather
	 * than with the rest of the nodes, so that is the only place anything can hand it its own. */
	void ApplyBindings(const FString& Key, UAnimGraphNode_Base* Node) const;

protected:
	/* Gives a node pins for the values it hands to whatever it runs */
	void ExposeHandedOver(UAnimGraphNode_Base* Node, const TArray<FString>& Named) const;

	/* Draws on the node the properties something hands a value to, which are hidden by default */
	void ShowBoundPins(const TSharedPtr<FJsonObject>& AnimNodeProperties, FUObjectExportContainer* Container) const;

	/* Draws a variable, and a Not where it was turned round, into the graph a node is in */
	void Draw(UAnimGraphNode_Base* Node, FName Member, const FString& Named, bool bTurned, int32 Row) const;

	/* Reads every variable a node is handed into the graph beside it, once the graph is laid out */
	void DrawBindings(FUObjectExportContainer* Container) const;

	/* Something a node is handed, against the property of its own it lands on */
	struct FHandedOver {
		FName Member;
		TArray<FString> Path;
		bool bTurned = false;
	};

	TMap<FString, TArray<FHandedOver>> Bindings;

	/* Where the class keeps what it folded away, and what reads each slot */
	TArray<TSharedPtr<FJsonValue>> Constants;
	TArray<TSharedPtr<FJsonValue>> Mutables;
	TMap<FString, int32> MutableAt;
	TSharedPtr<FJsonObject> AnimMutables;
	TMap<FString, TMap<FString, int32>> Slots;
	TMap<int32, TArray<uint32>> Folded;
	TMap<FString, FString> Kinds;

	/* Points each borrowed cached pose at the node that saved it, once every graph has been made */
	void SettleCachedPoses();

	/* Tells each pose a layer takes in which of the layer's poses it stands for */
	void SettleInputPoses(UEdGraph* Layer) const;

	/* Every container of nodes made, since a pose saved in one graph is borrowed from others */
	TArray<FUObjectExportContainer*> AnimNodeContainers;

	/* Each cached pose borrowed, and the name of the node said to have saved it */
	TArray<TPair<TWeakObjectPtr<class UAnimGraphNode_UseCachedPose>, FString>> Borrowed;

	static void UpdateBlendListByEnumVisibleEntries(FUObjectExport* NodeExport, FUObjectExportContainer* Container, UEdGraph* AnimGraph);
	
protected:
	/* Global Cached data to reuse */
	UAnimBlueprint* AnimBlueprint = nullptr;
	
	TArray<FString> NodesKeys;
	TArray<FString> ReversedNodesKeys;
	
	TArray<TSharedPtr<FJsonValue>> BakedStateMachines;
	
	TSharedPtr<FJsonObject> RootAnimNodeProperties;
	FUObjectExportContainer* RootAnimNodeContainer = new FUObjectExportContainer();

	/* UE5 Copy Record Cache Data */
	TSharedPtr<FJsonObject> SerializedSparseClassData;

	TArray<FString> SyncGroupNames;
};

REGISTER_IMPORTER(IAnimationBlueprintImporter, (TArray<FString>{ 
	TEXT("AnimBlueprintGeneratedClass")
}), TEXT("Blueprints"));