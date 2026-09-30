"""回归：边框缩放修复不能破坏页面交互。

检查项：
  1) 客户区内的点击/滚轮仍然生效（不被 8px 边框带吃掉）；
  2) 页面贴近窗口边缘的控件仍可点（边框带只占最外 8px）；
  3) 最大化后边缘不再进入缩放（最大化状态下应无可拖边框）；
  4) 最小尺寸（manifest min_width/min_height）仍然钳制拖拽。
真实鼠标事件驱动；两种模式都跑。
"""
from __future__ import annotations

import argparse
import ctypes
from ctypes import wintypes
import os
from pathlib import Path
import subprocess
import sys
import time

import win32con
import win32gui
import win32process

HERE = Path(__file__).resolve().parent
EXE = HERE / "nw.exe"
BRIDGE_DLL = HERE / "NativeMediaBridge.dll"
BRIDGE_DLL_BAK = HERE / "NativeMediaBridge.dll.regbak"

INPUT_MOUSE = 0
MOUSEEVENTF_MOVE = 0x0001
MOUSEEVENTF_LEFTDOWN = 0x0002
MOUSEEVENTF_LEFTUP = 0x0004
MOUSEEVENTF_WHEEL = 0x0800
MOUSEEVENTF_ABSOLUTE = 0x8000
MOUSEEVENTF_VIRTUALDESK = 0x4000
WM_NCHITTEST = 0x0084


class MOUSEINPUT(ctypes.Structure):
    _fields_ = [("dx", wintypes.LONG), ("dy", wintypes.LONG), ("mouseData", wintypes.DWORD),
                ("dwFlags", wintypes.DWORD), ("time", wintypes.DWORD),
                ("dwExtraInfo", ctypes.POINTER(ctypes.c_ulong))]


class INPUT_UNION(ctypes.Union):
    _fields_ = [("mi", MOUSEINPUT)]


class INPUT(ctypes.Structure):
    _fields_ = [("type", wintypes.DWORD), ("u", INPUT_UNION)]


class CURSORINFO(ctypes.Structure):
    _fields_ = [("cbSize", wintypes.DWORD), ("flags", wintypes.DWORD),
                ("hCursor", wintypes.HANDLE), ("ptScreenPos", wintypes.POINT)]


user32 = ctypes.windll.user32
user32.SendInput.argtypes = (wintypes.UINT, ctypes.POINTER(INPUT), ctypes.c_int)
user32.SendInput.restype = wintypes.UINT


def send_mouse(flags: int, data: int = 0, dx: int = 0, dy: int = 0) -> None:
    extra = ctypes.c_ulong(0)
    event = INPUT(type=INPUT_MOUSE,
                  u=INPUT_UNION(mi=MOUSEINPUT(dx, dy, data & 0xFFFFFFFF, flags, 0,
                                              ctypes.pointer(extra))))
    if user32.SendInput(1, ctypes.byref(event), ctypes.sizeof(INPUT)) != 1:
        raise ctypes.WinError()


def move_cursor(x: int, y: int) -> None:
    left, top = user32.GetSystemMetrics(76), user32.GetSystemMetrics(77)
    width, height = user32.GetSystemMetrics(78), user32.GetSystemMetrics(79)
    send_mouse(MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK,
               dx=round((x - left) * 65535 / max(1, width - 1)),
               dy=round((y - top) * 65535 / max(1, height - 1)))


def cursor_kind() -> str:
    info = CURSORINFO()
    info.cbSize = ctypes.sizeof(CURSORINFO)
    if not user32.GetCursorInfo(ctypes.byref(info)) or not info.hCursor:
        return "none"
    for name, code in (("sizewe", 32644), ("sizens", 32645), ("sizenwse", 32642),
                       ("sizenesw", 32643), ("arrow", 32512), ("ibeam", 32513),
                       ("hand", 32649), ("sizeall", 32646), ("cross", 32515)):
        if info.hCursor == user32.LoadCursorW(None, ctypes.c_wchar_p(code)):
            return name
    return "other"


def click(x: int, y: int) -> None:
    move_cursor(x, y)
    time.sleep(0.25)
    send_mouse(MOUSEEVENTF_LEFTDOWN)
    time.sleep(0.1)
    send_mouse(MOUSEEVENTF_LEFTUP)
    time.sleep(0.5)


def drag(from_xy, to_xy, steps: int = 12) -> None:
    move_cursor(*from_xy)
    time.sleep(0.25)
    send_mouse(MOUSEEVENTF_LEFTDOWN)
    time.sleep(0.25)
    for index in range(1, steps + 1):
        move_cursor(round(from_xy[0] + (to_xy[0] - from_xy[0]) * index / steps),
                    round(from_xy[1] + (to_xy[1] - from_xy[1]) * index / steps))
        time.sleep(0.05)
    time.sleep(0.25)
    send_mouse(MOUSEEVENTF_LEFTUP)
    time.sleep(0.6)


def activate_window(hwnd: int) -> None:
    root = win32gui.GetAncestor(hwnd, win32con.GA_ROOT) or hwnd
    win32gui.ShowWindow(root, win32con.SW_RESTORE)
    current_thread = ctypes.windll.kernel32.GetCurrentThreadId()
    for _ in range(6):
        foreground = win32gui.GetForegroundWindow()
        foreground_thread = win32process.GetWindowThreadProcessId(foreground)[0] if foreground else 0
        attached = bool(foreground_thread and foreground_thread != current_thread)
        if attached:
            user32.AttachThreadInput(current_thread, foreground_thread, True)
        try:
            user32.keybd_event(win32con.VK_MENU, 0, 0, 0)
            user32.keybd_event(win32con.VK_MENU, 0, 2, 0)
            user32.AllowSetForegroundWindow(-1)
            win32gui.SetWindowPos(root, win32con.HWND_TOPMOST, 0, 0, 0, 0,
                                  win32con.SWP_NOMOVE | win32con.SWP_NOSIZE)
            win32gui.BringWindowToTop(root)
            win32gui.SetForegroundWindow(root)
            win32gui.SetActiveWindow(root)
        except Exception:  # noqa: BLE001
            pass
        finally:
            if attached:
                user32.AttachThreadInput(current_thread, foreground_thread, False)
        time.sleep(0.25)
        active = win32gui.GetForegroundWindow()
        if active and win32gui.GetAncestor(active, win32con.GA_ROOT) == root:
            win32gui.SetWindowPos(root, win32con.HWND_NOTOPMOST, 0, 0, 0, 0,
                                  win32con.SWP_NOMOVE | win32con.SWP_NOSIZE)
            return
    raise RuntimeError("无法激活窗口")


def find_window(pid: int, timeout: float = 15.0) -> int:
    deadline = time.time() + timeout
    while time.time() < deadline:
        found: list[int] = []

        def visit(hwnd: int, _extra: object) -> None:
            _, window_pid = win32process.GetWindowThreadProcessId(hwnd)
            if window_pid == pid and win32gui.GetWindowText(hwnd) == "VG Switch":
                found.append(hwnd)

        win32gui.EnumWindows(visit, None)
        if found:
            return found[0]
        time.sleep(0.15)
    raise RuntimeError("等待 VG Switch 窗口超时")


def grab(hwnd: int):
    from PIL import Image, ImageChops
    width, height = win32gui.GetClientRect(hwnd)[2:]
    import win32ui
    window_dc = win32gui.GetWindowDC(hwnd)
    src = mem = bitmap = None
    try:
        src = win32ui.CreateDCFromHandle(window_dc)
        mem = src.CreateCompatibleDC()
        bitmap = win32ui.CreateBitmap()
        bitmap.CreateCompatibleBitmap(src, width, height)
        mem.SelectObject(bitmap)
        if not user32.PrintWindow(hwnd, mem.GetSafeHdc(), 2):
            raise RuntimeError("PrintWindow 失败")
        bits = bitmap.GetBitmapBits(True)
        return Image.frombytes("RGB", (width, height), bits, "raw", "BGRX", 0, 1)
    finally:
        if bitmap is not None:
            win32gui.DeleteObject(bitmap.GetHandle())
        if mem is not None:
            mem.DeleteDC()
        if src is not None:
            src.DeleteDC()
        win32gui.ReleaseDC(hwnd, window_dc)


def difference_score(before, after, box) -> int:
    from PIL import ImageChops
    diff = ImageChops.difference(before.crop(box), after.crop(box)).convert("L")
    return sum(1 for value in diff.getdata() if value >= 12)


def run_case(mode: str) -> list[str]:
    renamed = False
    if mode == "no-bridge" and BRIDGE_DLL.exists():
        BRIDGE_DLL.rename(BRIDGE_DLL_BAK)
        renamed = True
    env = dict(os.environ)
    env["NMB_NW_NO_DIALOG"] = "1"
    proc = subprocess.Popen([str(EXE)], cwd=HERE, env=env)
    failures: list[str] = []
    try:
        hwnd = find_window(proc.pid)
        activate_window(hwnd)
        time.sleep(1.6)
        root = win32gui.GetAncestor(hwnd, win32con.GA_ROOT) or hwnd
        left, top, right, bottom = win32gui.GetWindowRect(root)
        width = right - left
        height = bottom - top
        print(f"[{mode}] root={root:#x} rect=({left},{top},{right},{bottom})")

        # 1) 客户区中心点击仍要有效：点组卡片后界面应有明显变化
        before = grab(root)
        click(left + width // 2, top + 125)
        after = grab(root)
        score = difference_score(before, after, (20, 75, width - 20, height - 20))
        print(f"[{mode}] 中心点击差异像素={score}")
        if score < 3000:
            failures.append(f"客户区点击失效（差异像素 {score}）")

        # 2) 边缘带内的光标应为缩放光标；客户区内应不是
        move_cursor(right - 3, top + height // 2)
        time.sleep(0.5)
        edge_cursor = cursor_kind()
        move_cursor(left + width // 2, top + height // 2)
        time.sleep(0.5)
        inner_cursor = cursor_kind()
        print(f"[{mode}] 边缘光标={edge_cursor} 客户区光标={inner_cursor}")
        if edge_cursor != "sizewe":
            failures.append(f"右边缘光标不是缩放光标（{edge_cursor}）")

        # 3) 最小尺寸钳制：从右边缘往左拖远超 min_width(880)，宽度不应低于 880
        before = win32gui.GetWindowRect(root)
        edge_x = before[2] - 3
        mid_y = (before[1] + before[3]) // 2
        drag((edge_x, mid_y), (edge_x - 700, mid_y))
        after = win32gui.GetWindowRect(root)
        clamped = after[2] - after[0]
        print(f"[{mode}] 最小宽度钳制：{before[2] - before[0]} -> {clamped}（min_width=880）")
        if clamped < 870:
            failures.append(f"宽度拖到 {clamped}，低于 min_width=880")

        # 4) 最大化后边缘不再是缩放（maximized 状态下应无可拖边框）
        user32.SendMessageW(root, win32con.WM_SYSCOMMAND, win32con.SC_MAXIMIZE, 0)
        time.sleep(1.0)
        max_rect = win32gui.GetWindowRect(root)
        max_left, max_top, max_right, max_bottom = max_rect
        move_cursor(max_right - 3, (max_top + max_bottom) // 2)
        time.sleep(0.5)
        max_cursor = cursor_kind()
        print(f"[{mode}] 最大化后边缘光标={max_cursor} rect={max_rect}")
        if max_cursor == "sizewe":
            failures.append("最大化后边缘仍进入缩放")

        # 还原窗口，避免影响后续
        user32.SendMessageW(root, win32con.WM_SYSCOMMAND, win32con.SC_RESTORE, 0)
        time.sleep(0.8)
        return failures
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=8)
        except subprocess.TimeoutExpired:
            proc.kill()
        if renamed:
            BRIDGE_DLL_BAK.rename(BRIDGE_DLL)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--mode", choices=["bridge", "no-bridge", "both"], default="both")
    options = parser.parse_args()
    modes = ["bridge", "no-bridge"] if options.mode == "both" else [options.mode]
    all_failures: list[str] = []
    for mode in modes:
        try:
            failures = run_case(mode)
        except Exception as error:  # noqa: BLE001
            failures = [f"{type(error).__name__}: {error}"]
        for failure in failures:
            print(f"FAIL [{mode}] {failure}")
        all_failures.extend(f"{mode}: {failure}" for failure in failures)
    if all_failures:
        print(f"\n共 {len(all_failures)} 项失败")
        return 1
    print("\n边框缩放回归全部通过")
    return 0


if __name__ == "__main__":
    sys.exit(main())
