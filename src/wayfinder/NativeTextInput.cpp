#include "wayfinder/NativeTextInput.h"
#include <CommCtrl.h>
#include <imm.h>
#include <algorithm>
#include <condition_variable>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

namespace wayfinder {
namespace {
[[noreturn]] void inputError(char const* operation) {
    auto error = GetLastError();
    throw std::runtime_error(std::string("Text editor: ") + operation + " failed (Win32 " + std::to_string(error) + ")");
}
std::wstring wide(std::string const& text) {
    if (text.empty()) return {};
    int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (!count) throw std::runtime_error("Invalid UTF-8 name");
    std::wstring result(count, L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), result.data(), count);
    return result;
}
std::string utf8(std::wstring const& text) {
    if (text.empty()) return {};
    int count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (!count) throw std::runtime_error("Invalid UTF-8 name");
    std::string result(count, '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), result.data(), count, nullptr, nullptr);
    return result;
}
// Standard Win32 dialog template, without application resources or a registered
// window class. DWORD storage keeps the template and every control aligned.
struct DialogTemplate {
    std::vector<DWORD> data;
    std::size_t words{};
    void word(WORD value) {
        if (words % 2 == 0) data.push_back(value);
        else data.back() |= DWORD(value) << 16;
        ++words;
    }
    void dword(DWORD value) { word(WORD(value)); word(WORD(value >> 16)); }
    void string(std::wstring const& value) { for (auto ch : value) word(ch); word(0); }
    void control(DWORD style, short x, short y, short w, short h, WORD id, WORD type, std::wstring const& text) {
        if (words % 2) word(0);
        dword(style | WS_CHILD | WS_VISIBLE); dword(0);
        word(x); word(y); word(w); word(h); word(id);
        word(0xffff); word(type); string(text); word(0);
    }
    explicit DialogTemplate(NativeTextInput::Request const& request) {
        dword(WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_MODALFRAME | DS_SETFONT);
        dword(WS_EX_TOOLWINDOW); word(4);
        word(0); word(0); word(270); word(92);
        word(0); word(0); string(wide(request.title)); word(10); string(L"Segoe UI");
        control(SS_LEFT, 8, 8, 254, 23, 101, 0x82, wide(request.hint));
        control(WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL, 8, 34, 254, 16, 100, 0x81, L"");
        control(WS_TABSTOP | BS_DEFPUSHBUTTON, 148, 65, 54, 18, IDOK, 0x80, wide(request.accept));
        control(WS_TABSTOP | BS_PUSHBUTTON, 208, 65, 54, 18, IDCANCEL, 0x80, wide(request.cancel));
    }
};
} // namespace

struct NativeTextInput::Impl {
    static constexpr UINT checkFocusMessage = WM_APP + 1;
    Request request;
    std::mutex mutex;
    std::condition_variable completed;
    bool uiFinished{true};
    HWND window{};
    HWND edit{};
    HHOOK inputHook{};
    DWORD uiThread{};
    static thread_local Impl* inputOwner;
    bool cancelled{};
    bool modeless{}, closing{};
    std::optional<Result> result;
    std::thread worker;
    // Accessed only by the UI thread.
    bool composing{}, activated{}, focusCheckPending{};
    std::wstring value;
    Result outcome;

    explicit Impl(Request input) : request(std::move(input)), modeless(request.owner != nullptr), value(wide(request.text)) {}
    ~Impl() {
        cancel();
        if (worker.joinable()) worker.join();
        if (modeless && GetCurrentThreadId() == uiThread) {
            // Unload can run on the window thread itself; it cannot wait for
            // that same thread to process our posted cancellation/startup.
            if (edit) finishModeless(edit, false);
            else {
                MSG message{};
                PeekMessageW(&message, nullptr, openMessage(), openMessage(), PM_REMOVE);
            }
        }
        std::unique_lock lock(mutex);
        completed.wait(lock, [&] { return uiFinished; });
    }
    void cancel() {
        HWND target{};
        {
            std::lock_guard lock(mutex);
            cancelled = true;
            target = window;
        }
        // Game callbacks may hold the map mutex. Never wait for the UI thread.
        if (target) PostMessageW(target, WM_CLOSE, 0, 0);
    }
    void publish(Result next) {
        std::lock_guard lock(mutex);
        window = nullptr;
        if (cancelled) next.accepted = false;
        result = std::move(next);
        uiFinished = true;
        completed.notify_all();
    }
    void removeInputHook() {
        if (inputHook) {
            UnhookWindowsHookEx(inputHook);
            inputHook = nullptr;
        }
        if (inputOwner == this) inputOwner = nullptr;
    }
    static UINT openMessage() {
        static UINT const message = RegisterWindowMessageW(L"Wayfinder.NativeTextInput.Open");
        return message;
    }
    void finishModeless(HWND control, bool accepted) {
        if (!modeless || closing || control != edit) return;
        closing = true;
        Result next;
        next.accepted = accepted;
        try {
            if (accepted) {
                int length = GetWindowTextLengthW(control);
                std::wstring text(static_cast<std::size_t>(length) + 1, L'\0');
                text.resize(GetWindowTextW(control, text.data(), static_cast<int>(text.size())));
                next.text = utf8(text);
            }
        } catch (std::exception const& ex) {
            next.error = ex.what();
            next.accepted = false;
        }
        // Restore the game only when this editor still owns the foreground.
        // Cancelling after Alt+Tab must not activate the game again.
        auto foreground = GetForegroundWindow();
        if (GetFocus() == control && (foreground == control || foreground == request.owner)) {
            SetForegroundWindow(request.owner);
            SetFocus(request.owner);
        }
        DestroyWindow(control);
        // Publish only after focus callbacks and subclass cleanup have finished.
        publish(std::move(next));
    }
    static LRESULT CALLBACK ownerProc(HWND owner, UINT msg, WPARAM w, LPARAM l, UINT_PTR id, DWORD_PTR data) {
        // The owned editor is part of the map. Suppress only the transfer
        // between these two windows, including activation of the popup.
        if ((msg == WM_KILLFOCUS || msg == WM_SETFOCUS) && w == data) return 0;
        if (msg == WM_ACTIVATE && static_cast<DWORD_PTR>(l) == data) return 0;
        if (msg == WM_SETCURSOR && LOWORD(l) == HTCLIENT) {
            SetCursor(LoadCursorW(nullptr, w == data ? IDC_IBEAM : IDC_ARROW));
            return TRUE;
        }
        if (msg == WM_NCDESTROY) RemoveWindowSubclass(owner, ownerProc, id);
        return DefSubclassProc(owner, msg, w, l);
    }
    static LRESULT CALLBACK inputProc(int code, WPARAM w, LPARAM l) {
        if (code == HC_ACTION && w == PM_REMOVE) {
            auto& message = *reinterpret_cast<MSG*>(l);
            if (message.message == openMessage()) {
                auto self = reinterpret_cast<Impl*>(message.lParam);
                message.hwnd = nullptr;
                message.message = WM_NULL;
                message.wParam = message.lParam = 0;
                try {
                    self->startModeless();
                } catch (std::exception const& ex) {
                    self->closing = true;
                    if (self->edit) DestroyWindow(self->edit);
                    self->edit = nullptr;
                    self->removeInputHook();
                    self->publish({false, {}, ex.what()});
                }
                return CallNextHookEx(nullptr, code, w, l);
            }
        }
        auto self = inputOwner;
        if (code == HC_ACTION && w == PM_REMOVE && self && self->edit) {
            auto& message = *reinterpret_cast<MSG*>(l);
            if (message.hwnd == self->edit) {
                // The game pumps messages for its own input system, not a
                // modeless EDIT. Translate and dispatch this editor's messages
                // before that system can consume them (including IME/WM_CHAR).
                auto input = message;
                message.hwnd = nullptr;
                message.message = WM_NULL;
                message.wParam = message.lParam = 0;
                TranslateMessage(&input);
                DispatchMessageW(&input);
            }
        }
        return CallNextHookEx(nullptr, code, w, l);
    }
    static LRESULT CALLBACK editProc(HWND edit, UINT msg, WPARAM w, LPARAM l, UINT_PTR id, DWORD_PTR data) {
        auto& self = *reinterpret_cast<Impl*>(data);
        if (self.modeless && msg == checkFocusMessage) {
            self.focusCheckPending = false;
            if (self.closing) return 0;
            auto foreground = GetForegroundWindow();
            if (foreground == self.request.owner || foreground == edit) {
                // The game can reclaim focus while finishing the opening click.
                // This is still the same editing session, not a cancellation.
                SetActiveWindow(edit);
                if (GetFocus() != edit) SetFocus(edit);
            } else if (foreground && GetAncestor(foreground, GA_ROOTOWNER) != self.request.owner) {
                self.finishModeless(edit, false);
            }
            return 0;
        }
        if (msg == WM_SETCURSOR) {
            SetCursor(LoadCursorW(nullptr, IDC_IBEAM));
            return TRUE;
        }
        if (msg == WM_IME_STARTCOMPOSITION) self.composing = true;
        if (msg == WM_IME_ENDCOMPOSITION) self.composing = false;
        if (msg == WM_GETDLGCODE && self.composing)
            return DefSubclassProc(edit, msg, w, l) | DLGC_WANTALLKEYS;
        if (self.modeless && msg == WM_KEYDOWN && w == VK_RETURN) {
            if (!self.composing) self.finishModeless(edit, true);
            return 0;
        }
        if (self.modeless && msg == WM_KEYDOWN && w == VK_ESCAPE) {
            self.finishModeless(edit, false);
            return 0;
        }
        if (self.modeless && msg == WM_KEYDOWN && w == VK_TAB) {
            self.finishModeless(edit, true);
            return 0;
        }
        if (self.modeless && msg == WM_CLOSE) {
            self.finishModeless(edit, false);
            return 0;
        }
        if (self.modeless && !self.closing && (msg == WM_KILLFOCUS
            || (msg == WM_ACTIVATE && LOWORD(w) == WA_INACTIVE))) {
            // Focus loss is also sent during activation and IME transitions.
            // Recheck after the transition; never queue an unconditional close.
            if (!self.focusCheckPending) {
                self.focusCheckPending = PostMessageW(edit, checkFocusMessage, 0, 0) != FALSE;
            }
        }
        if (msg == WM_KEYDOWN && w == 'A' && (GetKeyState(VK_CONTROL) & 0x8000)) {
            SendMessageW(edit, EM_SETSEL, 0, -1);
            return 0;
        }
        if (msg == WM_NCDESTROY) {
            if (self.modeless) {
                self.removeInputHook();
                RemoveWindowSubclass(self.request.owner, ownerProc, reinterpret_cast<UINT_PTR>(&self));
            }
            if (self.edit == edit) self.edit = nullptr;
            {
                std::lock_guard lock(self.mutex);
                if (self.window == edit) self.window = nullptr;
            }
            RemoveWindowSubclass(edit, editProc, id);
            if (self.modeless && !self.closing) {
                self.closing = true;
                auto result = DefSubclassProc(edit, msg, w, l);
                self.publish({});
                return result;
            }
        }
        return DefSubclassProc(edit, msg, w, l);
    }
    static INT_PTR CALLBACK dialogProc(HWND dialog, UINT msg, WPARAM w, LPARAM l) {
        auto self = reinterpret_cast<Impl*>(GetWindowLongPtrW(dialog, DWLP_USER));
        if (msg == WM_INITDIALOG) {
            self = reinterpret_cast<Impl*>(l);
            SetWindowLongPtrW(dialog, DWLP_USER, l);
            bool cancelled;
            {
                std::lock_guard lock(self->mutex);
                self->window = dialog;
                cancelled = self->cancelled;
            }
            if (cancelled) { EndDialog(dialog, IDCANCEL); return TRUE; }
            auto edit = GetDlgItem(dialog, 100);
            SendMessageW(edit, EM_SETLIMITTEXT, 4096, 0);
            SetWindowTextW(edit, self->value.c_str());
            SendMessageW(edit, EM_SETSEL, 0, -1);
            if (!SetWindowSubclass(edit, editProc, 1, reinterpret_cast<DWORD_PTR>(self))) {
                self->outcome.error = "Unable to open text editor";
                EndDialog(dialog, IDCANCEL);
                return TRUE;
            }
            ImmAssociateContextEx(edit, nullptr, IACE_DEFAULT);
            RECT size{};
            GetWindowRect(dialog, &size);
            auto const& anchor = self->request.anchor;
            MONITORINFO info{};
            info.cbSize = sizeof(info);
            auto monitor = MonitorFromRect(&self->request.anchor, MONITOR_DEFAULTTONEAREST);
            if (GetMonitorInfoW(monitor, &info)) {
                auto width = size.right - size.left, height = size.bottom - size.top;
                auto x = std::clamp((anchor.left + anchor.right - width) / 2, info.rcWork.left,
                    std::max(info.rcWork.left, info.rcWork.right - width));
                auto y = std::clamp((anchor.top + anchor.bottom - height) / 2, info.rcWork.top,
                    std::max(info.rcWork.top, info.rcWork.bottom - height));
                SetWindowPos(dialog, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
            }
            SetFocus(edit);
            return FALSE;
        }
        if (!self) return FALSE;
        if (msg == WM_ACTIVATE) {
            if (LOWORD(w) != WA_INACTIVE) self->activated = true;
            else if (self->activated) EndDialog(dialog, IDCANCEL);
        }
        if (msg == WM_COMMAND && LOWORD(w) == IDOK) {
            if (self->composing) return TRUE;
            try {
                auto edit = GetDlgItem(dialog, 100);
                int length = GetWindowTextLengthW(edit);
                std::wstring value(static_cast<std::size_t>(length) + 1, L'\0');
                value.resize(GetWindowTextW(edit, value.data(), static_cast<int>(value.size())));
                self->outcome = {true, utf8(value), {}};
                EndDialog(dialog, IDOK);
            } catch (std::exception const&) {
                // Keep invalid/incomplete Unicode in the editor for correction.
                MessageBeep(MB_ICONWARNING);
            }
            return TRUE;
        }
        if (msg == WM_CLOSE || (msg == WM_COMMAND && LOWORD(w) == IDCANCEL)) {
            EndDialog(dialog, IDCANCEL);
            return TRUE;
        }
        if (msg == WM_DESTROY) {
            std::lock_guard lock(self->mutex);
            self->window = nullptr;
        }
        return FALSE;
    }
    void run() {
        try {
            INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_STANDARD_CLASSES};
            InitCommonControlsEx(&controls);
            DialogTemplate dialog(request);
            // No cross-thread owner: disabling or messaging the game window
            // during creation/destruction could deadlock a game callback.
            auto code = DialogBoxIndirectParamW(GetModuleHandleW(nullptr),
                reinterpret_cast<DLGTEMPLATE const*>(dialog.data.data()), nullptr, dialogProc,
                reinterpret_cast<LPARAM>(this));
            if (code == -1) outcome.error = "Unable to open text editor";
        } catch (std::exception const&) { outcome.error = "Unable to open text editor"; }
        std::lock_guard lock(mutex);
        window = nullptr;
        if (cancelled) outcome.accepted = false;
        result = std::move(outcome);
    }
    void scheduleModeless() {
        uiThread = GetWindowThreadProcessId(request.owner, nullptr);
        if (!uiThread) inputError("GetWindowThreadProcessId");
        if (!openMessage()) inputError("RegisterWindowMessageW");
        HMODULE module{};
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCWSTR>(&inputProc), &module)) inputError("GetModuleHandleExW");
        inputHook = SetWindowsHookExW(WH_GETMESSAGE, inputProc, module, uiThread);
        if (!inputHook) inputError("SetWindowsHookExW");
        {
            std::lock_guard lock(mutex);
            uiFinished = false;
        }
        if (!PostMessageW(request.owner, openMessage(), 0, reinterpret_cast<LPARAM>(this))) {
            auto error = GetLastError();
            removeInputHook();
            std::lock_guard lock(mutex);
            uiFinished = true;
            SetLastError(error);
            inputError("PostMessageW");
        }
    }
    void startModeless() {
        bool abort;
        {
            std::lock_guard lock(mutex);
            abort = cancelled;
        }
        if (abort) {
            removeInputHook();
            publish({});
            return;
        }
        RECT client{};
        // Subclasses, focus and message dispatch must share the window thread.
        if (inputOwner) throw std::runtime_error("Text editor: another editor owns this window thread");
        if (GetWindowThreadProcessId(request.owner, nullptr) != GetCurrentThreadId())
            throw std::runtime_error("Text editor: startup ran on the wrong window thread");
        if (!GetClientRect(request.owner, &client)) inputError("GetClientRect");
        int width = std::clamp(client.right - client.left - 40, 220L, 420L);
        POINT position{(client.right - client.left - width) / 2, (client.bottom - client.top - 24) / 2};
        if (!ClientToScreen(request.owner, &position)) inputError("ClientToScreen");
        // Layered CHILD windows require Windows 8+ compatibility in the host
        // EXE's manifest. An owned POPUP works without changing the game and
        // is composed above DirectX. Keep it hidden until focus guards exist.
        edit = CreateWindowExW(WS_EX_CLIENTEDGE | WS_EX_LAYERED | WS_EX_TOOLWINDOW, L"EDIT", value.c_str(),
            WS_POPUP | WS_TABSTOP | ES_AUTOHSCROLL, position.x, position.y, width, 24,
            request.owner, nullptr, GetModuleHandleW(nullptr), nullptr);
        if (!edit) inputError("CreateWindowExW(EDIT popup)");
        if (!SetLayeredWindowAttributes(edit, 0, 255, LWA_ALPHA))
            inputError("SetLayeredWindowAttributes");
        SendMessageW(edit, EM_SETLIMITTEXT, 4096, 0);
        if (!SetWindowSubclass(edit, editProc, 1, reinterpret_cast<DWORD_PTR>(this))) {
            inputError("SetWindowSubclass(editor)");
        }
        if (!SetWindowSubclass(request.owner, ownerProc, reinterpret_cast<UINT_PTR>(this),
                reinterpret_cast<DWORD_PTR>(edit))) {
            inputError("SetWindowSubclass(owner)");
        }
        inputOwner = this;
        {
            std::lock_guard lock(mutex);
            window = edit;
            abort = cancelled;
        }
        if (abort) {
            finishModeless(edit, false);
            return;
        }
        ImmAssociateContextEx(edit, nullptr, IACE_DEFAULT);
        // A field can be opened inside the game's mouse-down callback.
        // Release any outstanding native capture before the editor takes over.
        if (GetCapture() == request.owner) ReleaseCapture();
        if (!SetWindowPos(edit, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW))
            inputError("SetWindowPos(editor)");
        SetActiveWindow(edit);
        SetFocus(edit);
        if (!edit || GetFocus() != edit) inputError("SetFocus(editor)");
        ClipCursor(nullptr);
        SetCursor(LoadCursorW(nullptr, IDC_ARROW));
        SendMessageW(edit, EM_SETSEL, 0, -1);
    }
};
thread_local NativeTextInput::Impl* NativeTextInput::Impl::inputOwner{};
NativeTextInput::NativeTextInput() = default;
NativeTextInput::~NativeTextInput() = default;
void NativeTextInput::start(Request request) {
    if (mImpl) throw std::runtime_error("Text editor is already open");
    // Native focus/capture changes can re-enter the game input callbacks.
    // Publish ownership first so those callbacks already treat this as editing.
    mImpl = std::make_unique<Impl>(std::move(request));
    try {
        if (mImpl->request.owner) {
            if (!IsWindow(mImpl->request.owner)) throw std::runtime_error("Text editor: game window is no longer valid");
            mImpl->scheduleModeless();
        } else mImpl->worker = std::thread([self = mImpl.get()] { self->run(); });
    } catch (...) {
        mImpl.reset();
        throw;
    }
}
bool NativeTextInput::active() const { return bool(mImpl); }
void NativeTextInput::cancel() { if (mImpl) mImpl->cancel(); }
void NativeTextInput::shutdown() { mImpl.reset(); }
std::optional<NativeTextInput::Result> NativeTextInput::take() {
    if (!mImpl) return {};
    std::optional<Result> result;
    {
        std::lock_guard lock(mImpl->mutex);
        if (!mImpl->result) return {};
        result = std::move(mImpl->result);
    }
    mImpl.reset();
    return result;
}
} // namespace wayfinder
