#pragma once
#include <Windows.h>
#include <memory>
#include <optional>
#include <string>

namespace wayfinder {
// Windows owns editing, selection and IME composition. The UI worker exchanges
// only strings/results; it never calls the game or a Wayfinder callback.
class NativeTextInput {
public:
    struct Request {
        std::string title, text, hint, accept, cancel;
        RECT anchor{};
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
