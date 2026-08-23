/* Copyright Reflection Contributors 2024-2026 */

#pragma once

#include "CoreMinimal.h"
#include "Modules/Toolbar/Toolbar.h"
#include "Engine/Compatibility.h"

#if ENGINE_UE4
#include "Modules/ModuleInterface.h"
#endif

class FReflectionModule : public IModuleInterface {
public:
    virtual void StartupModule() override;
    virtual void ShutdownModule() override;

private:
    UReflectionToolbar* Toolbar = nullptr;
};