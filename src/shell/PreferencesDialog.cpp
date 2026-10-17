#include "shell/PreferencesDialog.h"

#include <commctrl.h>

#include <algorithm>
#include <cwchar>
#include <string>
#include <vector>

#include "core/ConfigCenter.h"
#include "core/Localization.h"
#include "core/UpdateManifest.h"
#include "infra/Log.h"
#include "infra/UpdateService.h"
#include "shell/ChromeStyle.h"
#include "shell/MessageDialog.h"

namespace npcc::shell {
namespace {

constexpr int            kGap      = 8;
constexpr int            kRow      = 26;
constexpr int            kPageCount = 4;
constexpr const wchar_t* kWindowClass = L"notepadccPreferences";

enum PrefControl : int {
    kPrefTab = 1000,

    // 常规
    kPrefRestoreSession = 1001,
    kPrefRecentCount    = 1002,
    kPrefMultiInstance  = 1003,

    // 编辑
    kPrefTabWidth        = 1010,
    kPrefTabToSpaces     = 1011,
    kPrefAutoIndent      = 1012,
    kPrefSmartIndent     = 1013,
    kPrefShowWhitespace  = 1014,
    kPrefShowEol         = 1015,
    kPrefShowLineNumbers = 1016,
    kPrefIndentGuide     = 1017,
    kPrefCaretLine       = 1018,
    kPrefWrap            = 1019,
    kPrefZoom            = 1020,

    // 新建文件
    kPrefDefaultEncoding = 1030,
    kPrefDefaultCodePage = 1031,
    kPrefDefaultEol      = 1032,

    // 高级
    kPrefBackupEnabled  = 1040,
    kPrefBackupInterval = 1041,
    kPrefBackupKeep     = 1042,
    kPrefConvertBackup  = 1047,
    kPrefTier1MB        = 1043,
    kPrefTier2MB        = 1044,
    kPrefRefuseMB       = 1045,
    kPrefAdvancedNote   = 1046,
    kPrefUiLanguage     = 1048,
    kPrefUpdateCheck    = 1049,
    kPrefUpdateUrl      = 1050,

    kPrefOk     = 1060,
    kPrefCancel = 1061,
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

// 一页里的一行：可选的标签 + 可选的输入控件。
// fieldWidth 为 0 表示铺满剩余宽度；windowExtraHeight 给下拉框预留展开高度。
struct PrefRow {
    HWND label = nullptr;
    HWND field = nullptr;
    int  fieldWidth = 0;
    int  windowExtraHeight = 0;
};

// 页签名与它们的语言包键（文案在 BuildPages 里取，见那里的说明）
const wchar_t* const kPageTitleKeys[kPageCount] = {
    L"prefs.page.general", L"prefs.page.edit", L"prefs.page.newFile", L"prefs.page.advanced",
};
const wchar_t* const kPageTitleTexts[kPageCount] = {
    L"常规", L"编辑", L"新建文件", L"高级",
};

class PreferencesDialog {
public:
    bool Show(HWND owner, npcc::core::ConfigCenter& config);

private:
    static LRESULT CALLBACK WndProcThunk(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT WndProc(UINT message, WPARAM wParam, LPARAM lParam);

    int  Scale(int value) const;
    HWND Make(const wchar_t* cls, const wchar_t* text, DWORD style, int id,
              DWORD exStyle = 0);
    void Row(int page, HWND label, HWND field, int fieldWidth = 0, int extraHeight = 0);
    // 文案都走语言包：调用点写 Tr(键, 中文默认文本)，两个参数都在原地看得见，
    // 键名（prefs.*）与默认文本挨着写，改文案时不容易漏掉一半
    HWND Label(const std::wstring& text);
    HWND Edit(int id, const wchar_t* text, bool allowNegative = false);
    HWND Check(int id, const std::wstring& text);
    HWND Combo(int id, const wchar_t* const* items, int count);

    void BuildPages();
    void BuildGeneralPage();
    void BuildEditPage();
    void BuildNewFilePage();
    void BuildAdvancedPage();
    void Layout();
    void ShowPage(int page);

    void LoadFromConfig();
    // 返回 false 表示有字段非法：对话框不关闭，error 说明原因，badControlId 是需要聚焦的控件。
    bool StoreToConfig(std::wstring& error, int& badControlId);

    HWND         Ctrl(int id) const { return ::GetDlgItem(hwnd_, id); }
    std::wstring TextOf(int id) const;
    void         SetInt(int id, int value) const;
    void         SetText(int id, const std::wstring& text) const;
    bool         IsChecked(int id) const;
    void         SetChecked(int id, bool checked) const;
    void         SetComboIndex(int id, int index) const;
    int          ComboIndex(int id) const;

    HWND                      owner_ = nullptr;
    HWND                      hwnd_ = nullptr;
    HFONT                     font_ = nullptr;
    bool                      done_ = false;
    bool                      applied_ = false;
    npcc::core::ConfigCenter* config_ = nullptr;
    HWND                      noteLabel_ = nullptr;
    std::vector<PrefRow>      rows_[kPageCount];
    // 界面语言下拉里每一项对应的 locale（取值时按下标查回来）
    std::vector<std::wstring> uiLanguageIds_;
};

int PreferencesDialog::Scale(int value) const {
    return ::MulDiv(value, DpiOf(owner_), 96);
}

HWND PreferencesDialog::Make(const wchar_t* cls, const wchar_t* text, DWORD style, int id,
                             DWORD exStyle) {
    return ::CreateWindowExW(exStyle, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10,
                             hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                             ::GetModuleHandleW(nullptr), nullptr);
}

void PreferencesDialog::Row(int page, HWND label, HWND field, int fieldWidth, int extraHeight) {
    if (page < 0 || page >= kPageCount) {
        return;
    }
    PrefRow row;
    row.label = label;
    row.field = field;
    row.fieldWidth = fieldWidth;
    row.windowExtraHeight = extraHeight;
    rows_[page].push_back(row);
}

HWND PreferencesDialog::Label(const std::wstring& text) {
    return Make(L"STATIC", text.c_str(), WS_VISIBLE, -1);
}

HWND PreferencesDialog::Edit(int id, const wchar_t* text, bool allowNegative) {
    DWORD style = WS_VISIBLE | WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL;
    if (!allowNegative) {
        style |= ES_NUMBER;
    }
    return Make(L"EDIT", text, style, id);
}

HWND PreferencesDialog::Check(int id, const std::wstring& text) {
    return Make(L"BUTTON", text.c_str(), WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX, id);
}

HWND PreferencesDialog::Combo(int id, const wchar_t* const* items, int count) {
    HWND combo = Make(WC_COMBOBOXW, L"",
                      WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL, id);
    if (combo != nullptr) {
        for (int i = 0; i < count; ++i) {
            ::SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(items[i]));
        }
        ::SendMessageW(combo, CB_SETCURSEL, 0, 0);
    }
    return combo;
}

void PreferencesDialog::BuildGeneralPage() {
    const int page = 0;
    Row(page, nullptr,
        Check(kPrefRestoreSession,
              core::Tr(L"prefs.restoreSession", L"启动时恢复上次打开的会话")));
    Row(page, nullptr,
        Check(kPrefMultiInstance,
              core::Tr(L"prefs.multiInstance",
                       L"允许同时运行多个实例（默认关闭：再启动会把文件交给已在跑的实例）")));
    Row(page, Label(core::Tr(L"prefs.recentCount", L"最近文件条数（0~100）：")),
        Edit(kPrefRecentCount, L"20"), Scale(90));
    // 界面语言：与别的项一样，点"确定"时写进配置；外壳发现它变了就当场换语言
    //（不在这里直接切：对话框不该反过来驱动外壳）
    {
        const std::vector<std::pair<std::wstring, std::wstring>> available =
            core::Localization::Ui().Available();
        std::vector<std::wstring>        names;
        std::vector<const wchar_t*>      pointers;
        names.reserve(available.size());
        pointers.reserve(available.size());
        uiLanguageIds_.clear();
        for (const std::pair<std::wstring, std::wstring>& item : available) {
            uiLanguageIds_.push_back(item.first);
            names.push_back(item.second);
        }
        for (const std::wstring& name : names) {
            pointers.push_back(name.c_str());
        }
        Row(page, Label(core::Tr(L"prefs.uiLanguage", L"界面语言：")),
            Combo(kPrefUiLanguage, pointers.data(), static_cast<int>(pointers.size())),
            Scale(220), Scale(kRow) * 5);
    }
}

void PreferencesDialog::BuildEditPage() {
    const int page = 1;
    Row(page, Label(core::Tr(L"prefs.tabWidth", L"制表符宽度（1~64）：")),
        Edit(kPrefTabWidth, L"4"), Scale(70));
    Row(page, nullptr, Check(kPrefTabToSpaces, core::Tr(L"prefs.tabToSpaces", L"用空格代替制表符")));
    Row(page, nullptr, Check(kPrefAutoIndent, core::Tr(L"prefs.autoIndent", L"自动缩进")));
    Row(page, nullptr,
        Check(kPrefSmartIndent,
              core::Tr(L"prefs.smartIndent", L"智能缩进（跟随上一行语义）")));
    Row(page, nullptr,
        Check(kPrefShowWhitespace, core::Tr(L"prefs.showWhitespace", L"显示空白符号")));
    Row(page, nullptr, Check(kPrefShowEol, core::Tr(L"prefs.showEol", L"显示行尾符")));
    Row(page, nullptr,
        Check(kPrefShowLineNumbers, core::Tr(L"prefs.showLineNumbers", L"显示行号")));
    Row(page, nullptr,
        Check(kPrefIndentGuide, core::Tr(L"prefs.indentGuide", L"显示缩进引导线")));
    Row(page, nullptr,
        Check(kPrefCaretLine, core::Tr(L"prefs.caretLine", L"高亮光标所在行")));
    Row(page, nullptr, Check(kPrefWrap, core::Tr(L"prefs.wrap", L"自动换行")));
    Row(page, Label(core::Tr(L"prefs.zoom", L"缩放级别（-20~40）：")),
        Edit(kPrefZoom, L"0", true), Scale(70));
}

void PreferencesDialog::BuildNewFilePage() {
    const int page = 2;
    // 编码/行尾那几项的显示文本也走语言包（键与默认文本一一对应，顺序即下拉顺序）
    const std::wstring encodingItems[] = {
        L"UTF-8",
        core::Tr(L"prefs.enc.utf8Bom", L"UTF-8（带 BOM）"),
        core::Tr(L"prefs.enc.ansi", L"ANSI（按代码页）"),
        L"UTF-16 LE",
        L"UTF-16 BE",
    };
    const std::wstring eolItems[] = {
        core::Tr(L"prefs.eol.crlf", L"CRLF（Windows）"),
        core::Tr(L"prefs.eol.lf", L"LF（Unix）"),
        core::Tr(L"prefs.eol.cr", L"CR（Mac）"),
    };
    const wchar_t* encodingPointers[5] = {encodingItems[0].c_str(), encodingItems[1].c_str(),
                                          encodingItems[2].c_str(), encodingItems[3].c_str(),
                                          encodingItems[4].c_str()};
    const wchar_t* eolPointers[3] = {eolItems[0].c_str(), eolItems[1].c_str(),
                                     eolItems[2].c_str()};

    Row(page, Label(core::Tr(L"prefs.defaultEncoding", L"默认编码：")),
        Combo(kPrefDefaultEncoding, encodingPointers, 5), Scale(220), Scale(kRow) * 7);
    Row(page, Label(core::Tr(L"prefs.defaultCodePage", L"默认代码页（仅 ANSI 用）：")),
        Edit(kPrefDefaultCodePage, L"936"), Scale(90));
    Row(page, Label(core::Tr(L"prefs.defaultEol", L"默认行尾：")),
        Combo(kPrefDefaultEol, eolPointers, 3), Scale(220), Scale(kRow) * 5);
}

void PreferencesDialog::BuildAdvancedPage() {
    const int page = 3;
    Row(page, nullptr, Check(kPrefBackupEnabled, core::Tr(L"prefs.backupEnabled", L"启用崩溃恢复快照")));
    Row(page, Label(core::Tr(L"prefs.backupInterval", L"快照间隔（秒，5~3600）：")),
        Edit(kPrefBackupInterval, L"30"), Scale(90));
    Row(page, Label(core::Tr(L"prefs.backupKeep", L"保留崩溃快照数（1~100）：")),
        Edit(kPrefBackupKeep, L"10"), Scale(90));
    Row(page, nullptr,
        Check(kPrefConvertBackup,
              core::Tr(L"prefs.convertBackup", L"转换编码前给文件留一份 .bak 备份")));
    Row(page, Label(core::Tr(L"prefs.tier1", L"降级阈值（MB，超过改用映射装载）：")),
        Edit(kPrefTier1MB, L"8"), Scale(90));
    Row(page, Label(core::Tr(L"prefs.tier2", L"只读阈值（MB，超过只读打开）：")),
        Edit(kPrefTier2MB, L"64"), Scale(90));
    Row(page, Label(core::Tr(L"prefs.refuse", L"拒绝阈值（MB，超过不再打开）：")),
        Edit(kPrefRefuseMB, L"512"), Scale(90));
    Row(page, nullptr,
        Check(kPrefUpdateCheck, core::Tr(L"prefs.updateCheck", L"启动后自动检查更新")));
    Row(page, Label(core::Tr(L"prefs.updateUrl", L"更新源 URL（必须是 https）：")),
        Edit(kPrefUpdateUrl, L"", true), Scale(320));
    noteLabel_ = Label(core::Tr(
        L"prefs.advancedNote",
        L"说明：三个阈值必须严格递增，保存后立即生效。未保存的内容按上面的间隔定时写入"
        L"配置目录的 backup 下；超过降级阈值的文档不做定时快照（仍会在崩溃时抢救），"
        L"异常退出时另会生成一份崩溃报告与内存转储。"));
    Row(page, noteLabel_, nullptr);
}

void PreferencesDialog::BuildPages() {
    HWND tab = Make(WC_TABCONTROLW, L"", WS_VISIBLE | WS_TABSTOP | TCS_TABS, kPrefTab);
    if (tab != nullptr) {
        for (int i = 0; i < kPageCount; ++i) {
            TCITEMW item{};
            item.mask = TCIF_TEXT;
            // TCM_INSERTITEMW 会把文本拷走，所以临时字符串在这里是安全的
            const std::wstring title = core::Tr(kPageTitleKeys[i], kPageTitleTexts[i]);
            item.pszText = const_cast<wchar_t*>(title.c_str());
            ::SendMessageW(tab, TCM_INSERTITEMW, static_cast<WPARAM>(i),
                           reinterpret_cast<LPARAM>(&item));
        }
        ::SendMessageW(tab, TCM_SETCURSEL, 0, 0);
    }

    BuildGeneralPage();
    BuildEditPage();
    BuildNewFilePage();
    BuildAdvancedPage();

    const std::wstring ok = core::Tr(L"prefs.ok", L"确定");
    const std::wstring cancel = core::Tr(L"prefs.cancel", L"取消");
    Make(L"BUTTON", ok.c_str(), WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON, kPrefOk);
    Make(L"BUTTON", cancel.c_str(), WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, kPrefCancel);
    (void)::SendMessageW(hwnd_, DM_SETDEFID, kPrefOk, 0);
}

void PreferencesDialog::Layout() {
    RECT client{};
    ::GetClientRect(hwnd_, &client);
    const int gap = Scale(kGap);
    const int rowHeight = Scale(kRow);

    const int buttonHeight = Scale(30);
    const int buttonWidth = Scale(100);
    int buttonX = client.right - gap - buttonWidth;
    for (const int id : {kPrefCancel, kPrefOk}) {
        if (HWND control = Ctrl(id); control != nullptr) {
            ::MoveWindow(control, buttonX, client.bottom - buttonHeight - gap, buttonWidth,
                         buttonHeight, TRUE);
            buttonX -= buttonWidth + gap;
        }
    }

    HWND tab = Ctrl(kPrefTab);
    if (tab == nullptr) {
        return;
    }
    RECT tabRect{};
    ::GetWindowRect(tab, &tabRect);
    ::MapWindowPoints(nullptr, hwnd_, reinterpret_cast<POINT*>(&tabRect), 2);
    RECT content = tabRect;
    ::SendMessageW(tab, TCM_ADJUSTRECT, FALSE, reinterpret_cast<LPARAM>(&content));
    // 页内留白：左右各 gap，顶部 gap
    content.left += gap;
    content.right -= gap;
    content.top += gap;

    const int labelWidth = Scale(215);
    for (int page = 0; page < kPageCount; ++page) {
        int y = content.top;
        for (const PrefRow& row : rows_[page]) {
            if (row.label != nullptr) {
                ::MoveWindow(row.label, content.left, y + Scale(4), labelWidth, rowHeight, TRUE);
            }
            if (row.field != nullptr) {
                const int fieldX = row.label != nullptr ? content.left + labelWidth : content.left;
                const int available = static_cast<int>(content.right) - fieldX;
                const int fieldWidth =
                    row.fieldWidth > 0 ? row.fieldWidth : std::max(Scale(60), available);
                ::MoveWindow(row.field, fieldX, y, fieldWidth,
                             rowHeight + row.windowExtraHeight, TRUE);
            }
            y += rowHeight + Scale(2);
        }
        // 高级页末尾是一段说明文字，给它三行的高度
        if (noteLabel_ != nullptr && page == 3) {
            ::MoveWindow(noteLabel_, content.left, y - rowHeight - Scale(2),
                         content.right - content.left, rowHeight * 3, TRUE);
        }
    }
}

void PreferencesDialog::ShowPage(int page) {
    if (page < 0 || page >= kPageCount) {
        return;
    }
    for (int p = 0; p < kPageCount; ++p) {
        const int show = (p == page) ? SW_SHOW : SW_HIDE;
        for (const PrefRow& row : rows_[p]) {
            if (row.label != nullptr) {
                ::ShowWindow(row.label, show);
            }
            if (row.field != nullptr) {
                ::ShowWindow(row.field, show);
            }
        }
    }
}

std::wstring PreferencesDialog::TextOf(int id) const {
    const HWND control = Ctrl(id);
    if (control == nullptr) {
        return {};
    }
    const int length = ::GetWindowTextLengthW(control);
    if (length <= 0) {
        return {};
    }
    std::wstring text(static_cast<size_t>(length) + 1, L'\0');
    ::GetWindowTextW(control, text.data(), length + 1);
    text.resize(static_cast<size_t>(length));
    return text;
}

void PreferencesDialog::SetInt(int id, int value) const {
    if (HWND control = Ctrl(id); control != nullptr) {
        const std::wstring text = std::to_wstring(value);
        ::SetWindowTextW(control, text.c_str());
    }
}

void PreferencesDialog::SetText(int id, const std::wstring& text) const {
    if (HWND control = Ctrl(id); control != nullptr) {
        ::SetWindowTextW(control, text.c_str());
    }
}

bool PreferencesDialog::IsChecked(int id) const {
    const HWND control = Ctrl(id);
    return control != nullptr &&
           ::SendMessageW(control, BM_GETCHECK, 0, 0) == static_cast<LRESULT>(BST_CHECKED);
}

void PreferencesDialog::SetChecked(int id, bool checked) const {
    if (HWND control = Ctrl(id); control != nullptr) {
        ::SendMessageW(control, BM_SETCHECK, checked ? BST_CHECKED : BST_UNCHECKED, 0);
    }
}

void PreferencesDialog::SetComboIndex(int id, int index) const {
    if (HWND control = Ctrl(id); control != nullptr) {
        ::SendMessageW(control, CB_SETCURSEL, static_cast<WPARAM>(index), 0);
    }
}

int PreferencesDialog::ComboIndex(int id) const {
    const HWND control = Ctrl(id);
    if (control == nullptr) {
        return 0;
    }
    const LRESULT index = ::SendMessageW(control, CB_GETCURSEL, 0, 0);
    return index == CB_ERR ? 0 : static_cast<int>(index);
}

void PreferencesDialog::LoadFromConfig() {
    const core::ConfigCenter& config = *config_;

    SetChecked(kPrefRestoreSession, config.General().restoreSessionOnStart);
    SetChecked(kPrefMultiInstance, config.General().multiInstance);
    SetInt(kPrefRecentCount, static_cast<int>(config.General().recentFileCount));
    {
        int index = 0;
        for (std::size_t slot = 0; slot < uiLanguageIds_.size(); ++slot) {
            if (uiLanguageIds_[slot] == config.General().uiLanguage) {
                index = static_cast<int>(slot);
                break;
            }
        }
        SetComboIndex(kPrefUiLanguage, index);
    }

    const core::EditConfig& edit = config.Edit();
    SetInt(kPrefTabWidth, static_cast<int>(edit.tabWidth));
    SetChecked(kPrefTabToSpaces, edit.tabToSpaces);
    SetChecked(kPrefAutoIndent, edit.autoIndent);
    SetChecked(kPrefSmartIndent, edit.smartIndent);
    SetChecked(kPrefShowWhitespace, edit.showWhitespace);
    SetChecked(kPrefShowEol, edit.showEol);
    SetChecked(kPrefShowLineNumbers, edit.showLineNumbers);
    SetChecked(kPrefIndentGuide, edit.showIndentGuide);
    SetChecked(kPrefCaretLine, edit.caretLineHighlight);
    SetChecked(kPrefWrap, edit.wrapLongLines);
    SetInt(kPrefZoom, static_cast<int>(static_cast<int32_t>(edit.zoomLevel)));

    switch (config.NewFile().defaultUniMode) {
        case edit::UniMode::Utf8Bom: SetComboIndex(kPrefDefaultEncoding, 1); break;
        case edit::UniMode::Ansi:    SetComboIndex(kPrefDefaultEncoding, 2); break;
        case edit::UniMode::Utf16LE: SetComboIndex(kPrefDefaultEncoding, 3); break;
        case edit::UniMode::Utf16BE: SetComboIndex(kPrefDefaultEncoding, 4); break;
        case edit::UniMode::Utf8:
        default:                     SetComboIndex(kPrefDefaultEncoding, 0); break;
    }
    SetInt(kPrefDefaultCodePage, static_cast<int>(config.NewFile().defaultCodePage));
    switch (config.NewFile().defaultEol) {
        case edit::EolMode::Lf: SetComboIndex(kPrefDefaultEol, 1); break;
        case edit::EolMode::Cr: SetComboIndex(kPrefDefaultEol, 2); break;
        case edit::EolMode::Crlf:
        case edit::EolMode::Mixed:
        default:                SetComboIndex(kPrefDefaultEol, 0); break;
    }

    SetChecked(kPrefBackupEnabled, config.Backup().enabled);
    SetInt(kPrefBackupInterval, static_cast<int>(config.Backup().intervalSeconds));
    SetInt(kPrefBackupKeep, static_cast<int>(config.Backup().keepSnapshots));
    SetChecked(kPrefConvertBackup, config.Backup().keepConvertBackup);

    const core::LargeFileConfig& large = config.LargeFile();
    SetInt(kPrefTier1MB, static_cast<int>(large.tier1MaxBytes / (1024 * 1024)));
    SetInt(kPrefTier2MB, static_cast<int>(large.tier2MaxBytes / (1024 * 1024)));
    SetInt(kPrefRefuseMB, static_cast<int>(large.refuseAboveBytes / (1024 * 1024)));

    SetChecked(kPrefUpdateCheck, config.Update().checkOnStartup);
    SetText(kPrefUpdateUrl, config.Update().sourceUrl);
}

bool PreferencesDialog::StoreToConfig(std::wstring& error, int& badControlId) {
    auto readInt = [this, &error, &badControlId](int id, const std::wstring& name, long long low,
                                                 long long high, long long& out) -> bool {
        const std::wstring text = TextOf(id);
        wchar_t*           end = nullptr;
        const long long    value = text.empty() ? 0 : std::wcstoll(text.c_str(), &end, 10);
        if (text.empty() || end == nullptr || *end != L'\0' || value < low || value > high) {
            // 整句交给语言包 + {0}{1}{2}{3}：分段拼接在英文里读不通（语序、冠词都不同）
            error = core::Tr(L"prefs.error.range",
                             L"「{0}」必须是 {1} 到 {2} 之间的整数，当前输入「{3}」。",
                             {name, std::to_wstring(low), std::to_wstring(high), text});
            badControlId = id;
            return false;
        }
        out = value;
        return true;
    };

    core::ConfigCenter& config = *config_;
    long long           value = 0;

    if (!readInt(kPrefRecentCount, core::Tr(L"prefs.field.recentCount", L"最近文件列表保留条数"),
                 0, 100, value)) {
        return false;
    }
    const std::uint32_t recentCount = static_cast<std::uint32_t>(value);

    if (!readInt(kPrefTabWidth, core::Tr(L"prefs.field.tabWidth", L"制表符宽度"), 1, 64,
                 value)) {
        return false;
    }
    const std::uint32_t tabWidth = static_cast<std::uint32_t>(value);
    if (!readInt(kPrefZoom, core::Tr(L"prefs.field.zoom", L"缩放级别"), -20, 40, value)) {
        return false;
    }
    const int zoom = static_cast<int>(value);
    if (!readInt(kPrefDefaultCodePage, core::Tr(L"prefs.field.codePage", L"默认代码页"), 1,
                 65535, value)) {
        return false;
    }
    const std::uint32_t codePage = static_cast<std::uint32_t>(value);
    if (!readInt(kPrefBackupInterval, core::Tr(L"prefs.field.backupInterval", L"快照间隔"), 5,
                 3600, value)) {
        return false;
    }
    const std::uint32_t interval = static_cast<std::uint32_t>(value);
    if (!readInt(kPrefBackupKeep, core::Tr(L"prefs.field.backupKeep", L"保留快照数量"), 1, 100,
                 value)) {
        return false;
    }
    const std::uint32_t keep = static_cast<std::uint32_t>(value);

    long long tier1 = 0;
    long long tier2 = 0;
    long long refuse = 0;
    if (!readInt(kPrefTier1MB, core::Tr(L"prefs.field.tier1", L"大文件阈值"), 1, 4096, tier1) ||
        !readInt(kPrefTier2MB, core::Tr(L"prefs.field.tier2", L"只读阈值"), 1, 4096, tier2) ||
        !readInt(kPrefRefuseMB, core::Tr(L"prefs.field.refuse", L"拒绝打开阈值"), 1, 65536,
                 refuse)) {
        return false;
    }
    if (!(tier1 < tier2 && tier2 < refuse)) {
        // 三个阈值必须严格递增，否则整组不采纳，避免出现"只读阈值小于降级阈值"这种自相矛盾的状态
        error = core::Tr(L"prefs.error.tiers",
                         L"大文件阈值必须严格递增：降级阈值 < 只读阈值 < 拒绝打开阈值。");
        badControlId = kPrefTier1MB;
        infra::LogWarn(L"config", L"首选项：大文件阈值不满足递增要求，已放弃本次大文件设置");
        return false;
    }

    config.General().restoreSessionOnStart = IsChecked(kPrefRestoreSession);
    config.General().multiInstance = IsChecked(kPrefMultiInstance);
    config.General().recentFileCount = recentCount;
    {
        // 界面语言写进配置；**换语言的动作由外壳做**（它要重建菜单），这里只管存值
        const int index = ComboIndex(kPrefUiLanguage);
        if (index >= 0 && index < static_cast<int>(uiLanguageIds_.size())) {
            config.General().uiLanguage = uiLanguageIds_[static_cast<std::size_t>(index)];
        }
    }

    core::EditConfig& edit = config.Edit();
    edit.tabWidth = tabWidth;
    edit.tabToSpaces = IsChecked(kPrefTabToSpaces);
    edit.autoIndent = IsChecked(kPrefAutoIndent);
    edit.smartIndent = IsChecked(kPrefSmartIndent);
    edit.showWhitespace = IsChecked(kPrefShowWhitespace);
    edit.showEol = IsChecked(kPrefShowEol);
    edit.showLineNumbers = IsChecked(kPrefShowLineNumbers);
    edit.showIndentGuide = IsChecked(kPrefIndentGuide);
    edit.caretLineHighlight = IsChecked(kPrefCaretLine);
    edit.wrapLongLines = IsChecked(kPrefWrap);
    edit.zoomLevel = static_cast<std::uint32_t>(static_cast<std::int32_t>(zoom));

    core::NewFileConfig& newFile = config.NewFile();
    switch (ComboIndex(kPrefDefaultEncoding)) {
        case 1: newFile.defaultUniMode = edit::UniMode::Utf8Bom; break;
        case 2: newFile.defaultUniMode = edit::UniMode::Ansi; break;
        case 3: newFile.defaultUniMode = edit::UniMode::Utf16LE; break;
        case 4: newFile.defaultUniMode = edit::UniMode::Utf16BE; break;
        default: newFile.defaultUniMode = edit::UniMode::Utf8; break;
    }
    newFile.defaultCodePage = codePage;
    switch (ComboIndex(kPrefDefaultEol)) {
        case 1: newFile.defaultEol = edit::EolMode::Lf; break;
        case 2: newFile.defaultEol = edit::EolMode::Cr; break;
        default: newFile.defaultEol = edit::EolMode::Crlf; break;
    }

    config.Backup().enabled = IsChecked(kPrefBackupEnabled);
    config.Backup().intervalSeconds = interval;
    config.Backup().keepSnapshots = keep;
    config.Backup().keepConvertBackup = IsChecked(kPrefConvertBackup);

    config.LargeFile().tier1MaxBytes = static_cast<std::uint64_t>(tier1) * 1024 * 1024;
    config.LargeFile().tier2MaxBytes = static_cast<std::uint64_t>(tier2) * 1024 * 1024;
    config.LargeFile().refuseAboveBytes = static_cast<std::uint64_t>(refuse) * 1024 * 1024;

    // 更新源：在这里就用与检查时同一个策略函数验一遍——输错了当场说清楚，
    // 而不是等 30 秒后的自动检查在日志里失败（日志普通用户不会看）
    {
        std::wstring url = TextOf(kPrefUpdateUrl);
        const std::size_t first = url.find_first_not_of(L" \t");
        const std::size_t last = url.find_last_not_of(L" \t");
        url = first == std::wstring::npos ? std::wstring() : url.substr(first, last - first + 1);
        std::wstring why;
        if (!core::IsAllowedUpdateUrl(infra::Utf8FromWide(url), &why)) {
            error = core::Tr(L"prefs.error.updateUrl",
                             L"更新源不合规：{0}（要求 https://，本机 http://127.0.0.1 仅供测试）",
                             {why});
            badControlId = kPrefUpdateUrl;
            return false;
        }
        config.Update().checkOnStartup = IsChecked(kPrefUpdateCheck);
        config.Update().sourceUrl = url;
    }

    config.RequestSave();   // 合并写盘：真正落盘可能延后到下一次请求或退出时
    return true;
}

bool PreferencesDialog::Show(HWND owner, npcc::core::ConfigCenter& config) {
    owner_ = owner;
    config_ = &config;

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = &PreferencesDialog::WndProcThunk;
    wc.hInstance = ::GetModuleHandleW(nullptr);
    wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(static_cast<INT_PTR>(COLOR_BTNFACE + 1));
    wc.lpszClassName = kWindowClass;
    ::RegisterClassExW(&wc);

    RECT ownerRect{};
    ::GetWindowRect(owner, &ownerRect);
    const int width = Scale(560);
    const int height = Scale(430);
    const int x = ownerRect.left + ((ownerRect.right - ownerRect.left) - width) / 2;
    const int y = ownerRect.top + ((ownerRect.bottom - ownerRect.top) - height) / 2;

    const std::wstring title = core::Tr(L"prefs.title", L"首选项");
    hwnd_ = ::CreateWindowExW(WS_EX_DLGMODALFRAME, kWindowClass, title.c_str(),
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

    BuildPages();

    if (font_ != nullptr) {
        ::EnumChildWindows(
            hwnd_,
            [](HWND child, LPARAM param) -> BOOL {
                ::SendMessageW(child, WM_SETFONT, static_cast<WPARAM>(param), TRUE);
                return TRUE;
            },
            reinterpret_cast<LPARAM>(font_));
    }

    ApplyDialogChrome(hwnd_);
    LoadFromConfig();
    Layout();
    ShowPage(0);

    ::EnableWindow(owner_, FALSE);
    ::ShowWindow(hwnd_, SW_SHOW);
    ::SetFocus(Ctrl(kPrefRestoreSession));

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
    return applied_;
}

LRESULT CALLBACK PreferencesDialog::WndProcThunk(HWND hwnd, UINT message, WPARAM wParam,
                                                 LPARAM lParam) {
    PreferencesDialog* self =
        reinterpret_cast<PreferencesDialog*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        self = static_cast<PreferencesDialog*>(create->lpCreateParams);
        self->hwnd_ = hwnd;
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    if (self == nullptr) {
        return ::DefWindowProcW(hwnd, message, wParam, lParam);
    }
    return self->WndProc(message, wParam, lParam);
}

LRESULT PreferencesDialog::WndProc(UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_NOTIFY: {
            const auto* header = reinterpret_cast<const NMHDR*>(lParam);
            if (header != nullptr && header->idFrom == kPrefTab && header->code == TCN_SELCHANGE) {
                const LRESULT index = ::SendMessageW(Ctrl(kPrefTab), TCM_GETCURSEL, 0, 0);
                if (index >= 0) {
                    ShowPage(static_cast<int>(index));
                }
            }
            break;
        }
        case WM_COMMAND: {
            switch (LOWORD(wParam)) {
                case kPrefOk: {
                    std::wstring error;
                    int          badControl = 0;
                    if (!StoreToConfig(error, badControl)) {
                        ShowMessageBox(hwnd_, error.c_str(), L"首选项", MB_ICONWARNING);
                        if (HWND control = Ctrl(badControl); control != nullptr) {
                            ::SetFocus(control);
                        }
                        return 0;
                    }
                    applied_ = true;
                    done_ = true;
                    return 0;
                }
                case kPrefCancel:
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

}  // namespace

bool ShowPreferencesDialog(HWND owner, npcc::core::ConfigCenter& config) {
    PreferencesDialog dialog;
    return dialog.Show(owner, config);
}

}  // namespace npcc::shell
