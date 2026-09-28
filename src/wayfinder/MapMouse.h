#pragma once
#include <memory>

class IClientInstance;
namespace wayfinder {
// Scoped interception of the engine's capture request, before it can warp the
// native pointer. No global Win32 hooks or cursor visibility counters.
class MapMouse {
public:
    MapMouse();
    ~MapMouse();
    void acquire(IClientInstance& client);
    void release();

private:
    struct Impl;
    std::unique_ptr<Impl> mImpl;
};
} // namespace wayfinder
