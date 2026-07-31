/* Copyright Reflection Contributors 2024-2026 */

#pragma once

#include "Importers/Constructor/Importer.h"

/* 4.25 and below build this module without the engine's shared PCH (see Reflection.Build.cs),
 * which is where the material function type used to come in from */
#if UE4_25_BELOW
class UMaterialFunction;
#endif

/*
 * Material Graph Handler
 * Handles everything needed to create a material graph from JSON.
*/
class IMaterialGraph : public IImporter {
protected:
	/* Find Material's Data, and creates a container of material nodes */
	TSharedPtr<FJsonObject> FindMaterialData(const FString& Type, FUObjectExportContainer* Container);

	/* Functions to Handle Expressions */
	static void SetExpressionParent(UObject* Parent, UMaterialExpression* Expression, const TSharedPtr<FJsonObject>& Json);
	static void AddExpressionToParent(UObject* Parent, UMaterialExpression* Expression);
	
	/* Makes each expression with their class */
	void ConstructExpressions(FUObjectExportContainer* Container);
	UMaterialExpression* CreateEmptyExpression(FUObjectExport* Export, FUObjectExportContainer* Container);

	/* Modifies Graph Nodes (copies over properties from FJsonObject) */
	void PropagateExpressions(FUObjectExportContainer* Container);
	/* ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ */

	/* Functions to Handle Node Connections ~~~~~~~~~~~~ */
	static FExpressionInput PopulateExpressionInput(const FJsonObject* JsonProperties, UMaterialExpression* Expression, const FString& Type = "Default");
	static FName GetExpressionName(const FJsonObject* JsonProperties, const FString& OverrideParameterName = "Expression");

public:
	UMaterialExpression* OnMissingNodeClass(FUObjectExport* Export, FUObjectExportContainer* Container);
	void SpawnMaterialDataMissingNotification() const;
	void CreatedStubsNotification() const;

#if ENGINE_UE4
	/*
	 * In Unreal Engine 4, to combat the absence of Sub-graphs, create a Material Function in place of it
	 * This holds a mapping to the name of the composite node it was created from, and the material
	 * function created in-place of it
	 */

	TMap<FName, UMaterialFunction*> SubgraphFunctions;
#endif
};
