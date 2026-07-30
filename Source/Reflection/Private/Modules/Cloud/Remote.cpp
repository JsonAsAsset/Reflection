/* Copyright Reflection Contributors 2024-2026 */

#include "Modules/Cloud/Remote.h"

#include "HttpManager.h"
#include "HttpModule.h"
#include "Misc/ScopedSlowTask.h"
#include "Engine/Log.h"

namespace {
	/* How long a blocking wait sits between HTTP ticks: short enough that a quick reply isn't
	 * held up, long enough not to spin a core for the duration */
	constexpr float BlockingPollInterval = 0.01f;

	/* Progress only appears once a wait has lasted long enough for the user to notice it, so
	 * operations the Cloud answers immediately never flash a dialog */
	constexpr float BlockingProgressDelay = 0.4f;

	int32 GBlockingScopeDepth = 0;

	/* Scoped rather than bare FSlowTask: the scoped form is the one that registers itself with
	 * the feedback context on construction and unregisters on destruction */
	TUniquePtr<FScopedSlowTask> GBlockingProgress;

	/* One entry per open scope, in nesting order. Pump() always shows the top, so the dialog
	 * tracks whatever the innermost scope is doing right now rather than freezing on the
	 * outermost scope's generic description for the length of the whole operation. */
	TArray<FText> GDescriptionStack;

	/* Entering a progress frame ticks Slate, and that tick can reach code that starts a blocking
	 * request of its own. Ticking Slate from inside a Slate tick tears widgets down underneath
	 * the tick that is still walking them, so the inner pump gives up its repaint instead. */
	bool GPumping = false;
}

FBlockingRequestScope::FBlockingRequestScope(const FText& Description) {
	bTracked = IsInGameThread();

	if (!bTracked) {
		return;
	}

	GDescriptionStack.Push(Description);

	bOwnsProgress = GBlockingScopeDepth++ == 0;

	if (bOwnsProgress) {
		/* Zero total work: a wait has no measurable length to report, the task exists to keep
		 * the editor drawn and to carry the Cancel button */
		GBlockingProgress = MakeUnique<FScopedSlowTask>(0.0f, Description);
		GBlockingProgress->MakeDialogDelayed(BlockingProgressDelay, true);
	}
}

FBlockingRequestScope::~FBlockingRequestScope() {
	if (!bTracked) {
		return;
	}

	GBlockingScopeDepth--;
	GDescriptionStack.Pop();

	if (bOwnsProgress) {
		GBlockingProgress.Reset();
	}
}

bool FBlockingRequestScope::IsActive() {
	return GBlockingScopeDepth > 0;
}

bool FBlockingRequestScope::Pump() {
	/* The progress task belongs to the game thread, and a wait on a worker thread is not what
	 * this is here to keep alive anyway */
	if (!GBlockingProgress.IsValid() || !IsInGameThread() || GPumping) {
		return false;
	}

	/* The innermost open scope is whatever is actually happening right now, so its description
	 * is what the dialog shows. The FrameMessage this sets overrides the outer scope's
	 * DefaultMessage until the next call retargets it or the nested scope closes */
	const FText CurrentDescription = GDescriptionStack.Num() > 0 ? GDescriptionStack.Top() : FText::GetEmpty();

	GPumping = true;

	/* Entering a frame is what gets the editor repainted and the dialog's input read */
	GBlockingProgress->EnterProgressFrame(0.0f, CurrentDescription);

	GPumping = false;

	/* The repaint runs arbitrary editor code, and a scope closing in there takes the task with
	 * it */
	return GBlockingProgress.IsValid() && GBlockingProgress->ShouldCancel();
}

void FRemoteUtilities::ExecuteRequestAsync(FReflectionHttpRequest HttpRequest, TFunction<void(FReflectionHttpResponse)> OnComplete) {
	/* A failed ProcessRequest may or may not have already reported through the delegate, and
	 * OnComplete has to run exactly once either way */
	const TSharedRef<bool, ESPMode::ThreadSafe> Reported = MakeShared<bool, ESPMode::ThreadSafe>(false);

	HttpRequest->OnProcessRequestComplete().BindLambda(
		[OnComplete, Reported](FHttpRequestPtr Request, FHttpResponsePtr Response, const bool bSuccess) {
			if (*Reported) {
				return;
			}

			*Reported = true;

			if (!bSuccess || !Response.IsValid()) {
				const FString RequestURL = Request.IsValid() ? Request->GetURL() : TEXT("<unknown>");

				UE_LOG(LogReflection, Warning, TEXT("HTTP request failed: \"%s\""), *RequestURL);
				OnComplete(nullptr);

				return;
			}

			OnComplete(Response);
		}
	);

	if (!HttpRequest->ProcessRequest() && !*Reported) {
		*Reported = true;

		UE_LOG(LogReflection, Error, TEXT("Failed to start HTTP request: \"%s\""), *HttpRequest->GetURL());
		OnComplete(nullptr);
	}
}

FReflectionHttpResponse FRemoteUtilities::ExecuteRequestBlocking(FReflectionHttpRequest HttpRequest, const float TimeoutSeconds) {
	const FString RequestURL = HttpRequest->GetURL();

	if (!HttpRequest->ProcessRequest()) {
		UE_LOG(LogReflection, Error, TEXT("Failed to start HTTP request: \"%s\""), *RequestURL);

		return nullptr;
	}

	double LastTime = FPlatformTime::Seconds();
	const double Deadline = LastTime + TimeoutSeconds;

	while (EHttpRequestStatus::Processing == HttpRequest->GetStatus()) {
		const double CurrentTime = FPlatformTime::Seconds();

		/* Nothing else is driving the manager while this thread is parked here */
		FHttpModule::Get().GetHttpManager().Tick(static_cast<float>(CurrentTime - LastTime));
		LastTime = CurrentTime;

		if (FBlockingRequestScope::Pump()) {
			UE_LOG(LogReflection, Log, TEXT("HTTP request cancelled: \"%s\""), *RequestURL);
			HttpRequest->CancelRequest();

			return nullptr;
		}

		if (CurrentTime >= Deadline) {
			UE_LOG(LogReflection, Error, TEXT("HTTP request timed out after %.0f seconds: \"%s\""), TimeoutSeconds, *RequestURL);
			HttpRequest->CancelRequest();

			return nullptr;
		}

		FPlatformProcess::Sleep(BlockingPollInterval);
	}

	return HttpRequest->GetResponse();
}
