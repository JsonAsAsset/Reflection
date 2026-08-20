/* Copyright Reflection Contributors 2024-2026 */

#pragma once

#include "Engine/EngineUtilities.h"

/* Takes a type off whichever importer already claims it.
 *
 * The plugin is built in more than one place: what ships with it handles a type one way, and a
 * build carrying its own work may handle it another. An override is looked at first, so the two can
 * both claim a type without either having to know about the other. */
#define REGISTER_IMPORTER_OVERRIDE(ImporterClass, AcceptedTypes, Category) \
namespace { \
    struct FAutoOverride_##ImporterClass { \
        FAutoOverride_##ImporterClass() { \
            FImporterRegistrationInfo Info(FString(Category), &CreateImporter<ImporterClass>); \
            GetFactoryOverrides().Add(AcceptedTypes, Info); \
        } \
    }; \
    static FAutoOverride_##ImporterClass AutoOverride_##ImporterClass; \
}

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