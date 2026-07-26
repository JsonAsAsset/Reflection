/* Copyright Reflection Contributors 2024-2026 */

#pragma once

#include "HttpModule.h"
#include "Engine/Compatibility.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "Modules/Log.h"

class REFLECTION_API FRemoteUtilities {
public:
	static TSharedPtr<IHttpResponse, ESPMode::ThreadSafe> ExecuteRequestSync(

		/* Different type declarations for HttpRequest on UE5 */
#if ENGINE_UE5
		TSharedRef<IHttpRequest> HttpRequest,
#else
		const TSharedRef<IHttpRequest, ESPMode::ThreadSafe>& HttpRequest,
#endif
		/* ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ */
		
		float LoopDelay = 0.02);

	static void ExecuteRequestAsync(

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
		)> OnComplete
	);
};

inline void SendHttpRequest(const FString& URL, TFunction<void(FHttpRequestPtr, FHttpResponsePtr, bool)> OnComplete, const FString& Verb = "GET", const FString& ContentType = "", const FString& Content = "") {
	FHttpModule* Http = &FHttpModule::Get();
	if (!Http) {
		UE_LOG(LogReflection, Error, TEXT("HTTP module not available"));
		return;
	}

	const auto Request = Http->CreateRequest();
	
	Request->SetURL(URL);
	Request->SetVerb(Verb);

	if (!ContentType.IsEmpty()) {
		Request->SetHeader(TEXT("Content-Type"), ContentType);
	}
    
	if (!Content.IsEmpty()) {
		Request->SetContentAsString(Content);
	}

	Request->OnProcessRequestComplete().BindLambda([OnComplete](const FHttpRequestPtr& RequestPtr, const FHttpResponsePtr& Response, const bool bWasSuccessful) {
		OnComplete(RequestPtr, Response, bWasSuccessful);
	});

	Request->ProcessRequest();
}