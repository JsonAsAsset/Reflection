/* Copyright Reflection Contributors 2024-2026 */

#pragma once

#include "CoreMinimal.h"
#include "Engine/Compatibility.h"

/* 4.25 and below build this module without the engine's shared PCH (see Reflection.Build.cs),
 * which is where the multi box builders used to come in from */
#if UE4_25_BELOW
#include "Framework/MultiBox/MultiBoxBuilder.h"
#endif

/* What every section of the dropdown is built through */
struct IParentDropdownBuilder {
	virtual ~IParentDropdownBuilder() = default;
	virtual void Build(FMenuBuilder& MenuBuilder) const = 0;
};