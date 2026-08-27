/* Copyright Reflection Contributors 2024-2026 */

#include "AnimGraphNode_CustomProperty.h"
#include "Kismet/KismetMathLibrary.h"
#include "K2Node_CallFunction.h"
#include "K2Node_VariableGet.h"
#include "AnimGraphNode_LinkedInputPose.h"
#include "AnimGraphNode_Root.h"
#include "AnimationGraphSchema.h"
#include "Animation/BlendProfile.h"
#include "AnimationGraph.h"
#include "Importers/Types/Blueprint/AnimationBlueprintImporter.h"

#include "AnimationStateMachineGraph.h"
#include "AnimationStateMachineSchema.h"
#include "AnimGraphNode_Base.h"
#include "AnimGraphNode_SaveCachedPose.h"
#include "AnimGraphNode_StateMachine.h"
#include "AnimGraphNode_StateResult.h"
#include "AnimGraphNode_UseCachedPose.h"
#include "Animation/AnimBlueprint.h"
#include "Kismet2/BlueprintEditorUtils.h"

#include "Importers/Types/Blueprint/Utilities/AnimationBlueprintUtilities.h"
#include "Importers/Types/Blueprint/Utilities/AnimNodeLayoutUtillties.h"
#include "Importers/Types/Blueprint/Utilities/StateMachineUtilities.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Importers/Types/Blueprint/BlueprintUtilities.h"
#include "Importers/Types/Blueprint/BlueprintVariables.h"
#include "Utilities/JsonHelpers.h"

#if ENGINE_UE5
#include "UObject/UnrealTypePrivate.h"
#endif

int32 IAnimationBlueprintImporter::ConstructBlendOptions() {
	if (AnimBlueprint == nullptr) return 0;

	const FUObjectJsonValueExport Data = GetAssetDataAsValue();

	if (!Data.Has(TEXT("GraphBlendOptions"))) return 0;

	TArray<UEdGraph*> Graphs;
	AnimBlueprint->GetAllGraphs(Graphs);

	int32 Said = 0;

	for (const FUObjectJsonValueExport& Entry : Data.GetArray(TEXT("GraphBlendOptions"))) {
		if (!Entry.Has(TEXT("Key")) || !Entry.Has(TEXT("Value"))) continue;

		const FString Named = Entry.GetString(TEXT("Key"));

		for (UEdGraph* Graph : Graphs) {
			UAnimationGraph* Layer = Cast<UAnimationGraph>(Graph);

			if (Layer == nullptr || Layer->GetName() != Named) continue;

			/* Said over the graph rather than field by field.
			 *
			 * How a layer blends is a property of the graph like any other, so it is read back the way
			 * every other property is. Set by hand instead, it would carry only the fields somebody
			 * here thought of, and a profile would have to be looked up all over again. */
			const TSharedPtr<FJsonObject> Says = MakeShared<FJsonObject>();

			Says->SetObjectField(TEXT("BlendOptions"), Entry.GetObject(TEXT("Value")).JsonObject);

			GetObjectSerializer()->DeserializeObjectProperties(Says, Layer);

			Said++;

			break;
		}
	}

	return Said;
}

bool IAnimationBlueprintImporter::Import() {
	AnimBlueprint = GetSelectedAsset<UAnimBlueprint>(true);

	/* Nothing selected in the content browser doesn't mean anything is there, reflecting the same animation
	 * blueprint a second time lands on an asset that already exists, and FKismetEditorUtilities::CreateBlueprint
	 * asserts outright when any blueprint of that name is already in the package. */
	if (!AnimBlueprint && GetPackage()) {
		UBlueprint* ExistingBlueprint = FindObject<UBlueprint>(GetPackage(), *GetAssetName());
		if (!ExistingBlueprint) ExistingBlueprint = LoadObject<UBlueprint>(nullptr, *GetPackage()->GetPathName());

		if (ExistingBlueprint) {
			AnimBlueprint = Cast<UAnimBlueprint>(ExistingBlueprint);

			/* Something of that name is there but isn't an animation blueprint, so it can neither be reused nor
			 * created over. Bail rather than let the assert take the editor down. */
			if (!AnimBlueprint) {
				FImportIssues::Report(
					EImportIssue::Failed,
					TEXT("The asset name is already taken"),
					FString::Printf(TEXT("'%s' already exists and is not an Animation Blueprint. Rename or delete it before reflecting."), *GetAssetName())
				);

				return false;
			}
		}
	}

	if (!AnimBlueprint) {
		const TSharedPtr<FJsonObject> SuperStruct = GetAssetData()->GetObjectField(TEXT("SuperStruct"));
		UClass* ParentClass = LoadClass(SuperStruct);

		/* An animation blueprint is built on the class it was written against, and a build without
		 * that class has nothing to build it on. Making one anyway is asked of the engine, which
		 * takes the parent apart without checking it is there. */
		if (ParentClass == nullptr) {
			FImportIssues::Report(
				EImportIssue::MissingClass,
				TEXT("Couldn't resolve the parent class"),
				FString::Printf(TEXT("'%s' is built on a class this build does not carry, so there is nothing to make it from."), *GetAssetName())
			);

			return false;
		}

		AnimBlueprint = CreateAnimBlueprint(ParentClass);
	}

	if (!AnimBlueprint) return false;

	const TSharedPtr<FJsonObject> RootAnimNodeDefaults = GetExportStartingWith("Default__", "Name", GetContainer()->JsonObjects);
	if (!RootAnimNodeDefaults.IsValid()) return false;
	
	RootAnimNodeProperties = RootAnimNodeDefaults->GetObjectField(TEXT("Properties"));
	if (!RootAnimNodeProperties.IsValid()) return false;

	/* The variables the blueprint declares have to exist before the class default object below can
	 * put anything in them. ChildProperties holds them alongside the anim graph node state, which
	 * FBlueprintVariables filters out. */
	if (FBlueprintVariables::Construct(AnimBlueprint, FBlueprintVariables::GetDeclared(GetAssetExport(), GetContainer())) > 0) {
		/* The properties only appear on the generated class once it recompiles */
		FKismetEditorUtilities::CompileBlueprint(AnimBlueprint, EBlueprintCompileOptions::SkipGarbageCollection);
	}

	/* UClass::GetDefaultObject only became const later on */
#if UE4_24_BELOW
	UBlueprintGeneratedClass* GeneratedClass = Cast<UBlueprintGeneratedClass>(AnimBlueprint->GeneratedClass);
#else
	const UBlueprintGeneratedClass* GeneratedClass = Cast<UBlueprintGeneratedClass>(AnimBlueprint->GeneratedClass);
#endif
	GetObjectSerializer()->Exports = GetContainer()->JsonObjects;
	GetObjectSerializer()->DeserializeObjectProperties(RemovePropertiesShared(RootAnimNodeProperties, {
		"RootComponent"
	}), GeneratedClass->GetDefaultObject());

	GetObjectSerializer()->DeserializeObjectProperties(RemovePropertiesShared(GetAssetData(), {
		"FuncMap",
		"bCooked",
		"Children",
		"RootAnimNodeIndex",
		"UberGraphFunction",
		"UberGraphFramePointerProperty",
		"SuperStruct"
	}), AnimBlueprint);

	/* Newer Unreal Engine versions use CopyRecords and SerializedSparseClassData */
	if (RootAnimNodeDefaults->HasField(TEXT("SerializedSparseClassData"))) {
		SerializedSparseClassData = RootAnimNodeDefaults->GetObjectField(TEXT("SerializedSparseClassData"));
	}

	/* Kept here, before what the class carries is filtered down to its nodes.
	 *
	 * The folded values that change at run time are kept in one of the class's own properties
	 * rather than on any node, so the filter takes it out along with everything else that is not a
	 * node and it is the only place the far end of a hand-over can be looked up. */
	if (const TSharedPtr<FJsonObject>* Held = nullptr; RootAnimNodeProperties->TryGetObjectField(TEXT("__AnimBlueprintMutables"), Held)) {
		AnimMutables = *Held;
	}

	/* Array of sync group names cached to use at later points of importing */
	if (GetAssetData()->HasField(TEXT("SyncGroupNames"))) {
		for (const auto& SyncGroupNameValue : GetAssetData()->GetArrayField(TEXT("SyncGroupNames"))) {
			SyncGroupNames.Add(SyncGroupNameValue->AsString());
		}
	}

	/* Filter AnimNodeProperties ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ */
	FilterAnimGraphNodeProperties(RootAnimNodeProperties);
	ProcessEvaluateGraphExposedInputs(RootAnimNodeProperties);

	/* Parse LinkIDs to proper Node IDs ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ */
	for (const auto& Pair : RootAnimNodeProperties->Values) {
		NodesKeys.Add(JsonKeyToString(Pair.Key));
	}

	ReversedNodesKeys = NodesKeys;
	Algo::Reverse(ReversedNodesKeys);

	/* Said before anything is read off a node, since a folded value is read like any other once it
	 * is back where it belongs */
	/* Read again from nothing, the same as the rest: these hold nodes made for this run, and a node
	 * from the run before is one the editor has already cleared away */
	AnimNodeContainers.Empty();
	Borrowed.Empty();

	ReadFolded(RootAnimNodeProperties, NodesKeys);
	ReadFolded(RootAnimNodeProperties, NodesKeys);
	Unfold(RootAnimNodeProperties, NodesKeys);
	ReadBindings(RootAnimNodeProperties, NodesKeys);

	for (const FString& Key : NodesKeys) {
		TSharedPtr<FJsonValue> NodeValue = RootAnimNodeProperties->Values.FindChecked(StringToJsonKey(Key));
		if (!NodeValue.IsValid()) continue;

		ReplaceLinkID(NodeValue, NodesKeys);
		RootAnimNodeProperties->Values[StringToJsonKey(Key)] = NodeValue;
	}

	/* Sets "State" and "Machine" for each state result */
	if (GetAssetData()->HasField(TEXT("BakedStateMachines"))) {
		BakedStateMachines = GetAssetData()->GetArrayField(TEXT("BakedStateMachines"));
    
		for (const TSharedPtr<FJsonValue>& MachineValue : BakedStateMachines) {
			const TSharedPtr<FJsonObject> MachineObject = MachineValue->AsObject();
			const TArray<TSharedPtr<FJsonValue>> States = MachineObject->GetArrayField(TEXT("States"));
			const FString MachineName = MachineObject->GetStringField(TEXT("MachineName"));
        
			/* Loop through each state */
			for (const TSharedPtr<FJsonValue>& StateValue : States) {
				const TSharedPtr<FJsonObject> StateObject = StateValue->AsObject();
				const int32 StateRootNodeIndex = StateObject->GetIntegerField(TEXT("StateRootNodeIndex"));
            
				if (StateRootNodeIndex == -1 || !ReversedNodesKeys.IsValidIndex(StateRootNodeIndex)) {
					continue;
				}
            
				const FString StartKey = ReversedNodesKeys[StateRootNodeIndex];
				HarvestAndTagConnectedStateMachineNodes(StartKey, StateObject->GetStringField(TEXT("StateName")), MachineName, RootAnimNodeProperties->Values);
			}
		}
	}

	/* Separate main graph nodes (without "State" and "Machine") into RootGraphAnimProperties ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ */
	const TSharedPtr<FJsonObject> RootGraphAnimProperties = MakeShared<FJsonObject>(); {
		for (const FString& Key : NodesKeys) {
			const TSharedPtr<FJsonValue> NodeValue = RootAnimNodeProperties->Values.FindChecked(StringToJsonKey(Key));
		
			if (NodeValue->Type == EJson::Object) {
				const TSharedPtr<FJsonObject> NodeObject = NodeValue->AsObject();
			
				if (!NodeObject->HasField(TEXT("State")) && !NodeObject->HasField(TEXT("Machine"))) {
					RootGraphAnimProperties->SetObjectField(Key, NodeObject);
				}
			}
		}
	}

	/* The interfaces first, since a layer is laid out into the graph an interface brings with it.
	 *
	 * Said here rather than with the rest of what a blueprint carries, because that is said after the
	 * animation graph is built and the layers have nowhere to go by then. */
	Blueprint = AnimBlueprint;

	if (const int32 Answered = ConstructInterfaces(); Answered > 0) {
		UE_LOG(LogReflection, Display, TEXT("%d interface(s) answered for"), Answered);
	}

	/* Which graph each of them belongs to.
	 *
	 * A layer is a graph of its own with its own output pose, and the class writes the nodes of
	 * every graph into one list. Read as one graph they all land in the main one, which then has an
	 * output pose for each layer and will not compile: the editor says it expected one root and
	 * found two.
	 *
	 * Nothing on a node says which graph it is in. What does say is where each graph's own things
	 * are: a pose a layer takes in names the graph it is for, and the class lists the cached poses
	 * and the asset players of each graph by name. Those are the ones that are known, and the rest
	 * of a graph is whatever its output pose is built from. */
	TMap<FString, FString> Belongs; {
		/* A pose taken in says the graph it is taken into */
		for (const FString& Key : NodesKeys) {
			const TSharedPtr<FJsonValue> NodeValue = RootAnimNodeProperties->Values.FindChecked(StringToJsonKey(Key));

			if (NodeValue->Type != EJson::Object) continue;

			FString Named;

			if (NodeValue->AsObject()->TryGetStringField(TEXT("Graph"), Named) && !Named.IsEmpty()) {
				Belongs.Add(Key, Named);
			}
		}

		/* And the class says which nodes of each graph it had to remember */
		const FUObjectJsonValueExport Data = GetAssetDataAsValue();

		for (const TCHAR* Field : { TEXT("OrderedSavedPoseIndicesMap"), TEXT("GraphAssetPlayerInformation") }) {
			if (!Data.Has(Field)) continue;

			for (const FUObjectJsonValueExport& Entry : Data.GetArray(Field)) {
				if (!Entry.Has(TEXT("Key")) || !Entry.Has(TEXT("Value"))) continue;

				/* The animation graph itself is not a layer, whatever it is listed as.
				 *
				 * The class lists what each of its graphs had to remember, and the animation graph
				 * is one of those graphs listed by name alongside the layers. Taken as a layer
				 * it is laid out twice: once as the graph everything unclaimed belongs to, and
				 * again as a layer of its own into the very same graph, which is emptied first.
				 *
				 * So the second pass throws away the first, and what survives is whichever handful
				 * of nodes the class happened to name the rest of the graph, and every node that
				 * read one of them, gone. An asset whose animation graph remembers nothing never
				 * showed it, because there was no name to be listed under. */
				FString Named = Entry.GetString(TEXT("Key"));

				if (Named == UEdGraphSchema_K2::GN_AnimGraph.ToString()) Named = FString();

				const FUObjectJsonValueExport Held = Entry.GetObject(TEXT("Value"));

				for (const TCHAR* Listed : { TEXT("OrderedSavedPoseNodeIndices"), TEXT("PlayerNodeIndices") }) {
					if (!Held.Has(Listed)) continue;

					/* Plain numbers rather than things, so they are read off the array itself */
					const TArray<TSharedPtr<FJsonValue>>* Listing = nullptr;

					if (!Held.JsonObject.IsValid() || !Held.JsonObject->TryGetArrayField(Listed, Listing)) continue;

					for (const TSharedPtr<FJsonValue>& Which : *Listing) {
						const int32 Index = Which.IsValid() ? static_cast<int32>(Which->AsNumber()) : INDEX_NONE;

						if (ReversedNodesKeys.IsValidIndex(Index)) Belongs.Add(ReversedNodesKeys[Index], Named);
					}
				}
			}
		}
	}

	/* Borrowing a cached pose is not being built from it.
	 *
	 * Every other link says a pin is fed by a node, and both ends sit in the one graph. A cached
	 * pose is the exception: one graph saves it, any graph may borrow it, and a borrower does not
	 * hold what it borrows. Counted as a feed, a graph that borrows a pose is built out of the
	 * graph that saved it and drags that graph's nodes across with it.
	 *
	 * Left out, the node that saved the pose is fed by nothing, which makes it the head of what it
	 * saved, so the state machine or whatever else was cached behind it is claimed along with it by
	 * the graph the class says saved it. */
	auto Borrows = [](const FString& Property) {
		return Property == TEXT("LinkToCachingNode");
	};

	/* Everything an output pose is built from is in the graph that output pose belongs to. Walked
	 * from the node rather than to it, since a link says what feeds a pin. */
	TFunction<void(const FString&, TSet<FString>&)> Gather = [&](const FString& Key, TSet<FString>& Into) {
		if (Into.Contains(Key) || !RootGraphAnimProperties->HasField(Key)) return;

		Into.Add(Key);

		for (const TPair<FString, FString>& Link : FindLinkIDs(MakeShared<FJsonValueObject>(RootGraphAnimProperties->GetObjectField(Key)), Key)) {
			if (Borrows(Link.Key)) continue;

			Gather(Link.Value, Into);
		}
	};

	/* What each graph is made of, by the name it goes under */
	TMap<FString, TSharedPtr<FJsonObject>> Made;

	{
		/* An output pose is a node nothing else is built from */
		TSet<FString> Fed;

		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : RootGraphAnimProperties->Values) {
			for (const TPair<FString, FString>& Link : FindLinkIDs(Pair.Value, JsonKeyToString(Pair.Key))) {
				if (Borrows(Link.Key)) continue;

				Fed.Add(Link.Value);
			}
		}

		/* Which graph has each node, once each.
		 *
		 * A node can be reached from more than one output pose a cached pose is saved by one
		 * graph and borrowed by the rest, which is the whole point of one so the graphs a node
		 * is reached from are not the graph it is in. Put in each of them it is made once per
		 * graph, and every copy after the first is a node with no properties, no name and nothing
		 * wired to it, sitting in a graph it was never part of.
		 *
		 * So each is claimed once. What the class says outright is claimed first and settles it;
		 * everything else falls to whichever output pose reaches it, and to only one of them. */
		TMap<FString, FString> Claimed = Belongs;

		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : RootGraphAnimProperties->Values) {
			const FString Key = JsonKeyToString(Pair.Key);

			if (Fed.Contains(Key)) continue;

			TSet<FString> Owned;
			Gather(Key, Owned);

			/* Which graph it is, said by whichever of its own things is known */
			FString Named;

			for (const FString& Held : Owned) {
				if (const FString* Says = Belongs.Find(Held)) {
					Named = *Says;

					break;
				}
			}

			/* Or, failing that, by the pose it borrows.
			 *
			 * A graph the class names nothing in is a graph of nodes that are none of the class's
			 * business: no asset player to be listed against a layer, no pose taken in to name the
			 * layer it is taken into, nothing cached. What such a graph does have is what it reads,
			 * and a cached pose is named so the graph that saved the pose is the one to go by.
			 *
			 * Only for the name. Which graph these nodes are in is still their own question, and
			 * borrowing a pose does not put the borrower in the graph that saved it. */
			if (Named.IsEmpty()) {
				for (const FString& Held : Owned) {
					for (const TPair<FString, FString>& Link : FindLinkIDs(MakeShared<FJsonValueObject>(RootGraphAnimProperties->GetObjectField(Held)), Held)) {
						if (!Borrows(Link.Key)) continue;

						if (const FString* Says = Belongs.Find(Link.Value)) {
							Named = *Says;

							break;
						}
					}

					if (!Named.IsEmpty()) break;
				}
			}

			for (const FString& Held : Owned) {
				if (!Claimed.Contains(Held)) Claimed.Add(Held, Named);
			}
		}

		for (const TPair<FString, FString>& One : Claimed) {
			if (!RootGraphAnimProperties->HasField(One.Key)) continue;

			TSharedPtr<FJsonObject>& Into = Made.FindOrAdd(One.Value);

			if (!Into.IsValid()) Into = MakeShared<FJsonObject>();

			Into->SetObjectField(One.Key, RootGraphAnimProperties->GetObjectField(One.Key));
		}
	}

	/* One output pose to a graph, and never none.
	 *
	 * The class writes every graph's nodes into one list and says nothing on a node about which
	 * graph it belongs to, so a graph put back from it can come out with two output poses or with
	 * none. Neither is something the engine reports: it asserts, in the compiler and again when it
	 * lays an interface graph out, and takes the editor with it.
	 *
	 * Whichever one something was actually built into is the one kept, since an output pose with
	 * nothing feeding it answers nothing. */
	const auto SettleOutputPose = [](UEdGraph* Graph) {
		if (Graph == nullptr || Graph->GetSchema() == nullptr) return;

		TArray<UAnimGraphNode_Root*> Poses;
		Graph->GetNodesOfClass<UAnimGraphNode_Root>(Poses);

		if (Poses.Num() == 0) {
			Graph->GetSchema()->CreateDefaultNodesForGraph(*Graph);

			return;
		}

		const auto IsFed = [](const UAnimGraphNode_Root* Pose) {
			for (const UEdGraphPin* Pin : Pose->Pins) {
				if (Pin != nullptr && Pin->LinkedTo.Num() > 0) return true;
			}

			return false;
		};

		int32 Kept = 0;

		for (int32 At = 1; At < Poses.Num(); ++At) {
			if (IsFed(Poses[At]) && !IsFed(Poses[Kept])) Kept = At;
		}

		for (int32 At = Poses.Num() - 1; At >= 0; --At) {
			if (At == Kept) continue;

			UE_LOG(LogReflection, Display, TEXT("\"%s\" was left with an output pose too many, so one was taken out"), *Graph->GetName());

			Graph->RemoveNode(Poses[At]);
		}
	};

	UEdGraph* AnimGraph = FindAnimGraph(AnimBlueprint);

	/* Made where the blueprint hasn't got one.
	 *
	 * Every animation blueprint has an animation graph except an interface, which says what its
	 * layers take and hand back and draws none of them. One made against a parent that already had
	 * an animation graph is given none of its own, and then there is nowhere to lay its nodes out.
	 *
	 * Made the way the engine makes one for a blueprint that turns out to want it, and kept from
	 * being deleted the same way, since a blueprint without it is not one. */
	if (AnimGraph == nullptr && AnimBlueprint->BlueprintType != BPTYPE_Interface) {
		AnimGraph = FBlueprintEditorUtils::CreateNewGraph(AnimBlueprint, UEdGraphSchema_K2::GN_AnimGraph, UAnimationGraph::StaticClass(), UAnimationGraphSchema::StaticClass());

		if (AnimGraph != nullptr) {
			FBlueprintEditorUtils::AddDomainSpecificGraph(AnimBlueprint, AnimGraph);

			AnimGraph->bAllowDeletion = false;

			UE_LOG(LogReflection, Display, TEXT("\"%s\" had no animation graph, so it was given one"), *GetAssetName());
		}
	}

	if (AnimGraph) {
		AnimGraph->SubGraphs.Empty();
	}

	/* Anything no graph claimed is the animation graph's own */
	if (const TSharedPtr<FJsonObject>* Main = Made.Find(FString()); Main != nullptr && Main->IsValid()) {
		CreateGraph(*Main, AnimGraph, RootAnimNodeContainer);
	} else {
		CreateGraph(RootGraphAnimProperties, AnimGraph, RootAnimNodeContainer);
	}

	SettleOutputPose(AnimGraph);

	/* And each layer into the graph the interface brought in for it */
	TArray<UEdGraph*> Graphs;
	AnimBlueprint->GetAllGraphs(Graphs);

	for (const TPair<FString, TSharedPtr<FJsonObject>>& One : Made) {
		if (One.Key.IsEmpty() || !One.Value.IsValid()) continue;

		UEdGraph* Layer = nullptr;

		for (UEdGraph* Candidate : Graphs) {
			if (Candidate != nullptr && Candidate->GetName() == One.Key) {
				Layer = Candidate;

				break;
			}
		}

		if (Layer == nullptr) {
			FImportIssues::Report(
				EImportIssue::MissingClass,
				TEXT("A layer with nowhere to lay it out"),
				FString::Printf(TEXT("'%s' has nodes for the layer '%s', and no graph of that name to put them in."), *GetAssetName(), *One.Key)
			);

			continue;
		}

		Layer->SubGraphs.Empty();

		/* What the graph came with, so a node the interface already drew is not drawn again */
		FString Had;

		for (const UEdGraphNode* Node : Layer->Nodes) {
			if (Node != nullptr) Had += FString::Printf(TEXT("%s "), *Node->GetName());
		}

		FString Adding;

		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : One.Value->Values) {
			Adding += FString::Printf(TEXT("%s "), *JsonKeyToString(Pair.Key));
		}

		CreateGraph(One.Value, Layer, new FUObjectExportContainer());

		SettleOutputPose(Layer);
		SettleInputPoses(Layer);

		UE_LOG(LogReflection, Display, TEXT("layer \"%s\" laid out: it already had [%s], and was given [%s]"),
			*One.Key, Had.IsEmpty() ? TEXT("nothing") : *Had, *Adding);
	}

	/* Now that there is every graph to look in */
	SettleCachedPoses();

	/* And the rest of it, which is a blueprint's whatever else it is. The anim graph above is the
	 * one part an animation blueprint keeps differently; its events were cooked to bytecode the
	 * same as any other blueprint's, and were being left in it. */
	Blueprint = AnimBlueprint;

	ConstructBody();

	/* How a layer blends when it is put on and taken off, which somebody set on the graph itself.
	 *
	 * Said after the rest, since the graphs it is said over are the ones an interface brought in.
	 * The cook only keeps it where it was changed from what a graph starts with, so a graph with
	 * nothing said about it is one nobody touched. */
	if (const int32 Blended = ConstructBlendOptions(); Blended > 0) {
		UE_LOG(LogReflection, Display, TEXT("%d graph(s) blend the way they were set to"), Blended);
	}

	return OnAssetCreation(AnimBlueprint);
}

UAnimBlueprint* IAnimationBlueprintImporter::CreateAnimBlueprint(UClass* ParentClass) {
	const EBlueprintType BlueprintType = GetBlueprintTypeSaid(GetAssetData(), ParentClass);

	if (UBlueprint* Made = FKismetEditorUtilities::CreateBlueprint(ParentClass, GetPackage(), FName(*GetAssetName()), BlueprintType, UAnimBlueprint::StaticClass(), UAnimBlueprintGeneratedClass::StaticClass())) {
		/* Said of the base, which only takes what was made. An animation blueprint is made here,
		 * where the blueprint importer's own makes one from nothing, and asking for that would put
		 * a second blueprint over the one just created. */
		return Cast<UAnimBlueprint>(IImporter::CreateAsset(Made));
	}

	return nullptr;
}

void IAnimationBlueprintImporter::ReadFolded(const TSharedPtr<FJsonObject>& Nodes, const TArray<FString>& Keys) {
	if (!Nodes.IsValid()) return;

	const FUObjectJsonValueExport Data = GetAssetDataAsValue();

	if (!Data.Has(TEXT("AnimNodeData")) || !Data.Has(TEXT("NodeTypeMap"))) return;

	/* Read again from nothing, since this says what one class is.
	 *
	 * A slot is a place in these, so what they say is only true while they hold that class and
	 * nothing else. Added to rather than replaced, a second read puts every slot in a second time
	 * and each name ends up pointing at its later copy: every hand-over then lands as many places
	 * along as the class has slots, which is no property of the node at all. */
	Constants.Empty();
	Mutables.Empty();
	MutableAt.Empty();
	Slots.Empty();
	Folded.Empty();
	Kinds.Empty();

	/* What the class kept the folded values in, in the order they are numbered.
	 *
	 * A node property the compiler found to be the same everywhere is not written on the node at
	 * all: it is put once into the class's constants and the node keeps an index into them. Two
	 * nodes playing the same sequence share the one entry, and a value that never changes at run
	 * time which is most of what somebody typed into a pin is folded this way.
	 *
	 * So a node read straight off the class has no sequence, no blend space and no name: those are
	 * not missing from the asset, they are one level further in. */
	if (SerializedSparseClassData.IsValid()) {
		for (const TPair<FString, TSharedPtr<FJsonValue>>& One : SerializedSparseClassData->Values) {
			Constants.Add(One.Value);
		}
	}

	/* And what it keeps the ones that do change in, which are read the same way and are also where
	 * anything handed to a node from outside is put */
	if (AnimMutables.IsValid()) {
		for (const TPair<FString, TSharedPtr<FJsonValue>>& One : AnimMutables->Values) {
			/* Under the name the copy would use for it.
			 *
			 * The same property is written out as "FloatProperty" by one exporter and as
			 * "__FloatProperty" by another, and a hand-over names it the first way either way. */
			FString Named = JsonKeyToString(One.Key);

			Named.RemoveFromStart(TEXT("__"));

			MutableAt.Add(Named, Mutables.Num());

			Mutables.Add(One.Value);
		}
	}

	/* Which slot of a node of each kind holds which of its properties */
	for (const FUObjectJsonValueExport& One : Data.GetArray(TEXT("NodeTypeMap"))) {
		/* Spelled as a reference to the class, of which only the last part is the struct's name */
		FString Kind = One.GetString(TEXT("Key"));

		if (FString Rest; Kind.Split(TEXT("."), nullptr, &Rest, ESearchCase::IgnoreCase, ESearchDir::FromEnd)) {
			Kind = Rest;
		}

		Kind.RemoveFromEnd(TEXT("'"));

		if (Kind.IsEmpty()) continue;

		TMap<FString, int32>& Named = Slots.FindOrAdd(Kind);

		for (const FUObjectJsonValueExport& Says : One.GetObject(TEXT("Value")).GetArray(TEXT("NameToIndexMap"))) {
			Named.Add(Says.GetString(TEXT("Key")), Says.GetInteger(TEXT("Value"), INDEX_NONE));
		}
	}

	/* Where each node's own slots are, said against the node it is */
	for (const FUObjectJsonValueExport& One : Data.GetArray(TEXT("AnimNodeData"))) {
		const int32 Which = One.GetInteger(TEXT("NodeIndex"), INDEX_NONE);

		if (Which == INDEX_NONE || !One.Has(TEXT("Entries"))) continue;

		TArray<uint32> Says;

		for (const FUObjectJsonValueExport& Entry : One.GetArray(TEXT("Entries"))) {
			Says.Add(Entry.Value.IsValid() ? static_cast<uint32>(Entry.Value->AsNumber()) : MAX_uint32);
		}

		Folded.Add(Which, Says);
	}

	/* Which kind of node each of them is, asked of the engine rather than read off the name. The
	 * class names its properties after the editor's node; the slots are named after the animation
	 * node inside it, and only the engine says which goes with which. */
	for (const FString& Key : Keys) {
		FString NodeType, Rest;

		Key.Split(TEXT("_"), &NodeType, &Rest, ESearchCase::IgnoreCase, ESearchDir::FromEnd);

		if (NodeType == TEXT("AnimGraphNode") || NodeType.IsEmpty()) NodeType = Key;

		const UClass* Class = FindClassByType(NodeType);

		if (Class == nullptr) continue;

		const UAnimGraphNode_Base* Says = Cast<UAnimGraphNode_Base>(Class->GetDefaultObject());

		if (const FStructProperty* Inside = GetNodeStructProperty(Says); Inside != nullptr && Inside->Struct != nullptr) {
			Kinds.Add(Key, Inside->Struct->GetName());
		}
	}

	UE_LOG(LogReflection, Display, TEXT("the class keeps %d folded value(s) and %d that change, over %d node(s) of %d kind(s)"),
		Constants.Num(), Mutables.Num(), Folded.Num(), Slots.Num());

}

void IAnimationBlueprintImporter::Unfold(const TSharedPtr<FJsonObject>& Nodes, const TArray<FString>& Keys) const {
	if (!Nodes.IsValid() || (Constants.Num() == 0 && Mutables.Num() == 0)) return;

	int32 Put = 0;

	for (int32 Which = 0; Which < Keys.Num(); ++Which) {
		const TArray<uint32>* Entries = Folded.Find(Which);
		const FString* Kind = Kinds.Find(Keys[Which]);

		if (Entries == nullptr || Kind == nullptr) continue;

		const TMap<FString, int32>* Named = Slots.Find(*Kind);

		const TSharedPtr<FJsonObject>* Node = nullptr;

		if (Named == nullptr || !Nodes->TryGetObjectField(StringToJsonKey(Keys[Which]), Node)) continue;

		for (const TPair<FString, int32>& One : *Named) {
			if (!Entries->IsValidIndex(One.Value)) continue;

			/* Already on the node, which is what an entry saying nothing means */
			if ((*Node)->HasField(One.Key)) continue;

			const uint32 Entry = (*Entries)[One.Value];

			if (Entry == MAX_uint32) continue;

			const TArray<TSharedPtr<FJsonValue>>& From = (Entry & 0x80000000u) != 0 ? Mutables : Constants;

			const int32 At = static_cast<int32>(Entry & 0x7FFFFFFFu);

			if (!From.IsValidIndex(At)) continue;

			(*Node)->SetField(One.Key, From[At]);

			Put++;
		}
	}

	if (Put > 0) {
		UE_LOG(LogReflection, Display, TEXT("%d folded value(s) put back on the nodes that hold them"), Put);
	}
}

FName IAnimationBlueprintImporter::MemberAt(const FString& Key, const int32 Which, const uint32 Entry) const {
	const TArray<uint32>* Entries = Folded.Find(Which);
	const FString* Kind = Kinds.Find(Key);

	if (Entries == nullptr || Kind == nullptr) return NAME_None;

	const TMap<FString, int32>* Named = Slots.Find(*Kind);

	if (Named == nullptr) return NAME_None;

	for (const TPair<FString, int32>& One : *Named) {
		if (Entries->IsValidIndex(One.Value) && (*Entries)[One.Value] == Entry) return FName(*One.Key);
	}

	return NAME_None;
}

FString IAnimationBlueprintImporter::NodeAt(const TArray<FString>& Keys, const uint32 Entry, int32& Which) const {
	for (const TPair<int32, TArray<uint32>>& One : Folded) {
		if (!Keys.IsValidIndex(One.Key)) continue;

		for (const uint32 Held : One.Value) {
			if (Held != Entry) continue;

			Which = One.Key;

			return Keys[One.Key];
		}
	}

	Which = INDEX_NONE;

	return FString();
}

void IAnimationBlueprintImporter::ReadBindings(const TSharedPtr<FJsonObject>& Nodes, const TArray<FString>& Keys) {
	if (!SerializedSparseClassData.IsValid()) return;

	Bindings.Empty();

	/* Which pin of which node each of the class's hand-over properties stands for.
	 *
	 * A node that drives something of its own is fed through a property the compiler made for it,
	 * named after the node and the pin together. Read as a name it is neither: it belongs to no
	 * node's own properties and stands for no pin of anything. What pairs the two is the node
	 * itself, which lists what it is fed by alongside what each one is called on the far side. */
	TMap<FString, TPair<FString, FName>> Handed;

	if (Nodes.IsValid()) {
		for (const FString& Key : Keys) {
			const TSharedPtr<FJsonObject>* Node = nullptr;

			if (!Nodes->TryGetObjectField(StringToJsonKey(Key), Node)) continue;

			const TArray<TSharedPtr<FJsonValue>>* Fed = nullptr;
			const TArray<TSharedPtr<FJsonValue>>* Called = nullptr;

			if (!(*Node)->TryGetArrayField(TEXT("SourcePropertyNames"), Fed)) continue;
			if (!(*Node)->TryGetArrayField(TEXT("DestPropertyNames"), Called)) continue;

			for (int32 At = 0; At < Fed->Num() && At < Called->Num(); ++At) {
				if (!(*Fed)[At].IsValid() || !(*Called)[At].IsValid()) continue;

				Handed.Add((*Fed)[At]->AsString(), TPair<FString, FName>(Key, FName(*(*Called)[At]->AsString())));
			}
		}
	}

	/* What a node is handed from a variable, which is not a wire and not a value.
	 *
	 * A pin somebody bound to a variable is not written into the node and not worked out in the
	 * ubergraph either: the class copies the variable into the node's property before the graph
	 * runs, and says so as a list of copies. Read as anything else the pin comes back empty, which
	 * is what a node whose input nothing feeds looks like. */
	const TSharedPtr<FJsonObject>* Access = nullptr;

	if (!SerializedSparseClassData->TryGetObjectField(TEXT("AnimBlueprintExtension_PropertyAccess"), Access)) return;

	const TSharedPtr<FJsonObject>* Library = nullptr;

	if (!(*Access)->TryGetObjectField(TEXT("Library"), Library)) return;

	const TArray<TSharedPtr<FJsonValue>>* Steps = nullptr;
	const TArray<TSharedPtr<FJsonValue>>* Sources = nullptr;
	const TArray<TSharedPtr<FJsonValue>>* Destinations = nullptr;
	const TArray<TSharedPtr<FJsonValue>>* Batches = nullptr;

	if (!(*Library)->TryGetArrayField(TEXT("PathSegments"), Steps)) return;
	if (!(*Library)->TryGetArrayField(TEXT("SrcPaths"), Sources)) return;
	if (!(*Library)->TryGetArrayField(TEXT("DestPaths"), Destinations)) return;
	if (!(*Library)->TryGetArrayField(TEXT("CopyBatchArray"), Batches)) return;

	/* Every copy the class makes, in the order anything counting them counts */
	TArray<TSharedPtr<FJsonObject>> Copies;

	for (const TSharedPtr<FJsonValue>& Batch : *Batches) {
		const TArray<TSharedPtr<FJsonValue>>* Held = nullptr;

		if (Batch.IsValid() && Batch->AsObject().IsValid() && Batch->AsObject()->TryGetArrayField(TEXT("Copies"), Held)) {
			for (const TSharedPtr<FJsonValue>& One : *Held) Copies.Add(One->AsObject());
		}
	}

	/* What the nodes say about those copies, where they say anything.
	 *
	 * Some classes hand each node a handler naming the copies that fill it in; others keep the
	 * copies alone and let each one say outright which node property it lands on. Only the first
	 * kind says what is done to the value on the way, and only the first kind can name the node
	 * for a copy that lands in the class's own keeping rather than on a node. */
	TMap<int32, FString> Doing;
	TMap<int32, int32> Whose;

	if (const TSharedPtr<FJsonObject>* Base = nullptr; SerializedSparseClassData->TryGetObjectField(TEXT("AnimBlueprintExtension_Base"), Base)) {
		const TArray<TSharedPtr<FJsonValue>>* Handlers = nullptr;

		if ((*Base)->TryGetArrayField(TEXT("ExposedValueHandlers"), Handlers)) {
			for (int32 Which = 0; Which < Handlers->Num(); ++Which) {
				const TArray<TSharedPtr<FJsonValue>>* Records = nullptr;

				if (!(*Handlers)[Which]->AsObject()->TryGetArrayField(TEXT("CopyRecords"), Records)) continue;

				for (const TSharedPtr<FJsonValue>& Record : *Records) {
					const TSharedPtr<FJsonObject> Says = Record->AsObject();

					if (!Says.IsValid()) continue;

					const int32 Index = Says->GetIntegerField(TEXT("CopyIndex"));

					Whose.Add(Index, Which);

					if (FString Done; Says->TryGetStringField(TEXT("PostCopyOperation"), Done)) Doing.Add(Index, Done);
				}
			}
		}
	}

	/* A way to something, said as where its steps begin and how many of them there are */
	const auto Walk = [Steps](const TSharedPtr<FJsonObject>& Path, TArray<FString>& Into) {
		if (!Path.IsValid()) return;

		const int32 From = Path->GetIntegerField(TEXT("PathSegmentStartIndex"));
		const int32 Many = Path->GetIntegerField(TEXT("PathSegmentCount"));

		for (int32 At = From; At < From + Many; ++At) {
			if (!Steps->IsValidIndex(At)) return;

			FString Named;

			if ((*Steps)[At]->AsObject()->TryGetStringField(TEXT("Name"), Named)) Into.Add(Named);
		}
	};

	int32 Made = 0;

	for (int32 Index = 0; Index < Copies.Num(); ++Index) {
		const TSharedPtr<FJsonObject>& Copy = Copies[Index];

		if (!Copy.IsValid()) continue;

		TArray<FString> From;
		TArray<FString> Onto;

		if (const int32 At = Copy->GetIntegerField(TEXT("AccessIndex")); Sources->IsValidIndex(At)) {
			Walk((*Sources)[At]->AsObject(), From);
		}

		if (const int32 At = Copy->GetIntegerField(TEXT("DestAccessStartIndex")); Destinations->IsValidIndex(At)) {
			Walk((*Destinations)[At]->AsObject(), Onto);
		}

		if (From.Num() == 0 || Onto.Num() == 0) continue;

		FString Key;
		FName Member;

		if (Onto[0] == TEXT("__AnimBlueprintMutables")) {
			/* Which of the node's own the value lands on.
			 *
			 * Handed straight to a node property the way says so outright. Handed to one the class
			 * folded away it does not: a folded property that changes at run time is kept in the
			 * class's mutables, so the copy names a slot there and it is the node that says which
			 * of its properties reads that slot. */
			FString Slotted = Onto.Last();

			Slotted.RemoveFromStart(TEXT("__"));

			const int32* Slot = MutableAt.Find(Slotted);

			if (Slot == nullptr) {
				FImportIssues::Report(
					EImportIssue::Data,
					TEXT("A hand-over to a slot the class does not keep"),
					FString::Printf(TEXT("'%s' hands \"%s\" through \"%s\", and the class keeps %d slot(s), none of them under that name."),
						*GetAssetName(), *FString::Join(From, TEXT(".")), *Onto.Last(), MutableAt.Num())
				);

				continue;
			}

			const uint32 Entry = 0x80000000u | static_cast<uint32>(*Slot);

			int32 Which = Whose.FindRef(Index, INDEX_NONE);

			Key = Keys.IsValidIndex(Which) ? Keys[Which] : NodeAt(Keys, Entry, Which);

			if (Key.IsEmpty() || Which == INDEX_NONE) {
				FImportIssues::Report(
					EImportIssue::Data,
					TEXT("A hand-over with nowhere to land"),
					FString::Printf(TEXT("'%s' hands \"%s\" through \"%s\", which is slot %d, and no node reads that slot."),
						*GetAssetName(), *FString::Join(From, TEXT(".")), *Onto.Last(), *Slot)
				);

				continue;
			}

			Member = MemberAt(Key, Which, Entry);

			if (Member.IsNone()) continue;
		} else if (Onto[0].StartsWith(TEXT("__"))) {
			/* One of the hand-over properties, which is a node's pin under another name */
			const TPair<FString, FName>* Says = Handed.Find(Onto[0]);

			if (Says == nullptr) continue;

			Key = Says->Key;
			Member = Says->Value;
		} else {
			Key = Onto[0];
			Member = FName(*Onto.Last());
		}

		if (!Kinds.Contains(Key)) continue;

		bool bTurned = false;

		/* Something done to the value on the way, which a binding cannot say.
		 *
		 * A rule that is nothing but a variable turned round is never compiled as a graph: the
		 * class copies the variable and negates it as it goes, and there is no bytecode to read
		 * back. Bound as it stands the node would read the variable the right way up, which is the
		 * wrong answer every frame. So it is drawn instead the variable, then a Not which is
		 * what somebody wrote to get a copy that negates in the first place. */
		if (const FString* Done = Doing.Find(Index); Done != nullptr && !Done->EndsWith(TEXT("::None"))) {
			if (!Done->EndsWith(TEXT("::LogicalNegateBool")) || From.Num() != 1) {
				FImportIssues::Report(
					EImportIssue::Data,
					TEXT("A binding that does something to the value on the way"),
					FString::Printf(TEXT("'%s' hands \"%s\" to %s.%s as %s, which a pin binding has no way of saying."),
						*GetAssetName(), *FString::Join(From, TEXT(".")), *Key, *Member.ToString(), **Done)
				);

				continue;
			}

			bTurned = true;
		}

		Bindings.FindOrAdd(Key).Add(FHandedOver{ Member, From, bTurned });

		Made++;
	}

	if (Made > 0) {
		UE_LOG(LogReflection, Display, TEXT("%d pin(s) are handed a variable rather than fed by a wire"), Made);
	}
}

void IAnimationBlueprintImporter::Draw(UAnimGraphNode_Base* Node, const FName Member, const FString& Named, const bool bTurned, const int32 Row) const {
	UEdGraph* Into = Node->GetGraph();

	UEdGraphPin* Onto = Node->FindPin(Member, EGPD_Input);

	if (Into == nullptr || Onto == nullptr) return;

	/* Drawn once. Saying it again is how a pin learns what it carries, and the run that draws it
	 * would otherwise put down a second variable and a second Not every time it was asked. */
	if (Onto->LinkedTo.Num() > 0) return;

	/* The variable, read where the node that wants it is */
	UK2Node_VariableGet* Reads = NewObject<UK2Node_VariableGet>(Into);

	Into->AddNode(Reads, false, false);

	Reads->CreateNewGuid();
	Reads->VariableReference.SetSelfMember(FName(*Named));
	Reads->AllocateDefaultPins();
	Reads->PostPlacedNewNode();

	UEdGraphPin* Held = Reads->FindPin(FName(*Named), EGPD_Output);

	if (Held == nullptr) {
		UE_LOG(LogReflection, Warning, TEXT("\"%s\" is read for %s and hands nothing out"), *Named, *Member.ToString());

		return;
	}

	/* And the Not that turns it round, where it was turned round */
	if (bTurned) {
		UK2Node_CallFunction* Turns = NewObject<UK2Node_CallFunction>(Into);

		Into->AddNode(Turns, false, false);

		Turns->CreateNewGuid();
		Turns->FunctionReference.SetExternalMember(GET_FUNCTION_NAME_CHECKED(UKismetMathLibrary, Not_PreBool), UKismetMathLibrary::StaticClass());
		Turns->AllocateDefaultPins();
		Turns->PostPlacedNewNode();

		UEdGraphPin* Takes = Turns->FindPin(TEXT("A"), EGPD_Input);
		UEdGraphPin* Gives = Turns->GetReturnValuePin();

		if (Takes == nullptr || Gives == nullptr) {
			UE_LOG(LogReflection, Warning, TEXT("\"%s\" turned round for %s has nowhere to go"), *Named, *Member.ToString());

			return;
		}

		Held->MakeLinkTo(Takes);
		Gives->MakeLinkTo(Onto);

		Turns->NodePosX = Node->NodePosX - 220;
		Turns->NodePosY = Node->NodePosY + Row * 90;

		Reads->NodePosX = Turns->NodePosX - 220;
		Reads->NodePosY = Turns->NodePosY;

		Into->NotifyGraphChanged();

		return;
	}

	Held->MakeLinkTo(Onto);

	Reads->NodePosX = Node->NodePosX - 220;
	Reads->NodePosY = Node->NodePosY + Row * 90;

	Into->NotifyGraphChanged();
}

void IAnimationBlueprintImporter::ApplyBindings(const FString& Key, UAnimGraphNode_Base* Node) const {
	if (Node == nullptr || Key.IsEmpty()) return;

	const TArray<FHandedOver>* Says = Bindings.Find(Key);

	if (Says == nullptr) return;

	/* Drawn rather than bound.
	 *
	 * A variable read straight into a pin and a variable bound to that pin come out of the compiler
	 * as the same thing: it turns the simple read into a copy so the graph never has to run for it.
	 * So the copy does not say which was written, and either is a fair reading of it.
	 *
	 * Drawn is the one worth having. It is the same shape as everything else in the graph, it can
	 * be followed by eye, and it does not depend on a node's private bookkeeping surviving the trip.
	 * A way in that needs more than a single read something reached through a struct has no
	 * one node to draw, so that stays a binding. */
	int32 Row = 0;

	for (const FHandedOver& One : *Says) {
		if (One.Path.Num() == 1) {
			Draw(Node, One.Member, One.Path[0], One.bTurned, Row++);

			continue;
		}

#if !UE4_25_BELOW
		FAnimGraphNodePropertyBinding Binding;

		Binding.PropertyName = One.Member;
		Binding.PropertyPath = One.Path;
		Binding.PathAsText = FText::FromString(FString::Join(One.Path, TEXT(".")));
		Binding.bIsBound = true;

		/* The pin settles what it carries, which the way to it does not say */
		if (const UEdGraphPin* Pin = Node->FindPin(One.Member, EGPD_Input)) {
			Binding.PinType = Pin->PinType;
		}

		if (!AddPropertyBinding(Node, One.Member, Binding)) {
			UE_LOG(LogReflection, Warning, TEXT("\"%s\" would be handed to %s.%s, and there is no pin of that name"),
				*Binding.PathAsText.ToString(), *Node->GetClass()->GetName(), *One.Member.ToString());
		}
#endif
	}
}

void IAnimationBlueprintImporter::ExposeHandedOver(UAnimGraphNode_Base* Node, const TArray<FString>& Named) const {
	/* Taken by reflection, since the node keeps the list to itself */
	const FArrayProperty* Exposed = FindFProperty<FArrayProperty>(Node->GetClass(), TEXT("CustomPinProperties"));

	if (Exposed == nullptr) return;

	const FStructProperty* Inner = CastField<FStructProperty>(Exposed->Inner);

	if (Inner == nullptr || Inner->Struct != FOptionalPinFromProperty::StaticStruct()) return;

	FScriptArrayHelper Held(Exposed, Exposed->ContainerPtrToValuePtr<void>(Node));

	for (const FString& One : Named) {
		const FName Which(*One);

		bool bAlready = false;

		for (int32 At = 0; At < Held.Num(); ++At) {
			FOptionalPinFromProperty* Says = reinterpret_cast<FOptionalPinFromProperty*>(Held.GetRawPtr(At));

			if (Says->PropertyName != Which) continue;

			Says->bShowPin = true;
			bAlready = true;

			break;
		}

		if (bAlready) continue;

		/* Said the way the engine says it when it brings an older node forward: a name, and that it
		 * is shown. What the value is stays the far side's business, and the node reads it there. */
		FOptionalPinFromProperty* Says = reinterpret_cast<FOptionalPinFromProperty*>(Held.GetRawPtr(Held.AddValue()));

		Says->PropertyName = Which;
		Says->bCanToggleVisibility = true;
		Says->bShowPin = true;
	}

	UE_LOG(LogReflection, Display, TEXT("\"%s\" hands over %d value(s), which it is given pins for"), *Node->GetName(), Named.Num());
}

void IAnimationBlueprintImporter::DrawBindings(FUObjectExportContainer* Container) const {
	if (Container == nullptr) return;

	/* Done last of all.
	 *
	 * What is drawn sits beside the node it feeds, so it can only be put down once that node is
	 * where it is going to stay. Laying the graph out moves every node it knows about and none of
	 * these, so anything drawn before it ends up beside where a node used to be. */
	for (const FUObjectExport* NodeExport : Container->Exports) {
		if (UAnimGraphNode_Base* Node = Cast<UAnimGraphNode_Base>(NodeExport->Object)) {
			ApplyBindings(NodeExport->GetName().ToString(), Node);
		}
	}
}

void IAnimationBlueprintImporter::ShowBoundPins(const TSharedPtr<FJsonObject>& AnimNodeProperties, FUObjectExportContainer* Container) const {
	if (Container == nullptr) return;

	for (const FUObjectExport* NodeExport : Container->Exports) {
		UAnimGraphNode_Base* Node = Cast<UAnimGraphNode_Base>(NodeExport->Object);

		if (Node == nullptr) continue;

		const TArray<FHandedOver>* Says = Bindings.Find(NodeExport->GetName().ToString());

		if (Says == nullptr) continue;

		/* Drawn on the node rather than left in the details.
		 *
		 * Whether a property was ever drawn as a pin is the editor's own bookkeeping and the cook
		 * throws it away, so the engine falls back to what the property says about itself and
		 * most of them say to stay hidden. A property nobody touched staying hidden is right; one
		 * somebody bound to a variable is not, since the whole reason it is bound is that it was
		 * being driven, and driven properties are read off the node in every other graph here.
		 *
		 * Both come back from the cook the same way, so this cannot be read from the asset. It is
		 * the more useful of the two readings rather than the provably right one. */
		bool bAny = false;

		for (FOptionalPinFromProperty& Held : Node->ShowPinForProperties) {
			for (const FHandedOver& One : *Says) {
				if (Held.PropertyName != One.Member || Held.bShowPin) continue;

				Held.bShowPin = true;

				bAny = true;
			}
		}

		if (bAny) {
			Node->ReconstructNode();
		}
	}
}

void IAnimationBlueprintImporter::CreateGraph(const TSharedPtr<FJsonObject>& AnimNodeProperties, UEdGraph* AnimGraph, FUObjectExportContainer* Container) {
	/* Nowhere to put them.
	 *
	 * An animation layer interface has no animation graph of its own: it says what its layers take
	 * and hand back, and draws none of them. Carried on regardless, every node is made belonging to
	 * nothing, and a node belonging to nothing is not something the engine will make. It does not
	 * refuse either it stops the editor where it stands. */
	if (AnimGraph == nullptr) {
		const int32 Waiting = AnimNodeProperties.IsValid() ? AnimNodeProperties->Values.Num() : 0;

		if (Waiting > 0) {
			FImportIssues::Report(
				EImportIssue::Data,
				TEXT("Animation nodes with no graph to lay them out in"),
				FString::Printf(TEXT("'%s' carries %d animation node(s) and has no animation graph to put them in."), *GetAssetName(), Waiting)
			);
		}

		return;
	}

	/* Kept, since a pose saved in this graph can be borrowed from any other */
	AnimNodeContainers.AddUnique(Container);

	/* Remove all pre-existing nodes */
	if (AnimGraph) {
		for (UEdGraphNode* Node : AnimGraph->Nodes) {
			if (Node) {
				Node->BreakAllNodeLinks();
				Node->ConditionalBeginDestroy();
			}
		}
        
		AnimGraph->Nodes.Empty();
		AnimGraph->SubGraphs.Empty();
	}
	
	CreateAnimGraphNodes(AnimGraph, AnimNodeProperties, *Container);
	AddNodesToGraph(AnimGraph, Container);

	HandleNodeDeserialization(Container);
	ShowBoundPins(AnimNodeProperties, Container);
	ConnectAnimGraphNodes(Container, AnimGraph);
	AutoLayoutAnimGraphNodes(Container->Exports);
	DrawBindings(Container);

	for (const FUObjectExport* ExportNode : Container->Exports) {
		const TSharedPtr<FJsonObject> ExportJsonObject = ExportNode->JsonObject;
		
		if (UAnimGraphNode_StateMachine* StateMachine = Cast<UAnimGraphNode_StateMachine>(ExportNode->Object)) {
			UAnimationStateMachineGraph* EditorStateMachineGraph = CastChecked<UAnimationStateMachineGraph>(FBlueprintEditorUtils::CreateNewGraph(StateMachine, NAME_None, UAnimationStateMachineGraph::StaticClass(), UAnimationStateMachineSchema::StaticClass()));
			EditorStateMachineGraph->OwnerAnimGraphNode = StateMachine;

			const TSharedPtr<FJsonObject> StateMachineObject = BakedStateMachines[ExportJsonObject->GetIntegerField(TEXT("StateMachineIndexInClass"))]->AsObject();
					
			FString MachineName = StateMachineObject->GetStringField(TEXT("MachineName"));
			EditorStateMachineGraph->Rename(*MachineName);

			const UEdGraphSchema* Schema = EditorStateMachineGraph->GetSchema();
			Schema->CreateDefaultNodesForGraph(*EditorStateMachineGraph);

			UEdGraph* ParentGraph = StateMachine->GetGraph();
	
			if(ParentGraph->SubGraphs.Find(EditorStateMachineGraph) == INDEX_NONE) {
				ParentGraph->Modify();
				ParentGraph->SubGraphs.Add(EditorStateMachineGraph);
			}

			StateMachine->EditorStateMachineGraph = EditorStateMachineGraph;
			CreateStateMachineGraph(EditorStateMachineGraph, StateMachineObject, GetObjectSerializer(), RootAnimNodeContainer, ReversedNodesKeys, this, AnimBlueprint);

			/* Add nodes to graph */
			if (!StateMachineObject->HasField(TEXT("States"))) continue;

			TArray<TSharedPtr<FJsonValue>> States = StateMachineObject->GetArrayField(TEXT("States"));

			for (const TSharedPtr<FJsonValue>& StateValue : States) {
				const TSharedPtr<FJsonObject> StateObject = StateValue->AsObject();
				FString StateName = StateObject->GetStringField(TEXT("StateName"));

				UAnimationStateGraph* Graph = nullptr;

				for (UEdGraph* SubGraph : EditorStateMachineGraph->SubGraphs) {
					if (SubGraph->GetName() == StateName) {
						Graph = Cast<UAnimationStateGraph>(SubGraph);
					}
				}

				TSharedPtr<FJsonObject> StateMachineAnimNodeProperties = MakeShared<FJsonObject>();

				for (const auto& Pair : RootAnimNodeProperties->Values) {
					const FString Key = JsonKeyToString(Pair.Key);
					const TSharedPtr<FJsonObject> Value = Pair.Value->AsObject();

					if (!Value.IsValid()) continue;

					if (Value->HasField(TEXT("State")) && Value->HasField(TEXT("Machine"))) {
						const FString NodeStateName = Value->GetStringField(TEXT("State"));
						const FString NodeMachineName = Value->GetStringField(TEXT("Machine"));

						if (StateName == NodeStateName && NodeMachineName == MachineName) {
							StateMachineAnimNodeProperties->SetObjectField(Key, Value);
						}
					}
				}

				if (Graph) {
					FUObjectExportContainer* StateMachineContainer = new FUObjectExportContainer();
					CreateGraph(StateMachineAnimNodeProperties, Graph, StateMachineContainer);

					if (Graph->MyResultNode) {
						Graph->MyResultNode->BreakAllNodeLinks();
						Graph->RemoveNode(Graph->MyResultNode);
						Graph->MyResultNode->ConditionalBeginDestroy();
						Graph->MyResultNode = nullptr;
					}

					for (const FUObjectExport* StateMachineExport : StateMachineContainer->Exports) {
						if (UAnimGraphNode_StateResult* StateResult = Cast<UAnimGraphNode_StateResult>(StateMachineExport->Object)) {
							Graph->MyResultNode = StateResult;
						}
					}
				}
			}
		}
	}
}

void inline LinkPoseInputPin(const FString& PinName, UAnimGraphNode_Base* Node, UAnimGraphNode_Base* TargetNode, UEdGraph* AnimGraph) {
	UEdGraphPin* InputPin = Node->FindPin(PinName, EGPD_Input);
	UEdGraphPin* OutputPin = GetFirstOutputPin(TargetNode);
	
	if (InputPin && OutputPin) {
		InputPin->MakeLinkTo(OutputPin);
		InputPin->DefaultValue.Reset();
		
		Node->Modify();
		TargetNode->Modify();
		AnimGraph->Modify();

		return;
	}

	/* Said rather than passed over, since a pose that goes nowhere is a graph of loose nodes and
	 * nothing about the node says which of the two ends was missing */
	UE_LOG(LogReflection, Warning, TEXT("\"%s\" was to feed %s.%s: %s"),
		*TargetNode->GetName(), *Node->GetName(), *PinName,
		InputPin == nullptr
			? (OutputPin == nullptr ? TEXT("neither end has a pin for it") : TEXT("it has no pin of that name"))
			: TEXT("what feeds it hands nothing out"));
}

void IAnimationBlueprintImporter::UpdateBlendListByEnumVisibleEntries(FUObjectExport* NodeExport, FUObjectExportContainer* Container, UEdGraph* AnimGraph) {
	TSharedPtr<FJsonObject> NodeJsonObject = NodeExport->JsonObject;
	UAnimGraphNode_BlendListByEnum* BlendListByEnum = Cast<UAnimGraphNode_BlendListByEnum>(NodeExport->Object);
	
    if (!BlendListByEnum || !NodeJsonObject) {
        return;
    }

	/* Get the BoundEnum set before in the BlendListByEnum node */
    const UEnum* BoundEnum = BlendListByEnum->GetEnum();
    if (!BoundEnum) {
        return;
    }

	TArray<TSharedPtr<FJsonValue>> EnumToPoseIndexArray = NodeJsonObject->GetArrayField(TEXT("EnumToPoseIndex"));

	/* Create VisibleEnumEntries array using the enum */
	TArray<FName> VisibleEnumEntries;

    for (int32 i = 0; i < EnumToPoseIndexArray.Num(); i++) {
        const int32 Value = static_cast<int32>(EnumToPoseIndexArray[i]->AsNumber());
        
        if (Value != 0) {
            if (BoundEnum->NumEnums() > i) {
                FString EnumEntryName = BoundEnum->GetNameByIndex(i).ToString();
                if (EnumEntryName.Contains("::")) {
                    EnumEntryName.Split("::", nullptr, &EnumEntryName);
                }

                VisibleEnumEntries.Add(FName(*EnumEntryName));
            }
        }
    }
	
    if (const FArrayProperty* VisEnumArrayProp = FindFProperty<FArrayProperty>(BlendListByEnum->GetClass(), TEXT("VisibleEnumEntries"))) {
        const void* ArrayPtr = VisEnumArrayProp->ContainerPtrToValuePtr<void>(BlendListByEnum);
        FScriptArrayHelper ArrayHelper(VisEnumArrayProp, ArrayPtr);
    	
        ArrayHelper.Resize(0);
    	
        const FNameProperty* NameProp = CastField<FNameProperty>(VisEnumArrayProp->Inner);
        if (!NameProp) {
            return;
        }
    	
        for (const FName& Entry : VisibleEnumEntries) {
            const int32 NewIdx = ArrayHelper.AddValue();
            void* ElemPtr = ArrayHelper.GetRawPtr(NewIdx);
        	
            NameProp->SetPropertyValue(ElemPtr, Entry);
        }
    }

	BlendListByEnum->ReconstructNode();

	TArray<TSharedPtr<FJsonValue>> BlendPoseArray = NodeJsonObject->GetArrayField(TEXT("BlendPose"));

	int BlendPoseIndex = 0;
	if (BlendPoseArray.IsValidIndex(0)) {
		FString LinkID = BlendPoseArray[0]->AsObject()->GetStringField(TEXT("LinkID"));
		const FString IndexedPinName = FString::Printf(TEXT("BlendPose_%d"), 0);

		FUObjectExport* TargetNodeExport = Container->Find(LinkID);
		UAnimGraphNode_Base* TargetNode = Cast<UAnimGraphNode_Base>(TargetNodeExport->Object);

		LinkPoseInputPin(IndexedPinName, BlendListByEnum, TargetNode, AnimGraph);

		BlendPoseIndex++;
	}

	/* Map poses corresponding to the bound enum */
	for (int32 EnumIndex = 0; EnumIndex < BoundEnum->NumEnums() - 1; ++EnumIndex) {
		if (EnumIndex < EnumToPoseIndexArray.Num()) {
			const int PoseIndex = EnumToPoseIndexArray[EnumIndex]->AsNumber();

			if (PoseIndex == 0) continue;

			if (PoseIndex < BlendPoseArray.Num()) {
				FString LinkID = BlendPoseArray[PoseIndex]->AsObject()->GetStringField(TEXT("LinkID"));
                const FString IndexedPinName = FString::Printf(TEXT("BlendPose_%d"), BlendPoseIndex);

				FUObjectExport* TargetNodeExport = Container->Find(LinkID);
				UAnimGraphNode_Base* TargetNode = Cast<UAnimGraphNode_Base>(TargetNodeExport->Object);

				LinkPoseInputPin(IndexedPinName, BlendListByEnum, TargetNode, AnimGraph);
			}
		}

		BlendPoseIndex++;
	}
}

void IAnimationBlueprintImporter::CreateAnimGraphNodes(UEdGraph* AnimGraph, const TSharedPtr<FJsonObject>& AnimNodeProperties, FUObjectExportContainer& OutContainer) {
	for (const auto& Pair : AnimNodeProperties->Values) {
		FString Key = JsonKeyToString(Pair.Key);

		TSharedPtr<FJsonObject> Value = Pair.Value->AsObject();

		/* Find the NodeType and GUID from the key */
		FString NodeType, NodeStringGUID; {
			Key.Split(TEXT("_"), &NodeType, &NodeStringGUID, ESearchCase::IgnoreCase, ESearchDir::FromEnd);

			/* Handle case for format: "AnimGraphNode[0]" */
			if (Key.Contains("[")) {
				FString CleanKey = Key.Left(Key.Find("["));
				
				TArray<FString> Parts; {
					CleanKey.ParseIntoArray(Parts, TEXT("_"));
				}
				
				NodeType = Parts.Num() >= 2 ? Parts[0] + TEXT("_") + Parts[1] : CleanKey;
				NodeStringGUID.Empty();
			}
		}

		if (NodeType == "AnimGraphNode") {
			NodeType = Key;
		}

		/* Redirections */
		if (NodeType == "AnimGraphNode_SubInput") {
			NodeType = "AnimGraphNode_LinkedInputPose";
		}

		/* Only add json object data, transition result is handled different */
		if (NodeType == "AnimGraphNode_TransitionResult") {
			OutContainer.Exports.Add(
				new FUObjectExport(
					FName(*Key),
					FName(*NodeType),
					StringToName(AnimGraph->GetName()),
					Value,
					nullptr,
					nullptr
				)
			);

			continue;
		}

		/* Parse the NodeGuid, if not parsed properly, generate a new one.
		 *
		 * UE4 named node properties after the node's guid ("AnimGraphNode_ApplyAdditive_10AB22C6...") so it
		 * parses straight back out. UE5 numbers them instead ("AnimGraphNode_ModifyBone_3"), so the parse fails
		 * for every node and FGuid() is all zeroes, not a fresh guid, so they would all end up sharing one.
		 * FAnimBlueprintCompilerContext does NodeGuidToIndexMap.Add(Node->NodeGuid, Index) per node, and TMap::Add
		 * overwrites on a duplicate key, so a whole graph of zeroes collapses to a single entry pointing at
		 * whichever node compiled last. Everything that resolves a node through that map afterward state
		 * machines, asset players, blend space graphs then reads the wrong index. */
		FGuid NodeGuid; {
			FGuid::Parse(NodeStringGUID, NodeGuid);

			if (!NodeGuid.IsValid()) NodeGuid = FGuid::NewGuid();
		}

		const UClass* Class = FindClassByType(NodeType);

		if (!Class) {
			UE_LOG(LogReflection, Warning, TEXT("\"%s\" is a %s, which this build has no node for"), *Key, *NodeType);

			continue;
		}

		UAnimGraphNode_Base* Node = NewObject<UAnimGraphNode_Base>(AnimGraph, ToNewObjectClass(Class), NAME_None, RF_Transactional);
		Node->NodeGuid = NodeGuid;

		/* Add new node */
		OutContainer.Exports.Add(
			new FUObjectExport(
				FName(*Key),
				FName(*NodeType),
				StringToName(AnimGraph->GetName()),
				Value,
				Node,
				AnimGraph
			)
		);
	}
}

void IAnimationBlueprintImporter::AddNodesToGraph(UEdGraph* AnimGraph, FUObjectExportContainer* Container) {
    for (const FUObjectExport* Export : Container->Exports) {
        if (!IsValid(Export->Object) || !Export->JsonObject.IsValid()) {
            continue;
        }

        UAnimGraphNode_Base* Node = Cast<UAnimGraphNode_Base>(Export->Object);

        Node->Rename(nullptr, AnimGraph);
        AnimGraph->Nodes.Add(Node);
        Node->Modify();
    }
}

void IAnimationBlueprintImporter::HandleNodeDeserialization(FUObjectExportContainer* Container) {
	GetObjectSerializer()->GetPropertySerializer()->BlacklistedPropertyNames.Add(TEXT("LinkID"));

	for (FUObjectExport* NodeExport : Container->Exports) {
		if (NodeExport->Object == nullptr) continue;

		UAnimGraphNode_Base* Node = Cast<UAnimGraphNode_Base>(NodeExport->Object);
		TSharedPtr<FJsonObject> NodeProperties = NodeExport->JsonObject;

		/* Post-processing modifications ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ */
		if (NodeProperties->HasField(TEXT("GroupRole")) && NodeProperties->HasField(TEXT("GroupIndex"))) {
			const int GroupIndexInteger = NodeProperties->GetIntegerField(TEXT("GroupIndex"));

			/* -1 is no group role */
			if (GroupIndexInteger != -1) {
				TSharedPtr<FJsonObject> SyncGroup = MakeShared<FJsonObject>();
				FString SyncGroupName = SyncGroupNames[GroupIndexInteger];
			
				SyncGroup->SetStringField(TEXT("GroupName"), SyncGroupName);
				SyncGroup->SetStringField(TEXT("GroupRole"), NodeProperties->GetStringField(TEXT("GroupRole")));

				NodeProperties->SetObjectField(TEXT("SyncGroup"), SyncGroup);
			}
		}

#if ENGINE_UE4
		/* UE5+ games use PhysicsBodyDefinitions for AnimGraphNode_AnimDynamics */
		if (NodeProperties->HasField(TEXT("PhysicsBodyDefinitions"))) {
			TSharedPtr<FJsonObject> PhysicsBodyDefinition = NodeProperties->GetArrayField(TEXT("PhysicsBodyDefinitions"))[0]->AsObject();
			if (PhysicsBodyDefinition.IsValid()) {
				for (const auto& Pair : PhysicsBodyDefinition->Values) {
					NodeExport->JsonObject->SetField(Pair.Key, Pair.Value);
				}
			}
		}
#else
		/* UE5+ games use PhysicsBodyDefinitions for AnimGraphNode_AnimDynamics */
		if (NodeExport->HasProperty("BoxExtents")) {
			TSharedPtr<FJsonObject> PhysicsBodyDefinition = MakeShared<FJsonObject>();
			auto& RootValues = NodeExport->JsonObject->Values;

			auto MoveField = [&](const FString& Key) {
				if (RootValues.Contains(StringToJsonKey(Key))) {
					if (Key == TEXT("LocalJointOffset")) {
						TSharedPtr<FJsonObject> Original = RootValues[StringToJsonKey(Key)]->AsObject();

						if (Original.IsValid()) {
							TSharedPtr<FJsonObject> VecObj = MakeShared<FJsonObject>(*Original);

							for (const auto& Pair : Original->Values) {
								if (Pair.Value->Type == EJson::Number && Pair.Value->AsNumber() != 0.0) {
									VecObj->SetNumberField(Pair.Key, -Pair.Value->AsNumber());
								}
								else {
									VecObj->SetField(Pair.Key, Pair.Value);
								}
							}

							PhysicsBodyDefinition->SetObjectField(Key, VecObj);
							return;
						}
					}

					PhysicsBodyDefinition->SetField(Key, RootValues[StringToJsonKey(Key)]);
				}
			};

			/* Move all PhysicsBodyDefinition related fields */
			MoveField(TEXT("BoundBone"));
			MoveField(TEXT("BoxExtents"));
			MoveField(TEXT("LocalJointOffset"));
			MoveField(TEXT("ConstraintSetup"));
			MoveField(TEXT("CollisionType"));
			MoveField(TEXT("SphereCollisionRadius"));

			/* Create array and assign */
			TArray<TSharedPtr<FJsonValue>> PhysicsBodyDefinitionsArray;
			PhysicsBodyDefinitionsArray.Add(MakeShared<FJsonValueObject>(PhysicsBodyDefinition));

			NodeExport->JsonObject->SetArrayField(TEXT("PhysicsBodyDefinitions"), PhysicsBodyDefinitionsArray);
		}
#endif

#if ENGINE_UE4
		/* Looks like UE5 flipped axes on LocalJointOffset */
		if (GReflectionRuntime.IsUE5OrNewer()) {
			if (NodeProperties->HasField(TEXT("LocalJointOffset"))) {
				auto LocalJointOffset = NodeProperties->GetObjectField(TEXT("LocalJointOffset"));
				LocalJointOffset->SetNumberField("X", -LocalJointOffset->GetNumberField(TEXT("X")));
				LocalJointOffset->SetNumberField("Y", -LocalJointOffset->GetNumberField(TEXT("Y")));
				LocalJointOffset->SetNumberField("Z", -LocalJointOffset->GetNumberField(TEXT("Z")));
			}
		}
#endif
		
		/* What the node hands the thing it runs, which is rebuilt rather than read back.
		 *
		 * A node that drives something of its own a control rig, a linked graph is handed its
		 * values through properties the compiler makes on the class, each named after the node that
		 * wanted it. The class still names them, and they are the names of a node that no longer
		 * exists: this one was made fresh, so the properties it asks for will be called something
		 * else entirely.
		 *
		 * Read back as they stand, the node names properties nothing declares, and what it hands
		 * over is read off the end of them the first time the graph runs.
		 *
		 * The half worth keeping is the other one, which says what each value is called on the far
		 * side. Given pins for those, the compiler makes the properties and names both halves
		 * itself, against this node. */
		TArray<FString> Handing;

		if (Cast<UAnimGraphNode_CustomProperty>(Node) != nullptr) {
			if (const TArray<TSharedPtr<FJsonValue>>* Listed = nullptr; NodeProperties->TryGetArrayField(TEXT("DestPropertyNames"), Listed)) {
				for (const TSharedPtr<FJsonValue>& One : *Listed) {
					if (One.IsValid()) Handing.Add(One->AsString());
				}
			}

			NodeProperties->RemoveField(TEXT("SourcePropertyNames"));
			NodeProperties->RemoveField(TEXT("DestPropertyNames"));
		}

		/* What rig to run, said the way this engine says it.
		 *
		 * A newer engine keeps it behind a reference that names either a class or an asset; an
		 * older one names the class outright. Written by the newer and read by the older, the
		 * property the asset names is not on the node at all, so it is passed over in silence and
		 * the node comes back with no rig to spawn which the graph will not run without.
		 *
		 * Asked of the node rather than of the version, so the engine that already has the newer
		 * shape is left to read it as it stands. */
		for (const TPair<FString, FString>& Says : {
			TPair<FString, FString>(TEXT("ControlRigAssetReference"), TEXT("ControlRigClass")),
			TPair<FString, FString>(TEXT("DefaultControlRigAssetReference"), TEXT("DefaultControlRigClass"))
		}) {
			if (!NodeProperties->HasField(Says.Key)) continue;

			const FStructProperty* Inside = GetNodeStructProperty(Node);

			if (Inside == nullptr || Inside->Struct == nullptr) continue;
			if (Inside->Struct->FindPropertyByName(FName(*Says.Key)) != nullptr) continue;
			if (Inside->Struct->FindPropertyByName(FName(*Says.Value)) == nullptr) continue;

			if (const TSharedPtr<FJsonObject>* Held = nullptr; NodeProperties->TryGetObjectField(Says.Key, Held)) {
				if (const TSharedPtr<FJsonObject>* Named = nullptr; (*Held)->TryGetObjectField(TEXT("BlueprintRigClass"), Named)) {
					NodeProperties->SetObjectField(Says.Value, *Named);

					UE_LOG(LogReflection, Display, TEXT("\"%s\" names its rig through %s, which this build reads as %s"),
						*NodeExport->GetName().ToString(), *Says.Key, *Says.Value);
				}
			}

			NodeProperties->RemoveField(Says.Key);
		}

		GetObjectSerializer()->DeserializeObjectProperties(NodeProperties, Node);

		/* Once what it runs is known, since the pins are drawn from what that says it takes */
		if (Handing.Num() > 0) {
			ExposeHandedOver(Node, Handing);
		}

		/* Specific needs for certain nodes ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ */
		if (UAnimGraphNode_SaveCachedPose* SaveCachedPose = Cast<UAnimGraphNode_SaveCachedPose>(Node)) {
			SaveCachedPose->CacheName = NodeProperties->GetStringField(TEXT("CachePoseName"));
		}

		if (UAnimGraphNode_UseCachedPose* UseCachedPose = Cast<UAnimGraphNode_UseCachedPose>(Node)) {
			/* The name it borrows under, said whether or not the saver is ever found.
			 *
			 * The engine looks the saver up by this name itself, over every graph the blueprint
			 * has, whenever the node it is holding turns out to be no good. Left empty there is
			 * nothing to look up: the compiler reaches for the node instead and goes down on it. */
			if (FString Cached; NodeProperties->TryGetStringField(TEXT("CachePoseName"), Cached)) {
				/* Set the way the class says to, since the node keeps it to itself */
				if (FStrProperty* Under = FindFProperty<FStrProperty>(UAnimGraphNode_UseCachedPose::StaticClass(), TEXT("NameOfCache"))) {
					Under->SetPropertyValue_InContainer(UseCachedPose, Cached);
				}
			}

			if (NodeProperties->HasField(TEXT("LinkToCachingNode"))) {
				const TSharedPtr<FJsonObject> LinkToCachingNode = NodeProperties->GetObjectField(TEXT("LinkToCachingNode"));

				if (LinkToCachingNode->HasField(TEXT("LinkID"))) {
					/* Kept until every graph has been laid out.
					 *
					 * The node that saved a pose is not in the graph borrowing it, and is not always
					 * in a graph made before it: a layer saves a pose the animation graph borrows, and
					 * a state machine borrows one saved outside it. Looked for while this graph is
					 * being laid out, it is looked for in graphs that do not exist yet, and each of
					 * those comes out as a pose borrowed from nothing no name on the node, and
					 * nothing drawn between the two. */
					Borrowed.Add(TPair<TWeakObjectPtr<UAnimGraphNode_UseCachedPose>, FString>(UseCachedPose, LinkToCachingNode->GetStringField(TEXT("LinkID"))));
				}
			}
		}

		HandlePropertyBinding(NodeExport, GetContainer()->JsonObjects, Node, this, AnimBlueprint);

		/* And whatever the class hands it, which is said the way newer engines say it */
		ApplyBindings(NodeExport->GetName().ToString(), Node);

		const UReflectionSettings* Settings = GetSettings();
		if (Settings->AssetSettings.AnimationBlueprint.NodeIDComments) {
			Node->NodeComment = NodeExport->GetName().ToString();
			Node->bCommentBubbleVisible = true;
		}
		
		Node->AllocateDefaultPins();
		Node->Modify();
		Node->PostPlacedNewNode();
	}
}

void IAnimationBlueprintImporter::ConnectAnimGraphNodes(FUObjectExportContainer* Container, UEdGraph* AnimGraph) {
    for (FUObjectExport* Export : Container->Exports) {
        UAnimGraphNode_Base* Node = Cast<UAnimGraphNode_Base>(Export->Object);
        const TSharedPtr<FJsonObject> Json = Export->JsonObject;

        if (Cast<UAnimGraphNode_BlendListByEnum>(Node)) {
            UpdateBlendListByEnumVisibleEntries(Export, Container, AnimGraph);
        	continue;
        }
    	
        for (const auto& Pair : Json->Values) {
            const FString Key = JsonKeyToString(Pair.Key);
            const TSharedPtr<FJsonValue>& Value = Pair.Value;
            
            if (Value->Type == EJson::Array) {
                const TArray<TSharedPtr<FJsonValue>>& JsonArray = Value->AsArray();
                
                for (int32 Index = 0; Index < JsonArray.Num(); ++Index) {
                    const TSharedPtr<FJsonValue>& Elem = JsonArray[Index];
                    
                    if (!Elem.IsValid() || !Elem->AsObject().IsValid()) {
                        continue;
                    }
                    
                    const TSharedPtr<FJsonObject>& Obj = Elem->AsObject();
                    if (!Obj->HasField(TEXT("LinkID"))) {
                        continue;
                    }
                    
                    const FString LinkID = Obj->GetStringField(TEXT("LinkID"));
                    UAnimGraphNode_Base* TargetNode = Cast<UAnimGraphNode_Base>(Container->Find(LinkID)->Object);
                    
                    if (!TargetNode) {
                        continue;
                    }
                    
                    const FStructProperty* NodeProp = GetNodeStructProperty(Node);
                    if (!NodeProp) {
                        continue;
                    }
                    
                    for (TFieldIterator<FProperty> It(NodeProp->Struct); It; ++It) {
                        FProperty* Property = *It;
                        
                        if (Property->GetName() != JsonKeyToString(Pair.Key)) {
                            continue;
                        }
                        
                        if (const FArrayProperty* ArrayProp = CastField<FArrayProperty>(Property)) {
                            const FStructProperty* InnerStruct = CastField<FStructProperty>(ArrayProp->Inner);
                            
                            if (!InnerStruct || !InnerStruct->Struct->IsChildOf(FPoseLinkBase::StaticStruct())) {
                                continue;
                            }
                            
                            const FString IndexedPinName = FString::Printf(TEXT("%s_%d"), *Pair.Key, Index);
                            LinkPoseInputPin(IndexedPinName, Node, TargetNode, AnimGraph);
                        }
                    }
                }
            }
            
            if (Value->Type == EJson::Object && Value->AsObject()->HasTypedField<EJson::String>(TEXT("LinkID"))) {
                /* Borrowing a cached pose is not a pin being fed.
                 *
                 * It reads like every other link and it is not one: it names the node that saved
                 * the pose rather than something feeding a pin, and neither end has a pin for it.
                 * Which node it is gets settled once every graph exists, since the saver is rarely
                 * in the graph doing the borrowing. */
                if (Key == TEXT("LinkToCachingNode")) {
                    continue;
                }

                const FString LinkID = Value->AsObject()->GetStringField(TEXT("LinkID"));
                UAnimGraphNode_Base* TargetNode = Cast<UAnimGraphNode_Base>(Container->Find(LinkID)->Object);

                if (!TargetNode) {
                    UE_LOG(LogReflection, Warning, TEXT("\"%s\" feeds %s.%s, and is not in the same graph as it"), *LinkID, *Export->GetName().ToString(), *Key);

                    continue;
                }
                
                const FStructProperty* NodeProp = GetNodeStructProperty(Node);

                if (!NodeProp) {
                    continue;
                }
                
                for (TFieldIterator<FProperty> It(NodeProp->Struct); It; ++It) {
                    const FProperty* Property = *It;
                    
                    if (Property->GetName() != JsonKeyToString(Pair.Key)) {
                        continue;
                    }
                    
                    LinkPoseInputPin(Key, Node, TargetNode, AnimGraph);
                }
            }
        }
    }
}

/* In newer versions of Unreal Engine, EvaluateGraphExposedInputs was moved to the main AnimBlueprintGeneratedClass class */
/* Here, we move them into the node data to use more easily */
void IAnimationBlueprintImporter::ProcessEvaluateGraphExposedInputs(const TSharedPtr<FJsonObject>& AnimNodeProperties) const {
	if (!GetAssetData()->HasField(TEXT("EvaluateGraphExposedInputs"))) return;
	TArray<TSharedPtr<FJsonValue>> EvaluateInputs = GetAssetData()->GetArrayField(TEXT("EvaluateGraphExposedInputs"));
	
	for (const auto& Value : EvaluateInputs) {
		TSharedPtr<FJsonObject> InputObj = Value->AsObject();
		
		FString NodeName = InputObj->GetObjectField(TEXT("ValueHandlerNodeProperty"))->GetStringField(TEXT("ObjectName")); {
			NodeName.Split(":", nullptr, &NodeName);
			NodeName = NodeName.Replace(TEXT("'"), TEXT(""));	
		}
		
		AnimNodeProperties->GetObjectField(NodeName)->SetObjectField(TEXT("EvaluateGraphExposedInputs"), InputObj);
	}
}

void IAnimationBlueprintImporter::SettleInputPoses(UEdGraph* Layer) const {
	if (Layer == nullptr || AnimBlueprint == nullptr) return;

	TArray<UAnimGraphNode_LinkedInputPose*> Taking;
	Layer->GetNodesOfClass<UAnimGraphNode_LinkedInputPose>(Taking);

	if (Taking.Num() == 0) return;

	/* The layer as whichever interface declares it */
	UClass* Declaring = nullptr;
	UFunction* Signature = nullptr;

	for (const FBPInterfaceDescription& Implemented : AnimBlueprint->ImplementedInterfaces) {
		UClass* Interface = Implemented.Interface;

		if (Interface == nullptr) continue;

		if (UFunction* Found = Interface->FindFunctionByName(Layer->GetFName())) {
			Declaring = Interface;
			Signature = Found;

			break;
		}
	}

	/* A layer of its own is nothing to conform to, and nothing conforms it */
	if (Signature == nullptr) return;

	/* Which pose of the layer each node stands for.
	 *
	 * A node made from the class knows the pose it is (its name) and the layer it is taken into
	 * (its graph), and that is all the class kept: which of the layer's poses it is, is the
	 * editor's own bookkeeping and was never cooked. Left as it comes, that is INDEX_NONE.
	 *
	 * The compiler conforms every layer to the interface it came from, and finds the node standing
	 * for each pose by that index alone. INDEX_NONE matches no pose, so the node it is looking for
	 * is not there and it makes one named after the same parameter, so the layer ends up with
	 * the node twice under the one name. Counted off the signature the way the schema counts it,
	 * the node it is looking for is the node already there. */
	const UEdGraphSchema_K2* Schema = GetDefault<UEdGraphSchema_K2>();

	int32 Which = 0;

	for (TFieldIterator<FProperty> It(Signature); It && (It->PropertyFlags & CPF_Parm); ++It) {
		FProperty* Parameter = *It;

		if (Parameter->HasAnyPropertyFlags(CPF_OutParm) && !Parameter->HasAnyPropertyFlags(CPF_ReferenceParm)) continue;

		FEdGraphPinType Type;

		if (!Schema->ConvertPropertyToPinType(Parameter, Type)) continue;

		if (!UAnimationGraphSchema::IsPosePin(Type)) continue;

		for (UAnimGraphNode_LinkedInputPose* One : Taking) {
			if (One->Node.Name != Parameter->GetFName()) continue;

			One->InputPoseIndex = Which;

			/* Said the way the schema says it, so the node builds what it takes from the layer it
			 * answers for rather than from what it was left holding */
			One->FunctionReference.SetExternalMember(Layer->GetFName(), Declaring);
			One->Modify();
		}

		Which++;
	}
}

void IAnimationBlueprintImporter::SettleCachedPoses() {
	for (const TPair<TWeakObjectPtr<UAnimGraphNode_UseCachedPose>, FString>& One : Borrowed) {
		UAnimGraphNode_UseCachedPose* Borrowing = One.Key.Get();

		if (Borrowing == nullptr) continue;

		/* Looked for in every graph, since the one that saved the pose is not the one borrowing it,
		 * and a state machine keeps its nodes apart from the graph around it */
		UAnimGraphNode_SaveCachedPose* Saved = nullptr;

		for (FUObjectExportContainer* Held : AnimNodeContainers) {
			if (Held == nullptr) continue;

			const FUObjectExport* Export = Held->Find(One.Value);

			if (!Export->IsJsonAndObjectValid()) continue;

			Saved = Cast<UAnimGraphNode_SaveCachedPose>(Export->Object);

			if (Saved != nullptr) break;
		}

		/* And it has to be a node that will still be there to answer.
		 *
		 * The compiler does not ask whether the saver is any good: it takes the node the borrower
		 * is holding and reads its pins outright. One that was made and then cleared away answers
		 * that with the editor. Left alone instead, the borrower has only the name which is what
		 * the engine goes looking by, and it finds the right node itself. */
		if (Saved != nullptr && (!IsValid(Saved) || Saved->Pins.Num() == 0 || Saved->GetGraph() == nullptr)) {
			Saved = nullptr;
		}

		if (Saved == nullptr) {
			FImportIssues::Report(
				EImportIssue::Data,
				TEXT("A borrowed pose that nothing saved"),
				FString::Printf(TEXT("'%s' borrows the cached pose '%s', and there is no node of that name still standing to have saved one."), *GetAssetName(), *One.Value)
			);

			continue;
		}

		Borrowing->SaveCachedPoseNode = Saved;
		Borrowing->Modify();
		Saved->Modify();
	}

	Borrowed.Empty();
}

UEdGraph* IAnimationBlueprintImporter::FindAnimGraph(UAnimBlueprint* AnimBlueprint) {
	for (UEdGraph* Graph : AnimBlueprint->FunctionGraphs) {
		if (Graph && Graph->GetName() == TEXT("AnimGraph")) {
			return Graph;
		}
	}
	
	return nullptr;
}