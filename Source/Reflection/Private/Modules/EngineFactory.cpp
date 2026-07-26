/* Copyright Reflection Contributors 2024-2026 */

#include "Modules/EngineFactory.h"

UREngineImplementation::UREngineImplementation() {
	Formats.Add("json;Plugin");
	SupportedClass = UObject::StaticClass();

	bEditorImport = true;
	bText = false;
}

bool UREngineImplementation::FactoryCanImport(const FString& Filename) {
	return false;
}

UObject* UREngineImplementation::Import(const FString& Filename) {
	return nullptr;
}

UObject* UREngineImplementation::FactoryCreateFile(UClass* InClass, UObject* InParent, FName InName, EObjectFlags Flags, const FString& Filename, const TCHAR* Params, FFeedbackContext* Warn, bool& bOutOperationCanceled) {
	return Import(Filename);
}