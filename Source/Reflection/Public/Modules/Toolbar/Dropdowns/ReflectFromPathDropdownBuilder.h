/* Copyright Reflection Contributors 2024-2026 */

#pragma once

#include "ParentDropdownBuilder.h"

/* Everything reachable from a path alone, with nothing selected and nothing in the project
 * pointing at it yet */
struct IReflectFromPathDropdownBuilder final : IParentDropdownBuilder {
	virtual void Build(FMenuBuilder& MenuBuilder) const override;
};
