/* Copyright Reflection Contributors 2024-2026 */

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

	UEdGraph* AnimGraph = FindAnimGraph(AnimBlueprint);
	
	if (AnimGraph) {
		AnimGraph->SubGraphs.Empty();
	}

	CreateGraph(RootGraphAnimProperties, AnimGraph, RootAnimNodeContainer);
	
	return OnAssetCreation(AnimBlueprint);
}

UAnimBlueprint* IAnimationBlueprintImporter::CreateAnimBlueprint(UClass* ParentClass) {
	const EBlueprintType BlueprintType = GetBlueprintType(ParentClass);

	if (UBlueprint* Blueprint = FKismetEditorUtilities::CreateBlueprint(ParentClass, GetPackage(), FName(*GetAssetName()), BlueprintType, UAnimBlueprint::StaticClass(), UAnimBlueprintGeneratedClass::StaticClass())) {
		return Cast<UAnimBlueprint>(CreateAsset(Blueprint));
	}

	return nullptr;
}

void IAnimationBlueprintImporter::CreateGraph(const TSharedPtr<FJsonObject>& AnimNodeProperties, UEdGraph* AnimGraph, FUObjectExportContainer* Container) {
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
	ConnectAnimGraphNodes(Container, AnimGraph);
	AutoLayoutAnimGraphNodes(Container->Exports);

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
	}
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
		
		if (!Class) continue;

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
		
		GetObjectSerializer()->DeserializeObjectProperties(NodeProperties, Node);

		/* Specific needs for certain nodes ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ */
		if (UAnimGraphNode_SaveCachedPose* SaveCachedPose = Cast<UAnimGraphNode_SaveCachedPose>(Node)) {
			SaveCachedPose->CacheName = NodeProperties->GetStringField(TEXT("CachePoseName"));
		}

		if (UAnimGraphNode_UseCachedPose* UseCachedPose = Cast<UAnimGraphNode_UseCachedPose>(Node)) {
			if (NodeProperties->HasField(TEXT("LinkToCachingNode"))) {
				const TSharedPtr<FJsonObject> LinkToCachingNode = NodeProperties->GetObjectField(TEXT("LinkToCachingNode"));
				
				if (LinkToCachingNode->HasField(TEXT("LinkID"))) {
					const FString LinkID = LinkToCachingNode->GetStringField(TEXT("LinkID"));

					/* Specifically use RootAnimNodeContainer, because cached poses won't move with state machines */
					FUObjectExport* SaveCachedPoseExport = RootAnimNodeContainer->Find(LinkID);
					if (!SaveCachedPoseExport->IsJsonAndObjectValid()) continue;

					UAnimGraphNode_SaveCachedPose* SaveCachedPose = Cast<UAnimGraphNode_SaveCachedPose>(SaveCachedPoseExport->Object);
					if (!SaveCachedPose) continue;
					
					UseCachedPose->SaveCachedPoseNode = SaveCachedPose;
					UseCachedPose->Modify();
					SaveCachedPose->Modify();
				}
			}
		}

		HandlePropertyBinding(NodeExport, GetContainer()->JsonObjects, Node, this, AnimBlueprint);

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
                const FString LinkID = Value->AsObject()->GetStringField(TEXT("LinkID"));
                UAnimGraphNode_Base* TargetNode = Cast<UAnimGraphNode_Base>(Container->Find(LinkID)->Object);
                
                if (!TargetNode) {
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

UEdGraph* IAnimationBlueprintImporter::FindAnimGraph(UAnimBlueprint* AnimBlueprint) {
	for (UEdGraph* Graph : AnimBlueprint->FunctionGraphs) {
		if (Graph && Graph->GetName() == TEXT("AnimGraph")) {
			return Graph;
		}
	}
	
	return nullptr;
}