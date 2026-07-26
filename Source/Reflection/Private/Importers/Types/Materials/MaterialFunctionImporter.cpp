/* Copyright Reflection Contributors 2024-2026 */

#include "Importers/Types/Materials/MaterialFunctionImporter.h"
#include "Factories/MaterialFunctionFactoryNew.h"

UObject* IMaterialFunctionImporter::CreateAsset(UObject* CreatedAsset) {
	/* Create Material Function Factory (factory automatically creates the Material Function) */
	UMaterialFunctionFactoryNew* MaterialFunctionFactory = NewObject<UMaterialFunctionFactoryNew>();
	UMaterialFunction* MaterialFunction = Cast<UMaterialFunction>(MaterialFunctionFactory->FactoryCreateNew(UMaterialFunction::StaticClass(), GetPackage(), *GetAssetName(), RF_Standalone | RF_Public, nullptr, GWarn));

	return IImporter::CreateAsset(MaterialFunction);
}

bool IMaterialFunctionImporter::Import() {
	UMaterialFunction* MaterialFunction = Create<UMaterialFunction>();

	/* Empty all expressions, we create them */
#if ENGINE_UE5
	MaterialFunction->GetExpressionCollection().Empty();
#else
	MaterialFunction->FunctionExpressions.Empty();
#endif

	/* Handle edit changes, and add it to the content browser */
	if (!OnAssetCreation(MaterialFunction)) return false;

	/* Define editor only data from the JSON */
	FUObjectExportContainer* ExpressionContainer = new FUObjectExportContainer();
	const TSharedPtr<FJsonObject> Props = FindMaterialData(GetAssetType(), ExpressionContainer);

	/* Map out each expression for easier access */
	ConstructExpressions(ExpressionContainer);

	/* If Missing Material Data */
	if (ExpressionContainer->Num() == 0) {
		SpawnMaterialDataMissingNotification();

		return false;
	}

	/* Iterate through all the expressions, and set properties */
	PropagateExpressions(ExpressionContainer);

	/* Deserialize any properties */
	GetObjectSerializer()->DeserializeObjectProperties(GetAssetData(), MaterialFunction);
	
	MaterialFunction->PreEditChange(nullptr);
	MaterialFunction->PostEditChange();

	Save();
	
	return true;
}
