/* Copyright Reflection Contributors 2024-2026 */

#pragma once

#include "CoreMinimal.h"
#include "Containers/JsonValueExport.h"

class UBlueprint;
class UEdGraph;
class UK2Node;
class UK2Node_FunctionEntry;

/* Makes the graphs a class's functions were written in.
 *
 * A recreated blueprint starts with only what its parent gave it, and a cooked class carries a
 * function for everything the blueprint had. Nothing can be laid back out until the graph it was
 * written in exists, so the graphs are made from the functions before any bytecode is read.
 *
 * Not everything the class carries was written as a function. An event compiles to a function that
 * does nothing but call the ubergraph at the offset its body begins at, and the ubergraph is every
 * event's body run from one place. Those two are the event graph rather than graphs of their own,
 * and telling them apart is reading the function rather than reading its name. */
struct REFLECTION_API FBlueprintGraphs {
	/* What a cooked function turns out to have been written as */
	enum class EWritten : uint8 {
		/* A graph of its own */
		Function,

		/* A node in the event graph, whose body is part of the ubergraph */
		Event,

		/* Every event's body, run from one place */
		Ubergraph
	};

	struct FWritten {
		EWritten Kind = EWritten::Function;

		/* Where in the ubergraph an event's body begins */
		int32 EntryPoint = INDEX_NONE;

		/* What the event was handed, by the name the ubergraph reads it under.
		 *
		 * An event's parameters are not the ubergraph's. The event's own function copies each one
		 * onto the frame the ubergraph runs against, and the property it copies it to is named
		 * after the node and the pin rather than after the parameter, so the event's function is
		 * the only thing that says which is which. */
		TMap<FString, FString> Frame;
	};

	/* Reads a cooked function for what it was written as */
	static FWritten Reads(const FUObjectJsonValueExport& Function);

	/* Whether a property is part of a function's signature rather than something it keeps */
	static bool IsParameter(const TSharedPtr<FJsonObject>& Property, bool& bOutGivenBack);

	/* The graph an animation layer was drawn in, made where the blueprint hasn't got one.
	 *
	 * A layer is not a function graph: it is made of poses, it answers through an output pose, and
	 * what it takes is drawn on nodes rather than declared on an entry. */
	static UEdGraph* MakeLayer(UBlueprint* Blueprint, const FString& Name, const TArray<TSharedPtr<FJsonValue>>& Declared);

	/* The graph a function was written in, made where the blueprint hasn't got one */
	static UEdGraph* Make(UBlueprint* Blueprint, const FString& Name, const FUObjectJsonValueExport& Function, const TArray<TSharedPtr<FJsonValue>>& Declared);

	/* The graph a dispatcher's signature is written in, made where the blueprint hasn't got one.
	 *
	 * An event dispatcher is a delegate the blueprint declares and other things bind to. It is not
	 * a variable and not a function: it is a graph that is never opened, kept apart from both, and
	 * it exists to say what whoever binds to it will be handed. */
	static UEdGraph* MakeDispatcher(UBlueprint* Blueprint, const FString& Name, const TArray<TSharedPtr<FJsonValue>>& Declared);

	/* The graph the events were written in */
	/* The entry node of a graph, which is what a function's signature is written on */
	static UK2Node_FunctionEntry* EntryOf(const UEdGraph* Graph);

	static UEdGraph* Events(UBlueprint* Blueprint);

	/* Takes back out the event nodes the editor offered and the game's class never carried */
	static int32 RemoveUnwrittenEvents(UEdGraph* EventGraph, const TSet<FString>& Written);

	/* The node an event is entered through, made where the graph hasn't got one */
	static UK2Node* MakeEvent(UBlueprint* Blueprint, UEdGraph* EventGraph, const FString& Name, const TArray<TSharedPtr<FJsonValue>>& Declared);
};
