/* Copyright Reflection Contributors 2024-2026 */

#pragma once

#include "ParentDropdownBuilder.h"

struct ICloudToolsDropdownBuilder final : IParentDropdownBuilder {
	virtual void Build(FMenuBuilder& MenuBuilder) const override;
};