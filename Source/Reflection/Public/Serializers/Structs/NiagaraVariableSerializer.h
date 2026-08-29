/* Copyright Reflection Contributors 2024-2026 */

#pragma once

#include "StructSerializer.h"
#include "FallbackStructSerializer.h"

class UPropertySerializer;

/* A Niagara variable, whose type does not arrive with the rest of it.
 *
 * The variable keeps its type as a handle into a registry, and the handle holds nothing but the
 * number that registry gave it, written and read by an archive of its own rather than as a
 * property. Nothing runs that archive here, so the handle is never set and every variable keeps
 * what its constructor gave it, which is Vec4. A float reads sixteen bytes, a whole collection of
 * them draws as four component vectors, and the last one reads off the end of whatever holds them.
 *
 * The export says the type plainly beside the name, so it is read back and put on afterwards.
 *
 * Registered against the base every kind of variable is built on, so it covers the ones a
 * parameter store keeps, the ones a collection lists, and the ones a data channel declares. */
class FNiagaraVariableSerializer : public FStructSerializer {
	FFallbackStructSerializer Fallback;

public:
	explicit FNiagaraVariableSerializer(UPropertySerializer* PropertySerializer) : Fallback(PropertySerializer) {}

	virtual void Deserialize(UScriptStruct* Struct, void* StructData, const TSharedPtr<FJsonObject> JsonValue, UObject* OptionalOuter) override;
};
