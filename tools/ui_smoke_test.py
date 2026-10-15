# -*- coding: utf-8 -*-
"""界面回归脚本：驱动 notepadcc 的查找栏与"在文件中查找"面板，做真实的端到端验证。

它不看窗口标题之类的表象，而是真的发命令、填输入框、点按钮，
再回来核对编辑器选区与结果列表，因此能抓住"编译通过但功能没接上"的问题。
"""
import ctypes
import os
import subprocess
import sys
import time
from ctypes import wintypes

user32 = ctypes.WinDLL("user32", use_last_error=True)
user32.FindWindowW.restype = wintypes.HWND
user32.GetDlgItem.restype = wintypes.HWND
user32.SendMessageW.restype = ctypes.c_ssize_t

WM_COMMAND = 0x0111
WM_SETTEXT = 0x000C
WM_GETTEXT = 0x000D
WM_GETTEXTLENGTH = 0x000E
WM_CLOSE = 0x0010

SCI_GETSELECTIONSTART = 2143
SCI_GETSELECTIONEND = 2145
LVM_GETITEMCOUNT = 0x1004

CMD_FIND = (3 << 12) | 1            # CmdCategory::Search, index 1
CMD_FIND_IN_FILES = (3 << 12) | 5   # index 5
BTN_FIND_NEXT = 908                 # FindBarControl::kFindNextButton
CTRL_FIND_EDIT = 901

PANEL_FIND = 701                    # SearchPanelControl
PANEL_DIR = 702
PANEL_FILTER = 703
PANEL_START = 707
PANEL_LIST = 710


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
    length = user32.SendMessageW(hwnd, WM_GETTEXTLENGTH, 0, 0)
    if length <= 0:
        return ""
    buffer = ctypes.create_unicode_buffer(length + 1)
    user32.SendMessageW(hwnd, WM_GETTEXT, length + 1, ctypes.cast(buffer, ctypes.c_void_p))
    return buffer.value


def find_child(parent, cls):
    for child in children(parent):
        if class_name(child) == cls:
            return child
    return None


def set_text(hwnd, control_id, text):
    control = user32.GetDlgItem(hwnd, control_id)
    if not control:
        return False
    user32.SendMessageW(control, WM_SETTEXT, 0, text)
    return True


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

    scinti = None
    for child in children(main_window):
        if class_name(child) == "Scintilla" and user32.IsWindowVisible(child):
            scinti = child
            break
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

    proc = subprocess.Popen([exe, "-portable", os.path.join(root, "README.md")], cwd=root)
    time.sleep(5)
    main_window = user32.FindWindowW("notepadccMainWindow", None)
    failed = False
    try:
        if not main_window:
            print("FAIL: 找不到主窗口")
            return 1
        if not test_find_bar(main_window, root):
            failed = True
        if not test_search_panel(main_window, root):
            failed = True
    finally:
        if main_window:
            user32.PostMessageW(main_window, WM_CLOSE, 0, 0)
            time.sleep(2)
        if proc.poll() is None:
            proc.kill()

    if failed:
        print("FAIL: 界面回归未通过")
        return 1
    print("PASS: 界面回归通过（查找栏 + 在文件中查找）")
    return 0


if __name__ == "__main__":
    sys.exit(run())
