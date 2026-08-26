/* Copyright Reflection Contributors 2024-2026 */

#pragma once

#include "CoreMinimal.h"
#include "Containers/JsonValueExport.h"

class UEdGraph;

/* A macro, read back out of what it compiles to.
 *
 * A macro is one node in a graph and a dozen statements in a script: the compiler copies the whole
 * of the macro in wherever it was used, so nothing in the bytecode says a macro was ever there.
 * Written back out statement by statement, the reader gets the macro's own workings spread through
 * their graph instead of the one node they wrote.
 *
 * What each macro compiles to is fixed, and the engine keeps the only copy of it, in
 * /Engine/EditorBlueprintResources/StandardMacros. Reading that says exactly what to look for.
 *
 * One macro to a file, beside this one. Adding another is writing its Match and registering it;
 * nothing that lays graphs out has to know about it. */
struct FMacroMatch {
	/* The statements the macro accounts for, first and last */
	int32 First = INDEX_NONE;
	int32 Last = INDEX_NONE;

	/* What feeds each of the macro's inputs, by the name the macro gives the pin */
	TMap<FName, FUObjectJsonValueExport> Inputs;

	/* Inputs worked out after the macro begins, wired once the statements that work them out have
	 * been laid down. A loop's last index is the plain case. */
	TMap<FName, FUObjectJsonValueExport> Deferred;

	/* What runs from each of the macro's execution outputs, as the run of statements it covers.
	 * For a macro whose parts are written one after another, which is how a function reads. */
	TArray<TPair<FName, TPair<int32, int32>>> Bodies;

	/* Where each of the macro's execution outputs goes, as the address it begins at.
	 *
	 * The compiler writes a function in the order it runs, so a macro in one is a run of statements
	 * with its body in the middle. It writes an event graph as blocks stitched together by jumps,
	 * and there a macro's parts can sit anywhere: the first thing a loop does is written after
	 * everything it runs. Saying where each way out goes rather than carrying the run on is what
	 * lets one be read back at all, and a macro that says this is not read as a run of statements. */
	TMap<FName, int32> Leads;

	/* The compiler's name for a value the macro hands out, and the pin it comes from. A loop's
	 * counter is the plain case: every read of it in the body is a read of the macro's Index. */
	TMap<FString, FName> Handouts;

	/* Statements that are the macro's own workings and stand for nothing on their own */
	TSet<int32> Internal;

	/* Which macro this is, filled in once matched */
	const struct FMacroPattern* Pattern = nullptr;

	bool IsValid() const { return First != INDEX_NONE && Last != INDEX_NONE; }
};

/* One macro's worth of knowing what it looks like once compiled */
struct FMacroPattern {
	virtual ~FMacroPattern() = default;

	/* The macro's graph in StandardMacros, which is both its name and where it is read from */
	virtual const TCHAR* GetName() const = 0;

	/* Whether the statements from At onwards are this macro, and what its parts are */
	virtual bool Match(const TArray<FUObjectJsonValueExport>& Statements, int32 At, FMacroMatch& Out) const = 0;
};

/* Every macro that can be read back, in the order they are tried */
REFLECTION_API const TArray<TSharedRef<FMacroPattern>>& GetMacroPatterns();

/* Takes a macro into the list. One of these sits at the bottom of each macro's own file. */
REFLECTION_API void AddMacroPattern(const TSharedRef<FMacroPattern>& Pattern);

#define REGISTER_MACRO(PatternClass) \
namespace { \
	struct FAutoMacro_##PatternClass { \
		FAutoMacro_##PatternClass() { \
			AddMacroPattern(MakeShared<PatternClass>()); \
		} \
	}; \
	static FAutoMacro_##PatternClass AutoMacro_##PatternClass; \
}

/* Reading the bytecode, for the macros to say what they are looking at ~~~~~~~~~~~~~~~~~~~~~~~~~ */
namespace MacroReading {
	/* The name a Variable field names, however the asset spelled the reference.
	 *
	 * A property used to be an object, so an older asset points at one the way it points at any
	 * object, or writes the property itself out where it is used. Newer ones name it through a
	 * field path. All four say one name, and this is that name. */
	REFLECTION_API FString NamedProperty(const FUObjectJsonValueExport& Variable);

	/* The name a statement writes into, where it writes into a local at all */
	REFLECTION_API FString WrittenTo(const FUObjectJsonValueExport& Statement);

	/* The name a local is read by, where the expression is a plain read */
	REFLECTION_API FString ReadFrom(const FUObjectJsonValueExport& Expression);

	/* What a call names, however the bytecode spells the reference */
	REFLECTION_API FString CallsTo(const FUObjectJsonValueExport& Expression);

	REFLECTION_API FString TokenOf(const FUObjectJsonValueExport& Statement);

	/* One of the engine's own macros, by name. The engine keeps the only statement of what a macro
	 * means, and both reading one back out of bytecode and putting one back in a graph want it. */
	REFLECTION_API UEdGraph* StandardMacro(const TCHAR* Named);

	/* Whether two expressions say the same thing. Read on its own an expression is only a shape,
	 * so two of them are the same value where they are the same shape all the way down. */
	REFLECTION_API bool Same(const FUObjectJsonValueExport& A, const FUObjectJsonValueExport& B);

	/* Writing into something is spelled by what is written: a bool is let one way, an object
	 * another, and a plain value a third. They are all the same statement. */
	REFLECTION_API bool IsLet(const FString& Token);

	/* Where a statement sits in the script, which is what a jump or a push names it by */
	REFLECTION_API int32 AddressOf(const FUObjectJsonValueExport& Statement);

	REFLECTION_API int32 IndexOfAddress(const TArray<FUObjectJsonValueExport>& Statements, int32 Address);

	/* The address the run reaches after the one given.
	 *
	 * Written out, one statement follows another, and the run carries on to whichever comes next.
	 * A jump only says where to carry on, so the run steps straight over it: which ones were
	 * stepped over is answered too, since they belong to whatever is being read back rather than
	 * standing for anything themselves. */
	REFLECTION_API int32 NextInRun(const TArray<FUObjectJsonValueExport>& Statements, int32 Address, TSet<int32>& Passed);
}
