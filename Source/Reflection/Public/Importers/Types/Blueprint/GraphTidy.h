/* Copyright Reflection Contributors 2024-2026 */

#pragma once

#include "CoreMinimal.h"

class UEdGraph;
class UK2Node_VariableGet;

/* A tidying, run over a graph once it has been laid out.
 *
 * The compiler writes out what a node means, not the node: a get that checks what it read comes
 * back as a get, a call and a branch, and that is three nodes where somebody wrote one. Nothing in
 * the bytecode says which it was, but the three only ever appear together in that shape, so the one
 * can be put back.
 *
 * These only run where somebody is going to read the graph. A run with nobody in front of it is
 * checking the bytecode comes back the same, and every tidying is one more thing between what was
 * read and what is compared.
 *
 * One tidying to a file, beside this one. */
struct FGraphTidy {
	virtual ~FGraphTidy() = default;

	/* What it is called, for saying what was done */
	virtual const TCHAR* GetName() const = 0;

	/* Puts back whatever it can find, and answers how many it put back */
	virtual int32 Apply(UEdGraph* Graph) const = 0;
};

/* Puts a get into one of its other shapes.
 *
 * Which variation a get wears is the node's own business and kept private, so it is said through
 * the property rather than through the node, and the node is laid out again to grow the pins that
 * go with it. */
REFLECTION_API bool SetVariableGetVariation(UK2Node_VariableGet* Get, FName Variation);

/* Gives everything that was reading a get its own reading of the same variable.
 *
 * A get that has become a branch is there for the branch. Its value pin still hands the value out,
 * but anything reaching into it from elsewhere is a wire drawn across the run of execution to a
 * node that is now part of that run. Reading the variable again where it is wanted says the same
 * thing and draws nothing. */
REFLECTION_API void GiveReadersTheirOwn(UEdGraph* Graph, UK2Node_VariableGet* Converted, const TArray<UEdGraphPin*>& Readers);

/* Every tidying, in the order they are run */
REFLECTION_API const TArray<TSharedRef<FGraphTidy>>& GetGraphTidies();

/* Takes a tidying into the list. One of these sits at the bottom of each tidying's own file. */
REFLECTION_API void AddGraphTidy(const TSharedRef<FGraphTidy>& Tidy);

#define REGISTER_TIDY(TidyClass) \
namespace { \
	struct FAutoTidy_##TidyClass { \
		FAutoTidy_##TidyClass() { \
			AddGraphTidy(MakeShared<TidyClass>()); \
		} \
	}; \
	static FAutoTidy_##TidyClass AutoTidy_##TidyClass; \
}
