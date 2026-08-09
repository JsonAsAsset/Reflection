/* Copyright Reflection Contributors 2024-2026 */

#pragma once

#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "NodeFactory.h"
#include "SGraphNode.h"
#include "Framework/Application/SlateApplication.h"

namespace SoundNodeLayout {
	constexpr float ColumnSpacing = 80.0f;
	constexpr float RowSpacing = 40.0f;
	constexpr float TreeSpacing = 160.0f;

	/* Only reached with no Slate to measure against (commandlet, -nullrhi) */
	constexpr float FallbackWidth = 200.0f;
	constexpr float FallbackHeight = 90.0f;
}

struct FSoundNodeLayoutContext {
	TMap<UEdGraphNode*, FVector2D> Sizes;
	TMap<UEdGraphNode*, TArray<UEdGraphNode*>> Inputs;

	/* Longest path back from the root, so a node shared by two parents stays left of both */
	TMap<UEdGraphNode*, int32> Depths;

	TArray<float> ColumnWidth;
	TArray<float> ColumnLeft;
	TArray<float> ColumnNextY;

	/* Doubles as the visited set */
	TMap<UEdGraphNode*, float> PlacedTop;

	float NextLeafY = 0.0f;
};

/* NodeWidth/NodeHeight only mean anything on nodes the user can resize, so this measures the widget */
inline FVector2D MeasureSoundGraphNode(UEdGraphNode* Node) {
	if (Node != nullptr && FSlateApplication::IsInitialized()) {
		if (const TSharedPtr<SGraphNode> NodeWidget = FNodeFactory::CreateNodeWidget(Node)) {
			NodeWidget->SlatePrepass(1.0f);

			const FVector2D DesiredSize = NodeWidget->GetDesiredSize();

			if (DesiredSize.X > 1.0f && DesiredSize.Y > 1.0f) {
				return DesiredSize;
			}
		}
	}

	return FVector2D(SoundNodeLayout::FallbackWidth, SoundNodeLayout::FallbackHeight);
}

inline bool IsSoundGraphRoot(UEdGraphNode* Node) {
	for (const UEdGraphPin* Pin : Node->Pins) {
		if (Pin != nullptr && Pin->Direction == EGPD_Output && Pin->LinkedTo.Num() > 0) {
			return false;
		}
	}

	return true;
}

inline void CollectSoundNodeTree(UEdGraphNode* Node, const int32 Depth, FSoundNodeLayoutContext& Context, TArray<UEdGraphNode*>& OutTree, TSet<UEdGraphNode*>& Stack) {
	if (const int32* Existing = Context.Depths.Find(Node)) {
		if (*Existing >= Depth) return;

		Context.Depths.Add(Node, Depth);
	} else {
		Context.Depths.Add(Node, Depth);
		OutTree.Add(Node);
	}

	if (Stack.Contains(Node)) return;
	Stack.Add(Node);

	if (const TArray<UEdGraphNode*>* Children = Context.Inputs.Find(Node)) {
		for (UEdGraphNode* Child : *Children) {
			CollectSoundNodeTree(Child, Depth + 1, Context, OutTree, Stack);
		}
	}

	Stack.Remove(Node);
}

/* Centres a node on the children it is fed by, placing those first */
inline float PlaceSoundNode(UEdGraphNode* Node, FSoundNodeLayoutContext& Context, TSet<UEdGraphNode*>& Visiting) {
	if (const float* Placed = Context.PlacedTop.Find(Node)) {
		return *Placed;
	}

	if (Visiting.Contains(Node)) {
		return Context.NextLeafY;
	}

	Visiting.Add(Node);

	const int32 Depth = Context.Depths.FindChecked(Node);
	const FVector2D Size = Context.Sizes.FindChecked(Node);

	const TArray<UEdGraphNode*>* Children = Context.Inputs.Find(Node);

	float Top;

	if (Children == nullptr || Children->Num() == 0) {
		Top = Context.NextLeafY;
	} else {
		float FirstCenter = 0.0f;
		float LastCenter = 0.0f;
		bool bHasChild = false;

		for (UEdGraphNode* Child : *Children) {
			if (!Context.Depths.Contains(Child)) continue;

			const float ChildCenter = PlaceSoundNode(Child, Context, Visiting) + Context.Sizes.FindChecked(Child).Y * 0.5f;

			if (!bHasChild) {
				FirstCenter = ChildCenter;
				bHasChild = true;
			}

			LastCenter = ChildCenter;
		}

		Top = bHasChild ? (FirstCenter + LastCenter) * 0.5f - Size.Y * 0.5f : Context.NextLeafY;
	}

	Top = FMath::Max(Top, Context.ColumnNextY[Depth]);

	Context.ColumnNextY[Depth] = Top + Size.Y + SoundNodeLayout::RowSpacing;
	Context.NextLeafY = FMath::Max(Context.NextLeafY, Context.ColumnNextY[Depth]);

	Context.PlacedTop.Add(Node, Top);

	/* Right edges line up with the column, so an output sits as close to its consumer as it can */
	Node->NodePosX = FMath::RoundToInt(Context.ColumnLeft[Depth] + Context.ColumnWidth[Depth] - Size.X);
	Node->NodePosY = FMath::RoundToInt(Top);

	Visiting.Remove(Node);

	return Top;
}

/* Cooked cues carry no node positions, so everything imports stacked at the origin */
inline void OrganizeSoundCueGraph(UEdGraph* Graph) {
	if (Graph == nullptr || Graph->Nodes.Num() == 0) {
		return;
	}

	FSoundNodeLayoutContext Context;

	for (UEdGraphNode* Node : Graph->Nodes) {
		if (Node == nullptr) continue;

		Context.Sizes.Add(Node, MeasureSoundGraphNode(Node));

		TArray<UEdGraphNode*> Children;

		for (const UEdGraphPin* Pin : Node->Pins) {
			if (Pin == nullptr || Pin->Direction != EGPD_Input) continue;

			for (const UEdGraphPin* LinkedPin : Pin->LinkedTo) {
				if (LinkedPin == nullptr) continue;

				if (UEdGraphNode* Linked = LinkedPin->GetOwningNode()) {
					Children.AddUnique(Linked);
				}
			}
		}

		Context.Inputs.Add(Node, Children);
	}

	TArray<UEdGraphNode*> Roots;

	for (UEdGraphNode* Node : Graph->Nodes) {
		if (Node != nullptr && IsSoundGraphRoot(Node)) {
			Roots.Add(Node);
		}
	}

	/* A cycle can leave every node with a consumer, in which case graph order decides */
	if (Roots.Num() == 0) {
		Roots.Add(Graph->Nodes[0]);
	}

	for (UEdGraphNode* Root : Roots) {
		if (Context.PlacedTop.Contains(Root)) continue;

		Context.Depths.Reset();
		Context.ColumnWidth.Reset();
		Context.ColumnLeft.Reset();
		Context.ColumnNextY.Reset();

		TArray<UEdGraphNode*> Tree;
		TSet<UEdGraphNode*> Stack;

		CollectSoundNodeTree(Root, 0, Context, Tree, Stack);

		Tree.RemoveAll([&Context](UEdGraphNode* Node) {
			return Context.PlacedTop.Contains(Node);
		});

		if (Tree.Num() == 0) continue;

		for (UEdGraphNode* Node : Tree) {
			const int32 Depth = Context.Depths.FindChecked(Node);

			while (Context.ColumnWidth.Num() <= Depth) {
				Context.ColumnWidth.Add(0.0f);
				Context.ColumnLeft.Add(0.0f);
				Context.ColumnNextY.Add(Context.NextLeafY);
			}

			Context.ColumnWidth[Depth] = FMath::Max(Context.ColumnWidth[Depth], Context.Sizes.FindChecked(Node).X);
		}

		for (int32 Depth = 0; Depth < Context.ColumnLeft.Num(); Depth++) {
			Context.ColumnLeft[Depth] = Depth == 0
				? -Context.ColumnWidth[0]
				: Context.ColumnLeft[Depth - 1] - SoundNodeLayout::ColumnSpacing - Context.ColumnWidth[Depth];
		}

		TSet<UEdGraphNode*> Visiting;
		PlaceSoundNode(Root, Context, Visiting);

		for (UEdGraphNode* Node : Tree) {
			PlaceSoundNode(Node, Context, Visiting);
		}

		Context.NextLeafY += SoundNodeLayout::TreeSpacing;
	}
}
