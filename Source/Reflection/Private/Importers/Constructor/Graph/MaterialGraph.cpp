/* Copyright Reflection Contributors 2024-2026 */

#include "Importers/Constructor/Graph/MaterialGraph.h"

/* Expressions */
#include "Materials/MaterialExpressionAppendVector.h"
#include "Materials/MaterialExpressionComment.h"
#include "Materials/MaterialExpressionComponentMask.h"
#include "Materials/MaterialExpressionConstant.h"
#include "Materials/MaterialExpressionReroute.h"
#include "Engine/EngineUtilities.h"
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

	/* Every export has an object now, which is what the convert substitutes were waiting on */
	ResolveConvertSubstitutes(Container);
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

		bool AddToParentExpression = true;
		
		/* Sub-graph (natively only on Unreal Engine 5) */
		if (Properties->HasField(TEXT("SubgraphExpression"))) {
			TSharedPtr<FJsonObject> SubGraphExpressionObject = Properties->GetObjectField(TEXT("SubgraphExpression"));

			FName SubGraphExpressionName = GetExportNameOfSubobject(SubGraphExpressionObject->GetStringField(TEXT("ObjectName")));
			FUObjectExport* SubGraphExport = Container->Find(SubGraphExpressionName);

#if ENGINE_UE5
			UMaterialExpression* SubGraphExpression = SubGraphExport->Get<UMaterialExpression>();

			/* SubgraphExpression is only on Unreal Engine 5 */
			Expression->SubgraphExpression = SubGraphExpression;
#else

			/* Not implemented yet */
			continue;
			
			/* Add it to the subgraph function ~ UE4 ONLY */
			UMaterialFunction* ParentSubgraphFunction = SubgraphFunctions[SubGraphExpressionName];

			Export->Parent = ParentSubgraphFunction;
			Expression = CreateEmptyExpression(Export, Container);

			Expression->Function = ParentSubgraphFunction;
			ParentSubgraphFunction->FunctionExpressions.Add(Expression);

			AddToParentExpression = false;
#endif
		}

		GetObjectSerializer()->DeserializeObjectProperties(Properties, Expression);
		SetExpressionParent(Parent, Expression, Properties);

		if (AddToParentExpression) {
			AddExpressionToParent(Parent, Expression);
		}
	}
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

/*
 * MaterialExpressionConvert arrived in 5.6. All it does is shuffle input components into output
 * components, and UMaterialExpressionConvert::Compile emits exactly that shuffle: a component mask
 * per mapped component, a constant for every output component no mapping writes to, and a chain of
 * appends joining them into the output's vector. Older engines get the same graph spelled out with
 * those three nodes, which have existed the whole time.
 *
 * The node has several outputs and the nodes replacing it have one each, so the root of every
 * output is registered with the property serializer, which moves each incoming connection onto the
 * root belonging to the output it asked for.
 */
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

	Comment->Text = *("Missing Node Class " + Type.ToString());
	Comment->CommentColor = FLinearColor(1.0, 0.0, 0.0);
	Comment->bCommentBubbleVisible = true;
	Comment->SizeX = 415;
	Comment->SizeY = 40;

	Comment->Desc = "A node is missing from your Unreal Engine build. This can occur for several reasons, but it is most likely because your version of Unreal Engine is older than the one you are porting from.";

	GetObjectSerializer()->DeserializeObjectProperties(Properties, Comment);
	AddExpressionToParent(Parent, Comment);

	/* Add a notification letting the user know of a missing node ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ */
	GLog->Log(*("Reflection: Missing Node " + Type.ToString() + " in Parent " + Parent->GetName()));
	AppendNotification(
		FText::FromString("Missing Node (" + Parent->GetName() + ")"),
		FText::FromString(Type.ToString()),
		8.0f,
		SNotificationItem::ECompletionState::CS_Fail,
		true,
		456.0
	);

	/* Put a reroute in place of the missing node */
	return NewObject<UMaterialExpression>(
		Parent,
		UMaterialExpressionReroute::StaticClass(),
		Name
	);
}

void IMaterialGraph::SpawnMaterialDataMissingNotification() const {
	FNotificationInfo Info = FNotificationInfo(FText::FromString("Empty Material (" + GetAssetName() + ")"));
	Info.ExpireDuration = 7.0f;
	Info.bUseLargeFont = true;
	Info.bUseSuccessFailIcons = true;
	Info.WidthOverride = FOptionalSize(350);
	SetNotificationSubText(Info, FText::FromString(FString("Please see the requirements for Materials on GitHub")));

	const TSharedPtr<SNotificationItem> NotificationPtr = FSlateNotificationManager::Get().AddNotification(Info);
	NotificationPtr->SetCompletionState(SNotificationItem::CS_Fail);
}

void IMaterialGraph::CreatedStubsNotification() const {
	FNotificationInfo Info = FNotificationInfo(FText::FromString("Created Stubs for " + GetAssetName()));
	Info.ExpireDuration = 7.0f;
	Info.bUseLargeFont = true;
	Info.bUseSuccessFailIcons = true;
	Info.WidthOverride = FOptionalSize(350);

	const TSharedPtr<SNotificationItem> NotificationPtr = FSlateNotificationManager::Get().AddNotification(Info);
	NotificationPtr->SetCompletionState(SNotificationItem::CS_Fail);
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