/* Copyright Reflection Contributors 2024-2026 */

#include "Commandlets/WidgetAnimationCheckCommandlet.h"

#include "Engine/Compatibility.h"

#include "Animation/WidgetAnimation.h"
#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetBlueprintGeneratedClass.h"
#include "Blueprint/WidgetTree.h"
#include "MovieScene.h"
#include "WidgetBlueprint.h"

DEFINE_LOG_CATEGORY_STATIC(LogWidgetAnimationCheck, Display, All);

int32 UWidgetAnimationCheckCommandlet::Main(const FString& Params) {
	FString Path;
	if (!FParse::Value(*Params, TEXT("path="), Path)) {
		UE_LOG(LogWidgetAnimationCheck, Error, TEXT("usage: -run=WidgetAnimationCheck -path=/Game/Some/Widget"));
		return 1;
	}

	UWidgetBlueprint* WidgetBlueprint = LoadObject<UWidgetBlueprint>(nullptr, *Path);
	if (!WidgetBlueprint) {
		UE_LOG(LogWidgetAnimationCheck, Error, TEXT("no widget blueprint at %s"), *Path);
		return 1;
	}

	UWidgetBlueprintGeneratedClass* GeneratedClass = Cast<UWidgetBlueprintGeneratedClass>(WidgetBlueprint->GeneratedClass);
	if (!GeneratedClass) {
		UE_LOG(LogWidgetAnimationCheck, Error, TEXT("%s has no generated class"), *Path);
		return 1;
	}

	/* A widget made the way CreateWidget makes one, so its tree is the instanced copy animations resolve against. */
	UUserWidget* Widget = NewObject<UUserWidget>(GetTransientPackage(), GeneratedClass);
	Widget->Initialize();

	UE_LOG(LogWidgetAnimationCheck, Display, TEXT("widget %s, tree %s, root %s"), *GetNameSafe(Widget), *GetNameSafe(Widget->WidgetTree), Widget->WidgetTree ? *GetNameSafe(Widget->WidgetTree->RootWidget) : TEXT("-"));

	const auto Check = [Widget](const TCHAR* Which, UWidgetAnimation* Animation) {
		if (!Animation || !Animation->MovieScene) {
			return;
		}

		UE_LOG(LogWidgetAnimationCheck, Display, TEXT("[%s] %s: %d animation binding(s), %d movie scene binding(s)"), Which, *Animation->GetName(), Animation->AnimationBindings.Num(), Animation->MovieScene->GetBindings().Num());

		for (const FWidgetAnimationBinding& Binding : Animation->AnimationBindings) {
			UWidget* Found = Widget->WidgetTree ? Widget->WidgetTree->FindWidget(Binding.WidgetName) : nullptr;
			UE_LOG(LogWidgetAnimationCheck, Display, TEXT("    AnimationBinding %s guid=%s slot=%s root=%d -> FindWidget %s"), *Binding.WidgetName.ToString(), *Binding.AnimationGuid.ToString(), *Binding.SlotWidgetName.ToString(), Binding.bIsRootWidget ? 1 : 0, *GetNameSafe(Found));
		}

		for (const FMovieSceneBinding& Binding : Animation->MovieScene->GetBindings()) {
			TArray<UObject*, TInlineAllocator<1>> Objects;

#if UE5_5_BEYOND
			/* 5.5 resolves a binding through a locator and a playback state rather than against the
			 * widget itself. This only reports what a binding points at, so the older call is the
			 * one worth keeping and the newer path is left to the engine's own tooling. */
			Animation->LocateBoundObjects(Binding.GetObjectGuid(), UE::UniversalObjectLocator::FResolveParams(Widget), nullptr, Objects);
#else
			Animation->LocateBoundObjects(Binding.GetObjectGuid(), Widget, Objects);
#endif

			FString Names;
			for (const UObject* Object : Objects) {
				Names += GetNameSafe(Object) + TEXT(" ");
			}

			const FMovieScenePossessable* Possessable = Animation->MovieScene->FindPossessable(Binding.GetObjectGuid());
			UE_LOG(LogWidgetAnimationCheck, Display, TEXT("    MovieSceneBinding %s guid=%s possessable=%s tracks=%d -> LocateBoundObjects [%s]"), *Binding.GetName(), *Binding.GetObjectGuid().ToString(), Possessable ? *Possessable->GetName() : TEXT("<none>"), Binding.GetTracks().Num(), *Names);
		}
	};

	for (UWidgetAnimation* Animation : WidgetBlueprint->Animations) {
		Check(TEXT("editor"), Animation);
	}

	for (UWidgetAnimation* Animation : GeneratedClass->Animations) {
		Check(TEXT("compiled"), Animation);
	}

	return 0;
}
