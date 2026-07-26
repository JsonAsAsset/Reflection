/* Copyright Reflection Contributors 2024-2026 */

#pragma once

#include "Engine/EngineUtilities.h"

#define REGISTER_IMPORTER(ImporterClass, AcceptedTypes, Category) \
namespace { \
    struct FAutoRegister_##ImporterClass { \
        FAutoRegister_##ImporterClass() { \
            FImporterRegistrationInfo Info(FString(Category), &CreateImporter<ImporterClass>); \
            GetFactoryRegistry().Add(AcceptedTypes, Info); \
        } \
    }; \
    static FAutoRegister_##ImporterClass AutoRegister_##ImporterClass; \
}