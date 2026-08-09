/* Copyright Reflection Contributors 2024-2026 */

#include "Importers/Types/Materials/MaterialImporter.h"
#include "Importers/Types/Materials/MaterialStubs.h"

/* Include Material.h (depends on UE Version) */
#if (ENGINE_UE5 && ENGINE_MINOR_VERSION < 3) || ENGINE_UE4
#include "Materials/Material.h"
#else
#include "MaterialDomain.h"
#endif

/* Cached expression data, and the header holding it, only exist from 4.25 on */
#if !UE4_24_BELOW
#include "MaterialCachedData.h"
#endif
#include "Factories/MaterialFactoryNew.h"
#include "Settings/ReflectionSettings.h"

#if ENGINE_UE5
class UMaterialAccessor final : public UMaterial {
public:
	FMaterialCachedExpressionData* GetCachedExpressionDataRef() const {
		return CachedExpressionData.Get();
	}
};
#endif

UObject* IMaterialImporter::CreateAsset(UObject* CreatedAsset) {
	/* Create Material Factory (factory automatically creates the Material) */
	UMaterialFactoryNew* MaterialFactory = NewObject<UMaterialFactoryNew>();
	UMaterial* Material = Cast<UMaterial>(MaterialFactory->FactoryCreateNew(UMaterial::StaticClass(), GetPackage(), *GetAssetName(), RF_Standalone | RF_Public, nullptr, GWarn));

	return IImporter::CreateAsset(Material);
}

bool IMaterialImporter::Import() {
	UMaterial* Material = Create<UMaterial>();

	/* Clear any default expressions the engine adds */
#if UE5_1_BEYOND
	Material->GetExpressionCollection().Empty();
#else
	Material->Expressions.Empty();
#endif

	/* Define material data from the JSON */
	FUObjectExportContainer* ExpressionContainer = new FUObjectExportContainer();
	TSharedPtr<FJsonObject> Props = FindMaterialData(GetAssetType(), ExpressionContainer);

	/* Whatever would be built out of a list with holes in it is not this material */
	if (HasNullExpressions(Props)) {
		ReportNullExpressions();

		return false;
	}

	/* Map out each expression for easier access */
	ConstructExpressions(ExpressionContainer);

	const UReflectionSettings* Settings = GetSettings();
	
	/* If Missing Material Data */
	if (ExpressionContainer->Num() == 0) {
#if ENGINE_UE5
		if (GetSettings()->AssetSettings.Material.Stubs) {
			CreateStubs(this);
			ReportCreatedStubs();
		}
		else {
#endif
			ReportMaterialDataMissing();
#if ENGINE_UE5
			return false;
		}
#endif
	}

	/* Iterate through all the expressions, and set properties */
	PropagateExpressions(ExpressionContainer);

#if UE5_1_BEYOND
	UMaterialEditorOnlyData* EditorOnlyData = Material->GetEditorOnlyData();
#else
	UMaterial* EditorOnlyData = Material;
#endif
	
	TArray<FString> IgnoredProperties = TArray<FString> {
		"ParameterGroupData",
		"ExpressionCollection",
		"CustomizedUVs"
	};

	const TSharedPtr<FJsonObject> RawConnectionData = TSharedPtr<FJsonObject>(Props);
	for (FString Property : IgnoredProperties) {
		if (RawConnectionData->HasField(Property)) {
			RawConnectionData->RemoveField(Property);
		}
	}
	
	/* Connect all pins using deserializer */
	GetObjectSerializer()->DeserializeObjectProperties(RawConnectionData, EditorOnlyData);

	/* CustomizedUVs defined here */
	const TArray<TSharedPtr<FJsonValue>>* InputsPtr;
	
	if (Props->TryGetArrayField(TEXT("CustomizedUVs"), InputsPtr)) {
		int i = 0;
		for (const auto& InputValue : *InputsPtr) {
			FJsonObject* InputObject = InputValue->AsObject().Get();
			FName InputExpressionName = GetExpressionName(InputObject);

			if (ExpressionContainer->Contains(InputExpressionName)) {
				FExpressionInput Input = PopulateExpressionInput(InputObject, ExpressionContainer->Find<UMaterialExpression>(InputExpressionName));
				RemapConvertOutput(Input);

				EditorOnlyData->CustomizedUVs[i] = *reinterpret_cast<FVector2MaterialInput*>(&Input);
			}
			i++;
		}
	}

	const TArray<TSharedPtr<FJsonValue>>* StringParameterGroupData;
	if (Props->TryGetArrayField(TEXT("ParameterGroupData"), StringParameterGroupData)) {
		TArray<FParameterGroupData> ParameterGroupData;

		for (const auto& ParameterGroupDataObject : *StringParameterGroupData) {
			if (ParameterGroupDataObject->IsNull()) continue;
			FParameterGroupData GroupData;

			FString GroupName;
			if (ParameterGroupDataObject->AsObject()->TryGetStringField(TEXT("GroupName"), GroupName)) GroupData.GroupName = GroupName;
			int GroupSortPriority;
			if (ParameterGroupDataObject->AsObject()->TryGetNumberField(TEXT("GroupSortPriority"), GroupSortPriority)) GroupData.GroupSortPriority = GroupSortPriority;

			ParameterGroupData.Add(GroupData);
		}

		EditorOnlyData->ParameterGroupData = ParameterGroupData;
	}

	/* Handle edit changes, and add it to the content browser */
	if (!OnAssetCreation(Material)) return false;

	const TSharedPtr<FJsonObject>* ShadingModelsPtr;
	
	if (GetAssetData()->TryGetObjectField(TEXT("ShadingModels"), ShadingModelsPtr)) {
		int ShadingModelField;
		
		if (ShadingModelsPtr->Get()->TryGetNumberField(TEXT("ShadingModelField"), ShadingModelField)) {
#if UE5_1_BEYOND
			Material->GetShadingModels().SetShadingModelField(ShadingModelField);
#else
			/* Not to sure what to do in UE4, no function exists to override it. */
#endif
		}
	}

	/* Deserialize any properties */
	GetObjectSerializer()->DeserializeObjectProperties(GetAssetData(), Material);

#if ENGINE_UE5
	/* Update Cached Expression Data */
	if (AssetExport->GetJsonObject().Has("CachedExpressionData")) {
		FUObjectJsonValueExport CachedExpressionData = AssetExport->GetJsonObject().GetObject("CachedExpressionData");

		UMaterialAccessor* Accessor = Cast<UMaterialAccessor>(Material);
		FMaterialCachedExpressionData* CachedData = Accessor->GetCachedExpressionDataRef();

		/*GetPropertySerializer()->DeserializeStruct(FMaterialCachedExpressionData::StaticStruct(), CachedExpressionData.JsonObject.ToSharedRef(), CachedData);*/
	}
#endif
	
	/* Move Material Result Node ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ */
	UMaterialExpression* PositionalExpression = EditorOnlyData->BaseColor.Expression;
	if (UMaterialExpression* MaterialAttributes = EditorOnlyData->MaterialAttributes.Expression) {
		if (Material->bUseMaterialAttributes) {
			PositionalExpression = MaterialAttributes;
		}
	}

	if (!PositionalExpression) {
		PositionalExpression = EditorOnlyData->EmissiveColor.Expression;
	}

	if (PositionalExpression) {
		Material->EditorX = PositionalExpression->MaterialExpressionEditorX + PositionalExpression->GetWidth() * 2.5;
		Material->EditorY = PositionalExpression->MaterialExpressionEditorY;
	}
	/* Move Material Result Node ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ */

	/* Cached expression data arrived in 4.25; before that ForceRecompileForRendering below is
	 * what walks the expressions the importer just built */
#if !UE4_24_BELOW
	Material->UpdateCachedExpressionData();
#endif

	FMaterialUpdateContext MaterialUpdateContext;
	MaterialUpdateContext.AddMaterial(Material);
	
	Material->ForceRecompileForRendering();

	Material->PostEditChange();
	Material->MarkPackageDirty();
	Material->PreEditChange(nullptr);

	Save();

	return true;
}