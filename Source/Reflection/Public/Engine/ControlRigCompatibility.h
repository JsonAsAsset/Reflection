/* Copyright Reflection Contributors 2024-2026 */

#pragma once

/* Where the rig blueprint comes from.
 *
 * 5.7 pulled the parts of a rig asset anything can implement out into an interface of their own
 * and left the blueprint behind under a name that says so. UControlRigBlueprint is the same class
 * either side of that, so the header it is declared in is all that changes. */

#include "Engine/Compatibility.h"

#if REFLECTION_CONTROL_RIG

#if UE5_7_BEYOND
#include "ControlRigBlueprintLegacy.h"
#else
#include "ControlRigBlueprint.h"
#endif

#endif
