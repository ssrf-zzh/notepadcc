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
# 必须显式声明参数类型：不声明时 ctypes 会把指针参数按 32 位传，
# 在 x64 上地址会被截断，Scintilla 读到的是野地址。
user32.SendMessageW.argtypes = [wintypes.HWND, ctypes.c_uint, ctypes.c_size_t,
                                ctypes.c_ssize_t]


def address_of(buffer):
    return ctypes.cast(buffer, ctypes.c_void_p).value

WM_COMMAND = 0x0111
WM_SETTEXT = 0x000C
WM_GETTEXT = 0x000D
WM_GETTEXTLENGTH = 0x000E
WM_CLOSE = 0x0010

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

COLUMN_TEMPLATE_EDIT = 751
COLUMN_INSERT_BUTTON = 758

IDYES = 6
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
    length = user32.SendMessageW(hwnd, WM_GETTEXTLENGTH, 0, 0)
    if length <= 0:
        return ""
    buffer = ctypes.create_unicode_buffer(length + 1)
    user32.SendMessageW(hwnd, WM_GETTEXT, length + 1, address_of(buffer))
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
    buffer = ctypes.create_unicode_buffer(text)
    user32.SendMessageW(control, WM_SETTEXT, 0, address_of(buffer))
    return True


def visible_scintilla(main_window):
    for child in children(main_window):
        if class_name(child) == "Scintilla" and user32.IsWindowVisible(child):
            return child
    return None


def current_line(scinti):
    position = user32.SendMessageW(scinti, SCI_GETCURRENTPOS, 0, 0)
    return user32.SendMessageW(scinti, SCI_LINEFROMPOSITION, position, 0)


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
    """诊断用：列出当前可见的顶层窗口。"""
    result = []
    callback_type = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)

    def callback(hwnd, _):
        if user32.IsWindowVisible(hwnd):
            result.append((class_name(hwnd), window_text(hwnd)))
        return True

    user32.EnumWindows(callback_type(callback), 0)
    return result


def dismiss_dialog(title, command_id, timeout=6.0, quiet=False):
    """确认框是模态的，脚本必须替用户点掉，否则后面的断言都无从谈起。"""
    deadline = time.time() + timeout
    while time.time() < deadline:
        dialog = user32.FindWindowW("#32770", title)
        if dialog:
            # 直接给按钮发 BM_CLICK。早先试过给对话框发 WM_COMMAND，
            # 对替换确认框有效，对这个却点不掉——按按钮才是可靠的路子。
            button = user32.GetDlgItem(dialog, command_id)
            if button:
                user32.SendMessageW(button, 0x00F5, 0, 0)   # BM_CLICK
            else:
                user32.PostMessageW(dialog, WM_COMMAND, command_id, 0)
            time.sleep(0.4)
            return True
        time.sleep(0.2)
    if not quiet:
        print(f"    [诊断] 未找到标题为 {title!r} 的对话框；当前可见顶层窗口：")
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
    length = user32.SendMessageW(scinti, SCI_GETLENGTH, 0, 0)
    data = bytearray()
    for index in range(length):
        data.append(user32.SendMessageW(scinti, SCI_GETCHARAT, index, 0) & 0xFF)
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
    proc = subprocess.Popen([exe, "-portable", path], cwd=root)
    time.sleep(4)
    window = user32.FindWindowW("notepadccMainWindow", "notepadcc - cols.txt")
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

        # 第 0 行第 1 列 → 第 2 行第 3 列，即覆盖三行的同一段两列
        user32.SendMessageW(scinti, SCI_SETSELECTIONMODE, SC_SEL_RECTANGLE, 0)
        user32.SendMessageW(scinti, SCI_SETRECTANGULARSELECTIONANCHOR, 1, 0)
        user32.SendMessageW(scinti, SCI_SETRECTANGULARSELECTIONCARET, 11, 0)
        time.sleep(0.3)

        if not user32.SendMessageW(scinti, SCI_SELECTIONISRECTANGLE, 0, 0):
            print("  FAIL: 选区没有进入矩形模式（ConfigureColumnEditing 可能没生效）")
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

    log_path = os.path.join(root, "build", "bin", "config", "logs", "notepadcc.log")
    if os.path.exists(log_path):
        os.remove(log_path)
    session = os.path.join(root, "build", "bin", "config", "session.xml")
    if os.path.exists(session):
        os.remove(session)

    exe = os.path.join(root, "build", "bin", "notepadcc.exe")
    started = time.time()
    proc = subprocess.Popen([exe, "-portable", path], cwd=root)
    time.sleep(12)
    window = user32.FindWindowW("notepadccMainWindow", "notepadcc - big.txt")
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

        print(f"  大文件分档：{size_mb:.1f} MB 文件以{elapsed:.1f} 秒进入大文件模式，"
              f"装载长度一致")
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

    log_path = os.path.join(root, "build", "bin", "config", "logs", "notepadcc.log")
    if os.path.exists(log_path):
        os.remove(log_path)
    session = os.path.join(root, "build", "bin", "config", "session.xml")
    if os.path.exists(session):
        os.remove(session)

    exe = os.path.join(root, "build", "bin", "notepadcc.exe")
    proc = subprocess.Popen([exe, "-portable", path], cwd=root)
    time.sleep(4)
    window = user32.FindWindowW("notepadccMainWindow", "notepadcc - doc.txt")
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

        # 在程序之外直接改文件
        with open(path, "w", encoding="utf-8", newline="") as handle:
            handle.write("beta\n")

        # 等程序自己发现（目录通知），弹确认框
        if not dismiss_dialog("文件已被外部修改", IDYES, timeout=15.0):
            print("  FAIL: 外部改动没有被检测到（没等到确认框）")
            return False
        time.sleep(1.0)

        after = scinti_text(scinti)
        if after != "beta\n":
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

    if failed:
        print("FAIL: 界面回归未通过")
        return 1
    print("PASS: 界面回归通过（查找栏 + 在文件中查找 + 书签 + 在文件中替换 + 列编辑 + "
          "大文件 + 外部改动）")
    return 0


if __name__ == "__main__":
    sys.exit(run())
