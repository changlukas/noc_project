// DPI boundary try/catch macros — every extern "C" DPI handler MUST wrap its
// body in DPI_BOUNDARY_BEGIN/END so C++ exceptions do not cross the DPI ABI
// (which is IEEE 1800 undefined behavior). Per Stage 5b spec §5.2.
//
// On exception: sets g_dpi_error_{code,msg}, returns silently. The SV side
// polls cmodel_check_error() at each shell's always_ff end and raises $fatal.

#ifndef COSIM2_DPI_BOUNDARY_MACROS_H
#define COSIM2_DPI_BOUNDARY_MACROS_H

#include "cmodel_dpi.h"
#include <atomic>
#include <exception>
#include <string>

namespace ni::cmodel::cosim2 {
extern std::atomic<int> g_dpi_error_code;
extern std::string      g_dpi_error_msg;
}  // namespace ni::cmodel::cosim2

// Usage:
//   extern "C" void cmodel_foo() {
//       DPI_BOUNDARY_BEGIN(cmodel_foo) {
//           // body
//       } DPI_BOUNDARY_END(cmodel_foo);
//   }
#define DPI_BOUNDARY_BEGIN(fn_name) try
#define DPI_BOUNDARY_END(fn_name)                                                   \
    catch (const std::exception& e) {                                               \
        ni::cmodel::cosim2::g_dpi_error_code.store(CMODEL_DPI_ERR_GENERIC);        \
        ni::cmodel::cosim2::g_dpi_error_msg =                                      \
            std::string(#fn_name ": ") + e.what();                                 \
    }                                                                               \
    catch (...) {                                                                   \
        ni::cmodel::cosim2::g_dpi_error_code.store(CMODEL_DPI_ERR_UNKNOWN);        \
        ni::cmodel::cosim2::g_dpi_error_msg = std::string(#fn_name) + ": ...";    \
    }

#endif  // COSIM2_DPI_BOUNDARY_MACROS_H
