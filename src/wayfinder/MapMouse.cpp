#include "wayfinder/MapMouse.h"
#include "ll/api/memory/Hook.h"
#include "mc/client/game/ClientInstance.h"
#include <atomic>

namespace wayfinder {
namespace {
// Detours only compare identity. They never dereference a Wayfinder instance or
// take its callback mutex (including during hook removal).
std::atomic<IClientInstance const*> mapOwner{};
} // namespace
LL_TYPE_INSTANCE_HOOK(
    MapGrabMouseHook,
    ll::memory::HookPriority::High,
    ClientInstance,
    &ClientInstance::$grabMouse,
    void
) {
    if (mapOwner.load(std::memory_order_acquire) == static_cast<IClientInstance*>(this)) return;
    origin();
}
LL_TYPE_INSTANCE_HOOK(
    MapUiCursorHook,
    ll::memory::HookPriority::High,
    ClientInstance,
    &ClientInstance::$shouldRenderUICursor,
    bool
) {
    if (mapOwner.load(std::memory_order_acquire) == static_cast<IClientInstance const*>(this)) return false;
    return origin();
}
struct MapMouse::Impl {
    ll::memory::HookRegistrar<MapGrabMouseHook, MapUiCursorHook> hooks;
};
MapMouse::MapMouse() : mImpl(std::make_unique<Impl>()) {}
MapMouse::~MapMouse() { release(); }
void MapMouse::acquire(IClientInstance& client) { mapOwner.store(&client, std::memory_order_release); }
void MapMouse::release() { mapOwner.store(nullptr, std::memory_order_release); }
} // namespace wayfinder
