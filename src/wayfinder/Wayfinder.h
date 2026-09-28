#pragma once
#include <memory>
namespace ll::mod {
class NativeMod;
}
namespace wayfinder {
class Wayfinder : public std::enable_shared_from_this<Wayfinder> {
public:
    explicit Wayfinder(ll::mod::NativeMod& mod);
    ~Wayfinder();
    bool enable();
    void disable();

private:
    struct Impl;
    std::unique_ptr<Impl> mImpl;
};
} // namespace wayfinder
