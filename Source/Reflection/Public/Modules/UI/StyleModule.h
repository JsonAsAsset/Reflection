/* Copyright Reflection Contributors 2024-2026 */

#pragma once

#include "CoreMinimal.h"
#include "Styling/SlateStyle.h"

/* Style of Reflection */
class FReflectionStyle {
public:
	static const ISlateStyle& Get();
	static FName GetStyleSetName();
	
public:
	static void Initialize();
	static void ReloadTextures();

private:
	static TSharedRef<FSlateStyleSet> Create();
	static TSharedPtr<FSlateStyleSet> StyleInstance;

public:
	static void Shutdown();
};