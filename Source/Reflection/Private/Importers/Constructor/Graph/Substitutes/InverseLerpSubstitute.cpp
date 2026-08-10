/* Copyright Reflection Contributors 2024-2026 */

#include "Importers/Constructor/Graph/MaterialNodeSubstitute.h"

#include "Materials/MaterialExpressionClamp.h"
#include "Materials/MaterialExpressionDivide.h"
#include "Materials/MaterialExpressionReroute.h"
#include "Materials/MaterialExpressionSubtract.h"

/* Spacing of the nodes the arithmetic is spelled out over */
static constexpr int32 InverseLerpColumnWidth = 190;
static constexpr int32 InverseLerpRowHeight = 110;

/* What the node reads for an unconnected input, per its own class */
static constexpr float InverseLerpConstA = 0.0f;
static constexpr float InverseLerpConstB = 1.0f;
static constexpr float InverseLerpConstValue = 0.0f;

/* (Value - A) / (B - A), saturated if the node says to clamp. Nodes rather than a custom
 * expression, which would have to name one output width and lose the componentwise case.
 *
 * Subtract and Divide read their own ConstA/ConstB when nothing is connected, same as the node did.
 *
 * Hands back a reroute, not the Divide: the export's properties land on whatever this returns, and
 * a Divide has A and B of its own that the node's A and B would overwrite. */
class FInverseLerpSubstitute final : public FMaterialNodeSubstitute {
public:
	virtual bool Handles(const FName Type) const override {
		return Type == FName(TEXT("MaterialExpressionInverseLinearInterpolate"));
	}

	virtual UMaterialExpression* Create(FMaterialSubstituteContext& Context) override {
		const TSharedPtr<FJsonObject> Properties = Context.GetProperties();

		const int32 EditorX = Context.GetEditorX();
		const int32 EditorY = Context.GetEditorY();

		UMaterialExpressionSubtract* Numerator = Context.CreateExpression<UMaterialExpressionSubtract>(
			TEXT("_ValueMinusA"),
			EditorX - InverseLerpColumnWidth * 2,
			EditorY
		);

		UMaterialExpressionSubtract* Denominator = Context.CreateExpression<UMaterialExpressionSubtract>(
			TEXT("_BMinusA"),
			EditorX - InverseLerpColumnWidth * 2,
			EditorY + InverseLerpRowHeight
		);

		/* One of the node's inputs onto one slot of the arithmetic */
		auto ConnectInput = [&](UMaterialExpression* Target, const int32 InputIndex, const TCHAR* NodeInput, const TCHAR* NodeConstant, const float DefaultConstant, float& SlotConstant) {
			if (const TSharedPtr<FJsonObject> ExpressionInput = Context.FindConnectedInput(NodeInput)) {
				Context.ConnectLater(Target, InputIndex, ExpressionInput);

				return;
			}

			SlotConstant = Context.ReadInputConstant(NodeConstant, DefaultConstant);
		};

		ConnectInput(Numerator, BinaryInputA, TEXT("Value"), TEXT("ConstValue"), InverseLerpConstValue, Numerator->ConstA);
		ConnectInput(Numerator, BinaryInputB, TEXT("A"), TEXT("ConstA"), InverseLerpConstA, Numerator->ConstB);

		ConnectInput(Denominator, BinaryInputA, TEXT("B"), TEXT("ConstB"), InverseLerpConstB, Denominator->ConstA);
		ConnectInput(Denominator, BinaryInputB, TEXT("A"), TEXT("ConstA"), InverseLerpConstA, Denominator->ConstB);

		UMaterialExpressionDivide* Divide = Context.CreateExpression<UMaterialExpressionDivide>(
			TEXT("_Divide"),
			EditorX - InverseLerpColumnWidth,
			EditorY
		);

		Divide->A.Connect(0, Numerator);
		Divide->B.Connect(0, Denominator);

		UMaterialExpression* Result = Divide;

		bool bClampResult = false;

		if (Properties.IsValid()) {
			Properties->TryGetBoolField(TEXT("bClampResult"), bClampResult);
		}

		/* Clamp of 0..1, as Saturate is not a node on every engine */
		if (bClampResult) {
			UMaterialExpressionClamp* Clamp = Context.CreateExpression<UMaterialExpressionClamp>(
				TEXT("_Clamp"),
				EditorX - InverseLerpColumnWidth / 2,
				EditorY
			);

			Clamp->ClampMode = CMODE_Clamp;
			Clamp->MinDefault = 0.0f;
			Clamp->MaxDefault = 1.0f;

			Clamp->Input.Connect(0, Divide);

			Result = Clamp;
		}

		UMaterialExpressionReroute* Reroute = NewObject<UMaterialExpressionReroute>(
			Context.GetParent(),
			UMaterialExpressionReroute::StaticClass(),
			Context.GetName(),
			RF_Transactional
		);

		Reroute->Input.Connect(0, Result);
		Reroute->Desc = TEXT("InvLerp");

		GLog->Log(*FString::Printf(TEXT("Reflection: Rebuilt MaterialExpressionInverseLinearInterpolate \"%s\" out of subtracts and a divide"), *Context.GetBaseName()));

		return Reroute;
	}
};

REGISTER_MATERIAL_NODE_SUBSTITUTE(FInverseLerpSubstitute);
