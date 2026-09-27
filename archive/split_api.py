# -*- coding: utf-8 -*-
r"""一次性重构脚本：把单文件 nw_api.js 拆成 nw\api\ 下按 SDK 模块边界分开的模块。

为什么要拆：官方 SDK 的页面侧是 api_nw_*.js 一组模块 + api_window_internal.js 绑定层，
本实现原来是单文件 700 行。拆完之后一个文件对应 SDK 里的一个模块，改哪块找哪块。

拆分的语义前提（重要）：宿主把 api\*.js 按文件名排序后**拼成一个脚本**，并在外面统一包
一层 IIFE（含 window.__nmbInstalled 幂等判断），所以各模块共享同一个函数作用域——
不要给单个模块再套自己的 IIFE，也不要再写 'use strict'。文件名数字前缀 = 加载顺序。

本脚本只做"按行范围搬家 + 加文件头"，不改变任何语句；语义改动（title/zoomLevel 访问器、
internal 绑定层等）在搬完之后单独改。跑完即可删除：它只是一次性的搬迁工具。
"""
import io
import os

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))   # ...\nw
SRC = os.path.join(ROOT, 'nw_api.js')
OUTDIR = os.path.join(ROOT, 'api')

text = io.open(SRC, encoding='utf-8').read()
NL = '\r\n' if '\r\n' in text else '\n'
lines = [line[:-1] if line.endswith('\r') else line for line in text.split('\n')]


def sl(a, b):
    """取原文 [a, b] 行（1 起、含两端）。"""
    return lines[a - 1:b]


HDR_BASE = [
    '// 00_base.js —— 页面侧 nw.* 层的地基：两条通道（同步取数 / 异步动手）、事件发射器、公共小工具。',
    '//',
    '// 与 SDK 的对应：src/resources/base.js 那类基础设施，加上内核 bindingUtil 的角色。',
    '// 各模块共用同一个函数作用域——宿主把 api\\*.js 按文件名排序**拼成一个脚本**，外面统一包',
    '// 一层 IIFE（含 window.__nmbInstalled 幂等判断）。所以：不要给单个模块再套自己的 IIFE，',
    '// 也不要再写 \'use strict\'（已经在外层了）。文件名的数字前缀就是加载顺序。',
]

HDR_APP = [
    '// 10_api_nw_app.js —— nw.App。对应 SDK：src/resources/api_nw_app.js（browser 侧 app.js）。',
    '// argv / fullArgv / filteredArgv 三个的语义照抄 SDK：fullArgv 是 native 给的原样，',
    '// argv 是过掉 filteredArgv 那组正则之后的，filteredArgv 本身可读可写。',
]

HDR_WINDOW = [
    '// 20_api_nw_window.js —— nw.Window / WindowState，含 nw.Window.open。',
    '// 对应 SDK：src/resources/api_nw_window.js + api_nw_newwin.js。',
    '// title / zoomLevel 在 SDK 里是**访问器**（读走 currentWindowInternal，写走 setXxxInternal），',
    '// 不是普通字段——所以 sync() 只许写 __title 这种内部字段，别直接赋 this.title（那会变成设覆盖值）。',
]

HDR_MENUITEM = [
    '// 40_api_nw_menuitem.js —— nw.MenuItem 与"菜单树 → 可 JSON 化的纯对象"序列化。',
    '// 对应 SDK：src/resources/api_nw_menuitem.js。',
    '// 菜单回调的 id 由这里分配：native 建 HMENU 时原样用回去，点击才能找回函数（见 41 的注册表）。',
]

HDR_MENU = [
    '// 41_api_nw_menu.js —— nw.Menu（menubar / popup）与菜单点击回调的入口。',
    '// 对应 SDK：src/resources/api_nw_menu.js。',
]

HDR_CLIPBOARD = [
    '// 50_api_nw_clipboard.js —— nw.Clipboard。对应 SDK：src/resources/api_nw_clipboard.js。',
    '// 静态 get() 取单例，实例上的 get() 才是"读文本"——两个 get 不是一回事，别合并。',
]

HDR_SHELL = [
    '// 51_api_nw_shell.js —— nw.Shell。对应 SDK：src/resources/api_nw_shell.js。',
]

HDR_SCREEN = [
    '// 52_api_nw_screen.js —— nw.Screen。对应 SDK：src/resources/api_nw_screen.js。',
]

HDR_SHORTCUT = [
    '// 53_api_nw_shortcut.js —— nw.Shortcut。对应 SDK：src/resources/api_nw_shortcut.js。',
    '// registerGlobalHotKey / unregisterGlobalHotKey 是**静态**方法（SDK 里 nw.App 只是转发到这里）。',
]

HDR_TRAY = [
    '// 54_api_nw_tray.js —— nw.Tray。对应 SDK：src/resources/api_nw_tray.js。',
]

HDR_INSTALL = [
    '// 90_install.js —— 装配层：把上述模块缝成 window.nw，接内核/桥推上来的事件入口，接管 window.open。',
    '// 对应 SDK 里 nw.require(\'nw\') 装出来的那一层（不含模块实现本身）。',
    '// 它必须最后加载：前面所有模块都在这里被引用。',
]

SPEC = [
    ('00_base.js', HDR_BASE, [(1, 29), (35, 188), (586, 591)]),
    ('10_api_nw_app.js', HDR_APP, [(193, 236)]),
    ('20_api_nw_window.js', HDR_WINDOW, [(241, 444)]),
    ('40_api_nw_menuitem.js', HDR_MENUITEM, [(449, 450), (452, 481)]),
    ('41_api_nw_menu.js', HDR_MENU, [(483, 510)]),
    ('50_api_nw_clipboard.js', HDR_CLIPBOARD, [(564, 570), (594, 594), (596, 600)]),
    ('51_api_nw_shell.js', HDR_SHELL, [(572, 576), (592, 592)]),
    ('52_api_nw_screen.js', HDR_SCREEN, [(578, 580), (593, 593)]),
    ('53_api_nw_shortcut.js', HDR_SHORTCUT, [(602, 627)]),
    ('54_api_nw_tray.js', HDR_TRAY, [(515, 559)]),
    ('90_install.js', HDR_INSTALL, [(632, 671), (676, 693)]),
]

# 原文第一行是"nw_api.js 是一个文件"的口吻，搬进模块后要改掉。
FIXUPS = [
    ('// nw_api.js —— 在页面里重建 nw.* 的那一层。',
     '// 本层（nw\\api\\*.js）—— 在页面里重建 nw.*，按 SDK 的模块边界拆成多个文件。'),
]

if not os.path.isdir(OUTDIR):
    os.makedirs(OUTDIR)

total = 0
for name, header, ranges in SPEC:
    body = []
    for a, b in ranges:
        piece = sl(a, b)
        # 去掉切片两端的空行，避免文件之间出现一堆连续空行
        while piece and piece[0].strip() == '':
            piece = piece[1:]
        while piece and piece[-1].strip() == '':
            piece = piece[:-1]
        if not piece:
            raise SystemExit('range %d-%d of %s is empty' % (a, b, name))
        body.extend(piece)
    for old, new in FIXUPS:
        for i, line in enumerate(body):
            if line == old:
                body[i] = new
    content = NL.join(header + [''] + body) + NL
    with io.open(os.path.join(OUTDIR, name), 'w', encoding='utf-8', newline='') as handle:
        handle.write(content)
    total += len(body)
    print('[split-api] %-26s %4d lines' % (name, len(body) + len(header) + 1))

print('[split-api] wrote %d files, %d body lines' % (len(SPEC), total))

# ---------------------------------------------------------------------------
# 自检一：把写出去的文件"反向拼回去"，必须与原文对应行逐行相同。
# 只看文件大小/行数会漏掉错位，逐行比对才是"搬家没丢语句"的硬证据。
# ---------------------------------------------------------------------------
expected = []
for name, header, ranges in SPEC:
    for a, b in ranges:
        piece = sl(a, b)
        while piece and piece[0].strip() == '':
            piece = piece[1:]
        while piece and piece[-1].strip() == '':
            piece = piece[:-1]
        expected.extend(piece)
for old, new in FIXUPS:
    expected = [new if line == old else line for line in expected]

actual = []
for name, header, ranges in SPEC:
    with io.open(os.path.join(OUTDIR, name), encoding='utf-8') as handle:
        got = handle.read().split('\n')
    if got and got[-1] == '':
        got.pop()
    actual.extend(got[len(header) + 1:])          # 跳过模块头 + 那个空行

if actual == expected:
    print('[split-api] verify A OK: %d body lines, identical to source ranges' % len(expected))
else:
    bad = [i for i, pair in enumerate(zip(expected, actual)) if pair[0] != pair[1]]
    print('[split-api] verify A FAIL: expected %d lines / got %d / first diff %s'
          % (len(expected), len(actual), bad[0] if bad else 'length'))
    raise SystemExit(1)

# ---------------------------------------------------------------------------
# 自检二：没被搬走的非空行必须**只有**外层包装那几句。不打印出来就没法确认
# 某一个 range 是不是漏掉了一段真代码——这是最容易犯的错。
# 输出统一转成 unicode_escape，免得中文经控制台编码再看不清。
# ---------------------------------------------------------------------------
covered = set()
for name, header, ranges in SPEC:
    for a, b in ranges:
        covered.update(range(a, b + 1))
left = [(i, lines[i - 1]) for i in range(1, len(lines) + 1)
        if i not in covered and lines[i - 1].strip() != '']
print('[split-api] not moved (%d non-blank lines):' % len(left))
for number, line in left:
    print('    %4d | %s' % (number, line.encode('unicode_escape').decode('ascii')))
