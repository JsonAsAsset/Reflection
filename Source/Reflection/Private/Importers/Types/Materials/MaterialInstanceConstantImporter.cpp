/* Copyright Reflection Contributors 2024-2026 */

#include "Importers/Types/Materials/MaterialInstanceConstantImporter.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Dom/JsonObject.h"
#include "RHIDefinitions.h"
#include "MaterialShared.h"
#include "Utilities/JsonHelpers.h"

UObject* IMaterialInstanceConstantImporter::CreateAsset(UObject* CreatedAsset) {
	return IImporter::CreateAsset(NewObject<UMaterialInstanceConstant>(GetPackage(), UMaterialInstanceConstant::StaticClass(), *GetAssetName(), RF_Public | RF_Standalone));
}

bool IMaterialInstanceConstantImporter::Import() {
	UMaterialInstanceConstant* MaterialInstanceConstant = Create<UMaterialInstanceConstant>();

	/* Specific fix for 4.16 engines */
	const TArray<FString> ParameterFields = {
		TEXT("ScalarParameterValues"),
		TEXT("TextureParameterValues"),
		TEXT("VectorParameterValues")
	};

	for (const FString& FieldName : ParameterFields) {
		if (GetAssetDataAsValue().Has(FieldName)) {
			TArray<FUObjectJsonValueExport> Params = GetAssetDataAsValue().GetArray(FieldName);
			
			ConvertParameterNamesToInfos(Params);
			GetAssetDataAsValue().SetArray(FieldName, Params);
		}
	}
	
	GetObjectSerializer()->DeserializeObjectProperties(RemovePropertiesShared(GetAssetData(),
	{
		"CachedReferencedTextures"
	}), MaterialInstanceConstant);

	TArray<FUObjectJsonValueExport> StaticSwitchParametersObjects;
	TArray<FUObjectJsonValueExport> StaticComponentMaskParametersObjects;
	
	/* Optional Editor Data [contains static switch parameters] ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ */
	if (FUObjectExport* EditorOnlyData = GetContainer()->FindByType(FString("MaterialInstanceEditorOnlyData"))) {
		if (EditorOnlyData->GetPropertiesAsValue().Has("StaticParameters")) {
			ReadStaticParameters(EditorOnlyData->GetPropertiesAsValue().GetObject("StaticParameters"), StaticSwitchParametersObjects, StaticComponentMaskParametersObjects);
		}
	}

	/* Read from potential properties inside of asset data */
	if (GetAssetDataAsValue().Has("StaticParametersRuntime")) {
		ReadStaticParameters(GetAssetDataAsValue().GetObject("StaticParametersRuntime"), StaticSwitchParametersObjects, StaticComponentMaskParametersObjects);
	}
	if (GetAssetDataAsValue().Has(TEXT("StaticParameters"))) {
		ReadStaticParameters(GetAssetDataAsValue().GetObject("StaticParameters"), StaticSwitchParametersObjects, StaticComponentMaskParametersObjects);
	}

	/* ~~~~~~~~~ STATIC PARAMETERS ~~~~~~~~~~~ */
#if UE5_2_BEYOND || UE4_27_BELOW
	FStaticParameterSet NewStaticParameterSet; /* Unreal Engine 5.2/4.26 and beyond have a different method */
#endif

	TArray<FStaticSwitchParameter> StaticSwitchParameters;
	for (FUObjectJsonValueExport& StaticParameter : StaticSwitchParametersObjects) {
		auto ParameterInfo = StaticParameter.GetObject("ParameterInfo");

		/* Create Material Parameter Info */
		FMaterialParameterInfo MaterialParameterParameterInfo = FMaterialParameterInfo(
			FName(ParameterInfo.GetString("Name")),
			static_cast<EMaterialParameterAssociation>(StaticEnum<EMaterialParameterAssociation>()->GetValueByNameString(ParameterInfo.GetString("Association"))),
			ParameterInfo.GetInteger("Index")
		);

		/* Now, create the actual switch parameter */
		FStaticSwitchParameter Parameter = FStaticSwitchParameter(
			MaterialParameterParameterInfo,
			StaticParameter.GetBool(TEXT("Value")),
			StaticParameter.GetBool("bOverride"),
			FGuid(StaticParameter.GetString("ExpressionGUID"))
		);

		StaticSwitchParameters.Add(Parameter);
#if UE5_1_BELOW
		MaterialInstanceConstant->GetEditorOnlyData()->StaticParameters.StaticSwitchParameters.Add(Parameter);
#endif
		
#if UE5_2_BEYOND || UE4_27_BELOW
		/* Unreal Engine 5.2/4.26 and beyond have a different method */
		NewStaticParameterSet.StaticSwitchParameters.Add(Parameter); 
#endif
	}

	TArray<FStaticComponentMaskParameter> StaticSwitchMaskParameters;
	
	for (FUObjectJsonValueExport& StaticParameter : StaticComponentMaskParametersObjects) {
		auto ParameterInfo = StaticParameter.GetObject("ParameterInfo");

		/* Create Material Parameter Info */
		FMaterialParameterInfo MaterialParameterParameterInfo = FMaterialParameterInfo(
			FName(ParameterInfo.GetString("Name")),
			static_cast<EMaterialParameterAssociation>(StaticEnum<EMaterialParameterAssociation>()->GetValueByNameString(ParameterInfo.GetString("Association"))),
			ParameterInfo.GetInteger("Index")
		);

		FStaticComponentMaskParameter Parameter = FStaticComponentMaskParameter(
			MaterialParameterParameterInfo,
			StaticParameter.GetBool("R"),
			StaticParameter.GetBool("G"),
			StaticParameter.GetBool("B"),
			StaticParameter.GetBool("A"),
			StaticParameter.GetBool("bOverride"),
			FGuid(StaticParameter.GetString("ExpressionGUID"))
		);

		StaticSwitchMaskParameters.Add(Parameter);
#if UE5_1_BELOW
		MaterialInstanceConstant->GetEditorOnlyData()->StaticParameters.StaticComponentMaskParameters.Add(Parameter);
#endif
		
#if UE5_2_BEYOND || UE4_27_BELOW
		NewStaticParameterSet.
		/* EditorOnly is needed on 5.2+ */
		#if UE5_2_BEYOND
			EditorOnly.
		#endif
		StaticComponentMaskParameters.Add(Parameter);
#endif
	}

#if UE5_2_BEYOND || UE4_27_BELOW
	FMaterialUpdateContext MaterialUpdateContext(FMaterialUpdateContext::EOptions::Default & ~FMaterialUpdateContext::EOptions::RecreateRenderStates);

	MaterialInstanceConstant->UpdateStaticPermutation(NewStaticParameterSet, &MaterialUpdateContext);
	MaterialInstanceConstant->InitStaticPermutation();
#endif

	return OnAssetCreation(MaterialInstanceConstant);
}

void IMaterialInstanceConstantImporter::ReadStaticParameters(const FUObjectJsonValueExport& StaticParameters, TArray<FUObjectJsonValueExport>& StaticSwitchParameters, TArray<FUObjectJsonValueExport>& StaticComponentMaskParameters) {
	if (StaticParameters.Has("StaticSwitchParameters")) {
		TArray<FUObjectJsonValueExport> Params = StaticParameters.GetArray("StaticSwitchParameters");
		ConvertParameterNamesToInfos(Params);
		
		for (FUObjectJsonValueExport& Parameter : Params) {
			StaticSwitchParameters.Add(Parameter);
		}
	}

	if (StaticParameters.Has("StaticComponentMaskParameters")) {
		TArray<FUObjectJsonValueExport> Params = StaticParameters.GetArray("StaticComponentMaskParameters");
		ConvertParameterNamesToInfos(Params);
		
		for (FUObjectJsonValueExport& Parameter : Params) {
			StaticComponentMaskParameters.Add(Parameter);
		}
	}
}

void IMaterialInstanceConstantImporter::ConvertParameterNamesToInfos(TArray<FUObjectJsonValueExport>& Input) {
	/* Convert ParameterName to be inside ParameterInfo */
	for (FUObjectJsonValueExport& Parameter : Input) {
		if (Parameter.Has("ParameterName")) {
			TSharedPtr<FJsonObject> ParameterInfo = MakeShared<FJsonObject>();
			
			ParameterInfo->SetStringField(TEXT("Name"), Parameter.GetString("ParameterName"));
			Parameter.SetObject("ParameterInfo", ParameterInfo);

			/* Cleanup */
			Parameter.Remove("ParameterName");
		}
	}
}