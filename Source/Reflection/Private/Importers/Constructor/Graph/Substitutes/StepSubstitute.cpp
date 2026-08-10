/* Copyright Reflection Contributors 2024-2026 */

#include "Importers/Constructor/Graph/MaterialNodeSubstitute.h"

#include "Materials/MaterialExpressionConstant.h"
#include "Materials/MaterialExpressionCustom.h"

#include "Engine/Compatibility.h"

static const TCHAR* StepCode = TEXT("return step(Y, X);");

/* Y is the edge, X the value tested against it, in the order step() takes them */
static const FSubstituteInputMapping StepInputs[] = {
	{ TEXT("Y"), TEXT("Y"), TEXT("ConstY"), 0.0f },
	{ TEXT("X"), TEXT("X"), TEXT("ConstX"), 0.0f }
};

/* Step wraps one intrinsic, so it becomes a custom expression holding the call.
 *
 * Both inputs are always wired: the node compiled an unconnected one off ConstY/ConstX, where a
 * custom expression with an open input does not compile at all.
 *
 * Output is a scalar. A Step over vectors would need the output type widened, which cannot be told
 * from the export. */
class FStepSubstitute final : public FMaterialNodeSubstitute {
public:
	virtual bool Handles(const FName Type) const override {
		return Type == FName(TEXT("MaterialExpressionStep"));
	}

	virtual UMaterialExpression* Create(FMaterialSubstituteContext& Context) override {
		UObject* Parent = Context.GetParent();

		UMaterialExpressionCustom* Custom = NewObject<UMaterialExpressionCustom>(
			Parent,
			UMaterialExpressionCustom::StaticClass(),
			MakeUniqueObjectName(Parent, UMaterialExpressionCustom::StaticClass(), Context.GetName()),
			RF_Transactional
		);

		Custom->Code = StepCode;
		Custom->OutputType = CMOT_Float1;

		/* A custom expression captions itself with its description */
		Custom->Description = TEXT("Step");

		/* The class is constructed holding an unnamed input of its own */
		Custom->Inputs.Empty();

		for (const FSubstituteInputMapping& Mapping : StepInputs) {
			const int32 InputIndex = Custom->Inputs.AddDefaulted();

			SetExpressionName(Custom->Inputs[InputIndex].InputName, Mapping.TargetInput);
		}

		int32 ConstantRow = 0;

		/* A pass of its own, so the array is done growing before a slot is handed out */
		for (int32 InputIndex = 0; InputIndex < Custom->Inputs.Num(); InputIndex++) {
			const FSubstituteInputMapping& Mapping = StepInputs[InputIndex];

			if (const TSharedPtr<FJsonObject> ExpressionInput = Context.FindConnectedInput(Mapping.NodeInput)) {
				Context.ConnectLater(Custom, InputIndex, ExpressionInput);

				continue;
			}

			UMaterialExpressionConstant* Constant = Context.CreateConstant(
				Context.ReadInputConstant(Mapping.NodeConstant, Mapping.DefaultConstant),
				Mapping.TargetInput,
				Context.GetEditorX() - SubstituteConstantOffsetX,
				Context.GetEditorY() + ConstantRow * SubstituteConstantRowHeight
			);

			Custom->Inputs[InputIndex].Input.Connect(0, Constant);

			ConstantRow++;
		}

		GLog->Log(*FString::Printf(TEXT("Reflection: Rebuilt MaterialExpressionStep \"%s\" as a custom expression"), *Context.GetBaseName()));

		return Custom;
	}
};

REGISTER_MATERIAL_NODE_SUBSTITUTE(FStepSubstitute);
