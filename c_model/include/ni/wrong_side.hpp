#pragma once
// Shared abort helper for Packetizer / Depacketizer "wrong side" stubs.
//
// A concrete NMU/NSU class implements only one half of each interface (the
// half matching its channel direction: NMU packetizes AW/W/AR, NSU packetizes
// B/R; symmetrically for depacketizers). The other half must still be
// overridden because the base interface declares them as pure-virtual; we
// override them with a stub that aborts.
//
// The type system normally prevents these stubs from being reached. Firing
// one means an upstream wiring or vtable-routing bug in the port adapter
// (e.g. AxiSlavePort dispatched a response into the Packetizer base of Rob
// instead of the Depacketizer base) or a test fixture invoking the wrong
// interface instance.
//
// [[noreturn]] lets stubs with non-void return types omit a synthetic
// return value (e.g. std::optional<axi::BBeat> pop_b()).
#include <cassert>
#include <cstdio>
#include <cstdlib>

namespace ni::cmodel {

[[noreturn]] inline void wrong_side_(const char* class_name, const char* method_name) {
    // Emit the precise endpoint to stderr before abort so the failure mode is
    // diagnosable in release builds (where the assert message is stripped).
    std::fprintf(stderr,
                 "[wrong_side] %s::%s called — Packetizer/Depacketizer wrong-side "
                 "dispatch; likely an upstream wiring or vtable-routing bug in the "
                 "port adapter or test fixture.\n",
                 class_name, method_name);
    assert(false && "Packetizer/Depacketizer wrong-side method called");
    std::abort();
}

}  // namespace ni::cmodel
