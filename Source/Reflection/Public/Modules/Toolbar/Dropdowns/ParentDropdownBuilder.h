/* Copyright Reflection Contributors 2024-2026 */

#pragma once

#include "CoreMinimal.h"

/* Default: Does the first section in the dropdown */
struct IParentDropdownBuilder {
	virtual ~IParentDropdownBuilder() = default;
	virtual void Build(FMenuBuilder& MenuBuilder) const;
};