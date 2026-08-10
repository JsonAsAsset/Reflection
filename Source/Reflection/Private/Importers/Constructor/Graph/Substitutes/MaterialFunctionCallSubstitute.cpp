/* Copyright Reflection Contributors 2024-2026 */

#include "Importers/Constructor/Graph/MaterialNodeSubstitute.h"

#include "Materials/MaterialExpressionConstant.h"
#include "Materials/MaterialExpressionMaterialFunctionCall.h"
#include "Materials/MaterialFunctionInterface.h"

#include "Engine/Package.h"

/* A node an engine material function already covers */
struct FMaterialFunctionSubstitution {
	const TCHAR* NodeType;
	const TCHAR* FunctionPath;
	TArray<FSubstituteInputMapping> Inputs;
};

/* Another such node is a row here, not a substitute of its own */
static const TArray<FMaterialFunctionSubstitution>& GetMaterialFunctionSubstitutions() {
	static const TArray<FMaterialFunctionSubstitution> Substitutions = {
		{
			TEXT("MaterialExpressionSmoothStep"),
			TEXT("/Engine/Functions/Engine_MaterialFunctions02/SmoothStep.SmoothStep"),
			{
				{ TEXT("Value"), TEXT("Alpha"), TEXT("ConstValue"), 0.0f },
				{ TEXT("Min"), TEXT("Min"), TEXT("ConstMin"), 0.0f },
				{ TEXT("Max"), TEXT("Max"), TEXT("ConstMax"), 1.0f }
			}
		}
	};

	return Substitutions;
}

/* Asked of the call, as the order the function lists its inputs in is not this table's business */
static int32 FindFunctionInputIndexByName(const UMaterialExpressionMaterialFunctionCall* Call, const TCHAR* Name) {
	for (int32 Index = 0; Index < Call->FunctionInputs.Num(); Index++) {
		if (Call->FunctionInputs[Index].Input.InputName == FName(Name)) {
			return Index;
		}
	}

	return INDEX_NONE;
}

/* An unconnected input gets a constant: an open function input takes the function's default rather
 * than the node's, and errors outright where the function input has none. */
class FMaterialFunctionCallSubstitute final : public FMaterialNodeSubstitute {
public:
	virtual bool Handles(const FName Type) const override {
		return FindSubstitution(Type) != nullptr;
	}

	virtual UMaterialExpression* Create(FMaterialSubstituteContext& Context) override {
		const FMaterialFunctionSubstitution* Substitution = FindSubstitution(Context.GetType());

		if (Substitution == nullptr) {
			return nullptr;
		}

		UMaterialFunctionInterface* Function = LoadObjectByPath<UMaterialFunctionInterface>(Substitution->FunctionPath);

		if (Function == nullptr) {
			return nullptr;
		}

		UObject* Parent = Context.GetParent();
		const FString BaseName = Context.GetBaseName();

		UMaterialExpressionMaterialFunctionCall* Call = NewObject<UMaterialExpressionMaterialFunctionCall>(
			Parent,
			UMaterialExpressionMaterialFunctionCall::StaticClass(),
			MakeUniqueObjectName(Parent, UMaterialExpressionMaterialFunctionCall::StaticClass(), Context.GetName()),
			RF_Transactional
		);

		/* Names the call's inputs and outputs off the function */
		Call->MaterialFunction = Function;
		Call->UpdateFromFunctionResource();

		int32 ConstantRow = 0;

		for (const FSubstituteInputMapping& Mapping : Substitution->Inputs) {
			const int32 InputIndex = FindFunctionInputIndexByName(Call, Mapping.TargetInput);

			/* This engine ships a different function than the table was written against */
			if (InputIndex == INDEX_NONE) {
				GLog->Log(*FString::Printf(
					TEXT("Reflection: the engine's %s function has no \"%s\" input, so \"%s\" is left without one"),
					Substitution->NodeType, Mapping.TargetInput, *BaseName
				));

				continue;
			}

			if (const TSharedPtr<FJsonObject> ExpressionInput = Context.FindConnectedInput(Mapping.NodeInput)) {
				Context.ConnectLater(Call, InputIndex, ExpressionInput);

				continue;
			}

			UMaterialExpressionConstant* Constant = Context.CreateConstant(
				Context.ReadInputConstant(Mapping.NodeConstant, Mapping.DefaultConstant),
				Mapping.TargetInput,
				Context.GetEditorX() - SubstituteConstantOffsetX,
				Context.GetEditorY() + ConstantRow * SubstituteConstantRowHeight
			);

			/* Connect leaves the slot's name alone, which pairs it with the function's input */
			Call->FunctionInputs[InputIndex].Input.Connect(0, Constant);

			ConstantRow++;
		}

		GLog->Log(*FString::Printf(
			TEXT("Reflection: Rebuilt %s \"%s\" as a call to %s"),
			Substitution->NodeType, *BaseName, Substitution->FunctionPath
		));

		return Call;
	}

private:
	static const FMaterialFunctionSubstitution* FindSubstitution(const FName Type) {
		for (const FMaterialFunctionSubstitution& Candidate : GetMaterialFunctionSubstitutions()) {
			if (Type == FName(Candidate.NodeType)) {
				return &Candidate;
			}
		}

		return nullptr;
	}
};

REGISTER_MATERIAL_NODE_SUBSTITUTE(FMaterialFunctionCallSubstitute);
