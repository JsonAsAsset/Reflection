/* Copyright Reflection Contributors 2024-2026 */

#pragma once

#include "CoreMinimal.h"
#include "EdGraph/EdGraphPin.h"
#include "Dom/JsonObject.h"

class UBlueprint;

/* Rebuilds the variables a blueprint declares.
 *
 * A blueprint is created from its parent class, so it starts out with only what the parent gave
 * it. Everything the blueprint added itself lives in the generated class's ChildProperties, and
 * until those exist as member variables there is nowhere for their defaults to land: the class
 * default object carries values for properties the recreated blueprint doesn't have, and setting
 * them quietly does nothing.
 *
 * ChildProperties also holds the blueprint's plumbing, the ubergraph frame and every anim graph
 * node's state. Only the entries a user would see in the editor are turned into variables. */
struct REFLECTION_API FBlueprintVariables {
	/* Adds a member variable for every user facing ChildProperty, returns how many were added */
	static int32 Construct(UBlueprint* Blueprint, const TArray<TSharedPtr<FJsonValue>>& ChildProperties);

	/* Whether a ChildProperty is one the editor would show rather than blueprint plumbing */
	static bool IsUserVariable(const TSharedPtr<FJsonObject>& Property);

	/* Resolves a ChildProperty to the pin type a blueprint variable is declared with.
	 * False when the type isn't one that maps onto a blueprint variable. */
	static bool GetPinType(const TSharedPtr<FJsonObject>& Property, FEdGraphPinType& OutPinType);
};
