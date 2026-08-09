/* Copyright Reflection Contributors 2024-2026 */

#include "Importers/Constructor/Graph/MaterialGraph.h"

/* Expressions */
#include "Materials/MaterialExpressionAppendVector.h"
#include "Materials/MaterialExpressionComment.h"
#include "Materials/MaterialExpressionComponentMask.h"
#include "Materials/MaterialExpressionConstant.h"
#include "Materials/MaterialExpressionReroute.h"
/* MaterialExpressionLocalPosition is rebuilt as a call to an engine material function */
#include "Materials/MaterialExpressionMaterialFunctionCall.h"
#include "Materials/MaterialFunctionInterface.h"
/* FCustomPrimitiveData, for how many data floats this engine gives a primitive */
#include "SceneTypes.h"
#include "Engine/EngineUtilities.h"
#include "Importers/Constructor/ImportIssues.h"
#include "Utilities/JsonHelpers.h"

#if ENGINE_UE5
#include "Materials/MaterialExpressionTextureBase.h"
#endif

/* 4.25 and below build this module without the engine's shared PCH (see Reflection.Build.cs),
 * which is where the material types used below used to come in from */
#if UE4_25_BELOW
#include "Materials/Material.h"
#include "Materials/MaterialFunction.h"
#include "Materials/MaterialExpressionTextureBase.h"
#endif

TSharedPtr<FJsonObject> IMaterialGraph::FindMaterialData(const FString& Type, FUObjectExportContainer* Container) {
	TSharedPtr<FJsonObject> EditorOnlyData;

	for (FUObjectExport* Export : GetContainer()->Exports) {
		FString ExportType = Export->GetType().ToString();

		/* If an editor only data object is found, just set it */
		if (ExportType == Type + "EditorOnlyData") {
			EditorOnlyData = Export->JsonObject;
			continue;
		}

		/* For older versions, the "editor" data is in the main UMaterial/UMaterialFunction export */
		if (ExportType == Type) {
			EditorOnlyData = Export->JsonObject;
			continue;
		}

		/* Add to the list of expressions */
		Export->Parent = AssetExport->Object;
		Container->Exports.Add(Export);
	}

	return EditorOnlyData->GetObjectField(TEXT("Properties"));
}

/* A null in the expression list is a node the export never carried. */
bool IMaterialGraph::HasNullExpressions(const TSharedPtr<FJsonObject>& Properties) {
	const TArray<TSharedPtr<FJsonValue>>* Expressions = nullptr;
	const TSharedPtr<FJsonObject>* ExpressionCollection;

	/* 5.1 moved the list into a collection of its own */
	if (Properties->TryGetObjectField(TEXT("ExpressionCollection"), ExpressionCollection)) {
		(*ExpressionCollection)->TryGetArrayField(TEXT("Expressions"), Expressions);
	} else {
		Properties->TryGetArrayField(TEXT("Expressions"), Expressions);
	}

	if (Expressions == nullptr) {
		return false;
	}

	for (const TSharedPtr<FJsonValue>& Expression : *Expressions) {
		if (!Expression.IsValid() || Expression->IsNull()) {
			return true;
		}
	}

	return false;
}

void IMaterialGraph::ConstructExpressions(FUObjectExportContainer* Container) {
	/* Go through each expression, and create the expression */
	for (FUObjectExport* Export : Container->Exports) {
		/* Invalid Json Object */
		if (!Export->JsonObject.IsValid()) {
			continue;
		}

		UObject* Expression = Export->Object;

		if (Expression == nullptr) {
			Expression = CreateEmptyExpression(Export, Container);
		}

		/* If nullptr, expression isn't valid */
		if (Expression == nullptr) {
			continue;
		}

		Export->Object = Expression;
	}

	/* Every export has an object now, which is what the substitutes were waiting on */
	ResolveConvertSubstitutes(Container);
	ResolveNamedRerouteUsages(Container);
	ResolveSwitchPassthroughs(Container);
}

void IMaterialGraph::PropagateExpressions(FUObjectExportContainer* Container) {
	for (FUObjectExport* Export : Container->Exports) {
		/* Get variables from the export data */
		UObject* Parent = Export->Parent;

		/* Get Json Objects from Export */
		TSharedPtr<FJsonObject> ExportJsonObject = Export->JsonObject;
		TSharedPtr<FJsonObject> Properties = Export->GetProperties();

		/* Created Expression */
		UMaterialExpression* Expression = Export->Get<UMaterialExpression>();

		/* Skip null expressions */
		if (Expression == nullptr) {
			continue;
		}

		/* A convert export was expanded into its own nodes when it was created, all of which are
		 * already parented. What is left on the export describes a class this engine does not have */
		if (IsConvertSubstitute(Expression)) {
			continue;
		}

		/* Which subgraph an expression was drawn inside of, on the engines that have subgraphs.
		 *
		 * Where they do not, the nesting is the only thing that does not survive: the expression is
		 * kept and parented to the material or function directly, so the graph comes out flat with
		 * every node in it. Dropping it instead leaves the rest of the graph holding an expression
		 * that is in no expression list, which has no node built for it, and the material editor
		 * casts that missing node unchecked the first time the asset is opened. */
		if (Properties->HasField(TEXT("SubgraphExpression"))) {
#if ENGINE_UE5
			const TSharedPtr<FJsonObject> SubGraphExpressionObject = Properties->GetObjectField(TEXT("SubgraphExpression"));

			const FName SubGraphExpressionName = GetExportNameOfSubobject(SubGraphExpressionObject->GetStringField(TEXT("ObjectName")));
			const FUObjectExport* SubGraphExport = Container->Find(SubGraphExpressionName);

			Expression->SubgraphExpression = SubGraphExport->Get<UMaterialExpression>();
#endif
		}

		GetObjectSerializer()->DeserializeObjectProperties(Properties, Expression);
		ClampCustomPrimitiveDataIndex(Expression);
		SetExpressionParent(Parent, Expression, Properties);

		AddExpressionToParent(Parent, Expression);
	}
}

/* How many custom primitive data floats a primitive carries is not the same number on every
 * engine: 4.23 has 32 where 4.27 and 5.x have 36. A scalar or vector parameter set to read one
 * past the end of what this build has does not fail to compile, it trips the translator's own
 * bounds check the first time the material is cached, and that is a crash rather than an error in
 * the material editor.
 *
 * The slot the node wanted is not in this engine either way, so the index is brought back to the
 * last one that exists. The translator already fills the components past the end of the array
 * with zero, so a vector parameter clamped this way reads what there is and zero for the rest.
 *
 * Found by property name rather than by class: the engines that never had custom primitive data
 * have neither property and so have nothing here to clamp. */
void IMaterialGraph::ClampCustomPrimitiveDataIndex(UMaterialExpression* Expression) {
	if (Expression == nullptr) {
		return;
	}

	/* Custom primitive data arrives in 4.23, and the count that bounds an index into it arrives
	 * with it. Before that there is no property below to find and no number to hold one to, so
	 * this is the one part of the check that has to be asked of the engine version rather than of
	 * the object: the count is a constant on a type that is not there to name. */
#if !UE4_22_BELOW
	const FBoolProperty* UsesCustomData = FindFProperty<FBoolProperty>(Expression->GetClass(), TEXT("bUseCustomPrimitiveData"));

	if (UsesCustomData == nullptr || !UsesCustomData->GetPropertyValue_InContainer(Expression)) {
		return;
	}

	FNumericProperty* IndexProperty = FindFProperty<FNumericProperty>(Expression->GetClass(), TEXT("PrimitiveDataIndex"));

	if (IndexProperty == nullptr) {
		return;
	}

	void* IndexValue = IndexProperty->ContainerPtrToValuePtr<void>(Expression);

	constexpr int64 LastIndex = FCustomPrimitiveData::NumCustomPrimitiveDataFloats - 1;
	const int64 Index = IndexProperty->GetSignedIntPropertyValue(IndexValue);

	if (Index <= LastIndex) {
		return;
	}

	IndexProperty->SetIntPropertyValue(IndexValue, LastIndex);

	GLog->Log(*FString::Printf(
		TEXT("Reflection: \"%s\" reads custom primitive data %lld, which this engine stops at %lld"),
		*Expression->GetName(), Index, LastIndex
	));
#endif
}

void IMaterialGraph::SetExpressionParent(UObject* Parent, UMaterialExpression* Expression, const TSharedPtr<FJsonObject>& Json) {
	if (UMaterialFunction* MaterialFunction = Cast<UMaterialFunction>(Parent)) {
		Expression->Function = MaterialFunction;
	} else if (UMaterial* Material = Cast<UMaterial>(Parent)) {
		Expression->Material = Material;
	}

	if (Cast<UMaterialExpressionTextureBase>(Expression)) {
		const TSharedPtr<FJsonObject>* TexturePtr;
	
		if (Json->TryGetObjectField(TEXT("Texture"), TexturePtr)) {
			Expression->UpdateParameterGuid(true, false);
		}
	}
}

void IMaterialGraph::AddExpressionToParent(UObject* Parent, UMaterialExpression* Expression) {
	/* Comments are added differently */
	if (UMaterialExpressionComment* Comment = Cast<UMaterialExpressionComment>(Expression)) {
		/* From 5.1 on, we have to get the expression collection to add the comment */
#if UE5_1_BEYOND
		if (UMaterialFunction* MaterialFunction = Cast<UMaterialFunction>(Parent)) MaterialFunction->GetExpressionCollection().AddComment(Comment);
		if (UMaterial* Material = Cast<UMaterial>(Parent)) Material->GetExpressionCollection().AddComment(Comment);
#else
		/* On 5.0 and UE4, we have to get the EditorComments array to add the comment */
		if (UMaterialFunction* MaterialFunction = Cast<UMaterialFunction>(Parent)) MaterialFunction->FunctionEditorComments.Add(Comment);
		if (UMaterial* Material = Cast<UMaterial>(Parent)) Material->EditorComments.Add(Comment);
#endif
	} else {
		/* Adding expressions is different from 5.1 on */
#if UE5_1_BEYOND
		if (UMaterialFunction* MaterialFunction = Cast<UMaterialFunction>(Parent)) {
			MaterialFunction->GetExpressionCollection().AddExpression(Expression);
		}

		if (UMaterial* Material = Cast<UMaterial>(Parent)) {
			Material->GetEditorOnlyData()->ExpressionCollection.Expressions.Add(Expression);
			Expression->UpdateMaterialExpressionGuid(true, false);
			Material->AddExpressionParameter(Expression, Material->EditorParameters);
		}
#else
		if (UMaterialFunction* MaterialFunction = Cast<UMaterialFunction>(Parent)) {
			MaterialFunction->FunctionExpressions.Add(Expression);
		}

		if (UMaterial* Material = Cast<UMaterial>(Parent)) {
			Material->Expressions.Add(Expression);
			Expression->UpdateMaterialExpressionGuid(true, false);
			Material->AddExpressionParameter(Expression, Material->EditorParameters);
		}
#endif
	}
}

UMaterialExpression* IMaterialGraph::CreateEmptyExpression(FUObjectExport* Export, FUObjectExportContainer* Container) {
	const FName Type = Export->GetType();
	const FName Name = Export->GetName();

	UClass* Class = FindClassByType(Type.ToString());
	
	/* Material/MaterialFunction Parent */
	UObject* Parent = Export->Parent;

	if (!Class) {
		/* FLinkerLoad::FindNewPathNameForClass does not exist before 5.1 */
#if UE5_1_BEYOND
		TArray<FString> Redirects = TArray {
			FLinkerLoad::FindNewPathNameForClass("/Script/InterchangeImport." + Type.ToString(), false),
			FLinkerLoad::FindNewPathNameForClass("/Script/Landscape." + Type.ToString(), false)
		};
		
		for (FString RedirectedPath : Redirects) {
			if (!RedirectedPath.IsEmpty() && !Class)
				Class = FindObject<UClass>(nullptr, *RedirectedPath);
		}
#endif

		if (!Class) {
			Class = FindClassByType(Type.ToString().Replace(TEXT("MaterialExpressionPhysicalMaterialOutput"), TEXT("MaterialExpressionLandscapePhysicalMaterialOutput")));
		}
	}

	/* If a node is missing in the class, notify the user */
	if (!Class) {
		/* Convert is a fixed shuffle of components, so it is rebuilt rather than reported missing */
		if (Type == "MaterialExpressionConvert") {
			if (UMaterialExpression* Substitute = CreateConvertSubstitute(Export)) {
				return Substitute;
			}
		}

		/* A named reroute carries a wire and a name, and only the wire has to survive the port */
		if (UMaterialExpression* Substitute = CreateNamedRerouteSubstitute(Export)) {
			return Substitute;
		}

		/* Local position was a material function before it was a node, and still is one */
		if (UMaterialExpression* Substitute = CreateLocalPositionSubstitute(Export)) {
			return Substitute;
		}

		return OnMissingNodeClass(Export, Container);
	}

#if ENGINE_UE4
	/* Manually handled in UE4, as it doesn't exist */
	if (Type == "MaterialExpressionPinBase") {
		return NewObject<UMaterialExpression>(
			Parent,
			UMaterialExpressionReroute::StaticClass(),
			Name
		);
	}
#endif

	if (!Class->IsChildOf<UMaterialExpression>()) return nullptr;

	return NewObject<UMaterialExpression>
	(
		Parent,
		Class, /* Find class using ANY_PACKAGE (may error in the future) */
		Name,
		RF_Transactional
	);
}

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
 * output is registered with the property serializer, which moves each incoming connection onto the
 * root belonging to the output it asked for. */
UMaterialExpression* IMaterialGraph::CreateConvertSubstitute(FUObjectExport* Export) {
	UObject* Parent = Export->Parent;
	const TSharedPtr<FJsonObject> Properties = Export->GetProperties();

	if (Parent == nullptr || !Properties.IsValid()) {
		return nullptr;
	}

	TArray<TSharedPtr<FJsonValue>> ConvertInputs;
	TArray<TSharedPtr<FJsonValue>> ConvertOutputs;
	TArray<TSharedPtr<FJsonValue>> ConvertMappings;

	const TArray<TSharedPtr<FJsonValue>>* ArrayField;
	if (Properties->TryGetArrayField(TEXT("ConvertInputs"), ArrayField)) ConvertInputs = *ArrayField;
	if (Properties->TryGetArrayField(TEXT("ConvertOutputs"), ArrayField)) ConvertOutputs = *ArrayField;
	if (Properties->TryGetArrayField(TEXT("ConvertMappings"), ArrayField)) ConvertMappings = *ArrayField;

	/* Nothing to stand in for, so let the caller fall back to the missing node handling */
	if (ConvertOutputs.Num() == 0) {
		return nullptr;
	}

	const FString BaseName = Export->GetName().ToString();

	FString NodeName = TEXT("Convert");
	Properties->TryGetStringField(TEXT("NodeName"), NodeName);

	int32 BaseX = 0;
	int32 BaseY = 0;
	Properties->TryGetNumberField(TEXT("MaterialExpressionEditorX"), BaseX);
	Properties->TryGetNumberField(TEXT("MaterialExpressionEditorY"), BaseY);

	auto CreateNode = [&](UClass* Class, const FString& NameSuffix, const int32 X, const int32 Y) -> UMaterialExpression* {
		UMaterialExpression* Expression = NewObject<UMaterialExpression>(
			Parent,
			Class,
			MakeUniqueObjectName(Parent, Class, *(BaseName + NameSuffix)),
			RF_Transactional
		);

		Expression->MaterialExpressionEditorX = X;
		Expression->MaterialExpressionEditorY = Y;

		SetExpressionParent(Parent, Expression, Properties);
		AddExpressionToParent(Parent, Expression);

		return Expression;
	};

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
				UMaterialExpressionConstant* Constant = Cast<UMaterialExpressionConstant>(CreateNode(UMaterialExpressionConstant::StaticClass(), NameSuffix, BaseX, NodeY));
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
				UMaterialExpressionConstant* Constant = Cast<UMaterialExpressionConstant>(CreateNode(UMaterialExpressionConstant::StaticClass(), NameSuffix, BaseX, NodeY));
				Constant->R = GetLinearColorComponent(InputDefault, InputComponentIndex);

				Components.Add(Constant);
				continue;
			}

			UMaterialExpressionComponentMask* Mask = Cast<UMaterialExpressionComponentMask>(CreateNode(UMaterialExpressionComponentMask::StaticClass(), NameSuffix, BaseX, NodeY));
			Mask->R = InputComponentIndex == 0 ? 1 : 0;
			Mask->G = InputComponentIndex == 1 ? 1 : 0;
			Mask->B = InputComponentIndex == 2 ? 1 : 0;
			Mask->A = InputComponentIndex == 3 ? 1 : 0;

			PendingConvertInputs.Add(FPendingConvertInput{ Mask, ExpressionInput });
			Components.Add(Mask);
		}

		/* Chain appends to form the output's vector, exactly as the node compiles it */
		UMaterialExpression* Root = Components[0];

		for (int32 ComponentIndex = 1; ComponentIndex < Components.Num(); ComponentIndex++) {
			UMaterialExpressionAppendVector* Append = Cast<UMaterialExpressionAppendVector>(CreateNode(
				UMaterialExpressionAppendVector::StaticClass(),
				FString::Printf(TEXT("_Out%d_Append%d"), OutputIndex, ComponentIndex),
				BaseX + ComponentIndex * ConvertColumnWidth,
				RowY
			));

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
	GetPropertySerializer()->ConvertOutputRoots.Add(Roots[0], RootObjects);

	GLog->Log(*FString::Printf(TEXT("Reflection: Rebuilt Convert node \"%s\" (%s) out of masks and appends"), *NodeName, *BaseName));

	return Roots[0];
}

void IMaterialGraph::ResolveConvertSubstitutes(FUObjectExportContainer* Container) {
	for (const FPendingConvertInput& Pending : PendingConvertInputs) {
		if (Pending.Mask == nullptr || !Pending.ExpressionInput.IsValid()) {
			continue;
		}

		UMaterialExpression* Source = Container->Find<UMaterialExpression>(GetExpressionName(Pending.ExpressionInput.Get()));
		if (Source == nullptr) {
			continue;
		}

		Pending.Mask->Input = PopulateExpressionInput(Pending.ExpressionInput.Get(), Source);

		/* The source can be another rebuilt convert node, whose outputs live on separate roots */
		RemapConvertOutput(Pending.Mask->Input);
	}

	PendingConvertInputs.Empty();
}

void IMaterialGraph::RemapConvertOutput(FExpressionInput& Input) const {
	const TArray<UObject*>* Roots = GetPropertySerializer()->ConvertOutputRoots.Find(Input.Expression);

	if (Roots == nullptr || !Roots->IsValidIndex(Input.OutputIndex)) {
		return;
	}

	/* Each root has the one output, so the index the convert node was asked for is spent here */
	Input.Expression = Cast<UMaterialExpression>((*Roots)[Input.OutputIndex]);
	Input.OutputIndex = 0;
}

bool IMaterialGraph::IsConvertSubstitute(UMaterialExpression* Expression) const {
	return Expression != nullptr && GetPropertySerializer()->ConvertOutputRoots.Contains(Expression);
}

/* Local Position ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ */

static const TCHAR* LocalPositionType = TEXT("MaterialExpressionLocalPosition");

/* The function the node was made out of, which every engine this runs on already ships */
static const TCHAR* LocalPositionFunctionPath = TEXT("/Engine/Functions/Engine_MaterialFunctions02/WorldPositionOffset/LocalPosition.LocalPosition");
static const TCHAR* LocalPositionExcludingOffsetsOutput = TEXT("Local Position (Excluding Offsets)");

/* Where an output of that name sits on the node, asked of the node rather than counted on */
static int32 FindOutputIndexByName(const UMaterialExpression* Expression, const FString& Name) {
	for (int32 Index = 0; Index < Expression->Outputs.Num(); Index++) {
		if (OutputNameToString(Expression->Outputs[Index].OutputName) == Name) {
			return Index;
		}
	}

	return INDEX_NONE;
}

static void ReadEditorPosition(const TSharedPtr<FJsonObject>& Properties, int32& OutX, int32& OutY) {
	if (!Properties.IsValid()) {
		return;
	}

	Properties->TryGetNumberField(TEXT("MaterialExpressionEditorX"), OutX);
	Properties->TryGetNumberField(TEXT("MaterialExpressionEditorY"), OutY);
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
 * offsets excluded value.
 *
 * Matched on the type string, so an engine that has the class of its own never comes here. */
UMaterialExpression* IMaterialGraph::CreateLocalPositionSubstitute(FUObjectExport* Export) {
	if (Export->GetType() != FName(LocalPositionType)) {
		return nullptr;
	}

	UMaterialFunctionInterface* Function = LoadObjectByPath<UMaterialFunctionInterface>(LocalPositionFunctionPath);

	/* Without the function there is nothing to build out of, so it goes back to being reported
	 * missing like any other node this engine has no answer for */
	if (Function == nullptr) {
		return nullptr;
	}

	UObject* Parent = Export->Parent;
	const TSharedPtr<FJsonObject> Properties = Export->GetProperties();

	int32 EditorX = 0;
	int32 EditorY = 0;
	ReadEditorPosition(Properties, EditorX, EditorY);

	UMaterialExpressionMaterialFunctionCall* Call = NewObject<UMaterialExpressionMaterialFunctionCall>(
		Parent,
		UMaterialExpressionMaterialFunctionCall::StaticClass(),
		MakeUniqueObjectName(Parent, UMaterialExpressionMaterialFunctionCall::StaticClass(), *(Export->GetName().ToString() + TEXT("_Function"))),
		RF_Transactional
	);

	/* Fills in the call's inputs and outputs off the function, which is what names them */
	Call->MaterialFunction = Function;
	Call->UpdateFromFunctionResource();

	Call->MaterialExpressionEditorX = EditorX;
	Call->MaterialExpressionEditorY = EditorY;

	/* Local Origin has no equivalent on the function, so a node asking for anything but the
	 * instance position comes out reading the instance position and says so */
	FString LocalOrigin;

	if (Properties.IsValid() && Properties->TryGetStringField(TEXT("LocalOrigin"), LocalOrigin) && !LocalOrigin.Contains(TEXT("Instance"))) {
		GLog->Log(*FString::Printf(TEXT("Reflection: \"%s\" wanted local position origin %s, which the engine function has no output for"), *Export->GetName().ToString(), *LocalOrigin));
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
		GLog->Log(*FString::Printf(TEXT("Reflection: the engine's LocalPosition function has no \"%s\" output, so \"%s\" reads the offsets included one"), LocalPositionExcludingOffsetsOutput, *Export->GetName().ToString()));

		return Call;
	}

	/* The call is the extra node here, so it is parented on the way past. The reroute handed back
	 * is parented by the caller like any other expression an export resolves to. */
	AddExpressionToParent(Parent, Call);

	/* Sat to the left of where the node was, leaving the reroute to land on the original spot when
	 * the export's own editor position is read onto it */
	Call->MaterialExpressionEditorX = EditorX - 200;

	UMaterialExpressionReroute* Reroute = NewObject<UMaterialExpressionReroute>(
		Parent,
		UMaterialExpressionReroute::StaticClass(),
		Export->GetName(),
		RF_Transactional
	);

	Reroute->Input.Expression = Call;
	Reroute->Input.OutputIndex = ExcludingOffsetsOutput;
	Reroute->Desc = LocalPositionExcludingOffsetsOutput;

	return Reroute;
}

/* Named Reroutes ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ */

static const TCHAR* NamedRerouteDeclarationType = TEXT("MaterialExpressionNamedRerouteDeclaration");
static const TCHAR* NamedRerouteUsageType = TEXT("MaterialExpressionNamedRerouteUsage");

/* The name a declaration goes by in the graph, which is what a usage is reading when it names it */
static FString GetNamedRerouteName(const TSharedPtr<FJsonObject>& Properties) {
	FString Name;

	if (Properties.IsValid()) {
		Properties->TryGetStringField(TEXT("Name"), Name);
	}

	return Name;
}

/* A usage and its declaration are paired by guid rather than by a wire, so DeclarationGuid against
 * the declaration's VariableGuid is the link that is still there when the object reference is not. */
static UMaterialExpression* FindNamedRerouteDeclarationByGuid(FUObjectExportContainer* Container, const TSharedPtr<FJsonObject>& UsageProperties) {
	FString DeclarationGuid;

	if (!UsageProperties->TryGetStringField(TEXT("DeclarationGuid"), DeclarationGuid) || DeclarationGuid.IsEmpty()) {
		return nullptr;
	}

	for (FUObjectExport* Export : Container->Exports) {
		if (Export->GetType() != FName(NamedRerouteDeclarationType)) {
			continue;
		}

		const TSharedPtr<FJsonObject> Properties = Export->GetProperties();
		FString VariableGuid;

		if (Properties.IsValid() && Properties->TryGetStringField(TEXT("VariableGuid"), VariableGuid) && VariableGuid == DeclarationGuid) {
			return Export->Get<UMaterialExpression>();
		}
	}

	return nullptr;
}

/* Named reroutes arrived in 5.0. A declaration is a reroute with a name on it, and a usage is a
 * read of that name from anywhere else in the graph, the two joined by a guid instead of a wire.
 * Both collapse onto the plain reroute, which every engine has: what a named reroute adds over one
 * is a way to carry a wire across the graph without drawing it, and that is presentation.
 *
 * The declaration keeps its own connection for free. Its input is spelled Input on both classes,
 * so the property serializer wires it without being told. The usage has no input to fill in, only
 * a declaration to find, which is what ResolveNamedRerouteUsages does once the graph is built.
 *
 * Matched on the type string rather than gated on a version: an engine either has the class or it
 * does not, and the ones that do never reach here. */
UMaterialExpression* IMaterialGraph::CreateNamedRerouteSubstitute(FUObjectExport* Export) {
	const FName Type = Export->GetType();

	const bool bIsDeclaration = Type == FName(NamedRerouteDeclarationType);
	const bool bIsUsage = Type == FName(NamedRerouteUsageType);

	if (!bIsDeclaration && !bIsUsage) {
		return nullptr;
	}

	const TSharedPtr<FJsonObject> Properties = Export->GetProperties();

	UMaterialExpressionReroute* Reroute = NewObject<UMaterialExpressionReroute>(
		Export->Parent,
		UMaterialExpressionReroute::StaticClass(),
		Export->GetName(),
		RF_Transactional
	);

	if (bIsDeclaration) {
		/* Nothing else carries the name once the class is gone, and a graph full of anonymous
		 * reroutes is not one anybody can read */
		const FString Name = GetNamedRerouteName(Properties);

		if (!Name.IsEmpty()) {
			Reroute->Desc = Name;
		}
	} else {
		/* The declaration may not have an object yet, so the wire waits for the whole container */
		PendingNamedRerouteUsages.Add({ Reroute, Properties });
	}

	GLog->Log(*FString::Printf(TEXT("Reflection: Rebuilt %s \"%s\" as a Reroute"), *Type.ToString(), *Export->GetName().ToString()));

	return Reroute;
}

void IMaterialGraph::ResolveNamedRerouteUsages(FUObjectExportContainer* Container) {
	for (const FPendingNamedRerouteUsage& Pending : PendingNamedRerouteUsages) {
		if (Pending.Usage == nullptr || !Pending.Properties.IsValid()) {
			continue;
		}

		UMaterialExpression* Declaration = nullptr;

		/* The reference to the declaration, for the exports that still carry one */
		const TSharedPtr<FJsonObject>* DeclarationObject;

		if (Pending.Properties->TryGetObjectField(TEXT("Declaration"), DeclarationObject)) {
			FString ObjectName;

			if ((*DeclarationObject)->TryGetStringField(TEXT("ObjectName"), ObjectName)) {
				Declaration = Container->Find<UMaterialExpression>(GetExportNameOfSubobject(ObjectName));
			}
		}

		/* Otherwise the guid, which is the link the pair is really kept by */
		if (Declaration == nullptr) {
			Declaration = FindNamedRerouteDeclarationByGuid(Container, Pending.Properties);
		}

		/* A usage whose declaration is nowhere in this graph is left unconnected, which is the
		 * same thing an unresolved reference leaves behind everywhere else */
		if (Declaration == nullptr) {
			GLog->Log(*FString::Printf(TEXT("Reflection: Named reroute usage \"%s\" names a declaration this graph does not have"), *Pending.Usage->GetName()));

			continue;
		}

		Pending.Usage->Input.Expression = Declaration;
		Pending.Usage->Input.OutputIndex = 0;

		/* Reads the same name it points at, so both ends of the pair say so */
		Pending.Usage->Desc = Declaration->Desc;
	}

	PendingNamedRerouteUsages.Empty();
}

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

/* ReSharper disable once CppMemberFunctionMayBeConst */
UMaterialExpression* IMaterialGraph::OnMissingNodeClass(FUObjectExport* Export, FUObjectExportContainer* Container) {
	/* Get variables from the export data */
	const FName Name = Export->GetName();
	FName Type = Export->GetType();

	/* Material/MaterialFunction Parent */
	UObject* Parent = Export->Parent;

	/* Get Json Objects from Export */
	const TSharedPtr<FJsonObject> Properties = Export->GetProperties();

#if ENGINE_UE4
	/* In Unreal Engine 4, to combat the absence of Sub-graphs, create a Material Function in place of it */
	if (Type == "MaterialExpressionComposite") {
		return nullptr;
	}
#endif

	/* Add a comment in the graph notifying the user of a missing node ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ */
	UMaterialExpressionComment* Comment = NewObject<UMaterialExpressionComment>(Parent, UMaterialExpressionComment::StaticClass(), *("UMaterialExpressionComment_" + Type.ToString()), RF_Transactional);

	Comment->Text = *("Missing Node: " + Type.ToString());
	Comment->CommentColor = FLinearColor(1.0, 0.0, 0.0);
	Comment->bCommentBubbleVisible = true;
	Comment->SizeX = 415;
	Comment->SizeY = 40;

	Comment->Desc = "A node is missing from your Unreal Engine build. This can occur for several reasons, but it is most likely because your version of Unreal Engine is older than the one you are porting from.";

	GetObjectSerializer()->DeserializeObjectProperties(Properties, Comment);
	AddExpressionToParent(Parent, Comment);

	/* Report the missing node ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ */
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
		Name
	);

	/* A switch picks one of its branches, and which one is a decision this engine has no class to
	 * make, so the first branch is carried through the reroute. A switch dropped outright takes
	 * everything hanging off it with it, where the first branch is the one the node itself reads
	 * when nothing tells it otherwise, and leaves a graph that still resolves. The comment above
	 * stays either way, so the node is still reported missing rather than quietly stood in for. */
	if (Type.ToString().Contains(TEXT("Switch"))) {
		if (const TSharedPtr<FJsonObject> FirstInput = FindFirstExpressionInput(Properties)) {
			PendingSwitchPassthroughs.Add({ Reroute, FirstInput });
		}
	}

	return Reroute;
}

void IMaterialGraph::ResolveSwitchPassthroughs(FUObjectExportContainer* Container) {
	for (const FPendingSwitchPassthrough& Pending : PendingSwitchPassthroughs) {
		if (Pending.Reroute == nullptr || !Pending.ExpressionInput.IsValid()) {
			continue;
		}

		UMaterialExpression* Source = Container->Find<UMaterialExpression>(GetExpressionName(Pending.ExpressionInput.Get()));

		if (Source == nullptr) {
			continue;
		}

		Pending.Reroute->Input = PopulateExpressionInput(Pending.ExpressionInput.Get(), Source);

		/* The branch can come off a rebuilt convert node, whose outputs live on separate roots */
		RemapConvertOutput(Pending.Reroute->Input);
	}

	PendingSwitchPassthroughs.Empty();
}

void IMaterialGraph::ReportMaterialDataMissing() const {
	FImportIssues::Report(
		EImportIssue::Data,
		TEXT("The export carries no material data"),
		TEXT("Nothing was there to build a graph from - see the requirements for Materials on GitHub.")
	);
}

void IMaterialGraph::ReportNullExpressions() const {
	FImportIssues::Report(
		EImportIssue::Data,
		TEXT("The export's expression list has holes in it"),
		TEXT("Some of the nodes this material is built from were not in the export, so there is no graph here to rebuild.")
	);
}

void IMaterialGraph::ReportCreatedStubs() const {
	FImportIssues::Report(
		EImportIssue::Data,
		TEXT("Built from stubs"),
		TEXT("The export carries no material data, so the parameters were stubbed out and the graph is empty.")
	);
}

FExpressionInput IMaterialGraph::PopulateExpressionInput(const FJsonObject* JsonProperties, UMaterialExpression* Expression, const FString& Type) {
	FExpressionInput Input;
	Input.Expression = Expression;

	/* Each Mask input/output */
	int OutputIndex;
	if (JsonProperties->TryGetNumberField(TEXT("OutputIndex"), OutputIndex)) Input.OutputIndex = OutputIndex;
	FString InputName;
	if (JsonProperties->TryGetStringField(TEXT("InputName"), InputName)) Input.InputName = StringToName(InputName);
	int Mask;
	if (JsonProperties->TryGetNumberField(TEXT("Mask"), Mask)) Input.Mask = Mask;
	int MaskR;
	if (JsonProperties->TryGetNumberField(TEXT("MaskR"), MaskR)) Input.MaskR = MaskR;
	int MaskG;
	if (JsonProperties->TryGetNumberField(TEXT("MaskG"), MaskG)) Input.MaskG = MaskG;
	int MaskB;
	if (JsonProperties->TryGetNumberField(TEXT("MaskB"), MaskB)) Input.MaskB = MaskB;
	int MaskA;
	if (JsonProperties->TryGetNumberField(TEXT("MaskA"), MaskA)) Input.MaskA = MaskA;

	if (Type == "Color") {
		if (FColorMaterialInput* ColorInput = reinterpret_cast<FColorMaterialInput*>(&Input)) {
			bool UseConstant;
			if (JsonProperties->TryGetBoolField(TEXT("UseConstant"), UseConstant)) ColorInput->UseConstant = UseConstant;
			const TSharedPtr<FJsonObject>* Constant;
			if (JsonProperties->TryGetObjectField(TEXT("Constant"), Constant)) ColorInput->Constant = ObjectToLinearColor(Constant->Get()).ToFColor(true);
			Input = FExpressionInput(*ColorInput);
		}
	} else if (Type == "Scalar") {
		if (FScalarMaterialInput* ScalarInput = reinterpret_cast<FScalarMaterialInput*>(&Input)) {
			bool UseConstant;
			if (JsonProperties->TryGetBoolField(TEXT("UseConstant"), UseConstant)) ScalarInput->UseConstant = UseConstant;
#if ENGINE_UE5
			float Constant;
#else
			double Constant;
#endif
			if (JsonProperties->TryGetNumberField(TEXT("Constant"), Constant)) ScalarInput->Constant = Constant;
			Input = FExpressionInput(*ScalarInput);
		}
	} else if (Type == "Vector") {
		if (FVectorMaterialInput* VectorInput = reinterpret_cast<FVectorMaterialInput*>(&Input)) {
			bool UseConstant;
			if (JsonProperties->TryGetBoolField(TEXT("UseConstant"), UseConstant)) VectorInput->UseConstant = UseConstant;
			const TSharedPtr<FJsonObject>* Constant;
			if (JsonProperties->TryGetObjectField(TEXT("Constant"), Constant)) VectorInput->Constant = ObjectToVector3F(Constant->Get());
			Input = FExpressionInput(*VectorInput);
		}
	}

	return Input;
}

FName IMaterialGraph::GetExpressionName(const FJsonObject* JsonProperties, const FString& OverrideParameterName) {
	const TSharedPtr<FJsonValue> ExpressionField = JsonProperties->TryGetField(OverrideParameterName);

	if (ExpressionField == nullptr || ExpressionField->IsNull()) {
		/* Must be from < 4.25 */
		return StringToName(JsonProperties->GetStringField(TEXT("ExpressionName")));
	}

	const TSharedPtr<FJsonObject> ExpressionObject = ExpressionField->AsObject();
	FString ObjectName;
	
	if (ExpressionObject->TryGetStringField(TEXT("ObjectName"), ObjectName)) {
		return GetExportNameOfSubobject(ObjectName);
	}

	return NAME_None;
}