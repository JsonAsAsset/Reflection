/* Copyright Reflection Contributors 2024-2026 */

#pragma once

#include "CoreMinimal.h"
#include "Modules/Toolbar/Tools/ToolBase.h"

class REFLECTION_API TSkeletalMeshData : public TToolBase {
public:
	virtual void Execute();

protected:
	static TArray<FSkeletalMaterial> GetMaterials(USkeletalMesh* Mesh);
};