/* Copyright Reflection Contributors 2024-2026 */

#pragma once

#include "CoreMinimal.h"
#include "Containers/ExportContainer.h"
#include "Rigs/RigHierarchyDefines.h"

class UBlueprint;
class UPropertySerializer;
class URigHierarchy;
class URigHierarchyController;

/*
 * Rebuilds a rig's hierarchy - its bones, nulls, controls, curves and sockets - from the exported elements.
 *
 * A hierarchy isn't reflected data: URigHierarchy serialises its elements by hand rather than declaring them as
 * properties, so nothing the object serialiser does can put them back. The elements do survive the export
 * though, and the engine offers URigHierarchyController to add them one by one, which is the same route the
 * editor takes when a rigger builds one. Every value handed to it is deserialised into the engine's own structs
 * by the property serialiser, so this knows nothing about what any particular element contains.
 */
class REFLECTION_API FRigHierarchyBuilder {
public:
	FRigHierarchyBuilder(UBlueprint* InBlueprint, FUObjectExportContainer* InContainer, UPropertySerializer* InPropertySerializer);

	/* Rebuilds the hierarchy. False when the blueprint has none, or the package shipped no elements. */
	bool Build();

private:
	/* Fills any engine struct from its exported json, whatever it happens to contain */
	bool ReadStruct(const TSharedPtr<FJsonObject>& Json, UScriptStruct* Struct, void* OutValue) const;

	/* Elements name their parent directly when they can only have one, and through a constraint when they can have several */
	FRigElementKey ReadParentKey(const TSharedPtr<FJsonObject>& Element) const;

	/* Every element carries a pose; the local one is what the controller wants */
	FTransform ReadLocalTransform(const TSharedPtr<FJsonObject>& Element, const FString& StorageName) const;

	bool AddElement(const FRigElementKey& Key, const TSharedPtr<FJsonObject>& Element);

	UBlueprint* Blueprint = nullptr;
	FUObjectExportContainer* Container = nullptr;
	UPropertySerializer* PropertySerializer = nullptr;

	URigHierarchy* Hierarchy = nullptr;
	URigHierarchyController* Controller = nullptr;
};
