/* Copyright Reflection Contributors 2024-2026 */

#pragma once

#include "CoreMinimal.h"
#include "Modules/Toolbar/Toolbar.h"
#include "Engine/Compatibility.h"

#if ENGINE_UE4
#include "Modules/ModuleInterface.h"
#endif

struct GitHub {
    static inline FString URL = "https://github.com/JsonAsAsset/Reflection";
    
    struct README {
        static inline FString Link = URL + "?tab=readme-ov-file#asset-types";
        static inline FString AssetTypes = Link + "#asset-types";
        static inline FString Cloud = Link + "#cloud";
    };
};

struct Donation {
    static inline FString KO_FI = "https://ko-fi.com/t4ctor";
};

class FReflectionModule : public IModuleInterface {
public:
    virtual void StartupModule() override;
    virtual void ShutdownModule() override;

private:
    UReflectionToolbar* Toolbar = nullptr;
};