/* Copyright Reflection Contributors 2024-2026 */

#pragma once

#include "CoreMinimal.h"
#include "Styling/SlateStyle.h"

/* Style of Reflection */
class FReflectionStyle {
public:
	static const ISlateStyle& Get();
	static FName GetStyleSetName();

	/* A toolbar style with a background distinct from the Content Browser toolbar. */
	static FName GetEmbeddedToolbarStyleName();

	/* The style the reflect button itself draws with: the engine's callout toolbar, widened. */
	static FName GetActionButtonStyleName();

	/* Registers the style GetEmbeddedToolbarStyleName() names. Safe to call more than once; only
	 * the first call does anything. */
	static void EnsureEmbeddedToolbarStyleRegistered();

public:
	static void Initialize();
	static void ReloadTextures();

private:
	static TSharedRef<FSlateStyleSet> Create();
	static TSharedPtr<FSlateStyleSet> StyleInstance;

public:
	static void Shutdown();
};