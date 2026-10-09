/* Copyright Reflection Contributors 2024-2026 */

#pragma once

/* Umbrella over the headers this file used to be.
 *
 * It grew into a grab bag of notifications, packages, plugins, settings and property reflection,
 * and 39 files include it, so splitting it without a shim would have meant guessing at what each
 * one actually uses. The pieces below are the real homes; include those directly in new code, and
 * narrow the existing includes as files are touched. */

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

/* [linux]  I would hope it's obvious that we don't have access to
 *          Windows libraries/code on a Linux environment. If anyone
 *          in the future cares enough, a proper guard for other
 *          platforms should probably be added here. (ie #if PLATFORM_WINDOWS) */
#if  !PLATFORM_LINUX
#include "Windows/WindowsHWrapper.h"
#endif
