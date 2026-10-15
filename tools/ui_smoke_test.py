# -*- coding: utf-8 -*-
"""临时脚本：驱动 notepadcc 的查找栏做一次端到端验证，跑完即删。"""
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
SCI_GETSELECTIONSTART = 2143
SCI_GETSELECTIONEND = 2145

CMD_FIND = (3 << 12) | 1          # CmdCategory::Search, index 1
BTN_FIND_NEXT = 908               # FindBarControl::kFindNextButton
CTRL_FIND_EDIT = 901


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


def run():
    root = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
    exe = os.path.join(root, "build", "bin", "notepadcc.exe")
    target = os.path.join(root, "README.md")
    with open(target, "r", encoding="utf-8") as handle:
        content = handle.read()
    raw = content.encode("utf-8")
    needle = "notepadcc"
    needle_bytes = needle.encode("utf-8")

    proc = subprocess.Popen([exe, "-portable", target], cwd=root)
    time.sleep(5)
    main_window = user32.FindWindowW("notepadccMainWindow", None)
    try:
        if not main_window:
            print("FAIL: 找不到主窗口")
            return 1

        user32.SendMessageW(main_window, WM_COMMAND, CMD_FIND, 0)
        time.sleep(1)

        find_bar = None
        for child in children(main_window):
            if class_name(child) == "notepadccFindBar":
                find_bar = child
                break
        if find_bar is None:
            print("FAIL: 查找栏没有创建出来")
            return 1
        print("查找栏已创建，可见 =", bool(user32.IsWindowVisible(find_bar)))

        edit = user32.GetDlgItem(find_bar, CTRL_FIND_EDIT)
        if not edit:
            print("FAIL: 找不到查找输入框")
            return 1
        user32.SendMessageW(edit, WM_SETTEXT, 0, needle)
        time.sleep(0.3)

        user32.SendMessageW(main_window, WM_COMMAND, BTN_FIND_NEXT, 0)
        time.sleep(1.2)

        scinti = None
        for child in children(main_window):
            if class_name(child) == "Scintilla" and user32.IsWindowVisible(child):
                scinti = child
                break
        if scinti is None:
            print("FAIL: 找不到可见的编辑控件")
            return 1

        start = user32.SendMessageW(scinti, SCI_GETSELECTIONSTART, 0, 0)
        end = user32.SendMessageW(scinti, SCI_GETSELECTIONEND, 0, 0)
        selected = raw[start:end].decode("utf-8", errors="replace")
        print(f"选中区间 = [{start}, {end})  选中文本 = {selected!r}")
        if selected != needle:
            print("FAIL: 选中内容不是预期匹配")
            return 1

        first = raw.find(needle_bytes)
        if start != first:
            print(f"FAIL: 应命中第一个匹配（字节偏移 {first}），实际 {start}")
            return 1

        status = None
        for child in children(find_bar):
            if class_name(child) == "Static":
                status = window_text(child)
                break
        print("状态文字 =", repr(status))
        if not status or "匹配" not in status:
            print("FAIL: 状态栏没有给出匹配数量提示")
            return 1

        print("PASS: 查找栏端到端验证通过")
        return 0
    finally:
        if main_window:
            user32.PostMessageW(main_window, 0x0010, 0, 0)   # WM_CLOSE
            time.sleep(1.5)
        if proc.poll() is None:
            proc.kill()


if __name__ == "__main__":
    sys.exit(run())
