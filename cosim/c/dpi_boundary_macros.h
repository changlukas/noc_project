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

namespace ni::cmodel::cosim {
extern std::atomic<int> g_dpi_error_code;
extern std::string g_dpi_error_msg;
}  // namespace ni::cmodel::cosim

// Usage:
//   extern "C" void cmodel_foo() {
//       DPI_BOUNDARY_BEGIN(cmodel_foo) {
//           // body
//       } DPI_BOUNDARY_END(cmodel_foo);
//   }

// First-error-wins: skip overwrite if a prior error is already latched.
#define DPI_SET_ERR_IF_CLEAR(code_expr, msg_expr)                 \
    do {                                                          \
        int prior = ni::cmodel::cosim::g_dpi_error_code.load();   \
        if (prior == CMODEL_DPI_OK) {                             \
            ni::cmodel::cosim::g_dpi_error_code.store(code_expr); \
            ni::cmodel::cosim::g_dpi_error_msg = (msg_expr);      \
        }                                                         \
    } while (0)

#define DPI_BOUNDARY_BEGIN(fn_name) try
#define DPI_BOUNDARY_END(fn_name)                                                            \
    catch (const std::exception& e) {                                                        \
        DPI_SET_ERR_IF_CLEAR(CMODEL_DPI_ERR_GENERIC, std::string(#fn_name ": ") + e.what()); \
    }                                                                                        \
    catch (...) {                                                                            \
        DPI_SET_ERR_IF_CLEAR(CMODEL_DPI_ERR_UNKNOWN, std::string(#fn_name) + ": ...");       \
    }

#define DPI_BOUNDARY_BEGIN_R(fn_name, fail_value) \
    auto _dpi_fail_value = (fail_value);          \
    try
#define DPI_BOUNDARY_END_R(fn_name)                                                          \
    catch (const std::exception& e) {                                                        \
        DPI_SET_ERR_IF_CLEAR(CMODEL_DPI_ERR_GENERIC, std::string(#fn_name ": ") + e.what()); \
        return _dpi_fail_value;                                                              \
    }                                                                                        \
    catch (...) {                                                                            \
        DPI_SET_ERR_IF_CLEAR(CMODEL_DPI_ERR_UNKNOWN, std::string(#fn_name) + ": ...");       \
        return _dpi_fail_value;                                                              \
    }

// Per-shell DPI handlers (set_inputs/tick/get_outputs) all guard against a null
// adapter (cmodel_init not yet called). REQUIRE_ADAPTER deduplicates the check:
// on null adapter it sets CMODEL_DPI_ERR_NOT_INITIALIZED + an error message
// including the function name (so cmodel_check_error reports which shell
// faulted), then returns from the handler. Handler return type must be void.
#define REQUIRE_ADAPTER(adapter_ptr, fn_name)                                                      \
    do {                                                                                           \
        if (!(adapter_ptr)) {                                                                      \
            ni::cmodel::cosim::g_dpi_error_code.store(CMODEL_DPI_ERR_NOT_INITIALIZED);             \
            ni::cmodel::cosim::g_dpi_error_msg = std::string(fn_name) + ": " #adapter_ptr " null"; \
            return;                                                                                \
        }                                                                                          \
    } while (0)

#endif  // COSIM2_DPI_BOUNDARY_MACROS_H
