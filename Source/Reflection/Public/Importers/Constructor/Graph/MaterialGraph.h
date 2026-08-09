/* Copyright Reflection Contributors 2024-2026 */

#pragma once

#include "Importers/Constructor/Importer.h"

/* 4.25 and below build this module without the engine's shared PCH (see Reflection.Build.cs),
 * which is where the material function type used to come in from */
#if UE4_25_BELOW
class UMaterialFunction;
#endif

class UMaterialExpressionComponentMask;
class UMaterialExpressionReroute;

/*
 * Material Graph Handler
 * Handles everything needed to create a material graph from JSON.
*/
class IMaterialGraph : public IImporter {
protected:
	/* Find Material's Data, and creates a container of material nodes */
	TSharedPtr<FJsonObject> FindMaterialData(const FString& Type, FUObjectExportContainer* Container);

	/* Whether the expression list has slots the export carried no node for */
	static bool HasNullExpressions(const TSharedPtr<FJsonObject>& Properties);

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

	/* MaterialExpressionConvert (5.6) rebuilt out of nodes every engine has ~~~~~~~~~~~~ */
	UMaterialExpression* CreateConvertSubstitute(FUObjectExport* Export);
	void ResolveConvertSubstitutes(FUObjectExportContainer* Container);
	bool IsConvertSubstitute(UMaterialExpression* Expression) const;

	/* Moves an input that named one of a rebuilt convert node's outputs onto the expression
	 * standing in for that output. Inputs built by the property serializer are handled there;
	 * this is for the few the importers populate by hand. */
	void RemapConvertOutput(FExpressionInput& Input) const;

	/* A component mask standing in for one mapped convert input. The expression it reads from is
	 * only known once every export in the graph has an object, so the connection is made after
	 * the whole container has been constructed. */
	struct FPendingConvertInput {
		UMaterialExpressionComponentMask* Mask;
		TSharedPtr<FJsonObject> ExpressionInput;
	};

	TArray<FPendingConvertInput> PendingConvertInputs;

	/* MaterialExpressionLocalPosition rebuilt as the engine function it predates ~~~~~~~~~~ */
	UMaterialExpression* CreateLocalPositionSubstitute(FUObjectExport* Export);

	/* Named reroutes (5.0) rebuilt out of the plain reroute every engine has ~~~~~~~~~~~~ */
	UMaterialExpression* CreateNamedRerouteSubstitute(FUObjectExport* Export);
	void ResolveNamedRerouteUsages(FUObjectExportContainer* Container);

	/* A reroute standing in for a named reroute usage. A usage holds no input of its own, it names
	 * the declaration it reads, so the connection is made once every export in the graph has an
	 * object to point at. */
	struct FPendingNamedRerouteUsage {
		UMaterialExpressionReroute* Usage;
		TSharedPtr<FJsonObject> Properties;
	};

	TArray<FPendingNamedRerouteUsage> PendingNamedRerouteUsages;

	/* Holds a parameter reading custom primitive data to the slots this engine has ~~~~~~~~~ */
	static void ClampCustomPrimitiveDataIndex(UMaterialExpression* Expression);

	/* Carries a missing switch's first branch through the reroute standing in for it ~~~~~~~ */
	void ResolveSwitchPassthroughs(FUObjectExportContainer* Container);

	/* The reroute left behind for a switch class this engine does not have, with the connection
	 * it is to carry. Read after the whole container is built, like the ones above. */
	struct FPendingSwitchPassthrough {
		UMaterialExpressionReroute* Reroute;
		TSharedPtr<FJsonObject> ExpressionInput;
	};

	TArray<FPendingSwitchPassthrough> PendingSwitchPassthroughs;

public:
	UMaterialExpression* OnMissingNodeClass(FUObjectExport* Export, FUObjectExportContainer* Container);
	void ReportMaterialDataMissing() const;
	void ReportNullExpressions() const;
	void ReportCreatedStubs() const;

#if ENGINE_UE4
	/*
	 * In Unreal Engine 4, to combat the absence of Sub-graphs, create a Material Function in place of it
	 * This holds a mapping to the name of the composite node it was created from, and the material
	 * function created in-place of it
	 */

	TMap<FName, UMaterialFunction*> SubgraphFunctions;
#endif
};
