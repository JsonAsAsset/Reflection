/* Copyright Reflection Contributors 2024-2026 */

#include "Importers/Types/Animation/AnimationBaseImporter.h"

#include "Animation/AnimMontage.h"
#include "Dom/JsonObject.h"
#include "Animation/AnimSequence.h"
#include "Modules/Cloud/Tools/AnimationData.h"

#if ENGINE_UE5
#include "Animation/AnimData/IAnimationDataController.h"
#if ENGINE_MINOR_VERSION >= 4
#include "Animation/AnimData/IAnimationDataModel.h"
#endif
#include "AnimDataController.h"
#endif

bool IAnimationBaseImporter::Import() {
	return ReadAnimationData(this, true, this);
}
