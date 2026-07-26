/* Copyright Reflection Contributors 2024-2026 */

#include "Utilities/RemoteUtilities.h"

#include "HttpManager.h"
#include "HttpModule.h"
#include "Modules/Log.h"

#if ENGINE_UE5
TSharedPtr<IHttpResponse>
#else
TSharedPtr<IHttpResponse, ESPMode::ThreadSafe>
#endif
	FRemoteUtilities::ExecuteRequestSync(

	/* Different type declarations for HttpRequest on UE5 */
#if ENGINE_UE5
	TSharedRef<IHttpRequest> HttpRequest
#else
	const TSharedRef<IHttpRequest, ESPMode::ThreadSafe>& HttpRequest
#endif
	/* ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ */
	
	, const float LoopDelay)
{
	const bool StartedRequest = HttpRequest->ProcessRequest();
	if (!StartedRequest) {
		UE_LOG(LogReflection, Error, TEXT("Failed to start HTTP Request."));
		return nullptr;
	}

	double LastTime = FPlatformTime::Seconds();
	while (EHttpRequestStatus::Processing == HttpRequest->GetStatus()) {
		const double AppTime = FPlatformTime::Seconds();
		FHttpModule::Get().GetHttpManager().Tick(AppTime - LastTime);
		
		LastTime = AppTime;
		FPlatformProcess::Sleep(LoopDelay);
	}

	return HttpRequest->GetResponse();
}

void FRemoteUtilities::ExecuteRequestAsync(
#if ENGINE_UE5
	TSharedRef<IHttpRequest> HttpRequest,
#else
	const TSharedRef<IHttpRequest, ESPMode::ThreadSafe>& HttpRequest,
#endif

	TFunction<void(
#if ENGINE_UE5
		TSharedPtr<IHttpResponse>
#else
		TSharedPtr<IHttpResponse, ESPMode::ThreadSafe>
#endif
	)> OnComplete)
{
	HttpRequest->OnProcessRequestComplete().BindLambda(
		[OnComplete](
			FHttpRequestPtr Request,
			const FHttpResponsePtr& Response,
			const bool bSuccess) {
			if (!bSuccess || !Response.IsValid()) {
				UE_LOG(LogReflection, Error, TEXT("HTTP Request failed."));
				OnComplete(nullptr);
				
				return;
			}

			OnComplete(Response);
		}
	);

	if (!HttpRequest->ProcessRequest()) {
		UE_LOG(LogReflection, Error, TEXT("Failed to start HTTP Request."));
		OnComplete(nullptr);
	}
}