/* Copyright Reflection Contributors 2024-2026 */

#include "Importers/Constructor/Graph/MaterialNodeSubstitute.h"

#include "Importers/Constructor/Graph/MaterialGraph.h"

#include "Materials/MaterialExpressionComment.h"
#include "Materials/MaterialExpressionReroute.h"

#include "Engine/Compatibility.h"
#include "Importers/Constructor/ImportIssues.h"
#include "Serializers/ObjectSerializer.h"

/* The first connection written on an export, which for a switch is the branch its own class lists
 * first. Matched by shape: an expression input is the one property carrying an Expression, and the
 * classes this is reached for are the ones with no properties here to name. */
static TSharedPtr<FJsonObject> FindFirstExpressionInput(const TSharedPtr<FJsonObject>& Properties) {
	if (!Properties.IsValid()) {
		return nullptr;
	}

	for (const auto& Pair : Properties->Values) {
		if (!Pair.Value.IsValid() || Pair.Value->Type != EJson::Object) {
			continue;
		}

		const TSharedPtr<FJsonObject> Value = Pair.Value->AsObject();

		if (Value.IsValid() && Value->HasField(TEXT("Expression"))) {
			return Value;
		}
	}

	return nullptr;
}

/* Reports what nothing else claimed. Answers for every type, below everything. */
class FMissingNodeSubstitute final : public FMaterialNodeSubstitute {
public:
	virtual bool Handles(const FName Type) const override {
		return true;
	}

	virtual int32 GetPriority() const override {
		return MIN_int32;
	}

	virtual UMaterialExpression* Create(FMaterialSubstituteContext& Context) override {
		const FName Type = Context.GetType();
		UObject* Parent = Context.GetParent();

		const TSharedPtr<FJsonObject> Properties = Context.GetProperties();

#if ENGINE_UE4
		/* In Unreal Engine 4, to combat the absence of Sub-graphs, create a Material Function in place of it */
		if (Type == "MaterialExpressionComposite") {
			return nullptr;
		}
#endif

		/* Add a comment in the graph notifying the user of a missing node ~~~~~~~~~~~~~~~~~~~~~~~~ */
		UMaterialExpressionComment* Comment = NewObject<UMaterialExpressionComment>(Parent, UMaterialExpressionComment::StaticClass(), *("UMaterialExpressionComment_" + Type.ToString()), RF_Transactional);

		Comment->Text = *("Missing Node: " + Type.ToString());
		Comment->CommentColor = FLinearColor(1.0, 0.0, 0.0);
		Comment->bCommentBubbleVisible = true;
		Comment->SizeX = 415;
		Comment->SizeY = 40;

		Comment->Desc = "A node is missing from your Unreal Engine build. This can occur for several reasons, but it is most likely because your version of Unreal Engine is older than the one you are porting from.";

		Context.GetGraph().GetObjectSerializer()->DeserializeObjectProperties(Properties, Comment);
		Context.AddExpression(Comment);

		/* Report the missing node ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ */
		GLog->Log(*("Reflection: Missing Node " + Type.ToString() + " in Parent " + Parent->GetName()));

		FImportIssues::Report(
			EImportIssue::MissingClass,
			"Missing node " + Type.ToString(),
			"This engine build has no class for it, so a commented reroute stands in for it in " + Parent->GetName() + "."
		);

		/* Put a reroute in place of the missing node */
		UMaterialExpressionReroute* Reroute = NewObject<UMaterialExpressionReroute>(
			Parent,
			UMaterialExpressionReroute::StaticClass(),
			Context.GetName()
		);

		/* A switch picks one of its branches, and which one is a decision this engine has no class to
		 * make, so the first branch is carried through the reroute. A switch dropped outright takes
		 * everything hanging off it with it, where the first branch is the one the node itself reads
		 * when nothing tells it otherwise, and leaves a graph that still resolves. The comment above
		 * stays either way, so the node is still reported missing rather than quietly stood in for. */
		if (Type.ToString().Contains(TEXT("Switch"))) {
			if (const TSharedPtr<FJsonObject> FirstInput = FindFirstExpressionInput(Properties)) {
				Context.ConnectLater(Reroute, SingleInput, FirstInput);
			}
		}

		return Reroute;
	}
};

REGISTER_MATERIAL_NODE_SUBSTITUTE(FMissingNodeSubstitute);
