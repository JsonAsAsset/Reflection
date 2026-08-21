/* Copyright Reflection Contributors 2024-2026 */

#pragma once

/* What RigLogic hands back per joint, under whichever name the engine knows it by.
 *
 * 5.5 dropped the FTransformArrayView halves of these two and handed their names to the float
 * views that used to be the raw ones. The floats are what gets read either way, so only the name
 * they answer to changes. */

#include "Engine/Compatibility.h"

#if REFLECTION_RIG_LOGIC

#include "RigLogic.h"
#include "RigInstance.h"

inline TArrayView<const float> GetDnaNeutralJoints(const FRigLogic& RigLogic) {
#if UE5_5_BEYOND
	return RigLogic.GetNeutralJointValues();
#else
	return RigLogic.GetRawNeutralJointValues();
#endif
}

inline TArrayView<const float> GetDnaJointOutputs(const FRigInstance& Instance) {
#if UE5_5_BEYOND
	return Instance.GetJointOutputs();
#else
	return Instance.GetRawJointOutputs();
#endif
}

#endif
