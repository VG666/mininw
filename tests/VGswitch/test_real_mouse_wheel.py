"""VG Switch 真实鼠标滚轮回归测试。

测试只用 Win32 SendInput 注入鼠标移动、点击和滚轮，不调用 DOM API，
也不发送 WM_MOUSEWHEEL。脚本会临时替换 providers.json，退出时恢复原文件。
"""
from __future__ import annotations

import argparse
import ctypes
from ctypes import wintypes
import json
import os
from pathlib import Path
import subprocess
import sys
import time

import win32con
import win32gui
import win32process
import win32ui
from PIL import Image, ImageChops

HERE = Path(__file__).resolve().parent
EXE = HERE / "nw.exe"
DATA_FILE = Path(os.environ["LOCALAPPDATA"]) / "nw-bridge" / "vg-switch" / "providers.json"
WHEEL_DELTA = 120
INPUT_MOUSE = 0
MOUSEEVENTF_MOVE = 0x0001
MOUSEEVENTF_LEFTDOWN = 0x0002
MOUSEEVENTF_LEFTUP = 0x0004
MOUSEEVENTF_WHEEL = 0x0800
MOUSEEVENTF_ABSOLUTE = 0x8000
MOUSEEVENTF_VIRTUALDESK = 0x4000


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


def client_point(hwnd: int, x: int, y: int) -> tuple[int, int]:
    return win32gui.ClientToScreen(hwnd, (x, y))


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
            left, top, right, bottom = win32gui.GetWindowRect(root)
            move_cursor((left + right) // 2, (top + bottom) // 2)
            send_mouse(MOUSEEVENTF_LEFTDOWN)
            send_mouse(MOUSEEVENTF_LEFTUP)
        finally:
            if attached:
                user32.AttachThreadInput(current_thread, foreground_thread, False)
        time.sleep(0.2)
        active = win32gui.GetForegroundWindow()
        if active and win32gui.GetAncestor(active, win32con.GA_ROOT) == root:
            win32gui.SetWindowPos(root, win32con.HWND_NOTOPMOST, 0, 0, 0, 0,
                                  win32con.SWP_NOMOVE | win32con.SWP_NOSIZE)
            return
    raise RuntimeError(f"无法激活 VG Switch 窗口：target={hwnd:#x}, foreground={active:#x}")


def real_click(hwnd: int, x: int, y: int) -> None:
    sx, sy = client_point(hwnd, x, y)
    move_cursor(sx, sy)
    time.sleep(0.08)
    send_mouse(MOUSEEVENTF_LEFTDOWN)
    time.sleep(0.06)
    send_mouse(MOUSEEVENTF_LEFTUP)
    time.sleep(0.35)


def real_wheel(hwnd: int, x: int, y: int, notches: int) -> None:
    sx, sy = client_point(hwnd, x, y)
    move_cursor(sx, sy)
    time.sleep(0.1)
    step = WHEEL_DELTA if notches > 0 else -WHEEL_DELTA
    for _ in range(abs(notches)):
        send_mouse(MOUSEEVENTF_WHEEL, step)
        time.sleep(0.08)
    time.sleep(0.45)


def find_window(pid: int, timeout: float = 12.0) -> int:
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


def grab(hwnd: int, path: Path) -> Image.Image:
    left, top, right, bottom = win32gui.GetWindowRect(hwnd)
    width, height = right - left, bottom - top
    window_dc = win32gui.GetWindowDC(hwnd)
    if not window_dc:
        raise RuntimeError("取不到窗口 DC")
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
        image = Image.frombytes("RGB", (width, height), bits, "raw", "BGRX", 0, 1)
        image.save(path)
        return image
    finally:
        if bitmap is not None:
            win32gui.DeleteObject(bitmap.GetHandle())
        if mem is not None:
            mem.DeleteDC()
        if src is not None:
            src.DeleteDC()
        win32gui.ReleaseDC(hwnd, window_dc)


def difference_score(before: Image.Image, after: Image.Image, box: tuple[int, int, int, int]) -> int:
    diff = ImageChops.difference(before.crop(box), after.crop(box)).convert("L")
    return sum(1 for value in diff.getdata() if value >= 12)


def fixture() -> dict:
    models = [f"wheel-test-model-{index:02d}" for index in range(1, 31)]
    return {
        "proxy": {"enabled": False, "port": 18080, "key": ""},
        "headroom": {"enabled": False, "url": ""},
        "minimizeToTray": False,
        "activeGroupId": None,
        "groups": [{
            "id": "wheel-test-group",
            "name": "wheel-test-group",
            "enabled": True,
            "autoSelect": False,
            "activeItemId": "wheel-test-item",
            "items": [{
                "id": "wheel-test-item",
                "name": models[0],
                "baseUrl": "https://example.invalid/v1",
                "apiKey": "",
                "modelsPath": "/v1/models",
                "models": models,
                "modelsAt": 1,
                "enabled": True,
                "tlsInsecure": False,
                "color": "#2f9e5f",
                "headroom": None,
            }],
        }],
    }


def run_case(mode: str, out_dir: Path) -> list[str]:
    args = [str(EXE)]
    if mode == "no-bridge":
        args.append("--nw-no-bridge")
    proc = subprocess.Popen(args, cwd=HERE)
    failures: list[str] = []
    try:
        hwnd = find_window(proc.pid)
        activate_window(hwnd)
        time.sleep(1.25)
        width, height = win32gui.GetClientRect(hwnd)[2:]
        if width < 900 or height < 600:
            failures.append(f"窗口客户区异常：{width}x{height}")
            return failures

        initial = grab(hwnd, out_dir / f"{mode}-01-initial.png")
        real_click(hwnd, width // 2, 125)
        group = grab(hwnd, out_dir / f"{mode}-02-group.png")
        if difference_score(initial, group, (20, 75, width - 20, height - 20)) < 5000:
            failures.append("真实点击组卡片后界面没有明显变化")
            return failures

        real_click(hwnd, width - 145, 230)
        opened = grab(hwnd, out_dir / f"{mode}-03-edit-open.png")
        center_box = (width // 2 - 350, 35, width // 2 + 350, height - 20)
        if difference_score(group, opened, center_box) < 10000:
            failures.append("真实点击编辑按钮后弹窗未出现")
            return failures

        # 模型列表大致位于弹窗上半部；滚轮必须只推动内层列表。
        model_x, model_y = width // 2, 365
        screen_point = client_point(hwnd, model_x, model_y)
        foreground = win32gui.GetForegroundWindow()
        focus = user32.GetFocus()
        under_cursor = win32gui.WindowFromPoint(screen_point)
        print(f"[{mode}] hwnd={hwnd:#x} foreground={foreground:#x} focus={focus:#x} "
              f"under={under_cursor:#x} class={win32gui.GetClassName(under_cursor)!r}")
        real_wheel(hwnd, model_x, model_y, -3)
        print(f"[{mode}] inner-down title={win32gui.GetWindowText(hwnd)!r}")
        inner_down = grab(hwnd, out_dir / f"{mode}-04-inner-down.png")
        inner_score = difference_score(opened, inner_down, (width // 2 - 310, 295, width // 2 + 310, 465))
        header_score = difference_score(opened, inner_down, (width // 2 - 310, 70, width // 2 + 310, 245))
        if inner_score < 2500:
            failures.append(f"内层模型列表未随真实滚轮滚动（差异像素 {inner_score}）")
        if header_score > 1500:
            failures.append(f"滚动模型列表时外层弹窗也发生位移（顶部差异像素 {header_score}）")

        real_wheel(hwnd, model_x, model_y, 3)
        inner_up = grab(hwnd, out_dir / f"{mode}-05-inner-up.png")
        return_score = difference_score(opened, inner_up, (width // 2 - 310, 295, width // 2 + 310, 465))
        if return_score > 1800:
            failures.append(f"内层列表反向滚轮后未基本回到原位（差异像素 {return_score}）")

        # 外层可滚区域选择模型列表下方的 API Key 区域。
        outer_x, outer_y = width // 2, 545
        real_wheel(hwnd, outer_x, outer_y, -4)
        outer_down = grab(hwnd, out_dir / f"{mode}-06-outer-down.png")
        outer_score = difference_score(opened, outer_down, center_box)
        if outer_score < 8000:
            failures.append(f"外层弹窗未随真实滚轮滚动（差异像素 {outer_score}）")

        real_wheel(hwnd, outer_x, outer_y, 6)
        outer_up = grab(hwnd, out_dir / f"{mode}-07-outer-up.png")
        outer_return = difference_score(opened, outer_up, center_box)
        if outer_return > 3000:
            failures.append(f"外层弹窗反向滚轮后未基本回到顶部（差异像素 {outer_return}）")
        return failures
    finally:
        if proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(3)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait(3)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--mode", choices=("bridge", "no-bridge", "both"), default="both")
    parser.add_argument("--out", type=Path, default=HERE / "_wheel_test_output")
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    for stale in args.out.glob("*.png"):
        stale.unlink()

    DATA_FILE.parent.mkdir(parents=True, exist_ok=True)
    existed = DATA_FILE.exists()
    backup = DATA_FILE.read_bytes() if existed else None
    DATA_FILE.write_text(json.dumps(fixture(), ensure_ascii=False, indent=2), encoding="utf-8")
    modes = ("bridge", "no-bridge") if args.mode == "both" else (args.mode,)
    all_failures: list[str] = []
    try:
        for mode in modes:
            print(f"[{mode}] 开始")
            failures = run_case(mode, args.out)
            if failures:
                for failure in failures:
                    print(f"[{mode}] FAIL: {failure}")
                    all_failures.append(f"{mode}: {failure}")
            else:
                print(f"[{mode}] PASS")
    finally:
        if backup is not None:
            DATA_FILE.write_bytes(backup)
        elif DATA_FILE.exists():
            DATA_FILE.unlink()

    if all_failures:
        print(f"共 {len(all_failures)} 项失败；截图在 {args.out}")
        return 1
    print(f"全部通过；截图在 {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
