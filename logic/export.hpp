// generates export.hpp for each module from compat/linkage.hpp

#pragma once

#include "compat/linkage-macros.hpp"

#ifdef BUILD_LOGIC
#   define OTR_LOGIC_EXPORT OTR_GENERIC_EXPORT
#else
#   define OTR_LOGIC_EXPORT OTR_GENERIC_IMPORT
#endif
