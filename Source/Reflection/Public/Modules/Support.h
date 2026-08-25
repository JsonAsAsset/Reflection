/* Copyright Reflection Contributors 2024-2026 */

#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"
#include "Engine/Compatibility.h"

/* The nudge to star the repository.
 *
 * A clock turns every twenty minutes and only every other turn asks, so the ask comes round once
 * every forty and there is a quiet twenty between one and the next. */
struct FReflectionSupport {
	/* Starts the clock, unless there is nobody sat in front of the editor to read it */
	static void Register();
	static void Unregister();

private:
	static bool Tick(float DeltaTime);
	static void Show();

#if ENGINE_UE5
	static FTSTicker::FDelegateHandle Handle;
#else
	static FDelegateHandle Handle;
#endif

	static int32 Turn;
};
