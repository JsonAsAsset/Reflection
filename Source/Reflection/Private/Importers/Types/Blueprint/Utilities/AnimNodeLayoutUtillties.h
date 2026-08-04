/* Copyright Reflection Contributors 2024-2026 */

#pragma once

#include "AnimGraphNode_Base.h"
#include "AnimGraphNode_Root.h"
#include "NodeFactory.h"
#include "SGraphNode.h"
#include "SGraphPin.h"
#include "Framework/Application/SlateApplication.h"
#include "Layout/ArrangedChildren.h"
#include "Math/NumericLimits.h"
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

/** Anim nodes only get their real widget - SAnimationGraphNode, with the property binding rows and per-node
 * extras that make up a good part of their height - through a visual node factory that
 * FAnimationBlueprintEditorModule::StartupModule registers. That module lives outside this one's dependencies
 * and isn't necessarily loaded during an import: without it FNodeFactory falls through to the generic
 * SGraphNodeK2Default, and every node measures smaller than it will actually draw. Worse, it would depend on
 * whether an animation blueprint had been opened earlier in the session, so the layout would come out
 * differently on a fresh editor than on a warm one. */
inline void EnsureAnimGraphNodeFactoryRegistered() {
	static bool bRequested = false;
	if (bRequested) return;

	bRequested = true;
	FModuleManager::Get().LoadModule(TEXT("AnimationBlueprintEditor"));
}

/** A node's footprint, plus where each of its pins sits inside it. */
struct FAnimNodeMetrics {
	FVector2D Size = FVector2D::ZeroVector;

	/* How far the node draws above its own top edge. A comment bubble - which is what the node ID comments
	 * setting puts on every node - hangs off the top of the node and isn't part of its size, so a row spaced
	 * only by that size has the bubble sitting on top of whatever is above it. */
	float TopOverhang = 0.f;

	/* Vertical center of each pin, measured down from the top of the node */
	TMap<const UEdGraphPin*, float> PinCenterY;
};

/** Walks the widget tree the way Slate itself would, noting where the pin widgets land.
 *
 * A wire doesn't attach to a node, it attaches to a pin: FConnectionDrawingPolicy draws from the vertical
 * middle of the source pin's geometry to the vertical middle of the target's. Pins sit below the title, so how
 * far down the node one starts depends on how tall that node's header drew - a Look At carrying a bone name
 * under its title pushes its pins further down than a Component To Local does. Lining nodes up by their centers,
 * or by their tops, therefore still leaves the wire sloping; only the pins themselves can be lined up.
 *
 * Slate hands out those positions through ArrangeChildren, which is pure layout and needs nothing but a
 * geometry to arrange into - no panel, no window. Arranging the node into its own desired size reproduces
 * exactly what SGraphPanel would do with it, since the panel sizes a node to its desired size and places it at
 * NodePosX/NodePosY. */
inline void MeasureAnimGraphPinCenters(const TSharedRef<SWidget>& Widget, const FGeometry& Geometry, const TMap<const SWidget*, UEdGraphPin*>& PinWidgets, TMap<const UEdGraphPin*, float>& OutPinCenterY) {
	if (OutPinCenterY.Num() == PinWidgets.Num()) return;

	if (UEdGraphPin* const* Pin = PinWidgets.Find(&Widget.Get())) {
		/* Arranged at a scale of one, so local and absolute are the same space */
		OutPinCenterY.Add(*Pin, Geometry.GetAbsolutePosition().Y + Geometry.GetLocalSize().Y * 0.5f);

		/* Pins don't nest */
		return;
	}

	FArrangedChildren Children(EVisibility::All);
	Widget->ArrangeChildren(Geometry, Children);

	for (int32 Index = 0; Index < Children.Num(); Index++) {
		MeasureAnimGraphPinCenters(Children[Index].Widget, Children[Index].Geometry, PinWidgets, OutPinCenterY);
	}
}

/** Measures a node the way the graph panel itself does.
 *
 * UEdGraphNode::NodeWidth/NodeHeight are only meaningful for user-resizable nodes (comments), so they read 0
 * for anim nodes and any layout built on them is really laying out fixed-size boxes. The true size is whatever
 * the node's Slate widget asks for, and that varies enormously: a Constraint node grows a pin row per bone, so
 * two of them in the same graph can differ by hundreds of units in both axes.
 *
 * SGraphPanel::AddNode gets that size by building the widget and pre-passing it, and notes that this is safe
 * to do out of band because "graph widgets don't rely on any outer layout information for their metrics".
 * SNodePanel::GetBoundsForNode then reads GetDesiredSize() off the widget. That is exactly what happens here,
 * minus the panel - the widget is created, measured, and dropped, so no editor window has to be open. */
inline FAnimNodeMetrics MeasureAnimGraphNode(UAnimGraphNode_Base* Node) {
	FAnimNodeMetrics Metrics;
	if (!Node) return Metrics;

	if (FSlateApplication::IsInitialized()) {
		EnsureAnimGraphNodeFactoryRegistered();

		/* Node widgets are built through the factory rather than by hand so registered per-type factories still
		 * apply - anim nodes come out as SAnimationGraphNode, with the property-binding widgets and dynamic pin
		 * rows that make up most of a Constraint node's height. A freshly constructed widget already reports
		 * bNeedsPrepass, so the prepass below really does measure rather than returning a cached zero. */
		if (const TSharedPtr<SGraphNode> NodeWidget = FNodeFactory::CreateNodeWidget(Node)) {
			NodeWidget->SlatePrepass(1.f);

			const FVector2D DesiredSize = NodeWidget->GetDesiredSize();
			if (DesiredSize.X > 1.f && DesiredSize.Y > 1.f) {
				Metrics.Size = DesiredSize;

				const FGeometry NodeGeometry = FGeometry::MakeRoot(DesiredSize, FSlateLayoutTransform());

				/* SNodePanel::SNode sizes itself to its center zone alone, then arranges the rest around it at
				 * their own offsets - negative for anything meant to sit above the node. Reading those back is
				 * what turns "a comment bubble is roughly this tall" into the height this node actually drew. */
				FArrangedChildren Zones(EVisibility::All);
				NodeWidget->ArrangeChildren(NodeGeometry, Zones);

				for (int32 Index = 0; Index < Zones.Num(); Index++) {
					Metrics.TopOverhang = FMath::Max(Metrics.TopOverhang, -Zones[Index].Geometry.GetAbsolutePosition().Y);
				}

				TMap<const SWidget*, UEdGraphPin*> PinWidgets;
				for (UEdGraphPin* Pin : Node->Pins) {
					if (!Pin) continue;

					/* Null for a pin the node chose not to draw */
					if (const TSharedPtr<SGraphPin> PinWidget = NodeWidget->FindWidgetForPin(Pin)) {
						PinWidgets.Add(PinWidget.Get(), Pin);
					}
				}

				if (PinWidgets.Num() > 0) {
					MeasureAnimGraphPinCenters(NodeWidget.ToSharedRef(), NodeGeometry, PinWidgets, Metrics.PinCenterY);
				}

				return Metrics;
			}
		}
	}

	/* Nothing to measure against: approximate from the pin count, which is what drives the height of the nodes
	 * that vary the most anyway. Inputs and outputs run down opposite sides of the node, so they each start
	 * their own row under the header. */
	int32 InputRows = 0;
	int32 OutputRows = 0;

	for (const UEdGraphPin* Pin : Node->Pins) {
		if (!Pin || Pin->bHidden) continue;

		int32& Row = Pin->Direction == EGPD_Input ? InputRows : OutputRows;
		Metrics.PinCenterY.Add(Pin, AnimNodeLayout::FallbackHeaderHeight + (Row + 0.5f) * AnimNodeLayout::FallbackPinHeight);

		Row++;
	}

	Metrics.Size = FVector2D(
		AnimNodeLayout::FallbackWidth,
		AnimNodeLayout::FallbackHeaderHeight + FMath::Max(InputRows, OutputRows) * AnimNodeLayout::FallbackPinHeight
	);

	return Metrics;
}

struct FAnimGraphLayoutContext {
	TMap<UAnimGraphNode_Base*, FAnimNodeMetrics> Metrics;
	TMap<UAnimGraphNode_Base*, TArray<UAnimGraphNode_Base*>> Inputs;

	/* Everything below is scoped to the tree being laid out and reset between trees */

	TMap<UAnimGraphNode_Base*, int32> Depths;

	/* Per column: the left edge it starts at, and the width of the widest node in it */
	TArray<float> ColumnLeft;
	TArray<float> ColumnWidth;

	/* Per column: the first Y that is still free, so two branches can't land on each other. Starts out
	 * unbounded rather than at zero - a node taller than what feeds it has to reach above the top of the graph
	 * to line its pins up, and clamping the first node in a column to zero is what flushes a row of nodes to a
	 * shared top edge and bends every wire coming into the tall one. */
	TArray<float> ColumnNextY;

	/* Top edge of every node already placed. Doubles as the visited set - a node feeding two consumers is
	 * positioned once and the second consumer just reads where it ended up. */
	TMap<UAnimGraphNode_Base*, float> PlacedTop;

	/* Running Y for nodes with no inputs. Everything else hangs off its children, so this cursor is what
	 * actually decides how the graph is spaced vertically. */
	float NextLeafY = 0.f;
};

/** Vertical center of a pin, measured down from the top of the node it belongs to. */
inline float GetAnimPinCenterY(const FAnimGraphLayoutContext& Context, UAnimGraphNode_Base* Node, const UEdGraphPin* Pin) {
	const FAnimNodeMetrics& Metrics = Context.Metrics.FindChecked(Node);

	if (const float* CenterY = Metrics.PinCenterY.Find(Pin)) {
		return *CenterY;
	}

	return Metrics.Size.Y * 0.5f;
}

/** Claims one tree's worth of nodes and gives each a column: the longest path back from the root.
 *
 * Longest rather than shortest matters when a node feeds two consumers that are themselves different distances
 * from the output - taking the longest keeps it left of both, so a wire never has to run backwards. */
inline void CollectAnimNodeTree(UAnimGraphNode_Base* Node, const int32 Depth, FAnimGraphLayoutContext& Context, TSet<UAnimGraphNode_Base*>& Claimed, TArray<UAnimGraphNode_Base*>& OutTree, TSet<UAnimGraphNode_Base*>& Stack) {
	if (const int32* Existing = Context.Depths.Find(Node)) {
		if (*Existing >= Depth) return;

		Context.Depths.Add(Node, Depth);
	} else {
		/* Reached out of an earlier tree, which has already placed it. Pulling it into this one would drag it
		 * out from under the wires that found it there. */
		if (Claimed.Contains(Node)) return;

		Claimed.Add(Node);
		OutTree.Add(Node);
		Context.Depths.Add(Node, Depth);
	}

	/* Anim graphs shouldn't contain cycles, but a malformed import could still produce one */
	if (Stack.Contains(Node)) return;
	Stack.Add(Node);

	if (const TArray<UAnimGraphNode_Base*>* Children = Context.Inputs.Find(Node)) {
		for (UAnimGraphNode_Base* Child : *Children) {
			CollectAnimNodeTree(Child, Depth + 1, Context, Claimed, OutTree, Stack);
		}
	}

	Stack.Remove(Node);
}

/**
 * Moves a node and everything feeding it down by the same amount.
 *
 * A node is placed level with what feeds it, and then shoved down if something is already sitting in its
 * column. On its own that shove only moves the one node: the branch behind it stays where the alignment put
 * it, so the wire into it bends and the run of nodes leading up to it reads as floating above the rest of its
 * row. Taking the branch along keeps every wire in it exactly as straight as it was laid out and moves the
 * whole thing into the space the shove was aiming for.
 */
inline void ShiftAnimNodeSubtree(UAnimGraphNode_Base* Node, const float Delta, FAnimGraphLayoutContext& Context, TSet<UAnimGraphNode_Base*>& Shifted) {
	if (Shifted.Contains(Node)) return;

	float* Top = Context.PlacedTop.Find(Node);
	if (!Top) return;

	int32 PlacedConsumers = 0;
	bool bHasInputs = false;

	for (UEdGraphPin* Pin : Node->Pins) {
		if (!Pin) continue;

		for (UEdGraphPin* LinkedPin : Pin->LinkedTo) {
			if (!LinkedPin) continue;

			UAnimGraphNode_Base* Linked = Cast<UAnimGraphNode_Base>(LinkedPin->GetOwningNode());
			if (!Linked || !Context.PlacedTop.Contains(Linked)) continue;

			if (Pin->Direction == EGPD_Input) {
				bHasInputs = true;
			} else {
				PlacedConsumers++;
			}
		}
	}

	/* A pose feeding two consumers is anchored by whichever one placed it. Moving it now would only trade this
	 * bend for one on the other consumer's wire. */
	if (PlacedConsumers > 1) return;

	Shifted.Add(Node);

	*Top += Delta;
	Node->NodePosY = FMath::RoundToInt(*Top);

	const float Bottom = *Top + Context.Metrics.FindChecked(Node).Size.Y + AnimNodeLayout::RowSpacing;

	/* The cursors were left pointing under where this node used to be */
	float& NextY = Context.ColumnNextY[Context.Depths.FindChecked(Node)];
	NextY = FMath::Max(NextY, Bottom);

	if (!bHasInputs) {
		Context.NextLeafY = FMath::Max(Context.NextLeafY, Bottom);
		return;
	}

	for (UEdGraphPin* Pin : Node->Pins) {
		if (!Pin || Pin->Direction != EGPD_Input) continue;

		for (UEdGraphPin* LinkedPin : Pin->LinkedTo) {
			if (!LinkedPin) continue;

			if (UAnimGraphNode_Base* Child = Cast<UAnimGraphNode_Base>(LinkedPin->GetOwningNode())) {
				ShiftAnimNodeSubtree(Child, Delta, Context, Shifted);
			}
		}
	}
}

/** Positions a node and everything feeding it, returning the node's top edge. */
inline float PlaceAnimGraphNode(UAnimGraphNode_Base* Node, FAnimGraphLayoutContext& Context) {
	if (const float* Existing = Context.PlacedTop.Find(Node)) {
		return *Existing;
	}

	const int32 Depth = Context.Depths.FindChecked(Node);

	const FAnimNodeMetrics& Metrics = Context.Metrics.FindChecked(Node);
	const FVector2D Size = Metrics.Size;
	const float Overhang = Metrics.TopOverhang;

	/* Claim the node before recursing so a cycle terminates instead of overflowing the stack */
	Context.PlacedTop.Add(Node, 0.f);

	float FirstTop = 0.f;
	float LastTop = 0.f;
	int32 LinkCount = 0;

	/* Pins are drawn in declaration order, so walking them in order visits the incoming wires top to bottom */
	for (UEdGraphPin* Pin : Node->Pins) {
		if (!Pin || Pin->Direction != EGPD_Input) continue;

		for (UEdGraphPin* LinkedPin : Pin->LinkedTo) {
			if (!LinkedPin) continue;

			UAnimGraphNode_Base* Child = Cast<UAnimGraphNode_Base>(LinkedPin->GetOwningNode());

			/* Belongs to another tree, so it isn't this layout's to move */
			if (!Child || !Context.Depths.Contains(Child)) continue;

			const float ChildTop = PlaceAnimGraphNode(Child, Context);

			/* Where this node would have to start for the wire to come in level */
			const float AlignedTop = ChildTop + GetAnimPinCenterY(Context, Child, LinkedPin) - GetAnimPinCenterY(Context, Node, Pin);

			if (LinkCount == 0) FirstTop = AlignedTop;
			LastTop = AlignedTop;

			LinkCount++;
		}
	}

	float Top;

	if (LinkCount > 0) {
		/* One input is the common case by far and lands the wire dead straight. More than one can't all be
		 * straight at once, so the node sits halfway between what its first and last inputs each asked for,
		 * which spreads the slope evenly across them. */
		Top = (FirstTop + LastTop) * 0.5f;
	} else {
		Top = Context.NextLeafY + Overhang;
	}

	/* Following the wire ignores what else is in the column, so push down past anything already there */
	const float PushedTop = FMath::Max(Top, Context.ColumnNextY[Depth] + Overhang);
	const float Pushed = PushedTop - Top;

	Top = PushedTop;

	const float Bottom = Top + Size.Y + AnimNodeLayout::RowSpacing;
	Context.ColumnNextY[Depth] = Bottom;

	if (LinkCount == 0) {
		Context.NextLeafY = Bottom;
	}

	/* Right-aligned within the column, so output pins share an edge and the wires into the next column stay
	 * short and level regardless of how much wider one node is than its neighbours. */
	const float X = Context.ColumnLeft[Depth] + (Context.ColumnWidth[Depth] - Size.X);

	Node->NodePosX = FMath::RoundToInt(X);
	Node->NodePosY = FMath::RoundToInt(Top);

	Context.PlacedTop.Add(Node, Top);

	/* Bring the branch behind it along, so the wires it was lined up with stay lined up */
	if (Pushed > 0.f && LinkCount > 0) {
		TSet<UAnimGraphNode_Base*> Shifted;
		Shifted.Add(Node);

		for (UEdGraphPin* Pin : Node->Pins) {
			if (!Pin || Pin->Direction != EGPD_Input) continue;

			for (UEdGraphPin* LinkedPin : Pin->LinkedTo) {
				if (!LinkedPin) continue;

				if (UAnimGraphNode_Base* Child = Cast<UAnimGraphNode_Base>(LinkedPin->GetOwningNode())) {
					ShiftAnimNodeSubtree(Child, Pushed, Context, Shifted);
				}
			}
		}
	}

	return Top;
}

/** Lays out everything reachable from one root, then drops the result in under whatever came before it. */
inline void LayoutAnimGraphTree(UAnimGraphNode_Base* Root, FAnimGraphLayoutContext& Context, TSet<UAnimGraphNode_Base*>& Claimed, float& TreeTop) {
	Context.Depths.Reset();
	Context.ColumnLeft.Reset();
	Context.ColumnWidth.Reset();
	Context.ColumnNextY.Reset();
	Context.PlacedTop.Reset();
	Context.NextLeafY = 0.f;

	TArray<UAnimGraphNode_Base*> TreeNodes;
	TSet<UAnimGraphNode_Base*> Stack;

	CollectAnimNodeTree(Root, 0, Context, Claimed, TreeNodes, Stack);
	if (TreeNodes.Num() == 0) return;

	int32 MaxDepth = 0;
	for (UAnimGraphNode_Base* Node : TreeNodes) {
		MaxDepth = FMath::Max(MaxDepth, Context.Depths.FindChecked(Node));
	}

	/* Columns are measured per tree. Shared across trees, a node would end up right-aligned against whatever
	 * the widest node at that depth happened to be somewhere else entirely - and since it keeps its own width,
	 * the gap in front of it grows by the difference. That is what makes an otherwise uniform run of Modify
	 * Bone nodes read as unevenly spaced. */
	Context.ColumnWidth.SetNumZeroed(MaxDepth + 1);
	for (UAnimGraphNode_Base* Node : TreeNodes) {
		const int32 Depth = Context.Depths.FindChecked(Node);
		Context.ColumnWidth[Depth] = FMath::Max(Context.ColumnWidth[Depth], Context.Metrics.FindChecked(Node).Size.X);
	}

	/* The graph flows right, into the root at depth 0, so each deeper column steps left by its own widest node
	 * plus the gap. Stepping by the measured width is what stops a column of Constraint nodes from running over
	 * the top of whatever sits to its right. */
	Context.ColumnLeft.SetNumZeroed(MaxDepth + 1);
	for (int32 Depth = 1; Depth <= MaxDepth; Depth++) {
		Context.ColumnLeft[Depth] = Context.ColumnLeft[Depth - 1] - AnimNodeLayout::ColumnSpacing - Context.ColumnWidth[Depth];
	}

	/* Nothing occupies a column until something is placed in it, so the first node into each one is free to sit
	 * wherever its wire wants it, however far above the tree's baseline that is */
	Context.ColumnNextY.Init(TNumericLimits<float>::Lowest(), MaxDepth + 1);

	PlaceAnimGraphNode(Root, Context);

	/* Only reachable if a cycle cut the walk short */
	for (UAnimGraphNode_Base* Node : TreeNodes) {
		if (!Context.PlacedTop.Contains(Node)) {
			PlaceAnimGraphNode(Node, Context);
		}
	}

	float MinY = TNumericLimits<float>::Max();
	float MaxY = TNumericLimits<float>::Lowest();

	for (UAnimGraphNode_Base* Node : TreeNodes) {
		const float Top = Context.PlacedTop.FindChecked(Node);
		const FAnimNodeMetrics& Metrics = Context.Metrics.FindChecked(Node);

		MinY = FMath::Min(MinY, Top - Metrics.TopOverhang);
		MaxY = FMath::Max(MaxY, Top + Metrics.Size.Y);
	}

	/* Placement runs in the tree's own space, so shift the finished tree clear of the one before it. A whole
	 * number keeps the wires exactly as straight as they were laid out. */
	const int32 Offset = FMath::RoundToInt(TreeTop - MinY);
	for (UAnimGraphNode_Base* Node : TreeNodes) {
		Node->NodePosY += Offset;
	}

	TreeTop += MaxY - MinY + AnimNodeLayout::TreeSpacing;
}

inline void AutoLayoutAnimGraphNodes(const TArray<FUObjectExport*>& NodeExports) {
	FAnimGraphLayoutContext Context;

	TArray<UAnimGraphNode_Base*> Nodes;
	TArray<UAnimGraphNode_Base*> Roots;

	for (const FUObjectExport* Export : NodeExports) {
		UAnimGraphNode_Base* Node = Cast<UAnimGraphNode_Base>(Export->Object);
		if (!Node) continue;

		Nodes.Add(Node);
		Context.Metrics.Add(Node, MeasureAnimGraphNode(Node));

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

	if (Nodes.Num() == 0) return;

	/* Output pose first so the graph proper is the top tree and the cached poses stack underneath it */
	Roots.Sort([](const UAnimGraphNode_Base& A, const UAnimGraphNode_Base& B) {
		return A.IsA<UAnimGraphNode_Root>() && !B.IsA<UAnimGraphNode_Root>();
	});

	TSet<UAnimGraphNode_Base*> Claimed;
	float TreeTop = 0.f;

	for (UAnimGraphNode_Base* Root : Roots) {
		LayoutAnimGraphTree(Root, Context, Claimed, TreeTop);
	}

	/* Anything unreachable from a root - an island left by a broken link - still needs a home */
	for (UAnimGraphNode_Base* Node : Nodes) {
		if (!Claimed.Contains(Node)) {
			LayoutAnimGraphTree(Node, Context, Claimed, TreeTop);
		}
	}
}
