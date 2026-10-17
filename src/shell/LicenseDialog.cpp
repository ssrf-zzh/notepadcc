#include "shell/LicenseDialog.h"

#include <commdlg.h>

#include <cstring>
#include <cwchar>
#include <iterator>
#include <memory>
#include <string>
#include <utility>

#include "core/Localization.h"
#include "core/Messages.h"
#include "core/Version.h"
#include "infra/TextCodec.h"
#include "io/AtomicFile.h"
#include "license/ActivationClient.h"
#include "license/Fingerprint.h"
#include "license/LicenseService.h"

namespace npcc::shell {
namespace {

constexpr const wchar_t* kWindowClass = L"notepadccLicenseDialog";

// 控件 ID 是对界面回归脚本的稳定接口（tools/ui_smoke_test.py 第 10 段）：
// 数值一经发布不再改。1501 的状态文本是脚本"读回状态"的目标。
enum : int {
    kIdStatusGroup   = 1500,
    kIdStatusText    = 1501,
    kIdOnlineGroup   = 1510,
    kIdServiceLabel  = 1511,
    kIdKeyLabel      = 1512,
    kIdKeyEdit       = 1513,
    kIdActivate      = 1514,
    kIdOnlineHint    = 1515,
    kIdOfflineGroup  = 1520,
    kIdSeatsLabel    = 1521,
    kIdSeatsEdit     = 1522,
    kIdHolderLabel   = 1523,
    kIdHolderEdit    = 1524,
    kIdChallengeText = 1525,
    kIdCopyChallenge = 1526,
    kIdSaveChallenge = 1527,
    kIdChallengeLabel = 1528,
    kIdImportGroup   = 1530,
    kIdImportPath    = 1531,
    kIdBrowse        = 1532,
    kIdImport        = 1533,
    kIdImportHint    = 1534,
    kIdClose         = 1540,
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

std::wstring Trim(std::wstring text) {
    while (!text.empty() && (text.front() == L' ' || text.front() == L'\t' ||
                             text.front() == L'\r' || text.front() == L'\n')) {
        text.erase(text.begin());
    }
    while (!text.empty() && (text.back() == L' ' || text.back() == L'\t' ||
                             text.back() == L'\r' || text.back() == L'\n')) {
        text.pop_back();
    }
    return text;
}

std::wstring TextOf(HWND control) {
    if (control == nullptr) {
        return {};
    }
    const int length = ::GetWindowTextLengthW(control);
    if (length <= 0) {
        return {};
    }
    std::wstring text(static_cast<std::size_t>(length) + 1, L'\0');
    const int written = ::GetWindowTextW(control, text.data(), length + 1);
    text.resize(static_cast<std::size_t>(written < 0 ? 0 : written));
    return text;
}

// 纯文本进剪贴板（申请串要能贴进邮件 / 微信 / 工单）。别的程序占着剪贴板时
// OpenClipboard 会失败：重试几次，仍失败就如实告诉调用方。
bool CopyPlainTextToClipboard(HWND owner, const std::wstring& text) {
    if (text.empty()) {
        return false;
    }
    bool opened = false;
    for (int attempt = 0; attempt < 5 && !opened; ++attempt) {
        opened = ::OpenClipboard(owner) != 0;
        if (!opened) {
            ::Sleep(10);
        }
    }
    if (!opened) {
        return false;
    }
    ::EmptyClipboard();
    const std::size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL block = ::GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (block == nullptr) {
        ::CloseClipboard();
        return false;
    }
    void* target = ::GlobalLock(block);
    if (target == nullptr) {
        ::GlobalFree(block);
        ::CloseClipboard();
        return false;
    }
    std::memcpy(target, text.c_str(), bytes);
    ::GlobalUnlock(block);
    if (::SetClipboardData(CF_UNICODETEXT, block) == nullptr) {
        ::GlobalFree(block);   // 交出去才算数：失败时这块内存还归我们
        ::CloseClipboard();
        return false;
    }
    ::CloseClipboard();
    return true;
}

std::string ComputerNameUtf8() {
    wchar_t buffer[MAX_COMPUTERNAME_LENGTH + 1] = {};
    DWORD size = MAX_COMPUTERNAME_LENGTH + 1;
    if (::GetComputerNameW(buffer, &size) == 0) {
        return {};
    }
    return infra::Utf8FromWide(buffer);
}

std::wstring EditionLabel(license::Edition edition) {
    switch (edition) {
        case license::Edition::Pro:
            return L"Pro";
        case license::Edition::Enterprise:
            return L"Enterprise";
        case license::Edition::Free:
            break;
    }
    return L"Free";
}

// 界面里守住 1~999：申请串是合同的引子，数字不能荒唐（服务端还会再校验一遍）
int SeatsFromText(const std::wstring& text) {
    if (text.empty()) {
        return 1;
    }
    long value = ::wcstol(text.c_str(), nullptr, 10);
    if (value < 1) {
        return 1;
    }
    if (value > 999) {
        return 999;
    }
    return static_cast<int>(value);
}

// 在线激活的后台结果：从工作线程把所有权递回界面线程（约定同更新 / 远程对话框）
struct ActivatePayload {
    license::ActivationResult result;
};

class LicenseDialogImpl {
public:
    LicenseDialogOutcome Show(HWND owner, infra::ThreadPool& pool, std::string serviceUrl);

private:
    static LRESULT CALLBACK WndProcThunk(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT WndProc(UINT message, WPARAM wParam, LPARAM lParam);

    int  Scale(int value) const { return ::MulDiv(value, DpiOf(owner_), 96); }
    HWND Make(const wchar_t* cls, const wchar_t* text, DWORD style, int id, DWORD exStyle = 0);
    void Layout();

    void RefreshStatus();
    void RefreshChallenge();
    void StartActivate();
    void HandleActivate(std::unique_ptr<ActivatePayload> payload);
    void DoImport();
    void BrowseImport();
    void CopyChallenge();
    void SaveChallenge();
    void UpdateButtons();

    HWND owner_ = nullptr;
    HWND hwnd_ = nullptr;
    HFONT font_ = nullptr;
    HWND statusText_ = nullptr;
    HWND serviceLabel_ = nullptr;
    HWND keyEdit_ = nullptr;
    HWND onlineHint_ = nullptr;
    HWND seatsEdit_ = nullptr;
    HWND holderEdit_ = nullptr;
    HWND challengeText_ = nullptr;
    HWND challengeLabel_ = nullptr;
    HWND importPathEdit_ = nullptr;
    HWND importHint_ = nullptr;
    infra::ThreadPool* pool_ = nullptr;
    std::string serviceUrl_;
    std::string lastChallenge_;   // 与屏幕上的申请串逐字节一致（复制 / 保存用）
    bool done_ = false;
    bool changed_ = false;
    bool activating_ = false;
    bool ready_ = false;          // 控件齐了才允许 EN_CHANGE 触发刷新
};
HWND LicenseDialogImpl::Make(const wchar_t* cls, const wchar_t* text, DWORD style, int id,
                             DWORD exStyle) {
    return ::CreateWindowExW(exStyle, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10,
                             hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                             ::GetModuleHandleW(nullptr), nullptr);
}

void LicenseDialogImpl::Layout() {
    RECT client{};
    ::GetClientRect(hwnd_, &client);
    const int gap = Scale(10);
    const int width = client.right - client.left;
    const int inner = width - gap * 2;
    const int pad = Scale(10);   // 组内留白

    const auto moveItem = [this](int id, int x, int y, int w, int h) {
        if (HWND item = ::GetDlgItem(hwnd_, id); item != nullptr) {
            ::MoveWindow(item, x, y, w, h, TRUE);
        }
    };

    // 分区 1：许可状态（脚本读 1501 的文本核对状态）
    int y = gap;
    moveItem(kIdStatusGroup, gap, y, inner, Scale(96));
    moveItem(kIdStatusText, gap + pad, y + Scale(22), inner - pad * 2, Scale(68));

    // 分区 2：在线激活
    y += Scale(96) + gap;
    moveItem(kIdOnlineGroup, gap, y, inner, Scale(116));
    moveItem(kIdServiceLabel, gap + pad, y + Scale(20), inner - pad * 2, Scale(18));
    moveItem(kIdKeyLabel, gap + pad, y + Scale(46), Scale(56), Scale(20));
    moveItem(kIdKeyEdit, gap + pad + Scale(60), y + Scale(44), inner - pad * 2 - Scale(160),
             Scale(24));
    moveItem(kIdActivate, width - gap - pad - Scale(90), y + Scale(43), Scale(90), Scale(26));
    moveItem(kIdOnlineHint, gap + pad, y + Scale(76), inner - pad * 2, Scale(34));

    // 分区 3：离线激活
    y += Scale(116) + gap;
    moveItem(kIdOfflineGroup, gap, y, inner, Scale(208));
    moveItem(kIdSeatsLabel, gap + pad, y + Scale(24), Scale(46), Scale(20));
    moveItem(kIdSeatsEdit, gap + pad + Scale(48), y + Scale(22), Scale(52), Scale(24));
    moveItem(kIdHolderLabel, gap + pad + Scale(112), y + Scale(24), Scale(60), Scale(20));
    moveItem(kIdHolderEdit, gap + pad + Scale(176), y + Scale(22), inner - pad * 2 - Scale(186),
             Scale(24));
    moveItem(kIdChallengeLabel, gap + pad, y + Scale(50), inner - pad * 2, Scale(18));
    moveItem(kIdChallengeText, gap + pad, y + Scale(70), inner - pad * 2, Scale(92));
    moveItem(kIdCopyChallenge, gap + pad, y + Scale(170), Scale(120), Scale(28));
    moveItem(kIdSaveChallenge, gap + pad + Scale(128), y + Scale(170), Scale(130), Scale(28));

    // 分区 4：导入
    y += Scale(208) + gap;
    moveItem(kIdImportGroup, gap, y, inner, Scale(106));
    moveItem(kIdImportPath, gap + pad, y + Scale(24), inner - pad * 2 - Scale(200), Scale(24));
    moveItem(kIdBrowse, width - gap - pad - Scale(190), y + Scale(23), Scale(90), Scale(26));
    moveItem(kIdImport, width - gap - pad - Scale(90), y + Scale(23), Scale(90), Scale(26));
    moveItem(kIdImportHint, gap + pad, y + Scale(58), inner - pad * 2, Scale(42));

    // 底部：关闭按钮贴右下
    moveItem(kIdClose, width - gap - Scale(90), client.bottom - gap - Scale(28), Scale(90),
             Scale(28));
}

void LicenseDialogImpl::RefreshStatus() {
    const license::LicenseService& service = license::LicenseService::Instance();
    std::wstring text;
    if (service.State() != license::LicenseState::Free) {
        text += core::Tr(L"license.dialog.state.active", L"当前版本：");
        text += EditionLabel(service.EditionValue());
        text += core::Tr(L"license.dialog.state.activated", L"（已激活）");
        text += L"\r\n";
        const license::LicenseInfo& info = service.Info();
        std::wstring holder = infra::WideFromUtf8(info.holder);
        if (holder.empty()) {
            holder = core::Tr(L"license.dialog.state.anonymousHolder", L"（未署名）");
        }
        text += core::Tr(L"license.dialog.state.holder", L"持有人：") + holder;
        if (info.seats == 0) {
            text += core::Tr(L"license.dialog.state.seatsSite", L"　　席位：站点 / 不限量");
        } else {
            text += core::Tr(L"license.dialog.state.seats", L"　　席位：") +
                    std::to_wstring(info.seats);
        }
        text += L"\r\n";
        text += core::Tr(L"license.dialog.state.validity", L"有效期：");
        if (info.expiresAt.has_value()) {
            const std::string iso = license::FormatIso8601Utc(*info.expiresAt);
            text += core::Tr(L"license.dialog.state.until", L"至 ") +
                    infra::WideFromUtf8(iso.substr(0, 10)) +
                    core::Tr(L"license.dialog.state.utc", L"（UTC）");
        } else {
            text += core::Tr(L"license.dialog.state.perpetual", L"永久");
        }
    } else {
        text += core::Tr(L"license.dialog.state.free", L"当前版本：Free（未激活）") + L"\r\n";
        const std::wstring reason =
            service.LastError() == license::LicenseError::None
                ? core::Tr(L"license.dialog.state.noLicense", L"没有有效许可。")
                : license::DescribeLicenseError(service.LastError());
        text += core::Tr(L"license.dialog.state.reason", L"说明：") + reason;
    }
    text += L"\r\n";
    text += core::Tr(L"license.dialog.state.fingerprint", L"机器短码：") +
            infra::WideFromUtf8(license::Format(license::Current()));
    ::SetWindowTextW(statusText_, text.c_str());
}

void LicenseDialogImpl::RefreshChallenge() {
    const int seats = SeatsFromText(Trim(TextOf(seatsEdit_)));
    const std::wstring holder = Trim(TextOf(holderEdit_));
    lastChallenge_ = license::LicenseService::Instance().ExportChallenge(
        core::kAppVersionAscii, seats, infra::Utf8FromWide(holder));
    if (challengeText_ != nullptr) {
        ::SetWindowTextW(challengeText_, infra::WideFromUtf8(lastChallenge_).c_str());
    }
}
void LicenseDialogImpl::StartActivate() {
    if (activating_) {
        return;
    }
    const std::wstring key = Trim(TextOf(keyEdit_));
    if (key.empty()) {
        ::SetWindowTextW(onlineHint_,
                         core::Tr(L"license.dialog.online.needKey",
                                  L"先填写授权码（购买后由客服提供，形如 XXXXX-XXXXX-…）。")
                             .c_str());
        ::SetFocus(keyEdit_);
        return;
    }

    license::ActivateInput input;
    input.licenseKey = infra::Utf8FromWide(key);
    input.fingerprintText = license::Format(license::Current());
    input.appVersion = core::kAppVersionAscii;
    input.osVersion = license::CurrentOsVersion();
    input.computerName = ComputerNameUtf8();

    activating_ = true;
    UpdateButtons();
    ::SetWindowTextW(onlineHint_,
                     core::Tr(L"license.dialog.online.busy",
                              L"正在联系激活服务（超时 10 秒，网络失败会自动重试一次）…")
                         .c_str());

    // 请求在后台线程跑；结果作为指针递回本对话框（对话框提前关闭时 PostMessage
    // 失败，payload 随 unique_ptr 释放——与更新检查同一条写法）
    const HWND target = hwnd_;
    const std::string url = serviceUrl_;
    pool_->Submit([target, input, url]() {
        auto payload = std::make_unique<ActivatePayload>();
        payload->result = license::ActivationClient(url).Activate(input);
        if (::PostMessageW(target, core::kMsgLicenseActivateResult, 0,
                           reinterpret_cast<LPARAM>(payload.get())) == 0) {
            return;
        }
        payload.release();
    });
}

void LicenseDialogImpl::HandleActivate(std::unique_ptr<ActivatePayload> payload) {
    activating_ = false;
    UpdateButtons();
    const license::ActivationResult& result = payload->result;
    if (!result.ok()) {
        std::wstring hint = license::DescribeActivationError(result.error);
        if (!result.message.empty()) {
            hint += L"（服务端说明：" + result.message + L"）";
        } else if (!result.detail.empty() &&
                   result.error != license::ActivationError::NotConfigured) {
            hint += L"（" + result.detail + L"）";
        }
        ::SetWindowTextW(onlineHint_, hint.c_str());
        RefreshChallenge();   // 降级为离线指引：申请串当场摆出来，不让人扑空
        return;
    }

    // 服务端回的是许可容器原文：与导入同一条门槛——先验签，过了才落盘
    license::LicenseError importError = license::LicenseError::None;
    std::wstring importDetail;
    if (!license::LicenseService::Instance().ImportBytes(result.licenseBytes, &importError,
                                                         &importDetail)) {
        const std::wstring hint =
            L"服务端返回的许可没有通过本地验签（" + license::DescribeLicenseError(importError) +
            L"）——已拒绝写入；请联系客服，或改用离线激活走一遍签发。";
        ::SetWindowTextW(onlineHint_, hint.c_str());
        return;
    }
    changed_ = true;
    ::SetWindowTextW(onlineHint_,
                     result.message.empty()
                         ? core::Tr(L"license.dialog.online.ok",
                                    L"激活成功，许可已写入并立即生效。")
                               .c_str()
                         : (L"激活成功：" + result.message).c_str());
    RefreshStatus();
    RefreshChallenge();
}

void LicenseDialogImpl::DoImport() {
    const std::wstring path = Trim(TextOf(importPathEdit_));
    if (path.empty()) {
        ::SetWindowTextW(importHint_,
                         core::Tr(L"license.dialog.import.needPath",
                                  L"先选择一个许可文件（.lic / license.dat），再点“导入”。")
                             .c_str());
        return;
    }
    license::LicenseError error = license::LicenseError::None;
    std::wstring detail;
    if (!license::LicenseService::Instance().Import(path, &error, &detail)) {
        const std::wstring reason = detail.empty() ? license::DescribeLicenseError(error) : detail;
        ::SetWindowTextW(importHint_, (L"导入失败：" + reason).c_str());
        return;
    }
    changed_ = true;
    ::SetWindowTextW(importHint_, detail.empty() ? L"已导入并生效。" : detail.c_str());
    RefreshStatus();
    RefreshChallenge();
}

void LicenseDialogImpl::BrowseImport() {
    wchar_t buffer[1024] = {};
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = hwnd_;
    dialog.lpstrFilter = L"许可文件 (*.lic;*.dat)\0*.lic;*.dat\0所有文件 (*.*)\0*.*\0";
    dialog.nFilterIndex = 1;
    dialog.lpstrFile = buffer;
    dialog.nMaxFile = static_cast<DWORD>(std::size(buffer));
    dialog.lpstrTitle = L"选择许可文件";
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER;
    if (::GetOpenFileNameW(&dialog) == 0) {
        return;   // 用户取消：不打扰
    }
    ::SetWindowTextW(importPathEdit_, buffer);
    ::SetFocus(importPathEdit_);
}

void LicenseDialogImpl::CopyChallenge() {
    if (lastChallenge_.empty()) {
        RefreshChallenge();
    }
    if (!CopyPlainTextToClipboard(hwnd_, infra::WideFromUtf8(lastChallenge_))) {
        ::SetWindowTextW(challengeLabel_,
                         core::Tr(L"license.dialog.offline.copyFailed",
                                  L"复制失败：剪贴板被别的程序占着，稍后再试。")
                             .c_str());
        return;
    }
    ::SetWindowTextW(challengeLabel_,
                     core::Tr(L"license.dialog.offline.copied",
                              L"申请串已复制到剪贴板——把它发给客服即可换取许可文件。")
                         .c_str());
}

void LicenseDialogImpl::SaveChallenge() {
    if (lastChallenge_.empty()) {
        RefreshChallenge();
    }
    wchar_t buffer[1024] = {};
    ::wcscpy_s(buffer, L"notepadcc-license-request.txt");
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = hwnd_;
    dialog.lpstrFilter = L"文本文件 (*.txt)\0*.txt\0所有文件 (*.*)\0*.*\0";
    dialog.nFilterIndex = 1;
    dialog.lpstrFile = buffer;
    dialog.nMaxFile = static_cast<DWORD>(std::size(buffer));
    dialog.lpstrDefExt = L"txt";
    dialog.lpstrTitle = L"保存许可申请串";
    dialog.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_EXPLORER;
    if (::GetSaveFileNameW(&dialog) == 0) {
        return;   // 用户取消
    }
    const io::WriteResult result = io::WriteFileAtomic(buffer, lastChallenge_);
    if (!result.ok) {
        ::SetWindowTextW(challengeLabel_, (L"保存失败：" + result.message).c_str());
        return;
    }
    ::SetWindowTextW(challengeLabel_, (L"已保存到 " + std::wstring(buffer)).c_str());
}

void LicenseDialogImpl::UpdateButtons() {
    if (HWND button = ::GetDlgItem(hwnd_, kIdActivate); button != nullptr) {
        ::EnableWindow(button, activating_ ? FALSE : TRUE);
    }
}
LRESULT CALLBACK LicenseDialogImpl::WndProcThunk(HWND hwnd, UINT message, WPARAM wParam,
                                                 LPARAM lParam) {
    auto* self = reinterpret_cast<LicenseDialogImpl*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        self = static_cast<LicenseDialogImpl*>(create->lpCreateParams);
        self->hwnd_ = hwnd;
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    if (self == nullptr) {
        return ::DefWindowProcW(hwnd, message, wParam, lParam);
    }
    return self->WndProc(message, wParam, lParam);
}

LRESULT LicenseDialogImpl::WndProc(UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case core::kMsgLicenseActivateResult: {
            // 指针所有权在这里接过来（与更新 / 远程对话框同一条约定）
            std::unique_ptr<ActivatePayload> payload(reinterpret_cast<ActivatePayload*>(lParam));
            if (payload != nullptr) {
                HandleActivate(std::move(payload));
            }
            return 0;
        }
        case WM_COMMAND: {
            const int id = LOWORD(wParam);
            const int notification = HIWORD(wParam);
            if (ready_ && (id == kIdSeatsEdit || id == kIdHolderEdit) &&
                notification == EN_CHANGE) {
                // 席位 / 单位名改了：申请串跟着重生成（requestId 取新，属于新一次申请）
                RefreshChallenge();
                return 0;
            }
            switch (id) {
                case kIdActivate:
                    StartActivate();
                    return 0;
                case kIdCopyChallenge:
                    CopyChallenge();
                    return 0;
                case kIdSaveChallenge:
                    SaveChallenge();
                    return 0;
                case kIdBrowse:
                    BrowseImport();
                    return 0;
                case kIdImport:
                    DoImport();
                    return 0;
                case kIdClose:
                case IDCANCEL:
                    done_ = true;
                    return 0;
                default:
                    break;
            }
            break;
        }
        case WM_KEYDOWN:
            if (wParam == VK_ESCAPE) {
                done_ = true;
                return 0;
            }
            break;
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLORBTN: {
            const HDC dc = reinterpret_cast<HDC>(wParam);
            ::SetBkMode(dc, TRANSPARENT);
            return reinterpret_cast<LRESULT>(::GetSysColorBrush(COLOR_BTNFACE));
        }
        case WM_SIZE:
            Layout();
            return 0;
        case WM_CLOSE:
            done_ = true;
            return 0;
        case WM_DESTROY:
            ::SetWindowLongPtrW(hwnd_, GWLP_USERDATA, 0);
            return 0;
        default:
            break;
    }
    return ::DefWindowProcW(hwnd_, message, wParam, lParam);
}

LicenseDialogOutcome LicenseDialogImpl::Show(HWND owner, infra::ThreadPool& pool,
                                             std::string serviceUrl) {
    owner_ = owner;
    pool_ = &pool;
    serviceUrl_ = std::move(serviceUrl);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = &LicenseDialogImpl::WndProcThunk;
    wc.hInstance = ::GetModuleHandleW(nullptr);
    wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(static_cast<INT_PTR>(COLOR_BTNFACE + 1));
    wc.lpszClassName = kWindowClass;
    ::RegisterClassExW(&wc);

    RECT ownerRect{};
    ::GetWindowRect(owner, &ownerRect);
    const int width = Scale(660);
    const int height = Scale(660);
    const int x = ownerRect.left + ((ownerRect.right - ownerRect.left) - width) / 2;
    const int y = ownerRect.top + ((ownerRect.bottom - ownerRect.top) - height) / 2;
    hwnd_ = ::CreateWindowExW(
        WS_EX_DLGMODALFRAME, kWindowClass,
        core::Tr(L"license.dialog.title", L"许可证").c_str(),
        WS_POPUP | WS_CAPTION | WS_SYSMENU, x, y, width, height, owner, nullptr,
        ::GetModuleHandleW(nullptr), this);
    if (hwnd_ == nullptr) {
        return {};
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

    // 分区 1：许可状态
    Make(L"BUTTON", core::Tr(L"license.dialog.statusGroup", L"许可状态").c_str(),
         WS_VISIBLE | BS_GROUPBOX, kIdStatusGroup);
    statusText_ = Make(L"STATIC", L"", WS_VISIBLE | SS_LEFT, kIdStatusText);

    // 分区 2：在线激活
    Make(L"BUTTON", core::Tr(L"license.dialog.onlineGroup", L"在线激活").c_str(),
         WS_VISIBLE | BS_GROUPBOX, kIdOnlineGroup);
    serviceLabel_ = Make(L"STATIC", L"", WS_VISIBLE | SS_LEFT, kIdServiceLabel);
    Make(L"STATIC", core::Tr(L"license.dialog.keyLabel", L"授权码：").c_str(),
         WS_VISIBLE | SS_LEFT, kIdKeyLabel);
    keyEdit_ = Make(L"EDIT", L"", WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL, kIdKeyEdit,
                    WS_EX_CLIENTEDGE);
    Make(L"BUTTON", core::Tr(L"license.dialog.activate", L"激活(&A)").c_str(),
         WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON, kIdActivate);
    onlineHint_ = Make(L"STATIC", L"", WS_VISIBLE | SS_LEFT, kIdOnlineHint);

    // 分区 3：离线激活
    Make(L"BUTTON", core::Tr(L"license.dialog.offlineGroup", L"离线激活").c_str(),
         WS_VISIBLE | BS_GROUPBOX, kIdOfflineGroup);
    Make(L"STATIC", core::Tr(L"license.dialog.seatsLabel", L"席位：").c_str(),
         WS_VISIBLE | SS_LEFT, kIdSeatsLabel);
    seatsEdit_ = Make(L"EDIT", L"1", WS_VISIBLE | WS_TABSTOP | ES_NUMBER | ES_AUTOHSCROLL,
                      kIdSeatsEdit, WS_EX_CLIENTEDGE);
    Make(L"STATIC", core::Tr(L"license.dialog.holderLabel", L"单位名：").c_str(),
         WS_VISIBLE | SS_LEFT, kIdHolderLabel);
    holderEdit_ = Make(L"EDIT", L"", WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL, kIdHolderEdit,
                       WS_EX_CLIENTEDGE);
    challengeLabel_ =
        Make(L"STATIC",
             core::Tr(L"license.dialog.challengeLabel", L"申请串（发给客服，用于离线签发）：")
                 .c_str(),
             WS_VISIBLE | SS_LEFT, kIdChallengeLabel);
    challengeText_ =
        Make(L"EDIT", L"",
             WS_VISIBLE | WS_TABSTOP | ES_MULTILINE | ES_READONLY | WS_VSCROLL | ES_AUTOVSCROLL,
             kIdChallengeText, WS_EX_CLIENTEDGE);
    Make(L"BUTTON", core::Tr(L"license.dialog.copyChallenge", L"复制申请串(&C)").c_str(),
         WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, kIdCopyChallenge);
    Make(L"BUTTON", core::Tr(L"license.dialog.saveChallenge", L"保存为文件(&V)…").c_str(),
         WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, kIdSaveChallenge);

    // 分区 4：导入许可文件
    Make(L"BUTTON", core::Tr(L"license.dialog.importGroup", L"导入许可文件").c_str(),
         WS_VISIBLE | BS_GROUPBOX, kIdImportGroup);
    importPathEdit_ = Make(L"EDIT", L"", WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL, kIdImportPath,
                           WS_EX_CLIENTEDGE);
    Make(L"BUTTON", core::Tr(L"license.dialog.browse", L"浏览(&B)…").c_str(),
         WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, kIdBrowse);
    Make(L"BUTTON", core::Tr(L"license.dialog.import", L"导入(&I)").c_str(),
         WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, kIdImport);
    importHint_ = Make(L"STATIC",
                       core::Tr(L"license.dialog.importHint",
                                L"导入会先验签：不通过的文件不会覆盖现有许可。")
                           .c_str(),
                       WS_VISIBLE | SS_LEFT, kIdImportHint);

    Make(L"BUTTON", core::Tr(L"license.dialog.close", L"关闭").c_str(),
         WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, kIdClose);

    if (font_ != nullptr) {
        ::EnumChildWindows(
            hwnd_,
            [](const HWND child, const LPARAM param) -> BOOL {
                ::SendMessageW(child, WM_SETFONT, static_cast<WPARAM>(param), TRUE);
                return TRUE;
            },
            reinterpret_cast<LPARAM>(font_));
    }
    Layout();

    {
        std::wstring label = core::Tr(L"license.dialog.serviceLabel", L"服务地址：");
        label += serviceUrl_.empty()
                     ? core::Tr(L"license.dialog.serviceMissing",
                                L"（未配置——请用下面的离线激活）")
                     : infra::WideFromUtf8(serviceUrl_);
        ::SetWindowTextW(serviceLabel_, label.c_str());
    }
    RefreshStatus();
    RefreshChallenge();
    UpdateButtons();
    ready_ = true;

    ::EnableWindow(owner_, FALSE);
    ::ShowWindow(hwnd_, SW_SHOW);
    Layout();
    ::SetFocus(keyEdit_);

    MSG message{};
    while (!done_ && ::IsWindow(hwnd_) != 0) {
        const BOOL fetched = ::GetMessageW(&message, nullptr, 0, 0);
        if (fetched <= 0) {
            break;
        }
        // IsDialogMessage 让 Tab/回车按系统对话框语义走（回车 = 默认按钮）
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

    LicenseDialogOutcome outcome;
    outcome.changed = changed_;
    return outcome;
}

}  // namespace

LicenseDialogOutcome LicenseDialog::Show(HWND owner, infra::ThreadPool& pool,
                                         std::string serviceUrl) {
    LicenseDialogImpl dialog;
    return dialog.Show(owner, pool, std::move(serviceUrl));
}

}  // namespace npcc::shell
