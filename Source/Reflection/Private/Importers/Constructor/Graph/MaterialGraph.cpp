/* Copyright Reflection Contributors 2024-2026 */

#include "Importers/Constructor/Graph/MaterialGraph.h"

/* Expressions */
#include "Materials/MaterialExpressionComment.h"
#include "Materials/MaterialExpressionReroute.h"
#include "Engine/EngineUtilities.h"
#include "Utilities/JsonUtilities.h"

#if ENGINE_UE5
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
		/* In Unreal Engine 5, we have to get the expression collection to add the comment */
#if ENGINE_UE5
		if (UMaterialFunction* MaterialFunction = Cast<UMaterialFunction>(Parent)) MaterialFunction->GetExpressionCollection().AddComment(Comment);
		if (UMaterial* Material = Cast<UMaterial>(Parent)) Material->GetExpressionCollection().AddComment(Comment);
#else
		/* In Unreal Engine 4, we have to get the EditorComments array to add the comment */
		if (UMaterialFunction* MaterialFunction = Cast<UMaterialFunction>(Parent)) MaterialFunction->FunctionEditorComments.Add(Comment);
		if (UMaterial* Material = Cast<UMaterial>(Parent)) Material->EditorComments.Add(Comment);
#endif
	} else {
		/* Adding expressions is different between UE4 and UE5 */
#if ENGINE_UE5
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
#if ENGINE_UE5
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
	if (JsonProperties->TryGetStringField(TEXT("InputName"), InputName)) Input.InputName = FName(InputName);
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
		return FName(JsonProperties->GetStringField(TEXT("ExpressionName")));
	}

	const TSharedPtr<FJsonObject> ExpressionObject = ExpressionField->AsObject();
	FString ObjectName;
	
	if (ExpressionObject->TryGetStringField(TEXT("ObjectName"), ObjectName)) {
		return GetExportNameOfSubobject(ObjectName);
	}

	return NAME_None;
}