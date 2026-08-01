/* Copyright Reflection Contributors 2024-2026 */

#pragma once

#include "AnimGraphNode_Base.h"
#include "AnimGraphNode_Root.h"
#include "NodeFactory.h"
#include "SGraphNode.h"
#include "Framework/Application/SlateApplication.h"
#include "Math/UnrealMathUtility.h"
#include "Modules/ModuleManager.h"
#include "Serializers/PropertySerializer.h"

namespace AnimNodeLayout {
	/* Gap left between a column's widest node and the column to its right */
	constexpr float ColumnSpacing = 100.f;

	/* Gap between two nodes stacked in the same column */
	constexpr float RowSpacing = 40.f;

	/* Gap between whole trees - the anim graph proper, then one per cached pose */
	constexpr float TreeSpacing = 160.f;

	/* Only reached when a node can't be measured, i.e. no Slate (commandlet, -nullrhi) */
	constexpr float FallbackWidth = 220.f;
	constexpr float FallbackHeaderHeight = 60.f;
	constexpr float FallbackPinHeight = 26.f;
}

/**
 * Anim nodes only get their real widget - SAnimationGraphNode, with the property binding rows and per-node
 * extras that make up a good part of their height - through a visual node factory that
 * FAnimationBlueprintEditorModule::StartupModule registers. That module lives outside this one's dependencies
 * and isn't necessarily loaded during an import: without it FNodeFactory falls through to the generic
 * SGraphNodeK2Default, and every node measures smaller than it will actually draw. Worse, it would depend on
 * whether an animation blueprint had been opened earlier in the session, so the layout would come out
 * differently on a fresh editor than on a warm one.
 */
inline void EnsureAnimGraphNodeFactoryRegistered() {
	static bool bRequested = false;
	if (bRequested) return;

	bRequested = true;
	FModuleManager::Get().LoadModule(TEXT("AnimationBlueprintEditor"));
}

/**
 * Measures a node the way the graph panel itself does.
 *
 * UEdGraphNode::NodeWidth/NodeHeight are only meaningful for user-resizable nodes (comments), so they read 0
 * for anim nodes and any layout built on them is really laying out fixed-size boxes. The true size is whatever
 * the node's Slate widget asks for, and that varies enormously: a Constraint node grows a pin row per bone, so
 * two of them in the same graph can differ by hundreds of units in both axes.
 *
 * SGraphPanel::AddNode gets that size by building the widget and pre-passing it, and notes that this is safe
 * to do out of band because "graph widgets don't rely on any outer layout information for their metrics".
 * SNodePanel::GetBoundsForNode then reads GetDesiredSize() off the widget. That is exactly what happens here,
 * minus the panel - the widget is created, measured, and dropped, so no editor window has to be open.
 */
inline FVector2D MeasureAnimGraphNode(UAnimGraphNode_Base* Node) {
	if (Node && FSlateApplication::IsInitialized()) {
		EnsureAnimGraphNodeFactoryRegistered();

		/* Node widgets are built through the factory rather than by hand so registered per-type factories still
		 * apply - anim nodes come out as SAnimationGraphNode, with the property-binding widgets and dynamic pin
		 * rows that make up most of a Constraint node's height. A freshly constructed widget already reports
		 * bNeedsPrepass, so the prepass below really does measure rather than returning a cached zero. */
		if (const TSharedPtr<SGraphNode> NodeWidget = FNodeFactory::CreateNodeWidget(Node)) {
			NodeWidget->SlatePrepass(1.f);

			const FVector2D DesiredSize = NodeWidget->GetDesiredSize();
			if (DesiredSize.X > 1.f && DesiredSize.Y > 1.f) {
				return DesiredSize;
			}
		}
	}

	/* Nothing to measure against: approximate from the pin count, which is what drives the height of the nodes
	 * that vary the most anyway. */
	int32 VisiblePins = 0;

	if (Node) {
		for (const UEdGraphPin* Pin : Node->Pins) {
			if (Pin && !Pin->bHidden) {
				VisiblePins++;
			}
		}
	}

	return FVector2D(
		AnimNodeLayout::FallbackWidth,
		AnimNodeLayout::FallbackHeaderHeight + VisiblePins * AnimNodeLayout::FallbackPinHeight
	);
}

struct FAnimGraphLayoutContext {
	TMap<UAnimGraphNode_Base*, FVector2D> Sizes;
	TMap<UAnimGraphNode_Base*, TArray<UAnimGraphNode_Base*>> Inputs;
	TMap<UAnimGraphNode_Base*, int32> Depths;

	/* Per column: the left edge it starts at, and the width of the widest node in it */
	TArray<float> ColumnLeft;
	TArray<float> ColumnWidth;

	/* Per column: the first Y that is still free, so two branches can't land on each other */
	TMap<int32, float> ColumnNextY;

	/* Vertical center of every node already placed. Doubles as the visited set - a node feeding two consumers
	 * is positioned once and the second consumer just reads where it ended up. */
	TMap<UAnimGraphNode_Base*, float> PlacedCenterY;

	/* Running Y for nodes with no inputs. Everything else centers on its children, so this cursor is what
	 * actually decides how the graph is spaced vertically. */
	float NextLeafY = 0.f;
};

/**
 * Column index for each node: the longest path back from a sink.
 *
 * Longest rather than shortest matters when a node feeds two consumers that are themselves different distances
 * from the output - taking the longest keeps it left of both, so a wire never has to run backwards.
 */
inline void AssignAnimNodeDepth(UAnimGraphNode_Base* Node, const int32 Depth, FAnimGraphLayoutContext& Context, TSet<UAnimGraphNode_Base*>& Stack) {
	if (const int32* Existing = Context.Depths.Find(Node)) {
		if (*Existing >= Depth) return;
	}

	Context.Depths.Add(Node, Depth);

	/* Anim graphs shouldn't contain cycles, but a malformed import could still produce one */
	if (Stack.Contains(Node)) return;
	Stack.Add(Node);

	if (const TArray<UAnimGraphNode_Base*>* Children = Context.Inputs.Find(Node)) {
		for (UAnimGraphNode_Base* Child : *Children) {
			AssignAnimNodeDepth(Child, Depth + 1, Context, Stack);
		}
	}

	Stack.Remove(Node);
}

/** Positions a node and everything feeding it, returning the node's vertical center. */
inline float PlaceAnimGraphNode(UAnimGraphNode_Base* Node, FAnimGraphLayoutContext& Context) {
	if (const float* Existing = Context.PlacedCenterY.Find(Node)) {
		return *Existing;
	}

	const int32 Depth = Context.Depths.FindChecked(Node);
	const FVector2D Size = Context.Sizes.FindChecked(Node);

	/* Claim the node before recursing so a cycle terminates instead of overflowing the stack */
	Context.PlacedCenterY.Add(Node, 0.f);

	float FirstChildCenter = 0.f;
	float LastChildCenter = 0.f;
	int32 ChildCount = 0;

	if (const TArray<UAnimGraphNode_Base*>* Children = Context.Inputs.Find(Node)) {
		for (UAnimGraphNode_Base* Child : *Children) {
			const float ChildCenter = PlaceAnimGraphNode(Child, Context);

			if (ChildCount == 0) FirstChildCenter = ChildCenter;
			LastChildCenter = ChildCenter;

			ChildCount++;
		}
	}

	float Top;

	if (ChildCount > 0) {
		/* Centered on the span its inputs occupy. This is what lines a node up with what feeds it - the output
		 * pose sitting level with the Component To Local in front of it, rather than at whatever Y the walk
		 * happened to reach. */
		Top = (FirstChildCenter + LastChildCenter) * 0.5f - Size.Y * 0.5f;
	} else {
		Top = Context.NextLeafY;
		Context.NextLeafY += Size.Y + AnimNodeLayout::RowSpacing;
	}

	/* Centering ignores what else is in the column, so push down past anything already there */
	float& NextY = Context.ColumnNextY.FindOrAdd(Depth);
	Top = FMath::Max(Top, NextY);
	NextY = Top + Size.Y + AnimNodeLayout::RowSpacing;

	/* Right-aligned within the column, so output pins share an edge and the wires into the next column stay
	 * short and level regardless of how much wider one node is than its neighbours. */
	const float X = Context.ColumnLeft[Depth] + (Context.ColumnWidth[Depth] - Size.X);

	Node->NodePosX = FMath::RoundToInt(X);
	Node->NodePosY = FMath::RoundToInt(Top);

	const float Center = Top + Size.Y * 0.5f;
	Context.PlacedCenterY.Add(Node, Center);

	return Center;
}

inline void AutoLayoutAnimGraphNodes(const TArray<FUObjectExport*>& NodeExports) {
	FAnimGraphLayoutContext Context;

	TArray<UAnimGraphNode_Base*> Nodes;
	TArray<UAnimGraphNode_Base*> Roots;

	for (const FUObjectExport* Export : NodeExports) {
		UAnimGraphNode_Base* Node = Cast<UAnimGraphNode_Base>(Export->Object);
		if (!Node) continue;

		Nodes.Add(Node);
		Context.Sizes.Add(Node, MeasureAnimGraphNode(Node));

		TArray<UAnimGraphNode_Base*>& Inputs = Context.Inputs.Add(Node);
		bool bHasOutputs = false;

		for (UEdGraphPin* Pin : Node->Pins) {
			if (!Pin) continue;

			if (Pin->Direction == EGPD_Input) {
				for (const UEdGraphPin* LinkedPin : Pin->LinkedTo) {
					if (!LinkedPin) continue;

					if (UAnimGraphNode_Base* LinkedNode = Cast<UAnimGraphNode_Base>(LinkedPin->GetOwningNode())) {
						Inputs.AddUnique(LinkedNode);
					}
				}
			} else if (Pin->LinkedTo.Num() > 0) {
				bHasOutputs = true;
			}
		}

		/* Nothing consumes this node, so it anchors a tree of its own. That's the output pose, plus one per
		 * cached pose - a Save Cached Pose is reached by name from Use Cached Pose, never by a wire. */
		if (!bHasOutputs) {
			Roots.Add(Node);
		}
	}

	if (Roots.Num() == 0) return;

	/* Output pose first so the graph proper is the top tree and the cached poses stack underneath it */
	Roots.Sort([](const UAnimGraphNode_Base& A, const UAnimGraphNode_Base& B) {
		return A.IsA<UAnimGraphNode_Root>() && !B.IsA<UAnimGraphNode_Root>();
	});

	TSet<UAnimGraphNode_Base*> Stack;
	for (UAnimGraphNode_Base* Root : Roots) {
		AssignAnimNodeDepth(Root, 0, Context, Stack);
	}

	/* Anything unreachable from a root (an island left by a broken link) still needs a column */
	for (UAnimGraphNode_Base* Node : Nodes) {
		if (!Context.Depths.Contains(Node)) {
			AssignAnimNodeDepth(Node, 0, Context, Stack);
			Roots.Add(Node);
		}
	}

	int32 MaxDepth = 0;
	for (const TPair<UAnimGraphNode_Base*, int32>& Pair : Context.Depths) {
		MaxDepth = FMath::Max(MaxDepth, Pair.Value);
	}

	Context.ColumnWidth.SetNumZeroed(MaxDepth + 1);
	for (const TPair<UAnimGraphNode_Base*, int32>& Pair : Context.Depths) {
		const float Width = Context.Sizes.FindChecked(Pair.Key).X;
		Context.ColumnWidth[Pair.Value] = FMath::Max(Context.ColumnWidth[Pair.Value], Width);
	}

	/* The graph flows right, into the output pose at depth 0, so each deeper column steps left by its own
	 * widest node plus the gap. Stepping by the measured width is what stops a column of Constraint nodes from
	 * running over the top of whatever sits to its right. */
	Context.ColumnLeft.SetNumZeroed(MaxDepth + 1);
	for (int32 Depth = 1; Depth <= MaxDepth; Depth++) {
		Context.ColumnLeft[Depth] = Context.ColumnLeft[Depth - 1] - AnimNodeLayout::ColumnSpacing - Context.ColumnWidth[Depth];
	}

	for (UAnimGraphNode_Base* Root : Roots) {
		PlaceAnimGraphNode(Root, Context);

		/* Drop every column cursor to the bottom of what's been placed, so the next tree starts clear of it
		 * instead of interleaving with it */
		float Bottom = Context.NextLeafY;
		for (const TPair<int32, float>& Pair : Context.ColumnNextY) {
			Bottom = FMath::Max(Bottom, Pair.Value);
		}

		Bottom += AnimNodeLayout::TreeSpacing;

		Context.NextLeafY = Bottom;
		for (TPair<int32, float>& Pair : Context.ColumnNextY) {
			Pair.Value = Bottom;
		}
	}
}
