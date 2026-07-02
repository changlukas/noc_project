// Typed handle block + process-wide registry for multi-instance DPI.
#ifndef WRAP_HANDLE_BLOCK_HPP
#define WRAP_HANDLE_BLOCK_HPP

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_set>

namespace ni::cmodel::wrap {

enum class WrapType : uint32_t {
    Master = 0x4D415354u,  // 'MAST'
    Slave = 0x534C4156u,   // 'SLAV'
    Nmu = 0x4E4D5520u,     // 'NMU '
    Nsu = 0x4E535520u,     // 'NSU '
    Router = 0x524F5554u,  // 'ROUT'
};

enum class HandleState { Live };  // closed handles are removed from registry

struct HandleBlock {
    uint32_t magic;
    WrapType type;
    HandleState state;
    std::string name;
    // type-erased; the deleter MUST cast to the concrete adapter type before
    // delete (e.g. [](void* p){ delete static_cast<NmuWrap*>(p); }).
    // Deleting via void* skips the destructor → UB.
    std::unique_ptr<void, void (*)(void*)> adapter;
};

extern std::unordered_set<HandleBlock*> g_handle_registry;

}  // namespace ni::cmodel::wrap

#endif  // WRAP_HANDLE_BLOCK_HPP
