#include "wayfinder/NativeTextInput.h"
#include <CommCtrl.h>
#include <imm.h>
#include <algorithm>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

namespace wayfinder {
namespace {
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
    Request request;
    std::mutex mutex;
    HWND window{};
    bool cancelled{};
    std::optional<Result> result;
    std::thread worker;
    // Accessed only by the UI thread.
    bool composing{}, activated{};
    std::wstring value;
    Result outcome;

    explicit Impl(Request input) : request(std::move(input)), value(wide(request.text)) {}
    ~Impl() { cancel(); if (worker.joinable()) worker.join(); }
    void cancel() {
        std::lock_guard lock(mutex);
        cancelled = true;
        if (window) PostMessageW(window, WM_CLOSE, 0, 0);
    }
    static LRESULT CALLBACK editProc(HWND edit, UINT msg, WPARAM w, LPARAM l, UINT_PTR id, DWORD_PTR data) {
        auto& self = *reinterpret_cast<Impl*>(data);
        if (msg == WM_IME_STARTCOMPOSITION) self.composing = true;
        if (msg == WM_IME_ENDCOMPOSITION) self.composing = false;
        if (msg == WM_GETDLGCODE && self.composing)
            return DefSubclassProc(edit, msg, w, l) | DLGC_WANTALLKEYS;
        if (msg == WM_KEYDOWN && w == 'A' && (GetKeyState(VK_CONTROL) & 0x8000)) {
            SendMessageW(edit, EM_SETSEL, 0, -1);
            return 0;
        }
        if (msg == WM_NCDESTROY) RemoveWindowSubclass(edit, editProc, id);
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
};
NativeTextInput::NativeTextInput() = default;
NativeTextInput::~NativeTextInput() = default;
void NativeTextInput::start(Request request) {
    if (mImpl) throw std::runtime_error("Text editor is already open");
    auto next = std::make_unique<Impl>(std::move(request));
    next->worker = std::thread([self = next.get()] { self->run(); });
    mImpl = std::move(next);
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
