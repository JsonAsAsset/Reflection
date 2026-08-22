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
#include "DNAReader.h"
#include "DNAReaderAdapter.h"

#if REFLECTION_RIG_LOGIC_UE_SPACE_READER
#include "RigLogicDNAReader.h"
#endif

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

/* Builds RigLogic the way UDNAAsset does, which is the only way to get the pose the anim node
 * would write.
 *
 * From 5.6 the engine puts a RigLogicDNAReader between the DNA and RigLogic to convert into UE's
 * axes as it reads, and the node then writes joints without flipping anything itself. Building
 * RigLogic straight off the DNA instead leaves its output a DNA-space pose that nothing downstream
 * expects, which reads as a skeleton with every rotation and every sideways offset inverted.
 *
 * Older engines have no such wrapper: the DNA goes in as it is and the node does the axes on the
 * way out, so there the plain reader is already the right one.
 *
 * The wrapper only has to survive the construction -- RigLogic copies everything it needs out of
 * the reader there and never looks at it again -- so it is safe to let it go at the return. */
inline FRigLogic MakeDnaRigLogic(const TSharedPtr<IDNAReader>& Behavior) {
#if REFLECTION_RIG_LOGIC_UE_SPACE_READER
	RigLogicDNAReader InUeSpace{Behavior->Unwrap()};
	FDNAReader<RigLogicDNAReader> Wrapper{&InUeSpace};

	return FRigLogic(&Wrapper);
#else
	return FRigLogic(Behavior.Get());
#endif
}

#endif
