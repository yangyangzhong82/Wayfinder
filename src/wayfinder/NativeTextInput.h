#pragma once
#include <Windows.h>
#include <memory>
#include <optional>
#include <string>

namespace wayfinder {
// Windows owns editing, selection and IME composition. Owned popup fields are
// created asynchronously on the owner's window thread; only results cross back
// to game callbacks. Standalone dialogs use a separate UI worker.
class NativeTextInput {
public:
    struct Request {
        std::string title, text, hint, accept, cancel;
        RECT anchor{};
        HWND owner{};
    };
    struct Result { bool accepted{}; std::string text, error; };
    NativeTextInput();
    ~NativeTextInput();
    NativeTextInput(NativeTextInput const&) = delete;
    NativeTextInput& operator=(NativeTextInput const&) = delete;
    void start(Request request);
    bool active() const;
    void cancel();
    void shutdown();
    std::optional<Result> take();
private:
    struct Impl;
    std::unique_ptr<Impl> mImpl;
};
} // namespace wayfinder
