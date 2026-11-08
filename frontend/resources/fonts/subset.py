"""把 frontend/resources/fonts 里的字体子集化。

两种模式：

1. 默认（内容字体，霞鹜文楷 / 霞鹜文楷等宽）：字符集要"宽"。
   字符集口径（与同目录 README 一致）：
   - 拉丁：ASCII 0x20-0x7E、Latin-1 0xA0-0xFF、Latin Ext-A 0x100-0x17F
   - 符号：General Punctuation、Currency、Arrows、Math Operators、Geometric Shapes、
           Misc Symbols / Dingbats（界面用到的 ▾ ✓ ✕ × ≈ 等都在这些区）
   - 中日韩：CJK 标点 0x3000-0x303F、平假名/片假名 0x3040-0x30FF、半角全角 0xFF00-0xFFEF
   - 汉字：GB2312 全部 + JIS X 0208 的汉字（日文原文要用）
   - 韩语：KS X 1001 的 2350 个常用音节
   - 项目里实际出现过的字：扫 frontend/**/*.qml、*.cpp、*.h、*.ts
   理由：译文/原文是任意文本，缺字就是豆腐块。

2. `--ui`（界面字体，思源黑体 Regular/Medium/Bold）：字符集要"窄"。
   只保留"界面上真的会出现的字"：扫 frontend/qml、frontend/src、backend/src、
   backend/include、frontend/translations 得到的全部非 ASCII 字符，加上 ASCII
   可打印区与中英标点区（版面需要）。缺的字由系统字体回落兜底，所以用户自己
   输入的生僻字不会变成方块，只是字形来自系统字体。

用法（在仓库根目录执行）：
    python frontend/resources/fonts/subset.py          # 内容字体（宽）
    python frontend/resources/fonts/subset.py --ui     # 界面字体（按实际用字）

结果写在 _fontbuild/，核对无误后再覆盖 frontend/resources/fonts/ 里的同名文件。
"""

from __future__ import annotations

import os
import sys
from pathlib import Path

FONT_DIR = Path("frontend/resources/fonts")
TMP_DIR = Path("_fontbuild")
CHARSET_FILE = Path("_charset.txt")
UI_CHARSET_FILE = Path("_charset_ui.txt")


def ranges(*pairs: tuple[int, int]) -> set[str]:
    out: set[str] = set()
    for lo, hi in pairs:
        out.update(chr(cp) for cp in range(lo, hi + 1))
    return out


def double_byte_charset(codec: str, lead: range, trail: list[range]) -> set[str]:
    out: set[str] = set()
    for a in lead:
        for b_range in trail:
            for b in b_range:
                try:
                    out.add(bytes([a, b]).decode(codec))
                except UnicodeDecodeError:
                    pass
    return out


def repo_chars() -> set[str]:
    out: set[str] = set()
    roots = [Path("frontend/qml"), Path("frontend/src"), Path("frontend/translations")]
    for root in roots:
        for path in root.rglob("*"):
            if path.suffix.lower() in {".qml", ".cpp", ".h", ".ts", ".js", ".md"}:
                out.update(path.read_text(encoding="utf-8", errors="ignore"))
    return out


# 界面字体（思源黑体）的扫描范围：只关心"界面上真的会出现的字"。
UI_SCAN_ROOTS = [
    (Path("frontend/qml"), {".qml", ".js"}),
    (Path("frontend/src"), {".cpp", ".h"}),
    (Path("backend/src"), {".cpp", ".h"}),
    (Path("backend/include"), {".cpp", ".h"}),
    (Path("frontend/translations"), {".ts"}),
]


def ui_charset() -> set[str]:
    """界面字体用：源码/翻译里实际出现过的字 + 版面必需的标点与 ASCII。

    注意：界面字体不做"用户输入兜底"——缺字由系统字体回落（Qt 自动选字），
    所以用户自己输入的生僻字不会变方块，只是字形来自系统字体。
    """
    chars: set[str] = set()
    for root, suffixes in UI_SCAN_ROOTS:
        if not root.exists():
            continue
        for path in root.rglob("*"):
            if path.suffix.lower() in suffixes:
                chars.update(path.read_text(encoding="utf-8", errors="ignore"))
    # 版面必需的区段：ASCII、Latin-1、通用标点、CJK 标点、全角半角、箭头/符号。
    # 这些区段按整段保留，避免"少了一个逗号"这种事。
    chars |= ranges(
        (0x20, 0x7E),
        (0xA0, 0xFF),
        (0x2000, 0x206F),
        (0x2190, 0x21FF),
        (0x2200, 0x22FF),
        (0x25A0, 0x25FF),
        (0x2600, 0x27BF),
        (0x3000, 0x303F),
        (0xFF00, 0xFFEF),
    )
    for drop in ("\n", "\r", "\t", "\u00a0"):
        chars.discard(drop)
    return {c for c in chars if c.isprintable() or c in " "}


def build_charset() -> set[str]:
    chars = set()
    chars |= ranges(
        (0x20, 0x7E),      # ASCII
        (0xA0, 0xFF),      # Latin-1
        (0x100, 0x17F),    # Latin Ext-A
        (0x2000, 0x206F),  # 标点
        (0x20A0, 0x20BF),  # 货币
        (0x2190, 0x219F),  # 箭头
        (0x2200, 0x22FF),  # 数学
        (0x25A0, 0x25FF),  # 几何（▾ ▸ ■ ●）
        (0x2600, 0x27BF),  # 杂项符号（✓ ✔ ✕ ✂）
        (0x3000, 0x303F),  # CJK 标点
        (0x3040, 0x30FF),  # 假名
        (0x31F0, 0x31FF),  # 假名扩展
        (0xFF00, 0xFFEF),  # 全角/半角
    )
    # 简体常用字（GB2312 6763 字）。
    chars |= double_byte_charset("gb2312", range(0xA1, 0xF8), [range(0xA1, 0xFF)])
    # 日语汉字：JIS X 0208 里的汉字（汉字部分约 6000 字，与 GB2312 有重叠）。
    # 游戏/漫画的日文原文要能正常显示，所以这部分必须留。
    jis = double_byte_charset("cp932", range(0x81, 0xFF),
                              [range(0x40, 0x7F), range(0x80, 0xFD)])
    chars |= {c for c in jis if "\u4e00" <= c <= "\u9fff"}
    # 韩语：KS X 1001 的 2350 个常用音节（全量谚文有 11172 个，太占地方）。
    ks = double_byte_charset("euc-kr", range(0xA1, 0xFF), [range(0xA1, 0xFF)])
    chars |= {c for c in ks if "\uac00" <= c <= "\ud7a3"}
    # 项目里实际出现过的字（界面文案、翻译文件）。
    chars |= repo_chars()
    chars.discard("\n")
    chars.discard("\r")
    chars.discard("\t")
    return chars


def main() -> int:
    try:
        from fontTools import subset  # noqa: F401
    except ImportError:
        print("需要 fontTools：python -m pip install fonttools", file=sys.stderr)
        return 1

    ui_mode = "--ui" in sys.argv[1:]

    if ui_mode:
        chars = ui_charset()
        charset_file = UI_CHARSET_FILE
        targets = sorted(FONT_DIR.glob("StudiousSans-*"))
    else:
        chars = build_charset()
        charset_file = CHARSET_FILE
        targets = sorted(FONT_DIR.glob("Studious*"))

    han = [c for c in chars if "\u4e00" <= c <= "\u9fff"]
    print(f"字符集：{len(chars)} 个（汉字 {len(han)}）")
    charset_file.write_text("".join(sorted(chars)), encoding="utf-8")

    TMP_DIR.mkdir(exist_ok=True)
    total_before = 0
    total_after = 0
    import subprocess
    for font in targets:
        if font.suffix.lower() not in {".otf", ".ttf"}:
            continue
        out = TMP_DIR / font.name
        args = [
            sys.executable, "-m", "fontTools.subset", str(font),
            f"--text-file={charset_file}",
            f"--output-file={out}",
            "--layout-features=*",
            "--name-IDs=*",
            "--name-languages=*",
            "--name-legacy",
            "--glyph-names",
            "--symbol-cmap",
            "--legacy-cmap",
            "--notdef-glyph",
            "--notdef-outline",
            "--recommended-glyphs",
        ]
        result = subprocess.run(args, capture_output=True, text=True)
        if result.returncode != 0:
            print(result.stdout[-3000:])
            print(result.stderr[-3000:])
            return result.returncode
        before = font.stat().st_size
        after = out.stat().st_size
        total_before += before
        total_after += after
        print(f"{font.name}: {before/1048576:.1f} MB -> {after/1048576:.1f} MB")

    print(f"合计：{total_before/1048576:.1f} MB -> {total_after/1048576:.1f} MB")
    print(f"产物在 {TMP_DIR}/，核对后覆盖 {FONT_DIR}/ 里的同名文件。")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
