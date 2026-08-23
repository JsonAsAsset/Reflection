/* Copyright Reflection Contributors 2024-2026 */

#pragma once

#include "Importers/Constructor/Importer.h"

/* 4.25 and below build this module without the engine's shared PCH (see Reflection.Build.cs),
 * which is where the material function type used to come in from */
#if UE4_25_BELOW
class UMaterialFunction;
#endif

class FMaterialSubstituteContext;

/* Material Graph Handler
 * Handles everything needed to create a material graph from JSON. */
class IMaterialGraph : public IImporter {
public:
	/* Public for the substitute context, which builds expressions and reads connections too */
	static void SetExpressionParent(UObject* Parent, UMaterialExpression* Expression, const TSharedPtr<FJsonObject>& Json);
	static void AddExpressionToParent(UObject* Parent, UMaterialExpression* Expression);
	static FExpressionInput PopulateExpressionInput(const FJsonObject* JsonProperties, UMaterialExpression* Expression, const FString& Type = "Default");
	static FName GetExpressionName(const FJsonObject* JsonProperties, const FString& OverrideParameterName = "Expression");

	/* Moves an input naming a rebuilt convert node's output onto the node standing in for it. The
	 * property serializer handles its own; this is for inputs populated by hand. */
	void RemapConvertOutput(FExpressionInput& Input) const;

	/* A rebuilt convert node has already spent its properties, so they must not be read onto it */
	bool IsConvertSubstitute(UMaterialExpression* Expression) const;

	void ReportMaterialDataMissing() const;
	void ReportNullExpressions() const;
	void ReportCreatedStubs() const;

#if ENGINE_UE4
	/* In Unreal Engine 4, to combat the absence of Sub-graphs, create a Material Function in place of it
	 * This holds a mapping to the name of the composite node it was created from, and the material
	 * function created in-place of it */

	TMap<FName, UMaterialFunction*> SubgraphFunctions;
#endif

protected:
	/* Find Material's Data, and creates a container of material nodes */
	TSharedPtr<FJsonObject> FindMaterialData(const FString& Type, FUObjectExportContainer* Container);

	/* Whether the expression list has slots the export carried no node for */
	static bool HasNullExpressions(const TSharedPtr<FJsonObject>& Properties);

	/* Makes each expression with their class */
	void ConstructExpressions(FUObjectExportContainer* Container);
	UMaterialExpression* CreateEmptyExpression(FUObjectExport* Export, FMaterialSubstituteContext& Context);

	/* Modifies Graph Nodes (copies over properties from FJsonObject) */
	void PropagateExpressions(FUObjectExportContainer* Container);

	/* Holds a parameter reading custom primitive data to the slots this engine has ~~~~~~~~~ */
	static void ClampCustomPrimitiveDataIndex(UMaterialExpression* Expression);

	/* Takes the pins for attributes this engine has no definition for off every get and set
	 * material attributes node, and moves what read them off with them. Wants a wired graph, so
	 * call it once the material's own inputs have been connected too.
	 *
	 * MaterialData is where those inputs live, which is the material itself before 5.1 and its
	 * editor only data from there on. Null for a material function. */
	static void PruneMissingMaterialAttributes(UObject* Parent, UObject* MaterialData = nullptr);

	/* Offers a node this engine has no class for around the substitute registry. See
	 * MaterialNodeSubstitute.h; the substitutes themselves are a file each under Graph/Substitutes. */
	static UMaterialExpression* CreateSubstituteExpression(FMaterialSubstituteContext& Context);
};
