#pragma once
// flit_builders.hpp — minimal router-test flits, shared where the definition
// was byte-identical across test files. Builders whose fields or defaults
// differ per test stay in their own file: a shared builder that silently
// changes a test's header fields is worse than the duplicate.
#include "flit.hpp"

#include <cstdint>

namespace ni::cmodel::testing {

inline Flit make_flit(uint8_t dst, uint8_t vc, uint64_t flit_tail) {
    Flit f;
    f.set_header_field("dst_id", dst);
    f.set_header_field("vc_id", vc);
    f.set_header_field("flit_tail", flit_tail);
    return f;
}

inline Flit make_unicast_flit(uint8_t dst, uint8_t src, uint8_t vc, uint64_t flit_tail,
                              uint8_t tag) {
    Flit f;
    f.set_header_field("dst_id", dst);
    f.set_header_field("src_id", src);
    f.set_header_field("vc_id", vc);
    f.set_header_field("flit_tail", flit_tail);
    f.set_header_field("ordering_tag", tag);
    return f;
}

}  // namespace ni::cmodel::testing
