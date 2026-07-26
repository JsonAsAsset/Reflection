/* Copyright Reflection Contributors 2024-2026 */

#pragma once

#include "AnimGraphNode_Base.h"
#include "AnimGraphNode_SaveCachedPose.h"
#include "AnimGraphNode_BlendListByEnum.h"
#include "Serializers/PropertySerializer.h"
#include "Math/UnrealMathUtility.h"

inline void AutoLayoutAnimGraphRecursive(
	UAnimGraphNode_Base* Node,
	TSet<UAnimGraphNode_Base*>& Visited,
	TMap<UAnimGraphNode_Base*, FVector2D>& Positions,
	const float X,
	float& Y,
	float& CachedPoseY,
	float CachedPoseXOffset
) {
	if (!Node || Visited.Contains(Node)) return;
	Visited.Add(Node);

	/* Constants */
	constexpr float CachedPoseXSpacing = 0.f;
	constexpr float XSpacing = 150.f;
	constexpr float YSpacing = 150.f;

	/* Use the node's size if available */
	const float BaseNodeWidth = (Node->NodeWidth > 0.f ? Node->NodeWidth : 100.f);
	float NodeHeight = (Node->NodeHeight > 0.f ? Node->NodeHeight : 150.f);

	/* Adjust height for BlendListByEnum nodes by averaging over input count */
	TArray<UAnimGraphNode_Base*> TempInputNodes;
	for (UEdGraphPin* Pin : Node->Pins) {
		if (Pin && Pin->Direction == EGPD_Input) {
			for (UEdGraphPin* LinkedPin : Pin->LinkedTo) {
				if (LinkedPin) {
					if (UAnimGraphNode_Base* LinkedNode = Cast<UAnimGraphNode_Base>(LinkedPin->GetOwningNode())) {
						TempInputNodes.AddUnique(LinkedNode);
					}
				}
			}
		}
	}
	if (Node->IsA<UAnimGraphNode_BlendListByEnum>()) {
		float DefaultHeight = (Node->NodeHeight > 0.f ? Node->NodeHeight : 150.f);
		if (TempInputNodes.Num() > 0) {
			NodeHeight = FMath::Max(DefaultHeight / static_cast<float>(TempInputNodes.Num()), 50.f);
		}
	}

	const bool bIsCachedPose = Node->IsA<UAnimGraphNode_SaveCachedPose>();

	/* For cached pose nodes, update Y using the cached layout and add extra spacing */
	if (bIsCachedPose) {
		Y = CachedPoseY;
		CachedPoseY = Y + NodeHeight + YSpacing + 150.f;
		CachedPoseXOffset += CachedPoseXSpacing;
	}

	bool bIsBlendListByBool = Node->GetClass()->GetName().Contains(TEXT("BlendListByBool"));

	/* Collect input nodes */
	TArray<UAnimGraphNode_Base*> InputNodes;
	for (UEdGraphPin* Pin : Node->Pins) {
		if (Pin && Pin->Direction == EGPD_Input) {
			for (UEdGraphPin* LinkedPin : Pin->LinkedTo) {
				if (LinkedPin) {
					if (UAnimGraphNode_Base* LinkedNode = Cast<UAnimGraphNode_Base>(LinkedPin->GetOwningNode())) {
						InputNodes.AddUnique(LinkedNode);
					}
				}
			}
		}
	}

	/* Layout children */
	float StartY = Y;
	for (int32 i = 0; i < InputNodes.Num(); ++i) {
		UAnimGraphNode_Base* Input = InputNodes[i];
		float LocalY = StartY + i * (NodeHeight + YSpacing);
		AutoLayoutAnimGraphRecursive(
			Input,
			Visited,
			Positions,
			X - BaseNodeWidth - XSpacing,
			LocalY,
			CachedPoseY,
			CachedPoseXOffset
		);
	}

	/* Apply cached pose X offset */
	const float FinalX = X + CachedPoseXOffset;

	Node->NodePosX = static_cast<int32>(FinalX);
	Node->NodePosY = static_cast<int32>(Y);
	Positions.Add(Node, FVector2D(FinalX, Y));

	/* For BlendListByBool nodes, ensure a minimum vertical offset to avoid overlapping */
	if (bIsBlendListByBool) {
		Y += FMath::Max(NodeHeight + YSpacing, 150.f);
	}

	if (!bIsCachedPose) {
		Y += NodeHeight + YSpacing;
	}
}

inline void AutoLayoutAnimGraphNodes(const TArray<FUObjectExport*>& NodeExports) {
	TMap<FName, UAnimGraphNode_Base*> NameToNode;
	TSet<UAnimGraphNode_Base*> RootNodes;

	for (const FUObjectExport* Export : NodeExports) {
		if (UAnimGraphNode_Base* Node = Cast<UAnimGraphNode_Base>(Export->Object)) {
			NameToNode.Add(Export->GetName(), Node);

			/* Node with no outgoing links = root-level / sink node */
			bool bHasOutputs = false;
			for (UEdGraphPin* Pin : Node->Pins) {
				if (Pin && Pin->Direction == EGPD_Output && Pin->LinkedTo.Num() > 0) {
					bHasOutputs = true;
					break;
				}
			}
			if (!bHasOutputs) {
				RootNodes.Add(Node);
			}
		}
	}

	/* Start layout from the right edge */
	TSet<UAnimGraphNode_Base*> Visited;
	TMap<UAnimGraphNode_Base*, FVector2D> Positions;

	float InitialX = 0.f;
	float InitialY = 0.f;
	float CachedPoseY = 500.f;

	for (UAnimGraphNode_Base* Root : RootNodes) {
		float Y = InitialY;
		float CachedPoseXOffset = 0.f;
		AutoLayoutAnimGraphRecursive(
			Root,
			Visited,
			Positions,
			InitialX,
			Y,
			CachedPoseY,
			CachedPoseXOffset
		);
	}
}
