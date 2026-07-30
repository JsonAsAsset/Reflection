/* Copyright Reflection Contributors 2024-2026 */

#pragma once

#include "CoreMinimal.h"

class TSelectedAssetsBase;

/* Hands back a tool's instance */
using FToolInstanceDelegate = TSelectedAssetsBase* (*)();

inline TMap<FName, FToolInstanceDelegate>& GetToolRegistry() {
	static TMap<FName, FToolInstanceDelegate> Registry;

	return Registry;
}

/* A run outlives the click that started it, so a tool has to outlive its own requests. One
 * instance per tool for the editor's lifetime is also what stops a second click from starting a
 * second run on top of the first.
 *
 * Built on first use, which is the menu being opened, so nothing here runs during static init. */
template <typename ToolType>
TSelectedAssetsBase* GetToolInstance() {
	static ToolType Tool;

	return &Tool;
}

/* Goes in a tool's header, below the class. Every translation unit that includes the header
 * registers, so entries repeat and arrive in whatever order the linker picked; keying on the
 * class name drops the repeats, and the dropdown sorts what it gets. */
#define REGISTER_TOOL(ToolClass) \
namespace { \
	struct FAutoRegisterTool_##ToolClass { \
		FAutoRegisterTool_##ToolClass() { \
			GetToolRegistry().Add(FName(TEXT(#ToolClass)), &GetToolInstance<ToolClass>); \
		} \
	}; \
	static FAutoRegisterTool_##ToolClass AutoRegisterTool_##ToolClass; \
}
