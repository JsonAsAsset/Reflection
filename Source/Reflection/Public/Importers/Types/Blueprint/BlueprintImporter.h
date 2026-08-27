/* Copyright Reflection Contributors 2024-2026 */

#pragma once

#include "Importers/Constructor/Importer.h"

class IBlueprintImporter : public IImporter {
protected:
	UBlueprint* Blueprint = nullptr;

	/* A property the class carries that some graph answers through, rather than writing */
	struct FDecided {
		FString Owner;
		FName Member;
		UEdGraphPin* Pin = nullptr;
	};

	TArray<FDecided> Decides;

public:
	virtual UObject* CreateAsset(UObject* CreatedAsset = nullptr) override;
	
	virtual bool Import() override;

	/* Says that a write into one of the class's own properties is answered by a pin.
	 *
	 * A transition rule is not written in the event graph, but it is compiled into the ubergraph
	 * with everything else, and it ends by setting a member of the node it decides. Said here, the
	 * stretch that does it is laid out in the graph that pin belongs to, and what it would have
	 * written is wired to the pin instead. */
	void Answers(const FString& Owner, FName Member, UEdGraphPin* Pin);

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

	/* Says which interfaces the blueprint answers for, and answers how many it took on */
	int32 ConstructInterfaces();

	/* Everything a blueprint carries beyond its defaults: its timelines, the graphs its functions
	 * were written in, and what the editor was told about them. Answers how many nodes were laid.
	 *
	 * Apart from the rest of the import, since what a blueprint is made of does not depend on what
	 * kind of blueprint it is. An animation blueprint is built another way entirely and still keeps
	 * its events in an event graph, cooked to the same bytecode as anybody else's. */
	int32 ConstructBody();

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