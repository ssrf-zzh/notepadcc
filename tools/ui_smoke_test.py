# -*- coding: utf-8 -*-
"""界面回归脚本：驱动 notepadcc 的查找栏与"在文件中查找"面板，做真实的端到端验证。

它不看窗口标题之类的表象，而是真的发命令、填输入框、点按钮，
再回来核对编辑器选区与结果列表，因此能抓住"编译通过但功能没接上"的问题。
"""
import ctypes
import base64
import os
import quopri
import shutil
import subprocess
import sys
import time
import winreg
import zipfile
from ctypes import wintypes

user32 = ctypes.WinDLL("user32", use_last_error=True)
user32.FindWindowW.restype = wintypes.HWND
user32.GetDlgItem.restype = wintypes.HWND
user32.SendMessageW.restype = ctypes.c_ssize_t
# 必须显式声明参数类型：不声明时 ctypes 会把指针参数按 32 位传，
# 在 x64 上地址会被截断，Scintilla 读到的是野地址。
user32.SendMessageW.argtypes = [wintypes.HWND, ctypes.c_uint, ctypes.c_size_t,
                                ctypes.c_ssize_t]
kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
kernel32.GlobalSize.restype = ctypes.c_size_t
kernel32.GlobalSize.argtypes = [wintypes.HGLOBAL]
kernel32.GlobalLock.restype = ctypes.c_void_p
kernel32.GlobalLock.argtypes = [wintypes.HGLOBAL]
kernel32.GlobalUnlock.argtypes = [wintypes.HGLOBAL]
kernel32.GlobalUnlock.restype = wintypes.BOOL
user32.OpenClipboard.argtypes = [wintypes.HWND]
user32.OpenClipboard.restype = wintypes.BOOL
user32.CloseClipboard.restype = wintypes.BOOL
user32.GetClipboardData.restype = wintypes.HANDLE
user32.GetClipboardData.argtypes = [ctypes.c_uint]
user32.RegisterClipboardFormatW.argtypes = [wintypes.LPCWSTR]
user32.RegisterClipboardFormatW.restype = ctypes.c_uint
# 状态栏上的 SB_GETTEXT* 超过 WM_USER，跨进程不封送指针（见 read_status_bar），
# 所以要自己去目标进程里分配内存、读回来
kernel32.OpenProcess.restype = wintypes.HANDLE
kernel32.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
kernel32.CloseHandle.argtypes = [wintypes.HANDLE]
kernel32.VirtualAllocEx.restype = ctypes.c_void_p
kernel32.VirtualAllocEx.argtypes = [wintypes.HANDLE, ctypes.c_void_p, ctypes.c_size_t,
                                    wintypes.DWORD, wintypes.DWORD]
kernel32.VirtualFreeEx.restype = wintypes.BOOL
kernel32.VirtualFreeEx.argtypes = [wintypes.HANDLE, ctypes.c_void_p, ctypes.c_size_t,
                                   wintypes.DWORD]
kernel32.ReadProcessMemory.restype = wintypes.BOOL
kernel32.ReadProcessMemory.argtypes = [wintypes.HANDLE, ctypes.c_void_p, ctypes.c_void_p,
                                       ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)]
# SendMessageTimeoutW 带指针参数，同样必须声明签名
user32.SendMessageTimeoutW.restype = wintypes.LPARAM
user32.SendMessageTimeoutW.argtypes = [wintypes.HWND, ctypes.c_uint, ctypes.c_size_t,
                                       ctypes.c_ssize_t, ctypes.c_uint, ctypes.c_uint,
                                       ctypes.POINTER(ctypes.c_size_t)]


# 本脚本要跨进程核对窗口坐标（分隔条位置、面板大小），必须和 notepadcc.exe 待在同一个
# 坐标空间里：程序清单声明的是 PerMonitorV2。脚本若保持"DPI 不感知"，在 125%/150%/200%
# 这类缩放比例的机器上，GetWindowRect / GetClientRect 拿回来的是**虚拟化**过的坐标
# （物理值 ÷ 缩放比），而投递过去的鼠标消息里的坐标又会被程序按物理像素读，
# 于是"分隔条拖动"这类按坐标操作的用例会凭空失败——100% 缩放的机器上两套坐标重合，
# 所以这个坑一直没暴露（200% 的机器上实测：客户区 537 而不是 1074，分隔条拖动丢目标）。
def declare_dpi_awareness():
    try:
        # -4 = DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
        if user32.SetProcessDpiAwarenessContext(ctypes.c_void_p(-4)):
            return
    except (AttributeError, OSError):
        pass
    try:
        ctypes.windll.shcore.SetProcessDpiAwareness(2)   # PROCESS_PER_MONITOR_DPI_AWARE
        return
    except (AttributeError, OSError):
        pass
    try:
        user32.SetProcessDPIAware()   # 最后兜底：至少别再看虚拟化坐标
    except (AttributeError, OSError):
        pass


declare_dpi_awareness()


def address_of(buffer):
    return ctypes.cast(buffer, ctypes.c_void_p).value

WM_COMMAND = 0x0111
WM_KEYDOWN = 0x0100
WM_NULL = 0x0000
SMTO_ABORTIFHUNG = 0x0002
WM_SETTEXT = 0x000C
WM_GETTEXT = 0x000D
WM_GETTEXTLENGTH = 0x000E
WM_CLOSE = 0x0010

# 除"单实例"那一段，其余各段一律用 --multi-instance 启动。
# 原因：单实例是产品行为——某个实例还活着时，新启动的会把参数交给它然后自己退出。
# 万一上一段留下一个没退干净的实例，下一段的启动就会被悄悄交接掉，报出来却是
# "找不到窗口"，与本节要测的功能毫无关系（实测踩到过一次）。
MULTI_INSTANCE = "--multi-instance"


def send_bounded(hwnd, message, wparam=0, lparam=0, timeout_ms=2000):
    """带超时的 SendMessage。

    普通 SendMessage 是同步的：发给一个卡住的窗口，脚本会**永久**阻塞在那里
    （实测被后台程序与自己进程里的模态框卡死过十几分钟）。凡是跨进程发的消息都走
    这个函数，超时返回 None，让测试干净地失败而不是挂住。
    """
    if not hwnd:
        return None
    result = ctypes.c_size_t(0)
    ok = user32.SendMessageTimeoutW(hwnd, message, wparam, lparam,
                                    SMTO_ABORTIFHUNG, timeout_ms, ctypes.byref(result))
    return result.value if ok else None

SCI_GETSELECTIONSTART = 2143
SCI_GETSELECTIONEND = 2145
SCI_GETCURRENTPOS = 2008
SCI_LINEFROMPOSITION = 2166
SCI_GOTOLINE = 2024
SCI_MARKERGET = 2046
SCI_SETTEXT = 2181
SCI_GETTEXT = 2182
SCI_GETLENGTH = 2006
SCI_GETCHARAT = 2007
SCI_GETSELECTIONS = 2570
SCI_SELECTIONISRECTANGLE = 2372
SCI_GETMOUSESELECTIONRECTANGULARSWITCH = 2669
SCI_SETSELECTIONMODE = 2422
SCI_SETRECTANGULARSELECTIONANCHOR = 2590
SCI_SETRECTANGULARSELECTIONCARET = 2588
SC_SEL_RECTANGLE = 1
LVM_GETITEMCOUNT = 0x1004

CMD_FIND = (3 << 12) | 1            # CmdCategory::Search, index 1
CMD_FIND_IN_FILES = (3 << 12) | 5   # index 5
CMD_TOGGLE_BOOKMARK = (3 << 12) | 6
CMD_NEXT_BOOKMARK = (3 << 12) | 7
CMD_PREV_BOOKMARK = (3 << 12) | 8
CMD_CLEAR_BOOKMARKS = (3 << 12) | 9
BTN_FIND_NEXT = 908                 # FindBarControl::kFindNextButton
CTRL_FIND_EDIT = 901

PANEL_FIND = 701                    # SearchPanelControl
PANEL_DIR = 702
PANEL_FILTER = 703
PANEL_START = 707
PANEL_LIST = 710
PANEL_REPLACE_EDIT = 712
PANEL_REPLACE_PREVIEW = 713
PANEL_APPLY = 714

CMD_REPLACE_IN_FILES = (3 << 12) | 10
CMD_COLUMN_EDIT = (2 << 12) | 7
CMD_PREFERENCES = (7 << 12) | 1     # CmdCategory::Settings, index 1
CMD_SHORTCUTS = (7 << 12) | 2       # 管理快捷键
CMD_FOLD_TOGGLE = (4 << 12) | 8     # 折叠/展开当前层级
CMD_FOLD_ALL = (4 << 12) | 9
CMD_UNFOLD_ALL = (4 << 12) | 10
CMD_SPLIT_TOGGLE = (4 << 12) | 11   # 分屏查看（克隆当前文档）
CMD_CYCLE_PANE = (4 << 12) | 12
CMD_SCROLL_SYNC = (4 << 12) | 13
CMD_EDIT_UNDO = (2 << 12) | 1
CMD_FILE_SAVE = (1 << 12) | 3
CMD_FORMAT_EOL_CRLF = (5 << 12) | 1
CMD_ENCODING_ANSI = (13 << 12) | 1
CMD_ENCODING_UTF8 = (13 << 12) | 2
CMD_SETTINGS_REGISTER_ASSOC = (7 << 12) | 3
CMD_SETTINGS_UNREGISTER_ASSOC = (7 << 12) | 4
CMD_TOOL_TRIM_TRAILING = (14 << 12) | 15
CMD_TOOL_SORT_ASC = (14 << 12) | 5
CMD_TOOL_REMOVE_DUP_FIRST = (14 << 12) | 12
CMD_TOOL_UPPERCASE = (14 << 12) | 19
CMD_TOOL_BASE64_ENCODE = (14 << 12) | 23
CMD_TOOL_BASE64_DECODE = (14 << 12) | 24
CMD_TOOL_QP_ENCODE = (14 << 12) | 38
CMD_TOOL_QP_DECODE = (14 << 12) | 39
CMD_TOOL_TOGGLE_COMMENT = (14 << 12) | 27
CMD_TOOL_BLOCK_COMMENT = (14 << 12) | 28
CMD_TOOL_EXPORT_LANGS = (14 << 12) | 29
CMD_TOOL_STYLE_CONFIG = (14 << 12) | 30
CMD_EDIT_AUTOCOMPLETE = (2 << 12) | 33
CMD_TOOL_EXPORT_UDL = (14 << 12) | 31
CMD_TOOL_FORMAT_JSON = (14 << 12) | 32
CMD_TOOL_MINIFY_JSON = (14 << 12) | 33
CMD_TOOL_FORMAT_XML = (14 << 12) | 34
CMD_TOOL_VALIDATE_XML = (14 << 12) | 40   # 工具 → 校验 XML
CMD_RUN_DIALOG = (9 << 12) | 1          # 运行…（F5）
CMD_RUN_FIRST_SAVED = (9 << 12) | 128   # 存下来的第一条运行命令
LANGUAGE_BASE = (7 << 12) | 128         # 界面语言：第 0 项是简体中文，第 1 项是内置英文
CMD_TOOL_EXPORT_LANGPACK = (14 << 12) | 35
CMD_VIEW_HEX_TOGGLE = (4 << 12) | 21
CMD_VIEW_HEX_GOTO = (4 << 12) | 22
CMD_EDIT_COPY_AS_RTF = (2 << 12) | 34
CMD_VIEW_PREVIEW_BROWSER = (4 << 12) | 23
CMD_VIEW_MARKDOWN_PREVIEW = (4 << 12) | 24
MD_PREVIEW_CLASS = "notepadccMarkdownPreview"
PREF_DIALOG_CLASS = "notepadccPreferences"
COMPARE_PICK_CLASS = "notepadccComparePick"
COMPARE_PICK_LIST = 1501
COMPARE_PICK_OK = 1510
CMD_TOOL_COMPARE_DOCUMENT = (14 << 12) | 36
CMD_TOOL_COMPARE_DISK = (14 << 12) | 37
CMD_WINDOW_NEXT = (11 << 12) | 1
CMD_WINDOW_PREV = (11 << 12) | 2
CMD_VIEW_JSON_TREE = (4 << 12) | 25
# JSON 树与 XML 树共用同一块面板（M3 第七块把面板并成了"结构树"）
STRUCTURE_TREE_CLASS = "notepadccStructureTree"
# 视图类 26/27/28：全屏（F11）、跳到匹配括号（Ctrl+B）、摘要
CMD_VIEW_FULL_SCREEN = (4 << 12) | 26
CMD_VIEW_GOTO_MATCH_BRACE = (4 << 12) | 27
CMD_VIEW_SUMMARY = (4 << 12) | 28
CMD_VIEW_TOOLBAR = (4 << 12) | 29        # 视图 → 工具栏（勾选）
CMD_VIEW_EYE_CARE = (4 << 12) | 30       # 视图 → 护眼模式（低蓝光，勾选）
CMD_VIEW_XML_TREE = (4 << 12) | 31       # 视图 → XML 树视图（勾选，与 JSON 树共用面板）
CMD_TOOL_XPATH = (14 << 12) | 41         # 工具 → XPath 查询…
XPATH_DIALOG_CLASS = "notepadccXPathQuery"
XPATH_EXPR = 1501                        # 表达式输入框
XPATH_RUN = 1510                         # 查询
XPATH_GOTO = 1511                        # 跳转
XPATH_CLOSE = 1512                       # 关闭
XPATH_RESULTS = 1520                     # 命中的列表
XPATH_STATUS = 1530                      # 状态行（共 N 个匹配 / 错误：…）
LBN_DBLCLK = 2
CMD_SETTINGS_TOOLBAR = (7 << 12) | 6     # 设置 → 自定义工具栏…
MENU_INDEX_VIEW = 3                   # 文件 / 编辑 / 查找 / 视图 …
PREF_UI_LANGUAGE_COMBO = 1048
PREF_CANCEL = 1061
CB_GETCOUNT = 0x0146
CB_SETCURSEL = 0x014E
CF_UNICODETEXT = 13
SCI_SETSEL = 2160
HEX_VIEW_CLASS = "notepadccHexView"
HEX_GOTO_CLASS = "notepadccHexGoto"
RUN_DIALOG_CLASS = "notepadccRunCommand"
RUN_COMBO = 1401
RUN_CAPTURE = 1402
RUN_GO = 1410
RUN_SAVE = 1411
RUN_CANCEL = 1412
VK_F5 = 0x74
CMD_VIEW_DARK_MODE = (4 << 12) | 14
CMD_MACRO_RECORD = (8 << 12) | 1
CMD_MACRO_PLAY = (8 << 12) | 3
CMD_MACRO_RUN_MULTIPLE = (8 << 12) | 6
CMD_MACRO_FIRST_SAVED = (8 << 12) | 128   # 已保存的宏在菜单里的命令号（从 128 起）
CMD_WORKSPACE_CLOSE = (4 << 12) | 16
CMD_WORKSPACE_REFRESH = (4 << 12) | 17
FOLDER_PANEL_CLASS = "notepadccFolderPanel"
FUNCTION_PANEL_CLASS = "notepadccFunctionPanel"
CMD_FUNCTION_JUMP = (4 << 12) | 20
CMD_FILE_PRINT = (1 << 12) | 8
CMD_FILE_EXPORT_HTML = (1 << 12) | 9
CMD_FILE_EXPORT_RTF = (1 << 12) | 10
CMD_SETTINGS_PLUGINS = (7 << 12) | 5
PLUGIN_ADMIN_CLASS = "notepadccPluginAdmin"
PLUGIN_ADMIN_LIST = 1401
PLUGIN_ADMIN_CANCEL = 1411
LB_GETCOUNT_ = 0x018B
TREE_CLASS = "SysTreeView32"
TVM_GETCOUNT = 0x1105

# 宏对话框（自建窗口类，所以不能用 dismiss_dialog——它只认标准对话框 #32770）
MACRO_NAME_CLASS = "notepadccMacroName"
MACRO_RUN_CLASS = "notepadccMacroRun"
MACRO_EDIT = 1301
MACRO_OK = 1310

WM_CHAR_INPUT = 0x0102

COLUMN_TEMPLATE_EDIT = 751
COLUMN_INSERT_BUTTON = 758

PREF_TAB = 1000
PREF_RESTORE_SESSION = 1001
PREF_TAB_WIDTH = 1010
PREF_SHOW_LINE_NUMBERS = 1016
PREF_OK = 1060

SH_FILTER_EDIT = 1101
SH_LIST = 1102
SH_STATUS = 1103
SH_DELETE = 1110
SH_RESTORE = 1111
SH_CLEAR_ALL = 1112
SH_OK = 1120
SH_CANCEL = 1121


def to_colorref(rgb):
    """#RRGGBB → COLORREF（0x00BBGGRR）。

    SCI_STYLEGETFORE 读回来的是**控件真正拿去绘制的** COLORREF，不是配置文件里的
    #RRGGBB——红蓝是反的。所以核对颜色必须转一下：拿 #RRGGBB 直接比会永远差一个红蓝，
    反过来若程序漏了转换，用对称色（如 #FF00FF）比又看不出来。两种都踩过。
    """
    return ((rgb & 0xFF) << 16) | (rgb & 0xFF00) | ((rgb >> 16) & 0xFF)

STYLE_LIST = 1261         # 样式配置器里「具体样式」那个列表
STYLE_ROLE_LABEL_BASE = 1290   # +role：角色名标签（"注释""关键字"…）
LB_GETCOUNT = 0x018B
LB_GETCURSEL = 0x0188
LB_SETCURSEL = 0x0186
LBN_SELCHANGE = 1

VK_F2 = 0x71
VK_F9 = 0x78

SCI_GETMARGINWIDTHN = 2243           # 边距 0 就是行号栏
SCI_GETLINECOUNT = 2154
SCI_VISIBLEFROMDOCLINE = 2220
SCI_GETFOLDLEVEL = 2223
SCI_GETFOLDEXPANDED = 2230
SCI_STYLEGETFORE = 2481
SCI_AUTOCACTIVE = 2102
SCI_GOTOPOS = 2025
SC_FOLDLEVELHEADERFLAG = 0x2000
SCI_GETFIRSTVISIBLELINE = 2152
SCI_GETMODIFY = 2159
SCI_GETCODEPAGE = 2137
SCI_GETSTYLEAT = 2010
SCI_STYLEGETBACK = 2482
STYLE_DEFAULT = 32          # Scintilla 的 STYLE_DEFAULT
STYLE_LINENUMBER = 33       # Scintilla 的 STYLE_LINENUMBER（行号边距）

WM_VSCROLL = 0x0115
SB_PAGEDOWN = 3
WM_LBUTTONDOWN = 0x0201
WM_LBUTTONUP = 0x0202
WM_MOUSEMOVE = 0x0200
TCM_GETITEMCOUNT = 0x1304            # TCM_FIRST + 4
BM_SETCHECK = 0x00F1
BST_UNCHECKED = 0
MF_BYPOSITION = 0x00000400            # 菜单项按位置而不是按命令号来找
MF_BYCOMMAND = 0x00000000
MF_CHECKED = 0x00000008
# ---- 工具栏（自绘控件 notepadccToolBar）----
WM_INITMENUPOPUP = 0x0117
TOOLBAR_CONTROL_CLASS = "notepadccToolBar"
TOOLBAR_DIALOG_CLASS = "notepadccToolbarCustomize"
TB_DLG_AVAILABLE = 1601              # 对话框里"可用命令"那个列表
TB_DLG_CURRENT = 1603                # 对话框里"现在的按钮"那个列表
TB_MSG_ITEM_COUNT = 0x8000 + 60      # ToolBar::kMsgItemCount
TB_MSG_ITEM_CENTER_X = 0x8000 + 61   # ToolBar::kMsgItemCenterX

# ---- 右键菜单定制（M3 第九块）----
CMD_SETTINGS_CONTEXT_MENU = (7 << 12) | 7     # 设置 → 自定义右键菜单…
CONTEXTMENU_DIALOG_CLASS = "notepadccContextMenuCustomize"
POPUP_MENU_CLASS = "#32768"                   # TrackPopupMenu 弹出的菜单窗口类
MN_GETHMENU = 0x01E1                          # 向菜单窗口要 HMENU（菜单是内核对象，跨进程可用）
WM_CONTEXTMENU = 0x007B
VK_DOWN = 0x28
VK_RETURN = 0x0D

# ---- 状态栏（系统控件 msctls_statusbar32）----
STATUS_CLASS = "msctls_statusbar32"
SB_GETTEXTLENGTHW = 0x040C            # WM_USER + 12
SB_GETTEXTW = 0x040D                  # WM_USER + 13
PROCESS_VM_OPERATION = 0x0008
PROCESS_VM_READ = 0x0010
PROCESS_VM_WRITE = 0x0020
MEM_COMMIT = 0x1000
MEM_RESERVE = 0x2000
MEM_RELEASE = 0x8000
PAGE_READWRITE = 0x04

# ---- 全屏（F11）----
WS_CAPTION = 0x00C00000
GWL_STYLE = -16
SPI_GETWORKAREA = 0x0030
VK_F11 = 0x7A
VK_ESCAPE = 0x1B

IDYES = 6
IDNO = 7
IDOK = 1

MARKER_BOOKMARK_MASK = 1            # EditView::kMarkerBookmark == 0


def children(hwnd):
    result = []
    callback_type = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)

    def callback(child, _):
        result.append(child)
        return True

    user32.EnumChildWindows(hwnd, callback_type(callback), 0)
    return result


def class_name(hwnd):
    buffer = ctypes.create_unicode_buffer(256)
    user32.GetClassNameW(hwnd, buffer, 256)
    return buffer.value


def window_text(hwnd):
    length = send_bounded(hwnd, WM_GETTEXTLENGTH)
    if length is None:
        return ""
    if length <= 0:
        return ""
    buffer = ctypes.create_unicode_buffer(length + 1)
    if send_bounded(hwnd, WM_GETTEXT, length + 1, address_of(buffer)) is None:
        return ""
    return buffer.value


def window_text_safe(hwnd):
    """只对**本机任意窗口**都安全的读标题方式。

    WM_GETTEXT 是同步消息：发给别的进程里一个不响应的窗口，脚本会永久阻塞在那里
    （实测被一个后台播放器卡死过十几分钟）。GetWindowTextW 取不到外部进程的文本，
    但它不会阻塞——诊断输出本来就"能拿到多少算多少"。
    """
    length = user32.GetWindowTextLengthW(hwnd)
    if length <= 0:
        return ""
    buffer = ctypes.create_unicode_buffer(length + 1)
    user32.GetWindowTextW(hwnd, buffer, length + 1)
    return buffer.value


def find_child(parent, cls):
    for child in children(parent):
        if class_name(child) == cls:
            return child
    return None


def read_status_bar(window, part=0):
    """读状态栏某一段此刻显示的文本。

    状态栏是系统控件（msctls_statusbar32），读它要用 SB_GETTEXTW——这条消息号超过
    WM_USER，跨进程**不封送指针**：把本进程的缓冲区地址递过去，控件会往它的地址空间里
    写，读回来的是垃圾。所以这里在**目标进程**里分配一块内存，把远端地址交给控件去写，
    再用 ReadProcessMemory 取回来。状态栏上"命令给出的说法"因此也能被核对，
    而不是只能看"命令发出去了没有"。
    """
    bar = find_child(window, STATUS_CLASS)
    if not bar:
        return None
    length = send_bounded(bar, SB_GETTEXTLENGTHW, part, 0)
    if length is None:
        return None
    if length <= 0:
        return ""
    pid = wintypes.DWORD()
    user32.GetWindowThreadProcessId(bar, ctypes.byref(pid))
    process = kernel32.OpenProcess(PROCESS_VM_OPERATION | PROCESS_VM_READ | PROCESS_VM_WRITE,
                                   False, pid)
    if not process:
        return None
    size = (length + 1) * 2
    remote = kernel32.VirtualAllocEx(process, None, size, MEM_COMMIT | MEM_RESERVE,
                                     PAGE_READWRITE)
    try:
        if not remote:
            return None
        if send_bounded(bar, SB_GETTEXTW, part, remote) is None:
            return None
        buffer = ctypes.create_unicode_buffer(length + 1)
        got = ctypes.c_size_t(0)
        if not kernel32.ReadProcessMemory(process, remote, address_of(buffer), size,
                                          ctypes.byref(got)):
            return None
        return buffer.value
    finally:
        if remote:
            kernel32.VirtualFreeEx(process, remote, 0, MEM_RELEASE)
        kernel32.CloseHandle(process)


def wait_status_bar(window, want, timeout=4.0):
    """等状态栏第一段变成想要的文本（状态栏刷新是异步的，直接读会读到上一条）。"""
    deadline = time.time() + timeout
    last = None
    while time.time() < deadline:
        last = read_status_bar(window)
        if last == want:
            return last
        time.sleep(0.2)
    return last


def work_area():
    """当前显示器的工作区（全屏铺的就是它，不是整个显示器）。"""
    rect = wintypes.RECT()
    user32.SystemParametersInfoW(SPI_GETWORKAREA, 0, ctypes.byref(rect), 0)
    return (rect.left, rect.top, rect.right, rect.bottom)


def window_rect(hwnd):
    rect = wintypes.RECT()
    user32.GetWindowRect(hwnd, ctypes.byref(rect))
    return (rect.left, rect.top, rect.right, rect.bottom)


def set_text(hwnd, control_id, text):
    control = user32.GetDlgItem(hwnd, control_id)
    if not control:
        return False
    buffer = ctypes.create_unicode_buffer(text)
    return send_bounded(control, WM_SETTEXT, 0, address_of(buffer)) is not None


def visible_scintilla(main_window):
    for child in children(main_window):
        if class_name(child) == "Scintilla" and user32.IsWindowVisible(child):
            return child
    return None


def main_windows():
    """当前所有可见的主窗口（单实例测试要数窗口个数）。"""
    found = []
    callback_type = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)

    def callback(hwnd, _):
        if user32.IsWindowVisible(hwnd) and class_name(hwnd) == "notepadccMainWindow":
            found.append(hwnd)
        return True

    user32.EnumWindows(callback_type(callback), 0)
    return found


def wait_until_equal(reader, first, second, timeout=4.0):
    """等两个视图的同一个读数收敛到一致。

    通知是异步投递的，而这台机器上还有真实的鼠标在动，读一次就下结论容易冤判。
    同步真的坏了的话，等多久都不会一致。
    """
    deadline = time.time() + timeout
    while time.time() < deadline:
        if reader(first) == reader(second):
            time.sleep(0.2)
            if reader(first) == reader(second):   # 连续两次一致才算数
                return True
        time.sleep(0.2)
    return reader(first) == reader(second)


def visible_scintillas_sorted(main_window):
    """按左边界排序返回可见的编辑控件：分屏时第一个是主视图，第二个是克隆视图。"""
    found = []
    for child in children(main_window):
        if class_name(child) == "Scintilla" and user32.IsWindowVisible(child):
            rect = wintypes.RECT()
            user32.GetWindowRect(child, ctypes.byref(rect))
            found.append((rect.left, child))
    found.sort(key=lambda item: item[0])
    return [child for _, child in found]


def wait_for_main_window(title, timeout=45.0):
    """等主窗口真正就绪，而不是睡固定几秒。

    固定 sleep 是不可靠的：机器一忙（后台在跑播放器、浏览器），程序启动
    就可能超过 4 秒，脚本会在它还没准备好时就开始发消息，于是报出来的是
    后面某条莫名其妙的断言失败。这里要求三件事同时成立：窗口找得到、
    已经显示出来（ShowWindow 过了）、UI 线程能立刻回一条 WM_NULL
    （说明它已经在正常取消息，而不是还卡在启动流程里）。
    """
    deadline = time.time() + timeout
    while time.time() < deadline:
        window = user32.FindWindowW("notepadccMainWindow", title)
        if window and user32.IsWindowVisible(window):
            result = ctypes.c_size_t(0)
            if user32.SendMessageTimeoutW(window, WM_NULL, 0, 0, SMTO_ABORTIFHUNG, 1000,
                                          ctypes.byref(result)):
                return window
        time.sleep(0.25)
    return user32.FindWindowW("notepadccMainWindow", title)


def current_line(scinti):
    position = user32.SendMessageW(scinti, SCI_GETCURRENTPOS, 0, 0)
    return user32.SendMessageW(scinti, SCI_LINEFROMPOSITION, position, 0)


def wait_until_responsive(hwnd, timeout=15.0):
    """等 UI 线程腾出手来，返回从调用起到它回话的秒数（一直不回则返回 None）。

    量的是"按下某个键之后窗口多久还能理我"。发的是 WM_NULL——它什么也不做，
    但必须由 UI 线程亲自取出消息才会回话，所以等待时间就是界面被占住的时长。
    """
    start = time.time()
    while time.time() - start < timeout:
        if send_bounded(hwnd, WM_NULL, timeout_ms=500) is not None:
            return time.time() - start
        time.sleep(0.2)
    return None


def wait_completion_state(scinti, want_active, timeout=3.0):
    """等候选列表出现（want_active=True）或确认它没出现（False）。

    返回 True/False 表示是否等到了；返 None 表示控件不回话。比固定 sleep 稳：
    整段回归里机器一忙，0.8 秒就不一定够（实测偶发失败过一次）。
    """
    deadline = time.time() + timeout
    while time.time() < deadline:
        active = send_bounded(scinti, SCI_AUTOCACTIVE, timeout_ms=500)
        if active is None:
            return None
        if bool(active) == want_active:
            return True
        time.sleep(0.1)
    return False


def trigger_completion(window, scinti, timeout=3.0):
    """触发"自动完成"，并等候选列表真的出现。

    Scintilla 在编辑器**失去焦点**时会关掉候选框，而整段回归里别的窗口偶尔会抢走焦点，
    所以等不到就再按一次（重试的是输入模拟，断言本身不打折：真坏了重按也出不来）。
    """
    for attempt in range(2):
        user32.PostMessageW(window, WM_COMMAND, CMD_EDIT_AUTOCOMPLETE, 0)
        if wait_completion_state(scinti, True, timeout=timeout):
            return True
        if attempt == 0:
            user32.SetForegroundWindow(window)
            time.sleep(0.2)
    return False


def test_bookmarks(main_window):
    """书签：标记两个位置，再验证跳转与清除。"""
    scinti = visible_scintilla(main_window)
    if scinti is None:
        print("  FAIL: 找不到可见的编辑控件")
        return False

    user32.SendMessageW(main_window, WM_COMMAND, CMD_CLEAR_BOOKMARKS, 0)
    time.sleep(0.3)

    # 在第 0 行与第 10 行各打一个书签
    for line in (0, 10):
        user32.SendMessageW(scinti, SCI_GOTOLINE, line, 0)
        time.sleep(0.2)
        user32.SendMessageW(main_window, WM_COMMAND, CMD_TOGGLE_BOOKMARK, 0)
        time.sleep(0.3)
        mask = user32.SendMessageW(scinti, SCI_MARKERGET, line, 0)
        if not (mask & MARKER_BOOKMARK_MASK):
            print(f"  FAIL: 第 {line} 行没有出现书签标记（margin mask = {mask}）")
            return False

    # 回到开头，F2 应跳到第 10 行
    user32.SendMessageW(scinti, SCI_GOTOLINE, 0, 0)
    time.sleep(0.2)
    user32.SendMessageW(main_window, WM_COMMAND, CMD_NEXT_BOOKMARK, 0)
    time.sleep(0.3)
    line = current_line(scinti)
    if line != 10:
        print(f"  FAIL: 下一个书签应跳到第 10 行，实际第 {line} 行")
        return False

    # Shift+F2 应回到第 0 行
    user32.SendMessageW(main_window, WM_COMMAND, CMD_PREV_BOOKMARK, 0)
    time.sleep(0.3)
    line = current_line(scinti)
    if line != 0:
        print(f"  FAIL: 上一个书签应回到第 0 行，实际第 {line} 行")
        return False

    user32.SendMessageW(main_window, WM_COMMAND, CMD_CLEAR_BOOKMARKS, 0)
    time.sleep(0.3)
    mask = user32.SendMessageW(scinti, SCI_MARKERGET, 10, 0)
    if mask & MARKER_BOOKMARK_MASK:
        print("  FAIL: 清除全部书签之后边距上还有标记")
        return False

    print("  书签：标记 / 跳转 / 清除均正确")
    return True


def dump_top_level_windows():
    """诊断用：列出当前可见的顶层窗口。用不阻塞的方式读标题（见 window_text_safe）。"""
    result = []
    callback_type = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)

    def callback(hwnd, _):
        if user32.IsWindowVisible(hwnd):
            result.append((class_name(hwnd), window_text_safe(hwnd)))
        return True

    user32.EnumWindows(callback_type(callback), 0)
    return result


# MessageBox 的按钮 ID 并不总是标准的：本机实测只带"确定"的提示框里，
# 那个"确定"按钮的 ID 是 2 而不是 IDOK(1)，只按 ID 找会找不到。
BUTTON_LABELS = {
    IDOK: ("确定", "OK"),
    IDYES: ("是", "Yes"),
    7: ("否", "No"),          # IDNO
    2: ("取消", "Cancel"),
}


def find_message_button(box, command_id):
    button = user32.GetDlgItem(box, command_id)
    if button:
        return button
    wanted = BUTTON_LABELS.get(command_id)
    if not wanted:
        return None
    for child in children(box):
        if class_name(child) != "Button":
            continue
        text = window_text(child).replace("&", "").strip()
        if text in wanted:
            return child
    return None


def dismiss_dialog(title, command_id, timeout=6.0, quiet=False):
    """确认框是模态的，脚本必须替用户点掉，否则后面的断言都无从谈起。

    三条经验都踩过，所以这里同时用上：
      1. 全部用 PostMessage，**不用同步 SendMessage**——后者碰上模态框会永久阻塞脚本；
      2. 框刚出现时它的默认按钮还没就位，这时候敲回车会被忽略，所以先等它settle，
         而且整段"点不动就再试"要循环到超时为止，不能试一轮就放弃；
      3. 按钮控件 ID 不一定是标准值（只带"确定"的框里实测是 2 而不是 IDOK），
         所以找不到就按按钮文字找。
    """
    def gone(after):
        limit = time.time() + after
        while time.time() < limit:
            if not user32.FindWindowW("#32770", title):
                time.sleep(0.2)
                return True
            time.sleep(0.1)
        return not user32.FindWindowW("#32770", title)

    key = 0x0D if command_id in (IDOK, IDYES) else 0x1B   # VK_RETURN / VK_ESCAPE
    deadline = time.time() + timeout
    while time.time() < deadline:
        dialog = user32.FindWindowW("#32770", title)
        if not dialog:
            time.sleep(0.2)
            continue
        time.sleep(0.4)                     # 等框就位（默认按钮挂上）
        user32.PostMessageW(dialog, 0x0100, key, 0)          # WM_KEYDOWN
        button = find_message_button(dialog, command_id)
        if button:
            user32.PostMessageW(button, 0x00F5, 0, 0)        # BM_CLICK（投递，不阻塞）
            user32.PostMessageW(dialog, WM_COMMAND, user32.GetDlgCtrlID(button), 0)
        else:
            user32.PostMessageW(dialog, WM_COMMAND, command_id, 0)
        if gone(1.0):
            return True
    if not quiet:
        print(f"    [诊断] 没能点掉标题为 {title!r} 的对话框；当前可见顶层窗口：")
        for cls, text in dump_top_level_windows():
            print(f"      {cls!r}  {text!r}")
    return False


def test_replace_in_files(main_window, root):
    """在文件中替换：生成预览 → 应用 → 核对磁盘内容。"""
    work = os.path.join(root, "build", "replace_test")
    if os.path.isdir(work):
        for name in os.listdir(work):
            os.remove(os.path.join(work, name))
    else:
        os.makedirs(work)
    targets = ["one.txt", "two.txt", "skip.log"]
    for name in targets:
        with open(os.path.join(work, name), "w", encoding="utf-8") as handle:
            handle.write("alpha needle beta\nno match\nneedle again\n")

    user32.SendMessageW(main_window, WM_COMMAND, CMD_REPLACE_IN_FILES, 0)
    time.sleep(1)
    panel = find_child(main_window, "notepadccSearchPanel")
    if panel is None or not user32.IsWindowVisible(panel):
        print("  FAIL: 替换面板不可见")
        return False

    set_text(panel, PANEL_FIND, "needle")
    set_text(panel, PANEL_REPLACE_EDIT, "NEEDLE")
    set_text(panel, PANEL_DIR, work)
    set_text(panel, PANEL_FILTER, "*.txt")
    time.sleep(0.3)

    user32.SendMessageW(main_window, WM_COMMAND, PANEL_REPLACE_PREVIEW, 0)
    list_view = user32.GetDlgItem(panel, PANEL_LIST)
    deadline = time.time() + 30
    status = ""
    while time.time() < deadline:
        time.sleep(0.5)
        status = ""
        for child in children(panel):
            if class_name(child) == "Static":
                status = window_text(child)
                break
        if status.startswith("预览就绪") or status.startswith("已取消"):
            break

    if not status.startswith("预览就绪"):
        print(f"  FAIL: 没有生成替换预览，状态 {status!r}")
        return False
    rows = user32.SendMessageW(list_view, LVM_GETITEMCOUNT, 0, 0)
    if rows != 2:
        print(f"  FAIL: 预览应有 2 个文件（skip.log 被过滤器排除），实际 {rows}")
        return False

    # 用 PostMessage 而不是 SendMessage：确认框是模态的，
    # SendMessage 会被它同步阻塞住，脚本就没机会去点它了。
    user32.PostMessageW(main_window, WM_COMMAND, PANEL_APPLY, 0)
    if not dismiss_dialog("在文件中替换", IDYES):
        print("  FAIL: 没能点掉替换确认框")
        return False
    # 被替换的文件没有在标签里打开，所以不会有第二个提示框；找不到属正常
    dismiss_dialog("替换完成", IDOK, timeout=2.0, quiet=True)

    time.sleep(1.0)
    for name in ("one.txt", "two.txt"):
        with open(os.path.join(work, name), "r", encoding="utf-8") as handle:
            content = handle.read()
        if content != "alpha NEEDLE beta\nno match\nNEEDLE again\n":
            print(f"  FAIL: {name} 替换后的内容不对：{content!r}")
            return False
    with open(os.path.join(work, "skip.log"), "r", encoding="utf-8") as handle:
        if "needle" not in handle.read():
            print("  FAIL: 被过滤器排除的文件不应被改动")
            return False

    print("  在文件中替换：预览 2 个文件 → 应用 → 磁盘内容正确")
    return True


def scinti_text(scinti):
    """逐字节读回文本。

    不能用 SCI_GETTEXT：跨进程 SendMessage 只为 WM_USER 以下的系统消息做
    参数封送，SCI_* 都在 WM_USER 以上，指针参数不会被封送——对面进程会按
    自己的地址空间去解引用，读到的是垃圾。SCI_GETCHARAT 只传值，安全。
    """
    length = send_bounded(scinti, SCI_GETLENGTH)
    if length is None:
        return ""
    data = bytearray()
    for index in range(length):
        value = send_bounded(scinti, SCI_GETCHARAT, index)
        if value is None:
            return ""
        data.append(value & 0xFF)
    return data.decode("utf-8", errors="replace")


def test_column_editing(root):
    """列编辑：验证真实应用里矩形选区被正确建立。

    这里另起一个实例，用"命令行打开已知内容的文件"准备文档——
    跨进程无法用 SCI_SETTEXT 设置文本（见 scinti_text 的说明）。

    只验证到"矩形选区建立正确"这一层：在矩形选区上打字会逐行插入是
    Scintilla 的行为，但跨进程合成的 WM_CHAR 复现不了真实键盘输入路径
    （实测只改到主选区那一行），所以不做断言，避免给出虚假的把握。
    """
    work = os.path.join(root, "build", "column_test")
    os.makedirs(work, exist_ok=True)
    path = os.path.join(work, "cols.txt")
    with open(path, "w", encoding="utf-8", newline="") as handle:
        handle.write("aaa\nbbb\nccc")

    session = os.path.join(root, "build", "bin", "config", "session.xml")
    if os.path.exists(session):
        os.remove(session)

    exe = os.path.join(root, "build", "bin", "notepadcc.exe")
    proc = subprocess.Popen([exe, MULTI_INSTANCE, "-portable", path], cwd=root)
    window = wait_for_main_window("notepadcc - cols.txt")
    try:
        if not window:
            print("  FAIL: 找不到打开了 cols.txt 的窗口")
            return False
        scinti = visible_scintilla(window)
        if scinti is None:
            print("  FAIL: 找不到可见的编辑控件")
            return False
        if scinti_text(scinti) != "aaa\nbbb\nccc":
            print(f"  FAIL: 初始内容不对 {scinti_text(scinti)!r}")
            return False
        if not user32.SendMessageW(scinti, SCI_GETMOUSESELECTIONRECTANGULARSWITCH, 0, 0):
            print("  FAIL: 鼠标切矩形选择的开关没打开（ConfigureColumnEditing 没生效）")
            return False

        # 第 0 行第 1 列 → 第 2 行第 3 列，即覆盖三行的同一段两列
        user32.SendMessageW(scinti, SCI_SETSELECTIONMODE, SC_SEL_RECTANGLE, 0)
        user32.SendMessageW(scinti, SCI_SETRECTANGULARSELECTIONANCHOR, 1, 0)
        user32.SendMessageW(scinti, SCI_SETRECTANGULARSELECTIONCARET, 11, 0)
        time.sleep(0.3)

        if not user32.SendMessageW(scinti, SCI_SELECTIONISRECTANGLE, 0, 0):
            print("  FAIL: 选区没有进入矩形模式（ConfigureColumnEditing 可能没生效）")
            print(f"    [诊断] GETSELECTIONMODE={user32.SendMessageW(scinti, 2423, 0, 0)} "
                  f"GETSELECTIONS={user32.SendMessageW(scinti, SCI_GETSELECTIONS, 0, 0)} "
                  f"内容={scinti_text(scinti)!r}")
            log_path = os.path.join(root, "build", "bin", "config", "logs", "notepadcc.log")
            if os.path.exists(log_path):
                with open(log_path, "r", encoding="utf-8", errors="replace") as handle:
                    for line in handle.read().splitlines()[-6:]:
                        print(f"    [诊断] 日志: {line}")
            return False
        selections = user32.SendMessageW(scinti, SCI_GETSELECTIONS, 0, 0)
        if selections != 3:
            print(f"  FAIL: 矩形选区应展开成 3 条，实际 {selections}")
            return False

        # Alt+C 打开列块编辑对话框（它会跑一个局部模态循环，所以用 PostMessage）
        user32.PostMessageW(window, WM_COMMAND, CMD_COLUMN_EDIT, 0)
        time.sleep(1.0)
        dialog = user32.FindWindowW("notepadccColumnDialog", None)
        if not dialog:
            print("  FAIL: 列块编辑对话框没有出现")
            return False

        # 标准控件的 WM_SETTEXT 属于系统消息，跨进程会被封送，可以放心用
        template = ctypes.create_unicode_buffer("item_\\i\\")
        user32.SendMessageW(user32.GetDlgItem(dialog, COLUMN_TEMPLATE_EDIT), WM_SETTEXT, 0,
                            address_of(template))
        time.sleep(0.4)
        user32.PostMessageW(dialog, WM_COMMAND, COLUMN_INSERT_BUTTON, 0)
        time.sleep(1.0)

        actual = scinti_text(scinti)
        expected = "aitem_1aa\nbitem_2bb\ncitem_3cc"
        if actual != expected:
            print(f"  FAIL: 列块插入结果不对\n           实际 {actual!r}\n           期望 {expected!r}")
            return False

        print("  列编辑：矩形选区建立正确（3 条），Alt+C 数字序列插入结果正确")
        return True
    finally:
        if window:
            user32.PostMessageW(window, WM_CLOSE, 0, 0)
            time.sleep(1.5)
        if proc.poll() is None:
            proc.kill()


def remove_log_with_retry(root, timeout=8.0):
    """删掉本次运行的日志文件。

    上一段留下的实例可能还在收尾（日志文件还被它开着），删不掉就等一会儿再试——
    这不是被测对象的问题，是"同一台机器上刚跑完另一个实例"的现实。
    """
    log_path = os.path.join(root, "build", "bin", "config", "logs", "notepadcc.log")
    deadline = time.time() + timeout
    while os.path.exists(log_path) and time.time() < deadline:
        try:
            os.remove(log_path)
        except PermissionError:
            time.sleep(0.3)
    return log_path


def test_large_file(root):
    """大文件分档：真开一个超过 8 MB 的文件，核对应用是否走了大文件路径。

    状态栏是 STATUSCLASSNAMEW 控件，读取文本要用 SB_GETTEXT——它的消息号
    超过 WM_USER，跨进程不会封送指针参数，所以这里改成核对程序自己写的日志。
    """
    work = os.path.join(root, "build", "large_test")
    os.makedirs(work, exist_ok=True)
    path = os.path.join(work, "big.txt")
    with open(path, "w", encoding="utf-8", newline="") as handle:
        for i in range(240000):
            handle.write(f"line {i:06d} abcdefghijklmnopqrstuvwxyz\n")
    size_mb = os.path.getsize(path) / (1024 * 1024)
    if size_mb <= 8:
        print(f"  FAIL: 造的测试文件只有 {size_mb:.1f} MB，够不到大文件阈值")
        return False

    log_path = remove_log_with_retry(root)
    session = os.path.join(root, "build", "bin", "config", "session.xml")
    if os.path.exists(session):
        os.remove(session)

    exe = os.path.join(root, "build", "bin", "notepadcc.exe")
    started = time.time()
    proc = subprocess.Popen([exe, MULTI_INSTANCE, "-portable", path], cwd=root)
    # 大文件装载慢是正常的，这里给足时间等它真正就绪
    window = wait_for_main_window("notepadcc - big.txt", timeout=90.0)
    try:
        if not window:
            print("  FAIL: 找不到打开了 big.txt 的窗口")
            return False
        elapsed = time.time() - started

        log_text = ""
        if os.path.exists(log_path):
            with open(log_path, "r", encoding="utf-8", errors="replace") as handle:
                log_text = handle.read()
        if "大文件模式" not in log_text:
            print(f"  FAIL: 日志里没有大文件模式的记录\n{log_text[-400:]}")
            return False

        scinti = visible_scintilla(window)
        if scinti is None:
            print("  FAIL: 找不到可见的编辑控件")
            return False
        length = user32.SendMessageW(scinti, SCI_GETLENGTH, 0, 0)
        expected = os.path.getsize(path)
        if length != expected:
            print(f"  FAIL: 装载长度不符，编辑器 {length} vs 文件 {expected}")
            return False

        # 顺手在大文件上按一次自动完成：这一下**曾经卡住界面 54 秒**（文档内提词的
        # 去重是线性扫描，而且收完才截断），所以这里既量"多久回话"也核对候选框出来了。
        user32.SendMessageW(scinti, SCI_GOTOPOS, length, 0)
        time.sleep(0.3)
        user32.PostMessageW(window, WM_COMMAND, CMD_EDIT_AUTOCOMPLETE, 0)
        response = wait_until_responsive(window, timeout=15.0)
        if response is None:
            print("  FAIL: 大文件上按自动完成之后窗口一直不回话（界面被卡死）")
            return False
        if response > 5.0:
            print(f"  FAIL: 大文件上自动完成让界面卡了 {response:.1f} 秒（上限 5 秒）")
            return False
        if user32.SendMessageW(scinti, SCI_AUTOCACTIVE, 0, 0) == 0:
            print("  FAIL: 大文件上按自动完成没有候选框")
            return False
        user32.PostMessageW(scinti, WM_KEYDOWN, 0x1B, 0)   # Esc 取消，别把候选框留给后面
        time.sleep(0.3)

        print(f"  大文件分档：{size_mb:.1f} MB 文件以{elapsed:.1f} 秒进入大文件模式，"
              f"装载长度一致，自动完成 {response:.2f} 秒内回话")
        return True
    finally:
        if window:
            user32.PostMessageW(window, WM_CLOSE, 0, 0)
            time.sleep(2)
        if proc.poll() is None:
            proc.kill()


def test_external_change(root):
    """外部改动检测：在程序之外改文件，看它能不能发现并重新加载。"""
    work = os.path.join(root, "build", "extern_test")
    os.makedirs(work, exist_ok=True)
    path = os.path.join(work, "doc.txt")
    with open(path, "w", encoding="utf-8", newline="") as handle:
        handle.write("alpha\n")

    log_path = remove_log_with_retry(root)
    session = os.path.join(root, "build", "bin", "config", "session.xml")
    if os.path.exists(session):
        os.remove(session)

    exe = os.path.join(root, "build", "bin", "notepadcc.exe")
    proc = subprocess.Popen([exe, MULTI_INSTANCE, "-portable", path], cwd=root)
    window = wait_for_main_window("notepadcc - doc.txt")
    try:
        if not window:
            print("  FAIL: 找不到打开了 doc.txt 的窗口")
            return False
        scinti = visible_scintilla(window)
        if scinti is None:
            print("  FAIL: 找不到可见的编辑控件")
            return False
        if scinti_text(scinti) != "alpha\n":
            print(f"  FAIL: 初始内容不对 {scinti_text(scinti)!r}")
            return False

        # 在程序之外直接改文件。这里**故意用等长重写（不截断）**：
        # open(path, "w") 会先把文件截成 0 字节再写入，程序有可能正好在那一瞬间
        # 检测到"文件变小了"并重新加载空文件——测试就会读到空内容而误报失败。
        # 等长重写没有这个中间态，改动的语义也更清楚：外部程序改了内容。
        with open(path, "r+b") as handle:
            handle.seek(0)
            handle.write(b"gamma\n")
            handle.truncate()

        # 等程序自己发现（目录通知），弹确认框
        if not dismiss_dialog("文件已被外部修改", IDYES, timeout=15.0):
            print("  FAIL: 外部改动没有被检测到（没等到确认框）")
            # 这一步失败时最需要的是"程序自己怎么说的"：日志里有监视是否挂上、
            # 有没有发现改动。没有这段，只能猜是产品没检测还是测试写得太早
            if os.path.exists(log_path):
                with open(log_path, "r", encoding="utf-8", errors="replace") as handle:
                    print("    日志尾部：")
                    for line in handle.read().splitlines()[-10:]:
                        print("     ", line)
            else:
                print("    日志文件不存在")
            print(f"    主窗口还在 = {bool(user32.FindWindowW('notepadccMainWindow', None))}")
            return False
        time.sleep(1.0)

        after = scinti_text(scinti)
        if after != "gamma\n":
            still_open = user32.FindWindowW("#32770", "文件已被外部修改")
            print(f"  FAIL: 重新加载后内容应被磁盘版本替换，实际 {after!r}；"
                  f"确认框是否仍在 = {bool(still_open)}")
            if os.path.exists(log_path):
                with open(log_path, "r", encoding="utf-8", errors="replace") as handle:
                    print("    日志尾部：")
                    for line in handle.read().splitlines()[-6:]:
                        print("     ", line)
            return False

        log_text = ""
        if os.path.exists(log_path):
            with open(log_path, "r", encoding="utf-8", errors="replace") as handle:
                log_text = handle.read()
        if "已重新加载" not in log_text:
            print("  FAIL: 日志里没有重新加载的记录")
            return False

        print("  外部改动：检测到磁盘变动并完成重新加载")
        return True
    finally:
        if window:
            user32.PostMessageW(window, WM_CLOSE, 0, 0)
            time.sleep(2)
        if proc.poll() is None:
            proc.kill()


def test_folding(root):
    """代码折叠：折叠边距、层级、命令、以及折叠状态随会话恢复。

    最后一步是这一段真正的价值所在：折起来 → 正常关窗口 → 不带文件参数重启，
    会话把文件与折叠状态一起恢复回来。只测"命令能折起来"是不够的，
    用户第二天打开程序时看到的是恢复出来的状态。
    """
    work = os.path.join(root, "build", "fold_test")
    os.makedirs(work, exist_ok=True)
    path = os.path.join(work, "folds.cpp")
    body = "".join(f"int f{i}() {{\n    return {i};\n}}\n" for i in range(3))
    with open(path, "w", encoding="utf-8", newline="") as handle:
        handle.write(body)

    cfg = os.path.join(root, "build", "bin", "config")
    for name in ("config.xml", "session.xml"):
        target = os.path.join(cfg, name)
        if os.path.exists(target):
            os.remove(target)

    exe = os.path.join(root, "build", "bin", "notepadcc.exe")

    def visible_lines(scinti):
        lines = user32.SendMessageW(scinti, SCI_GETLINECOUNT, 0, 0)
        if lines <= 0:
            return 0
        return user32.SendMessageW(scinti, SCI_VISIBLEFROMDOCLINE, lines - 1, 0) + 1

    def is_header(scinti, line):
        level = user32.SendMessageW(scinti, SCI_GETFOLDLEVEL, line, 0)
        return bool(level & SC_FOLDLEVELHEADERFLAG)

    def is_folded(scinti, line):
        return user32.SendMessageW(scinti, SCI_GETFOLDEXPANDED, line, 0) == 0

    proc = subprocess.Popen([exe, MULTI_INSTANCE, "-portable", path], cwd=root)
    window = wait_for_main_window("notepadcc - folds.cpp")
    try:
        if not window:
            print("  FAIL: 找不到主窗口")
            return False
        scinti = visible_scintilla(window)
        if scinti is None:
            print("  FAIL: 找不到可见的编辑控件")
            return False

        if user32.SendMessageW(scinti, SCI_GETMARGINWIDTHN, 2, 0) <= 0:
            print("  FAIL: C++ 文件应显示折叠边距（2 号边距宽度为 0）")
            return False
        for line in (0, 3, 6):
            if not is_header(scinti, line):
                print(f"  FAIL: 第 {line} 行应当是折叠头（lexer 没算折叠层级）")
                return False

        all_visible = visible_lines(scinti)
        if all_visible < 9:
            print(f"  FAIL: 未折叠时可见行数不对：{all_visible}")
            return False

        user32.PostMessageW(window, WM_COMMAND, CMD_FOLD_ALL, 0)
        time.sleep(0.8)
        if not is_folded(scinti, 0) or not is_folded(scinti, 6):
            print("  FAIL: 折叠全部之后仍有未折的块")
            return False
        folded_visible = visible_lines(scinti)
        if folded_visible >= all_visible:
            print(f"  FAIL: 折叠之后可见行没减少（{all_visible} → {folded_visible}）")
            return False

        user32.PostMessageW(window, WM_COMMAND, CMD_UNFOLD_ALL, 0)
        time.sleep(0.8)
        if visible_lines(scinti) != all_visible:
            print("  FAIL: 展开全部之后可见行没有回到原样")
            return False

        # 光标放进块内部，快捷键应当折起包住它的那一层
        user32.SendMessageW(scinti, SCI_GOTOLINE, 1, 0)
        time.sleep(0.3)
        user32.PostMessageW(window, WM_COMMAND, CMD_FOLD_TOGGLE, 0)
        time.sleep(0.8)
        if not is_folded(scinti, 0):
            print("  FAIL: 在块内部按折叠键，应当折起包住它的那一层")
            return False
        if is_folded(scinti, 3):
            print("  FAIL: 不该连带折起别的块")
            return False

        # 就带着这一处折叠退出，交给下面验证会话恢复
    finally:
        if window:
            user32.PostMessageW(window, WM_CLOSE, 0, 0)
            time.sleep(2)
        if proc.poll() is None:
            proc.kill()

    session = os.path.join(cfg, "session.xml")
    if not os.path.exists(session):
        print("  FAIL: 正常退出没有写出 session.xml")
        return False
    with open(session, "r", encoding="utf-8", errors="replace") as handle:
        session_text = handle.read()
    if "<Folds>" not in session_text:
        print(f"  FAIL: 会话里没有折叠记录\n{session_text[:400]}")
        return False

    # 不带文件参数重启：会话应当把文件与折叠状态一起恢复回来
    proc = subprocess.Popen([exe, MULTI_INSTANCE, "-portable"], cwd=root)
    window = wait_for_main_window("notepadcc - folds.cpp")
    try:
        if not window:
            print("  FAIL: 重启后没有恢复出上次的文件")
            return False
        scinti = visible_scintilla(window)
        if scinti is None:
            print("  FAIL: 恢复后找不到编辑控件")
            return False
        if not is_folded(scinti, 0):
            print("  FAIL: 折叠状态没有被会话恢复（第 0 行应当是折起来的）")
            return False
        if visible_lines(scinti) != all_visible - 2:
            print(f"  FAIL: 恢复后的可见行数不对：{visible_lines(scinti)}，"
                  f"期望 {all_visible - 2}")
            return False
    finally:
        if window:
            user32.PostMessageW(window, WM_CLOSE, 0, 0)
            time.sleep(1.5)
        if proc.poll() is None:
            proc.kill()
        for name in ("session.xml", "config.xml"):
            target = os.path.join(cfg, name)
            if os.path.exists(target):
                os.remove(target)

    print("  代码折叠：边距与层级正确、折叠/展开命令生效、折叠状态随会话恢复")
    return True


def assoc_registered(extension=".txt", prog_id="notepadcc.txt"):
    """查这台机器上 notepadcc 是否已经登记在该扩展名的「打开方式」里。"""
    try:
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER,
                            f"Software\\Classes\\{extension}\\OpenWithProgids") as key:
            winreg.QueryValueEx(key, prog_id)
            return True
    except OSError:
        return False


def appdata_snapshot():
    """记下 %APPDATA%\\notepadcc 的样子，用来核对"绿色版不写 AppData"。"""
    appdata = os.environ.get("APPDATA", "")
    if not appdata:
        return None
    target = os.path.join(appdata, "notepadcc")
    if not os.path.isdir(target):
        return "不存在"
    found = []
    for base, _, names in os.walk(target):
        for name in names:
            found.append(os.path.relpath(os.path.join(base, name), target))
    return sorted(found)


def test_udl(root):
    """UDL 用户自定义语言：自己写一份语言定义，重进程序后它真的接管了着色、注释与折叠。

    UDL 的词法器是本程序自己写的（锁定的 Lexilla 里没有 container 词法器可复用），
    所以这一段是它唯一的端到端证明：定义落到 userlangs.xml → 注册表按扩展名认领 →
    自研词法器把样式号打到 token 上 → 颜色真的与默认色不同；注释命令用的是 UDL 里写的
    注释符（这一段故意用 #，与任何内置语言都不撞）；折叠词真的产生了折叠层级。
    """
    work = os.path.join(root, "build", "udl_test")
    os.makedirs(work, exist_ok=True)
    sample = os.path.join(work, "sample.npccudl")
    with open(sample, "w", encoding="utf-8", newline="") as handle:
        handle.write('let x = 42\n(* 注释 let *)\ns = "文本 let"\nend\n')

    cfg = os.path.join(root, "build", "bin", "config")
    udl = os.path.join(cfg, "userlangs.xml")
    for name in ("config.xml", "session.xml", "langs.xml", "stylers.xml", "userlangs.xml"):
        target = os.path.join(cfg, name)
        if os.path.exists(target):
            os.remove(target)

    exe = os.path.join(root, "build", "bin", "notepadcc.exe")
    # 1) 导出示例：菜单里的那一条要能落下一份能用的模板
    proc = subprocess.Popen([exe, MULTI_INSTANCE, "-portable", sample], cwd=root)
    window = wait_for_main_window("notepadcc - sample.npccudl")
    try:
        if not window:
            print("  FAIL: 找不到主窗口")
            return False
        user32.PostMessageW(window, WM_COMMAND, CMD_TOOL_EXPORT_UDL, 0)
        if not dismiss_dialog("UDL 示例", IDOK, timeout=8.0):
            print("  FAIL: 导出 UDL 示例之后应当给出说明")
            return False
        time.sleep(0.5)
        if not os.path.exists(udl):
            print(f"  FAIL: 没有导出 {udl}")
            return False
        with open(udl, "r", encoding="utf-8", errors="replace") as handle:
            exported = handle.read()
        if "<UserLangs" not in exported or '<UserLang name=' not in exported:
            print(f"  FAIL: 导出的 UDL 示例内容不对\n{exported[:300]}")
            return False
    finally:
        if window:
            user32.PostMessageW(window, WM_CLOSE, 0, 0)
            dismiss_dialog("notepadcc", IDNO, timeout=2.0, quiet=True)
            time.sleep(1.5)
        if proc.poll() is None:
            proc.kill()

    # 1b) 导出的那份示例要能被自己读回来并且真的生效——"写出去的文件自己能解析"
    #     这件事只有真跑一遍才算证明（属性里的引号、转义、中文注释都可能写坏）
    template = os.path.join(work, "template.mydsl")
    with open(template, "w", encoding="utf-8", newline="") as handle:
        handle.write("let x = 42\n")
    proc = subprocess.Popen([exe, MULTI_INSTANCE, "-portable", template], cwd=root)
    window = wait_for_main_window("notepadcc - template.mydsl")
    try:
        if not window:
            print("  FAIL: 找不到打开了示例语言文件的窗口")
            return False
        scinti = visible_scintilla(window)
        if scinti is None:
            print("  FAIL: 找不到可见的编辑控件")
            return False
        if user32.SendMessageW(scinti, SCI_GETSTYLEAT, 0, 0) != 10:
            print("  FAIL: 导出的示例语言没能接管 .mydsl 文件（示例本身读不回来？）")
            return False
        default_fore = user32.SendMessageW(scinti, SCI_STYLEGETFORE, 0, 0)
        if user32.SendMessageW(scinti, SCI_STYLEGETFORE, 10, 0) == default_fore:
            print("  FAIL: 示例语言的关键字没有配色")
            return False
    finally:
        if window:
            user32.PostMessageW(window, WM_CLOSE, 0, 0)
            dismiss_dialog("notepadcc", IDNO, timeout=2.0, quiet=True)
            time.sleep(1.5)
        if proc.poll() is None:
            proc.kill()

    # 2) 换成自己写的语言定义，重进程序：注释符故意用 #，与内置语言都不撞
    definition = (
        '<?xml version="1.0" encoding="utf-8"?>\n'
        '<UserLangs schemaVersion="1">\n'
        '  <UserLang name="npccudl" displayName="测试 DSL" ext="npccudl" caseSensitive="yes">\n'
        '    <LineComment prefix="#"/>\n'
        '    <BlockComment start="(*" end="*)"/>\n'
        '    <Keywords group="0">let fun match with end</Keywords>\n'
        '    <StringDelim start="&quot;" end="&quot;" escape="\\"/>\n'
        '    <Operators>+ - * / = &lt; &gt;</Operators>\n'
        '    <Fold openTokens="let fun match" closeTokens="end"/>\n'
        '  </UserLang>\n'
        '</UserLangs>\n'
    )
    with open(udl, "w", encoding="utf-8", newline="") as handle:
        handle.write(definition)
    for name in ("config.xml", "session.xml"):
        target = os.path.join(cfg, name)
        if os.path.exists(target):
            os.remove(target)

    with open(sample, "w", encoding="utf-8", newline="") as handle:
        handle.write('let x = 42\n(* 注释 let *)\ns = "文本 let"\nend\n')

    proc = subprocess.Popen([exe, MULTI_INSTANCE, "-portable", sample], cwd=root)
    window = wait_for_main_window("notepadcc - sample.npccudl")
    try:
        if not window:
            print("  FAIL: 重新启动后找不到主窗口")
            return False
        scinti = visible_scintilla(window)
        if scinti is None:
            print("  FAIL: 找不到可见的编辑控件")
            return False

        # 2a) 着色：token 的样式号必须是自研词法器定下的那几个，且颜色与默认色不同
        default_fore = user32.SendMessageW(scinti, SCI_STYLEGETFORE, 0, 0)
        text = scinti_text(scinti)

        def byte_offset(needle):
            """把"第几个字符"换成"第几个字节"。

            SCI_GETSTYLEAT 收的是**字节**下标，而 scinti_text 给的是解码后的字符串——
            文件里有中文时两者不相等（注释"注释"两个字占 6 个字节），
            直接拿字符下标去查样式会查到旁边那个 token 上（实测踩过）。
            """
            index = text.find(needle)
            return -1 if index < 0 else len(text[:index].encode("utf-8"))

        checks = [
            ("(*", 2, "块注释"),            # UdlStyles: 2 = 块注释
            ("let", 10, "关键字（第 0 组）"),  # 10 = 关键字组 0
            ("42", 20, "数字"),             # 20 = 数字
            ('"文本', 21, "字符串"),        # 21 = 字符串
            ("=", 22, "运算符"),            # 22 = 运算符
        ]
        for needle, want_style, what in checks:
            pos = byte_offset(needle)
            if pos < 0:
                print(f"  FAIL: 样例文本里找不到 {needle!r}")
                return False
            style = user32.SendMessageW(scinti, SCI_GETSTYLEAT, pos, 0)
            if style != want_style:
                print(f"  FAIL: {what} 的样式号应当是 {want_style}，实际 {style}")
                return False
            colour = user32.SendMessageW(scinti, SCI_STYLEGETFORE, style, 0)
            if colour == default_fore:
                print(f"  FAIL: {what} 的样式号对了但没有配色（还是默认前景色）")
                return False

        # 2b) 注释与字符串里的关键字词不算关键字
        inside = byte_offset("let *")
        if user32.SendMessageW(scinti, SCI_GETSTYLEAT, inside, 0) != 2:
            print("  FAIL: 块注释里的 let 被当成了关键字")
            return False

        # 2c) 注释命令用的是 UDL 里写的注释符（# 而不是内置语言的 //）
        user32.PostMessageW(window, WM_COMMAND, CMD_TOOL_TOGGLE_COMMENT, 0)
        time.sleep(0.6)
        commented = scinti_text(scinti)
        if not commented.startswith("# let x = 42\n"):
            print(f"  FAIL: 行注释该用 UDL 的 #，实际 {commented[:30]!r}")
            return False
        user32.PostMessageW(window, WM_COMMAND, CMD_TOOL_TOGGLE_COMMENT, 0)
        time.sleep(0.6)
        restored = scinti_text(scinti)
        if restored != text:
            print(f"  FAIL: 再按一次注释应当回到原文\n"
                  f"        期望 {text!r}\n        实际 {restored!r}")
            return False

        # 2d) 折叠：折叠边距在、折叠头在、折叠全部真的把行收起来了
        if user32.SendMessageW(scinti, SCI_GETMARGINWIDTHN, 2, 0) <= 0:
            print("  FAIL: UDL 语言没有折叠边距")
            return False
        level0 = user32.SendMessageW(scinti, SCI_GETFOLDLEVEL, 0, 0)
        level1 = user32.SendMessageW(scinti, SCI_GETFOLDLEVEL, 1, 0)
        if not level0 & SC_FOLDLEVELHEADERFLAG:
            print(f"  FAIL: 第 1 行（let）应当是折叠头，层级 0x{level0:04X}")
            return False
        if (level1 & 0xFFF) <= (level0 & 0xFFF):
            print(f"  FAIL: 块内那一行的层级该比折叠头高（0x{level1:04X} vs 0x{level0:04X}）")
            return False
        lines = user32.SendMessageW(scinti, SCI_GETLINECOUNT, 0, 0)
        user32.PostMessageW(window, WM_COMMAND, CMD_FOLD_ALL, 0)
        time.sleep(0.6)
        visible = user32.SendMessageW(scinti, SCI_VISIBLEFROMDOCLINE, lines - 1, 0)
        if visible >= lines:
            print(f"  FAIL: 折叠全部之后屏幕上的行没有变少（{visible} / {lines}）")
            return False
        user32.PostMessageW(window, WM_COMMAND, CMD_UNFOLD_ALL, 0)
        time.sleep(0.6)
        if user32.SendMessageW(scinti, SCI_VISIBLEFROMDOCLINE, lines - 1, 0) != lines - 1:
            print("  FAIL: 展开全部之后应当回到原样")
            return False
    finally:
        if window:
            user32.PostMessageW(window, WM_CLOSE, 0, 0)
            dismiss_dialog("notepadcc", IDNO, timeout=2.0, quiet=True)
            time.sleep(1.5)
        if proc.poll() is None:
            proc.kill()
        for name in ("config.xml", "session.xml", "userlangs.xml"):
            target = os.path.join(cfg, name)
            if os.path.exists(target):
                os.remove(target)

    print("  UDL：导出的示例能被自己读回来并生效；自写的语言定义重进后接管着色"
          "（样式号与配色都核对）、注释符用 # 、折叠词生效")
    return True


def test_dark_mode(root):
    """暗色模式：菜单里切一下，编辑区、行号边距、语法颜色真的变深；重启后还记着。

    这一段的重点是"看得见的东西"：读的是控件里**真正拿去绘制**的前景/背景色
    （COLORREF），不是配置文件里的值。另外还要核对暗色下打开样式配置器点确定不会把
    主题冲掉——配置器改的是"当前主题"，写回文件时两套主题都要在。
    """
    work = os.path.join(root, "build", "dark_ui_test")
    os.makedirs(work, exist_ok=True)
    path = os.path.join(work, "night.cpp")
    with open(path, "w", encoding="utf-8", newline="") as handle:
        handle.write("// comment\nint alpha = 42;\n")

    cfg = os.path.join(root, "build", "bin", "config")
    config = os.path.join(cfg, "config.xml")
    styles = os.path.join(cfg, "stylers.xml")
    for name in ("config.xml", "session.xml", "stylers.xml", "langs.xml", "userlangs.xml"):
        target = os.path.join(cfg, name)
        if os.path.exists(target):
            os.remove(target)

    exe = os.path.join(root, "build", "bin", "notepadcc.exe")
    proc = subprocess.Popen([exe, MULTI_INSTANCE, "-portable", path], cwd=root)
    window = wait_for_main_window("notepadcc - night.cpp")
    try:
        if not window:
            print("  FAIL: 找不到主窗口")
            return False
        scinti = visible_scintilla(window)
        if scinti is None:
            print("  FAIL: 找不到可见的编辑控件")
            return False

        # 1) 默认是浅色：白底 + 深蓝关键字
        if user32.SendMessageW(scinti, SCI_STYLEGETBACK, STYLE_DEFAULT, 0) != to_colorref(0xFFFFFF):
            print("  FAIL: 默认（浅色）背景应当是白的")
            return False
        if user32.SendMessageW(scinti, SCI_STYLEGETFORE, 5, 0) != to_colorref(0x0000C0):
            print("  FAIL: 浅色下关键字应当是深蓝 #0000C0")
            return False

        # 2) 切到暗色
        user32.PostMessageW(window, WM_COMMAND, CMD_VIEW_DARK_MODE, 0)
        time.sleep(0.8)
        if user32.SendMessageW(scinti, SCI_STYLEGETBACK, STYLE_DEFAULT, 0) != to_colorref(0x1E1E1E):
            print(f"  FAIL: 暗色下编辑区背景应当变深，实际 "
                  f"0x{user32.SendMessageW(scinti, SCI_STYLEGETBACK, STYLE_DEFAULT, 0):06X}")
            return False
        if user32.SendMessageW(scinti, SCI_STYLEGETBACK, STYLE_LINENUMBER, 0) != to_colorref(0x2D2D2D):
            print("  FAIL: 暗色下行号边距也该变深（不能留一块白的）")
            return False
        if user32.SendMessageW(scinti, SCI_STYLEGETFORE, 5, 0) != to_colorref(0x569CD6):
            print("  FAIL: 暗色下关键字应当用深色主题的 #569CD6")
            return False
        if not os.path.exists(config):
            print("  FAIL: 切主题之后没有写出 config.xml")
            return False
        with open(config, "r", encoding="utf-8", errors="replace") as handle:
            saved = handle.read()
        if 'theme="dark"' not in saved:
            print(f"  FAIL: config.xml 里没有 theme=\"dark\"\n{saved[:300]}")
            return False

        # 3) 暗色下开一次样式配置器（什么都不改就确定）：主题不能被冲掉
        user32.PostMessageW(window, WM_COMMAND, CMD_TOOL_STYLE_CONFIG, 0)
        time.sleep(1.0)
        dialog = user32.FindWindowW("notepadccStyleConfig", None)
        if not dialog:
            print("  FAIL: 样式配置器没有出现")
            return False
        user32.PostMessageW(dialog, WM_COMMAND, 1271, 0)   # 确定
        deadline = time.time() + 5
        while time.time() < deadline and user32.FindWindowW("notepadccStyleConfig", None):
            time.sleep(0.2)
        time.sleep(0.6)
        if user32.SendMessageW(scinti, SCI_STYLEGETBACK, STYLE_DEFAULT, 0) != to_colorref(0x1E1E1E):
            print("  FAIL: 在暗色下动过样式配置器之后，编辑区不再是暗的")
            return False
        if os.path.exists(styles):
            with open(styles, "r", encoding="utf-8", errors="replace") as handle:
                written = handle.read()
            if 'name="light"' not in written or 'name="dark"' not in written:
                print(f"  FAIL: stylers.xml 里两套主题应当都在\n{written[:400]}")
                return False
    finally:
        if window:
            user32.PostMessageW(window, WM_CLOSE, 0, 0)
            dismiss_dialog("notepadcc", IDNO, timeout=2.0, quiet=True)
            time.sleep(1.5)
        if proc.poll() is None:
            proc.kill()

    # 4) 重启：还是暗的（用哪套主题记在 config.xml 里）
    proc = subprocess.Popen([exe, MULTI_INSTANCE, "-portable", path], cwd=root)
    window = wait_for_main_window("notepadcc - night.cpp")
    try:
        if not window:
            print("  FAIL: 重启后找不到主窗口")
            return False
        scinti = visible_scintilla(window)
        if user32.SendMessageW(scinti, SCI_STYLEGETBACK, STYLE_DEFAULT, 0) != to_colorref(0x1E1E1E):
            print("  FAIL: 重启之后应当还是暗色")
            return False
        if user32.SendMessageW(scinti, SCI_STYLEGETFORE, 5, 0) != to_colorref(0x569CD6):
            print("  FAIL: 重启之后关键字该用深色主题的颜色")
            return False

        # 5) 切回浅色
        user32.PostMessageW(window, WM_COMMAND, CMD_VIEW_DARK_MODE, 0)
        time.sleep(0.8)
        if user32.SendMessageW(scinti, SCI_STYLEGETBACK, STYLE_DEFAULT, 0) != to_colorref(0xFFFFFF):
            print("  FAIL: 再切一次应当回到浅色")
            return False
        with open(config, "r", encoding="utf-8", errors="replace") as handle:
            saved = handle.read()
        if 'theme="light"' not in saved:
            print(f"  FAIL: 切回浅色之后 config.xml 没更新\n{saved[:300]}")
            return False
    finally:
        if window:
            user32.PostMessageW(window, WM_CLOSE, 0, 0)
            dismiss_dialog("notepadcc", IDNO, timeout=2.0, quiet=True)
            time.sleep(1.5)
        if proc.poll() is None:
            proc.kill()
        for name in ("config.xml", "session.xml", "stylers.xml"):
            target = os.path.join(cfg, name)
            if os.path.exists(target):
                os.remove(target)

    print("  暗色模式：编辑区/行号边距/语法颜色都跟着变，重启后记住，再切能回浅色，"
          "配置器不会把主题冲掉")
    return True


def close_macro_dialog(class_name, ok_id=MACRO_OK, timeout=6.0):
    """点掉宏的自建对话框（取名字 / 运行选项）。

    它们不是标准对话框类（#32770），所以 dismiss_dialog 找不到——那三条经验
    （PostMessage、等就位、按 ID 找按钮）在这里照样适用。
    """
    deadline = time.time() + timeout
    while time.time() < deadline:
        dialog = user32.FindWindowW(class_name, None)
        if dialog:
            time.sleep(0.3)                                    # 等默认按钮挂上
            user32.PostMessageW(dialog, WM_COMMAND, ok_id, 0)
            stop = time.time() + 2.0
            while time.time() < stop and user32.FindWindowW(class_name, None):
                time.sleep(0.1)
            if not user32.FindWindowW(class_name, None):
                return True
        time.sleep(0.2)
    return False


def test_macro(root):
    """宏：录一段（打字 + 两个命令）→ 存下来 → 撤回去 → 回放，结果要和录制时一样。

    关键在于核对**结果文本**，而不是"菜单里多了个宏"：宏的价值就是"同样的操作能再来
    一遍"。回放用两条路径各走一次——菜单里的"播放上次的宏"，以及重启后从
    shortcuts.xml 读回来的那条宏命令（后者顺带证明了持久化与菜单注册）。
    最后走一遍"重复执行"，核对次数真的生效。
    """
    work = os.path.join(root, "build", "macro_test")
    os.makedirs(work, exist_ok=True)
    path = os.path.join(work, "m.txt")

    cfg = os.path.join(root, "build", "bin", "config")
    shortcuts = os.path.join(cfg, "shortcuts.xml")
    for name in ("config.xml", "session.xml", "stylers.xml", "langs.xml",
                 "userlangs.xml", "shortcuts.xml"):
        target = os.path.join(cfg, name)
        if os.path.exists(target):
            os.remove(target)

    exe = os.path.join(root, "build", "bin", "notepadcc.exe")
    recorded = ""
    # 文件要**在启动之前**写好：启动时文件还不存在的话，程序开的是一个空文档
    # （窗口标题照样是 m.txt，但编辑器里没有内容可断言）
    with open(path, "w", encoding="utf-8", newline="") as handle:
        handle.write("beta\nalpha\n")
    proc = subprocess.Popen([exe, MULTI_INSTANCE, "-portable", path], cwd=root)
    window = wait_for_main_window("notepadcc - m.txt")
    try:
        scinti = visible_scintilla(window)
        if scinti is None:
            print("  FAIL: 找不到可见的编辑控件")
            return False
        user32.SendMessageW(scinti, SCI_GOTOPOS, len("beta\nalpha\n"), 0)

        # 1) 开始录制，敲两个字（应当合并成一步），再执行两个命令
        user32.PostMessageW(window, WM_COMMAND, CMD_MACRO_RECORD, 0)
        time.sleep(0.6)
        user32.PostMessageW(scinti, WM_CHAR_INPUT, ord("x"), 0)
        time.sleep(0.15)
        user32.PostMessageW(scinti, WM_CHAR_INPUT, ord("y"), 0)
        time.sleep(0.4)
        after_typing = scinti_text(scinti)
        if after_typing != "beta\nalpha\nxy":
            print(f"  FAIL: 录制期间打字之后内容不对 {after_typing!r}")
            return False
        user32.PostMessageW(window, WM_COMMAND, CMD_TOOL_SORT_ASC, 0)
        time.sleep(0.6)
        user32.PostMessageW(window, WM_COMMAND, CMD_TOOL_UPPERCASE, 0)
        time.sleep(0.6)
        recorded = scinti_text(scinti)
        if recorded != "ALPHA\nBETA\nXY":
            print(f"  FAIL: 录制期间两个命令的结果不对 {recorded!r}")
            return False

        # 2) 停止录制 → 取名（用默认名）
        user32.PostMessageW(window, WM_COMMAND, CMD_MACRO_RECORD, 0)
        time.sleep(0.8)
        if not close_macro_dialog(MACRO_NAME_CLASS):
            print("  FAIL: 停止录制之后应该弹出取名对话框")
            return False
        time.sleep(0.6)
        if not os.path.exists(shortcuts):
            print(f"  FAIL: 保存宏之后没有写出 {shortcuts}")
            return False
        with open(shortcuts, "r", encoding="utf-8", errors="replace") as handle:
            saved = handle.read()
        if "<Macros>" not in saved or '<Macro name="Macro 1"' not in saved:
            print(f"  FAIL: shortcuts.xml 里没有这条宏\n{saved[-600:]}")
            return False
        if 'sParam="xy"' not in saved:
            print(f"  FAIL: 连续输入应当合并成一步（sParam=\"xy\"）\n{saved[-600:]}")
            return False
        if saved.count('type="2"') != 1 or saved.count('type="0"') != 2:
            print(f"  FAIL: 步骤数不对（应当是 1 步文本 + 2 步命令）\n{saved[-600:]}")
            return False
        if 'name="cmd.tool.sortAsc"' not in saved or 'name="cmd.tool.upperCase"' not in saved:
            print(f"  FAIL: 命令步骤没写命令键\n{saved[-600:]}")
            return False

        # 3) 撤回去（三次工具操作各一步，打字也是一步），核对回到原文
        for _ in range(4):
            user32.PostMessageW(window, WM_COMMAND, CMD_EDIT_UNDO, 0)
            time.sleep(0.3)
        if scinti_text(scinti) != "beta\nalpha\n":
            print(f"  FAIL: 撤销之后应当是原文，实际 {scinti_text(scinti)!r}")
            return False

        # 4) 回放：结果必须与录制时一模一样
        length = user32.SendMessageW(scinti, SCI_GETLENGTH, 0, 0)
        user32.SendMessageW(scinti, SCI_GOTOPOS, length, 0)
        time.sleep(0.3)
        user32.PostMessageW(window, WM_COMMAND, CMD_MACRO_PLAY, 0)
        time.sleep(1.5)
        replayed = scinti_text(scinti)
        if replayed != recorded:
            print(f"  FAIL: 回放结果与录制时不一致\n        录制时 {recorded!r}\n"
                  f"        回放后 {replayed!r}")
            return False
    finally:
        if window:
            user32.PostMessageW(window, WM_CLOSE, 0, 0)
            dismiss_dialog("notepadcc", IDNO, timeout=2.0, quiet=True)
            time.sleep(1.5)
        if proc.poll() is None:
            proc.kill()

    # 5) 重启：宏应当从 shortcuts.xml 读回来并出现在菜单里（命令号 = 8 类 128 起）
    with open(path, "w", encoding="utf-8", newline="") as handle:
        handle.write("beta\nalpha\n")
    for name in ("session.xml",):
        target = os.path.join(cfg, name)
        if os.path.exists(target):
            os.remove(target)
    proc = subprocess.Popen([exe, MULTI_INSTANCE, "-portable", path], cwd=root)
    window = wait_for_main_window("notepadcc - m.txt")
    try:
        if not window:
            print("  FAIL: 重启后找不到主窗口")
            return False
        scinti = visible_scintilla(window)
        if scinti is None:
            print("  FAIL: 找不到可见的编辑控件")
            return False
        length = user32.SendMessageW(scinti, SCI_GETLENGTH, 0, 0)
        user32.SendMessageW(scinti, SCI_GOTOPOS, length, 0)
        time.sleep(0.3)
        user32.PostMessageW(window, WM_COMMAND, CMD_MACRO_FIRST_SAVED, 0)
        time.sleep(1.5)
        after_restart = scinti_text(scinti)
        if after_restart != recorded:
            print(f"  FAIL: 重启后从菜单执行那条宏，结果应当与录制时一致\n"
                  f"        期望 {recorded!r}\n        实际 {after_restart!r}")
            return False

        # 6) 重复执行 2 次：第一次把文本再"拍"一遍（结果不变），第二次再拍一遍
        user32.PostMessageW(window, WM_COMMAND, CMD_MACRO_RUN_MULTIPLE, 0)
        time.sleep(0.8)
        dialog = user32.FindWindowW(MACRO_RUN_CLASS, None)
        if not dialog:
            print("  FAIL: 重复执行应当先问次数")
            return False
        # 输入框里初值 "1" 是全选的，敲个 2 就替换掉
        user32.PostMessageW(user32.GetDlgItem(dialog, MACRO_EDIT), WM_CHAR_INPUT, ord("2"), 0)
        time.sleep(0.3)
        if not close_macro_dialog(MACRO_RUN_CLASS):
            print("  FAIL: 重复执行的对话框点不掉")
            return False
        time.sleep(2.0)
        repeated = scinti_text(scinti)
        if repeated != "ALPHA\nBETA\nXYXYXY":
            print(f"  FAIL: 重复执行 2 次的结果不对\n        期望 'ALPHA\\nBETA\\nXYXYXY'\n"
                  f"        实际 {repeated!r}")
            return False
    finally:
        if window:
            user32.PostMessageW(window, WM_CLOSE, 0, 0)
            dismiss_dialog("notepadcc", IDNO, timeout=2.0, quiet=True)
            time.sleep(1.5)
        if proc.poll() is None:
            proc.kill()
        for name in ("config.xml", "session.xml", "shortcuts.xml"):
            target = os.path.join(cfg, name)
            if os.path.exists(target):
                os.remove(target)

    print("  宏：录制（输入合并成一步 + 两个命令）→ 存进 shortcuts.xml → 撤销 → 回放结果一致"
          " → 重启后从菜单执行仍一致 → 重复执行 2 次次数正确")
    return True


def test_workspace(root):
    """文件夹工作区：面板列出目录树、键盘选中即打开文件、刷新看到外部新增、随配置记住。

    驱动方式是**键盘**（Down 几次再回车），不是给树控件发"选中某一项"：
    跨进程传不了树节点的句柄（`TVM_GETNEXTITEM(TVGN_CHILD, 句柄)` 从别的进程调用
    永远返回 0，程序自己调用正常）——键盘路径本来也更像用户。
    """
    work = os.path.join(root, "build", "workspace_test")
    ws = os.path.join(work, "proj")
    shutil.rmtree(work, ignore_errors=True)
    os.makedirs(os.path.join(ws, "sub"))
    for name, text in (("a.txt", "file a\n"), ("alpha.md", "# note\n"), ("Zed.txt", "zed\n")):
        with open(os.path.join(ws, name), "w", encoding="utf-8", newline="") as handle:
            handle.write(text)
    with open(os.path.join(ws, "sub", "b.cpp"), "w", encoding="utf-8", newline="") as handle:
        handle.write("int main() { return 0; }\n")
    with open(os.path.join(ws, "sub", "c.txt"), "w", encoding="utf-8", newline="") as handle:
        handle.write("c\n")

    cfg = os.path.join(root, "build", "bin", "config")
    config = os.path.join(cfg, "config.xml")
    for name in ("config.xml", "session.xml", "stylers.xml", "langs.xml", "userlangs.xml",
                 "shortcuts.xml"):
        target = os.path.join(cfg, name)
        if os.path.exists(target):
            os.remove(target)
    # 工作区目录写在配置里：应用启动时自己打开（"选目录"那个对话框没法跨进程驱动）
    with open(config, "w", encoding="utf-8", newline="") as handle:
        handle.write('<?xml version="1.0" encoding="UTF-8"?>\n'
                     '<NotepadccConfig schemaVersion="1">\n'
                     '  <general uiLanguage="zh-CN" restoreSessionOnStart="yes"\n'
                     '           multiInstance="no" recentFileCount="20" theme="light"\n'
                     '           workspaceFolder="' + ws + '"/>\n'
                     '</NotepadccConfig>\n')

    exe = os.path.join(root, "build", "bin", "notepadcc.exe")
    proc = subprocess.Popen([exe, MULTI_INSTANCE, "-portable"], cwd=root)
    window = wait_for_main_window("notepadcc - 新建 1")
    try:
        with open(os.path.join(work, "note.txt"), "w", encoding="utf-8", newline="") as handle:
            handle.write("x\n")   # 造一个待会儿要打开的文件
        if not window:
            print("  FAIL: 找不到主窗口")
            return False
        panel = user32.FindWindowExW(window, None, FOLDER_PANEL_CLASS, None)
        if not panel or not user32.IsWindowVisible(panel):
            print("  FAIL: 配置里写了工作区目录，面板却没有出现")
            return False
        tree = user32.FindWindowExW(panel, None, TREE_CLASS, None)
        if not tree:
            print("  FAIL: 面板里没有树控件")
            return False
        count = user32.SendMessageW(tree, TVM_GETCOUNT, 0, 0)
        if count != 7:
            print(f"  FAIL: 树里应当有 7 个节点（根 + 4 子项 + sub 下 2 项），实际 {count}")
            return False

        # "在文件中查找"的目录默认取工作区根（有工作区时它比"当前文件所在目录"更贴意图）
        user32.PostMessageW(window, WM_COMMAND, CMD_FIND_IN_FILES, 0)
        time.sleep(0.8)
        # 搜索面板的控件挂在面板窗口下，不在主窗口下（GetDlgItem(主窗口) 会拿到 0）
        search_panel = find_child(window, "notepadccSearchPanel")
        directory = window_text(user32.GetDlgItem(search_panel, PANEL_DIR)) if search_panel else ""
        if directory != ws:
            print(f"  FAIL: 在文件中查找的目录应当默认是工作区根，实际 {directory!r}")
            return False
        user32.PostMessageW(window, WM_COMMAND, CMD_FIND_IN_FILES, 0)   # 关掉面板
        time.sleep(0.4)

        # 键盘：根 → sub → a.txt，回车打开
        for _ in range(3):
            user32.PostMessageW(tree, WM_KEYDOWN, 0x28, 0)   # VK_DOWN
            time.sleep(0.3)
        user32.PostMessageW(tree, WM_KEYDOWN, 0x0D, 0)       # VK_RETURN
        time.sleep(1.2)
        if window_text(window) != "notepadcc - a.txt":
            print(f"  FAIL: 在工作区里回车应当打开 a.txt，实际标题 {window_text(window)!r}")
            return False
        scinti = visible_scintilla(window)
        if scinti is None or scinti_text(scinti) != "file a\n":
            print(f"  FAIL: 打开的文件内容不对 {scinti_text(scinti)!r}")
            return False

        # 外部新增一个文件 → 刷新 → 树里多一个节点
        with open(os.path.join(ws, "brand-new.txt"), "w", encoding="utf-8", newline="") as handle:
            handle.write("new\n")
        user32.PostMessageW(window, WM_COMMAND, CMD_WORKSPACE_REFRESH, 0)
        time.sleep(0.8)
        if user32.SendMessageW(tree, TVM_GETCOUNT, 0, 0) != 8:
            print(f"  FAIL: 刷新之后应当多一个节点，实际 "
                  f"{user32.SendMessageW(tree, TVM_GETCOUNT, 0, 0)}")
            return False

        # 关掉工作区：面板收起，配置里也清空（否则下次启动又冒出来）
        user32.PostMessageW(window, WM_COMMAND, CMD_WORKSPACE_CLOSE, 0)
        time.sleep(0.8)
        if user32.IsWindowVisible(panel):
            print("  FAIL: 关闭工作区之后面板应当收起")
            return False
        with open(config, "r", encoding="utf-8", errors="replace") as handle:
            saved = handle.read()
        if 'workspaceFolder=""' not in saved:
            print(f"  FAIL: 关闭工作区之后配置里应当清空\n{saved[:400]}")
            return False
    finally:
        if window:
            user32.PostMessageW(window, WM_CLOSE, 0, 0)
            dismiss_dialog("notepadcc", IDNO, timeout=2.0, quiet=True)
            time.sleep(1.5)
        if proc.poll() is None:
            proc.kill()

    # 重启：工作区已经关掉了，面板不该再出现（配置确实生效了）
    for name in ("session.xml", "config.xml"):
        target = os.path.join(cfg, name)
        if os.path.exists(target):
            os.remove(target)
    with open(config, "w", encoding="utf-8", newline="") as handle:
        handle.write('<?xml version="1.0" encoding="UTF-8"?>\n'
                     '<NotepadccConfig schemaVersion="1">\n'
                     '  <general uiLanguage="zh-CN" restoreSessionOnStart="yes"\n'
                     '           multiInstance="no" recentFileCount="20" theme="light"\n'
                     '           workspaceFolder="' + ws + '"/>\n'
                     '</NotepadccConfig>\n')
    proc = subprocess.Popen([exe, MULTI_INSTANCE, "-portable"], cwd=root)
    window = wait_for_main_window("notepadcc - 新建 1")
    try:
        if not window:
            print("  FAIL: 重启后找不到主窗口")
            return False
        panel = user32.FindWindowExW(window, None, FOLDER_PANEL_CLASS, None)
        tree = user32.FindWindowExW(panel, None, TREE_CLASS, None) if panel else None
        if not panel or not user32.IsWindowVisible(panel) or not tree:
            print("  FAIL: 重启后工作区面板没有恢复")
            return False
        count = user32.SendMessageW(tree, TVM_GETCOUNT, 0, 0)
        if count != 8:
            print(f"  FAIL: 重启后树里应当有 8 个节点（含刚新建的），实际 {count}")
            return False
    finally:
        if window:
            user32.PostMessageW(window, WM_CLOSE, 0, 0)
            dismiss_dialog("notepadcc", IDNO, timeout=2.0, quiet=True)
            time.sleep(1.5)
        if proc.poll() is None:
            proc.kill()
        for name in ("config.xml", "session.xml"):
            target = os.path.join(cfg, name)
            if os.path.exists(target):
                os.remove(target)

    print("  文件夹工作区：配置里的目录启动即展开（7 个节点）、在文件中查找默认用它、"
          "键盘选中回车真的打开了文件、刷新看到外部新增、关闭后配置清空且重启不再出现")
    return True


def test_function_list(root):
    """函数列表：面板列出符号与层级、键盘选中之后跳到那个函数、改代码之后自动重算。

    节流（300 ms）与后台线程都由"等一下再看"来验证：改完代码之后立刻读到的还是旧的，
    稍微等一下才更新——这正是节流该有的行为，所以断言要等。
    """
    work = os.path.join(root, "build", "func_test")
    os.makedirs(work, exist_ok=True)
    source = os.path.join(work, "sample.cpp")
    with open(source, "w", encoding="utf-8", newline="") as handle:
        handle.write("#include <cstdio>\n"
                     "\n"
                     "int helper(int x) {\n"
                     "    return x + 1;\n"
                     "}\n"
                     "\n"
                     "class Widget {\n"
                     "public:\n"
                     "    void draw() {\n"
                     "        printf(\"}\");\n"
                     "    }\n"
                     "};\n"
                     "\n"
                     "int main() {\n"
                     "    return 0;\n"
                     "}\n")

    cfg = os.path.join(root, "build", "bin", "config")
    config = os.path.join(cfg, "config.xml")
    for name in ("config.xml", "session.xml", "stylers.xml", "langs.xml", "userlangs.xml",
                 "shortcuts.xml"):
        target = os.path.join(cfg, name)
        if os.path.exists(target):
            os.remove(target)
    # 面板开关记在配置里：启动即打开（不用去点菜单，跨进程点菜单只能发命令号）
    with open(config, "w", encoding="utf-8", newline="") as handle:
        handle.write('<?xml version="1.0" encoding="UTF-8"?>\n'
                     '<NotepadccConfig schemaVersion="1">\n'
                     '  <general uiLanguage="zh-CN" restoreSessionOnStart="yes"\n'
                     '           multiInstance="no" recentFileCount="20" theme="light"\n'
                     '           functionList="yes"/>\n'
                     '</NotepadccConfig>\n')

    exe = os.path.join(root, "build", "bin", "notepadcc.exe")
    proc = subprocess.Popen([exe, MULTI_INSTANCE, "-portable", source], cwd=root)
    window = wait_for_main_window("notepadcc - sample.cpp")
    try:
        scinti = visible_scintilla(window)
        if scinti is None:
            print("  FAIL: 找不到可见的编辑控件")
            return False
        panel = user32.FindWindowExW(window, None, FUNCTION_PANEL_CLASS, None)
        if not panel or not user32.IsWindowVisible(panel):
            print("  FAIL: 配置里开了函数列表，面板却没有出现")
            return False
        tree = user32.FindWindowExW(panel, None, TREE_CLASS, None)
        if not tree:
            print("  FAIL: 面板里没有树控件")
            return False
        # 解析是后台线程 + 300 ms 节流，所以这里等它出现（不是固定睡一觉就走）
        deadline = time.time() + 8.0
        count = 0
        while time.time() < deadline:
            count = user32.SendMessageW(tree, TVM_GETCOUNT, 0, 0)
            if count >= 4:
                break
            time.sleep(0.2)
        if count != 4:
            print(f"  FAIL: 应当列出 4 个符号（helper/Widget/draw/main），实际 {count}")
            return False

        # 键盘选到第 2 个符号（helper → Widget），执行"跳到选中的符号"
        user32.PostMessageW(tree, WM_KEYDOWN, 0x28, 0)
        time.sleep(0.3)
        user32.PostMessageW(tree, WM_KEYDOWN, 0x28, 0)
        time.sleep(0.3)
        user32.PostMessageW(window, WM_COMMAND, CMD_FUNCTION_JUMP, 0)
        time.sleep(0.8)
        position = user32.SendMessageW(scinti, SCI_GETCURRENTPOS, 0, 0)
        line = user32.SendMessageW(scinti, SCI_LINEFROMPOSITION, position, 0)
        if line != 6:
            print(f"  FAIL: 跳到 Widget 应当落在第 7 行（0 起 6），实际第 {line} 行")
            return False

        # 在第 1 行插一个新函数：节流之后列表应当自己变长（5 个符号）
        user32.SendMessageW(scinti, SCI_GOTOPOS, 0, 0)
        typed = "int fresh() {\n    return 1;\n}\n\n"
        for ch in typed:
            user32.PostMessageW(scinti, WM_CHAR_INPUT, ord(ch), 0)
        time.sleep(2.0)   # 打字之后等节流 + 后台解析
        updated = user32.SendMessageW(tree, TVM_GETCOUNT, 0, 0)
        if updated != 5:
            print(f"  FAIL: 加了一个函数之后应当列出 5 个符号，实际 {updated}")
            return False
    finally:
        if window:
            user32.PostMessageW(window, WM_CLOSE, 0, 0)
            dismiss_dialog("notepadcc", IDNO, timeout=2.0, quiet=True)
            time.sleep(1.5)
        if proc.poll() is None:
            proc.kill()
        for name in ("config.xml", "session.xml"):
            target = os.path.join(cfg, name)
            if os.path.exists(target):
                os.remove(target)

    print("  函数列表：面板列出 4 个符号（含类里的方法）、键盘选中后跳到对应行、"
          "改了代码之后节流到期自动重算（4 → 5）")
    return True


def accept_save_dialog(title, timeout=8.0):
    """点掉系统的"另存为"：默认文件名与目录都由程序给好了（文档旁边、同名换后缀），
    所以这里只需要敲一下回车——比去 DirectUI 里找输入框稳得多（实测过：合成
    WM_COMMAND(IDOK) 对 Vista 风格的保存框无效，框会关但什么都没存）。
    """
    deadline = time.time() + timeout
    while time.time() < deadline:
        dialog = user32.FindWindowW("#32770", title)
        if dialog:
            time.sleep(0.6)   # 等它把默认名与目录填好
            user32.PostMessageW(dialog, WM_KEYDOWN, 0x0D, 0)
            stop = time.time() + 4.0
            while time.time() < stop:
                if not user32.FindWindowW("#32770", title):
                    time.sleep(0.4)
                    return True
                time.sleep(0.1)
            return False
        time.sleep(0.2)
    return False


def test_export_and_print(root):
    """导出 HTML / RTF 与打印：导出要核对**文件内容**（颜色类、转义、颜色表），
    打印只验证到"对话框能弹出并取消"——测试**绝不往真打印机上送纸**。
    """
    work = os.path.join(root, "build", "export_test")
    shutil.rmtree(work, ignore_errors=True)
    os.makedirs(work)
    source = os.path.join(work, "sample.cpp")
    with open(source, "w", encoding="utf-8", newline="") as handle:
        handle.write("// 注释\nint main() {\n    return 42;\n}\n")
    html = os.path.join(work, "sample.html")
    rtf = os.path.join(work, "sample.rtf")

    cfg = os.path.join(root, "build", "bin", "config")
    for name in ("config.xml", "session.xml", "stylers.xml", "langs.xml", "userlangs.xml",
                 "shortcuts.xml"):
        target = os.path.join(cfg, name)
        if os.path.exists(target):
            os.remove(target)

    exe = os.path.join(root, "build", "bin", "notepadcc.exe")
    proc = subprocess.Popen([exe, MULTI_INSTANCE, "-portable", source], cwd=root)
    window = wait_for_main_window("notepadcc - sample.cpp")
    try:
        if not window:
            print("  FAIL: 找不到主窗口")
            return False
        scinti = visible_scintilla(window)
        if scinti is None:
            print("  FAIL: 找不到可见的编辑控件")
            return False

        # 1) 导出 HTML：对话框出来直接回车（默认就是文档旁边的 sample.html）
        user32.PostMessageW(window, WM_COMMAND, CMD_FILE_EXPORT_HTML, 0)
        if not accept_save_dialog("导出为 HTML"):
            print("  FAIL: 导出 HTML 的保存对话框没出来（或回车没能关掉它）")
            return False
        if not os.path.exists(html):
            print(f"  FAIL: 没有导出 {html}")
            return False
        with open(html, "r", encoding="utf-8", errors="replace") as handle:
            text = handle.read()
        if "span.s2 { color: #2E7D32; font-style: italic; }" not in text:
            print(f"  FAIL: HTML 里没有注释样式的 CSS\n{text[:400]}")
            return False
        if "span.s5 { color: #0000C0; font-weight: bold; }" not in text:
            print(f"  FAIL: HTML 里没有关键字样式的 CSS\n{text[:400]}")
            return False
        if "<td class=\"ln\">1</td>" not in text:
            print("  FAIL: HTML 里没有行号列")
            return False
        if "sample.cpp" not in text:
            print("  FAIL: HTML 的标题没写文档名")
            return False

        # 2) 导出 RTF：颜色表与 \cf 引用
        user32.PostMessageW(window, WM_COMMAND, CMD_FILE_EXPORT_RTF, 0)
        if not accept_save_dialog("导出为 RTF"):
            print("  FAIL: 导出 RTF 的保存对话框没出来")
            return False
        if not os.path.exists(rtf):
            print(f"  FAIL: 没有导出 {rtf}")
            return False
        with open(rtf, "r", encoding="utf-8", errors="replace") as handle:
            rtfText = handle.read()
        if not rtfText.startswith("{\\rtf1"):
            print(f"  FAIL: RTF 开头不对\n{rtfText[:200]}")
            return False
        if "\\red46\\green125\\blue50;" not in rtfText and "\\cf" not in rtfText:
            print(f"  FAIL: RTF 里没有颜色表/颜色引用\n{rtfText[:300]}")
            return False

        # 3) 打印：只验证"对话框确实弹出来了"——**绝不出纸**。
        # Win10 之后的打印对话框是独立进程的现代 UI（类名 ApplicationFrameWindow、
        # 标题带程序名），投递 WM_CLOSE/WM_ESC 都关不掉它，所以这里**只断言它出现**，
        # 随后由 finally 直接结束进程（对话框随之消失）。真正的排版那一步在单元测试里
        # 用内存 DC 跑过一遍（SCI_FORMATRANGE 真画了一页），所以"能不能排"不靠这一条验证
        user32.PostMessageW(window, WM_COMMAND, CMD_FILE_PRINT, 0)
        deadline = time.time() + 8.0
        found_print = False
        while time.time() < deadline:
            for cls, text in dump_top_level_windows():
                if "打印" in text and "notepadcc" in text:
                    found_print = True
                    break
            if found_print:
                break
            time.sleep(0.2)
        if not found_print:
            print("  FAIL: 打印对话框没有出现")
            return False
    finally:
        if window:
            # 打印对话框关不掉，所以这里不强求"优雅关闭"，直接结束进程
            user32.PostMessageW(window, WM_CLOSE, 0, 0)
            time.sleep(1.0)
        if proc.poll() is None:
            proc.kill()
        proc.wait(timeout=10)
        for name in ("config.xml", "session.xml"):
            target = os.path.join(cfg, name)
            if os.path.exists(target):
                os.remove(target)

    print("  导出与打印：HTML 落盘且样式类/行号/标题都对、RTF 有颜色表、"
          "打印对话框确实弹出（测试不往打印机送纸；排版那一步在单测里用内存 DC 真跑过）")
    return True


def test_plugins(root):
    """插件宿主：真加载一个 DLL 插件、它的命令能执行、坏插件被拒载、管理器列得出来。

    示例插件由 CMake 一起构建（build/plugins/hello），测试只把它拷进配置目录——
    这样"发现 → 校验 → Init → 注册命令 → 执行"整条链都是真的。
    触发插件命令用的是**快捷键**（shortcuts.xml 里把 plugin.hello.insert 绑到 F9）：
    自动分配的命令号在测试里不可预测，而按 F9 走的正是用户日常那条路。
    """
    import shutil as _shutil

    work = os.path.join(root, "build", "plugin_test")
    os.makedirs(work, exist_ok=True)
    source = os.path.join(work, "sample.cpp")
    with open(source, "w", encoding="utf-8", newline="") as handle:
        handle.write("int main() {\n    return 0;\n}\n")

    cfg = os.path.join(root, "build", "bin", "config")
    plugins = os.path.join(cfg, "plugins")
    _shutil.rmtree(plugins, ignore_errors=True)
    os.makedirs(plugins, exist_ok=True)
    built = os.path.join(root, "build", "plugins")
    for name in ("hello", "badabi"):
        if not os.path.isdir(os.path.join(built, name)):
            print(f"  FAIL: 找不到构建出来的示例插件 {name}（先构建 hello_plugin 目标）")
            return False
        _shutil.copytree(os.path.join(built, name), os.path.join(plugins, name))
    for name in ("config.xml", "session.xml", "shortcuts.xml", "plugins.xml"):
        target = os.path.join(cfg, name)
        if os.path.exists(target):
            os.remove(target)
    # 把插件命令绑到 F9（不带修饰键也能触发，脚本没法伪造 Ctrl/Alt）
    with open(os.path.join(cfg, "shortcuts.xml"), "w", encoding="utf-8", newline="") as handle:
        handle.write('<?xml version="1.0" encoding="UTF-8"?>\n'
                     '<NotepadPlusCompat schemaVersion="1">\n'
                     '  <InternalCommands>\n'
                     '    <Shortcut name="plugin.hello.hello.insert" id="0" Ctrl="no" Alt="no"'
                     ' Shift="no" Key="120"/>\n'
                     '  </InternalCommands>\n'
                     '</NotepadPlusCompat>\n')

    log = os.path.join(cfg, "logs", "notepadcc.log")
    if os.path.exists(log):
        os.remove(log)

    exe = os.path.join(root, "build", "bin", "notepadcc.exe")
    proc = subprocess.Popen([exe, MULTI_INSTANCE, "-portable", source], cwd=root)
    window = wait_for_main_window("notepadcc - sample.cpp")
    try:
        if not window:
            print("  FAIL: 找不到主窗口（插件加载把程序带崩了？）")
            return False
        scinti = visible_scintilla(window)
        if scinti is None:
            print("  FAIL: 找不到可见的编辑控件")
            return False
        # 1) 日志：hello 加载成功、badabi 因 ABI 被拒
        text = ""
        deadline = time.time() + 5.0
        while time.time() < deadline:
            if os.path.exists(log):
                with open(log, "r", encoding="utf-8", errors="replace") as handle:
                    text = handle.read()
            if "已加载插件：hello" in text:
                break
            time.sleep(0.2)
        if "已加载插件：hello" not in text:
            print(f"  FAIL: 插件没有被加载\n{text[-400:]}")
            return False
        if "ABI 版本不匹配" not in text:
            print(f"  FAIL: 坏插件应当被拒载并说明原因\n{text[-400:]}")
            return False

        # 2) 按 F9 触发插件命令：插件应当把它的文本插到光标处
        user32.SendMessageW(scinti, SCI_GOTOPOS, 0, 0)
        time.sleep(0.3)
        user32.PostMessageW(window, WM_KEYDOWN, VK_F9, 0)
        time.sleep(1.0)
        after = scinti_text(scinti)
        if not after.startswith("// 来自 hello 插件\n"):
            print(f"  FAIL: 插件命令没有插进文本，实际 {after[:40]!r}")
            return False

        # 3) 插件管理器：列得出两个插件（一个已加载、一个被拒）
        user32.PostMessageW(window, WM_COMMAND, CMD_SETTINGS_PLUGINS, 0)
        time.sleep(1.0)
        dialog = user32.FindWindowW(PLUGIN_ADMIN_CLASS, None)
        if not dialog:
            print("  FAIL: 插件管理器没有出现")
            return False
        count = user32.SendMessageW(user32.GetDlgItem(dialog, PLUGIN_ADMIN_LIST), LB_GETCOUNT_, 0, 0)
        if count != 2:
            print(f"  FAIL: 插件管理器应当列出 2 个插件，实际 {count}")
            return False
        user32.PostMessageW(dialog, WM_COMMAND, PLUGIN_ADMIN_CANCEL, 0)
        deadline = time.time() + 5.0
        while time.time() < deadline and user32.FindWindowW(PLUGIN_ADMIN_CLASS, None):
            time.sleep(0.1)
        if user32.FindWindowW(PLUGIN_ADMIN_CLASS, None):
            print("  FAIL: 插件管理器没有关掉")
            return False
    finally:
        if window:
            user32.PostMessageW(window, WM_CLOSE, 0, 0)
            dismiss_dialog("notepadcc", IDNO, timeout=2.0, quiet=True)
            time.sleep(1.5)
        if proc.poll() is None:
            proc.kill()
        for name in ("config.xml", "session.xml", "shortcuts.xml", "plugins.xml"):
            target = os.path.join(cfg, name)
            if os.path.exists(target):
                os.remove(target)
        _shutil.rmtree(plugins, ignore_errors=True)

    print("  插件宿主：示例 DLL 真加载（日志可查）、坏 ABI 被拒载、F9 触发插件命令真的改了文档、"
          "插件管理器列得出两个插件")
    return True


def test_auto_complete(root):
    """自动完成：文档内提词与语言词库两个来源都要真的能补出词来。

    验证方式是"按下补全键 → 回车"之后**读文档内容**：补全如果只是弹了个框，
    内容不会变；只有候选与插入真的接上了，才会看到词被替换进去。
    """
    work = os.path.join(root, "build", "complete_test")
    os.makedirs(work, exist_ok=True)
    path = os.path.join(work, "sample.cpp")
    # 开头放 "de"（词库里没有 de 开头的词，只有 API 词库里的 delta 能补上），
    # 结尾放 "be"（文档里有 beta）
    with open(path, "w", encoding="utf-8", newline="") as handle:
        handle.write("de\nalpha\nbeta\ngamma\nbe")

    cfg = os.path.join(root, "build", "bin", "config")
    apiDir = os.path.join(cfg, "api")
    os.makedirs(apiDir, exist_ok=True)
    for name in ("config.xml", "session.xml"):
        target = os.path.join(cfg, name)
        if os.path.exists(target):
            os.remove(target)
    apiFile = os.path.join(apiDir, "cpp.api")
    with open(apiFile, "w", encoding="utf-8", newline="") as handle:
        handle.write("delta\n# 这行是注释\n")

    exe = os.path.join(root, "build", "bin", "notepadcc.exe")
    proc = subprocess.Popen([exe, MULTI_INSTANCE, "-portable", path], cwd=root)
    window = wait_for_main_window("notepadcc - sample.cpp")
    try:
        if not window:
            print("  FAIL: 找不到主窗口")
            return False
        scinti = visible_scintilla(window)
        if scinti is None:
            print("  FAIL: 找不到可见的编辑控件")
            return False

        # 1) 语言词库：光标放在开头的 "de" 之后，候选里只可能是词库里的 delta
        user32.SendMessageW(scinti, SCI_GOTOPOS, 2, 0)
        time.sleep(0.3)
        if not trigger_completion(window, scinti):
            print("  FAIL: 按下自动完成之后候选列表没出现（试了两次）")
            return False
        user32.PostMessageW(scinti, WM_KEYDOWN, 0x0D, 0)   # 回车选中第一个候选
        time.sleep(0.8)
        # 断言整篇内容而不是"开头是不是 delta"：前缀为空的补全会把词插到光标处，
        # 只查开头会漏掉"插错位置"这类错误。
        text = scinti_text(scinti)
        expected = "delta\nalpha\nbeta\ngamma\nbe"
        if text != expected:
            print(f"  FAIL: 词库补全该把 de 补成 delta，期望 {expected!r}，实际 {text!r}")
            return False
        if user32.SendMessageW(scinti, SCI_GETCURRENTPOS, 0, 0) != 5:
            print("  FAIL: 补全之后光标该停在插入的词之后")
            return False

        # 2) 文档内提词：光标移到末尾（"be" 之后），候选是文档里的 beta
        length = user32.SendMessageW(scinti, SCI_GETLENGTH, 0, 0)
        if length != len(expected.encode("utf-8")):
            print(f"  FAIL: 文档长度读取异常，实际 {length}")
            return False
        user32.SendMessageW(scinti, SCI_GOTOPOS, length, 0)
        time.sleep(0.3)
        if not trigger_completion(window, scinti):
            print("  FAIL: 文档内提词时候选列表没出现（试了两次）")
            return False
        user32.PostMessageW(scinti, WM_KEYDOWN, 0x0D, 0)
        time.sleep(0.8)
        text = scinti_text(scinti)
        expected = "delta\nalpha\nbeta\ngamma\nbeta"
        if text != expected:
            print(f"  FAIL: 文档提词该把 be 补成 beta，期望 {expected!r}，实际 {text!r}")
            return False
    finally:
        if window:
            user32.PostMessageW(window, WM_CLOSE, 0, 0)
            dismiss_dialog("notepadcc", IDNO, timeout=2.0, quiet=True)
            time.sleep(1.5)
        if proc.poll() is None:
            proc.kill()

    # 3) 没有可补的词时不该弹框（短词被过滤掉，也没有对应词库）
    empty = os.path.join(work, "empty.txt")
    with open(empty, "w", encoding="utf-8", newline="") as handle:
        handle.write("qq")
    proc = subprocess.Popen([exe, MULTI_INSTANCE, "-portable", empty], cwd=root)
    window = wait_for_main_window("notepadcc - empty.txt")
    try:
        if not window:
            print("  FAIL: 找不到第三个实例的主窗口")
            return False
        scinti = visible_scintilla(window)
        user32.SendMessageW(scinti, SCI_GOTOPOS, 2, 0)
        user32.PostMessageW(window, WM_COMMAND, CMD_EDIT_AUTOCOMPLETE, 0)
        # 这一条要确认"真的没弹"：等满两秒，中途一旦出现就是失败
        if wait_completion_state(scinti, True, timeout=2.0):
            print("  FAIL: 没有任何候选时不该弹出候选列表")
            return False
        if scinti_text(scinti) != "qq":
            print("  FAIL: 没有候选时内容不该被改动")
            return False
    finally:
        if window:
            user32.PostMessageW(window, WM_CLOSE, 0, 0)
            dismiss_dialog("notepadcc", IDNO, timeout=2.0, quiet=True)
            time.sleep(1.5)
        if proc.poll() is None:
            proc.kill()
        if os.path.exists(apiFile):
            os.remove(apiFile)
        for name in ("config.xml", "session.xml"):
            target = os.path.join(cfg, name)
            if os.path.exists(target):
                os.remove(target)

    print("  自动完成：文档内提词与语言词库都能补出词（回车插入后核对内容），"
          "没有候选时不弹框")
    return True


def test_style_scheme(root):
    """样式方案：配色能按语言调、能落盘、改完重启真的改变渲染。

    最后一步是这一段的关键：手写一份 stylers.xml 把 cpp 的注释色调成 #FF00FF，
    重启后直接读 Scintilla 里注释样式的实际前景色来核对。只测"文件写出来了"是不够的，
    配色的意义在于屏幕上看得见。
    """
    work = os.path.join(root, "build", "styles_test")
    os.makedirs(work, exist_ok=True)
    path = os.path.join(work, "sample.cpp")
    with open(path, "w", encoding="utf-8", newline="") as handle:
        handle.write("// 注释\nint a = 1;\n")

    cfg = os.path.join(root, "build", "bin", "config")
    styles = os.path.join(cfg, "stylers.xml")
    langs = os.path.join(cfg, "langs.xml")
    for name in ("config.xml", "session.xml", "stylers.xml", "langs.xml"):
        target = os.path.join(cfg, name)
        if os.path.exists(target):
            os.remove(target)

    exe = os.path.join(root, "build", "bin", "notepadcc.exe")
    proc = subprocess.Popen([exe, MULTI_INSTANCE, "-portable", path], cwd=root)
    window = wait_for_main_window("notepadcc - sample.cpp")
    try:
        if not window:
            print("  FAIL: 找不到主窗口")
            return False
        scinti = visible_scintilla(window)
        if scinti is None:
            print("  FAIL: 找不到可见的编辑控件")
            return False
        # cpp 的行注释是样式 2（Lexilla 的 SCE_C_COMMENTLINE）
        if user32.SendMessageW(scinti, SCI_STYLEGETFORE, 2, 0) != to_colorref(0x2E7D32):
            print(f"  FAIL: 默认注释色应当是内置的绿 #2E7D32，实际 "
                  f"0x{user32.SendMessageW(scinti, SCI_STYLEGETFORE, 2, 0):06X}")
            return False

        # 打开样式配置器，直接点确定（不改颜色）：核对对话框能起来、确定能落盘
        user32.PostMessageW(window, WM_COMMAND, CMD_TOOL_STYLE_CONFIG, 0)
        time.sleep(1.0)
        dialog = user32.FindWindowW("notepadccStyleConfig", None)
        if not dialog:
            print("  FAIL: 样式配置器没有出现")
            return False
        if window_text(dialog) != "样式配置器":
            print(f"  FAIL: 对话框标题不对 {window_text(dialog)!r}")
            return False
        # "具体样式"那一列要真的列出这个语言的样式号。列表里有几项能读到数值
        # （LB_GETCOUNT 只回一个数，不用指针，跨进程是安全的），内容读不回来
        # 就由单元测试去核对名字与样式号。
        styleList = user32.GetDlgItem(dialog, STYLE_LIST)
        if not styleList:
            print("  FAIL: 样式配置器里没有「具体样式」列表")
            return False
        count = user32.SendMessageW(styleList, LB_GETCOUNT, 0, 0)
        if count != 7:
            print(f"  FAIL: cpp 的具体样式该列 7 项，实际 {count}")
            return False
        # 角色名标签必须真的摆过位置。这一条是被截图抓到的：标签当初用 id=-1 建，
        # 而 Layout() 按 id 找控件摆放，找不到就留在默认的 7x7 上——名字全叠在角落，
        # 界面上只剩六个色块，而"对话框能打开、确定能落盘"这类断言全是绿的。
        for role in range(6):
            label = user32.GetDlgItem(dialog, STYLE_ROLE_LABEL_BASE + role)
            if not label:
                print(f"  FAIL: 第 {role + 1} 个角色名标签不存在")
                return False
            rect = wintypes.RECT()
            user32.GetWindowRect(label, ctypes.byref(rect))
            if (rect.right - rect.left) < 20 or (rect.bottom - rect.top) < 10:
                print(f"  FAIL: 第 {role + 1} 个角色名标签没被摆到位置上"
                      f"（{rect.right - rect.left}x{rect.bottom - rect.top}）")
                return False
        # 选中一项再切回来：走的是 LBN_SELCHANGE 那条路（界面上的"选中即刷新"）
        user32.PostMessageW(dialog, WM_COMMAND, STYLE_LIST | (LBN_SELCHANGE << 16), 0)
        time.sleep(0.5)
        if user32.SendMessageW(styleList, LB_GETCURSEL, 0, 0) != 0:
            print("  FAIL: 「具体样式」列表的选中项被弄乱了")
            return False
        user32.PostMessageW(dialog, WM_COMMAND, 1271, 0)   # 确定
        deadline = time.time() + 5
        while time.time() < deadline and user32.FindWindowW("notepadccStyleConfig", None):
            time.sleep(0.2)
        if user32.FindWindowW("notepadccStyleConfig", None):
            print("  FAIL: 点确定之后样式配置器没有关闭")
            return False
        time.sleep(0.5)
        if not os.path.exists(styles):
            print(f"  FAIL: 确定之后没有写出 {styles}")
            return False
        with open(styles, "r", encoding="utf-8", errors="replace") as handle:
            saved = handle.read()
        if '<Style role="comment"' not in saved:
            print(f"  FAIL: stylers.xml 里没有注释角色的条目\n{saved[:300]}")
            return False
    finally:
        if window:
            user32.PostMessageW(window, WM_CLOSE, 0, 0)
            dismiss_dialog("notepadcc", IDNO, timeout=2.0, quiet=True)
            time.sleep(1.5)
        if proc.poll() is None:
            proc.kill()

    # 手写一份配色 → 重启 → 直接读渲染用的样式色。
    # 这里同时考两件事：角色色（注释）与**逐样式号**覆盖（样式 7 是 cpp 的字符字面量，
    # 它跟角色色冲突时应当赢——不然"逐项调整"就只是界面上好看）
    custom = (
        '<?xml version="1.0" encoding="UTF-8"?>\n'
        '<NotepadccStyles schemaVersion="1">\n'
        '  <Language name="cpp">\n'
        '    <Style role="comment" fore="#3366FF" bold="yes" italic="no"/>\n'
        '    <StyleNumber number="7" name="次要项" fore="#CC5500" bold="no" italic="no"/>\n'
        '  </Language>\n'
        '</NotepadccStyles>\n'
    )
    with open(styles, "w", encoding="utf-8", newline="") as handle:
        handle.write(custom)

    proc = subprocess.Popen([exe, MULTI_INSTANCE, "-portable", path], cwd=root)
    window = wait_for_main_window("notepadcc - sample.cpp")
    try:
        if not window:
            print("  FAIL: 改完配色之后重启找不到窗口")
            return False
        scinti = visible_scintilla(window)
        actual = user32.SendMessageW(scinti, SCI_STYLEGETFORE, 2, 0)
        if actual != to_colorref(0x3366FF):
            print(f"  FAIL: 自定义配色应当生效（注释色 #3366FF），实际 0x{actual:06X}")
            return False
        perStyle = user32.SendMessageW(scinti, SCI_STYLEGETFORE, 7, 0)
        if perStyle != to_colorref(0xCC5500):
            print(f"  FAIL: 逐样式号覆盖应当生效（样式 7 = #CC5500），实际 0x{perStyle:06X}")
            return False
    finally:
        if window:
            user32.PostMessageW(window, WM_CLOSE, 0, 0)
            dismiss_dialog("notepadcc", IDNO, timeout=2.0, quiet=True)
            time.sleep(1.5)
        if proc.poll() is None:
            proc.kill()
        for name in ("config.xml", "session.xml", "stylers.xml", "langs.xml"):
            target = os.path.join(cfg, name)
            if os.path.exists(target):
                os.remove(target)

    print("  样式方案：配置器能起来并落盘（角色 + 逐样式号两节）、手写配色重启后真的"
          "改变了注释色，逐样式号覆盖也真的压过角色色")
    return True


def test_language_defs(root):
    """语言定义与注释：注释命令按语言取注释符，导出的 langs.xml 改完重启真的生效。

    最后一步是这一段的关键：它证明"语言定义是数据，不是代码"——把 cpp 的行注释符
    改成 ;; 之后重启，注释命令就该用 ;; 而不是 //。只测"命令能加注释"是测不出这层的。
    """
    work = os.path.join(root, "build", "langs_test")
    os.makedirs(work, exist_ok=True)
    path = os.path.join(work, "sample.cpp")
    with open(path, "w", encoding="utf-8", newline="") as handle:
        handle.write("int a = 1;\nint b = 2;\n")

    cfg = os.path.join(root, "build", "bin", "config")
    langs = os.path.join(cfg, "langs.xml")
    for name in ("config.xml", "session.xml", "langs.xml"):
        target = os.path.join(cfg, name)
        if os.path.exists(target):
            os.remove(target)

    exe = os.path.join(root, "build", "bin", "notepadcc.exe")
    proc = subprocess.Popen([exe, MULTI_INSTANCE, "-portable", path], cwd=root)
    window = wait_for_main_window("notepadcc - sample.cpp")
    try:
        if not window:
            print("  FAIL: 找不到主窗口")
            return False
        scinti = visible_scintilla(window)
        if scinti is None:
            print("  FAIL: 找不到可见的编辑控件")
            return False

        # 行注释：加 → 取消
        user32.PostMessageW(window, WM_COMMAND, CMD_TOOL_TOGGLE_COMMENT, 0)
        time.sleep(0.6)
        if scinti_text(scinti) != "// int a = 1;\n// int b = 2;\n":
            print(f"  FAIL: 行注释结果不对 {scinti_text(scinti)!r}")
            return False
        user32.PostMessageW(window, WM_COMMAND, CMD_TOOL_TOGGLE_COMMENT, 0)
        time.sleep(0.6)
        if scinti_text(scinti) != "int a = 1;\nint b = 2;\n":
            print(f"  FAIL: 取消行注释应该回到原文，实际 {scinti_text(scinti)!r}")
            return False

        # 块注释：包住 → 脱掉
        user32.PostMessageW(window, WM_COMMAND, CMD_TOOL_BLOCK_COMMENT, 0)
        time.sleep(0.6)
        # 整段原字节被包住（含结尾换行），所以多行时 */ 会落在独立一行——这是刻意的：
        # 不擅自动用户的行结构
        if scinti_text(scinti) != "/*int a = 1;\nint b = 2;\n*/":
            print(f"  FAIL: 块注释结果不对 {scinti_text(scinti)!r}")
            return False
        user32.PostMessageW(window, WM_COMMAND, CMD_TOOL_BLOCK_COMMENT, 0)
        time.sleep(0.6)
        if scinti_text(scinti) != "int a = 1;\nint b = 2;\n":
            print(f"  FAIL: 取消块注释应该回到原文，实际 {scinti_text(scinti)!r}")
            return False

        # 导出语言定义
        user32.PostMessageW(window, WM_COMMAND, CMD_TOOL_EXPORT_LANGS, 0)
        if not dismiss_dialog("导出语言定义", IDOK, timeout=8.0):
            print("  FAIL: 导出语言定义之后应当给出说明")
            return False
        time.sleep(0.5)
        if not os.path.exists(langs):
            print(f"  FAIL: 没有导出 {langs}")
            return False
        with open(langs, "r", encoding="utf-8", errors="replace") as handle:
            exported = handle.read()
        if 'name="cpp"' not in exported or "<LineComment" not in exported:
            print(f"  FAIL: 导出的 langs.xml 内容不对\n{exported[:300]}")
            return False
    finally:
        if window:
            user32.PostMessageW(window, WM_CLOSE, 0, 0)
            dismiss_dialog("notepadcc", IDNO, timeout=2.0, quiet=True)
            time.sleep(1.5)
        if proc.poll() is None:
            proc.kill()

    # 改语言定义 → 重启 → 注释命令应当改用新注释符
    replaced = exported.replace('<LineComment prefix="//"/>', '<LineComment prefix=";;"/>')
    if replaced == exported:
        print("  FAIL: 没能改写导出的语言定义（找不到 cpp 的行注释）")
        return False
    with open(langs, "w", encoding="utf-8", newline="") as handle:
        handle.write(replaced)

    proc = subprocess.Popen([exe, MULTI_INSTANCE, "-portable", path], cwd=root)
    window = wait_for_main_window("notepadcc - sample.cpp")
    try:
        if not window:
            print("  FAIL: 改完语言定义之后重启找不到窗口")
            return False
        scinti = visible_scintilla(window)
        user32.PostMessageW(window, WM_COMMAND, CMD_TOOL_TOGGLE_COMMENT, 0)
        time.sleep(0.6)
        if scinti_text(scinti) != ";; int a = 1;\n;; int b = 2;\n":
            print(f"  FAIL: 改了 langs.xml 之后注释符应当变成 ;;，实际 "
                  f"{scinti_text(scinti)!r}")
            return False
    finally:
        if window:
            user32.PostMessageW(window, WM_CLOSE, 0, 0)
            dismiss_dialog("notepadcc", IDNO, timeout=2.0, quiet=True)
            time.sleep(1.5)
        if proc.poll() is None:
            proc.kill()
        if os.path.exists(langs):
            os.remove(langs)
        for name in ("config.xml", "session.xml"):
            target = os.path.join(cfg, name)
            if os.path.exists(target):
                os.remove(target)

    print("  语言定义：按语言注释/取消注释（行与块）、导出 langs.xml、"
          "改完重启立即生效")
    return True


def test_tools_menu(root):
    """工具菜单：行操作与文本转换，重点核对"一次操作 = 一步撤销"。

    行操作最容易出的问题不是算错，而是撤销栈被切碎成好几步——用户按一次 Ctrl+Z
    只回退半截，那种体验很糟。所以这里一路做四步，再连按四次撤销，
    核对内容与"已修改"标记一起回到起点。
    """
    work = os.path.join(root, "build", "tools_test")
    os.makedirs(work, exist_ok=True)
    path = os.path.join(work, "lines.txt")
    original = "b  \nA\na\nb\n"
    with open(path, "w", encoding="utf-8", newline="") as handle:
        handle.write(original)

    cfg = os.path.join(root, "build", "bin", "config")
    for name in ("config.xml", "session.xml"):
        target = os.path.join(cfg, name)
        if os.path.exists(target):
            os.remove(target)

    exe = os.path.join(root, "build", "bin", "notepadcc.exe")
    proc = subprocess.Popen([exe, MULTI_INSTANCE, "-portable", path], cwd=root)
    window = wait_for_main_window("notepadcc - lines.txt")
    try:
        if not window:
            print("  FAIL: 找不到主窗口")
            return False
        scinti = visible_scintilla(window)
        if scinti is None:
            print("  FAIL: 找不到可见的编辑控件")
            return False
        if scinti_text(scinti) != original:
            print(f"  FAIL: 初始内容不对 {scinti_text(scinti)!r}")
            return False

        steps = [
            (CMD_TOOL_TRIM_TRAILING, "b\nA\na\nb\n", "去掉行尾空白"),
            (CMD_TOOL_SORT_ASC, "A\na\nb\nb\n", "升序排序"),
            (CMD_TOOL_REMOVE_DUP_FIRST, "A\na\nb\n", "删除重复行"),
            (CMD_TOOL_UPPERCASE, "A\nA\nB\n", "转为大写"),
        ]
        for command_id, expected, label in steps:
            user32.PostMessageW(window, WM_COMMAND, command_id, 0)
            time.sleep(0.6)
            actual = scinti_text(scinti)
            if actual != expected:
                print(f"  FAIL: {label} 之后内容不对\n           实际 {actual!r}\n"
                      f"           期望 {expected!r}")
                return False
        if user32.SendMessageW(scinti, SCI_GETMODIFY, 0, 0) == 0:
            print("  FAIL: 改过内容之后应当处于已修改状态")
            return False

        # 一次撤销回退一步
        user32.PostMessageW(window, WM_COMMAND, CMD_EDIT_UNDO, 0)
        time.sleep(0.6)
        if scinti_text(scinti) != "A\na\nb\n":
            print(f"  FAIL: 一次撤销应当只回退「转为大写」这一步，实际 "
                  f"{scinti_text(scinti)!r}")
            return False
        # 再撤销三次，应当回到最初的内容与未修改状态
        for _ in range(3):
            user32.PostMessageW(window, WM_COMMAND, CMD_EDIT_UNDO, 0)
            time.sleep(0.4)
        if scinti_text(scinti) != original:
            print(f"  FAIL: 全部撤销之后应当回到原文，实际 {scinti_text(scinti)!r}")
            return False
        if user32.SendMessageW(scinti, SCI_GETMODIFY, 0, 0) != 0:
            print("  FAIL: 撤销回到保存点之后不应再是已修改状态")
            return False

        # Base64 编码 → 用 Python 独立解码核对 → 再在程序里解回来
        user32.PostMessageW(window, WM_COMMAND, CMD_TOOL_UPPERCASE, 0)
        time.sleep(0.5)
        user32.PostMessageW(window, WM_COMMAND, CMD_TOOL_BASE64_ENCODE, 0)
        time.sleep(0.6)
        encoded = scinti_text(scinti)
        try:
            decoded = base64.b64decode(encoded, validate=True).decode("utf-8")
        except Exception as error:                      # noqa: BLE001 - 诊断用
            print(f"  FAIL: 编辑器里的内容不是合法 Base64（{error}）：{encoded[:60]!r}")
            return False
        # 上面四次撤销已经把内容还原成原文，所以这里的大写结果是原文的大写版
        if decoded != "B  \nA\nA\nB\n":
            print(f"  FAIL: Base64 内容解出来不对 {decoded!r}")
            return False
        user32.PostMessageW(window, WM_COMMAND, CMD_TOOL_BASE64_DECODE, 0)
        time.sleep(0.6)
        if scinti_text(scinti) != "B  \nA\nA\nB\n":
            print(f"  FAIL: Base64 解码之后内容不对 {scinti_text(scinti)!r}")
            return False
    finally:
        if window:
            user32.PostMessageW(window, WM_CLOSE, 0, 0)
            dismiss_dialog("notepadcc", IDNO, timeout=2.0, quiet=True)
            time.sleep(1.5)
        if proc.poll() is None:
            proc.kill()

    # 非法内容要给出说明，而不是默默改坏内容
    broken = os.path.join(work, "broken.txt")
    with open(broken, "w", encoding="utf-8", newline="") as handle:
        handle.write("not base64!!\n")
    proc = subprocess.Popen([exe, MULTI_INSTANCE, "-portable", broken], cwd=root)
    window = wait_for_main_window("notepadcc - broken.txt")
    try:
        if not window:
            print("  FAIL: 找不到第二个实例的主窗口")
            return False
        user32.PostMessageW(window, WM_COMMAND, CMD_TOOL_BASE64_DECODE, 0)
        if not dismiss_dialog("无法解码", IDOK, timeout=8.0):
            print("  FAIL: 非法 Base64 应当弹出说明，而不是静默处理")
            return False
        time.sleep(0.5)
        scinti = visible_scintilla(window)
        if scinti_text(scinti) != "not base64!!\n":
            print("  FAIL: 解码失败之后内容不该被改动")
            return False
    finally:
        if window:
            user32.PostMessageW(window, WM_CLOSE, 0, 0)
            dismiss_dialog("notepadcc", IDNO, timeout=2.0, quiet=True)
            time.sleep(1.5)
        if proc.poll() is None:
            proc.kill()
        for name in ("config.xml", "session.xml"):
            target = os.path.join(cfg, name)
            if os.path.exists(target):
                os.remove(target)

    print("  工具菜单：去空白 / 排序 / 去重 / 大小写 / Base64 往返正确，"
          "一次操作正好一步撤销，非法输入被挡下")
    return True


def test_mime_binary(root):
    """文件类型识别与 MIME 工具。

    二进制：打开一个开头就有 NUL 的文件，提示要出现在状态栏与日志里，而且内容
    整份装进编辑控件、不被 NUL "截断"（被截断的话，一次保存就等于丢数据）。
    Quoted-Printable：与 Python 标准库 quopri 互为参照——两边各自编码、交对方解码，
    必须逐字节等于原文；非法转义要弹说明并且一个字节都不动。
    """
    work = os.path.join(root, "build", "mime_test")
    os.makedirs(work, exist_ok=True)
    cfg = os.path.join(root, "build", "bin", "config")
    exe = os.path.join(root, "build", "bin", "notepadcc.exe")

    def clean_config():
        for name in ("config.xml", "session.xml"):
            target = os.path.join(cfg, name)
            if os.path.exists(target):
                os.remove(target)

    # ---- 一、二进制识别：NUL 开头的文件要提示"更该用十六进制看" ----
    blob = os.path.join(work, "blob.bin")
    data = b"MZ\x90\x00\x03" + bytes((index * 7 + 1) & 0xFF for index in range(512))
    with open(blob, "wb") as handle:
        handle.write(data)
    clean_config()
    log_path = remove_log_with_retry(root)
    proc = subprocess.Popen([exe, MULTI_INSTANCE, "-portable", blob], cwd=root)
    window = wait_for_main_window("notepadcc - blob.bin")
    try:
        if not window:
            print("  FAIL: 找不到打开了 blob.bin 的窗口")
            return False
        status = ""
        deadline = time.time() + 6.0
        while time.time() < deadline:
            status = read_status_bar(window) or ""
            if "二进制" in status:
                break
            time.sleep(0.2)
        if "二进制" not in status or "十六进制" not in status:
            print(f"  FAIL: 二进制文件的提示没出现在状态栏，实际读到 {status!r}")
            return False
        scinti = visible_scintilla(window)
        if scinti is None:
            print("  FAIL: 找不到编辑控件")
            return False
        length = send_bounded(scinti, SCI_GETLENGTH)
        if length != len(data):
            print(f"  FAIL: 二进制内容被截断：控件里 {length} 字节，文件 {len(data)} 字节")
            return False
        log_text = ""
        if os.path.exists(log_path):
            with open(log_path, "r", encoding="utf-8", errors="replace") as handle:
                log_text = handle.read()
        if "打开提示" not in log_text or "二进制" not in log_text:
            print(f"  FAIL: 日志里没留下二进制提示的底档\n{log_text[-400:]}")
            return False
    finally:
        if window:
            user32.PostMessageW(window, WM_CLOSE, 0, 0)
            dismiss_dialog("notepadcc", IDNO, timeout=2.0, quiet=True)
            time.sleep(1.5)
        if proc.poll() is None:
            proc.kill()

    # ---- 二、Quoted-Printable：与标准库 quopri 互为参照 ----
    original = "中文 mixed = text \r\nline two\r\n" + "长" * 60 + "\r\n"
    encoded_by_python = quopri.encodestring(original.encode("utf-8")).decode("ascii")
    qp_path = os.path.join(work, "qp_in.txt")
    with open(qp_path, "w", encoding="utf-8", newline="") as handle:
        handle.write(encoded_by_python)
    clean_config()
    proc = subprocess.Popen([exe, MULTI_INSTANCE, "-portable", qp_path], cwd=root)
    window = wait_for_main_window("notepadcc - qp_in.txt")
    try:
        if not window:
            print("  FAIL: 找不到打开了 qp_in.txt 的窗口")
            return False
        scinti = visible_scintilla(window)
        if scinti is None:
            print("  FAIL: 找不到编辑控件")
            return False
        # 1) 程序解 quopri 编出来的内容：应当逐字等于原文（软换行 =\r\n 一并吃掉）
        user32.PostMessageW(window, WM_COMMAND, CMD_TOOL_QP_DECODE, 0)
        time.sleep(0.8)
        decoded = scinti_text(scinti)
        if decoded != original:
            print(f"  FAIL: 程序解码 quopri 产物不对\n           实际 {decoded[:80]!r}\n"
                  f"           期望 {original[:80]!r}")
            return False
        # 2) 程序再编码：几个形状必须对，且 quopri 能逐字节解回原文
        user32.PostMessageW(window, WM_COMMAND, CMD_TOOL_QP_ENCODE, 0)
        time.sleep(0.8)
        encoded = scinti_text(scinti)
        if "=E4=B8=AD" not in encoded:
            print(f"  FAIL: 非 ASCII 没编成大写 =XX {encoded[:80]!r}")
            return False
        if "=3D" not in encoded:
            print(f"  FAIL: '=' 没编码 {encoded[:80]!r}")
            return False
        if " \r\n" in encoded:
            print("  FAIL: 行尾空格没编码（运输途中会被抹掉）")
            return False
        back = quopri.decodestring(encoded.encode("ascii")).decode("utf-8")
        if back != original:
            print(f"  FAIL: quopri 把程序编的内容解回来与原文不一致\n"
                  f"           实际 {back[:80]!r}\n           期望 {original[:80]!r}")
            return False
    finally:
        if window:
            user32.PostMessageW(window, WM_CLOSE, 0, 0)
            dismiss_dialog("notepadcc", IDNO, timeout=2.0, quiet=True)
            time.sleep(1.5)
        if proc.poll() is None:
            proc.kill()

    # ---- 三、非法转义：弹说明、内容一个字节都不动 ----
    broken = os.path.join(work, "qp_broken.txt")
    with open(broken, "w", encoding="utf-8", newline="") as handle:
        handle.write("good =41 but broken =zz tail\r\n")
    clean_config()
    proc = subprocess.Popen([exe, MULTI_INSTANCE, "-portable", broken], cwd=root)
    window = wait_for_main_window("notepadcc - qp_broken.txt")
    try:
        if not window:
            print("  FAIL: 找不到打开了 qp_broken.txt 的窗口")
            return False
        user32.PostMessageW(window, WM_COMMAND, CMD_TOOL_QP_DECODE, 0)
        if not dismiss_dialog("无法解码", IDOK, timeout=8.0):
            print("  FAIL: 非法 Quoted-Printable 应当弹出说明，而不是静默处理")
            return False
        time.sleep(0.5)
        scinti = visible_scintilla(window)
        if scinti is None or scinti_text(scinti) != "good =41 but broken =zz tail\r\n":
            print("  FAIL: 解码失败之后内容不该被改动")
            return False
    finally:
        if window:
            user32.PostMessageW(window, WM_CLOSE, 0, 0)
            dismiss_dialog("notepadcc", IDNO, timeout=2.0, quiet=True)
            time.sleep(1.5)
        if proc.poll() is None:
            proc.kill()
        clean_config()

    print("  文件类型识别与 MIME 工具：二进制提示进状态栏与日志（内容不被 NUL 截断），"
          "Quoted-Printable 与 quopri 双向逐字节一致，非法转义被挡下")
    return True


def test_eye_care(root):
    """护眼模式（低蓝光）：编辑区默认前后景换成暖色、写进配置、重启保持、关掉恢复。

    读的是控件真正拿去绘制的颜色（SCI_STYLEGETFORE/BACK 的 COLORREF），不是配置文件
    里的值——"配置写对了而界面没换"正是这类功能最容易出的问题。
    """
    work = os.path.join(root, "build", "eyecare_test")
    os.makedirs(work, exist_ok=True)
    path = os.path.join(work, "paper.txt")
    with open(path, "w", encoding="utf-8", newline="") as handle:
        handle.write("护眼模式回归\n第二行\n")

    cfg = os.path.join(root, "build", "bin", "config")
    config = os.path.join(cfg, "config.xml")
    for name in ("config.xml", "session.xml"):
        target = os.path.join(cfg, name)
        if os.path.exists(target):
            os.remove(target)

    def read_config():
        if not os.path.exists(config):
            return ""
        with open(config, "r", encoding="utf-8", errors="replace") as handle:
            return handle.read()

    def wait_config(marker):
        deadline = time.time() + 5.0
        saved = ""
        while time.time() < deadline:
            saved = read_config()
            if marker in saved:
                return saved
            time.sleep(0.2)
        return saved

    exe = os.path.join(root, "build", "bin", "notepadcc.exe")
    proc = subprocess.Popen([exe, MULTI_INSTANCE, "-portable", path], cwd=root)
    window = wait_for_main_window("notepadcc - paper.txt")
    try:
        if not window:
            print("  FAIL: 找不到主窗口")
            return False
        scinti = visible_scintilla(window)
        if scinti is None:
            print("  FAIL: 找不到可见的编辑控件")
            return False
        # 基线：浅色主题的默认前景/背景（后面关掉护眼时要回到这两个值）
        base_fore = user32.SendMessageW(scinti, SCI_STYLEGETFORE, STYLE_DEFAULT, 0)
        base_back = user32.SendMessageW(scinti, SCI_STYLEGETBACK, STYLE_DEFAULT, 0)
        if base_back != to_colorref(0xFFFFFF):
            print(f"  FAIL: 浅色主题的默认背景应当是白的，实际 0x{base_back:06X}")
            return False

        # 1) 开护眼：默认背景变米色、前景变暖深灰，行号边距的底色也换
        user32.PostMessageW(window, WM_COMMAND, CMD_VIEW_EYE_CARE, 0)
        time.sleep(0.8)
        got_back = user32.SendMessageW(scinti, SCI_STYLEGETBACK, STYLE_DEFAULT, 0)
        if got_back != to_colorref(0xF4E8D4):
            print(f"  FAIL: 护眼模式下背景应当是米色 #F4E8D4，实际 0x{got_back:06X}")
            return False
        if user32.SendMessageW(scinti, SCI_STYLEGETFORE, STYLE_DEFAULT, 0) != to_colorref(0x3B362C):
            print("  FAIL: 护眼模式下默认前景应当是暖深灰 #3B362C")
            return False
        if user32.SendMessageW(scinti, SCI_STYLEGETBACK, STYLE_LINENUMBER, 0) != to_colorref(0xEADBBD):
            print("  FAIL: 行号边距的底色也要跟着换暖")
            return False
        saved = wait_config('eyeCare="yes"')
        if 'eyeCare="yes"' not in saved:
            print(f"  FAIL: config.xml 里没有 eyeCare=\"yes\"\n{saved[:300]}")
            return False
    finally:
        if window:
            user32.PostMessageW(window, WM_CLOSE, 0, 0)
            dismiss_dialog("notepadcc", IDNO, timeout=2.0, quiet=True)
            time.sleep(1.5)
        if proc.poll() is None:
            proc.kill()

    # 2) 重启：还记着（配置只记"要不要"，色值跟着主题走）
    proc = subprocess.Popen([exe, MULTI_INSTANCE, "-portable", path], cwd=root)
    window = wait_for_main_window("notepadcc - paper.txt")
    try:
        if not window:
            print("  FAIL: 重启后找不到主窗口")
            return False
        scinti = visible_scintilla(window)
        if user32.SendMessageW(scinti, SCI_STYLEGETBACK, STYLE_DEFAULT, 0) != to_colorref(0xF4E8D4):
            print("  FAIL: 重启之后护眼配色没有保持")
            return False

        # 3) 关掉：回到原色，config.xml 写回 no
        user32.PostMessageW(window, WM_COMMAND, CMD_VIEW_EYE_CARE, 0)
        time.sleep(0.8)
        if user32.SendMessageW(scinti, SCI_STYLEGETBACK, STYLE_DEFAULT, 0) != base_back:
            print("  FAIL: 关掉护眼之后没有回到原来的背景色")
            return False
        if user32.SendMessageW(scinti, SCI_STYLEGETFORE, STYLE_DEFAULT, 0) != base_fore:
            print("  FAIL: 关掉护眼之后没有回到原来的前景色")
            return False
        saved = wait_config('eyeCare="no"')
        if 'eyeCare="no"' not in saved:
            print(f"  FAIL: 关掉之后 config.xml 没写回 eyeCare=\"no\"\n{saved[:300]}")
            return False
    finally:
        if window:
            user32.PostMessageW(window, WM_CLOSE, 0, 0)
            dismiss_dialog("notepadcc", IDNO, timeout=2.0, quiet=True)
            time.sleep(1.5)
        if proc.poll() is None:
            proc.kill()
        for name in ("config.xml", "session.xml"):
            target = os.path.join(cfg, name)
            if os.path.exists(target):
                os.remove(target)

    print("  护眼模式：编辑区与行号换暖色、写进 config.xml、重启保持、关掉恢复原值")
    return True


def test_portable_package(root):
    """绿色发行：打包 → 解压 → 直接运行，核对"解压即用"与"只写程序目录"。

    这里是拿真实产物验证设计文档 2.15 的绿色承诺：不写 %APPDATA%、不写注册表、
    整个目录拷走或删掉即完成迁移与卸载。**故意不加 -portable**——靠的正是包里的
    portable 标记文件，加了开关就等于没测到这条。
    """
    script = os.path.join(root, "scripts", "package.ps1")
    result = subprocess.run(["powershell", "-ExecutionPolicy", "Bypass", "-File", script,
                             "-NoBuild"],
                            capture_output=True, text=True, encoding="utf-8", errors="replace")
    if result.returncode != 0:
        print(f"  FAIL: 打包脚本失败\n{result.stdout[-400:]}\n{result.stderr[-400:]}")
        return False

    dist = os.path.join(root, "dist")
    packages = [name for name in os.listdir(dist)
                if name.startswith("notepadcc-") and name.endswith("-portable-x64.zip")]
    if len(packages) != 1:
        print(f"  FAIL: dist 下的便携包应当正好 1 个，实际 {packages}")
        return False
    zip_path = os.path.join(dist, packages[0])

    target = os.path.join(root, "build", "pack_test")
    shutil.rmtree(target, ignore_errors=True)
    os.makedirs(target)
    with zipfile.ZipFile(zip_path) as archive:
        names = archive.namelist()
        archive.extractall(target)
    for required in ("notepadcc.exe", "portable", "说明.txt"):
        if required not in names:
            print(f"  FAIL: 便携包里缺少 {required}，实际内容 {names}")
            return False

    exe = os.path.join(target, "notepadcc.exe")
    sample = os.path.join(target, "示例.txt")
    with open(sample, "w", encoding="utf-8", newline="") as handle:
        handle.write("绿色版自检\n")

    before = appdata_snapshot()
    proc = subprocess.Popen([exe, MULTI_INSTANCE, sample], cwd=target)   # 注意：不加 -portable
    window = None
    try:
        window = wait_for_main_window("notepadcc - 示例.txt", timeout=40.0)
        if not window:
            print("  FAIL: 解压出来的程序没跑起来（可能是缺运行库或便携判定失效）")
            return False
        config_dir = os.path.join(target, "config")
        if not os.path.isdir(config_dir):
            print("  FAIL: 便携模式下配置应当写在程序目录的 config\\ 下")
            return False
    finally:
        if window:
            user32.PostMessageW(window, WM_CLOSE, 0, 0)
            dismiss_dialog("notepadcc", IDNO, timeout=2.0, quiet=True)
            time.sleep(1.5)
        if proc.poll() is None:
            proc.kill()

    # 会话与日志是在正常使用中一定会落盘的：用它们证明"真的写在了程序目录里"
    for produced in (os.path.join("config", "session.xml"),
                     os.path.join("config", "logs", "notepadcc.log")):
        if not os.path.exists(os.path.join(target, produced)):
            print(f"  FAIL: 便携模式下应当在程序目录里生成 {produced}")
            return False
    after = appdata_snapshot()
    if after != before:
        print(f"  FAIL: 绿色版不该动 %APPDATA%\\notepadcc（前 {before!r}，后 {after!r}）")
        return False
    # 卸载就是删目录：这里顺手验证"删掉之后什么都不剩"
    shutil.rmtree(target, ignore_errors=True)
    if os.path.exists(target):
        print("  FAIL: 程序目录删不掉")
        return False

    print("  绿色发行：打包产物解压即用（不靠开关）、配置写在程序目录、"
          "全程没碰 %APPDATA%、删目录即卸载")
    return True


def test_associated_commands(root):
    """文件关联：核对命令真的把程序写进「打开方式」列表，并且能干净移除。

    测试会**先记下既有状态**：如果这台机器上本来就把 notepadcc 登记过，就不做移除那一步，
    免得把用户的既有设置抹掉——测试不该改动用户机器上已经存在的状态。
    """
    exe = os.path.join(root, "build", "bin", "notepadcc.exe")
    work = os.path.join(root, "build", "assoc_test")
    os.makedirs(work, exist_ok=True)
    path = os.path.join(work, "assoc.txt")
    with open(path, "w", encoding="utf-8", newline="") as handle:
        handle.write("hello\n")

    had_it_before = assoc_registered()
    proc = subprocess.Popen([exe, MULTI_INSTANCE, "-portable", path], cwd=root)
    window = wait_for_main_window("notepadcc - assoc.txt")
    try:
        if not window:
            print("  FAIL: 找不到主窗口")
            return False
        user32.PostMessageW(window, WM_COMMAND, CMD_SETTINGS_REGISTER_ASSOC, 0)
        if not dismiss_dialog("文件关联", IDOK, timeout=10.0):
            print("  FAIL: 登记文件关联之后应当给出说明")
            return False
        time.sleep(0.5)
        if not assoc_registered():
            print("  FAIL: 登记之后 .txt 的打开方式列表里应当有 notepadcc")
            return False
        try:
            with winreg.OpenKey(winreg.HKEY_CURRENT_USER,
                                "Software\\Classes\\notepadcc.txt\\shell\\open\\command") as key:
                command, _ = winreg.QueryValueEx(key, "")
            if exe not in command or '"%1"' not in command:
                print(f"  FAIL: 打开命令不对：{command!r}")
                return False
        except OSError as error:
            print(f"  FAIL: 读不到打开命令：{error}")
            return False

        if had_it_before:
            print("  （这台机器上本来就登记过 notepadcc，跳过移除那一步以免动到既有设置）")
        else:
            user32.PostMessageW(window, WM_COMMAND, CMD_SETTINGS_UNREGISTER_ASSOC, 0)
            if not dismiss_dialog("文件关联", IDOK, timeout=10.0):
                print("  FAIL: 移除文件关联之后应当给出说明")
                return False
            time.sleep(0.5)
            if assoc_registered():
                print("  FAIL: 移除之后打开方式列表里不该还有 notepadcc")
                return False
    finally:
        if window:
            user32.PostMessageW(window, WM_CLOSE, 0, 0)
            dismiss_dialog("notepadcc", IDNO, timeout=2.0, quiet=True)
            time.sleep(1.5)
        if proc.poll() is None:
            proc.kill()

    print("  文件关联：命令把程序登记进「打开方式」列表（命令带引号），并能干净移除")
    return True


def test_single_instance(root):
    """单实例：第二个实例把文件交给已在跑的那个，然后自己退出。

    核对的是"窗口个数"与"标题"——只看第二个进程退没退，不足以说明文件真的交到了对方手里。
    """
    work = os.path.join(root, "build", "instance_test")
    os.makedirs(work, exist_ok=True)
    first = os.path.join(work, "first.txt")
    second = os.path.join(work, "second.txt")
    for target, text in ((first, "first file\n"), (second, "second file\n")):
        with open(target, "w", encoding="utf-8", newline="") as handle:
            handle.write(text)

    cfg = os.path.join(root, "build", "bin", "config")
    for name in ("config.xml", "session.xml"):
        target = os.path.join(cfg, name)
        if os.path.exists(target):
            os.remove(target)

    exe = os.path.join(root, "build", "bin", "notepadcc.exe")
    primary = subprocess.Popen([exe, "-portable", first], cwd=root)
    handoff = None
    extra = None
    try:
        window = wait_for_main_window("notepadcc - first.txt")
        if not window:
            print("  FAIL: 第一个实例没起来")
            return False

        handoff = subprocess.Popen([exe, "-portable", second], cwd=root)
        deadline = time.time() + 25
        while time.time() < deadline and handoff.poll() is None:
            time.sleep(0.3)
        if handoff.poll() is None:
            print("  FAIL: 第二个实例应当转交文件后自己退出，但它还活着")
            return False
        if handoff.poll() != 0:
            print(f"  FAIL: 第二个实例的退出码应当是 0，实际 {handoff.poll()}")
            return False
        if not wait_for_main_window("notepadcc - second.txt", timeout=15.0):
            print("  FAIL: 第一个实例没有打开转交过来的文件")
            return False
        if len(main_windows()) != 1:
            print(f"  FAIL: 单实例模式下应当只有 1 个主窗口，实际 {len(main_windows())}")
            return False
        scinti = visible_scintilla(window)
        if scinti is None or "second file" not in scinti_text(scinti):
            print("  FAIL: 转交过来的文件内容没有显示出来")
            return False

        extra = subprocess.Popen([exe, "-portable", "--multi-instance", first], cwd=root)
        deadline = time.time() + 25
        while time.time() < deadline and len(main_windows()) < 2:
            time.sleep(0.3)
        if len(main_windows()) != 2:
            print(f"  FAIL: 允许多开时应当有 2 个主窗口，实际 {len(main_windows())}")
            return False
    finally:
        for child in main_windows():
            user32.PostMessageW(child, WM_CLOSE, 0, 0)
            dismiss_dialog("notepadcc", IDNO, timeout=2.0, quiet=True)
        time.sleep(2)
        for child in (primary, handoff, extra):
            if child is not None and child.poll() is None:
                child.kill()
        for name in ("config.xml", "session.xml"):
            target = os.path.join(cfg, name)
            if os.path.exists(target):
                os.remove(target)

    print("  单实例：第二个实例转交文件后退出、只有一个窗口；--multi-instance 时各开各的")
    return True


def test_encoding_convert(root):
    """编码与行尾转换：核对**磁盘上的字节**真的变了，以及表示不了的字符被挡下来。

    只测"命令执行没报错"没有意义——编码转换错的后果是文件被改坏，
    所以这一段一律读回磁盘原始字节来核对，并且检查 .bak 备份确实留下了原样。
    """
    work = os.path.join(root, "build", "convert_test")
    os.makedirs(work, exist_ok=True)
    path = os.path.join(work, "code.txt")
    original = "中文 test\n第二行\n"
    # GBK 下的 "中文" 是 D6D0 CEC4（单元测试里已经核过一遍）
    gbk_head = b"\xd6\xd0\xce\xc4"
    with open(path, "w", encoding="utf-8", newline="") as handle:
        handle.write(original)
    backup = path + ".bak"
    if os.path.exists(backup):
        os.remove(backup)

    cfg = os.path.join(root, "build", "bin", "config")
    for name in ("config.xml", "session.xml"):
        target = os.path.join(cfg, name)
        if os.path.exists(target):
            os.remove(target)

    exe = os.path.join(root, "build", "bin", "notepadcc.exe")
    proc = subprocess.Popen([exe, MULTI_INSTANCE, "-portable", path], cwd=root)
    window = wait_for_main_window("notepadcc - code.txt")
    try:
        if not window:
            print("  FAIL: 找不到主窗口")
            return False
        scinti = visible_scintilla(window)
        if scinti is None:
            print("  FAIL: 找不到可见的编辑控件")
            return False
        if scinti_text(scinti) != original:
            print(f"  FAIL: 初始内容不对 {scinti_text(scinti)!r}")
            return False

        # 1) 转为 ANSI 并保存：磁盘上应当变成 GBK，且留下 .bak
        user32.PostMessageW(window, WM_COMMAND, CMD_ENCODING_ANSI, 0)
        time.sleep(0.8)
        if "无法转换编码" in str(dump_top_level_windows()):
            print("  FAIL: 纯中文内容转 ANSI 不该失败")
            return False
        user32.PostMessageW(window, WM_COMMAND, CMD_FILE_SAVE, 0)
        time.sleep(1.2)
        with open(path, "rb") as handle:
            converted = handle.read()
        if not converted.startswith(gbk_head):
            print(f"  FAIL: 转为 ANSI 之后磁盘上应当是 GBK 字节，实际 {converted[:8]!r}")
            return False
        if len(converted) >= len(original.encode("utf-8")):
            print(f"  FAIL: GBK 版本不该比 UTF-8 版本更长（{len(converted)}）")
            return False
        if not os.path.exists(backup):
            print("  FAIL: 转换前应当留下 .bak 备份")
            return False
        with open(backup, "rb") as handle:
            if handle.read() != original.encode("utf-8"):
                print("  FAIL: .bak 里应当是转换前的那份内容")
                return False
        # 转成 ANSI 之后缓冲区里就是 GBK 字节了：Scintilla 按代码页 936 解释它们、
        # 屏幕上显示中文，而测试助手是按 UTF-8 解码读字节的，所以这里会读到替换符。
        # 这是预期行为，不是内容坏了——判据是磁盘字节与 Scintilla 的代码页设置。
        if user32.SendMessageW(scinti, SCI_GETCODEPAGE, 0, 0) != 936:
            print("  FAIL: 转为 ANSI 之后 Scintilla 的代码页应当是 936")
            return False
        if scinti_text(scinti) == original:
            print("  FAIL: 转为 ANSI 之后缓冲区字节应当变成 GBK")
            return False

        # 2) 转回 UTF-8 并保存：磁盘上恢复成 UTF-8
        user32.PostMessageW(window, WM_COMMAND, CMD_ENCODING_UTF8, 0)
        time.sleep(0.8)
        user32.PostMessageW(window, WM_COMMAND, CMD_FILE_SAVE, 0)
        time.sleep(1.2)
        with open(path, "rb") as handle:
            back = handle.read()
        if back != original.encode("utf-8"):
            print(f"  FAIL: 转回 UTF-8 之后磁盘内容应当与最初一致，实际 {back!r}")
            return False
        if user32.SendMessageW(scinti, SCI_GETCODEPAGE, 0, 0) != 65001:
            print("  FAIL: 转回 UTF-8 之后 Scintilla 的代码页应当是 65001")
            return False
        if scinti_text(scinti) != original:
            print(f"  FAIL: 转回 UTF-8 之后编辑器内容不对 {scinti_text(scinti)!r}")
            return False

        # 3) 行尾转为 CRLF 并保存
        user32.PostMessageW(window, WM_COMMAND, CMD_FORMAT_EOL_CRLF, 0)
        time.sleep(0.8)
        user32.PostMessageW(window, WM_COMMAND, CMD_FILE_SAVE, 0)
        time.sleep(1.2)
        with open(path, "rb") as handle:
            crlf = handle.read()
        if crlf != original.replace("\n", "\r\n").encode("utf-8"):
            print(f"  FAIL: 转为 CRLF 之后磁盘内容不对 {crlf!r}")
            return False
    finally:
        if window:
            user32.PostMessageW(window, WM_CLOSE, 0, 0)
            dismiss_dialog("notepadcc", IDNO, timeout=2.0, quiet=True)
            time.sleep(1.5)
        if proc.poll() is None:
            proc.kill()

    # 4) 表示不了的字符必须被挡下来，文件不能被改坏
    with open(path, "wb") as handle:
        handle.write("emoji \U0001F600\n".encode("utf-8"))
    emoji_bytes = os.path.getsize(path)
    proc = subprocess.Popen([exe, MULTI_INSTANCE, "-portable", path], cwd=root)
    window = wait_for_main_window("notepadcc - code.txt")
    try:
        if not window:
            print("  FAIL: 找不到第二个实例的主窗口")
            return False
        user32.PostMessageW(window, WM_COMMAND, CMD_ENCODING_ANSI, 0)
        if not dismiss_dialog("无法转换编码", IDOK, timeout=8.0):
            print("  FAIL: 表示不了的字符应当弹出说明，而不是静默转换")
            return False
        time.sleep(0.5)
        user32.PostMessageW(window, WM_COMMAND, CMD_FILE_SAVE, 0)
        time.sleep(1.0)
        with open(path, "rb") as handle:
            after = handle.read()
        if len(after) != emoji_bytes:
            print(f"  FAIL: 转换失败之后文件不该被改动（{len(after)} vs {emoji_bytes}）")
            return False
        if "😀" not in after.decode("utf-8", errors="replace"):
            print("  FAIL: 文件内容被改坏了")
            return False
    finally:
        if window:
            user32.PostMessageW(window, WM_CLOSE, 0, 0)
            dismiss_dialog("notepadcc", IDNO, timeout=2.0, quiet=True)
            time.sleep(1.5)
        if proc.poll() is None:
            proc.kill()
        for name in ("config.xml", "session.xml"):
            target = os.path.join(cfg, name)
            if os.path.exists(target):
                os.remove(target)
        if os.path.exists(backup):
            os.remove(backup)

    print("  编码转换：UTF-8 ↔ ANSI(GBK) ↔ UTF-8 字节正确、.bak 留底、"
          "行尾转 CRLF、表示不了的字符被挡下")
    return True


def test_split_view(root):
    """分屏与克隆视图：两个视图共用一份文档、同步滚动、分隔条拖动、随会话恢复。

    跨进程没法伪造鼠标，所以分隔条是真发 WM_LBUTTONDOWN / WM_MOUSEMOVE / WM_LBUTTONUP
    过去拖的——这几个都是只传值的消息，能过进程边界，验证的是真正那条交互路径。
    """
    work = os.path.join(root, "build", "split_test")
    os.makedirs(work, exist_ok=True)
    path = os.path.join(work, "split.txt")
    with open(path, "w", encoding="utf-8", newline="") as handle:
        for i in range(200):
            handle.write(f"line {i:03d}\n")

    cfg = os.path.join(root, "build", "bin", "config")
    for name in ("config.xml", "session.xml"):
        target = os.path.join(cfg, name)
        if os.path.exists(target):
            os.remove(target)

    exe = os.path.join(root, "build", "bin", "notepadcc.exe")

    def client_size(window):
        rect = wintypes.RECT()
        user32.GetClientRect(window, ctypes.byref(rect))
        return rect.right - rect.left, rect.bottom - rect.top

    def width_of(view):
        rect = wintypes.RECT()
        user32.GetWindowRect(view, ctypes.byref(rect))
        return rect.right - rect.left

    def first_visible(view):
        return user32.SendMessageW(view, SCI_GETFIRSTVISIBLELINE, 0, 0)

    proc = subprocess.Popen([exe, MULTI_INSTANCE, "-portable", path], cwd=root)
    window = wait_for_main_window("notepadcc - split.txt")
    try:
        if not window:
            print("  FAIL: 找不到主窗口")
            return False
        if len(visible_scintillas_sorted(window)) != 1:
            print("  FAIL: 未分屏时应当只有 1 个可见的编辑控件")
            return False

        user32.PostMessageW(window, WM_COMMAND, CMD_SPLIT_TOGGLE, 0)
        time.sleep(1.0)
        views = visible_scintillas_sorted(window)
        if len(views) != 2:
            print(f"  FAIL: 分屏之后应有 2 个可见的编辑控件，实际 {len(views)}")
            return False
        primary, clone = views
        if scinti_text(primary) != scinti_text(clone):
            print("  FAIL: 两个视图显示的内容应当一致")
            return False
        if scinti_text(clone).splitlines()[0] != "line 000":
            print(f"  FAIL: 克隆视图内容不对 {scinti_text(clone)[:40]!r}")
            return False

        user32.SendMessageW(primary, SCI_SETSELECTIONMODE, SC_SEL_RECTANGLE, 0)
        user32.SendMessageW(primary, SCI_SETRECTANGULARSELECTIONANCHOR, 1, 0)
        user32.SendMessageW(primary, SCI_SETRECTANGULARSELECTIONCARET, 21, 0)
        time.sleep(0.3)
        user32.PostMessageW(window, WM_COMMAND, CMD_COLUMN_EDIT, 0)
        time.sleep(1.5)
        dialog = user32.FindWindowW("notepadccColumnDialog", None)
        if not dialog:
            print("  FAIL: 列块对话框没有出现")
            return False
        set_text(dialog, COLUMN_TEMPLATE_EDIT, "K")
        user32.PostMessageW(dialog, WM_COMMAND, COLUMN_INSERT_BUTTON, 0)
        time.sleep(1.0)
        # 矩形选区从第 1 列开始，所以 "line 000" 会变成 "lKine 000"
        if "lKine 000" not in scinti_text(clone):
            print(f"  FAIL: 主视图编辑后克隆视图没有同步\n{scinti_text(clone)[:80]!r}")
            return False
        if scinti_text(clone) != scinti_text(primary):
            print("  FAIL: 两个视图的内容不一致（共享文档没生效）")
            return False

        for _ in range(4):
            user32.PostMessageW(primary, WM_VSCROLL, SB_PAGEDOWN, 0)
            time.sleep(0.2)
        if first_visible(primary) <= 0:
            print("  FAIL: 主视图没有滚动起来，后面的同步断言没有意义")
            return False
        # 等两边收敛再判：通知是异步到的，而且这台机器上还有真实鼠标在动，
        # 读一次就下结论容易冤判。同步真的坏了的话，等多久都不会一致。
        if not wait_until_equal(first_visible, primary, clone, timeout=4.0):
            print(f"  FAIL: 同步滚动失效：主 {first_visible(primary)} / "
                  f"克隆 {first_visible(clone)}")
            return False

        user32.PostMessageW(window, WM_COMMAND, CMD_SCROLL_SYNC, 0)
        time.sleep(0.4)
        before = first_visible(clone)
        for _ in range(2):
            user32.PostMessageW(primary, WM_VSCROLL, SB_PAGEDOWN, 0)
            time.sleep(0.2)
        time.sleep(0.8)
        if first_visible(clone) != before:
            print("  FAIL: 关掉同步滚动之后克隆视图不该跟着动")
            return False
        user32.PostMessageW(window, WM_COMMAND, CMD_SCROLL_SYNC, 0)
        time.sleep(0.4)

        width, height = client_size(window)
        if width < 300:
            print(f"  FAIL: 客户区太窄（{width}），没法测分隔条拖动")
            return False
        # 分隔条的位置不要自己算：问程序主视图的右边界在哪，那就是分隔条。
        # （自己按 50% 算会和实际差一两个像素，点在分隔条外面就整段测不到了。）
        # 这一小段要重试：按下之后程序会 SetCapture，此后收到的是**真实鼠标**的
        # 移动。测试机上的物理光标停在哪儿、有没有人在动鼠标，都会影响结果，
        # 所以每次重新量一遍分隔条位置再拖。重试的是输入模拟，不是断言。
        target_client_x = width * 3 // 4
        dragged = False
        for _ in range(3):
            rect = wintypes.RECT()
            user32.GetWindowRect(primary, ctypes.byref(rect))
            point = wintypes.POINT(rect.right, (rect.top + rect.bottom) // 2)
            user32.ScreenToClient(window, ctypes.byref(point))
            divider_x = point.x + 2
            screen_y = (rect.top + rect.bottom) // 2
            target_point = wintypes.POINT(target_client_x, (rect.top + rect.bottom) // 2)
            user32.ClientToScreen(window, ctypes.byref(target_point))

            user32.SetCursorPos(rect.right + 2, screen_y)
            time.sleep(0.2)
            user32.PostMessageW(window, WM_LBUTTONDOWN, 1,
                                ((height // 2) << 16) | divider_x)
            time.sleep(0.3)
            user32.SetCursorPos(target_point.x, target_point.y)
            time.sleep(0.3)
            # 再补一次投递过去的移动：真实输入与投递的消息同队列按序处理，
            # 最后落定的坐标就是目标点。
            target_lparam = ((height // 2) << 16) | target_client_x
            user32.PostMessageW(window, WM_MOUSEMOVE, 1, target_lparam)
            time.sleep(0.3)
            user32.PostMessageW(window, WM_LBUTTONUP, 0, target_lparam)
            time.sleep(0.6)
            ratio = width_of(primary) * 100 // width
            if 65 <= ratio <= 85:
                dragged = True
                break
        if not dragged:
            print(f"  FAIL: 拖到 3/4 之后主视图应占约 75%，实际 {ratio}%"
                  f"（客户区宽 {width}，点击点 x={divider_x}，目标 x={target_client_x}）")
            return False

        user32.PostMessageW(window, WM_COMMAND, CMD_CYCLE_PANE, 0)
        time.sleep(0.4)
        if len(visible_scintillas_sorted(window)) != 2:
            print("  FAIL: 切换焦点之后分屏不该消失")
            return False

        # 收尾：撤销刚才那次列块插入。文档必须回到"未修改"，否则退出时会卡在
        # "是否保存"上——那是模态框，没人点它，会话就写不出去。
        user32.PostMessageW(window, WM_COMMAND, CMD_EDIT_UNDO, 0)
        time.sleep(1.0)
        if user32.SendMessageW(primary, SCI_GETMODIFY, 0, 0) != 0:
            print("  FAIL: 撤销之后文档仍是已修改状态，退出会卡在保存提示上")
            return False
    finally:
        if window:
            user32.PostMessageW(window, WM_CLOSE, 0, 0)
            dismiss_dialog("notepadcc", IDNO, timeout=2.0, quiet=True)
            time.sleep(2)
        if proc.poll() is None:
            proc.kill()

    session = os.path.join(cfg, "session.xml")
    if not os.path.exists(session):
        print("  FAIL: 退出时没有写出 session.xml")
        return False
    with open(session, "r", encoding="utf-8", errors="replace") as handle:
        session_text = handle.read()
    if "<ViewGroup" not in session_text or 'axis="Vertical"' not in session_text:
        print(f"  FAIL: 会话里没有分屏状态\n{session_text[:400]}")
        return False

    proc = subprocess.Popen([exe, MULTI_INSTANCE, "-portable"], cwd=root)
    window = wait_for_main_window("notepadcc - split.txt")
    try:
        if not window:
            print("  FAIL: 重启后没有恢复出上次的文件")
            return False
        views = visible_scintillas_sorted(window)
        if len(views) != 2:
            print(f"  FAIL: 分屏状态没有被会话恢复，可见编辑控件 {len(views)} 个")
            return False
        width, _ = client_size(window)
        restored = width_of(views[0]) * 100 // width
        if not 65 <= restored <= 85:
            print(f"  FAIL: 恢复后的分屏比例不对：{restored}%")
            return False
        if scinti_text(views[1]) != scinti_text(views[0]):
            print("  FAIL: 恢复出来的两个视图内容不一致")
            return False
    finally:
        if window:
            user32.PostMessageW(window, WM_CLOSE, 0, 0)
            time.sleep(1.5)
        if proc.poll() is None:
            proc.kill()
        for name in ("session.xml", "config.xml"):
            target = os.path.join(cfg, name)
            if os.path.exists(target):
                os.remove(target)

    print("  分屏与克隆视图：共享文档实时同步、同步滚动、分隔条拖动、随会话恢复")
    return True


def test_snapshots(root):
    """崩溃快照：定时快照扛硬杀，崩溃抢救扛异常退出，两条都要能恢复回来。

    两段都用同一套手法制造"未保存的改动"：建立矩形选区 → Alt+C → 列块插入。
    跨进程没法用 SCI_SETTEXT 塞文本（指针参数不封送），而这条路径是纯命令驱动的，
    顺带也验证了列编辑与快照两条链路能配合。
    """
    work = os.path.join(root, "build", "snapshot_test")
    os.makedirs(work, exist_ok=True)
    path = os.path.join(work, "doc.txt")

    cfg = os.path.join(root, "build", "bin", "config")
    backup = os.path.join(cfg, "backup")
    exe = os.path.join(root, "build", "bin", "notepadcc.exe")
    recovery_title = "恢复未保存的内容"
    crash_title = "notepadcc 崩溃"

    def reset(content, interval=5):
        with open(path, "w", encoding="utf-8", newline="") as handle:
            handle.write(content)
        for name in ("config.xml", "session.xml"):
            target = os.path.join(cfg, name)
            if os.path.exists(target):
                os.remove(target)
        # 定时快照默认 30 秒，测试等不起，先用配置文件把它调到 5 秒
        with open(os.path.join(cfg, "config.xml"), "w", encoding="utf-8") as handle:
            handle.write('<?xml version="1.0" encoding="UTF-8"?>\n'
                         '<NotepadccConfig schemaVersion="1">\n'
                         f'  <backup enabled="yes" intervalSeconds="{interval}" '
                         'keepSnapshots="3"/>\n'
                         '</NotepadccConfig>\n')
        if os.path.isdir(backup):
            for name in os.listdir(backup):
                shutil.rmtree(os.path.join(backup, name), ignore_errors=True)

    def snapshot_files():
        found = []
        if not os.path.isdir(backup):
            return found
        for name in os.listdir(backup):
            full = os.path.join(backup, name)
            if os.path.isdir(full):
                found.extend(os.path.join(full, f) for f in os.listdir(full)
                             if f.endswith(".snap"))
        return found

    def edit_via_column_insert(window, text):
        """用"矩形选区 + Alt+C + 列块插入"造出未保存的改动。"""
        scinti = visible_scintilla(window)
        if scinti is None:
            return None
        user32.SendMessageW(scinti, SCI_SETSELECTIONMODE, SC_SEL_RECTANGLE, 0)
        user32.SendMessageW(scinti, SCI_SETRECTANGULARSELECTIONANCHOR, 1, 0)
        user32.SendMessageW(scinti, SCI_SETRECTANGULARSELECTIONCARET, 11, 0)
        time.sleep(0.3)
        user32.PostMessageW(window, WM_COMMAND, CMD_COLUMN_EDIT, 0)
        time.sleep(1.5)
        dialog = user32.FindWindowW("notepadccColumnDialog", None)
        if not dialog:
            return None
        set_text(dialog, COLUMN_TEMPLATE_EDIT, text)
        user32.PostMessageW(dialog, WM_COMMAND, COLUMN_INSERT_BUTTON, 0)
        time.sleep(1.0)
        return scinti

    # ---- 1) 定时快照：硬杀进程后仍能恢复 ----
    reset("aaa\nbbb\nccc")
    proc = subprocess.Popen([exe, MULTI_INSTANCE, "-portable", path], cwd=root)
    window = wait_for_main_window("notepadcc - doc.txt")
    try:
        if not window:
            print("  FAIL: 找不到主窗口（定时快照）")
            return False
        if user32.FindWindowW("#32770", recovery_title):
            print("  FAIL: 没有任何快照时不该弹恢复提示")
            return False
        scinti = edit_via_column_insert(window, "T")
        if scinti is None:
            print("  FAIL: 没能通过列块插入造出改动")
            return False
        expected = "aTaa\nbTbb\ncTcc"
        if scinti_text(scinti) != expected:
            print(f"  FAIL: 改动后的内容不对 {scinti_text(scinti)!r}")
            return False

        deadline = time.time() + 25
        while time.time() < deadline and not snapshot_files():
            time.sleep(0.5)
        written = snapshot_files()
        if not written:
            print("  FAIL: 等了 25 秒也没写出定时快照")
            return False
    finally:
        if proc.poll() is None:
            proc.kill()          # 硬杀：不给任何收尾机会，模拟任务管理器结束进程
        time.sleep(1.0)

    proc = subprocess.Popen([exe, MULTI_INSTANCE, "-portable", path], cwd=root)
    window = wait_for_main_window("notepadcc - doc.txt")
    try:
        if not window:
            print("  FAIL: 重启后找不到主窗口")
            return False
        if not dismiss_dialog(recovery_title, IDYES, timeout=15.0):
            print("  FAIL: 硬杀之后没有弹出恢复提示")
            return False
        time.sleep(1.5)
        scinti = visible_scintilla(window)
        if scinti is None:
            print("  FAIL: 恢复后找不到编辑控件")
            return False
        if scinti_text(scinti) != expected:
            print(f"  FAIL: 恢复出来的内容不对 {scinti_text(scinti)!r}，期望 {expected!r}")
            return False
        if snapshot_files():
            print("  FAIL: 恢复之后快照应当已被消费掉")
            return False
    finally:
        if window:
            user32.PostMessageW(window, WM_CLOSE, 0, 0)
            time.sleep(1.5)
        if proc.poll() is None:
            proc.kill()

    # ---- 2) 崩溃抢救：异常退出时把未保存内容写进 crash 目录 ----
    reset("aaa\nbbb\nccc")
    crash_dir = None
    proc = subprocess.Popen([exe, MULTI_INSTANCE, "-portable", "--crash-test", path], cwd=root)
    window = wait_for_main_window("notepadcc - doc.txt", timeout=40.0)
    try:
        if not window:
            print("  FAIL: 自检开关下没能启动出窗口")
            return False
        if edit_via_column_insert(window, "C") is None:
            print("  FAIL: 崩溃前没能造出改动")
            return False
        if not dismiss_dialog(crash_title, IDOK, timeout=30.0):
            print("  FAIL: 崩溃处理器没有弹出提示（说明异常过滤器没接上）")
            return False
        deadline = time.time() + 15
        while time.time() < deadline and proc.poll() is None:
            time.sleep(0.3)
        if proc.poll() is None:
            print("  FAIL: 崩溃之后进程没有退出")
            return False
    finally:
        if proc.poll() is None:
            proc.kill()

    if not os.path.isdir(backup):
        print("  FAIL: 没有生成 backup 目录")
        return False
    dirs = [d for d in os.listdir(backup) if d.startswith("crash_")]
    if len(dirs) != 1:
        print(f"  FAIL: 崩溃目录应恰好 1 个，实际 {dirs}")
        return False
    crash_dir = os.path.join(backup, dirs[0])
    report_path = os.path.join(crash_dir, "crash.txt")
    if not os.path.exists(report_path):
        print("  FAIL: 没有写出 crash.txt")
        return False
    with open(report_path, "r", encoding="utf-8", errors="replace") as handle:
        report = handle.read()
    if "异常代码: 0xC0000005" not in report:
        print(f"  FAIL: crash.txt 里没有访问违例记录\n{report[:400]}")
        return False
    if "抢救成功 1 份" not in report:
        print(f"  FAIL: crash.txt 显示没有抢救到内容\n{report[:600]}")
        return False

    dumps = [f for f in os.listdir(crash_dir) if f.endswith(".dmp")]
    if len(dumps) != 1:
        print(f"  FAIL: 应恰好 1 个 minidump，实际 {dumps}")
        return False
    with open(os.path.join(crash_dir, dumps[0]), "rb") as handle:
        magic = handle.read(4)
    if magic != b"MDMP":
        print(f"  FAIL: minidump 文件头不是 MDMP，实际 {magic!r}")
        return False

    rescued = [f for f in os.listdir(crash_dir) if f.endswith(".snap")]
    if len(rescued) != 1:
        print(f"  FAIL: 应恰好 1 份抢救出来的内容，实际 {rescued}")
        return False
    with open(os.path.join(crash_dir, rescued[0]), "r", encoding="utf-8",
              errors="replace") as handle:
        rescued_text = handle.read()
    if "aCaa" not in rescued_text:
        print(f"  FAIL: 抢救出来的内容不对\n{rescued_text[:300]}")
        return False

    # ---- 3) 崩溃之后重启，应当提示并恢复 ----
    proc = subprocess.Popen([exe, MULTI_INSTANCE, "-portable", path], cwd=root)
    window = wait_for_main_window("notepadcc - doc.txt")
    try:
        if not window:
            print("  FAIL: 崩溃后重启找不到主窗口")
            return False
        if not dismiss_dialog(recovery_title, IDYES, timeout=15.0):
            print("  FAIL: 崩溃后重启没有提示恢复")
            return False
        time.sleep(1.5)
        scinti = visible_scintilla(window)
        if scinti is None:
            print("  FAIL: 恢复后找不到编辑控件")
            return False
        if scinti_text(scinti) != "aCaa\nbCbb\ncCcc":
            print(f"  FAIL: 崩溃恢复的内容不对 {scinti_text(scinti)!r}")
            return False
    finally:
        if window:
            user32.PostMessageW(window, WM_CLOSE, 0, 0)
            time.sleep(1.5)
        if proc.poll() is None:
            proc.kill()
        if os.path.exists(os.path.join(cfg, "config.xml")):
            os.remove(os.path.join(cfg, "config.xml"))
        shutil.rmtree(backup, ignore_errors=True)

    print("  崩溃快照：定时快照扛住硬杀、崩溃抢救拿到内容与 minidump，两条都恢复了")
    return True


def test_shortcuts(root):
    """快捷键管理：改绑 → 冲突确认 → 落盘 → 真的按键触发命令。

    这一段的关键是最后那句"真的按键触发命令"：跨进程没法伪造 GetKeyState，
    所以用不带修饰键的 F9 / F2 来验证——它们同样走 Windows 的
    TranslateAccelerator 路径，能证明加速键表确实被重建了。

    对话框里"按组合键"的做法是给列表控件发 WM_KEYDOWN（列表被子类化后自己抓键）。
    必须用 PostMessage：抢快捷键会弹出模态确认框，SendMessage 会被它同步挡住，
    脚本就没机会去点确认框了。
    """
    work = os.path.join(root, "build", "shortcut_test")
    os.makedirs(work, exist_ok=True)
    path = os.path.join(work, "doc.txt")
    with open(path, "w", encoding="utf-8", newline="") as handle:
        handle.write("one\ntwo\nthree\n")

    config_dir = os.path.join(root, "build", "bin", "config")
    shortcut_file = os.path.join(config_dir, "shortcuts.xml")
    for leftover in (shortcut_file, os.path.join(config_dir, "config.xml"),
                     os.path.join(config_dir, "session.xml")):
        if os.path.exists(leftover):
            os.remove(leftover)

    exe = os.path.join(root, "build", "bin", "notepadcc.exe")
    proc = subprocess.Popen([exe, MULTI_INSTANCE, "-portable", path], cwd=root)
    window = wait_for_main_window("notepadcc - doc.txt")
    try:
        if not window:
            print("  FAIL: 找不到打开了 doc.txt 的窗口")
            return False
        scinti = visible_scintilla(window)
        if scinti is None:
            print("  FAIL: 找不到可见的编辑控件")
            return False

        def margin():
            return user32.SendMessageW(scinti, SCI_GETMARGINWIDTHN, 0, 0)

        def open_dialog():
            user32.PostMessageW(window, WM_COMMAND, CMD_SHORTCUTS, 0)
            deadline = time.time() + 8
            while time.time() < deadline:
                dialog = user32.FindWindowW("notepadccShortcutDialog", None)
                if dialog:
                    time.sleep(0.6)
                    return dialog
                time.sleep(0.2)
            return None

        def close_dialog(dialog, command_id):
            user32.PostMessageW(dialog, WM_COMMAND, command_id, 0)
            deadline = time.time() + 8
            while time.time() < deadline:
                if not user32.FindWindowW("notepadccShortcutDialog", None):
                    time.sleep(0.4)
                    return True
                time.sleep(0.2)
            print("  FAIL: 对话框没有按预期关闭")
            return False

        def filter_to(dialog, text):
            set_text(dialog, SH_FILTER_EDIT, text)
            time.sleep(0.6)
            return user32.SendMessageW(user32.GetDlgItem(dialog, SH_LIST), LVM_GETITEMCOUNT, 0, 0)

        def status_of(dialog):
            return window_text(user32.GetDlgItem(dialog, SH_STATUS))

        def press_in_list(dialog, vk):
            user32.PostMessageW(user32.GetDlgItem(dialog, SH_LIST), WM_KEYDOWN, vk, 0)
            time.sleep(0.7)

        def press_in_editor(vk):
            user32.PostMessageW(window, WM_KEYDOWN, vk, 0)
            time.sleep(0.9)

        if margin() <= 0:
            print("  FAIL: 默认应显示行号")
            return False

        # ---- 1) 打开、筛选、改绑、取消要能完整回滚 ----
        dialog = open_dialog()
        if not dialog:
            print("  FAIL: 管理快捷键对话框没有出现")
            return False
        if window_text(dialog) != "管理快捷键":
            print(f"  FAIL: 对话框标题不对 {window_text(dialog)!r}")
            return False
        total = user32.SendMessageW(user32.GetDlgItem(dialog, SH_LIST), LVM_GETITEMCOUNT, 0, 0)
        if total < 30:
            print(f"  FAIL: 命令列表只有 {total} 行，太少")
            return False
        if filter_to(dialog, "显示行号") != 1:
            print("  FAIL: 按“显示行号”筛选应只剩 1 行")
            return False
        press_in_list(dialog, VK_F9)          # F9 没被占用，不该弹确认框
        status = status_of(dialog)
        if "显示行号" not in status or "F9" not in status:
            print(f"  FAIL: 改绑之后状态栏应给出反馈，实际 {status!r}")
            return False
        if not close_dialog(dialog, SH_CANCEL):
            return False
        press_in_editor(VK_F9)
        if margin() != 48:
            print("  FAIL: 取消之后 F9 不该生效（回滚没做成）")
            return False
        if os.path.exists(shortcut_file):
            print("  FAIL: 取消不该写出 shortcuts.xml")
            return False

        # ---- 2) 冲突：抢 F2（默认属于“标记/取消书签”）----
        dialog = open_dialog()
        if not dialog:
            print("  FAIL: 第二次没能打开对话框")
            return False
        if filter_to(dialog, "显示行号") != 1:
            print("  FAIL: 第二次筛选失败")
            return False
        press_in_list(dialog, VK_F2)
        if not dismiss_dialog("快捷键冲突", IDYES):
            print("  FAIL: 抢用已被占用的快捷键时没有弹出冲突确认")
            return False
        time.sleep(0.6)
        status = status_of(dialog)
        if "显示行号" not in status or "F2" not in status:
            print(f"  FAIL: 抢用之后状态栏应说明改绑结果，实际 {status!r}")
            return False
        if not close_dialog(dialog, SH_OK):
            return False
        if not os.path.exists(shortcut_file):
            print("  FAIL: 点确定之后没有写出 shortcuts.xml")
            return False

        with open(shortcut_file, "r", encoding="utf-8", errors="replace") as handle:
            xml = handle.read()

        def node_of(text, name):
            for chunk in text.split("<Shortcut ")[1:]:
                fragment = chunk.split("/>")[0]
                if f'name="{name}"' in fragment:
                    return fragment
            return None

        node = node_of(xml, "cmd.view.toggleNumbers")
        if node is None or 'Key="113"' not in node:      # VK_F2 == 113
            print(f"  FAIL: 「显示行号」应记录成 F2，实际 {node!r}")
            return False
        # F2 的默认主人是「下一个书签」（Ctrl+F2 才是标记书签），被抢的应该是它
        node = node_of(xml, "cmd.search.nextBookmark")
        if node is None or 'Key="0"' not in node:
            print(f"  FAIL: 被抢走快捷键的「下一个书签」应记成 Key=\"0\"，实际 {node!r}")
            return False

        press_in_editor(VK_F2)
        if margin() != 0:
            print(f"  FAIL: 保存后按 F2 应关掉行号，边距宽 {margin()}")
            return False
        press_in_editor(VK_F2)
        if margin() != 48:
            print("  FAIL: 再按一次 F2 应把行号开回来")
            return False

        # ---- 3) 全部清除：连默认方案一起清掉 ----
        dialog = open_dialog()
        if not dialog:
            print("  FAIL: 第三次没能打开对话框")
            return False
        user32.PostMessageW(dialog, WM_COMMAND, SH_CLEAR_ALL, 0)
        time.sleep(0.8)
        if not close_dialog(dialog, SH_OK):
            return False
        press_in_editor(VK_F2)
        if margin() != 48:
            print("  FAIL: 全部清除之后 F2 不该再触发任何命令")
            return False

        # ---- 4) 恢复默认：F2 应该重新属于「下一个书签」----
        dialog = open_dialog()
        if not dialog:
            print("  FAIL: 第四次没能打开对话框")
            return False
        user32.PostMessageW(dialog, WM_COMMAND, SH_RESTORE, 0)
        time.sleep(0.8)
        if not close_dialog(dialog, SH_OK):
            return False

        with open(shortcut_file, "r", encoding="utf-8", errors="replace") as handle:
            restored = handle.read()
        node = node_of(restored, "cmd.search.nextBookmark")
        if node is None or 'Ctrl="no"' not in node or 'Key="113"' not in node:
            print(f"  FAIL: 恢复默认之后「下一个书签」应重新是 F2，实际 {node!r}")
            return False
        node = node_of(restored, "cmd.view.toggleNumbers")
        if node is None or 'Key="0"' not in node:
            print(f"  FAIL: 恢复默认之后「显示行号」应回到未绑定，实际 {node!r}")
            return False

        # 真的按一次 F2：应当跳到书签所在行（而不是再去切换行号）
        user32.SendMessageW(window, WM_COMMAND, CMD_CLEAR_BOOKMARKS, 0)
        time.sleep(0.3)
        user32.SendMessageW(scinti, SCI_GOTOLINE, 3, 0)
        time.sleep(0.2)
        user32.SendMessageW(window, WM_COMMAND, CMD_TOGGLE_BOOKMARK, 0)
        time.sleep(0.3)
        user32.SendMessageW(scinti, SCI_GOTOLINE, 0, 0)
        time.sleep(0.2)
        press_in_editor(VK_F2)
        line = current_line(scinti)
        if line != 3:
            print(f"  FAIL: 恢复默认之后 F2 应跳到书签行（第 3 行），实际第 {line} 行")
            return False
        if margin() != 48:
            print("  FAIL: 恢复默认之后 F2 不该再切换行号")
            return False

        print("  快捷键：筛选 / 改绑 / 冲突抢占 / 取消回滚 / 全部清除 / 恢复默认，"
              "且加速键表真的跟着变了")
        return True
    finally:
        if window:
            user32.PostMessageW(window, WM_CLOSE, 0, 0)
            time.sleep(1.5)
        if proc.poll() is None:
            proc.kill()
        # 这一段会写出 shortcuts.xml 与 config.xml，留着会污染后续会话
        for leftover in (shortcut_file, os.path.join(config_dir, "config.xml")):
            if os.path.exists(leftover):
                os.remove(leftover)


def test_preferences(root):
    """首选项：开对话框 → 填非法值应被挡下 → 改成合法值 → 核对视图与磁盘配置。

    这一段刻意不只看"对话框弹出来了没有"：它要确认改动真的穿透到了
    Scintilla（行号边距宽度）并且落到了 config/config.xml。
    切页的 TCN_SELCHANGE 走的是 WM_NOTIFY（lParam 是指针），跨进程不封送，
    所以这里改为直接校验"打开时常规页可见、编辑页隐藏"，再直接操作控件。
    """
    work = os.path.join(root, "build", "prefs_test")
    os.makedirs(work, exist_ok=True)
    path = os.path.join(work, "doc.txt")
    with open(path, "w", encoding="utf-8", newline="") as handle:
        handle.write("one\ntwo\nthree\n")

    config_path = os.path.join(root, "build", "bin", "config", "config.xml")
    session = os.path.join(root, "build", "bin", "config", "session.xml")
    for leftover in (config_path, session):
        if os.path.exists(leftover):
            os.remove(leftover)

    exe = os.path.join(root, "build", "bin", "notepadcc.exe")
    proc = subprocess.Popen([exe, MULTI_INSTANCE, "-portable", path], cwd=root)
    window = wait_for_main_window("notepadcc - doc.txt")
    try:
        if not window:
            print("  FAIL: 找不到打开了 doc.txt 的窗口")
            return False
        scinti = visible_scintilla(window)
        if scinti is None:
            print("  FAIL: 找不到可见的编辑控件")
            return False

        margin_before = user32.SendMessageW(scinti, SCI_GETMARGINWIDTHN, 0, 0)
        if margin_before <= 0:
            print(f"  FAIL: 默认应显示行号，行号边距宽 {margin_before}")
            return False

        user32.PostMessageW(window, WM_COMMAND, CMD_PREFERENCES, 0)
        time.sleep(1.0)
        dialog = user32.FindWindowW("notepadccPreferences", None)
        if not dialog:
            print("  FAIL: 首选项对话框没有出现")
            return False
        if window_text(dialog) != "首选项":
            print(f"  FAIL: 对话框标题不对 {window_text(dialog)!r}")
            return False

        tab = user32.GetDlgItem(dialog, PREF_TAB)
        pages = user32.SendMessageW(tab, TCM_GETITEMCOUNT, 0, 0)
        if pages != 4:
            print(f"  FAIL: 应有 4 个分页，实际 {pages}")
            return False
        if not user32.IsWindowVisible(user32.GetDlgItem(dialog, PREF_RESTORE_SESSION)):
            print("  FAIL: 打开时常规页的控件应可见")
            return False
        if user32.IsWindowVisible(user32.GetDlgItem(dialog, PREF_TAB_WIDTH)):
            print("  FAIL: 打开时编辑页的控件不应可见")
            return False

        # 非法值（制表符宽度 0）必须被挡下：弹提示，且对话框不关闭
        if not set_text(dialog, PREF_TAB_WIDTH, "0"):
            print("  FAIL: 找不到制表符宽度输入框")
            return False
        user32.PostMessageW(dialog, WM_COMMAND, PREF_OK, 0)
        if not dismiss_dialog("首选项", IDOK):
            print("  FAIL: 非法值没有弹出提示框")
            return False
        if not user32.FindWindowW("notepadccPreferences", None):
            print("  FAIL: 值非法时对话框不应关闭")
            return False

        # 改成合法值，并顺手关掉行号
        set_text(dialog, PREF_TAB_WIDTH, "3")
        user32.SendMessageW(user32.GetDlgItem(dialog, PREF_SHOW_LINE_NUMBERS), BM_SETCHECK,
                            BST_UNCHECKED, 0)
        user32.PostMessageW(dialog, WM_COMMAND, PREF_OK, 0)
        deadline = time.time() + 5
        while time.time() < deadline and user32.FindWindowW("notepadccPreferences", None):
            time.sleep(0.2)
        if user32.FindWindowW("notepadccPreferences", None):
            print("  FAIL: 点确定之后对话框没有关闭")
            print(f"    制表符宽度输入框现在是 {window_text(user32.GetDlgItem(dialog, PREF_TAB_WIDTH))!r}")
            leftover = user32.FindWindowW("#32770", "首选项")
            if leftover:
                for child in children(leftover):
                    if class_name(child) == "Static" and window_text(child):
                        print(f"    [诊断] 提示框内容 {window_text(child)!r}")
            for cls, text in dump_top_level_windows():
                print(f"    [诊断] 可见顶层窗口 {cls!r}  {text!r}")
            return False
        time.sleep(1.0)

        margin_after = user32.SendMessageW(scinti, SCI_GETMARGINWIDTHN, 0, 0)
        if margin_after != 0:
            print(f"  FAIL: 关掉行号后行号边距应变成 0，实际 {margin_after}")
            return False

        if not os.path.exists(config_path):
            print(f"  FAIL: 没有写出配置文件的落盘 {config_path}")
            return False
        with open(config_path, "r", encoding="utf-8", errors="replace") as handle:
            config_text = handle.read()
        for needle in ('showLineNumbers="no"', 'tabWidth="3"'):
            if needle not in config_text:
                print(f"  FAIL: 配置文件里没有 {needle}\n{config_text}")
                return False

        print("  首选项：4 个分页、非法值被挡下、行号边距 48 → 0、config.xml 已落盘")
        return True
    finally:
        if window:
            user32.PostMessageW(window, WM_CLOSE, 0, 0)
            time.sleep(1.5)
        if proc.poll() is None:
            proc.kill()
        # 这一段把配置改成了"不显示行号"，留着会污染后续会话
        if os.path.exists(config_path):
            os.remove(config_path)


def test_find_bar(main_window, root):
    """查找栏：填词、点下一个，核对编辑器选区是否落在第一个匹配上。"""
    target = os.path.join(root, "README.md")
    with open(target, "r", encoding="utf-8") as handle:
        raw = handle.read().encode("utf-8")
    needle = "notepadcc"

    user32.SendMessageW(main_window, WM_COMMAND, CMD_FIND, 0)
    time.sleep(1)
    find_bar = find_child(main_window, "notepadccFindBar")
    if find_bar is None:
        print("  FAIL: 查找栏没有创建出来")
        return False
    if not user32.IsWindowVisible(find_bar):
        print("  FAIL: 查找栏不可见")
        return False

    if not set_text(find_bar, CTRL_FIND_EDIT, needle):
        print("  FAIL: 找不到查找输入框")
        return False
    time.sleep(0.3)
    user32.SendMessageW(main_window, WM_COMMAND, BTN_FIND_NEXT, 0)
    time.sleep(1.2)

    scinti = visible_scintilla(main_window)
    if scinti is None:
        print("  FAIL: 找不到可见的编辑控件")
        return False

    start = user32.SendMessageW(scinti, SCI_GETSELECTIONSTART, 0, 0)
    end = user32.SendMessageW(scinti, SCI_GETSELECTIONEND, 0, 0)
    selected = raw[start:end].decode("utf-8", errors="replace")
    if selected != needle or start != raw.find(needle.encode("utf-8")):
        print(f"  FAIL: 选区不对，选中 {selected!r} @ {start}")
        return False

    status = None
    for child in children(find_bar):
        if class_name(child) == "Static":
            status = window_text(child)
            break
    if not status or "匹配" not in status:
        print(f"  FAIL: 查找栏状态异常 {status!r}")
        return False
    print(f"  查找栏：命中 {selected!r} @ 字节 {start}，{status}")
    return True


def test_search_panel(main_window, root):
    """在文件中查找：真跑一次后台搜索，核对结果列表与状态。"""
    user32.SendMessageW(main_window, WM_COMMAND, CMD_FIND_IN_FILES, 0)
    time.sleep(1)
    panel = find_child(main_window, "notepadccSearchPanel")
    if panel is None:
        print("  FAIL: 搜索面板没有创建出来")
        return False
    if not user32.IsWindowVisible(panel):
        print("  FAIL: 搜索面板不可见")
        return False

    set_text(panel, PANEL_FIND, "notepadcc")
    set_text(panel, PANEL_DIR, root)
    set_text(panel, PANEL_FILTER, "*.md")
    time.sleep(0.3)

    user32.SendMessageW(main_window, WM_COMMAND, PANEL_START, 0)

    # 等后台搜索跑完：轮询状态文字
    list_view = user32.GetDlgItem(panel, PANEL_LIST)
    if not list_view:
        print("  FAIL: 找不到结果列表")
        return False

    rows = 0
    status = ""
    deadline = time.time() + 30
    while time.time() < deadline:
        time.sleep(0.5)
        rows = user32.SendMessageW(list_view, LVM_GETITEMCOUNT, 0, 0)
        status = ""
        for child in children(panel):
            if class_name(child) == "Static":
                status = window_text(child)
                break
        if status.startswith("完成") or status.startswith("已取消"):
            break

    print(f"  搜索面板：结果 {rows} 行，状态 {status!r}")
    if rows <= 0:
        print("  FAIL: 结果列表为空")
        return False
    if not status.startswith("完成"):
        print("  FAIL: 搜索没有正常收尾")
        return False
    if "命中 0 处" in status:
        print("  FAIL: 状态显示零命中，但列表里有行")
        return False
    return True


def run():
    root = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
    exe = os.path.join(root, "build", "bin", "notepadcc.exe")

    # 清掉上次会话，否则启动时会恢复别的文件、活动文档就不是 README.md 了
    session = os.path.join(root, "build", "bin", "config", "session.xml")
    if os.path.exists(session):
        os.remove(session)

    proc = subprocess.Popen([exe, MULTI_INSTANCE, "-portable", os.path.join(root, "README.md")], cwd=root)
    main_window = wait_for_main_window("notepadcc - README.md")
    failed = False
    try:
        if not main_window:
            print("FAIL: 找不到主窗口")
            return 1
        if not test_find_bar(main_window, root):
            failed = True
        if not test_search_panel(main_window, root):
            failed = True
        if not test_bookmarks(main_window):
            failed = True
        if not test_replace_in_files(main_window, root):
            failed = True
    finally:
        if main_window:
            user32.PostMessageW(main_window, WM_CLOSE, 0, 0)
            time.sleep(2)
        if proc.poll() is None:
            proc.kill()

    # 列编辑另起一个实例：跨进程无法用 SCI_SETTEXT 准备文档
    if not test_column_editing(root):
        failed = True
    if not test_large_file(root):
        failed = True
    if not test_external_change(root):
        failed = True
    if not test_preferences(root):
        failed = True
    if not test_shortcuts(root):
        failed = True
    if not test_snapshots(root):
        failed = True
    if not test_folding(root):
        failed = True
    if not test_split_view(root):
        failed = True
    if not test_encoding_convert(root):
        failed = True
    if not test_single_instance(root):
        failed = True
    if not test_tools_menu(root):
        failed = True
    if not test_mime_binary(root):
        failed = True
    if not test_eye_care(root):
        failed = True
    if not test_language_defs(root):
        failed = True
    if not test_style_scheme(root):
        failed = True
    if not test_udl(root):
        failed = True
    if not test_dark_mode(root):
        failed = True
    if not test_macro(root):
        failed = True
    if not test_workspace(root):
        failed = True
    if not test_function_list(root):
        failed = True
    if not test_export_and_print(root):
        failed = True
    if not test_plugins(root):
        failed = True
    if not test_structured_format(root):
        failed = True
    if not test_run_command(root):
        failed = True
    if not test_ui_language(root):
        failed = True
    if not test_hex_view(root):
        failed = True
    if not test_copy_as_rtf(root):
        failed = True
    if not test_markdown_preview(root):
        failed = True
    if not test_dialog_localization(root):
        failed = True
    if not test_compare(root):
        failed = True
    if not test_json_tree(root):
        failed = True
    if not test_xml_tree(root):
        failed = True
    if not test_xpath_query(root):
        failed = True
    if not test_brace_summary_fullscreen(root):
        failed = True
    if not test_toolbar(root):
        failed = True
    if not test_context_menu(root):
        failed = True
    if not test_auto_complete(root):
        failed = True
    if not test_associated_commands(root):
        failed = True
    if not test_portable_package(root):
        failed = True

    if failed:
        print("FAIL: 界面回归未通过")
        return 1
    print("PASS: 界面回归通过（查找栏 + 在文件中查找 + 书签 + 在文件中替换 + 列编辑 + "
          "大文件 + 外部改动 + 首选项 + 快捷键 + 崩溃快照 + 代码折叠 + 分屏克隆 + "
          "编码转换 + 单实例 + 工具菜单 + 语言定义 + 样式方案 + UDL + 暗色模式 + "
          "宏 + 文件夹工作区 + 函数列表 + 导出与打印 + 插件宿主 + JSON/XML 格式化 + "
          "运行命令 + 界面语言 + 十六进制查看 + 复制为富文本 + Markdown 预览 + 对话框文案 + "
          "比较 + JSON 树视图 + 括号匹配与摘要与全屏 + 工具栏 + 右键菜单定制 + 自动完成 + "
          "文件关联 + "
          "文件类型识别与 MIME 工具 + 护眼模式 + XML 树视图与校验 + XPath 查询 + 绿色打包）")
    return 0


def test_structured_format(root):
    """JSON / XML 格式化：真的排版、非法内容真的被挡下、被挡下时一个字节都没动。

    这一段不满足于"命令发出去了"：每个用例都核对全文。JSON 还额外核对"值一个字节
    都没变"（\\u4e2d 不被换成真汉字、1.500 不被写成 1.5），XML 核对混合内容
    `<p>hello <b>w</b></p>` 整段原样——这两条是这一块功能的底线。
    """
    work = os.path.join(root, "build", "format_test")
    os.makedirs(work, exist_ok=True)
    cfg = os.path.join(root, "build", "bin", "config")
    exe = os.path.join(root, "build", "bin", "notepadcc.exe")

    def clean_config():
        for name in ("config.xml", "session.xml"):
            target = os.path.join(cfg, name)
            if os.path.exists(target):
                os.remove(target)

    def write_case(name, text):
        path = os.path.join(work, name)
        with open(path, "w", encoding="utf-8", newline="") as handle:
            handle.write(text)
        return path, text

    def launch(name, path):
        clean_config()
        proc = subprocess.Popen([exe, MULTI_INSTANCE, "-portable", path], cwd=root)
        return proc, wait_for_main_window(f"notepadcc - {name}")

    def shutdown(proc, window):
        if window:
            user32.PostMessageW(window, WM_CLOSE, 0, 0)
            dismiss_dialog("notepadcc", IDNO, timeout=2.0, quiet=True)
            time.sleep(1.5)
        if proc.poll() is None:
            proc.kill()

    # ---- 1. JSON 美化：内容对得上，而且一次撤销就回到原文 ----
    compact = '{"a":1,"b":[1,2],"c":{"d":true}}'
    pretty = ('{\n  "a": 1,\n  "b": [\n    1,\n    2\n  ],\n'
              '  "c": {\n    "d": true\n  }\n}')
    path, _ = write_case("sample.json", compact)
    proc, window = launch("sample.json", path)
    try:
        if not window:
            print("  FAIL: 找不到主窗口（sample.json）")
            return False
        scinti = visible_scintilla(window)
        if scinti_text(scinti) != compact:
            print(f"  FAIL: 初始内容不对 {scinti_text(scinti)!r}")
            return False
        user32.PostMessageW(window, WM_COMMAND, CMD_TOOL_FORMAT_JSON, 0)
        time.sleep(0.8)
        actual = scinti_text(scinti)
        if actual != pretty:
            print(f"  FAIL: 格式化 JSON 之后内容不对\n           实际 {actual!r}\n"
                  f"           期望 {pretty!r}")
            return False
        # 原文末尾没有换行，格式化之后也不该凭空多一个（被格式化的正是整篇）
        if actual.endswith("\n"):
            print("  FAIL: 原文没有结尾换行，格式化后不该补一个")
            return False
        user32.PostMessageW(window, WM_COMMAND, CMD_EDIT_UNDO, 0)
        time.sleep(0.6)
        if scinti_text(scinti) != compact:
            print(f"  FAIL: 一次撤销应当回到原文，实际 {scinti_text(scinti)!r}")
            return False
    finally:
        shutdown(proc, window)

    # ---- 2. JSON 压缩：连值一起核对，确认只动了空白 ----
    spaced = '{ "u" : "\\u4e2d" , "n" : 1.500 }'
    minified = '{"u":"\\u4e2d","n":1.500}'
    path, _ = write_case("spaced.json", spaced)
    proc, window = launch("spaced.json", path)
    try:
        if not window:
            print("  FAIL: 找不到主窗口（spaced.json）")
            return False
        scinti = visible_scintilla(window)
        user32.PostMessageW(window, WM_COMMAND, CMD_TOOL_MINIFY_JSON, 0)
        time.sleep(0.8)
        actual = scinti_text(scinti)
        if actual != minified:
            print(f"  FAIL: 压缩 JSON 之后内容不对\n           实际 {actual!r}\n"
                  f"           期望 {minified!r}")
            return False
    finally:
        shutdown(proc, window)

    # ---- 3. XML 美化：外层重排、混合内容整段不动 ----
    xml_source = "<root><p>hello <b>w</b></p><a>1</a></root>\n"
    xml_pretty = "<root>\n  <p>hello <b>w</b></p>\n  <a>1</a>\n</root>\n"
    path, _ = write_case("doc.xml", xml_source)
    proc, window = launch("doc.xml", path)
    try:
        if not window:
            print("  FAIL: 找不到主窗口（doc.xml）")
            return False
        scinti = visible_scintilla(window)
        if scinti_text(scinti) != xml_source:
            print(f"  FAIL: 初始内容不对 {scinti_text(scinti)!r}")
            return False
        user32.PostMessageW(window, WM_COMMAND, CMD_TOOL_FORMAT_XML, 0)
        time.sleep(0.8)
        actual = scinti_text(scinti)
        if actual != xml_pretty:
            print(f"  FAIL: 美化 XML 之后内容不对\n           实际 {actual!r}\n"
                  f"           期望 {xml_pretty!r}")
            return False
        if "<p>hello <b>w</b></p>" not in actual:
            print("  FAIL: 混合内容被改动了（应当是整段原样搬运）")
            return False
    finally:
        shutdown(proc, window)

    # ---- 4. 非法内容：弹说明、内容一个字节都不动 ----
    bad_cases = [
        ("broken.json", '{"a":1,}\n', CMD_TOOL_FORMAT_JSON, "格式化 JSON"),
        ("broken.xml", "<a><b></a></b>\n", CMD_TOOL_FORMAT_XML, "美化 XML"),
    ]
    for name, text, command_id, dialog_title in bad_cases:
        path, _ = write_case(name, text)
        proc, window = launch(name, path)
        try:
            if not window:
                print(f"  FAIL: 找不到主窗口（{name}）")
                return False
            user32.PostMessageW(window, WM_COMMAND, command_id, 0)
            if not dismiss_dialog(dialog_title, IDOK, timeout=8.0):
                print(f"  FAIL: {name} 内容不合法，应当弹出说明而不是静默处理")
                return False
            time.sleep(0.5)
            scinti = visible_scintilla(window)
            if scinti_text(scinti) != text:
                print(f"  FAIL: {name} 报错之后内容不该被改动，实际 "
                      f"{scinti_text(scinti)!r}")
                return False
            if user32.SendMessageW(scinti, SCI_GETMODIFY, 0, 0) != 0:
                print(f"  FAIL: {name} 只是报了错，不该把文档标成已修改")
                return False
        finally:
            shutdown(proc, window)

    print("  JSON/XML 格式化：美化与压缩结果正确（值一个字节没动）、一次撤销回到原文、"
          "混合内容整段原样、非法内容弹说明且文档未改动")
    return True


def menu_texts(window):
    """把主菜单每个弹出菜单的**第一项**文本读回来（"首个可见项"是各菜单的代表）。

    跨进程读菜单文本是可行的：菜单对象在系统内核里，GetMenuStringW 由内核按调用
    线程的上下文把字符串拷进我们的缓冲区，不涉及"指针参数封送"那类问题。
    读的是**真正挂在窗口上的菜单**，所以它反映的是程序此刻的界面语言，
    而不是配置文件里写了什么——这一点是这一段的核心。
    """
    menu = user32.GetMenu(window)
    count = user32.GetMenuItemCount(menu)
    out = []
    for index in range(count):
        popup = user32.GetSubMenu(menu, index)
        buffer = ctypes.create_unicode_buffer(256)
        user32.GetMenuStringW(popup, 0, buffer, 256, MF_BYPOSITION)
        out.append(buffer.value)
    return out


def menu_bar_titles(window):
    menu = user32.GetMenu(window)
    count = user32.GetMenuItemCount(menu)
    out = []
    for index in range(count):
        buffer = ctypes.create_unicode_buffer(256)
        user32.GetMenuStringW(menu, index, buffer, 256, MF_BYPOSITION)
        out.append(buffer.value)
    return out


def menu_items(window, index):
    """某个弹出菜单里的全部可见项文本（分隔线读出来是空串）。"""
    menu = user32.GetMenu(window)
    popup = user32.GetSubMenu(menu, index)
    count = user32.GetMenuItemCount(popup)
    out = []
    for position in range(count):
        buffer = ctypes.create_unicode_buffer(512)
        user32.GetMenuStringW(popup, position, buffer, 512, MF_BYPOSITION)
        out.append(buffer.value)
    return out


def has_cjk(text):
    """有没有中日韩文字或全角标点。用来核对"英文界面里不该冒出中文"。"""
    for ch in text:
        if "\u3000" <= ch <= "\u9fff" or "\uff00" <= ch <= "\uffef":
            return True
    return False

def wait_menu_first_text(window, expected, timeout=6.0):
    """等主菜单第一个弹出菜单的第一项变成某个前缀（判据是"挂在窗口上的菜单"，
    不是配置文件里写了什么——配置写对了而界面没换正是这类功能最容易出的问题）。"""
    deadline = time.time() + timeout
    while time.time() < deadline:
        texts = menu_texts(window)
        if texts and texts[0].startswith(expected):
            return True
        time.sleep(0.2)
    return False


def dialog_texts(dialog):
    """把一个自建对话框里所有子控件的文本读出来（跨进程发 WM_GETTEXT，系统消息会封送）。

    页签（Tab）里的标题读不到——那要 TCM_GETITEM 传指针，跨进程拿不到；所以这一段
    核对的是标签、复选框、输入框与按钮，页签名由单元测试与人工核对兜着。
    """
    found = []
    for child in children(dialog):
        text = window_text(child)
        if text:
            found.append(text)
    return found


def test_json_tree(root):
    """JSON 树视图：面板里按层级列出结构、点容器折叠、点标量跳到源码那一行。

    点击位置固定是**第一行**（y=1）：面板的命中测试是 `topLine + y/行高`，y=1 必然落在
    第一行上，所以不需要知道行高就能确定点到了谁。第一行永远是根，"点一下折叠、再点一下
    展开"因此是确定的。跳跃那条用一个"第一个节点就是标量"的文件（顶层数字 + 几个空行），
    点第一行就能看出光标被挪回了第 1 行。
    """
    work = os.path.join(root, "build", "json_test")
    shutil.rmtree(work, ignore_errors=True)
    os.makedirs(work)
    cfg = os.path.join(root, "build", "bin", "config")
    exe = os.path.join(root, "build", "bin", "notepadcc.exe")

    nestedPath = os.path.join(work, "nested.json")
    nestedText = ('{\n  "name": "npp",\n  "nested": {\n    "n": 1,\n'
                  '    "arr": [true, null, "x"]\n  },\n  "empty": {}\n}\n')
    with open(nestedPath, "w", encoding="utf-8", newline="") as handle:
        handle.write(nestedText)
    scalarPath = os.path.join(work, "scalar.json")
    with open(scalarPath, "w", encoding="utf-8", newline="") as handle:
        handle.write("42\n\n\n\n")

    def clean_config():
        for name in ("config.xml", "session.xml"):
            target = os.path.join(cfg, name)
            if os.path.exists(target):
                os.remove(target)

    clean_config()
    log_file = remove_log_with_retry(root)

    def panel_lines(window, timeout=8.0):
        """等 JSON 树面板出现并读回它渲染的那几行。"""
        deadline = time.time() + timeout
        while time.time() < deadline:
            panel = find_child(window, STRUCTURE_TREE_CLASS)
            if panel and user32.IsWindowVisible(panel):
                text = window_text(panel)
                if text:
                    return panel, text.split("\r\n")
            time.sleep(0.2)
        return None, []

    def caret_line(window):
        scinti = visible_scintilla(window)
        position = user32.SendMessageW(scinti, SCI_GETCURRENTPOS, 0, 0)
        return user32.SendMessageW(scinti, SCI_LINEFROMPOSITION, position, 0)

    def click_first_row(panel):
        user32.PostMessageW(panel, WM_LBUTTONDOWN, 0, (1 << 16) | 1)

    # ---- 1) 结构与折叠 ----
    proc = subprocess.Popen([exe, MULTI_INSTANCE, "-portable", nestedPath], cwd=root)
    window = wait_for_main_window("notepadcc - nested.json")
    try:
        if not window:
            print("  FAIL: 找不到主窗口")
            return False
        time.sleep(0.8)
        user32.PostMessageW(window, WM_COMMAND, CMD_VIEW_JSON_TREE, 0)
        panel, lines = panel_lines(window)
        if not panel:
            print("  FAIL: 打开 JSON 树之后没看到那个控件")
            return False
        expected = [
            "v {",
            '    "name": "npp"',
            '  v "nested": {',
            '      "n": 1',
            '    v "arr": [',
            "        true",
            "        null",
            '        "x"',
            '  v "empty": {',   # empty 与 nested 同级（都是根的键），所以缩进一样
        ]
        if lines != expected:
            print(f"  FAIL: 树的结构不对\n           实际 {lines}\n           期望 {expected}")
            return False

        click_first_row(panel)
        time.sleep(0.6)
        _, collapsed = panel_lines(window)
        if collapsed != ["> { 3 }"]:
            print(f"  FAIL: 点根应当折叠成一行摘要，实际 {collapsed}")
            return False
        click_first_row(panel)
        time.sleep(0.6)
        _, expanded = panel_lines(window)
        if expanded != expected:
            print(f"  FAIL: 再点一次应当展开回来，实际 {expanded[:3]}…")
            return False
    finally:
        if window:
            user32.PostMessageW(window, WM_CLOSE, 0, 0)
            dismiss_dialog("notepadcc", IDNO, timeout=2.0, quiet=True)
            time.sleep(1.5)
        if proc.poll() is None:
            proc.kill()

    # ---- 2) 点标量跳到源码那一行；内容不合法时面板里说清原因 ----
    clean_config()
    proc = subprocess.Popen([exe, MULTI_INSTANCE, "-portable", scalarPath], cwd=root)
    window = wait_for_main_window("notepadcc - scalar.json")
    try:
        if not window:
            print("  FAIL: 找不到第二个实例的主窗口")
            return False
        time.sleep(0.8)
        user32.PostMessageW(window, WM_COMMAND, CMD_VIEW_JSON_TREE, 0)
        panel, lines = panel_lines(window)
        if not panel or lines[0] != "  42":
            print(f"  FAIL: 顶层标量应当渲染成一行，实际 {lines}")
            return False
        # 先把光标挪到文末，再点树里的那一行
        scinti = visible_scintilla(window)
        user32.SendMessageW(scinti, SCI_GOTOPOS, 6, 0)   # "42\n\n\n\n" 共六个字节
        time.sleep(0.3)
        if caret_line(window) == 0:
            print("  FAIL: 光标没有先挪开，这条就测不出跳跃")
            return False
        click_first_row(panel)
        time.sleep(0.6)
        if caret_line(window) != 0:
            print(f"  FAIL: 点标量应当跳到源码第 1 行，实际第 {caret_line(window) + 1} 行")
            return False

        # 把文档改坏 → 面板里应当说明"不是合法的 JSON"（错误信息含行列）
        user32.SendMessageW(scinti, SCI_GOTOPOS, 0, 0)
        user32.PostMessageW(scinti, WM_CHAR_INPUT, ord("{"), 0)
        time.sleep(1.2)   # 刷新是节流的（300 ms），等宽裕一点
        _, broken = panel_lines(window)
        if not broken or "不是合法的 JSON" not in broken[0]:
            print(f"  FAIL: 内容不合法时面板里应当说明原因，实际 {broken[:2]}")
            return False
        if "第 1 行" not in broken[0]:
            print(f"  FAIL: 错误信息里应当带行列，实际 {broken[0]!r}")
            return False
    finally:
        if window:
            user32.PostMessageW(window, WM_CLOSE, 0, 0)
            dismiss_dialog("notepadcc", IDNO, timeout=2.0, quiet=True)
            time.sleep(1.5)
        if proc.poll() is None:
            proc.kill()
        clean_config()

    print("  JSON 树视图：按层级列出结构（缩进/折叠记号/键名/值逐行核对）、"
          "点容器折叠与展开、点标量跳到源码那一行、内容不合法时说明原因（含行列）")
    return True


def test_xml_tree(root):
    """XML 树视图与"校验 XML"（M3 第七块）：结构逐行核对、折叠/展开、点行跳转、
    与 JSON 树共用同一块面板、校验成功报规模、失败弹说明且文档一个字节不动。

    与 test_json_tree 同一套手法：面板是自绘的，渲染内容靠 WM_GETTEXT 读回；
    点击位置固定 y=1（命中"此刻第一行"），所以折叠/跳转的判据都是确定的。
    树的结构用嵌套文档核对；跳转用"根就是空元素"的文档——第一行不是容器，
    点它才会走"跳到源码行"这条路而不是折叠。
    """
    work = os.path.join(root, "build", "xml_test")
    shutil.rmtree(work, ignore_errors=True)
    os.makedirs(work)
    cfg = os.path.join(root, "build", "bin", "config")
    exe = os.path.join(root, "build", "bin", "notepadcc.exe")

    nestedPath = os.path.join(work, "nested.xml")
    nestedText = '<root id="1">\n  <item>one</item>\n  <empty/>\n</root>\n'
    with open(nestedPath, "w", encoding="utf-8", newline="") as handle:
        handle.write(nestedText)
    simplePath = os.path.join(work, "simple.xml")
    with open(simplePath, "w", encoding="utf-8", newline="") as handle:
        handle.write("<a/>\n\n\n\n")

    def clean_config():
        for name in ("config.xml", "session.xml"):
            target = os.path.join(cfg, name)
            if os.path.exists(target):
                os.remove(target)

    clean_config()
    remove_log_with_retry(root)

    def panel_lines(window, timeout=8.0):
        """等结构树面板出现并读回它渲染的那几行。"""
        deadline = time.time() + timeout
        while time.time() < deadline:
            panel = find_child(window, STRUCTURE_TREE_CLASS)
            if panel and user32.IsWindowVisible(panel):
                text = window_text(panel)
                if text:
                    return panel, text.split("\r\n")
            time.sleep(0.2)
        return None, []

    def caret_line(window):
        scinti = visible_scintilla(window)
        position = user32.SendMessageW(scinti, SCI_GETCURRENTPOS, 0, 0)
        return user32.SendMessageW(scinti, SCI_LINEFROMPOSITION, position, 0)

    def click_first_row(panel):
        user32.PostMessageW(panel, WM_LBUTTONDOWN, 0, (1 << 16) | 1)

    # ---- 1) 结构 / 折叠展开 / 与 JSON 树共用面板 / 校验成功报规模 ----
    proc = subprocess.Popen([exe, MULTI_INSTANCE, "-portable", nestedPath], cwd=root)
    window = wait_for_main_window("notepadcc - nested.xml")
    try:
        if not window:
            print("  FAIL: 找不到主窗口（nested.xml）")
            return False
        time.sleep(0.8)
        user32.PostMessageW(window, WM_COMMAND, CMD_VIEW_XML_TREE, 0)
        panel, lines = panel_lines(window)
        if not panel:
            print("  FAIL: 打开 XML 树之后没看到那块面板")
            return False
        expected = [
            "v <root>",
            '    @id = "1"',
            "  v <item>",
            '      "one"',
            "    <empty>",
        ]
        if lines != expected:
            print(f"  FAIL: 树的结构不对\n           实际 {lines}\n           期望 {expected}")
            return False

        click_first_row(panel)
        time.sleep(0.6)
        _, collapsed = panel_lines(window)
        if collapsed != ["> <root ... 3 项>"]:
            print(f"  FAIL: 点根应当折叠成一行摘要，实际 {collapsed}")
            return False
        click_first_row(panel)
        time.sleep(0.6)
        _, expanded = panel_lines(window)
        if expanded != expected:
            print(f"  FAIL: 再点一次应当展开回来，实际 {expanded[:3]}…")
            return False

        # 打开 JSON 树：同一块面板换内容（这份 XML 不是合法 JSON，面板如实说明原因）；
        # 再切回 XML 树，结构原样回来
        user32.PostMessageW(window, WM_COMMAND, CMD_VIEW_JSON_TREE, 0)
        time.sleep(1.2)
        _, as_json = panel_lines(window)
        if not as_json or "不是合法的 JSON" not in as_json[0]:
            print(f"  FAIL: 打开 JSON 树后同一块面板应当换内容，实际 {as_json[:2]}")
            return False
        user32.PostMessageW(window, WM_COMMAND, CMD_VIEW_XML_TREE, 0)
        time.sleep(1.2)
        _, back = panel_lines(window)
        if back != expected:
            print(f"  FAIL: 切回 XML 树应当恢复结构，实际 {back[:3]}…")
            return False

        # 校验 XML：合法 → 状态栏报行数与节点数
        user32.PostMessageW(window, WM_COMMAND, CMD_TOOL_VALIDATE_XML, 0)
        time.sleep(0.8)
        message = read_status_bar(window)
        if not message or "XML 合法" not in message:
            print(f"  FAIL: 校验 XML 应当在状态栏报结果，实际 {message!r}")
            return False
        if "4 行" not in message or "5 个节点" not in message:
            print(f"  FAIL: 校验结果的规模不对（期望 4 行 5 个节点），实际 {message!r}")
            return False
    finally:
        if window:
            user32.PostMessageW(window, WM_CLOSE, 0, 0)
            dismiss_dialog("notepadcc", IDNO, timeout=2.0, quiet=True)
            time.sleep(1.5)
        if proc.poll() is None:
            proc.kill()
        clean_config()

    # ---- 2) 点元素跳到源码行；改坏后说明原因；校验失败弹说明且文档不动 ----
    proc = subprocess.Popen([exe, MULTI_INSTANCE, "-portable", simplePath], cwd=root)
    window = wait_for_main_window("notepadcc - simple.xml")
    try:
        if not window:
            print("  FAIL: 找不到第二个实例的主窗口")
            return False
        time.sleep(0.8)
        user32.PostMessageW(window, WM_COMMAND, CMD_VIEW_XML_TREE, 0)
        panel, lines = panel_lines(window)
        if not panel or lines[0] != "  <a>":
            print(f"  FAIL: 空元素根应当渲染成一行，实际 {lines}")
            return False
        # 先把光标挪到文末，再点树里的那一行
        scinti = visible_scintilla(window)
        user32.SendMessageW(scinti, SCI_GOTOPOS, 8, 0)   # "<a/>\n\n\n\n" 共八个字节
        time.sleep(0.3)
        if caret_line(window) == 0:
            print("  FAIL: 光标没有先挪开，这条就测不出跳跃")
            return False
        click_first_row(panel)
        time.sleep(0.6)
        if caret_line(window) != 0:
            print(f"  FAIL: 点元素应当跳到源码第 1 行，实际第 {caret_line(window) + 1} 行")
            return False

        # 把文档改坏 → 面板里应当说明"不是合法的 XML"（错误信息含行列）
        user32.SendMessageW(scinti, SCI_GOTOPOS, 0, 0)
        user32.PostMessageW(scinti, WM_CHAR_INPUT, ord("<"), 0)
        time.sleep(1.2)   # 刷新是节流的（300 ms），等宽裕一点
        _, broken = panel_lines(window)
        if not broken or "不是合法的 XML" not in broken[0]:
            print(f"  FAIL: 内容不合法时面板里应当说明原因，实际 {broken[:2]}")
            return False
        if "第 1 行" not in broken[0]:
            print(f"  FAIL: 错误信息里应当带行列，实际 {broken[0]!r}")
            return False

        # 校验 XML：不合法 → 弹说明（带行列），弹掉之后内容一个字节都没动
        user32.PostMessageW(window, WM_COMMAND, CMD_TOOL_VALIDATE_XML, 0)
        box = None
        deadline = time.time() + 8.0
        while time.time() < deadline and not box:
            box = user32.FindWindowW("#32770", "校验 XML")
            if not box:
                time.sleep(0.2)
        if not box:
            print("  FAIL: 校验 XML 失败时应当弹出说明")
            return False
        shown = dialog_texts(box)
        if not any("第 1 行" in item for item in shown):
            print(f"  FAIL: 弹框里应当说明出错的行列，实际 {shown!r}")
            dismiss_dialog("校验 XML", IDOK, timeout=4.0, quiet=True)
            return False
        if not dismiss_dialog("校验 XML", IDOK, timeout=6.0):
            print("  FAIL: 校验 XML 的说明弹框点不掉")
            return False
        time.sleep(0.5)
        if scinti_text(scinti) != "<<a/>\n\n\n\n":
            print(f"  FAIL: 校验只是看看，不该改动内容，实际 {scinti_text(scinti)!r}")
            return False
    finally:
        if window:
            user32.PostMessageW(window, WM_CLOSE, 0, 0)
            dismiss_dialog("notepadcc", IDNO, timeout=2.0, quiet=True)
            time.sleep(1.5)
        if proc.poll() is None:
            proc.kill()
        clean_config()

    print("  XML 树视图与校验 XML：树按层级逐行核对（元素/属性/文本/折叠摘要）、折叠与展开、"
          "与 JSON 树共用同一块面板、点元素跳到源码行、非法内容说明原因（含行列）、"
          "校验成功在状态栏报规模、失败弹说明且内容不动")
    return True


def test_xpath_query(root):
    """XPath 查询（M3 XML 工具的最后一件）：对话框里真列命中、跳转真落在那一行、
    表达式错误真的带列号、查询前后文档一个字节不动。

    对话框是模态的（主窗口会被禁用），脚本跨进程驱动它的控件：填表达式、发"查询"、
    读状态行、读列表项（LB_GETTEXT 要往目标进程里写指针，见 listbox_item_text）、
    点"跳转"或发双击通知。列表行格式是固定的"第 N 行  预览"，逐字核对。
    """
    work = os.path.join(root, "build", "xpath_test")
    shutil.rmtree(work, ignore_errors=True)
    os.makedirs(work)
    cfg = os.path.join(root, "build", "bin", "config")
    exe = os.path.join(root, "build", "bin", "notepadcc.exe")

    sample = os.path.join(work, "books.xml")
    with open(sample, "w", encoding="utf-8", newline="") as handle:
        handle.write('<?xml version="1.0"?>\n'
                     '<catalog>\n'
                     '  <book id="b1" lang="zh"><title>三体</title></book>\n'
                     '  <book id="b2"><title>球状闪电</title><note>科幻</note></book>\n'
                     '  <book id="b3" lang="en"><title>Dune</title></book>\n'
                     '</catalog>\n')

    for name in ("config.xml", "session.xml"):
        target = os.path.join(cfg, name)
        if os.path.exists(target):
            os.remove(target)
    log_file = remove_log_with_retry(root)

    def open_query_dialog(window):
        user32.PostMessageW(window, WM_COMMAND, CMD_TOOL_XPATH, 0)
        deadline = time.time() + 8.0
        while time.time() < deadline:
            dialog = user32.FindWindowW(XPATH_DIALOG_CLASS, None)
            if dialog:
                return dialog
            time.sleep(0.2)
        return None

    def wait_dialog_gone(timeout=10.0):
        deadline = time.time() + timeout
        while time.time() < deadline:
            if not user32.FindWindowW(XPATH_DIALOG_CLASS, None):
                return True
            time.sleep(0.15)
        return False

    def status_text(dialog):
        return window_text(user32.GetDlgItem(dialog, XPATH_STATUS))

    def rows(dialog):
        """读取对话框列出的结果行。

        对话框自己实现了 WM_GETTEXT，把"此刻列着什么"按面板同款规矩回出来
        （结果行用 \\r\\n 分隔）；标准 ListBox 的 LB_GETTEXT 要跨进程传指针，
        不可靠，所以不走那条路。
        """
        text = window_text(dialog)
        return text.split("\r\n") if text else []

    def run_query(dialog, expression):
        set_text(dialog, XPATH_EXPR, expression)
        send_bounded(dialog, WM_COMMAND, XPATH_RUN, 0)
        time.sleep(0.2)

    proc = subprocess.Popen([exe, MULTI_INSTANCE, "-portable", sample], cwd=root)
    window = wait_for_main_window("notepadcc - books.xml")
    try:
        if not window:
            print("  FAIL: 找不到主窗口")
            return False
        time.sleep(0.8)
        before = scinti_text(visible_scintilla(window))

        # ---- 1) 命中列表 + 状态行 + 模态 + 跳转 ----
        dialog = open_query_dialog(window)
        if not dialog:
            print("  FAIL: 「XPath 查询…」没弹出对话框")
            return False
        if user32.IsWindowEnabled(window):
            print("  FAIL: 对话框应当是模态的（主窗口该被禁用）")
            return False
        if status_text(dialog) != "输入表达式后按“查询”。":
            print(f"  FAIL: 初始状态行不对：{status_text(dialog)!r}")
            return False
        if user32.IsWindowEnabled(user32.GetDlgItem(dialog, XPATH_GOTO)):
            print("  FAIL: 还没查询时「跳转」应当是灰的")
            return False
        run_query(dialog, "/catalog/book[@id='b2']")
        if status_text(dialog) != "共 1 个匹配":
            print(f"  FAIL: 命中 1 条时状态行不对：{status_text(dialog)!r}")
            return False
        listed = rows(dialog)
        if listed != ['第 4 行  <book id="b2">']:
            print(f"  FAIL: 命中行不对：{listed!r}")
            return False
        send_bounded(dialog, WM_COMMAND, XPATH_GOTO, 0)
        if not wait_dialog_gone():
            print("  FAIL: 点跳转后对话框没关")
            return False
        line = current_line(visible_scintilla(window))
        if line != 3:
            print(f"  FAIL: 跳转后光标应当在第 4 行（0 基 3），实际 {line}")
            return False
        if scinti_text(visible_scintilla(window)) != before:
            print("  FAIL: 查询与跳转一个字节都不该改文档")
            return False

        # ---- 2) 表达式错误：带列号、列表为空、文档不动 ----
        dialog = open_query_dialog(window)
        if not dialog:
            print("  FAIL: 第二次没弹出对话框")
            return False
        run_query(dialog, "//")
        status = status_text(dialog)
        if not status.startswith("错误：") or "表达式第 3 列" not in status:
            print(f"  FAIL: 表达式错误的说明不对：{status!r}")
            return False
        if rows(dialog):
            print("  FAIL: 表达式错误时列表不该有内容")
            return False
        if scinti_text(visible_scintilla(window)) != before:
            print("  FAIL: 非法表达式不该动文档")
            return False

        # ---- 3) 文本命中 + 双击列表项跳转（LBN_DBLCLK 那条通知） ----
        run_query(dialog, "//book[@lang='en']/title/text()")
        if status_text(dialog) != "共 1 个匹配":
            print(f"  FAIL: 文本命中状态不对：{status_text(dialog)!r}")
            return False
        listed = rows(dialog)
        if listed != ['第 5 行  #text="Dune"']:
            print(f"  FAIL: 文本命中行不对：{listed!r}")
            return False
        user32.PostMessageW(dialog, WM_COMMAND, (LBN_DBLCLK << 16) | XPATH_RESULTS, 0)
        if not wait_dialog_gone():
            print("  FAIL: 双击结果后对话框没关")
            return False
        line = current_line(visible_scintilla(window))
        if line != 4:
            print(f"  FAIL: 双击跳转后光标应当在第 5 行（0 基 4），实际 {line}")
            return False

        # ---- 4) 没有命中不是错误；关掉对话框光标不动；日志里有记录 ----
        dialog = open_query_dialog(window)
        if not dialog:
            print("  FAIL: 第三次没弹出对话框")
            return False
        run_query(dialog, "/catalog/magazine")
        if status_text(dialog) != "共 0 个匹配":
            print(f"  FAIL: 无命中时状态行不对：{status_text(dialog)!r}")
            return False
        send_bounded(dialog, WM_COMMAND, XPATH_CLOSE, 0)
        if not wait_dialog_gone():
            print("  FAIL: 关闭按钮没关掉对话框")
            return False
        line = current_line(visible_scintilla(window))
        if line != 4:
            print(f"  FAIL: 直接关掉对话框不该挪光标，实际 {line}")
            return False
        time.sleep(0.5)
        try:
            with open(log_file, "r", encoding="utf-8", errors="replace") as handle:
                log = handle.read()
        except OSError:
            log = ""
        if ("XPath 查询" not in log or "跳到第 4 行" not in log or
                "跳到第 5 行" not in log or "表达式第 3 列" not in log):
            print("  FAIL: 日志里应当留下查询、两次跳转与那条表达式错误")
            return False
    finally:
        if window:
            user32.PostMessageW(window, WM_CLOSE, 0, 0)
            time.sleep(1.0)
        if proc.poll() is None:
            proc.kill()

    print("  XPath 查询：命中列表 / 跳转 / 表达式错误（含列号）/ 文档未动 全对")
    return True


def test_brace_summary_fullscreen(root):
    """括号匹配（Ctrl+B）、摘要（字数统计）、全屏（F11 / Esc）：三件天天看得见的基本功。

    三件事都是"界面上看得见的行为"，所以全都按"读回屏幕上真实的样子"来核对：
    光标位置读 SCI_GETCURRENTPOS；配不上时状态栏那句提示读 SB_GETTEXTW（状态栏是系统控件，
    读它要借目标进程的内存，见 read_status_bar）；摘要对话框四行统计逐字读回来与 Python
    这边独立算的结果比对；全屏核对菜单栏收起来了、窗口矩形等于工作区、样式里的标题栏位
    被摘掉，Esc 退出时三样逐项还原。
    """
    work = os.path.join(root, "build", "brace_test")
    shutil.rmtree(work, ignore_errors=True)
    os.makedirs(work)
    cfg = os.path.join(root, "build", "bin", "config")
    exe = os.path.join(root, "build", "bin", "notepadcc.exe")

    # 中文注释是故意的：它让"字符数"与"字节数"不相等，摘要那一条才分辨得出对错
    # （按字节数报字符数、或压根没按代码页解释，都会在这里露出来）
    text = ("// 括号匹配与摘要：中文注释让字符数与字节数不相等\n"
            "int main()\n"
            "{\n"
            "    return f(1);\n"
            "}\n"
            "// 孤立的右括号，没有左括号配它\n"
            ")\n")
    data = text.encode("utf-8")
    path = os.path.join(work, "code.txt")
    with open(path, "wb") as handle:
        handle.write(data)

    open_paren = data.index(b"(")        # "main()" 里那个左括号
    close_paren = data.index(b")")       # 紧跟着的右括号
    stray_paren = data.rindex(b")")      # 最后一行的孤立右括号
    plain_byte = data.index(b"main")     # 'm'：左右两个字节都不是括号

    stale = ("config.xml", "session.xml")
    for name in stale:
        target = os.path.join(cfg, name)
        if os.path.exists(target):
            os.remove(target)
    log_path = remove_log_with_retry(root)
    hint = "没有可配对的括号"

    def caret():
        return user32.SendMessageW(visible_scintilla(window), SCI_GETCURRENTPOS, 0, 0)

    def put_caret(position):
        user32.SendMessageW(visible_scintilla(window), SCI_GOTOPOS, position, 0)
        time.sleep(0.4)

    def wait_hint_gone(timeout=4.0):
        """等状态栏回到常规内容（挪光标会让它变回"行/列"）。

        不确认这一步，下面"配不上时应当出现提示"就成了读上一条的残留——永远为真的判据。
        """
        deadline = time.time() + timeout
        current = None
        while time.time() < deadline:
            current = read_status_bar(window)
            if current != hint:
                return current
            time.sleep(0.2)
        return current

    proc = subprocess.Popen([exe, MULTI_INSTANCE, "-portable", path], cwd=root)
    window = wait_for_main_window("notepadcc - code.txt")
    try:
        if not window:
            print("  FAIL: 找不到主窗口")
            return False
        time.sleep(1.0)

        # ---- 0) 先自检"读状态栏"这条路是通的，否则后面那两条判据都是空的 ----
        status = read_status_bar(window)
        if not status or "行 " not in status or "插入" not in status:
            print(f"  FAIL: 读不回状态栏内容（读到 {status!r}）")
            return False

        # ---- 1) 括号匹配：光标落在配对括号的那个字节上 ----
        put_caret(open_paren)
        user32.PostMessageW(window, WM_COMMAND, CMD_VIEW_GOTO_MATCH_BRACE, 0)
        time.sleep(0.6)
        if caret() != close_paren:
            print(f"  FAIL: 光标踩在 '(' 上应当跳到 ')'（字节 {close_paren}），实际 {caret()}")
            return False
        # 反方向：光标停在 ')' 后面（右边不是括号）要往左找到 '('
        put_caret(close_paren + 1)
        user32.PostMessageW(window, WM_COMMAND, CMD_VIEW_GOTO_MATCH_BRACE, 0)
        time.sleep(0.6)
        if caret() != open_paren:
            print(f"  FAIL: 光标在 ')' 后面应当跳到 '('（字节 {open_paren}），实际 {caret()}")
            return False

        # ---- 2) 配不上时：光标不动，而且状态栏要给个说法（两种"配不上"各走一遍）----
        put_caret(plain_byte)
        if wait_hint_gone() == hint:
            print("  FAIL: 挪光标之后状态栏还是上一条提示，后面这条判据就测不出新提示")
            return False
        user32.PostMessageW(window, WM_COMMAND, CMD_VIEW_GOTO_MATCH_BRACE, 0)
        time.sleep(0.6)
        if caret() != plain_byte:
            print(f"  FAIL: 旁边没有括号时光标不该动，实际 {caret()}")
            return False
        if wait_status_bar(window, hint) != hint:
            print(f"  FAIL: 旁边没括号时状态栏应当说明，实际 {read_status_bar(window)!r}")
            return False

        put_caret(stray_paren)          # 有括号、但配不上：另一条分支
        if wait_hint_gone() == hint:
            print("  FAIL: 状态栏没有回到常规内容，第二条提示判据测不出东西")
            return False
        user32.PostMessageW(window, WM_COMMAND, CMD_VIEW_GOTO_MATCH_BRACE, 0)
        time.sleep(0.6)
        if caret() != stray_paren:
            print(f"  FAIL: 孤立括号时光标不该动，实际 {caret()}")
            return False
        if wait_status_bar(window, hint) != hint:
            print(f"  FAIL: 孤立括号也应当给出同一条提示，实际 {read_status_bar(window)!r}")
            return False

        # 顺手钉住"命令留的提示真的看得见"这件事对**别的**命令也成立：命令执行完之后
        # 外壳会刷一次状态栏，刷得太狠就会把提示当场盖掉（这一条曾经整类失效）。
        # 书签命令在一份没有书签的文档上给的说法就是最好的样本
        put_caret(0)
        if wait_hint_gone() == hint:
            print("  FAIL: 状态栏没有回到常规内容，书签那条判据测不出东西")
            return False
        user32.PostMessageW(window, WM_COMMAND, CMD_NEXT_BOOKMARK, 0)
        time.sleep(0.6)
        if wait_status_bar(window, "没有书签") != "没有书签":
            print(f"  FAIL: 没有书签时状态栏应当说明，实际 {read_status_bar(window)!r}")
            return False

        # ---- 3) 摘要：四行统计逐字读回来，与 Python 独立算的结果比对 ----
        user32.PostMessageW(window, WM_COMMAND, CMD_VIEW_SUMMARY, 0)
        time.sleep(1.0)
        box = user32.FindWindowW("#32770", "摘要")
        if not box:
            print("  FAIL: 摘要对话框没有出现")
            return False
        chars = len(text)                          # Python 的字符数（码点，含换行）
        line_count = data.count(b"\n") + 1
        expected = (f"行数：{line_count}\r\n"
                    f"字符数：{chars}\r\n"
                    f"词数：{len(text.split())}\r\n"
                    f"字节数：{len(data)}")
        shown = [item for item in dialog_texts(box) if "行数：" in item]
        if not shown or shown[0] != expected:
            print(f"  FAIL: 摘要对话框的内容不对\n           实际 {shown[:1]}\n"
                  f"           期望 {expected!r}")
            dismiss_dialog("摘要", IDOK)
            return False
        if chars == len(data):
            print("  FAIL: 测试文件里字符数与字节数相等，这一条分辨不出对错")
            dismiss_dialog("摘要", IDOK)
            return False
        if not dismiss_dialog("摘要", IDOK):
            return False

        # ---- 4) 全屏（F11）：菜单栏收起来、铺满工作区、标题栏位摘掉；Esc 逐项还原 ----
        menu_before = user32.GetMenu(window)
        rect_before = window_rect(window)
        style_before = user32.GetWindowLongPtrW(window, GWL_STYLE)
        user32.PostMessageW(window, WM_KEYDOWN, VK_F11, 0)
        time.sleep(1.2)
        if user32.GetMenu(window):
            print("  FAIL: 全屏里菜单栏应当收起来")
            return False
        if window_rect(window) != work_area():
            print(f"  FAIL: 全屏的矩形应当等于工作区 {work_area()}，实际 {window_rect(window)}")
            return False
        if user32.GetWindowLongPtrW(window, GWL_STYLE) & WS_CAPTION:
            print("  FAIL: 全屏里标题栏位应当摘掉")
            return False

        user32.PostMessageW(window, WM_KEYDOWN, VK_ESCAPE, 0)
        time.sleep(1.2)
        if user32.GetMenu(window) != menu_before:
            print("  FAIL: 退出全屏后菜单栏没有挂回来")
            return False
        if window_rect(window) != rect_before:
            print(f"  FAIL: 退出全屏后窗口矩形应当是 {rect_before}，实际 {window_rect(window)}")
            return False
        if user32.GetWindowLongPtrW(window, GWL_STYLE) != style_before:
            print("  FAIL: 退出全屏后窗口样式没有还原")
            return False

        # 日志里的两句是"真的进出过全屏"的旁证：菜单栏与矩形都能被别的路径改，日志不会
        log_text = ""
        if os.path.exists(log_path):
            with open(log_path, "r", encoding="utf-8", errors="replace") as handle:
                log_text = handle.read()
        if "进入全屏" not in log_text or "退出全屏" not in log_text:
            print(f"  FAIL: 日志里没有进出全屏的记录\n{log_text[-300:]}")
            return False
    finally:
        if window:
            user32.PostMessageW(window, WM_CLOSE, 0, 0)
            dismiss_dialog("notepadcc", IDNO, timeout=2.0, quiet=True)
            time.sleep(1.5)
        if proc.poll() is None:
            proc.kill()
        for name in stale:
            target = os.path.join(cfg, name)
            if os.path.exists(target):
                os.remove(target)

    print("  括号匹配 + 摘要 + 全屏：Ctrl+B 两个方向都跳到配对括号那个字节、配不上时"
          "光标不动且状态栏给出提示、摘要四行统计逐字核对、F11 收菜单栏并铺满工作区、"
          "Esc 把菜单栏/矩形/窗口样式逐项还原（日志里有进出全屏两句）")
    return True


def test_toolbar(root):
    """工具栏（M3：工具栏与右键菜单定制）：按钮表由配置驱动、点按钮真的执行命令、
    勾选态与可用态回到界面上、定制对话框改得动表，改动当场落盘。

    自绘控件的文本能跨进程读回来（WM_GETTEXT 是系统消息），所以这里核对的是
    "栏里到底有哪几个按钮、按什么顺序、哪个现在是勾上的"，而不是"命令发出去了"。
    **点按钮走的是真正的命中测试**：先问控件"第 N 个按钮的中心 x 在哪"
    （ToolBar::kMsgItemCenterX，只传值不封送指针），再按那个坐标把鼠标消息投进去；
    命中测试写错、按钮位置算错，这一段就会失败。
    """
    work = os.path.join(root, "build", "toolbar_test")
    shutil.rmtree(work, ignore_errors=True)
    os.makedirs(work)
    cfg = os.path.join(root, "build", "bin", "config")
    exe = os.path.join(root, "build", "bin", "notepadcc.exe")

    path = os.path.join(work, "a.txt")
    with open(path, "w", encoding="utf-8", newline="") as handle:
        handle.write("one\ntwo\nthree\n")

    for name in ("config.xml", "session.xml"):
        target = os.path.join(cfg, name)
        if os.path.exists(target):
            os.remove(target)
    remove_log_with_retry(root)

    config_path = os.path.join(cfg, "config.xml")
    # 默认按钮表：None 是分隔符。顺序就是显示顺序，改动默认表要连这里一起改
    expected = ["新建", "打开", "保存", None, "撤销", "重做", None,
                "查找", "查找下一个", None, "标记/取消书签", "下一个书签", None,
                "注释/取消注释", None, "折叠全部", "展开全部", None,
                "暗色模式", "摘要"]
    expected_count = len([item for item in expected if item is not None])
    expected_as_text = ["---" if item is None else item for item in expected]

    def read_config():
        if not os.path.exists(config_path):
            return ""
        with open(config_path, "r", encoding="utf-8", errors="replace") as handle:
            return handle.read()

    def wait_config(predicate, timeout=8.0):
        # 可见性走 RequestSave（1 秒合并窗口）、按钮表走 SaveNow：两条路一条
        # 文件晚一步才出现，统一等它出现，别跟写盘抢跑
        deadline = time.time() + timeout
        while time.time() < deadline:
            if predicate(read_config()):
                return True
            time.sleep(0.2)
        return False

    def toolbar_of(window, timeout=8.0):
        deadline = time.time() + timeout
        while time.time() < deadline:
            for child in children(window):
                if class_name(child) == TOOLBAR_CONTROL_CLASS:
                    return child
            time.sleep(0.2)
        return None

    def dialog():
        return user32.FindWindowW(TOOLBAR_DIALOG_CLASS, None)

    def wait_dialog(timeout=8.0):
        deadline = time.time() + timeout
        while time.time() < deadline:
            box = dialog()
            if box:
                return box
            time.sleep(0.2)
        return None

    def wait_dialog_gone(timeout=8.0):
        deadline = time.time() + timeout
        while time.time() < deadline:
            if not dialog():
                return True
            time.sleep(0.2)
        return False

    def child_with_id(parent, control_id):
        for child in children(parent):
            if user32.GetDlgCtrlID(child) == control_id:
                return child
        return None

    def click_button(parent, text, timeout=6.0):
        deadline = time.time() + timeout
        while time.time() < deadline:
            for child in children(parent):
                if window_text(child) == text:
                    user32.PostMessageW(child, 0x00F5, 0, 0)   # BM_CLICK，投递不阻塞
                    return True
            time.sleep(0.2)
        return False

    def pieces_of(toolbar):
        return [piece.strip() for piece in window_text(toolbar).split("|")]

    def close_dialog_if_any():
        box = dialog()
        if box:
            send_bounded(box, WM_CLOSE)
            time.sleep(0.4)

    proc = subprocess.Popen([exe, MULTI_INSTANCE, "-portable", path], cwd=root)
    window = wait_for_main_window("notepadcc - a.txt")
    try:
        if not window:
            print("  FAIL: 找不到主窗口")
            return False
        time.sleep(1.0)

        toolbar = toolbar_of(window)
        if not toolbar:
            print("  FAIL: 主窗口里找不到工具栏子窗口（notepadccToolBar）")
            return False
        if not user32.IsWindowVisible(toolbar):
            print("  FAIL: 工具栏默认应当是显示的")
            return False

        # ---- 1) 默认按钮表：逐项核对（顺序、分隔符、可点按钮个数）----
        pieces = pieces_of(toolbar)
        if len(pieces) != len(expected):
            print(f"  FAIL: 默认按钮表应有 {len(expected)} 项，实际 {len(pieces)}："
                  f"{window_text(toolbar)!r}")
            return False
        for index, want in enumerate(expected):
            want = "---" if want is None else want
            if pieces[index] != want:
                print(f"  FAIL: 第 {index} 项应当是 {want!r}，实际 {pieces[index]!r}")
                return False
        if "[v]" in window_text(toolbar):
            print(f"  FAIL: 刚启动时不该有按钮是勾上的，实际 {window_text(toolbar)!r}")
            return False
        count = send_bounded(toolbar, TB_MSG_ITEM_COUNT)
        if count != expected_count:
            print(f"  FAIL: 可点按钮应当有 {expected_count} 个，实际 {count}")
            return False

        # ---- 2) 点真按钮：命中测试 → 执行命令 → 勾选态回到界面上 ----
        dark_ordinal = [item for item in expected if item is not None].index("暗色模式")
        x = send_bounded(toolbar, TB_MSG_ITEM_CENTER_X, dark_ordinal)
        if x is None or x < 0:
            print(f"  FAIL: 问不到第 {dark_ordinal} 个按钮的中心（返回 {x}）")
            return False
        rect = wintypes.RECT()
        user32.GetClientRect(toolbar, ctypes.byref(rect))
        y = (rect.bottom - rect.top) // 2
        user32.PostMessageW(toolbar, WM_LBUTTONDOWN, 1, ((y << 16) | x) & 0xFFFFFFFF)
        user32.PostMessageW(toolbar, WM_LBUTTONUP, 0, ((y << 16) | x) & 0xFFFFFFFF)
        time.sleep(1.2)
        if "[v] 暗色模式" not in window_text(toolbar):
            print(f"  FAIL: 点了暗色按钮之后它应当是勾上的，实际 {window_text(toolbar)!r}")
            return False
        user32.PostMessageW(toolbar, WM_LBUTTONDOWN, 1, ((y << 16) | x) & 0xFFFFFFFF)
        user32.PostMessageW(toolbar, WM_LBUTTONUP, 0, ((y << 16) | x) & 0xFFFFFFFF)
        time.sleep(1.2)
        if "[v]" in window_text(toolbar):
            print(f"  FAIL: 再点一次应当取消勾选，实际 {window_text(toolbar)!r}")
            return False

        if not os.path.exists(config_path):
            print("  FAIL: 启动之后 config.xml 就应当存在（工具栏的可见性写在里面）")
            return False

        # ---- 3) 菜单栏里那一项的勾选态：WM_INITMENUPOPUP 之后按注册表刷出来 ----
        menu_bar = user32.GetMenu(window)
        popup = user32.GetSubMenu(menu_bar, MENU_INDEX_VIEW)
        send_bounded(window, WM_INITMENUPOPUP, popup, MENU_INDEX_VIEW)
        position = None
        for index in range(user32.GetMenuItemCount(popup)):
            buffer = ctypes.create_unicode_buffer(128)
            user32.GetMenuStringW(popup, index, buffer, 128, MF_BYPOSITION)
            if "工具栏" in buffer.value:
                position = index
                break
        if position is None:
            print("  FAIL: 视图菜单里没有「工具栏」这一项")
            return False
        state = user32.GetMenuState(popup, position, MF_BYPOSITION)
        if not state & MF_CHECKED:
            print(f"  FAIL: 工具栏开着时菜单项应当勾上，GetMenuState={state:#x}")
            return False

        # ---- 4) 关掉工具栏：窗口真藏起来、菜单不勾了、配置里记下来 ----
        send_bounded(window, WM_COMMAND, CMD_VIEW_TOOLBAR, 0)
        time.sleep(1.2)
        if user32.IsWindowVisible(toolbar):
            print("  FAIL: 关掉之后工具栏应当隐藏")
            return False
        send_bounded(window, WM_INITMENUPOPUP, popup, MENU_INDEX_VIEW)
        state = user32.GetMenuState(popup, position, MF_BYPOSITION)
        if state & MF_CHECKED:
            print(f"  FAIL: 关掉之后菜单项不该还勾着，GetMenuState={state:#x}")
            return False
        if not wait_config(lambda text: 'visible="no"' in text):
            print(f"  FAIL: config.xml 里应当记下工具栏不可见\n{read_config()[-400:]}")
            return False
        send_bounded(window, WM_COMMAND, CMD_VIEW_TOOLBAR, 0)
        time.sleep(1.0)
        if not user32.IsWindowVisible(toolbar):
            print("  FAIL: 再开一次应当显示回来")
            return False

        # ---- 5) 定制对话框：删掉第一个按钮 → 界面与配置一起变；恢复默认 → 又回来 ----
        # 对话框是模态的：SendMessage 要一直占到窗口关掉才返回，这里只能投递
        user32.PostMessageW(window, WM_COMMAND, CMD_SETTINGS_TOOLBAR, 0)
        box = wait_dialog()
        if not box:
            print("  FAIL: 自定义工具栏对话框没有出现")
            return False
        # 窗口刚建出来时子控件与列表内容可能还没填完：等列表满员再往下走
        current = available = None
        listed = None
        deadline = time.time() + 6.0
        while time.time() < deadline:
            box = dialog()
            if box:
                current = child_with_id(box, TB_DLG_CURRENT)
                available = child_with_id(box, TB_DLG_AVAILABLE)
                listed = send_bounded(current, LB_GETCOUNT) if current else None
                if current and available and listed == len(expected):
                    break
            time.sleep(0.2)
        if not current or not available:
            print("  FAIL: 对话框里找不到两个列表（可用命令 / 现在的按钮）")
            close_dialog_if_any()
            return False
        if listed != len(expected):
            print(f"  FAIL: 当前按钮列表应当有 {len(expected)} 项，实际 {listed}")
            close_dialog_if_any()
            return False
        send_bounded(current, LB_SETCURSEL, 0, 0)
        if not click_button(box, "← 移除"):
            print("  FAIL: 对话框里找不到「← 移除」按钮")
            close_dialog_if_any()
            return False
        deadline = time.time() + 6.0
        while time.time() < deadline and send_bounded(current, LB_GETCOUNT) != len(expected) - 1:
            time.sleep(0.2)
        if send_bounded(current, LB_GETCOUNT) != len(expected) - 1:
            print("  FAIL: 移除之后当前列表应当少一项")
            close_dialog_if_any()
            return False
        if not click_button(box, "确定"):
            print("  FAIL: 对话框里找不到「确定」按钮")
            close_dialog_if_any()
            return False
        if not wait_dialog_gone():
            print("  FAIL: 点了确定之后对话框应当关掉")
            return False
        time.sleep(0.6)
        pieces = pieces_of(toolbar)
        if pieces[0] != "打开" or len(pieces) != len(expected) - 1:
            print(f"  FAIL: 移除「新建」之后第一项应当是「打开」且少一项，实际 "
                  f"{window_text(toolbar)!r}")
            return False
        if not wait_config(lambda text: "cmd.file.open" in text and "cmd.file.new" not in text):
            config_text = read_config()
            print(f"  FAIL: 配置里应当只剩改动后的按钮表\n{config_text[-400:]}")
            return False

        # 恢复默认：按钮表回到代码里那份，配置文件里不再留 <Item>
        user32.PostMessageW(window, WM_COMMAND, CMD_SETTINGS_TOOLBAR, 0)   # 模态框，投递
        box = wait_dialog()
        if not box:
            print("  FAIL: 第二次打开自定义对话框失败")
            return False
        if not click_button(box, "恢复默认"):
            print("  FAIL: 对话框里找不到「恢复默认」按钮")
            close_dialog_if_any()
            return False
        if not wait_dialog_gone():
            print("  FAIL: 点了恢复默认之后对话框应当关掉")
            return False
        time.sleep(0.6)
        if pieces_of(toolbar) != expected_as_text:
            print(f"  FAIL: 恢复默认之后应当回到默认按钮表，实际 {window_text(toolbar)!r}")
            return False
        if not wait_config(lambda text: "cmd.file." not in text):
            print(f"  FAIL: 恢复默认之后配置里不该再留着按钮表\n{read_config()[-400:]}")
            return False
    finally:
        close_dialog_if_any()
        if window:
            user32.PostMessageW(window, WM_CLOSE, 0, 0)
            dismiss_dialog("notepadcc", IDNO, timeout=2.0, quiet=True)
            time.sleep(1.5)
        if proc.poll() is None:
            proc.kill()
        for name in ("config.xml", "session.xml"):
            target = os.path.join(cfg, name)
            if os.path.exists(target):
                os.remove(target)

    print(f"  工具栏：默认 {len(expected)} 项按钮逐项核对、点第 {dark_ordinal} 个按钮"
          "（暗色）真的切换并显示勾选、菜单项勾选态跟着命令状态走、关掉后"
          "config.xml 记下 visible=no、定制对话框删按钮后界面与配置一起变、"
          "恢复默认又回到代码里那份表")
    return True


def test_context_menu(root):
    """右键菜单深度定制（M3 第九块）：弹出的是我们自己的菜单、项表由配置驱动、
    定制对话框改得动表，改动落盘并在重启后仍然生效。

    端到端链路比“命令发出去了”深一层：给编辑控件真的发一条 WM_CONTEXTMENU
    （与用户按右键同一条路：EditView 拦下 Scintilla 自带的英文菜单转给外壳）→
    真的弹出 TrackPopupMenu → 从菜单窗口（#32768）把 HMENU 要回来逐项核对
    （菜单是内核对象，GetMenuStringW 跨进程可用）→ 给菜单窗口投 Down/Enter 按键，
    真的选中并执行一条命令（「全选」，用 Scintilla 选区核对执行结果）→ Esc 关掉。
    """
    work = os.path.join(root, "build", "contextmenu_test")
    shutil.rmtree(work, ignore_errors=True)
    os.makedirs(work)
    cfg = os.path.join(root, "build", "bin", "config")
    exe = os.path.join(root, "build", "bin", "notepadcc.exe")

    path = os.path.join(work, "a.txt")
    with open(path, "w", encoding="utf-8", newline="") as handle:
        handle.write("one\ntwo\nthree\n")

    for name in ("config.xml", "session.xml"):
        target = os.path.join(cfg, name)
        if os.path.exists(target):
            os.remove(target)
    remove_log_with_retry(root)

    config_path = os.path.join(cfg, "config.xml")
    # 默认右键菜单项表：None 是分隔符。顺序就是显示顺序，改动默认表要连这里一起改
    default_items = ["撤销", "重做", None, "剪切", "复制", "粘贴", None, "全选", None,
                     "注释/取消注释"]

    def read_config():
        if not os.path.exists(config_path):
            return ""
        with open(config_path, "r", encoding="utf-8", errors="replace") as handle:
            return handle.read()

    def wait_config(predicate, timeout=8.0):
        # 改动走 SaveNow（立刻写盘）：等它出现，别跟写盘抢跑
        deadline = time.time() + timeout
        while time.time() < deadline:
            if predicate(read_config()):
                return True
            time.sleep(0.2)
        return False

    def strip_accel(text):
        # 菜单里的文本是"撤销(&U)"这种形式：& 是加速键下划线，尾部的 (U) 是加速键本身；
        # 核对的是命令名，两样都去掉
        text = text.replace("&", "")
        if len(text) >= 3 and text.endswith(")") and text[-3] == "(":
            text = text[:-3]
        return text

    def dialog():
        return user32.FindWindowW(CONTEXTMENU_DIALOG_CLASS, None)

    def wait_dialog(timeout=8.0):
        deadline = time.time() + timeout
        while time.time() < deadline:
            box = dialog()
            if box:
                return box
            time.sleep(0.2)
        return None

    def wait_dialog_gone(timeout=8.0):
        deadline = time.time() + timeout
        while time.time() < deadline:
            if not dialog():
                return True
            time.sleep(0.2)
        return False

    def close_dialog_if_any():
        box = dialog()
        if box:
            send_bounded(box, WM_CLOSE)
            time.sleep(0.4)

    def child_with_id(parent, control_id):
        for child in children(parent):
            if user32.GetDlgCtrlID(child) == control_id:
                return child
        return None

    def click_button(parent, text, timeout=6.0):
        deadline = time.time() + timeout
        while time.time() < deadline:
            for child in children(parent):
                if window_text(child) == text:
                    user32.PostMessageW(child, 0x00F5, 0, 0)   # BM_CLICK，投递不阻塞
                    return True
            time.sleep(0.2)
        return False

    def wait_current_list(box, expected_count, timeout=6.0):
        """等对话框的“右键菜单里的命令”列表填满到期望的项数。"""
        deadline = time.time() + timeout
        current = None
        while time.time() < deadline:
            box = dialog()
            if box:
                current = child_with_id(box, TB_DLG_CURRENT)
                if current and send_bounded(current, LB_GETCOUNT) == expected_count:
                    break
            time.sleep(0.2)
        return current

    def popup_window(timeout=5.0):
        deadline = time.time() + timeout
        while time.time() < deadline:
            found = user32.FindWindowW(POPUP_MENU_CLASS, None)
            if found:
                return found
            time.sleep(0.1)
        return None

    def post_contextmenu(window):
        """给编辑控件投一条 WM_CONTEXTMENU——外壳收到的就是这一类消息。

        必须投递而不是同步发送：外壳要等 TrackPopupMenu 结束才返回，
        同步发送会把脚本卡在菜单的整个生命周期里。
        """
        view = visible_scintilla(window)
        if not view:
            return False
        rect = wintypes.RECT()
        user32.GetWindowRect(view, ctypes.byref(rect))
        x = (rect.left + rect.right) // 2
        y = (rect.top + rect.bottom) // 2
        user32.PostMessageW(view, WM_CONTEXTMENU, view,
                            ((y & 0xFFFF) << 16) | (x & 0xFFFF))
        return True

    def open_popup(window):
        if not post_contextmenu(window):
            return None
        return popup_window()

    def popup_items(menu):
        """把此刻弹出的菜单逐项读回来（分隔线读出来是空串）。"""
        hmenu = send_bounded(menu, MN_GETHMENU, 0, 0)
        if not hmenu:
            return None
        out = []
        for position in range(user32.GetMenuItemCount(hmenu)):
            buffer = ctypes.create_unicode_buffer(512)
            user32.GetMenuStringW(hmenu, position, buffer, 512, MF_BYPOSITION)
            out.append(strip_accel(buffer.value))
        return out

    def dismiss_popup(menu):
        """给菜单窗口投一条 Esc（实测 WM_CANCELMODE 不管用、WM_KEYDOWN 管用）。

        菜单关掉之前主线程留在菜单的消息循环里，后面的断言都做不了。
        """
        user32.PostMessageW(menu, WM_KEYDOWN, VK_ESCAPE, 0)
        deadline = time.time() + 5.0
        while time.time() < deadline:
            if not user32.FindWindowW(POPUP_MENU_CLASS, None):
                return True
            time.sleep(0.1)
        return False

    def expected_of(items):
        # 弹出菜单里分隔线的文本读出来是空串
        return ["" if item is None else item for item in items]

    def launch():
        proc = subprocess.Popen([exe, MULTI_INSTANCE, "-portable", path], cwd=root)
        window = wait_for_main_window("notepadcc - a.txt")
        return proc, window

    def stop(proc, window):
        if window and user32.IsWindow(window):
            user32.PostMessageW(window, WM_CLOSE, 0, 0)
            dismiss_dialog("notepadcc", IDNO, timeout=2.0, quiet=True)
            deadline = time.time() + 6.0
            while time.time() < deadline and user32.IsWindow(window):
                time.sleep(0.2)
        if proc.poll() is None:
            proc.kill()
        time.sleep(0.4)

    proc, window = launch()
    try:
        if not window:
            print("  FAIL: 找不到主窗口")
            return False
        time.sleep(1.0)
        view = visible_scintilla(window)
        if not view:
            print("  FAIL: 找不到可见的编辑控件")
            return False

        # ---- 1) 默认菜单：真的弹出来、逐项核对；键盘选中「全选」真的执行 ----
        send_bounded(view, SCI_GOTOLINE, 2, 0)
        time.sleep(0.3)
        menu = open_popup(window)
        if not menu:
            print("  FAIL: 发 WM_CONTEXTMENU 之后没有弹出右键菜单（#32768 没出现）")
            return False
        items = popup_items(menu)
        if items != expected_of(default_items):
            print(f"  FAIL: 默认右键菜单项不对：{items!r}")
            dismiss_popup(menu)
            return False
        # 向下 6 次（箭头键会跳过分隔符）落到「全选」，回车执行
        for _ in range(6):
            user32.PostMessageW(menu, WM_KEYDOWN, VK_DOWN, 0)
            time.sleep(0.15)
        user32.PostMessageW(menu, WM_KEYDOWN, VK_RETURN, 0)
        if not dismiss_popup(menu):   # 回车应当已经关掉菜单；这里主要是等它真的关掉
            print("  FAIL: 回车之后右键菜单没关掉")
            return False
        start = end = length = None
        deadline = time.time() + 5.0
        while time.time() < deadline:
            start = send_bounded(view, SCI_GETSELECTIONSTART)
            end = send_bounded(view, SCI_GETSELECTIONEND)
            length = send_bounded(view, SCI_GETLENGTH)
            if start == 0 and end == length:
                break
            time.sleep(0.2)
        if (start, end, length) != (0, 14, 14):
            print(f"  FAIL: 从右键菜单选「全选」应当选中整篇，实际选区 {start}..{end}"
                  f"（长度 {length}）")
            return False

        # ---- 2) 定制对话框：标题 / 说明 / 项表；取消不改配置 ----
        user32.PostMessageW(window, WM_COMMAND, CMD_SETTINGS_CONTEXT_MENU, 0)   # 模态框，投递
        box = wait_dialog()
        if not box:
            print("  FAIL: 自定义右键菜单对话框没有出现")
            return False
        if user32.IsWindowEnabled(window):
            print("  FAIL: 对话框应当是模态的（主窗口该被禁用）")
            close_dialog_if_any()
            return False
        if window_text(box) != "自定义右键菜单":
            print(f"  FAIL: 对话框标题不对：{window_text(box)!r}")
            close_dialog_if_any()
            return False
        texts = dialog_texts(box)
        for want in ("可用命令（双击加进右键菜单）", "右键菜单里的命令（顺序就是显示顺序）"):
            if want not in texts:
                print(f"  FAIL: 对话框里找不到说明文字 {want!r}，实际 {texts!r}")
                close_dialog_if_any()
                return False
        current = wait_current_list(box, len(default_items))
        available = child_with_id(dialog() or box, TB_DLG_AVAILABLE)
        if not current or not available:
            print("  FAIL: 对话框里找不到两个列表（可用命令 / 右键菜单里的命令）")
            close_dialog_if_any()
            return False
        if (send_bounded(available, LB_GETCOUNT) or 0) <= 0:
            print("  FAIL: 「可用命令」列表不该是空的")
            close_dialog_if_any()
            return False
        if not click_button(dialog() or box, "取消"):
            print("  FAIL: 对话框里找不到「取消」按钮")
            close_dialog_if_any()
            return False
        if not wait_dialog_gone():
            print("  FAIL: 点取消之后对话框应当关掉")
            return False
        if "<contextmenu" in read_config():
            print("  FAIL: 取消不该往配置里写 <contextmenu>")
            return False

        # ---- 3) 删掉「撤销」→ 确定：弹出菜单与配置一起变 ----
        user32.PostMessageW(window, WM_COMMAND, CMD_SETTINGS_CONTEXT_MENU, 0)
        box = wait_dialog()
        if not box:
            print("  FAIL: 第二次打开自定义对话框失败")
            return False
        current = wait_current_list(box, len(default_items))
        if not current:
            print("  FAIL: 第二次打开时当前项表没满员")
            close_dialog_if_any()
            return False
        send_bounded(current, LB_SETCURSEL, 0, 0)
        if not click_button(dialog() or box, "← 移除"):
            print("  FAIL: 对话框里找不到「← 移除」按钮")
            close_dialog_if_any()
            return False
        deadline = time.time() + 6.0
        while (time.time() < deadline and
               send_bounded(current, LB_GETCOUNT) != len(default_items) - 1):
            time.sleep(0.2)
        if send_bounded(current, LB_GETCOUNT) != len(default_items) - 1:
            print("  FAIL: 移除之后当前项表应当少一项")
            close_dialog_if_any()
            return False
        if not click_button(dialog() or box, "确定"):
            print("  FAIL: 对话框里找不到「确定」按钮")
            close_dialog_if_any()
            return False
        if not wait_dialog_gone():
            print("  FAIL: 点了确定之后对话框应当关掉")
            return False
        if not wait_config(lambda text: "<contextmenu" in text and "cmd.edit.undo" not in text):
            print(f"  FAIL: 配置里应当只剩改动后的右键菜单项表\n{read_config()[-400:]}")
            return False
        after_remove = [item for item in default_items if item != "撤销"]
        menu = open_popup(window)
        if not menu:
            print("  FAIL: 改过配置之后右键菜单没弹出来")
            return False
        items = popup_items(menu)
        if items != expected_of(after_remove):
            print(f"  FAIL: 删掉「撤销」之后的菜单不对：{items!r}")
            dismiss_popup(menu)
            return False
        if not dismiss_popup(menu):
            print("  FAIL: 菜单没关掉")
            return False

        # ---- 4) 重启：配置里的项表仍然生效 ----
        stop(proc, window)
        session_path = os.path.join(cfg, "session.xml")
        if os.path.exists(session_path):
            os.remove(session_path)
        proc, window = launch()
        if not window:
            print("  FAIL: 重启后找不到主窗口")
            return False
        time.sleep(1.0)
        menu = open_popup(window)
        if not menu:
            print("  FAIL: 重启之后右键菜单没弹出来")
            return False
        items = popup_items(menu)
        if items != expected_of(after_remove):
            print(f"  FAIL: 重启后菜单应当保持删改后的项表：{items!r}")
            dismiss_popup(menu)
            return False
        if not dismiss_popup(menu):
            print("  FAIL: 菜单没关掉")
            return False

        # ---- 5) 恢复默认：菜单回到代码里那份表，配置文件里不再留 <contextmenu> ----
        user32.PostMessageW(window, WM_COMMAND, CMD_SETTINGS_CONTEXT_MENU, 0)
        box = wait_dialog()
        if not box:
            print("  FAIL: 第三次打开自定义对话框失败")
            return False
        if not click_button(box, "恢复默认"):
            print("  FAIL: 对话框里找不到「恢复默认」按钮")
            close_dialog_if_any()
            return False
        if not wait_dialog_gone():
            print("  FAIL: 点了恢复默认之后对话框应当关掉")
            return False
        if not wait_config(lambda text: "<contextmenu" not in text):
            print(f"  FAIL: 恢复默认之后配置里不该再留 <contextmenu>\n{read_config()[-400:]}")
            return False
        menu = open_popup(window)
        if not menu:
            print("  FAIL: 恢复默认之后右键菜单没弹出来")
            return False
        items = popup_items(menu)
        if items != expected_of(default_items):
            print(f"  FAIL: 恢复默认之后菜单应当回到默认项表：{items!r}")
            dismiss_popup(menu)
            return False
        if not dismiss_popup(menu):
            print("  FAIL: 菜单没关掉")
            return False

        # ---- 6) 全部删空：右键不弹菜单（配置说了算），配置里记 empty=yes ----
        user32.PostMessageW(window, WM_COMMAND, CMD_SETTINGS_CONTEXT_MENU, 0)
        box = wait_dialog()
        if not box:
            print("  FAIL: 第四次打开自定义对话框失败")
            return False
        current = wait_current_list(box, len(default_items))
        if not current:
            print("  FAIL: 第四次打开时当前项表没满员")
            close_dialog_if_any()
            return False
        removed = 0
        while removed <= len(default_items):
            if not send_bounded(current, LB_GETCOUNT):
                break
            send_bounded(current, LB_SETCURSEL, 0, 0)
            click_button(dialog() or box, "← 移除")
            removed += 1
            time.sleep(0.15)
        if send_bounded(current, LB_GETCOUNT) != 0:
            print(f"  FAIL: 没删空，还剩 {send_bounded(current, LB_GETCOUNT)} 项")
            close_dialog_if_any()
            return False
        if not click_button(dialog() or box, "确定"):
            print("  FAIL: 对话框里找不到「确定」按钮")
            close_dialog_if_any()
            return False
        if not wait_dialog_gone():
            print("  FAIL: 点了确定之后对话框应当关掉")
            return False
        if not wait_config(lambda text: "contextmenu" in text and 'empty="yes"' in text):
            print(f"  FAIL: 删空之后配置里应当记 empty=yes\n{read_config()[-400:]}")
            return False
        if not post_contextmenu(window):
            print("  FAIL: 找不到可见的编辑控件")
            return False
        appeared = None
        deadline = time.time() + 1.5
        while time.time() < deadline:
            appeared = user32.FindWindowW(POPUP_MENU_CLASS, None)
            if appeared:
                break
            time.sleep(0.1)
        if appeared:
            print("  FAIL: 项表删空之后右键不该再弹菜单（配置说了算）")
            dismiss_popup(appeared)
            return False
        if send_bounded(window, WM_NULL) is None:
            print("  FAIL: 不弹菜单不等于卡死：主窗口应当照常响应")
            return False

        # ---- 7) 再恢复默认收尾：配置干净、菜单回到默认 ----
        user32.PostMessageW(window, WM_COMMAND, CMD_SETTINGS_CONTEXT_MENU, 0)
        box = wait_dialog()
        if not box:
            print("  FAIL: 第五次打开自定义对话框失败")
            return False
        if not click_button(box, "恢复默认"):
            print("  FAIL: 对话框里找不到「恢复默认」按钮")
            close_dialog_if_any()
            return False
        if not wait_dialog_gone():
            print("  FAIL: 点了恢复默认之后对话框应当关掉")
            return False
        if not wait_config(lambda text: "<contextmenu" not in text):
            print(f"  FAIL: 收尾时配置里不该再留 <contextmenu>\n{read_config()[-400:]}")
            return False
    finally:
        close_dialog_if_any()
        leftover = user32.FindWindowW(POPUP_MENU_CLASS, None)
        if leftover:
            dismiss_popup(leftover)
        stop(proc, window)
        for name in ("config.xml", "session.xml"):
            target = os.path.join(cfg, name)
            if os.path.exists(target):
                os.remove(target)

    print("  右键菜单定制：默认 10 项逐项核对、键盘选中「全选」真的执行、对话框"
          "模态且删项后菜单与配置一起变、重启后仍生效、恢复默认回到代码里那份表、"
          "删空之后右键不再弹菜单")
    return True



def test_compare(root):
    """比较：两个文档之间、以及与磁盘上的版本之间，差异结果逐行核对。

    第二个文档是**用单实例交接打开**的（再启动一个带文件的实例，它把路径交给已在跑的
    那个）——脚本没法驱动系统的"打开"对话框（DirectUI，跨进程驱动不了），
    而交接这条路本来就在"单实例"那一段验证过。
    """
    work = os.path.join(root, "build", "compare_test")
    shutil.rmtree(work, ignore_errors=True)
    os.makedirs(work)
    cfg = os.path.join(root, "build", "bin", "config")
    exe = os.path.join(root, "build", "bin", "notepadcc.exe")

    leftText = "one\ntwo\nthree\n"
    rightText = "one\nTWO\nthree\nfour\n"
    leftPath = os.path.join(work, "a.txt")
    rightPath = os.path.join(work, "b.txt")
    for path, text in ((leftPath, leftText), (rightPath, rightText)):
        with open(path, "w", encoding="utf-8", newline="") as handle:
            handle.write(text)

    for name in ("config.xml", "session.xml"):
        target = os.path.join(cfg, name)
        if os.path.exists(target):
            os.remove(target)
    log_file = remove_log_with_retry(root)

    def open_first():
        # **不加 --multi-instance**：这一段要靠单实例交接把第二份文件送进来
        #（加了那个开关就等于"我不参与抢占"，第二个实例会自己开一个窗口）
        proc = subprocess.Popen([exe, "-portable", leftPath], cwd=root)
        window = wait_for_main_window("notepadcc - a.txt")
        return proc, window

    def hand_over(path, title):
        """再启动一个实例打开另一份文件：单实例交接会把标签加到第一个实例里。"""
        second = subprocess.Popen([exe, "-portable", path], cwd=root)
        deadline = time.time() + 20.0
        while time.time() < deadline:
            if user32.FindWindowW("notepadccMainWindow", f"notepadcc - {title}"):
                time.sleep(0.5)
                return True
            time.sleep(0.3)
        if second.poll() is None:
            second.kill()
        return False

    proc = None
    window = None
    try:
        proc, window = open_first()
        if not window:
            print("  FAIL: 找不到主窗口")
            return False
        time.sleep(0.8)
        # 命令行带了文件时**不该再带出一个空白标签**（这条是踩出来的：先建空白文档、
        # 再打开文件，用户双击文件第一眼看到的就是那个空标签）
        user32.PostMessageW(window, WM_COMMAND, CMD_WINDOW_NEXT, 0)
        time.sleep(0.6)
        if user32.FindWindowW("notepadccMainWindow", "notepadcc - 新建 1"):
            print("  FAIL: 带文件启动时不该多出一个空白标签")
            return False
        if not user32.FindWindowW("notepadccMainWindow", "notepadcc - a.txt"):
            print("  FAIL: 只有一个标签时「下一个标签」应当还是自己")
            return False
        if not hand_over(rightPath, "b.txt"):
            print("  FAIL: 第二个文档没有通过单实例交接打开")
            return False
        # 切回 a.txt（交接会激活 b.txt，而"比较"拿的是当前文档）
        user32.PostMessageW(window, WM_COMMAND, CMD_WINDOW_PREV, 0)
        time.sleep(0.8)

        # 1) 与另一个已打开的文档比较：选列表里的第一项（b.txt）
        user32.PostMessageW(window, WM_COMMAND, CMD_TOOL_COMPARE_DOCUMENT, 0)
        deadline = time.time() + 8.0
        dialog = None
        while time.time() < deadline:
            dialog = user32.FindWindowW(COMPARE_PICK_CLASS, None)
            if dialog:
                break
            time.sleep(0.2)
        if not dialog:
            print("  FAIL: 没有弹出「选择要比较的文档」")
            return False
        if user32.SendMessageW(user32.GetDlgItem(dialog, COMPARE_PICK_LIST), 0x018B, 0, 0) != 1:
            print("  FAIL: 可选的文档应当正好 1 个（另一个标签）")
            return False
        user32.PostMessageW(dialog, WM_COMMAND, COMPARE_PICK_OK, 0)
        time.sleep(1.2)

        scinti = visible_scintilla(window)
        diff = scinti_text(scinti)
        if not diff.startswith("比较：a.txt  ↔  b.txt"):
            print(f"  FAIL: 比较结果第一行不对：{diff.splitlines()[0]!r}")
            return False
        lines = diff.split("\n")
        # 改动行与新增行之间隔着相同的 "three"，所以算**两**处不同
        if lines[1] != "左 3 行，右 4 行；不同 2 处（改动 1、删除 0、新增 1）":
            print(f"  FAIL: 统计行不对：{lines[1]!r}")
            return False
        body = [line for line in lines[3:] if line.strip()]
        # 相同行、改动行、新增行：记号分别是 空格 / ~ / +
        marks = [line[0] for line in body]
        if marks != [" ", "~", " ", "+"]:
            print(f"  FAIL: 改动记号序列不对：{marks}\n{diff}")
            return False
        if "two" not in body[1] or "TWO" not in body[1]:
            print(f"  FAIL: 改动行应当两侧都显示（左边 two、右边 TWO）：{body[1]!r}")
            return False
        if "four" not in body[3] or body[3].count("│") != 1:
            print(f"  FAIL: 新增行应当只在右侧有内容：{body[3]!r}")
            return False

        # 2) 与磁盘上的版本比较：改一改当前文档（a.txt 那个标签），差异要指出来
        user32.PostMessageW(window, WM_COMMAND, CMD_WINDOW_NEXT, 0)   # 回到某个标签
        time.sleep(0.6)
        # 直接对当前可见的那个编辑控件敲字（哪个标签都行，比较的是"当前文档 vs 磁盘"）
        scinti = visible_scintilla(window)
        user32.SendMessageW(scinti, SCI_GOTOPOS, 0, 0)
        for ch in "X":
            user32.PostMessageW(scinti, WM_CHAR_INPUT, ord(ch), 0)
        time.sleep(0.5)
        user32.PostMessageW(window, WM_COMMAND, CMD_TOOL_COMPARE_DISK, 0)
        time.sleep(1.2)
        scinti = visible_scintilla(window)
        diskDiff = scinti_text(scinti)
        if "（磁盘上）" not in diskDiff.splitlines()[0]:
            print(f"  FAIL: 与磁盘比较的标题里应当标明右边是磁盘版本：{diskDiff.splitlines()[0]!r}")
            return False
        if diskDiff.splitlines()[1] != "左 3 行，右 3 行；不同 1 处（改动 1、删除 0、新增 0）":
            print(f"  FAIL: 与磁盘比较的统计不对：{diskDiff.splitlines()[1]!r}")
            return False
        if "Xone" not in diskDiff:
            print(f"  FAIL: 改动后的那一行应当出现在差异里\n{diskDiff}")
            return False
        with open(log_file, "r", encoding="utf-8", errors="replace") as handle:
            log = handle.read()
        if log.count("比较完成") < 2:
            print("  FAIL: 两次比较都应当在日志里留下记录")
            return False
    finally:
        if window:
            user32.PostMessageW(window, WM_CLOSE, 0, 0)
            dismiss_dialog("notepadcc", IDNO, timeout=2.0, quiet=True)
            time.sleep(1.5)
        if proc is not None and proc.poll() is None:
            proc.kill()
        for name in ("config.xml", "session.xml"):
            target = os.path.join(cfg, name)
            if os.path.exists(target):
                os.remove(target)

    print("  比较：两个文档之间的差异（改动/新增/相同行与两侧行号逐行核对）、"
          "与磁盘上的版本比较（改一个字就指出来）、日志里有记录")
    return True


def test_dialog_localization(root):
    """对话框文案进语言包：英文界面里打开首选项，一条中文都不该有。

    这是"文案进语言包"唯一的**可信**核对方式：命令能拿注册表逐条查，对话框文案没有
    静态表——只能真的把框打开、把每个控件的文本读回来。顺带验证两个机制：
    缺键自报（英文下开一遍首选项，日志里不该出现"缺键（用到时发现）"）与
    首选项里的界面语言下拉（选了就当场换语言，不重启）。
    """
    work = os.path.join(root, "build", "dlglang_test")
    os.makedirs(work, exist_ok=True)
    cfg = os.path.join(root, "build", "bin", "config")
    exe = os.path.join(root, "build", "bin", "notepadcc.exe")
    config_file = os.path.join(cfg, "config.xml")

    target = os.path.join(work, "sample.txt")
    with open(target, "w", encoding="utf-8", newline="") as handle:
        handle.write("hello\n")

    for name in ("config.xml", "session.xml"):
        path = os.path.join(cfg, name)
        if os.path.exists(path):
            os.remove(path)
    # 上一段的"导出语言包模板"会在配置目录留下一份 en-US.lang.xml。它会逐条覆盖内置的
    # 同名条目（这是约定），但本段要核对的是**内置**英文包的覆盖情况，所以先清干净
    stale_pack = os.path.join(cfg, "langs", "en-US.lang.xml")
    if os.path.exists(stale_pack):
        os.remove(stale_pack)
    log_file = remove_log_with_retry(root)

    def open_preferences(window):
        user32.PostMessageW(window, WM_COMMAND, CMD_PREFERENCES, 0)
        deadline = time.time() + 8.0
        while time.time() < deadline:
            dialog = user32.FindWindowW(PREF_DIALOG_CLASS, None)
            if dialog:
                time.sleep(0.5)
                return dialog
            time.sleep(0.2)
        return None

    proc = subprocess.Popen([exe, MULTI_INSTANCE, "-portable", target], cwd=root)
    window = wait_for_main_window("notepadcc - sample.txt")
    try:
        if not window:
            print("  FAIL: 找不到主窗口")
            return False
        time.sleep(0.8)

        # 1) 先切成英文（与界面语言那一段同一条路：设置菜单里的语言命令）
        user32.PostMessageW(window, WM_COMMAND, LANGUAGE_BASE + 1, 0)
        if not wait_menu_first_text(window, "New"):
            print("  FAIL: 切到英文失败")
            return False

        # 2) 打开首选项：每条控件文案都应当是英文
        dialog = open_preferences(window)
        if not dialog:
            print("  FAIL: 没弹出首选项对话框")
            return False
        texts = dialog_texts(dialog)
        if len(texts) < 15:
            print(f"  FAIL: 读回来的控件文案太少（{len(texts)} 条），可能是没找对控件")
            return False
        chinese = [text for text in texts if has_cjk(text)]
        if chinese:
            print(f"  FAIL: 英文界面下首选项里还有中文文案：{chinese[:6]}")
            return False
        joined = " | ".join(texts)
        # 标题不是子控件文本，单独读
        if window_text(dialog) != "Preferences":
            print(f"  FAIL: 英文界面的首选项标题应当是 Preferences，实际 "
                  f"{window_text(dialog)!r}")
            return False
        for expected in ("Restore the last session", "Tab width", "Default encoding",
                         "Snapshot interval", "OK", "Cancel"):
            if expected not in joined:
                print(f"  FAIL: 英文首选项里没有 {expected!r}\n{joined}")
                return False
        # 关掉（取消）：不改配置，但文案已经用过一遍了——缺键自报正是靠这个
        user32.PostMessageW(dialog, WM_COMMAND, PREF_CANCEL, 0)
        time.sleep(0.8)

        # 3) 缺键自报：英文包里没有的键会被记下来并写进日志。首选项的文案刚全用过一遍，
        #    所以这里没有记录就说明英文包覆盖住了那个框的全部文案
        if os.path.exists(log_file):
            with open(log_file, "r", encoding="utf-8", errors="replace") as handle:
                log = handle.read()
            missed = [line for line in log.splitlines() if "缺键（用到时发现）" in line]
            if missed:
                print(f"  FAIL: 英文包里缺对话框文案：{missed[:6]}")
                return False

        # 4) 首选项里的界面语言下拉：选中文 → 确定 → 菜单当场变回中文
        dialog = open_preferences(window)
        if not dialog:
            print("  FAIL: 第二次没弹出首选项对话框")
            return False
        combo = user32.GetDlgItem(dialog, PREF_UI_LANGUAGE_COMBO)
        if not combo:
            print("  FAIL: 首选项里没有界面语言下拉")
            return False
        if user32.SendMessageW(combo, CB_GETCOUNT, 0, 0) < 2:
            print("  FAIL: 界面语言下拉里应当至少有中文与英文两项")
            return False
        user32.SendMessageW(combo, CB_SETCURSEL, 0, 0)   # 0 = 简体中文
        time.sleep(0.3)
        user32.PostMessageW(dialog, WM_COMMAND, PREF_OK, 0)
        if not wait_menu_first_text(window, "新建"):
            print(f"  FAIL: 在首选项里选中文并确定之后菜单应当变成中文，实际 "
                  f"{menu_texts(window)[0]!r}")
            return False
        deadline = time.time() + 8.0
        wrote_back = False
        while time.time() < deadline:
            if os.path.exists(config_file):
                with open(config_file, "r", encoding="utf-8", errors="replace") as handle:
                    if 'uiLanguage="zh-CN"' in handle.read():
                        wrote_back = True
                        break
            time.sleep(0.3)
        if not wrote_back:
            print("  FAIL: 在首选项里换语言之后 config.xml 应当写回 zh-CN")
            return False
    finally:
        if window:
            user32.PostMessageW(window, WM_CLOSE, 0, 0)
            dismiss_dialog("notepadcc", IDNO, timeout=2.0, quiet=True)
            time.sleep(1.5)
        if proc.poll() is None:
            proc.kill()
        for name in ("config.xml", "session.xml"):
            path = os.path.join(cfg, name)
            if os.path.exists(path):
                os.remove(path)

    print("  对话框文案：英文界面下首选项里没有一条中文、日志里没有缺键、"
          "首选项里的界面语言下拉能当场把界面换回中文并写进配置")
    return True


def test_markdown_preview(root):
    """Markdown 预览：面板里渲染出来的**就是**解析结果，而且跟着改动实时更新。

    面板是自绘的，所以"渲染成什么样"靠 WM_GETTEXT 读回来核对（与十六进制视图同一招）。
    浏览器预览那一步会**真的**叫起默认浏览器打开生成的 HTML——它就是干这个的；
    脚本随后核对生成的那份 HTML 文件（内容对不对才是重点）。
    """
    work = os.path.join(root, "build", "md_test")
    shutil.rmtree(work, ignore_errors=True)
    os.makedirs(work)
    cfg = os.path.join(root, "build", "bin", "config")
    exe = os.path.join(root, "build", "bin", "notepadcc.exe")
    temp_dir = os.path.join(cfg, "tmp")
    config_file = os.path.join(cfg, "config.xml")

    target = os.path.join(work, "doc.md")
    source = ("# 标题一\n\n"
              "一段 **粗** 与 [链接](https://example.com)。\n\n"
              "- 第一项\n"
              "- 第二项\n\n"
              "1. 有序一\n\n"
              "> 引用一行\n\n"
              "```\nint main(void) { return 0; }\n```\n\n"
              "---\n")
    with open(target, "w", encoding="utf-8", newline="") as handle:
        handle.write(source)

    for name in ("config.xml", "session.xml"):
        path = os.path.join(cfg, name)
        if os.path.exists(path):
            os.remove(path)
    if os.path.isdir(temp_dir):
        for name in os.listdir(temp_dir):
            if name.startswith("preview-"):
                os.remove(os.path.join(temp_dir, name))
    log_file = remove_log_with_retry(root)

    def panel_lines(window, timeout=8.0):
        """等面板出现并读回它渲染的那几行。"""
        deadline = time.time() + timeout
        while time.time() < deadline:
            panel = find_child(window, MD_PREVIEW_CLASS)
            if panel and user32.IsWindowVisible(panel):
                text = window_text(panel)
                if text:
                    return panel, text.split("\r\n")
            time.sleep(0.2)
        return None, []

    proc = subprocess.Popen([exe, MULTI_INSTANCE, "-portable", target], cwd=root)
    window = wait_for_main_window("notepadcc - doc.md")
    try:
        if not window:
            print("  FAIL: 找不到主窗口")
            return False
        time.sleep(0.8)

        # 1) 开面板：核对面板里是**渲染后**的文本，不是 Markdown 源码
        user32.PostMessageW(window, WM_COMMAND, CMD_VIEW_MARKDOWN_PREVIEW, 0)
        panel, lines = panel_lines(window)
        if not panel:
            print("  FAIL: 打开 Markdown 预览面板之后没看到那个控件")
            return False
        rendered = "\n".join(lines)
        for expected in ("标题一", "一段 粗 与 链接。", "\u2022 第一项", "\u2022 第二项",
                         "1. 有序一", "\u2502 引用一行",
                         "int main(void) { return 0; }"):
            if expected not in rendered:
                print(f"  FAIL: 面板里没有 {expected!r}\n{rendered}")
                return False
        # 源码里的记号不该漏到渲染结果里
        for unwanted in ("**", "# ", "```", "[链接](", "](https"):
            if unwanted in rendered:
                print(f"  FAIL: 面板里漏出了 Markdown 记号 {unwanted!r}\n{rendered}")
                return False
        if not lines[0].startswith("标题一"):
            print(f"  FAIL: 第一行应当是渲染后的标题，实际 {lines[0]!r}")
            return False

        # 2) 实时：在文档末尾敲一行新的标题，节流到期后面板里应当出现它（不带 #）
        scinti = visible_scintilla(window)
        length = user32.SendMessageW(scinti, SCI_GETLENGTH, 0, 0)
        user32.SendMessageW(scinti, SCI_GOTOPOS, length, 0)
        for ch in "\n## LiveHeading\n":
            user32.PostMessageW(scinti, WM_CHAR_INPUT, ord(ch), 0)
        time.sleep(1.2)   # 节流是 300 ms，这里等宽裕一点
        _, lines = panel_lines(window)
        if "LiveHeading" not in "\n".join(lines):
            print(f"  FAIL: 改了文档之后预览没有跟着更新\n{chr(10).join(lines)}")
            return False
        if "## LiveHeading" in "\n".join(lines):
            print("  FAIL: 新标题应当渲染成不带 # 的标题")
            return False

        # 3) 浏览器预览：核对生成的那份 HTML（这一步会真的打开浏览器）
        user32.PostMessageW(window, WM_COMMAND, CMD_VIEW_PREVIEW_BROWSER, 0)
        deadline = time.time() + 10.0
        generated = None
        while time.time() < deadline:
            if os.path.isdir(temp_dir):
                candidates = [os.path.join(temp_dir, name) for name in os.listdir(temp_dir)
                              if name.startswith("preview-") and name.endswith(".html")]
                if candidates:
                    generated = max(candidates, key=os.path.getmtime)
                    break
            time.sleep(0.3)
        if not generated:
            print("  FAIL: 浏览器预览没有生成 HTML 文件")
            return False
        with open(generated, "r", encoding="utf-8", errors="replace") as handle:
            html = handle.read()
        for expected in ("<!DOCTYPE html>", "<h1>标题一</h1>", "<strong>粗</strong>",
                         "<a href=\"https://example.com\">链接</a>", "<hr>", "<blockquote>"):
            if expected not in html:
                print(f"  FAIL: 生成的 HTML 里没有 {expected!r}\n{html[:400]}")
                return False
        if os.path.exists(log_file):
            with open(log_file, "r", encoding="utf-8", errors="replace") as handle:
                log = handle.read()
            if "Markdown 预览已生成" not in log:
                print("  FAIL: 日志里应当留下生成预览的记录")
                return False

        # 4) 再按一次收起来，并且开关记进配置（下次启动自己回来）
        user32.PostMessageW(window, WM_COMMAND, CMD_VIEW_MARKDOWN_PREVIEW, 0)
        time.sleep(0.8)
        panel = find_child(window, MD_PREVIEW_CLASS)
        if panel and user32.IsWindowVisible(panel):
            print("  FAIL: 再按一次应当把面板收起来")
            return False
        user32.PostMessageW(window, WM_COMMAND, CMD_VIEW_MARKDOWN_PREVIEW, 0)
        time.sleep(0.8)
        deadline = time.time() + 8.0
        remembered = False
        while time.time() < deadline:
            if os.path.exists(config_file):
                with open(config_file, "r", encoding="utf-8", errors="replace") as handle:
                    if 'markdownPreview="yes"' in handle.read():
                        remembered = True
                        break
            time.sleep(0.3)
        if not remembered:
            print("  FAIL: 面板的开关应当记进 config.xml（markdownPreview）")
            return False
    finally:
        if window:
            user32.PostMessageW(window, WM_CLOSE, 0, 0)
            dismiss_dialog("notepadcc", IDNO, timeout=2.0, quiet=True)
            time.sleep(1.5)
        if proc.poll() is None:
            proc.kill()
        for name in ("config.xml", "session.xml"):
            path = os.path.join(cfg, name)
            if os.path.exists(path):
                os.remove(path)

    print("  Markdown 预览：面板里是渲染后的文本（标题/列表/引用/代码/链接都不带记号）、"
          "改了文档实时跟着更新、在浏览器中打开预览生成的 HTML 内容正确、开关记进配置")
    return True


def read_clipboard(format_id):
    """读剪贴板里某个格式的**原始字节**（读不到返回 None）。

    刻意不"读到第一个 NUL 就停"：那样会把"末尾多出一堆空字符"这种问题藏起来——
    本轮就抓到过（导出与剪贴板都拖着这些空字符），所以调用方自己按长度核对。
    """
    if not user32.OpenClipboard(None):
        return None
    try:
        handle = user32.GetClipboardData(format_id)
        if not handle:
            return None
        size = kernel32.GlobalSize(handle)
        pointer = kernel32.GlobalLock(handle)
        if not pointer:
            return None
        raw = ctypes.string_at(pointer, size)
        kernel32.GlobalUnlock(handle)
        return raw
    finally:
        user32.CloseClipboard()


def read_clipboard_rtf():
    fmt = user32.RegisterClipboardFormatW("Rich Text Format")
    raw = read_clipboard(fmt)
    if raw is None:
        return None
    return raw.rstrip(b"\x00").decode("ascii", errors="replace")


def test_copy_as_rtf(root):
    """复制为富文本：剪贴板里**同时**有纯文本与 RTF，两边的内容都对得上。

    这一段会占用系统剪贴板（复制本来就是这个行为）。
    """
    work = os.path.join(root, "build", "rtf_test")
    os.makedirs(work, exist_ok=True)
    cfg = os.path.join(root, "build", "bin", "config")
    exe = os.path.join(root, "build", "bin", "notepadcc.exe")

    utf8Path = os.path.join(work, "sample.cpp")
    utf8Source = "// 中文注释\nint value = 42;\n"
    with open(utf8Path, "w", encoding="utf-8", newline="") as handle:
        handle.write(utf8Source)
    # ANSI（GBK）那份：中文按 GBK 写进去，程序会把它识别成 ANSI 文档。
    # 这一份专门核对"缓冲区字节按代码页解成 UTF-8"那条路（不转换的话粘出来是乱码）
    gbkPath = os.path.join(work, "gbk.txt")
    gbkText = "中文注释\n"
    with open(gbkPath, "wb") as handle:
        handle.write(gbkText.encode("gbk"))

    def clean_config():
        for name in ("config.xml", "session.xml"):
            path = os.path.join(cfg, name)
            if os.path.exists(path):
                os.remove(path)

    def find_scintilla(window):
        for child in children(window):
            if class_name(child) == "Scintilla" and user32.IsWindowVisible(child):
                return child
        return None

    clean_config()
    proc = subprocess.Popen([exe, MULTI_INSTANCE, "-portable", utf8Path], cwd=root)
    window = wait_for_main_window("notepadcc - sample.cpp")
    try:
        if not window:
            print("  FAIL: 找不到主窗口")
            return False
        time.sleep(0.8)
        scinti = find_scintilla(window)
        if scinti is None:
            print("  FAIL: 找不到编辑控件")
            return False

        # 1) 选中第一行（含中文），复制为富文本
        selection = "// 中文注释"
        user32.SendMessageW(scinti, SCI_SETSEL, 0, len(selection.encode("utf-8")))
        time.sleep(0.3)
        user32.PostMessageW(window, WM_COMMAND, CMD_EDIT_COPY_AS_RTF, 0)
        time.sleep(1.2)

        raw = read_clipboard(CF_UNICODETEXT)
        if raw is None:
            print("  FAIL: 剪贴板里没有纯文本")
            return False
        # 精确长度：多出来的字符（比如一堆 NUL）在这里就会被抓住，而不是靠"读到 NUL 为止"
        if len(raw) != (len(selection) + 1) * 2:
            print(f"  FAIL: 剪贴板纯文本的长度不对：{len(raw)} 字节"
                  f"（期望 {(len(selection) + 1) * 2}）")
            return False
        plain = raw.decode("utf-16-le")
        if plain != selection + "\x00":
            print(f"  FAIL: 剪贴板纯文本不对 {plain!r}")
            return False

        rtf = read_clipboard_rtf()
        if rtf is None:
            print("  FAIL: 剪贴板里没有 \"Rich Text Format\" 格式")
            return False
        if not rtf.startswith("{\\rtf1"):
            print(f"  FAIL: RTF 开头不对 {rtf[:40]!r}")
            return False
        # cpp 的注释样式（内置配色是 #2E7D32）与斜体：证明带的是**语法配色**，不是纯文本
        if "\\colortbl" not in rtf or "\\red46\\green125\\blue50" not in rtf:
            print(f"  FAIL: RTF 里没有注释样式的颜色\n{rtf}")
            return False
        # 注释样式是斜体：RTF 里应当有 \i（开）与 \i0（关）这一对
        if "\\i" not in rtf or "\\i0" not in rtf:
            print(f"  FAIL: RTF 里没有斜体（注释样式是斜体）\n{rtf}")
            return False
        if "\\u20013?" not in rtf or "\\u25991?" not in rtf:
            print(f"  FAIL: RTF 里的中文不对（应当是 \\u20013?\\u25991?…）\n{rtf}")
            return False
        if "\\u0?" in rtf:
            print(f"  FAIL: RTF 里出现了 NUL 字符（读样式文本时多读了空字节）\n{rtf}")
            return False

        # 2) 没有选区时整篇：纯文本应当正好是整份文件
        user32.SendMessageW(scinti, SCI_SETSEL, 0, 0)
        time.sleep(0.3)
        user32.PostMessageW(window, WM_COMMAND, CMD_EDIT_COPY_AS_RTF, 0)
        time.sleep(1.2)
        raw = read_clipboard(CF_UNICODETEXT)
        if raw is None or raw.decode("utf-16-le").rstrip("\x00") != utf8Source:
            actual = raw.decode("utf-16-le") if raw else None
            print(f"  FAIL: 没有选区时应当复制整篇，实际 {actual!r}")
            return False
    finally:
        if window:
            user32.PostMessageW(window, WM_CLOSE, 0, 0)
            dismiss_dialog("notepadcc", IDNO, timeout=2.0, quiet=True)
            time.sleep(1.5)
        if proc.poll() is None:
            proc.kill()

    # 3) ANSI（GBK）文档：剪贴板里的中文必须是正确的、RTF 里的字也对
    clean_config()
    proc = subprocess.Popen([exe, MULTI_INSTANCE, "-portable", gbkPath], cwd=root)
    window = wait_for_main_window("notepadcc - gbk.txt")
    try:
        if not window:
            print("  FAIL: 找不到第二个实例的主窗口")
            return False
        time.sleep(0.8)
        scinti = find_scintilla(window)
        if scinti is None:
            print("  FAIL: 找不到编辑控件")
            return False
        user32.SendMessageW(scinti, SCI_SETSEL, 0, 0)
        time.sleep(0.3)
        user32.PostMessageW(window, WM_COMMAND, CMD_EDIT_COPY_AS_RTF, 0)
        time.sleep(1.2)
        raw = read_clipboard(CF_UNICODETEXT)
        if raw is None or raw.decode("utf-16-le").rstrip("\x00") != gbkText:
            actual = raw.decode("utf-16-le") if raw else None
            print(f"  FAIL: GBK 文档的纯文本不对：{actual!r}（期望 {gbkText!r}）")
            return False
        rtf = read_clipboard_rtf()
        if rtf is None or "\\u20013?" not in rtf:
            print(f"  FAIL: GBK 文档的 RTF 里「中」是错的（按代码页解那一步没生效）\n{rtf}")
            return False
    finally:
        if window:
            user32.PostMessageW(window, WM_CLOSE, 0, 0)
            dismiss_dialog("notepadcc", IDNO, timeout=2.0, quiet=True)
            time.sleep(1.5)
        if proc.poll() is None:
            proc.kill()
        for name in ("config.xml", "session.xml"):
            path = os.path.join(cfg, name)
            if os.path.exists(path):
                os.remove(path)

    print("  复制为富文本：剪贴板里同时有纯文本与 RTF（纯文本按精确长度核对）、"
          "RTF 带语法配色与斜体、中文是正确的字符、没有多余的空字符；"
          "没有选区时复制整篇；ANSI(GBK) 文档也能正确转换")
    return True


def hex_row_text(data, offset, width=8):
    """按界面上那一行该怎么排，算出期望的文本（偏移 → 十六进制 → ASCII）。

    这是**格式的规格**，不是把实现抄一遍：偏移 8 位大写、每字节两位大写、第 8 个字节后
    多一个空档、不满一行时十六进制列补空格、ASCII 里非可打印一律 '.'。
    单元测试里已经用手写死的整串钉过排版，这里用它来核对"滚到这一屏时看到的是不是
    那个偏移上的字节"。
    """
    row = data[offset:offset + 16]
    hex_part = ""
    for index in range(16):
        hex_part += f"{row[index]:02X}" if index < len(row) else "  "
        if index != 15:
            hex_part += " "
        if index == 7:
            hex_part += " "
    ascii_part = "".join(chr(byte) if 0x20 <= byte <= 0x7E else "." for byte in row)
    return f"{offset:0{width}X}  " + hex_part + "  " + ascii_part


def test_hex_view(root):
    """十六进制查看：屏幕上那几行**就是**文件里的字节，能滚、能跳。

    读回来的方式是跨进程发 WM_GETTEXT（系统消息会被封送），控件把"此刻渲染的那几行"
    交出来——所以核对的是屏幕上真正显示的内容，不是"命令发出去了"。
    """
    work = os.path.join(root, "build", "hex_test")
    os.makedirs(work, exist_ok=True)
    cfg = os.path.join(root, "build", "bin", "config")
    exe = os.path.join(root, "build", "bin", "notepadcc.exe")
    log_file = os.path.join(cfg, "logs", "notepadcc.log")

    # 造一个 4096 字节的文件：前 32 字节是认得出的图案，其余是可滚动的填充。
    # 里面故意放上 0x00、0x7F、0x80、0xFF 与可打印字符，好把 ASCII 那一列的规则也测到
    head = bytes([0x4A, 0x6F, 0x20, 0x62, 0x69, 0x6E, 0x00, 0x00,
                  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF,
                  0x7F, 0x80, 0x41, 0x42, 0x43, 0x00, 0x7E, 0x20,
                  0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08])
    data = head + bytes((index * 7 + 3) & 0xFF for index in range(4096 - len(head)))
    target = os.path.join(work, "blob.bin")
    with open(target, "wb") as handle:
        handle.write(data)

    for name in ("config.xml", "session.xml"):
        path = os.path.join(cfg, name)
        if os.path.exists(path):
            os.remove(path)
    log_file = remove_log_with_retry(root)

    def read_hex(window, timeout=6.0):
        """等十六进制视图出现并读回它渲染的那几行。"""
        deadline = time.time() + timeout
        while time.time() < deadline:
            view = find_child(window, HEX_VIEW_CLASS)
            if view and user32.IsWindowVisible(view):
                text = window_text(view)
                if text:
                    return view, text
            time.sleep(0.2)
        return None, ""

    proc = subprocess.Popen([exe, MULTI_INSTANCE, "-portable", target], cwd=root)
    window = wait_for_main_window("notepadcc - blob.bin")
    try:
        if not window:
            print("  FAIL: 找不到主窗口")
            return False
        time.sleep(0.8)

        # 1) 切到十六进制视图：屏幕上第一行应当**逐字**等于文件开头的 16 个字节
        user32.PostMessageW(window, WM_COMMAND, CMD_VIEW_HEX_TOGGLE, 0)
        view, text = read_hex(window)
        if not view:
            print("  FAIL: 切到十六进制视图之后没看到那个控件")
            return False
        lines = text.split("\r\n")
        expected_first = hex_row_text(data, 0)
        if lines[0] != expected_first:
            print(f"  FAIL: 第一行不对\n           实际 {lines[0]!r}\n"
                  f"           期望 {expected_first!r}")
            return False
        if len(lines) < 2 or lines[1] != hex_row_text(data, 16):
            print(f"  FAIL: 第二行不对（期望偏移 0x10 上的字节）\n           实际 {lines[1]!r}")
            return False
        # 文件里的 0x00/0x7F/0x80 一个都不该原样出现在渲染结果里
        if "\x00" in text or "\x7f" in text:
            print("  FAIL: 控制字符被原样渲染出来了")
            return False

        # 2) 翻页：左边那一列要往后走，而且内容必须是那个偏移上真实存在的字节
        user32.PostMessageW(view, WM_VSCROLL, SB_PAGEDOWN, 0)
        time.sleep(0.5)
        _, after_page = read_hex(window)
        first_after = after_page.split("\r\n")[0]
        offset_after = int(first_after.split("  ")[0], 16)
        if offset_after == 0:
            print("  FAIL: 翻页之后偏移没动")
            return False
        if offset_after % 16 != 0:
            print(f"  FAIL: 翻页之后偏移不是 16 的整数倍：{offset_after}")
            return False
        if first_after != hex_row_text(data, offset_after):
            print(f"  FAIL: 翻页之后那一行的内容与文件对不上\n           实际 {first_after!r}\n"
                  f"           期望 {hex_row_text(data, offset_after)!r}")
            return False

        # 3) 跳到偏移：0x800（2048），同样核对内容
        user32.PostMessageW(window, WM_COMMAND, CMD_VIEW_HEX_GOTO, 0)
        time.sleep(0.6)
        dialog = user32.FindWindowW(HEX_GOTO_CLASS, None)
        if not dialog:
            print("  FAIL: 应该弹出「跳到偏移」的输入框")
            return False
        set_text(dialog, MACRO_EDIT, "0x800")
        time.sleep(0.3)
        if not close_macro_dialog(HEX_GOTO_CLASS):
            print("  FAIL: 跳到偏移的框点不掉")
            return False
        time.sleep(0.6)
        _, after_jump = read_hex(window)
        first_jump = after_jump.split("\r\n")[0]
        if not first_jump.startswith("00000800"):
            print(f"  FAIL: 跳到 0x800 之后第一行应当是 00000800 开头，实际 {first_jump!r}")
            return False
        if first_jump != hex_row_text(data, 0x800):
            print(f"  FAIL: 跳过去之后那一行的内容与文件对不上\n           实际 {first_jump!r}\n"
                  f"           期望 {hex_row_text(data, 0x800)!r}")
            return False

        # 4) 非法输入要被挡下，并且停在原地
        user32.PostMessageW(window, WM_COMMAND, CMD_VIEW_HEX_GOTO, 0)
        time.sleep(0.6)
        dialog = user32.FindWindowW(HEX_GOTO_CLASS, None)
        if not dialog:
            print("  FAIL: 第二次跳转没有弹出输入框")
            return False
        set_text(dialog, MACRO_EDIT, "这不是偏移")
        time.sleep(0.3)
        if not close_macro_dialog(HEX_GOTO_CLASS):
            print("  FAIL: 输入框点不掉")
            return False
        if not dismiss_dialog("跳到偏移", IDOK, timeout=8.0):
            print("  FAIL: 看不懂的偏移应当给一句说明")
            return False
        time.sleep(0.5)
        _, after_bad = read_hex(window)
        if not after_bad.split("\r\n")[0].startswith("00000800"):
            print(f"  FAIL: 非法输入之后不该乱跳，实际 {after_bad.splitlines()[0]!r}")
            return False

        # 5) 切回文本：十六进制控件收起来，编辑控件回来
        user32.PostMessageW(window, WM_COMMAND, CMD_VIEW_HEX_TOGGLE, 0)
        time.sleep(0.8)
        hex_visible = False
        for child in children(window):
            if class_name(child) == HEX_VIEW_CLASS and user32.IsWindowVisible(child):
                hex_visible = True
        if hex_visible:
            print("  FAIL: 切回文本之后十六进制控件应当收起来")
            return False
        if visible_scintilla(window) is None:
            print("  FAIL: 切回文本之后编辑控件应当回来")
            return False

        # 6) 改过内容（未保存）的文档不给开：它读的是磁盘字节，两边对不上
        scinti = visible_scintilla(window)
        user32.PostMessageW(scinti, WM_CHAR_INPUT, ord("x"), 0)
        time.sleep(0.4)
        user32.PostMessageW(window, WM_COMMAND, CMD_VIEW_HEX_TOGGLE, 0)
        if not dismiss_dialog("十六进制查看", IDOK, timeout=8.0):
            print("  FAIL: 有未保存改动时应当说明为什么不给开")
            return False
        time.sleep(0.4)
        for child in children(window):
            if class_name(child) == HEX_VIEW_CLASS and user32.IsWindowVisible(child):
                print("  FAIL: 被拦下之后不该进十六进制视图")
                return False
    finally:
        if window:
            user32.PostMessageW(window, WM_CLOSE, 0, 0)
            dismiss_dialog("notepadcc", IDNO, timeout=2.0, quiet=True)
            time.sleep(1.5)
        if proc.poll() is None:
            proc.kill()

    with open(log_file, "r", encoding="utf-8", errors="replace") as handle:
        log = handle.read()
    if "以十六进制打开" not in log:
        print("  FAIL: 日志里应当留下打开十六进制视图的记录")
        return False
    for name in ("config.xml", "session.xml"):
        path = os.path.join(cfg, name)
        if os.path.exists(path):
            os.remove(path)

    print("  十六进制查看：屏幕上的行逐字等于文件字节（含末行补齐与 ASCII 替换）、"
          "翻页与跳到偏移后内容仍然对得上、非法偏移被挡下、切回文本正常、改过的文档不开")
    return True


def test_ui_language(root):
    """界面语言：菜单真的换成英文、不重启就生效、记住选择、英文界面里没有中文残留。

    核对的是**挂在窗口上的菜单文本**（跨进程读得回来，见 menu_texts 的说明），
    不是配置文件里写了什么——"配置写对了但界面没换"正是这一类功能最容易出的问题。
    """
    work = os.path.join(root, "build", "lang_test")
    os.makedirs(work, exist_ok=True)
    cfg = os.path.join(root, "build", "bin", "config")
    exe = os.path.join(root, "build", "bin", "notepadcc.exe")
    config_file = os.path.join(cfg, "config.xml")
    exported = os.path.join(cfg, "langs", "en-US.lang.xml")

    def clean_config():
        for name in ("config.xml", "session.xml", "shortcuts.xml"):
            target = os.path.join(cfg, name)
            if os.path.exists(target):
                os.remove(target)
        if os.path.exists(exported):
            os.remove(exported)

    def read_config():
        if not os.path.exists(config_file):
            return ""
        with open(config_file, "r", encoding="utf-8", errors="replace") as handle:
            return handle.read()

    def wait_menu_first(window, expected, timeout=6.0):
        deadline = time.time() + timeout
        while time.time() < deadline:
            texts = menu_texts(window)
            if texts and texts[0].startswith(expected):
                return True
            time.sleep(0.2)
        return False

    def wait_config(needle, timeout=8.0):
        """等配置落盘。配置是**合并写盘**的（1 秒窗口内的改动合成一次），所以
        切完语言立刻读文件可能还是旧值——这里等条件成立，不睡固定时间。
        """
        deadline = time.time() + timeout
        while time.time() < deadline:
            if needle in read_config():
                return True
            time.sleep(0.25)
        return False

    target = os.path.join(work, "sample.txt")
    with open(target, "w", encoding="utf-8", newline="") as handle:
        handle.write("hello\n")

    clean_config()
    # 日志里会留下**以前那些次**运行的记录（比如修好之前跑出来的"缺键"），
    # 所以这一段开头先把它删干净，只看这一次的
    log_file = remove_log_with_retry(root)
    proc = subprocess.Popen([exe, MULTI_INSTANCE, "-portable", target], cwd=root)
    window = wait_for_main_window("notepadcc - sample.txt")
    try:
        if not window:
            print("  FAIL: 找不到主窗口")
            return False
        time.sleep(0.6)
        first = menu_texts(window)
        if not first[0].startswith("新建"):
            print(f"  FAIL: 默认界面语言应当是简体中文，实际第一项 {first[0]!r}")
            return False
        titles = menu_bar_titles(window)
        if not titles or not titles[0].startswith("文件"):
            print(f"  FAIL: 菜单栏第一项应当是「文件」，实际 {titles[:3]!r}")
            return False

        # 1) 先触发"导出语言包模板"：配置文件不落盘也能拿到一份可改的模板
        user32.PostMessageW(window, WM_COMMAND, CMD_TOOL_EXPORT_LANGPACK, 0)
        if not dismiss_dialog("导出语言包", IDOK, timeout=8.0):
            print("  FAIL: 导出语言包模板之后应当弹出说明")
            return False
        if not os.path.exists(exported):
            print(f"  FAIL: 没有导出语言包模板 {exported}")
            return False
        with open(exported, "r", encoding="utf-8", errors="replace") as handle:
            template = handle.read()
        if '<LangPack locale="en-US"' not in template or "<Entry key=" not in template:
            print(f"  FAIL: 导出的模板内容不对\n{template[:300]}")
            return False

        # 2) 切成英文：不重启，菜单当场就该换
        user32.PostMessageW(window, WM_COMMAND, LANGUAGE_BASE + 1, 0)
        if not wait_menu_first(window, "New"):
            print(f"  FAIL: 切到英文之后菜单第一项还是 {menu_texts(window)[0]!r}")
            return False
        english = menu_texts(window)
        if not english[2].startswith("Find"):
            print(f"  FAIL: 英文菜单里搜索那一项应当是 Find...，实际 {english[2]!r}")
            return False
        titles = menu_bar_titles(window)
        if not titles or not titles[0].startswith("File"):
            print(f"  FAIL: 英文界面的菜单栏第一项应当是 File，实际 {titles[:3]!r}")
            return False

        # 3) 内置的那几类菜单里一个字的中文都不该有（宏/运行/插件里是用户数据，跳过）。
        #    唯一的例外是语言自己的名字：英文界面里的那一条就叫「简体中文」——
        #    语言名要用它自己的语言写，用户才找得到（这是刻意的，不是漏翻）
        leftovers = []
        for index in (0, 1, 2, 3, 4, 5, 6, 7, 8, 12):
            for item in menu_items(window, index):
                if item and has_cjk(item) and item != "简体中文":
                    leftovers.append(item)
        if leftovers:
            print(f"  FAIL: 英文界面里还有中文菜单项：{leftovers[:6]}")
            return False

        # 4) 配置里记下了选择，日志里没有"缺键"（缺键会中英混排，前面那条断言也会抓到，
        #    但日志能直接指出是哪一个键）
        if not wait_config('uiLanguage="en-US"'):
            print(f"  FAIL: config.xml 里没有记下 uiLanguage=\"en-US\"\n{read_config()[:400]}")
            return False
        if os.path.exists(log_file):
            with open(log_file, "r", encoding="utf-8", errors="replace") as handle:
                log = handle.read()
            missed = [line for line in log.splitlines()
                      if "缺键" in line and "语言包 en-US" in line]
            if missed:
                print(f"  FAIL: 英文包里缺键：{missed[:4]}")
                return False
            # 顺手把启动自检的另一条也读掉：命令 ID 撞车时后来的那条命令会被丢掉，
            # 表现是"菜单里有这一项、点了没反应"——本轮就踩着过一次（视图类 15/16 号
            # 早被工作区命令占了），所以这条警告必须有东西盯着
            duplicated = [line for line in log.splitlines() if "命令 ID 重复" in line]
            if duplicated:
                print(f"  FAIL: 启动时有命令 ID 撞车，后来的命令被丢掉了：{duplicated[:4]}")
                return False

        # 5) 再切回中文：不重启也该回来
        user32.PostMessageW(window, WM_COMMAND, LANGUAGE_BASE + 0, 0)
        if not wait_menu_first(window, "新建"):
            print(f"  FAIL: 切回中文之后菜单第一项还是 {menu_texts(window)[0]!r}")
            return False
        if not wait_config('uiLanguage="zh-CN"'):
            print("  FAIL: 切回中文之后配置里应当写回 zh-CN")
            return False
        # 最后再切回英文，留给"重启后仍然记得"那一步
        user32.PostMessageW(window, WM_COMMAND, LANGUAGE_BASE + 1, 0)
        if not wait_menu_first(window, "New"):
            print("  FAIL: 第二次切英文失败")
            return False
    finally:
        if window:
            user32.PostMessageW(window, WM_CLOSE, 0, 0)
            dismiss_dialog("notepadcc", IDNO, timeout=2.0, quiet=True)
            time.sleep(1.5)
        if proc.poll() is None:
            proc.kill()

    # 6) 重启：语言选择记在配置里，而且这一次配置目录里躺着一份"外置英文包"
    #    （上一步导出的模板）——顺带验证外置包能被读进来
    session = os.path.join(cfg, "session.xml")
    if os.path.exists(session):
        os.remove(session)
    proc = subprocess.Popen([exe, MULTI_INSTANCE, "-portable", target], cwd=root)
    window = wait_for_main_window("notepadcc - sample.txt")
    try:
        if not window:
            print("  FAIL: 重启后找不到主窗口")
            return False
        if not wait_menu_first(window, "New"):
            print(f"  FAIL: 重启后界面语言应当还是英文，实际 {menu_texts(window)[0]!r}")
            return False
        with open(log_file, "r", encoding="utf-8", errors="replace") as handle:
            log = handle.read()
        if "语言包 en-US.lang.xml 读不了" in log:
            print("  FAIL: 导出的模板应当能被当作外置语言包读回来")
            return False
    finally:
        if window:
            user32.PostMessageW(window, WM_CLOSE, 0, 0)
            dismiss_dialog("notepadcc", IDNO, timeout=2.0, quiet=True)
            time.sleep(1.5)
        if proc.poll() is None:
            proc.kill()
        for name in ("config.xml", "session.xml"):
            path = os.path.join(cfg, name)
            if os.path.exists(path):
                os.remove(path)

    print("  界面语言：菜单真的换成英文（菜单栏与菜单项逐条核对、英文界面里没有中文残留）、"
          "不重启即生效、选择记进 config.xml 且重启后保持、导出的模板能当外置包读回来")
    return True


def test_run_command(root):
    """运行命令（F5）：变量真的展开了、输出真的收进新文档、存的命令真的能从文件读回来。

    这一段刻意**不**只看"命令发出去了"：每条命令都要求出现真实证据——落盘的 side.txt
    里有展开后的字面量、输出文档里有命令自己打印的那一行。变量展开错了、工作目录不对、
    捕获管道接错，这三种失败都只会让证据消失，不会让断言"看起来还行"。
    """
    work = os.path.join(root, "build", "run_test")
    os.makedirs(work, exist_ok=True)
    cfg = os.path.join(root, "build", "bin", "config")
    exe = os.path.join(root, "build", "bin", "notepadcc.exe")
    shortcuts = os.path.join(cfg, "shortcuts.xml")

    def clean_config():
        for name in ("config.xml", "session.xml", "shortcuts.xml"):
            target = os.path.join(cfg, name)
            if os.path.exists(target):
                os.remove(target)

    def open_dialog(window, command, capture):
        """发 F5、填命令行、设好捕获开关；返回对话框句柄（没等到就是 None）。"""
        user32.PostMessageW(window, WM_COMMAND, CMD_RUN_DIALOG, 0)
        dialog = None
        deadline = time.time() + 8.0
        while time.time() < deadline:
            dialog = user32.FindWindowW(RUN_DIALOG_CLASS, None)
            if dialog:
                break
            time.sleep(0.2)
        if not dialog:
            return None
        time.sleep(0.3)
        set_text(dialog, RUN_COMBO, command)
        send_bounded(user32.GetDlgItem(dialog, RUN_CAPTURE), BM_SETCHECK, 1 if capture else 0, 0)
        return dialog

    def wait_dialog_gone(timeout=10.0):
        deadline = time.time() + timeout
        while time.time() < deadline:
            if not user32.FindWindowW(RUN_DIALOG_CLASS, None):
                return True
            time.sleep(0.2)
        return False

    def wait_output(window, needle, timeout=25.0):
        """等"捕获型命令跑完并开出了输出文档"：返回那份文档的全文，等不到返回 None。"""
        deadline = time.time() + timeout
        while time.time() < deadline:
            scinti = visible_scintilla(window)
            if scinti is not None:
                text = scinti_text(scinti)
                if needle in text and "[运行结束：退出码" in text:
                    return text
            time.sleep(0.3)
        return None

    sample = os.path.join(work, "sample.txt")
    with open(sample, "w", encoding="utf-8", newline="") as handle:
        handle.write("hello\n")
    side = os.path.join(work, "side.txt")
    if os.path.exists(side):
        os.remove(side)

    clean_config()
    proc = subprocess.Popen([exe, MULTI_INSTANCE, "-portable", sample], cwd=root)
    window = wait_for_main_window("notepadcc - sample.txt")
    try:
        if not window:
            print("  FAIL: 找不到主窗口")
            return False
        scinti = visible_scintilla(window)
        if scinti is None or scinti_text(scinti) != "hello\n":
            print(f"  FAIL: 初始内容不对 {scinti_text(scinti)!r}")
            return False

        # 1) 先按一次真的 F5（走加速键表那一层），核对对话框弹出来了
        user32.PostMessageW(window, WM_KEYDOWN, VK_F5, 0)
        dialog = None
        deadline = time.time() + 6.0
        while time.time() < deadline:
            dialog = user32.FindWindowW(RUN_DIALOG_CLASS, None)
            if dialog:
                break
            time.sleep(0.2)
        if not dialog:
            print("  FAIL: 按 F5 应当弹出运行命令对话框")
            return False
        user32.PostMessageW(dialog, WM_COMMAND, RUN_CANCEL, 0)
        if not wait_dialog_gone():
            print("  FAIL: 取消之后对话框没关掉")
            return False

        # 2) 不勾捕获：变量展开 + 工作目录（= 当前文件所在目录）
        dialog = open_dialog(window, "cmd /c echo $(NAME_PART)-$(CURRENT_LINE) > side.txt", False)
        if not dialog:
            print("  FAIL: 第二次 F5 没有弹出对话框")
            return False
        user32.PostMessageW(dialog, WM_COMMAND, RUN_GO, 0)
        if not wait_dialog_gone():
            print("  FAIL: 点了运行之后对话框没关掉")
            return False
        deadline = time.time() + 20.0
        while time.time() < deadline and not os.path.exists(side):
            time.sleep(0.3)
        if not os.path.exists(side):
            print("  FAIL: 命令没有跑起来（build\\run_test\\side.txt 没出现）")
            return False
        with open(side, "r", encoding="utf-8", errors="replace") as handle:
            produced = handle.read().strip()
        if produced != "sample-1":
            print(f"  FAIL: 变量展开不对，side.txt 里是 {produced!r}（期望 'sample-1'）")
            return False
        if len(visible_scintillas_sorted(window)) != 1:
            print("  FAIL: 不勾选捕获时不该多出一个输出文档")
            return False

        # 3) 勾上捕获：输出进新文档，末尾带退出码
        dialog = open_dialog(window, "cmd /c echo 运行输出-$(FILE_NAME)", True)
        if not dialog:
            print("  FAIL: 第三次 F5 没有弹出对话框")
            return False
        user32.PostMessageW(dialog, WM_COMMAND, RUN_GO, 0)
        if not wait_dialog_gone():
            print("  FAIL: 对话框没关掉")
            return False
        text = wait_output(window, "运行输出-sample.txt")
        if text is None:
            print("  FAIL: 没等到带结果的运行输出文档（输出没进新文档？）")
            return False
        if "[运行结束：退出码 0" not in text:
            print(f"  FAIL: 输出文档末尾应当有退出码说明\n{text!r}")
            return False
        if user32.SendMessageW(visible_scintilla(window), SCI_GETMODIFY, 0, 0) != 0:
            print("  FAIL: 刚生成的输出文档不该是「已修改」状态")
            return False

        # 4) 保存成命名命令：写进 shortcuts.xml，用户命令不进 <InternalCommands>
        dialog = open_dialog(window, "cmd /c echo saved-$(CURRENT_LINE)", True)
        if not dialog:
            print("  FAIL: 第四次 F5 没有弹出对话框")
            return False
        user32.PostMessageW(dialog, WM_COMMAND, RUN_SAVE, 0)
        time.sleep(0.6)
        name_dialog = user32.FindWindowW(MACRO_NAME_CLASS, None)
        if not name_dialog:
            print("  FAIL: 点「保存为命令」之后应当弹出取名框")
            return False
        set_text(name_dialog, MACRO_EDIT, "我的命令")
        time.sleep(0.3)
        if not close_macro_dialog(MACRO_NAME_CLASS):
            print("  FAIL: 取名框点不掉")
            return False
        if not wait_dialog_gone():
            print("  FAIL: 保存之后运行命令对话框没关掉")
            return False
        time.sleep(0.8)
        if not os.path.exists(shortcuts):
            print(f"  FAIL: 保存命令之后没有写出 {shortcuts}")
            return False
        with open(shortcuts, "r", encoding="utf-8", errors="replace") as handle:
            saved = handle.read()
        if "<UserDefinedCommands>" not in saved or 'name="我的命令"' not in saved:
            print(f"  FAIL: shortcuts.xml 里没有这条运行命令\n{saved[-800:]}")
            return False
        if "cmd /c echo saved-$(CURRENT_LINE)" not in saved:
            print(f"  FAIL: 命令行应当按原文（含变量）存进文件\n{saved[-800:]}")
            return False
        if 'name="run.我的命令"' in saved:
            print(f"  FAIL: 用户定义的命令不该出现在 <InternalCommands> 里\n{saved[-800:]}")
            return False

        # 5) 立刻从「运行」菜单执行这条命令
        user32.PostMessageW(window, WM_COMMAND, CMD_RUN_FIRST_SAVED, 0)
        if wait_output(window, "saved-1") is None:
            print("  FAIL: 从菜单执行刚保存的运行命令没有效果")
            return False

        # 6) 起不来的命令要弹说明，而且不该多出一个空的输出文档
        before = len(visible_scintillas_sorted(window))
        dialog = open_dialog(window, "definitely-not-here-xyz-1234 --help", True)
        if not dialog:
            print("  FAIL: 第五次 F5 没有弹出对话框")
            return False
        user32.PostMessageW(dialog, WM_COMMAND, RUN_GO, 0)
        if not wait_dialog_gone():
            print("  FAIL: 对话框没关掉")
            return False
        if not dismiss_dialog("运行命令", IDOK, timeout=8.0):
            print("  FAIL: 启动失败应当弹出说明")
            return False
        time.sleep(0.6)
        if len(visible_scintillas_sorted(window)) != before:
            print("  FAIL: 启动失败不该多出一个输出文档")
            return False
    finally:
        if window:
            user32.PostMessageW(window, WM_CLOSE, 0, 0)
            dismiss_dialog("notepadcc", IDNO, timeout=2.0, quiet=True)
            time.sleep(1.5)
        if proc.poll() is None:
            proc.kill()

    # 7) 重启：那条命令应当从 shortcuts.xml 读回来，菜单里照样能点
    session = os.path.join(cfg, "session.xml")
    if os.path.exists(session):
        os.remove(session)
    proc = subprocess.Popen([exe, MULTI_INSTANCE, "-portable", sample], cwd=root)
    window = wait_for_main_window("notepadcc - sample.txt")
    try:
        if not window:
            print("  FAIL: 重启后找不到主窗口")
            return False
        user32.PostMessageW(window, WM_COMMAND, CMD_RUN_FIRST_SAVED, 0)
        if wait_output(window, "saved-1") is None:
            print("  FAIL: 重启之后从菜单执行那条运行命令没有效果"
                  "（命令没有从 shortcuts.xml 读回来？）")
            return False
    finally:
        if window:
            user32.PostMessageW(window, WM_CLOSE, 0, 0)
            dismiss_dialog("notepadcc", IDNO, timeout=2.0, quiet=True)
            time.sleep(1.5)
        if proc.poll() is None:
            proc.kill()
        for name in ("config.xml", "session.xml"):
            target = os.path.join(cfg, name)
            if os.path.exists(target):
                os.remove(target)

    print("  运行命令：F5 弹窗、变量展开与工作目录正确、捕获输出进新文档、"
          "保存成命名命令能立刻用也能重启后读回来、起不来的命令被挡下且不留空文档")
    return True


if __name__ == "__main__":
    sys.exit(run())
