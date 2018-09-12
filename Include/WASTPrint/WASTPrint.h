#pragma once

#ifndef WASTPRINT_API
#define WASTPRINT_API DLL_IMPORT
#endif

#include <string>
#include "Inline/BasicTypes.h"

namespace IR
{
	struct Module;
}

namespace WAST
{
	// Prints a module in WAST format.
	WASTPRINT_API std::string print(const IR::Module& module);
}
