#pragma once

#include "Inline/BasicTypes.h"
#include "Platform/Defines.h"

namespace Platform
{
	struct Thread;
	PLATFORM_API Thread* createThread(Uptr numStackBytes,
									  I64 (*threadEntry)(void*),
									  void* argument);
	PLATFORM_API void detachThread(Thread* thread);
	PLATFORM_API I64 joinThread(Thread* thread);
	[[noreturn]] PLATFORM_API void exitThread(I64 code);

	RETURNS_TWICE PLATFORM_API Thread* forkCurrentThread();
}