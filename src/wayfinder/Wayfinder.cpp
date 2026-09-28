#include "wayfinder/WayfinderInternal.h"
#include "ll/api/mod/NativeMod.h"

namespace wayfinder {
Wayfinder::Impl::Impl(ll::mod::NativeMod& owner)
    : mod(owner), settings(loadSettings(owner.getConfigDir())), session(settings.maxCachedChunks) {}

template <class Function> void Wayfinder::Impl::guarded(Function&& function) {
    std::lock_guard lock(mutex);
    if (!enabled) return;
    try {
        function();
    } catch (std::exception const& ex) {
        if (Clock::now() - lastError > std::chrono::seconds(5)) {
            mod.getLogger().error("Wayfinder: {}", ex.what());
            lastError = Clock::now();
        }
        input.closeMap(*this, false);
    }
}

template <class Event, class Function>
void Wayfinder::Impl::subscribe(std::weak_ptr<Wayfinder> weak, Function handler) {
    auto listener = ll::event::EventBus::getInstance().emplaceListener<Event>([weak, handler](Event& event) {
        if (auto self = weak.lock()) self->mImpl->guarded([&] { handler(*self->mImpl, event); });
    });
    if (!listener) throw std::runtime_error("Unable to register Wayfinder event listener");
    listeners.push_back(std::move(listener));
}

Wayfinder::Wayfinder(ll::mod::NativeMod& mod) : mImpl(std::make_unique<Impl>(mod)) {}
Wayfinder::~Wayfinder() { disable(); }
bool Wayfinder::enable() {
    std::unique_lock lock(mImpl->mutex);
    if (mImpl->enabled) return true;
    mImpl->enabled = true;
    try {
        mImpl->input.mapMouse = std::make_unique<MapMouse>();
        auto weak = weak_from_this();
        mImpl->subscribe<ll::event::ClientLevelTickEvent>(weak,
                                                          [](Impl& app, auto& event) { app.session.tick(app, event); });
        mImpl->subscribe<ll::event::BlockChangedEvent>(weak,
                                                       [](Impl& app, auto& event) { app.session.changed(event); });
        mImpl->subscribe<ll::event::ClientExitLevelEvent>(weak,
                                                          [](Impl& app, auto& event) { app.session.exit(app, event); });
        mImpl->subscribe<ll::event::KeyInputEvent>(weak, [](Impl& app, auto& event) { app.input.key(app, event); });
        mImpl->subscribe<ll::event::MouseInputEvent>(weak, [](Impl& app, auto& event) { app.input.mouse(app, event); });
        mImpl->subscribe<ll::event::AfterUIRenderEvent>(
            weak, [](Impl& app, auto& event) { app.rendering.render(app, event); });
        mImpl->mod.getLogger().info("Wayfinder enabled. World map key: {}; minimap key: {} (Windows VK codes).",
                                    mImpl->settings.fullMapKey, mImpl->settings.toggleMinimapKey);
        return true;
    } catch (std::exception const& ex) {
        mImpl->mod.getLogger().error("Wayfinder enable failed: {}", ex.what());
        lock.unlock();
        disable();
        return false;
    }
}
void Wayfinder::disable() {
    std::vector<ll::event::ListenerPtr> listeners;
    {
        std::lock_guard lock(mImpl->mutex);
        if (!mImpl->enabled) return;
        mImpl->enabled = false;
        listeners.swap(mImpl->listeners);
    }
    // Removing hooks must not wait for callbacks while holding the callback mutex.
    for (auto const& listener : listeners)
        ll::event::EventBus::getInstance().removeListener(listener);
    std::unique_lock lock(mImpl->mutex);
    try {
        mImpl->input.closeMap(*mImpl, true);
        mImpl->session.end(*mImpl);
    } catch (std::exception const& ex) {
        mImpl->mod.getLogger().error("Wayfinder shutdown: {}", ex.what());
        if (mImpl->input.mapMouse) mImpl->input.mapMouse->release();
        mImpl->session.client = nullptr;
    }
    mImpl->session.history.finish(*mImpl);
    mImpl->rendering.renderer.reset();
    auto mapMouse = std::move(mImpl->input.mapMouse);
    lock.unlock();
    mapMouse.reset();
}
} // namespace wayfinder
