/* Copyright Reflection Contributors 2024-2026 */

#include "Importers/Constructor/Graph/MaterialNodeSubstitute.h"

#include "Materials/MaterialExpressionAppendVector.h"
#include "Materials/MaterialExpressionComponentMask.h"
#include "Materials/MaterialExpressionConstant.h"

#include "Utilities/JsonHelpers.h"

/* Spacing between the nodes a convert node is rebuilt out of */
static constexpr int32 ConvertColumnWidth = 160;
static constexpr int32 ConvertRowHeight = 80;

/* "EMaterialExpressionConvertType::Vector3" and friends. Anything unrecognized is treated as a
 * scalar, which is what the enum's first entry is. */
static int32 GetConvertComponentCount(const FString& ConvertType) {
	if (ConvertType.EndsWith(TEXT("Vector4"))) return 4;
	if (ConvertType.EndsWith(TEXT("Vector3"))) return 3;
	if (ConvertType.EndsWith(TEXT("Vector2"))) return 2;

	return 1;
}

static float GetLinearColorComponent(const FLinearColor& Color, const int32 ComponentIndex) {
	switch (ComponentIndex) {
		case 1: return Color.G;
		case 2: return Color.B;
		case 3: return Color.A;
		default: return Color.R;
	}
}

/* MaterialExpressionConvert arrived in 5.6. All it does is shuffle input components into output
 * components, and UMaterialExpressionConvert::Compile emits exactly that shuffle: a component mask
 * per mapped component, a constant for every output component no mapping writes to, and a chain of
 * appends joining them into the output's vector. Older engines get the same graph spelled out with
 * those three nodes, which have existed the whole time.
 *
 * The node has several outputs and the nodes replacing it have one each, so the root of every
 * output is registered, which moves each incoming connection onto the root belonging to the output
 * it asked for. */
class FConvertSubstitute final : public FMaterialNodeSubstitute {
public:
	virtual bool Handles(const FName Type) const override {
		return Type == FName(TEXT("MaterialExpressionConvert"));
	}

	virtual UMaterialExpression* Create(FMaterialSubstituteContext& Context) override {
		const TSharedPtr<FJsonObject> Properties = Context.GetProperties();

		if (Context.GetParent() == nullptr || !Properties.IsValid()) {
			return nullptr;
		}

		TArray<TSharedPtr<FJsonValue>> ConvertInputs;
		TArray<TSharedPtr<FJsonValue>> ConvertOutputs;
		TArray<TSharedPtr<FJsonValue>> ConvertMappings;

		const TArray<TSharedPtr<FJsonValue>>* ArrayField;
		if (Properties->TryGetArrayField(TEXT("ConvertInputs"), ArrayField)) ConvertInputs = *ArrayField;
		if (Properties->TryGetArrayField(TEXT("ConvertOutputs"), ArrayField)) ConvertOutputs = *ArrayField;
		if (Properties->TryGetArrayField(TEXT("ConvertMappings"), ArrayField)) ConvertMappings = *ArrayField;

		/* Nothing to stand in for, so let the node be reported missing like any other */
		if (ConvertOutputs.Num() == 0) {
			return nullptr;
		}

		FString NodeName = TEXT("Convert");
		Properties->TryGetStringField(TEXT("NodeName"), NodeName);

		const int32 BaseX = Context.GetEditorX();
		const int32 BaseY = Context.GetEditorY();

		TArray<UMaterialExpression*> Roots;
		int32 RowY = BaseY;

		for (int32 OutputIndex = 0; OutputIndex < ConvertOutputs.Num(); OutputIndex++) {
			FString OutputType = TEXT("Scalar");
			FLinearColor OutputDefault = FLinearColor::Black;

			if (const TSharedPtr<FJsonObject> Output = ConvertOutputs[OutputIndex]->AsObject()) {
				Output->TryGetStringField(TEXT("Type"), OutputType);

				const TSharedPtr<FJsonObject>* DefaultValue;
				if (Output->TryGetObjectField(TEXT("DefaultValue"), DefaultValue)) {
					OutputDefault = ObjectToLinearColor(DefaultValue->Get());
				}
			}

			const int32 ComponentCount = GetConvertComponentCount(OutputType);
			TArray<UMaterialExpression*> Components;

			for (int32 ComponentIndex = 0; ComponentIndex < ComponentCount; ComponentIndex++) {
				const int32 NodeY = RowY + ComponentIndex * ConvertRowHeight;
				const FString NameSuffix = FString::Printf(TEXT("_Out%d_C%d"), OutputIndex, ComponentIndex);

				/* The mapping that writes this output component, if there is one */
				TSharedPtr<FJsonObject> Mapping;

				for (const TSharedPtr<FJsonValue>& MappingValue : ConvertMappings) {
					const TSharedPtr<FJsonObject> MappingObject = MappingValue->AsObject();
					if (!MappingObject.IsValid()) continue;

					int32 MappedOutput = 0;
					int32 MappedOutputComponent = 0;
					MappingObject->TryGetNumberField(TEXT("OutputIndex"), MappedOutput);
					MappingObject->TryGetNumberField(TEXT("OutputComponentIndex"), MappedOutputComponent);

					if (MappedOutput == OutputIndex && MappedOutputComponent == ComponentIndex) {
						Mapping = MappingObject;
						break;
					}
				}

				/* Unwritten components take the output's own default value */
				if (!Mapping.IsValid()) {
					UMaterialExpressionConstant* Constant = Context.CreateExpression<UMaterialExpressionConstant>(NameSuffix, BaseX, NodeY);
					Constant->R = GetLinearColorComponent(OutputDefault, ComponentIndex);

					Components.Add(Constant);
					continue;
				}

				int32 InputIndex = 0;
				int32 InputComponentIndex = 0;
				Mapping->TryGetNumberField(TEXT("InputIndex"), InputIndex);
				Mapping->TryGetNumberField(TEXT("InputComponentIndex"), InputComponentIndex);

				TSharedPtr<FJsonObject> ExpressionInput;
				FLinearColor InputDefault = FLinearColor::Black;

				if (ConvertInputs.IsValidIndex(InputIndex)) {
					if (const TSharedPtr<FJsonObject> Input = ConvertInputs[InputIndex]->AsObject()) {
						const TSharedPtr<FJsonObject>* DefaultValue;
						if (Input->TryGetObjectField(TEXT("DefaultValue"), DefaultValue)) {
							InputDefault = ObjectToLinearColor(DefaultValue->Get());
						}

						const TSharedPtr<FJsonObject>* InputObject;
						if (Input->TryGetObjectField(TEXT("ExpressionInput"), InputObject)) {
							/* Pre-4.25 exports name the expression inline instead of nesting an object */
							if (InputObject->Get()->HasField(TEXT("Expression")) || InputObject->Get()->HasField(TEXT("ExpressionName"))) {
								ExpressionInput = *InputObject;
							}
						}
					}
				}

				/* An input with nothing connected compiles to its own default value, so the component
				 * it would have been masked out of is known here and needs no node to read from */
				if (!ExpressionInput.IsValid()) {
					UMaterialExpressionConstant* Constant = Context.CreateExpression<UMaterialExpressionConstant>(NameSuffix, BaseX, NodeY);
					Constant->R = GetLinearColorComponent(InputDefault, InputComponentIndex);

					Components.Add(Constant);
					continue;
				}

				UMaterialExpressionComponentMask* Mask = Context.CreateExpression<UMaterialExpressionComponentMask>(NameSuffix, BaseX, NodeY);
				Mask->R = InputComponentIndex == 0 ? 1 : 0;
				Mask->G = InputComponentIndex == 1 ? 1 : 0;
				Mask->B = InputComponentIndex == 2 ? 1 : 0;
				Mask->A = InputComponentIndex == 3 ? 1 : 0;

				Context.ConnectLater(Mask, SingleInput, ExpressionInput);
				Components.Add(Mask);
			}

			/* Chain appends to form the output's vector, exactly as the node compiles it */
			UMaterialExpression* Root = Components[0];

			for (int32 ComponentIndex = 1; ComponentIndex < Components.Num(); ComponentIndex++) {
				UMaterialExpressionAppendVector* Append = Context.CreateExpression<UMaterialExpressionAppendVector>(
					FString::Printf(TEXT("_Out%d_Append%d"), OutputIndex, ComponentIndex),
					BaseX + ComponentIndex * ConvertColumnWidth,
					RowY
				);

				Append->A.Connect(0, Root);
				Append->B.Connect(0, Components[ComponentIndex]);

				Root = Append;
			}

			/* The caption the convert node carried, so the graph still reads as what it replaced */
			Root->Desc = NodeName;

			Roots.Add(Root);
			RowY += (ComponentCount + 1) * ConvertRowHeight;
		}

		TArray<UObject*> RootObjects;
		RootObjects.Reserve(Roots.Num());

		for (UMaterialExpression* Root : Roots) {
			RootObjects.Add(Root);
		}

		/* The first output's root is what the export hands out, so it is what every reference to this
		 * convert node resolves to before being moved onto the root for the output it named */
		Context.RegisterOutputRoots(Roots[0], RootObjects);

		GLog->Log(*FString::Printf(TEXT("Reflection: Rebuilt Convert node \"%s\" (%s) out of masks and appends"), *NodeName, *Context.GetBaseName()));

		return Roots[0];
	}
};

REGISTER_MATERIAL_NODE_SUBSTITUTE(FConvertSubstitute);
