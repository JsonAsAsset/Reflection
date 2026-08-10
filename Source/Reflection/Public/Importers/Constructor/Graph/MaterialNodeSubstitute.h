/* Copyright Reflection Contributors 2024-2026 */

#pragma once

#include "CoreMinimal.h"
#include "Containers/ExportContainer.h"

class IMaterialGraph;
class UMaterialExpression;
class UMaterialExpressionConstant;
class FMaterialSubstituteContext;

/* Stands in for a material node this engine has no class for.
 *
 * To add one: drop a file in Private/Importers/Constructor/Graph/Substitutes with a class deriving
 * from this and REGISTER_MATERIAL_NODE_SUBSTITUTE under it. Nothing else to edit.
 *
 * Made once and shared by every import, so hold no state; put it on the context instead. */
class REFLECTION_API FMaterialNodeSubstitute {
public:
	virtual ~FMaterialNodeSubstitute() = default;

	virtual bool Handles(const FName Type) const = 0;

	/* nullptr passes the node to the next substitute */
	virtual UMaterialExpression* Create(FMaterialSubstituteContext& Context) = 0;

	/* Higher goes first. Only matters where two could answer for the same type. */
	virtual int32 GetPriority() const { return 0; }
};

/* Lives for one pass over one graph */
class REFLECTION_API FMaterialSubstituteContext {
public:
	FMaterialSubstituteContext(IMaterialGraph& InGraph, FUObjectExportContainer* InContainer);

	/* Called by the graph, not by substitutes */
	void SetExport(FUObjectExport* InExport);

	FUObjectExport* GetExport() const { return Export; }
	FName GetType() const;
	FName GetName() const;
	FString GetBaseName() const;
	UObject* GetParent() const;
	const TSharedPtr<FJsonObject>& GetProperties() const { return Properties; }

	int32 GetEditorX() const { return EditorX; }
	int32 GetEditorY() const { return EditorY; }

	FUObjectExportContainer* GetContainer() const { return Container; }
	IMaterialGraph& GetGraph() const { return Graph; }

	/* Building ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ */

	/* Everything a substitute builds except the expression it returns, which the caller parents */
	UMaterialExpression* CreateExpression(UClass* Class, const FString& NameSuffix, const int32 X, const int32 Y);

	template <typename ExpressionType>
	ExpressionType* CreateExpression(const FString& NameSuffix, const int32 X, const int32 Y) {
		return Cast<ExpressionType>(CreateExpression(ExpressionType::StaticClass(), NameSuffix, X, Y));
	}

	UMaterialExpressionConstant* CreateConstant(const float Value, const TCHAR* InputName, const int32 X, const int32 Y);

	void AddExpression(UMaterialExpression* Expression);

	/* Reading the export ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ */

	/* Null where the node was left to its own value */
	TSharedPtr<FJsonObject> FindConnectedInput(const TCHAR* InputName) const;

	float ReadInputConstant(const TCHAR* PropertyName, const float DefaultValue) const;

	/* Putting work off ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ */

	/* The expression an input reads may have no object yet. Held by index, as the arrays some
	 * expressions keep their inputs in can move. */
	void ConnectLater(UMaterialExpression* Expression, const int32 InputIndex, const TSharedPtr<FJsonObject>& ExpressionInput);

	void Defer(TFunction<void(FMaterialSubstituteContext&)> Work);

	/* Called by the graph once every export has an object */
	void ResolveDeferred();

	/* Registers what stands in for each output of a node rebuilt out of single-output nodes, so an
	 * input naming output N follows it */
	void RegisterOutputRoots(UMaterialExpression* Root, const TArray<UObject*>& Roots);

private:
	IMaterialGraph& Graph;
	FUObjectExportContainer* Container = nullptr;

	FUObjectExport* Export = nullptr;
	TSharedPtr<FJsonObject> Properties;

	int32 EditorX = 0;
	int32 EditorY = 0;

	TArray<TFunction<void(FMaterialSubstituteContext&)>> Deferred;
};

/* Filled at static initialisation by the macro below */
class REFLECTION_API FMaterialNodeSubstituteRegistry {
public:
	static FMaterialNodeSubstituteRegistry& Get();

	void Register(const TSharedRef<FMaterialNodeSubstitute>& Substitute);

	/* Highest priority first */
	const TArray<TSharedRef<FMaterialNodeSubstitute>>& GetSubstitutes() const { return Substitutes; }

private:
	TArray<TSharedRef<FMaterialNodeSubstitute>> Substitutes;
};

template <typename SubstituteType>
struct TMaterialNodeSubstituteRegistrar {
	TMaterialNodeSubstituteRegistrar() {
		FMaterialNodeSubstituteRegistry::Get().Register(MakeShared<SubstituteType>());
	}
};

#define REGISTER_MATERIAL_NODE_SUBSTITUTE(SubstituteType) \
	static const TMaterialNodeSubstituteRegistrar<SubstituteType> SubstituteType##Registrar

/* Writing one ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ */

/* One input of a node, and what it maps to on whatever replaces it */
struct FSubstituteInputMapping {
	const TCHAR* NodeInput;
	const TCHAR* TargetInput;
	const TCHAR* NodeConstant;
	float DefaultConstant;
};

static constexpr int32 BinaryInputA = 0;
static constexpr int32 BinaryInputB = 1;
static constexpr int32 SingleInput = 0;

static constexpr int32 SubstituteConstantOffsetX = 220;
static constexpr int32 SubstituteConstantRowHeight = 80;
