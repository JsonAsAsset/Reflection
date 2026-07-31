/* Copyright Reflection Contributors 2024-2026 */

#include "Modules/UI/StyleModule.h"
#include "Styling/SlateStyleRegistry.h"
#include "Interfaces/IPluginManager.h"
#include "Modules/Metadata.h"
#include "Engine/EngineUtilities.h"
#include "Engine/Compatibility.h"

/* 4.25 and below build this module without the engine's shared PCH (see Reflection.Build.cs),
 * which is where the slate application used to come in from */
#if UE4_25_BELOW
#include "Framework/Application/SlateApplication.h"
#endif

#if ENGINE_UE5
#include "Styling/ToolBarStyle.h"
#include "Styling/StyleColors.h"
#include "Brushes/SlateRoundedBoxBrush.h"
#endif

#define IMAGE_BRUSH(RelativePath, ...) FSlateImageBrush(Style->RootToContentDir(RelativePath, TEXT(".png")), __VA_ARGS__)

const FVector2D Icon40x40(40, 40);

TSharedRef<FSlateStyleSet> FReflectionStyle::Create() {
	TSharedRef<FSlateStyleSet> Style = MakeShareable(new FSlateStyleSet("ReflectionStyle"));
	Style->SetContentRoot(FRMetadata::Plugin->GetBaseDir() / TEXT("Resources"));

	Style->Set("Toolbar.Icon", new IMAGE_BRUSH(TEXT("./Toolbar/40px"), Icon40x40));
	Style->Set("Toolbar.Heart", new IMAGE_BRUSH(TEXT("./Toolbar/Heart_40px"), Icon40x40));
	Style->Set("Toolbar.Cloud", new IMAGE_BRUSH(TEXT("./Toolbar/Cloud_40px"), Icon40x40));

	return Style;
}

TSharedPtr<FSlateStyleSet> FReflectionStyle::StyleInstance = nullptr;

void FReflectionStyle::Initialize() {
	if (!StyleInstance.IsValid()) {
		StyleInstance = Create();
		FSlateStyleRegistry::RegisterSlateStyle(*StyleInstance);
	}
}

void FReflectionStyle::Shutdown() {
	FSlateStyleRegistry::UnRegisterSlateStyle(*StyleInstance);
	ensure(StyleInstance.IsUnique());
	StyleInstance.Reset();
}

FName FReflectionStyle::GetStyleSetName() {
	static FName StyleSetName(TEXT("ReflectionStyle"));
	return StyleSetName;
}

FName FReflectionStyle::GetEmbeddedToolbarStyleName() {
	static FName EmbeddedToolbarStyleName(TEXT("Reflection.EmbeddedToolbar"));
	return EmbeddedToolbarStyleName;
}

#if ENGINE_UE5
void FReflectionStyle::EnsureEmbeddedToolbarStyleRegistered() {
	static bool bRegistered = false;

	if (bRegistered) {
		return;
	}

	bRegistered = true;

	FToolBarStyle EmbeddedToolbarStyle = FAppStyle::Get().GetWidgetStyle<FToolBarStyle>("AssetEditorToolbar");

	EmbeddedToolbarStyle.SetBackground(FSlateRoundedBoxBrush(FStyleColors::Recessed, 8.0f));
	EmbeddedToolbarStyle.SetBackgroundPadding(FMargin(6.f, 3.f, 6.f, 3.f));

	StyleInstance->Set(GetEmbeddedToolbarStyleName(), EmbeddedToolbarStyle);
	StyleInstance->Set(TEXT("CalloutToolbar"), FAppStyle::Get().GetWidgetStyle<FToolBarStyle>("CalloutToolbar"));
}
#endif

const ISlateStyle& FReflectionStyle::Get() {
	return *StyleInstance;
}

#undef IMAGE_BRUSH

void FReflectionStyle::ReloadTextures() {
	if (FSlateApplication::IsInitialized()) {
		FSlateApplication::Get().GetRenderer()->ReloadTextureResources();
	}
}