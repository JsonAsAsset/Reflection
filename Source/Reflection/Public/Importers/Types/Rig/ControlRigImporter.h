/* Copyright Reflection Contributors 2024-2026 */

#pragma once

#include "Importers/Constructor/Importer.h"

/* Control Rig ships as an engine plugin, and the rig API this is written against is the one UE5
 * introduced. Reflection.Build.cs decides whether either is there to link against. */
#if REFLECTION_CONTROL_RIG

class UControlRigBlueprint;

/* Rebuilds a Control Rig from a cooked ControlRigBlueprintGeneratedClass.
 *
 * A cooked rig carries its hierarchy and the RigVM its graph was compiled into, and nothing of the
 * graph itself: the nodes, their links and their pin defaults are editor data the cook drops. What
 * comes back out of here is everything around that graph, the hierarchy with its bones, nulls,
 * controls and curves, the variables the rig declared, and the settings the editor shows. */
class IControlRigImporter final : public IImporter {
public:
	virtual UObject* CreateAsset(UObject* CreatedAsset) override;
	virtual bool Import() override;

private:
	/* Adds every element the export describes, returns how many landed in the hierarchy */
	int32 ConstructHierarchy(UControlRigBlueprint* Blueprint) const;

	/* The variables the rig declared, which its graph reads and writes through Get and Set nodes */
	int32 ConstructVariables(UControlRigBlueprint* Blueprint);

	/* The graph, read back out of the RigVM the rig's nodes were compiled into */
	void ConstructGraph(UControlRigBlueprint* Blueprint) const;

	/* The preview mesh and shape libraries, which sit on the blueprint rather than the hierarchy */
	void ConstructSettings(UControlRigBlueprint* Blueprint) const;

	/* Fills in the class default object's value for each variable that was added */
	void ApplyVariableDefaults(const UControlRigBlueprint* Blueprint) const;

	/* A runtime asset is a rig cooked without a class of its own: the asset itself holds what the
	 * class default object would, and its variables are a property bag rather than properties */
	bool IsRuntimeAsset() const;

	/* Where the rig's hierarchy, VM and shape libraries are named: the class default object's
	 * properties for a generated class, the asset's own for a runtime asset */
	FUObjectJsonValueExport GetRigProperties() const;

	/* The variables a runtime asset's property bag declares, which have no class to live on */
	int32 ConstructRuntimeVariables(UControlRigBlueprint* Blueprint);

	/* A runtime asset whose hierarchy is built by its construction event is cooked with none, so
	 * the bones and curves come from the skeleton the rig was authored against */
	int32 ImportHierarchyFromSkeleton(UControlRigBlueprint* Blueprint) const;
};

/* Later cooks keep a rig's class as the RigVM one it derives from; the class it generates still
 * derives from ControlRig. Later still, a rig cooks to a runtime asset with no class at all. */
REGISTER_IMPORTER(IControlRigImporter, (TArray<FString>{
	TEXT("ControlRigBlueprintGeneratedClass"),
	TEXT("RigVMBlueprintGeneratedClass"),
	TEXT("ControlRigRuntimeAsset")
}), "Animation Assets");

#endif
