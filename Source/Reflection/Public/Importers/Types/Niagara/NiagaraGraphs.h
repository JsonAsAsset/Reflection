/* Copyright Reflection Contributors 2024-2026 */

#pragma once

#include "CoreMinimal.h"

struct FUObjectExportContainer;
class UPropertySerializer;

/* What putting the graphs back together came to */
struct FNiagaraGraphStats {
	/* Nodes put into the graph that holds them */
	int32 Placed = 0;

	int32 Pins = 0;
	int32 Links = 0;

	/* Links onto a pin that isn't here */
	int32 Dangling = 0;

	/* Pins a node paired up with each other where one of the two isn't here */
	int32 Unpaired = 0;

	/* Calls onto a script with no graph the caller can walk into */
	int32 Uncalled = 0;
};

/* The graphs a Niagara asset is written as.
 *
 * A script, an emitter and a system all keep their work as node graphs hanging off a source, and
 * all three come across the same way: the nodes are ordinary exports the usual building makes, and
 * the wiring is not. A node writes its pins through an archive of its own after its properties, so
 * a graph read as properties alone is a pile of nodes with nothing joined to anything.
 *
 * Done once here rather than once per asset that has graphs in it. */
class REFLECTION_API FNiagaraGraphs {
public:
	static void Rebuild(FUObjectExportContainer* Container, UPackage* Package,
		UPropertySerializer* Serializer, FNiagaraGraphStats& OutStats);
};
