/* Copyright Reflection Contributors 2024-2026 */

#include "Modules/UI/StyleModule.h"
#include "Styling/SlateStyleRegistry.h"
#include "Interfaces/IPluginManager.h"
#include "Modules/Metadata.h"
#include "Engine/EngineUtilities.h"

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

const ISlateStyle& FReflectionStyle::Get() {
	return *StyleInstance;
}

#undef IMAGE_BRUSH

void FReflectionStyle::ReloadTextures() {
	if (FSlateApplication::IsInitialized()) {
		FSlateApplication::Get().GetRenderer()->ReloadTextureResources();
	}
}