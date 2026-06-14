// Sample: 用編譯時常數打包一個 flit header。
//
// 這個檔示範 C-model 怎麼用 codegen 出來的 constants：
//   1. #include 自動產生的 ni_flit_constants.h
//   2. 編譯後常數變 constexpr int，compiler 直接展開成 immediate
//   3. C++ binary 完全不依賴 Python（Python 只在 build 階段跑）
//
// 編譯（任一）:
//   g++   -std=c++17 -I include examples/use_constants.cpp -o use_constants.exe
//   cl.exe /std:c++17 /I include examples\use_constants.cpp /Fe:use_constants.exe
//
// 跑:
//   .\use_constants.exe

#include <cstdio>
#include <cstdint>
#include "ni_flit_constants.h"

int main() {
    std::printf("=== NI packet constants (from generated header) ===\n");
    std::printf("FLIT_WIDTH       = %d\n", ni::FLIT_WIDTH);
    std::printf("HEADER_WIDTH     = %d\n", ni::HEADER_WIDTH);
    std::printf("PAYLOAD_WIDTH    = %d\n", ni::PAYLOAD_WIDTH);
    std::printf("FLIT_DATA_WIDTH  = %d\n", ni::FLIT_DATA_WIDTH);
    std::printf("LINK_WIDTH       = %d\n", ni::LINK_WIDTH);
    std::printf("\n");

    std::printf("=== Header field bit positions ===\n");
    std::printf("axi_ch    at [%2d:%2d] (width %d)\n", ni::header::AXI_CH_MSB,
                ni::header::AXI_CH_LSB, ni::header::AXI_CH_WIDTH);
    std::printf("dst_id    at [%2d:%2d] (width %d)\n", ni::header::DST_ID_MSB,
                ni::header::DST_ID_LSB, ni::header::DST_ID_WIDTH);
    std::printf("rob_idx   at [%2d:%2d] (width %d)\n", ni::header::ROB_IDX_MSB,
                ni::header::ROB_IDX_LSB, ni::header::ROB_IDX_WIDTH);
    std::printf("\n");

    std::printf("=== Payload widths ===\n");
    std::printf("AW=%d  W=%d  AR=%d  B=%d  R=%d\n", ni::payload::AW_WIDTH, ni::payload::W_WIDTH,
                ni::payload::AR_WIDTH, ni::payload::B_WIDTH, ni::payload::R_WIDTH);
    std::printf("\n");

    // Demo: 用 constexpr 常數實際打包一個 AR header
    // 假設目的地 dst_id=0x12 (x=2, y=1), src_id=0x05, axi_ch=AR(2),
    //         last=1, rob_req=1, rob_idx=7
    // (noc_qos / vc_id / route_par / commtype / multicast / flit_ecc 留 0 不 demo 打包)
    std::printf("=== Demo: pack an AR header using compile-time constants ===\n");
    uint64_t hdr_lo = 0;                                  // bits 0..63 of header
    hdr_lo |= (uint64_t)(0x2) << ni::header::AXI_CH_LSB;  // AR
    hdr_lo |= (uint64_t)(0x05) << ni::header::SRC_ID_LSB;
    hdr_lo |= (uint64_t)(0x12) << ni::header::DST_ID_LSB;
    hdr_lo |= (uint64_t)(0x1) << ni::header::LAST_LSB;
    hdr_lo |= (uint64_t)(0x1) << ni::header::ROB_REQ_LSB;
    hdr_lo |= (uint64_t)(0x7) << ni::header::ROB_IDX_LSB;
    std::printf("header[63:0] = 0x%016llX\n", (unsigned long long)hdr_lo);

    return 0;
}
