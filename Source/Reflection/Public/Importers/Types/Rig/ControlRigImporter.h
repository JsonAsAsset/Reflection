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
};

REGISTER_IMPORTER(IControlRigImporter, {
	TEXT("ControlRigBlueprintGeneratedClass")
}, "Animation Assets");

#endif
