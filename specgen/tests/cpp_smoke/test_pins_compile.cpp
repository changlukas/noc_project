// Smoke-compile test for ni::pins::*Pins bundle structs.
//
// Compiled by tests/test_codegen.py::test_pins_bundle_compiles_with_gxx.
// Verifies the generated bundle structs build cleanly against the
// ni::signals::*_RESET constants and that reset_outputs() resolves.
#include "ni_signals.h"
#include <cstdio>

int main() {
    ni::pins::AxiSlavePortPins slv{};
    slv.reset_outputs();

    ni::pins::AxiMasterPortPins mst{};
    mst.reset_outputs();

    ni::pins::NocIntfMosiPins noc_mosi{};
    noc_mosi.reset_outputs();

    ni::pins::CsrPins csr{};
    csr.reset_outputs();

    ni::pins::NocIntfMisoPins noc_miso{};
    noc_miso.reset_outputs();

    std::printf("AxiSlavePortPins sizeof=%zu\n", sizeof(slv));
    std::printf("CsrPins sizeof=%zu\n", sizeof(csr));
    return 0;
}
