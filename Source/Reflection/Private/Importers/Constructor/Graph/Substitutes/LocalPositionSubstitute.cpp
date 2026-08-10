/* Copyright Reflection Contributors 2024-2026 */

#include "Importers/Constructor/Graph/MaterialNodeSubstitute.h"

#include "Materials/MaterialExpressionMaterialFunctionCall.h"
#include "Materials/MaterialExpressionReroute.h"
#include "Materials/MaterialFunctionInterface.h"

#include "Engine/Compatibility.h"
#include "Engine/Package.h"

/* The function the node was made out of, which every engine this runs on already ships */
static const TCHAR* LocalPositionFunctionPath = TEXT("/Engine/Functions/Engine_MaterialFunctions02/WorldPositionOffset/LocalPosition.LocalPosition");
static const TCHAR* LocalPositionExcludingOffsetsOutput = TEXT("Local Position (Excluding Offsets)");

/* Where an output of that name sits on the call, asked of it rather than counted on */
static int32 FindOutputIndexByName(const UMaterialExpression* Expression, const FString& Name) {
	for (int32 Index = 0; Index < Expression->Outputs.Num(); Index++) {
		if (OutputNameToString(Expression->Outputs[Index].OutputName) == Name) {
			return Index;
		}
	}

	return INDEX_NONE;
}

/* MaterialExpressionLocalPosition is a node put around something the engine had already been
 * shipping as a material function for years, and that function is still sitting in the same place
 * on every version this runs on, both of its outputs included. So the node goes back to being a
 * call to it.
 *
 * Shader Offsets is what picks between the two outputs. Included is the function's first output
 * and needs nothing further. Excluded is its second, and the node it is replacing had one output
 * that everything reading it recorded as index 0, so a reroute is put on the second output and
 * handed out in the node's place: whatever was reading index 0 keeps reading index 0 and gets the
 * offsets excluded value. */
class FLocalPositionSubstitute final : public FMaterialNodeSubstitute {
public:
	virtual bool Handles(const FName Type) const override {
		return Type == FName(TEXT("MaterialExpressionLocalPosition"));
	}

	virtual UMaterialExpression* Create(FMaterialSubstituteContext& Context) override {
		UMaterialFunctionInterface* Function = LoadObjectByPath<UMaterialFunctionInterface>(LocalPositionFunctionPath);

		/* Without the function there is nothing to build out of, so it goes back to being reported
		 * missing like any other node this engine has no answer for */
		if (Function == nullptr) {
			return nullptr;
		}

		UObject* Parent = Context.GetParent();
		const TSharedPtr<FJsonObject> Properties = Context.GetProperties();

		UMaterialExpressionMaterialFunctionCall* Call = NewObject<UMaterialExpressionMaterialFunctionCall>(
			Parent,
			UMaterialExpressionMaterialFunctionCall::StaticClass(),
			MakeUniqueObjectName(Parent, UMaterialExpressionMaterialFunctionCall::StaticClass(), *(Context.GetBaseName() + TEXT("_Function"))),
			RF_Transactional
		);

		/* Fills in the call's inputs and outputs off the function, which is what names them */
		Call->MaterialFunction = Function;
		Call->UpdateFromFunctionResource();

		Call->MaterialExpressionEditorX = Context.GetEditorX();
		Call->MaterialExpressionEditorY = Context.GetEditorY();

		/* Local Origin has no equivalent on the function, so a node asking for anything but the
		 * instance position comes out reading the instance position and says so */
		FString LocalOrigin;

		if (Properties.IsValid() && Properties->TryGetStringField(TEXT("LocalOrigin"), LocalOrigin) && !LocalOrigin.Contains(TEXT("Instance"))) {
			GLog->Log(*FString::Printf(TEXT("Reflection: \"%s\" wanted local position origin %s, which the engine function has no output for"), *Context.GetBaseName(), *LocalOrigin));
		}

		FString IncludedOffsets;

		const bool bExcludeOffsets = Properties.IsValid()
			&& Properties->TryGetStringField(TEXT("IncludedOffsets"), IncludedOffsets)
			&& IncludedOffsets.Contains(TEXT("ExcludeOffsets"));

		if (!bExcludeOffsets) {
			return Call;
		}

		const int32 ExcludingOffsetsOutput = FindOutputIndexByName(Call, LocalPositionExcludingOffsetsOutput);

		if (ExcludingOffsetsOutput == INDEX_NONE) {
			GLog->Log(*FString::Printf(TEXT("Reflection: the engine's LocalPosition function has no \"%s\" output, so \"%s\" reads the offsets included one"), LocalPositionExcludingOffsetsOutput, *Context.GetBaseName()));

			return Call;
		}

		/* The call is the extra node here, so it is parented on the way past. The reroute handed back
		 * is parented by the caller like any other expression an export resolves to. */
		Context.AddExpression(Call);

		/* Sat to the left of where the node was, leaving the reroute to land on the original spot when
		 * the export's own editor position is read onto it */
		Call->MaterialExpressionEditorX = Context.GetEditorX() - 200;

		UMaterialExpressionReroute* Reroute = NewObject<UMaterialExpressionReroute>(
			Parent,
			UMaterialExpressionReroute::StaticClass(),
			Context.GetName(),
			RF_Transactional
		);

		Reroute->Input.Expression = Call;
		Reroute->Input.OutputIndex = ExcludingOffsetsOutput;
		Reroute->Desc = LocalPositionExcludingOffsetsOutput;

		return Reroute;
	}
};

REGISTER_MATERIAL_NODE_SUBSTITUTE(FLocalPositionSubstitute);
