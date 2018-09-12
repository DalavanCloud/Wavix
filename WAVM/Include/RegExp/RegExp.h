#pragma once

#include "Inline/BasicTypes.h"
#include "NFA/NFA.h"

#ifndef REGEXP_API
#define REGEXP_API DLL_IMPORT
#endif

namespace RegExp
{
	// Parses a regular expression from a string, and adds a recognizer for it to the given NFA
	// The recognizer will start from initialState, and end in finalState when the regular
	// expression has been completely matched.
	REGEXP_API void addToNFA(const char* regexpString,
							 NFA::Builder* nfaBuilder,
							 NFA::StateIndex initialState,
							 NFA::StateIndex finalState);
}
