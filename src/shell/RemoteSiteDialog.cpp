#include "shell/RemoteDialog.h"

#include <commctrl.h>

#include <cstring>
#include <string>
#include <utility>

#include "infra/Log.h"
#include "shell/ChromeStyle.h"

namespace npcc::shell {
namespace {

constexpr const wchar_t* kSitesClass = L"notepadccRemoteSites";

// 站点编辑器的控件号（浏览对话框用 1801~1818，这里另起一段不撞车）
enum : int {
    kIdList = 1830,
    kIdNew = 1831,
    kIdSave = 1832,
    kIdDelete = 1833,
    kIdClose = 1834,
    kIdName = 1840,
    kIdProtocol = 1841,
    kIdHost = 1842,
    kIdPort = 1843,
    kIdUser = 1844,
    kIdPassword = 1845,
    kIdKeyFile = 1846,
    kIdInitialDir = 1847,
    kIdAcceptNew = 1848,
    kIdStatus = 1849,
};

int DpiOf(HWND reference) {
    UINT dpi = 96;
    const HMODULE user32 = ::GetModuleHandleW(L"user32.dll");
    if (user32 != nullptr && reference != nullptr) {
        using GetDpiForWindowFn = UINT(WINAPI*)(HWND);
        const auto getDpi = reinterpret_cast<GetDpiForWindowFn>(
            reinterpret_cast<void*>(::GetProcAddress(user32, "GetDpiForWindow")));
        if (getDpi != nullptr) {
            const UINT actual = getDpi(reference);
            if (actual != 0) {
                dpi = actual;
            }
        }
    }
    return static_cast<int>(dpi);
}

std::wstring TextOf(HWND control) {
    const int length = ::GetWindowTextLengthW(control);
    if (length <= 0) {
        return {};
    }
    std::wstring text(static_cast<std::size_t>(length) + 1, L'\0');
    const int copied = ::GetWindowTextW(control, text.data(), length + 1);
    text.resize(copied > 0 ? static_cast<std::size_t>(copied) : 0);
    return text;
}

std::wstring Trimmed(std::wstring text) {
    while (!text.empty() && (text.front() == L' ' || text.front() == L'\t')) {
        text.erase(0, 1);
    }
    while (!text.empty() && (text.back() == L' ' || text.back() == L'\t')) {
        text.pop_back();
    }
    return text;
}

class SiteEditorImpl {
public:
    bool Show(HWND owner, std::vector<core::RemoteSite> sites);
    const std::vector<core::RemoteSite>& Sites() const noexcept { return sites_; }

private:
    static LRESULT CALLBACK WndProcThunk(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    static void EnsureWindowClass();
    LRESULT WndProc(UINT message, WPARAM wParam, LPARAM lParam);

    int  Scale(int value) const { return ::MulDiv(value, DpiOf(owner_), 96); }
    HWND Make(const wchar_t* cls, const wchar_t* text, DWORD style, int id, int x, int y, int w,
              int h);
    HWND MakeLabel(const wchar_t* text, int x, int y, int w, int h) {
        return Make(L"STATIC", text, WS_VISIBLE | SS_LEFT, 0, x, y, w, h);
    }

    int  SelectedIndex() const;
    void RebuildList(int select);
    void LoadSelection();
    void ClearForm();
    void SaveForm();
    void DeleteSelection();
    void SetStatus(const std::wstring& text) const;
    core::RemoteSite FormSite() const;
    void FillForm(const core::RemoteSite& site);
    std::wstring ListText() const;

    HWND  owner_ = nullptr;
    HWND  hwnd_ = nullptr;
    HWND  list_ = nullptr;
    HWND  nameEdit_ = nullptr;
    HWND  protocolCombo_ = nullptr;
    HWND  hostEdit_ = nullptr;
    HWND  portEdit_ = nullptr;
    HWND  userEdit_ = nullptr;
    HWND  passwordEdit_ = nullptr;
    HWND  keyEdit_ = nullptr;
    HWND  initialEdit_ = nullptr;
    HWND  acceptCheck_ = nullptr;
    HWND  status_ = nullptr;
    HFONT font_ = nullptr;
    bool  done_ = false;
    bool  changed_ = false;
    std::vector<core::RemoteSite> sites_;
};

void SiteEditorImpl::EnsureWindowClass() {
    static bool registered = false;
    if (registered) {
        return;
    }
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = &SiteEditorImpl::WndProcThunk;
    wc.hInstance = ::GetModuleHandleW(nullptr);
    wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(static_cast<INT_PTR>(COLOR_BTNFACE + 1));
    wc.lpszClassName = kSitesClass;
    ::RegisterClassExW(&wc);
    registered = true;   // 重复注册同名类会失败，这里只认第一次（与 ToolbarDialog 同）
}

HWND SiteEditorImpl::Make(const wchar_t* cls, const wchar_t* text, const DWORD style, const int id,
                          const int x, const int y, const int w, const int h) {
    return ::CreateWindowExW(0, cls, text, WS_CHILD | style, x, y, w, h, hwnd_,
                             reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                             ::GetModuleHandleW(nullptr), nullptr);
}

int SiteEditorImpl::SelectedIndex() const {
    return static_cast<int>(::SendMessageW(list_, LB_GETCURSEL, 0, 0));
}

void SiteEditorImpl::SetStatus(const std::wstring& text) const {
    ::SetWindowTextW(status_, text.c_str());
}

std::wstring SiteEditorImpl::ListText() const {
    std::wstring text;
    for (const core::RemoteSite& site : sites_) {
        if (!text.empty()) {
            text += L"\r\n";
        }
        text += site.name;
        text += site.protocol == core::RemoteProtocol::Sftp ? L" (sftp)" : L" (ftp)";
    }
    return text;
}

void SiteEditorImpl::RebuildList(const int select) {
    ::SendMessageW(list_, LB_RESETCONTENT, 0, 0);
    for (const core::RemoteSite& site : sites_) {
        ::SendMessageW(list_, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(site.name.c_str()));
    }
    if (select >= 0 && select < static_cast<int>(sites_.size())) {
        ::SendMessageW(list_, LB_SETCURSEL, static_cast<WPARAM>(select), 0);
    } else {
        ::SendMessageW(list_, LB_SETCURSEL, static_cast<WPARAM>(-1), 0);
    }
}

void SiteEditorImpl::FillForm(const core::RemoteSite& site) {
    ::SetWindowTextW(nameEdit_, site.name.c_str());
    ::SendMessageW(protocolCombo_, CB_SETCURSEL,
                   site.protocol == core::RemoteProtocol::Sftp ? 1 : 0, 0);
    ::SetWindowTextW(hostEdit_, site.host.c_str());
    const std::wstring port = site.port != 0 ? std::to_wstring(site.port) : std::wstring();
    ::SetWindowTextW(portEdit_, port.c_str());
    ::SetWindowTextW(userEdit_, site.user.c_str());
    ::SetWindowTextW(passwordEdit_, site.password.c_str());
    ::SetWindowTextW(keyEdit_, site.keyFile.c_str());
    ::SetWindowTextW(initialEdit_, site.initialDir.c_str());
    ::SendMessageW(acceptCheck_, BM_SETCHECK,
                   site.acceptNewHostKey ? BST_CHECKED : BST_UNCHECKED, 0);
}

void SiteEditorImpl::ClearForm() {
    ::SendMessageW(list_, LB_SETCURSEL, static_cast<WPARAM>(-1), 0);
    FillForm(core::RemoteSite{});
}

void SiteEditorImpl::LoadSelection() {
    const int index = SelectedIndex();
    if (index < 0 || index >= static_cast<int>(sites_.size())) {
        return;
    }
    FillForm(sites_[static_cast<std::size_t>(index)]);
    SetStatus(L"编辑中：" + sites_[static_cast<std::size_t>(index)].name);
}

core::RemoteSite SiteEditorImpl::FormSite() const {
    core::RemoteSite site;
    site.name = Trimmed(TextOf(nameEdit_));
    const int protocol = static_cast<int>(::SendMessageW(protocolCombo_, CB_GETCURSEL, 0, 0));
    site.protocol = protocol == 1 ? core::RemoteProtocol::Sftp : core::RemoteProtocol::Ftp;
    site.host = Trimmed(TextOf(hostEdit_));
    site.user = Trimmed(TextOf(userEdit_));
    site.password = TextOf(passwordEdit_);
    site.keyFile = Trimmed(TextOf(keyEdit_));
    site.initialDir = Trimmed(TextOf(initialEdit_));
    site.acceptNewHostKey = ::SendMessageW(acceptCheck_, BM_GETCHECK, 0, 0) == BST_CHECKED;
    return site;
}

void SiteEditorImpl::SaveForm() {
    core::RemoteSite site = FormSite();
    const std::wstring portText = Trimmed(TextOf(portEdit_));
    if (portText.empty()) {
        site.port = 0;
    } else {
        wchar_t* end = nullptr;
        const long value = std::wcstol(portText.c_str(), &end, 10);
        if (end == nullptr || *end != L'\0' || value < 0 || value > 65535) {
            SetStatus(L"失败：端口要是 0~65535 之间的整数（留空 = 用默认端口）");
            return;
        }
        site.port = static_cast<std::uint16_t>(value);
    }
    std::wstring error;
    if (!core::ValidateRemoteSite(site, &error)) {
        SetStatus(L"失败：" + error);
        return;
    }
    int index = SelectedIndex();
    for (std::size_t i = 0; i < sites_.size(); ++i) {
        if (i != static_cast<std::size_t>(index) &&
            _wcsicmp(sites_[i].name.c_str(), site.name.c_str()) == 0) {
            SetStatus(L"失败：已经有叫 \"" + site.name + L"\" 的站点");
            return;
        }
    }
    if (index >= 0 && index < static_cast<int>(sites_.size())) {
        sites_[static_cast<std::size_t>(index)] = site;
    } else {
        sites_.push_back(site);
        index = static_cast<int>(sites_.size()) - 1;
    }
    RebuildList(index);
    changed_ = true;
    SetStatus(L"已保存：" + site.name);
    infra::LogInfo(L"remote", L"站点已保存：" + site.name + L"（" +
                                  core::RemoteProtocolToken(site.protocol) + L"）");
}

void SiteEditorImpl::DeleteSelection() {
    const int index = SelectedIndex();
    if (index < 0 || index >= static_cast<int>(sites_.size())) {
        SetStatus(L"先在上面选一个站点");
        return;
    }
    const std::wstring name = sites_[static_cast<std::size_t>(index)].name;
    sites_.erase(sites_.begin() + index);
    changed_ = true;
    if (sites_.empty()) {
        RebuildList(-1);
        ClearForm();
    } else {
        RebuildList(index);
        LoadSelection();
    }
    SetStatus(L"已删除：" + name);
    infra::LogInfo(L"remote", L"站点已删除：" + name);
}

LRESULT CALLBACK SiteEditorImpl::WndProcThunk(const HWND hwnd, const UINT message,
                                              const WPARAM wParam, const LPARAM lParam) {
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        auto* self = static_cast<SiteEditorImpl*>(create->lpCreateParams);
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->hwnd_ = hwnd;
        return ::DefWindowProcW(hwnd, message, wParam, lParam);
    }
    auto* self = reinterpret_cast<SiteEditorImpl*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (self == nullptr) {
        return ::DefWindowProcW(hwnd, message, wParam, lParam);
    }
    return self->WndProc(message, wParam, lParam);
}

LRESULT SiteEditorImpl::WndProc(const UINT message, const WPARAM wParam, const LPARAM lParam) {
    switch (message) {
        case WM_COMMAND: {
            const int id = LOWORD(wParam);
            const int notification = HIWORD(wParam);
            if (id == kIdClose || id == IDCANCEL) {
                done_ = true;
                return 0;
            }
            if (id == kIdList && notification == LBN_SELCHANGE) {
                LoadSelection();
                return 0;
            }
            switch (id) {
                case kIdNew:
                    ClearForm();
                    SetStatus(L"填好参数后点「保存」");
                    return 0;
                case kIdSave:
                    SaveForm();
                    return 0;
                case kIdDelete:
                    DeleteSelection();
                    return 0;
                default:
                    break;
            }
            break;
        }
        case WM_GETTEXT: {
            // 站点表的读回（跨进程读标准列表框不可靠，与 XPath 对话框同款）
            const std::wstring text = ListText();
            auto* buffer = reinterpret_cast<wchar_t*>(lParam);
            const UINT capacity = static_cast<UINT>(wParam);
            if (buffer == nullptr || capacity == 0) {
                return 0;
            }
            const std::size_t count =
                text.size() < capacity - 1 ? text.size() : static_cast<std::size_t>(capacity) - 1;
            std::memcpy(buffer, text.data(), count * sizeof(wchar_t));
            buffer[count] = L'\0';
            return static_cast<LRESULT>(count);
        }
        case WM_GETTEXTLENGTH:
            return static_cast<LRESULT>(ListText().size());
        case WM_KEYDOWN:
            if (wParam == VK_ESCAPE) {
                done_ = true;
                return 0;
            }
            break;
        case WM_CLOSE:
            done_ = true;
            return 0;
        default:
            break;
    }
    return ::DefWindowProcW(hwnd_, message, wParam, lParam);
}

bool SiteEditorImpl::Show(HWND owner, std::vector<core::RemoteSite> sites) {
    EnsureWindowClass();
    owner_ = owner;
    sites_ = std::move(sites);

    RECT ownerRect{};
    ::GetWindowRect(owner, &ownerRect);
    const int width = Scale(760);
    const int height = Scale(470);
    const int x = ownerRect.left + ((ownerRect.right - ownerRect.left) - width) / 2;
    const int y = ownerRect.top + ((ownerRect.bottom - ownerRect.top) - height) / 2;
    hwnd_ = ::CreateWindowExW(WS_EX_DLGMODALFRAME, kSitesClass, L"远程站点设置",
                              WS_POPUP | WS_CAPTION | WS_SYSMENU, x, y, width, height, owner,
                              nullptr, ::GetModuleHandleW(nullptr), this);
    if (hwnd_ == nullptr) {
        return false;
    }

    NONCLIENTMETRICSW metrics{};
    metrics.cbSize = sizeof(metrics);
    LOGFONTW logFont{};
    if (::SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0) != 0) {
        logFont = metrics.lfMessageFont;
    } else {
        ::wcscpy_s(logFont.lfFaceName, L"Microsoft YaHei UI");
        logFont.lfHeight = -12;
    }
    font_ = ::CreateFontIndirectW(&logFont);

    RECT client{};
    ::GetClientRect(hwnd_, &client);
    const int clientWidth = client.right;
    const int clientHeight = client.bottom;
    const int margin = Scale(14);
    const int rowHeight = Scale(24);
    const int gap = Scale(6);
    const int buttonHeight = Scale(26);
    const int listWidth = Scale(190);

    const int buttonY = clientHeight - margin - buttonHeight;
    const int statusY = buttonY - gap - Scale(20);
    const int hintY = statusY - gap - Scale(40);

    MakeLabel(L"站点：", margin, margin + Scale(4), listWidth, rowHeight);
    const int listTop = margin + rowHeight;
    list_ = Make(L"LISTBOX", L"",
                 WS_VISIBLE | WS_TABSTOP | WS_BORDER | WS_VSCROLL | LBS_NOTIFY, kIdList, margin,
                 listTop, listWidth, hintY - gap - listTop);

    const int rightX = margin + listWidth + Scale(14);
    const int fieldLabelWidth = Scale(84);
    const int fieldX = rightX + fieldLabelWidth;
    const int fieldWidth = clientWidth - margin - fieldX;
    int row = margin;
    auto rowY = [&]() {
        const int current = row;
        row += rowHeight + gap;
        return current;
    };
    auto makeField = [&](const wchar_t* label, const DWORD style, const int id) {
        const int current = rowY();
        MakeLabel(label, rightX, current + Scale(4), fieldLabelWidth, rowHeight);
        return Make(L"EDIT", L"", WS_VISIBLE | WS_TABSTOP | WS_BORDER | ES_AUTOHSCROLL | style,
                    id, fieldX, current, fieldWidth, buttonHeight);
    };

    nameEdit_ = makeField(L"名称：", 0, kIdName);
    {
        const int current = rowY();
        MakeLabel(L"协议：", rightX, current + Scale(4), fieldLabelWidth, rowHeight);
        protocolCombo_ = Make(L"COMBOBOX", L"",
                              WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
                              kIdProtocol, fieldX, current, fieldWidth, Scale(140));
        ::SendMessageW(protocolCombo_, CB_ADDSTRING, 0,
                       reinterpret_cast<LPARAM>(L"FTP（明文，自研客户端）"));
        ::SendMessageW(protocolCombo_, CB_ADDSTRING, 0,
                       reinterpret_cast<LPARAM>(L"SFTP（系统 OpenSSH，密钥认证）"));
    }
    hostEdit_ = makeField(L"主机：", 0, kIdHost);
    portEdit_ = makeField(L"端口：", ES_NUMBER, kIdPort);
    userEdit_ = makeField(L"用户名：", 0, kIdUser);
    passwordEdit_ = makeField(L"密码：", ES_PASSWORD, kIdPassword);
    keyEdit_ = makeField(L"私钥文件：", 0, kIdKeyFile);
    initialEdit_ = makeField(L"起始目录：", 0, kIdInitialDir);
    {
        const int current = rowY();
        acceptCheck_ = Make(L"BUTTON", L"首次连接自动接受主机密钥（仅 SFTP）",
                            WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX, kIdAcceptNew, rightX,
                            current, clientWidth - margin - rightX, rowHeight);
    }
    Make(L"STATIC",
         L"密码明文存在 config.xml 里（多数编辑器都这样，这里如实说明）。"
         L"SFTP 只支持密钥认证——批处理模式下没法输密码。",
         WS_VISIBLE | SS_LEFT, 0, rightX, hintY, clientWidth - margin - rightX, Scale(40));
    status_ = Make(L"STATIC", L"", WS_VISIBLE | SS_LEFT, kIdStatus, margin, statusY,
                   clientWidth - margin * 2, Scale(20));

    Make(L"BUTTON", L"新建", WS_VISIBLE | WS_TABSTOP, kIdNew, margin, buttonY, Scale(90),
         buttonHeight);
    Make(L"BUTTON", L"保存", WS_VISIBLE | WS_TABSTOP, kIdSave, margin + Scale(90) + gap, buttonY,
         Scale(90), buttonHeight);
    Make(L"BUTTON", L"删除", WS_VISIBLE | WS_TABSTOP, kIdDelete,
         margin + (Scale(90) + gap) * 2, buttonY, Scale(90), buttonHeight);
    Make(L"BUTTON", L"关闭", WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON, kIdClose,
         clientWidth - margin - Scale(90), buttonY, Scale(90), buttonHeight);

    if (font_ != nullptr) {
        ::EnumChildWindows(
            hwnd_,
            [](const HWND child, const LPARAM param) -> BOOL {
                ::SendMessageW(child, WM_SETFONT, static_cast<WPARAM>(param), TRUE);
                return TRUE;
            },
            reinterpret_cast<LPARAM>(font_));
    }

    ApplyDialogChrome(hwnd_);
    RebuildList(0);
    if (sites_.empty()) {
        ClearForm();
        SetStatus(L"还没有站点：填好参数后点「保存」");
    } else {
        LoadSelection();
    }

    ::EnableWindow(owner_, FALSE);
    ::ShowWindow(hwnd_, SW_SHOW);
    if (nameEdit_ != nullptr) {
        ::SetFocus(nameEdit_);
    }

    MSG message{};
    while (!done_ && ::IsWindow(hwnd_) != 0) {
        const BOOL fetched = ::GetMessageW(&message, nullptr, 0, 0);
        if (fetched <= 0) {
            break;
        }
        if (::IsDialogMessageW(hwnd_, &message) == 0) {
            ::TranslateMessage(&message);
            ::DispatchMessageW(&message);
        }
    }
    ::EnableWindow(owner_, TRUE);
    ::SetForegroundWindow(owner_);
    if (font_ != nullptr) {
        ::DeleteObject(font_);
        font_ = nullptr;
    }
    if (::IsWindow(hwnd_) != 0) {
        ::DestroyWindow(hwnd_);
    }
    hwnd_ = nullptr;
    return changed_;
}

}  // namespace

bool RemoteSiteDialog::Show(HWND owner, const std::vector<core::RemoteSite>& sites,
                            std::vector<core::RemoteSite>* out) {
    SiteEditorImpl editor;
    if (!editor.Show(owner, sites)) {
        return false;
    }
    if (out != nullptr) {
        *out = editor.Sites();
    }
    return true;
}

}  // namespace npcc::shell
