// Typed handle block + process-wide registry for multi-instance DPI.
#ifndef COSIM_HANDLE_BLOCK_HPP
#define COSIM_HANDLE_BLOCK_HPP

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_set>

namespace ni::cmodel::cosim {

enum class ShellType : uint32_t {
    Master = 0x4D415354u,        // 'MAST'
    Slave = 0x534C4156u,         // 'SLAV'
    Nmu = 0x4E4D5520u,           // 'NMU '
    Nsu = 0x4E535520u,           // 'NSU '
    ChannelModel = 0x434D4D20u,  // 'CMM '
};

enum class HandleState { Live };  // closed handles are removed from registry

struct HandleBlock {
    uint32_t magic;
    ShellType type;
    HandleState state;
    std::string name;
    std::unique_ptr<void, void (*)(void*)> adapter;  // type-erased
};

extern std::unordered_set<HandleBlock*> g_handle_registry;

}  // namespace ni::cmodel::cosim

#endif  // COSIM_HANDLE_BLOCK_HPP
