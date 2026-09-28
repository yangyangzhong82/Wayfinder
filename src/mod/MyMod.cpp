#include "mod/MyMod.h"

#include "ll/api/mod/RegisterHelper.h"
#include "wayfinder/Wayfinder.h"

namespace my_mod {

MyMod& MyMod::getInstance() {
    static MyMod instance;
    return instance;
}

bool MyMod::load() {
    try {
        mWayfinder = std::make_shared<wayfinder::Wayfinder>(getSelf());
        return true;
    } catch (std::exception const& ex) {
        getSelf().getLogger().error("Wayfinder configuration failed: {}", ex.what());
        return false;
    }
}

bool MyMod::enable() { return mWayfinder && mWayfinder->enable(); }

bool MyMod::disable() {
    if (mWayfinder) mWayfinder->disable();
    return true;
}

} // namespace my_mod

LL_REGISTER_MOD(my_mod::MyMod, my_mod::MyMod::getInstance());
