/* Copyright Reflection Contributors 2024-2026 */

#pragma once

#include "Engine/Compatibility.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"

/* HTTP hands out thread-safe shared pointers on both engines, but UE4 defaults TSharedRef/
 * TSharedPtr to ESPMode::Fast and UE5 defaults them to ESPMode::ThreadSafe, so the mode has to
 * be spelled out for UE4 to end up with the same type */
#if ENGINE_UE5
using FReflectionHttpRequest = TSharedRef<IHttpRequest>;
using FReflectionHttpResponse = TSharedPtr<IHttpResponse>;
#elif UE4_25_BELOW
/* HTTP only started handing out a thread-safe request ref in 4.26; before that CreateRequest
 * returned a plain ESPMode::Fast ref, while the response was already thread safe */
using FReflectionHttpRequest = TSharedRef<IHttpRequest>;
using FReflectionHttpResponse = TSharedPtr<IHttpResponse, ESPMode::ThreadSafe>;
#else
using FReflectionHttpRequest = TSharedRef<IHttpRequest, ESPMode::ThreadSafe>;
using FReflectionHttpResponse = TSharedPtr<IHttpResponse, ESPMode::ThreadSafe>;
#endif

/* Marks a stretch of work that may have to park the game thread on a Cloud request.
 *
 * Reflection resolves asset references while it deserializes properties, recursively, so those
 * requests have no continuation to hand a callback to and have to be waited on. A scope keeps
 * the editor painting and gives the user a way out for as long as that lasts; a wait outside a
 * scope still works, it just looks like a hung editor.
 */
class REFLECTION_API FBlockingRequestScope {
public:
	explicit FBlockingRequestScope(const FText& Description);
	~FBlockingRequestScope();

	FBlockingRequestScope(const FBlockingRequestScope&) = delete;
	FBlockingRequestScope& operator=(const FBlockingRequestScope&) = delete;

	/* True while a scope is open anywhere up the call stack.
	 *
	 * A blocking wait drives the HTTP manager itself, so unrelated async completions land
	 * partway through whatever operation is parked on that wait. Anything that touches assets
	 * from a completion has to step out of that call stack first, and this is how it tells. */
	static bool IsActive();

	/* Repaints the editor and picks up a click on Cancel. Returns true once the user cancels. */
	static bool Pump();

private:
	/* Only game thread scopes take part: the progress UI is game thread only, and a request
	 * waited on from a worker thread isn't blocking the editor in the first place */
	bool bTracked = false;
	bool bOwnsProgress = false;
};

class REFLECTION_API FRemoteUtilities {
public:
	/* Sends HttpRequest and returns immediately. OnComplete runs on the game thread, exactly
	 * once, with a null response if the request never produced one. */
	static void ExecuteRequestAsync(FReflectionHttpRequest HttpRequest, TFunction<void(FReflectionHttpResponse)> OnComplete);

	/* Parks the calling thread until HttpRequest finishes, and returns null if it failed, ran
	 * past TimeoutSeconds, or was cancelled from the progress dialog.
	 *
	 * Only for callers that genuinely have nowhere to put a callback, which today means the
	 * serializer's recursive reference resolution. Everything else uses ExecuteRequestAsync.
	 * Wrap the owning operation in an FBlockingRequestScope so the wait stays visible and
	 * cancellable. */
	static FReflectionHttpResponse ExecuteRequestBlocking(FReflectionHttpRequest HttpRequest, float TimeoutSeconds = 30.0f);
};
