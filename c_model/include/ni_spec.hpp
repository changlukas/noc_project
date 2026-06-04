// c_model C++ umbrella — #include this in any c_model class header that needs
// codegen-elaborated symbols. Adds no logic of its own.
#pragma once

#include "ni_flit_constants.h"  // ni::FLIT_WIDTH, ni::header::*, ni::payload::*
#include "ni_signals.h"         // ni::signals::*_RESET, ni::pins::*Pins structs
#include "ni_regs.h"            // ni::regs::*_OFFSET, ni::regs::csr_policy::*
