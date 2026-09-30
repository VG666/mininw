"""VG Switch 无边框窗口「边缘拖拽缩放」真实鼠标回归测试。

只注入真实鼠标事件（SendInput），不调用 DOM，也不直接发窗口消息。
每次拖拽后对比窗口矩形是否变化，并把命中信息打印出来（父窗口 / 子窗口 / 命中测试结果）。
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

import win32api
import win32con
import win32gui
import win32process

HERE = Path(__file__).resolve().parent
EXE = HERE / "nw.exe"
BRIDGE_DLL = HERE / "NativeMediaBridge.dll"
BRIDGE_DLL_BAK = HERE / "NativeMediaBridge.dll.borderbak"
INPUT_MOUSE = 0
MOUSEEVENTF_MOVE = 0x0001
MOUSEEVENTF_LEFTDOWN = 0x0002
MOUSEEVENTF_LEFTUP = 0x0004
MOUSEEVENTF_ABSOLUTE = 0x8000
MOUSEEVENTF_VIRTUALDESK = 0x4000

WM_NCHITTEST = 0x0084


class MOUSEINPUT(ctypes.Structure):
    _fields_ = [
        ("dx", wintypes.LONG),
        ("dy", wintypes.LONG),
        ("mouseData", wintypes.DWORD),
        ("dwFlags", wintypes.DWORD),
        ("time", wintypes.DWORD),
        ("dwExtraInfo", ctypes.POINTER(ctypes.c_ulong)),
    ]


class INPUT_UNION(ctypes.Union):
    _fields_ = [("mi", MOUSEINPUT)]


class INPUT(ctypes.Structure):
    _fields_ = [("type", wintypes.DWORD), ("u", INPUT_UNION)]


user32 = ctypes.windll.user32
user32.SendInput.argtypes = (wintypes.UINT, ctypes.POINTER(INPUT), ctypes.c_int)
user32.SendInput.restype = wintypes.UINT
user32.WindowFromPoint.argtypes = (wintypes.POINT,)
user32.WindowFromPoint.restype = wintypes.HWND


def send_mouse(flags: int, data: int = 0, dx: int = 0, dy: int = 0) -> None:
    extra = ctypes.c_ulong(0)
    event = INPUT(
        type=INPUT_MOUSE,
        u=INPUT_UNION(mi=MOUSEINPUT(dx, dy, data & 0xFFFFFFFF, flags, 0, ctypes.pointer(extra))),
    )
    if user32.SendInput(1, ctypes.byref(event), ctypes.sizeof(INPUT)) != 1:
        raise ctypes.WinError()


def move_cursor(x: int, y: int) -> None:
    left = user32.GetSystemMetrics(76)
    top = user32.GetSystemMetrics(77)
    width = user32.GetSystemMetrics(78)
    height = user32.GetSystemMetrics(79)
    ax = round((x - left) * 65535 / max(1, width - 1))
    ay = round((y - top) * 65535 / max(1, height - 1))
    send_mouse(MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK, dx=ax, dy=ay)


def drag(from_xy: tuple[int, int], to_xy: tuple[int, int], steps: int = 12) -> None:
    move_cursor(*from_xy)
    time.sleep(0.15)
    send_mouse(MOUSEEVENTF_LEFTDOWN)
    time.sleep(0.15)
    x0, y0 = from_xy
    x1, y1 = to_xy
    for index in range(1, steps + 1):
        move_cursor(round(x0 + (x1 - x0) * index / steps), round(y0 + (y1 - y0) * index / steps))
        time.sleep(0.03)
    time.sleep(0.15)
    send_mouse(MOUSEEVENTF_LEFTUP)
    time.sleep(0.45)


def activate_window(hwnd: int) -> None:
    root = win32gui.GetAncestor(hwnd, win32con.GA_ROOT) or hwnd
    win32gui.ShowWindow(root, win32con.SW_RESTORE)
    current_thread = ctypes.windll.kernel32.GetCurrentThreadId()
    for _ in range(5):
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
        finally:
            if attached:
                user32.AttachThreadInput(current_thread, foreground_thread, False)
        time.sleep(0.2)
        active = win32gui.GetForegroundWindow()
        if active and win32gui.GetAncestor(active, win32con.GA_ROOT) == root:
            win32gui.SetWindowPos(root, win32con.HWND_NOTOPMOST, 0, 0, 0, 0,
                                  win32con.SWP_NOMOVE | win32con.SWP_NOSIZE)
            return
    raise RuntimeError("无法激活 VG Switch 窗口")


def find_window(pid: int, timeout: float = 15.0) -> int:
    deadline = time.time() + timeout
    while time.time() < deadline:
        found: list[int] = []

        def visit(hwnd: int, _extra: object) -> None:
            if not win32gui.IsWindowVisible(hwnd):
                return
            _, window_pid = win32process.GetWindowThreadProcessId(hwnd)
            if window_pid == pid and win32gui.GetWindowText(hwnd) == "VG Switch":
                found.append(hwnd)

        win32gui.EnumWindows(visit, None)
        if found:
            return found[0]
        time.sleep(0.1)
    raise RuntimeError("等待 VG Switch 窗口超时")


def describe_tree(hwnd: int, indent: int = 0) -> list[str]:
    lines = []

    def visit(child: int, depth: int) -> None:
        _, pid = win32process.GetWindowThreadProcessId(child)
        rect = win32gui.GetWindowRect(child)
        style = win32gui.GetWindowLong(child, win32con.GWL_STYLE)
        ex_style = win32gui.GetWindowLong(child, win32con.GWL_EXSTYLE)
        lines.append(
            f"{'  ' * depth}hwnd={child:#x} pid={pid} class={win32gui.GetClassName(child)!r} "
            f"text={win32gui.GetWindowText(child)!r} visible={bool(win32gui.IsWindowVisible(child))} "
            f"rect={rect} style={style:#010x} ex={ex_style:#010x}"
        )
        win32gui.EnumChildWindows(child, lambda c, _e: visit(c, depth + 1), None)

    visit(hwnd, indent)
    return lines


def hit_test(hwnd: int, screen_xy: tuple[int, int]) -> int:
    x, y = screen_xy
    lparam = (y & 0xFFFF) << 16 | (x & 0xFFFF)
    return user32.SendMessageW(hwnd, WM_NCHITTEST, 0, lparam)


def run_case(mode: str, out_dir: Path, verbose: bool) -> list[str]:
    renamed_bridge = False
    if mode == "no-bridge":
        # --nw-no-bridge 走不了命令行：内核的 nodeblink 会把 argv[1] 当入口模块，
        # 传了参数窗口直接闪退。改用"临时移走桥 dll"强制纯内核路径，
        # 应用目录仍由 exe 旁的 package.json 自动定位（与正常启动一致）。
        if BRIDGE_DLL.exists():
            BRIDGE_DLL.rename(BRIDGE_DLL_BAK)
            renamed_bridge = True
    env = dict(os.environ)
    env["NMB_NW_NO_DIALOG"] = "1"
    proc = subprocess.Popen([str(EXE)], cwd=HERE, env=env)
    failures: list[str] = []
    try:
        hwnd = find_window(proc.pid)
        activate_window(hwnd)
        time.sleep(1.4)
        root = win32gui.GetAncestor(hwnd, win32con.GA_ROOT) or hwnd
        print(f"[{mode}] root={root:#x} ({win32gui.GetClassName(root)}) target={hwnd:#x}")
        for line in describe_tree(root):
            print("   ", line)

        def rect() -> tuple[int, int, int, int]:
            return win32gui.GetWindowRect(root)

        # --- 右边缘：向左拖 80px，宽度应减少 ---
        before = rect()
        right_mid_y = (before[1] + before[3]) // 2
        edge_x = before[2] - 2
        under = win32gui.WindowFromPoint((edge_x, right_mid_y))
        print(f"[{mode}] right-edge probe at ({edge_x},{right_mid_y}) -> under={under:#x} "
              f"class={win32gui.GetClassName(under)!r} parent={win32gui.GetParent(under) or 0:#x} "
              f"hit={hit_test(root, (edge_x, right_mid_y))}")
        drag((edge_x, right_mid_y), (edge_x - 80, right_mid_y))
        after = rect()
        width_before = before[2] - before[0]
        width_after = after[2] - after[0]
        print(f"[{mode}] right edge: width {width_before} -> {width_after}")
        if abs(width_after - width_before) < 40:
            failures.append(f"右边缘拖拽未改变宽度（{width_before} -> {width_after}）")

        # --- 下边缘：向上拖 60px，高度应减少 ---
        before = rect()
        bottom_mid_x = (before[0] + before[2]) // 2
        edge_y = before[3] - 2
        under = win32gui.WindowFromPoint((bottom_mid_x, edge_y))
        print(f"[{mode}] bottom-edge probe at ({bottom_mid_x},{edge_y}) -> under={under:#x} "
              f"class={win32gui.GetClassName(under)!r} hit={hit_test(root, (bottom_mid_x, edge_y))}")
        drag((bottom_mid_x, edge_y), (bottom_mid_x, edge_y - 60))
        after = rect()
        height_before = before[3] - before[1]
        height_after = after[3] - after[1]
        print(f"[{mode}] bottom edge: height {height_before} -> {height_after}")
        if abs(height_after - height_before) < 30:
            failures.append(f"下边缘拖拽未改变高度（{height_before} -> {height_after}）")

        # --- 左上角：斜向拖 ---
        before = rect()
        corner = (before[0] + 2, before[1] + 2)
        under = win32gui.WindowFromPoint(corner)
        print(f"[{mode}] topleft probe at {corner} -> under={under:#x} "
              f"class={win32gui.GetClassName(under)!r} hit={hit_test(root, corner)}")
        drag(corner, (corner[0] - 60, corner[1] - 40))
        after = rect()
        print(f"[{mode}] topleft corner: rect {before} -> {after}")
        if after[0] == before[0] and after[1] == before[1]:
            failures.append(f"左上角拖拽未改变位置（{before} -> {after}）")

        if verbose:
            out_dir.mkdir(parents=True, exist_ok=True)
            with open(out_dir / f"{mode}-border.txt", "w", encoding="utf-8") as handle:
                handle.write("\n".join(describe_tree(root)))
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=8)
        except subprocess.TimeoutExpired:
            proc.kill()
        if renamed_bridge:
            BRIDGE_DLL_BAK.rename(BRIDGE_DLL)
    return failures


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--mode", choices=["bridge", "no-bridge", "both"], default="both")
    parser.add_argument("--out", default=str(HERE / "_border_test_output"))
    parser.add_argument("--verbose", action="store_true")
    options = parser.parse_args()
    modes = ["bridge", "no-bridge"] if options.mode == "both" else [options.mode]
    out_dir = Path(options.out)
    all_failures: list[str] = []
    for mode in modes:
        try:
            failures = run_case(mode, out_dir, options.verbose)
        except Exception as error:  # noqa: BLE001
            failures = [f"{type(error).__name__}: {error}"]
        for failure in failures:
            print(f"FAIL [{mode}] {failure}")
        all_failures.extend(f"{mode}: {failure}" for failure in failures)
    if all_failures:
        print(f"\n共 {len(all_failures)} 项失败")
        return 1
    print("\n边框缩放全部通过")
    return 0


if __name__ == "__main__":
    sys.exit(main())
