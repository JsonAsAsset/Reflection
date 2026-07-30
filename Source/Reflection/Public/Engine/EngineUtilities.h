/* Copyright Reflection Contributors 2024-2026 */

#pragma once

/*
 * Umbrella over the headers this file used to be.
 *
 * It grew into a grab bag of notifications, packages, plugins, settings and property reflection,
 * and 39 files include it, so splitting it without a shim would have meant guessing at what each
 * one actually uses. The pieces below are the real homes; include those directly in new code, and
 * narrow the existing includes as files are touched.
 */

#include "Engine/AssetCompatibility.h"
#include "Engine/Compatibility.h"
#include "Engine/Notifications.h"
#include "Engine/Package.h"
#include "Engine/Plugin.h"
#include "Engine/Properties.h"

#include "Settings/SettingsAccess.h"
#include "Utilities/ContentBrowser.h"
#include "Utilities/Dialog.h"

#include "Modules/Metadata.h"
#include "Serializers/ObjectSerializer.h"
#include "Serializers/PropertySerializer.h"

#ifndef __linux__
#include "Windows/WindowsHWrapper.h"
#endif
