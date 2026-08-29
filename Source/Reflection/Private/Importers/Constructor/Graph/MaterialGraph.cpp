/* Copyright Reflection Contributors 2024-2026 */

#include "Importers/Constructor/Graph/MaterialGraph.h"

#include "Importers/Constructor/Graph/MaterialNodeSubstitute.h"

/* Expressions */
#include "Materials/MaterialExpressionComment.h"
#include "Materials/MaterialExpressionReroute.h"
#include "Materials/MaterialExpressionGetMaterialAttributes.h"
#include "Materials/MaterialExpressionSetMaterialAttributes.h"
/* FCustomPrimitiveData, for how many data floats this engine gives a primitive */
#include "SceneTypes.h"
#include "Engine/EngineUtilities.h"
#include "Importers/Constructor/ImportIssues.h"
#include "Utilities/JsonHelpers.h"

#if ENGINE_UE5
#include "Materials/MaterialExpressionTextureBase.h"
#endif

/* Where the attribute definitions live: 5.2 split them out of MaterialShared.h into a header of
 * their own, and every version before that keeps them there */
#if UE5_2_BEYOND
#include "Materials/MaterialAttributeDefinitionMap.h"
#else
#include "MaterialShared.h"
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
	FMaterialSubstituteContext Context(*this, Container);

	/* Go through each expression, and create the expression */
	for (FUObjectExport* Export : Container->Exports) {
		/* Invalid Json Object */
		if (!Export->JsonObject.IsValid()) {
			continue;
		}

		UObject* Expression = Export->Object;

		if (Expression == nullptr) {
			Expression = CreateEmptyExpression(Export, Context);
		}

		/* If nullptr, expression isn't valid */
		if (Expression == nullptr) {
			continue;
		}

		Export->Object = Expression;
	}

	/* Every export has an object now, which is what the substitutes were waiting on */
	Context.ResolveDeferred();
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

	const FNumericProperty* IndexProperty = FindFProperty<FNumericProperty>(Expression->GetClass(), TEXT("PrimitiveDataIndex"));

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

/* Material attributes ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ */

bool HasMaterialEditorOnlyData(const UObject* Object) {
	if (Object == nullptr) return false;

	const FObjectProperty* Held = FindFProperty<FObjectProperty>(Object->GetClass(), TEXT("EditorOnlyData"));

	/* Nothing to ask about before the split, where the expressions are on the material itself */
	return Held == nullptr || Held->GetObjectPropertyValue_InContainer(Object) != nullptr;
}

namespace {
	/* The map answers for an id it holds no definition for with MP_MAX, the entry it keeps under
	 * the name "Missing", which is what the pin ends up reading in the material editor */
	bool IsMissingMaterialAttribute(const FGuid& AttributeID) {
		return !AttributeID.IsValid() || FMaterialAttributeDefinitionMap::GetProperty(AttributeID) == MP_MAX;
	}

	/* One connection, reached through properties rather than by casting: an expression connects
	 * through FExpressionInput and a material's own inputs through FColorMaterialInput and friends,
	 * and which of those share a reflected base changes by version. All of them carry these two. */
	struct FGraphConnection {
		FObjectProperty* ExpressionProperty = nullptr;
		FIntProperty* OutputIndexProperty = nullptr;
		void* StructValue = nullptr;

		bool Bind(const UStruct* Struct) {
			ExpressionProperty = FindFProperty<FObjectProperty>(Struct, TEXT("Expression"));
			OutputIndexProperty = FindFProperty<FIntProperty>(Struct, TEXT("OutputIndex"));

			return ExpressionProperty != nullptr && OutputIndexProperty != nullptr;
		}

		UObject* GetExpression() const {
			return ExpressionProperty->GetObjectPropertyValue(ExpressionProperty->ContainerPtrToValuePtr<void>(StructValue));
		}

		int32 GetOutputIndex() const {
			return *OutputIndexProperty->ContainerPtrToValuePtr<int32>(StructValue);
		}

		void SetOutputIndex(const int32 OutputIndex) const {
			*OutputIndexProperty->ContainerPtrToValuePtr<int32>(StructValue) = OutputIndex;
		}

		void Disconnect() const {
			ExpressionProperty->SetObjectPropertyValue(ExpressionProperty->ContainerPtrToValuePtr<void>(StructValue), nullptr);
			SetOutputIndex(0);
		}
	};

	void ForEachConnectionInStruct(const UScriptStruct* Struct, void* StructValue, TFunctionRef<void(const FGraphConnection&)> Visit);

	/* Every connection reached from a property of Struct, which is a class where the walk starts on
	 * an object and a struct where it has gone into one. Covers a connection kept one to a property
	 * (a node's inputs), in a fixed-size array (CustomizedUVs) and in a dynamic one (a set
	 * attributes node) alike. */
	void ForEachConnectionInProperties(const UStruct* Struct, void* Container, TFunctionRef<void(const FGraphConnection&)> Visit) {
		for (TFieldIterator<FProperty> PropertyIt(Struct); PropertyIt; ++PropertyIt) {
			FProperty* Property = *PropertyIt;

			if (FStructProperty* StructProperty = CastField<FStructProperty>(Property)) {
				for (int32 Index = 0; Index < StructProperty->ArrayDim; Index++) {
					ForEachConnectionInStruct(StructProperty->Struct, StructProperty->ContainerPtrToValuePtr<void>(Container, Index), Visit);
				}

				continue;
			}

			FArrayProperty* ArrayProperty = CastField<FArrayProperty>(Property);
			FStructProperty* InnerProperty = ArrayProperty != nullptr ? CastField<FStructProperty>(ArrayProperty->Inner) : nullptr;

			if (InnerProperty == nullptr) {
				continue;
			}

			FScriptArrayHelper ArrayHelper(ArrayProperty, ArrayProperty->ContainerPtrToValuePtr<void>(Container));

			for (int32 Index = 0; Index < ArrayHelper.Num(); Index++) {
				ForEachConnectionInStruct(InnerProperty->Struct, ArrayHelper.GetRawPtr(Index), Visit);
			}
		}
	}

	/* Deeper than the properties of the object itself, as a function call keeps the connection for
	 * each of the function's inputs nested inside a struct of its own */
	void ForEachConnectionInStruct(const UScriptStruct* Struct, void* StructValue, TFunctionRef<void(const FGraphConnection&)> Visit) {
		FGraphConnection Connection;

		/* A connection is where the walk stops, nothing below one being another */
		if (Connection.Bind(Struct)) {
			Connection.StructValue = StructValue;

			Visit(Connection);

			return;
		}

		ForEachConnectionInProperties(Struct, StructValue, Visit);
	}

	void ForEachConnection(UObject* Object, TFunctionRef<void(const FGraphConnection&)> Visit) {
		if (Object == nullptr) {
			return;
		}

		ForEachConnectionInProperties(Object->GetClass(), Object, Visit);
	}

	/* Everything the graph ended up with, which is more than the export container holds: a node a
	 * substitute stood in for was rebuilt out of several, and only the one it returned is an export */
	void ForEachExpression(UObject* Parent, TFunctionRef<void(UMaterialExpression*)> Visit) {
#if UE5_1_BEYOND
		if (UMaterialFunction* MaterialFunction = Cast<UMaterialFunction>(Parent); MaterialFunction != nullptr && HasMaterialEditorOnlyData(MaterialFunction)) {
			for (UMaterialExpression* Expression : MaterialFunction->GetExpressionCollection().Expressions) Visit(Expression);
		}

		if (UMaterial* Material = Cast<UMaterial>(Parent); Material != nullptr && HasMaterialEditorOnlyData(Material)) {
			for (UMaterialExpression* Expression : Material->GetEditorOnlyData()->ExpressionCollection.Expressions) Visit(Expression);
		}
#else
		if (UMaterialFunction* MaterialFunction = Cast<UMaterialFunction>(Parent)) {
			for (UMaterialExpression* Expression : MaterialFunction->FunctionExpressions) Visit(Expression);
		}

		if (UMaterial* Material = Cast<UMaterial>(Parent)) {
			for (UMaterialExpression* Expression : Material->Expressions) Visit(Expression);
		}
#endif
	}

	/* Inputs[0] is the attributes the node starts from, so the pin for AttributeSetTypes[Index] is
	 * Inputs[Index + 1]. Whatever fed a removed pin is left unconnected, which is what the node
	 * would have looked like had the attribute never been set. */
	void PruneSetMaterialAttributes(UMaterialExpressionSetMaterialAttributes* Set) {
		for (int32 Index = Set->AttributeSetTypes.Num() - 1; Index >= 0; Index--) {
			if (!IsMissingMaterialAttribute(Set->AttributeSetTypes[Index])) {
				continue;
			}

			FString PinName = Set->AttributeSetTypes[Index].ToString(EGuidFormats::Digits);

			if (Set->Inputs.IsValidIndex(Index + 1)) {
				/* The name the engine that wrote the node gave the attribute, which is the only
				 * thing left that says what it was */
				if (!Set->Inputs[Index + 1].InputName.IsNone()) {
					PinName = Set->Inputs[Index + 1].InputName.ToString();
				}

				Set->Inputs.RemoveAt(Index + 1);
			}

			Set->AttributeSetTypes.RemoveAt(Index);

			GLog->Log(*FString::Printf(
				TEXT("Reflection: \"%s\" sets \"%s\", which this engine has no material attribute for, so the pin was removed"),
				*Set->GetName(), *PinName
			));
		}
	}

	/* The same the other way round, with Outputs[0] being the attributes passed through. Fills the
	 * remap with where each output index ends up, INDEX_NONE for the ones that go. */
	bool PruneGetMaterialAttributes(UMaterialExpressionGetMaterialAttributes* Get, TArray<int32>& OutRemap) {
		TArray<int32> Missing;

		for (int32 Index = 0; Index < Get->AttributeGetTypes.Num(); Index++) {
			if (IsMissingMaterialAttribute(Get->AttributeGetTypes[Index])) {
				Missing.Add(Index);
			}
		}

		if (Missing.Num() == 0) {
			return false;
		}

		/* Worked out before anything moves, as the indices below are the ones the graph was wired with */
		OutRemap.Init(INDEX_NONE, Get->AttributeGetTypes.Num() + 1);

		int32 NewIndex = 0;

		for (int32 OutputIndex = 0; OutputIndex < OutRemap.Num(); OutputIndex++) {
			if (OutputIndex > 0 && Missing.Contains(OutputIndex - 1)) {
				continue;
			}

			OutRemap[OutputIndex] = NewIndex++;
		}

		for (int32 Index = Missing.Num() - 1; Index >= 0; Index--) {
			const int32 Attribute = Missing[Index];

			FString PinName = Get->AttributeGetTypes[Attribute].ToString(EGuidFormats::Digits);

			if (Get->Outputs.IsValidIndex(Attribute + 1)) {
				const FString OutputName = OutputNameToString(Get->Outputs[Attribute + 1].OutputName);

				if (!OutputName.IsEmpty()) {
					PinName = OutputName;
				}

				Get->Outputs.RemoveAt(Attribute + 1);
			}

			Get->AttributeGetTypes.RemoveAt(Attribute);

			GLog->Log(*FString::Printf(
				TEXT("Reflection: \"%s\" gets \"%s\", which this engine has no material attribute for, so the pin was removed"),
				*Get->GetName(), *PinName
			));
		}

		return true;
	}
}

/* A get or set material attributes node names each of its pins by the id of the attribute it
 * reads or writes, and an export from a newer engine carries ids for attributes this one never
 * had. The engine has nothing to resolve those to: the pin comes out named "Missing", and the
 * material stops compiling on "Property type doesn't exist, needs re-mapping?" the moment it is
 * opened. There is no attribute here to point the pin at instead, so it comes off the node.
 *
 * Run once the graph is wired rather than as each node is read, because dropping an output of a
 * get node moves every output after it, and the inputs reading them have to move with it. */
void IMaterialGraph::PruneMissingMaterialAttributes(UObject* Parent, UObject* MaterialData) {
	TMap<UObject*, TArray<int32>> OutputRemaps;

	/* An input on a rebuilt node reaches the same expression, so an object is only ever walked once */
	TSet<UObject*> Objects;

	Objects.Add(Parent);
	Objects.Add(MaterialData);
	Objects.Remove(nullptr);

	ForEachExpression(Parent, [&OutputRemaps, &Objects](UMaterialExpression* Expression) {
		if (Expression == nullptr) {
			return;
		}

		Objects.Add(Expression);

		if (UMaterialExpressionSetMaterialAttributes* Set = Cast<UMaterialExpressionSetMaterialAttributes>(Expression)) {
			PruneSetMaterialAttributes(Set);

			return;
		}

		if (UMaterialExpressionGetMaterialAttributes* Get = Cast<UMaterialExpressionGetMaterialAttributes>(Expression)) {
			TArray<int32> Remap;

			if (PruneGetMaterialAttributes(Get, Remap)) {
				OutputRemaps.Add(Get, MoveTemp(Remap));
			}
		}
	});

	if (OutputRemaps.Num() == 0) {
		return;
	}

	for (UObject* Object : Objects) {
		ForEachConnection(Object, [&OutputRemaps](const FGraphConnection& Connection) {
			const TArray<int32>* Remap = OutputRemaps.Find(Connection.GetExpression());

			if (Remap == nullptr || !Remap->IsValidIndex(Connection.GetOutputIndex())) {
				return;
			}

			const int32 OutputIndex = (*Remap)[Connection.GetOutputIndex()];

			/* Read the pin that went. Nothing here answers for the attribute it wanted, so the wire
			 * comes off and the input falls back to whatever the node it sits on defaults to. */
			if (OutputIndex == INDEX_NONE) {
				Connection.Disconnect();

				return;
			}

			Connection.SetOutputIndex(OutputIndex);
		});
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

UMaterialExpression* IMaterialGraph::CreateEmptyExpression(FUObjectExport* Export, FMaterialSubstituteContext& Context) {
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

	/* No class for it, so the substitutes get a go */
	if (!Class) {
		Context.SetExport(Export);

		return CreateSubstituteExpression(Context);
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

/* First substitute to build something answers for the node */
UMaterialExpression* IMaterialGraph::CreateSubstituteExpression(FMaterialSubstituteContext& Context) {
	const FName Type = Context.GetType();

	for (const TSharedRef<FMaterialNodeSubstitute>& Substitute : FMaterialNodeSubstituteRegistry::Get().GetSubstitutes()) {
		if (!Substitute->Handles(Type)) {
			continue;
		}

		if (UMaterialExpression* Expression = Substitute->Create(Context)) {
			return Expression;
		}
	}

	/* The fallback answers for everything, so this means it never registered */
	GLog->Log(*FString::Printf(TEXT("Reflection: nothing stood in for \"%s\" and nothing reported it"), *Type.ToString()));

	return nullptr;
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