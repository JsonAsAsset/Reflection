/* Copyright Reflection Contributors 2024-2026 */

#pragma once

#include "Importers/Constructor/Importer.h"

class IBlueprintImporter final : public IImporter {
protected:
	UBlueprint* Blueprint = nullptr;
	
public:
	virtual UObject* CreateAsset(UObject* CreatedAsset = nullptr) override;
	
	virtual bool Import() override;
	
protected:
	/* Recreates the variables the blueprint declares, returns how many were added.
	 * Not const, reading the export off the container isn't. */
	int32 ConstructVariables();

	/* Handles SimpleConstructionScript, the component layout for Actor blueprints */
	void ConstructScript() const;

	/* Handles WidgetTree, the UI layout for Widget blueprints */
	void ConstructWidgetTree();

	/* Lays every function the class carries back out as a graph, read from the bytecode it was
	 * cooked as. Returns how many nodes it placed across all of them. */
	int32 ConstructGraphs();

	/* Rebuilds the timelines the blueprint keeps.
	 *
	 * A timeline is three things at once: a template the class carries holding the curves and how
	 * long it runs, a node in the event graph that starts and stops it, and a pair of functions the
	 * class calls as it plays. The template is the only one of the three the cook keeps, and the
	 * other two are made from it. */
	int32 ConstructTimelines();

	/* What each timeline hands out, by the name the script reads it under: the property a track is
	 * kept in against the node and pin it was drawn as */
	TMap<FString, TPair<TWeakObjectPtr<class UK2Node>, FName>> Handouts;

	/* What each timeline calls as it plays, by the name the class carries it under: the node it
	 * was drawn as against the way out of it that call stands for */
	TMap<FName, TPair<TWeakObjectPtr<class UK2Node>, FName>> Resumes;
};

REGISTER_IMPORTER(IBlueprintImporter, (TArray<FString>{ 
	TEXT("BlueprintGeneratedClass"),
	TEXT("WidgetBlueprintGeneratedClass")
}), TEXT("Blueprints"));