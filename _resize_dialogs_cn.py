# -*- coding: utf-8 -*-
"""
PUI 对话框中文加宽扫改脚本（设计文档 docs/pui-theme-design.md §4「对话框扫改」）。

功能概述：
  1. 读取 Install/fgdata_2024_1/fgdata-overlay/Translations/zh_CN/dialog.xml 建立「英文原文 -> 中文译文」映射；
  2. 遍历 Install/fgdata_2024_1/gui/dialogs/*.xml，对每个同时含 <legend>/<label> 与 <width>
     的控件元素，按中文字符宽度估算所需像素宽：
         CJK 字符 ≈ 1.0em，拉丁/数字/半角符号/空格 ≈ 0.5em，em = 字号 20px
     （字号取自 gui/styles/anthrax.xml 的 <fonts>/<gui>/<size>20</size>；本估算公式与调研结论一致，
      真实字体 advance 未实测，故只作为「加宽下限」的近似值）；
  3. 当「估算宽 + 16px 边距」超过现有 <width> 时加宽：目标宽向上取整到 5 的倍数，
     并限制在原宽的 1.6 倍以内（只增不减，避免破坏既有布局）；
  4. 把 <list>/<loglist>/<textbox> 内部的标量滚动条宽度 <slider>20</slider> 改为 12；
  5. 修改前把原文件备份到系统临时目录；采用「行级最小编辑」（只替换目标行里的数字），
     不做整文件 XML 重写，避免 diff 爆炸和格式/换行/缩进被破坏；
  6. 输出统计报告到工作区根 _resize_dialogs_cn_report.txt；
  7. 把每个被修改的文件同步复制到 Install/fgdata_2024_1/fgdata-overlay/gui/dialogs/（保持相对路径），
     因为 Install/ 目录被 .gitignore 忽略，overlay 才是可提交的交付物。

用法（在工作区根目录执行）：
    python _resize_dialogs_cn.py
脚本路径全部以脚本自身所在目录为基准解析，因此不依赖当前工作目录。
可重复执行：报告末尾的 #STATE# 行记录了每个被加宽 <width> 的「原宽」，
第二次运行时据此还原基准值，保证重复执行结果一致（幂等）。
"""

import html
import json
import math
import re
import shutil
import sys
import tempfile
from bisect import bisect_right
from pathlib import Path
import xml.etree.ElementTree as ET

# ---------------------------------------------------------------------------
# 路径常量：全部相对脚本所在目录（即工作区根）解析，避免 CWD 依赖
# ---------------------------------------------------------------------------
WORKSPACE = Path(__file__).resolve().parent
# 安装目录中的对话框 XML（运行时真正读取的数据；该目录被 git 忽略）
INSTALL_DIALOGS = WORKSPACE / "Install" / "fgdata_2024_1" / "gui" / "dialogs"
# 可提交的覆盖目录（相对路径镜像 $FG_ROOT）
OVERLAY_DIALOGS = WORKSPACE / "Install" / "fgdata_2024_1" / "fgdata-overlay" / "gui" / "dialogs"
# 翻译表：以 overlay 副本为权威来源（与 Install 副本内容一致）
TRANSLATION_FILE = (WORKSPACE / "Install" / "fgdata_2024_1" / "fgdata-overlay" / "Translations" /
                    "zh_CN" / "dialog.xml")
# 统计报告输出路径
REPORT_FILE = WORKSPACE / "_resize_dialogs_cn_report.txt"

# ---------------------------------------------------------------------------
# 估算与加宽参数（设计文档 §4 明确规定的数值）
# ---------------------------------------------------------------------------
EM_PX = 20.0        # 字号 20px：anthrax.xml 中 <fonts>/<gui>/<size> 为 20
MARGIN_PX = 16      # 估算宽度之外预留的安全边距（像素）
MAX_SCALE = 1.6     # 加宽上限：不超过原宽的 1.6 倍
ROUND_STEP = 5      # 目标宽度向上取整到 5 的倍数
SLIDER_NEW_WIDTH = 12       # 滚动条新宽度
SLIDER_OLD_WIDTH = 20       # 需要被替换的旧滚动条宽度（数据中实际存在的 20）
# 标量 <slider>N</slider> 只在这些复合控件内部表示「滚动条宽度」
SCROLL_PARENT_TAGS = {"list", "loglist", "textbox"}

# ---------------------------------------------------------------------------
# 轻量 XML 结构解析：ElementTree 不保留行号，而本脚本需要做行级最小编辑，
# 因此用正则分词 + 栈模拟，记录每个元素的起始行、直接子元素与文本内容。
# 对话框 XML 结构简单（无 CDATA、无带 '>' 的属性值），该解析足够健壮。
# ---------------------------------------------------------------------------
TAG_RE = re.compile(r"<(/?)([A-Za-z_][\w.:-]*)([^<>]*?)(/?)>", re.S)
COMMENT_RE = re.compile(r"<!--.*?-->", re.S)


class Elem:
    """解析结果中的一个 XML 元素节点（只保留本脚本需要的字段）。"""

    __slots__ = ("tag", "line", "start", "end_line", "text", "children", "parent")

    def __init__(self, tag, line, start, parent):
        self.tag = tag            # 元素名（如 width/label/slider）
        self.line = line          # 起始标签所在行号（1 起）
        self.start = start        # 起始标签结束位置（用于截取文本）
        self.end_line = line      # 结束标签所在行号
        self.text = ""            # 叶子元素的文本内容（已做 XML 实体反转义）
        self.children = []        # 直接子元素列表
        self.parent = parent      # 父元素引用


def _line_starts(data):
    """返回每一行起始偏移的列表，供 offset -> 行号 的二分查找使用。"""
    starts = [0]
    for m in re.finditer("\n", data):
        starts.append(m.end())
    return starts


def parse_structure(data):
    """解析 XML 文本，返回根元素节点（栈式分词，忽略注释内的标签）。"""
    starts = _line_starts(data)

    def line_of(pos):
        # 二分查找 pos 属于哪一行（返回 1 起的行号）
        return bisect_right(starts, pos)

    # 先记录所有注释区间，用于跳过注释中的伪标签（例如被注释掉的 <slider>）
    comment_spans = [(m.start(), m.end()) for m in COMMENT_RE.finditer(data)]

    def inside_comment(pos):
        # 注释区间数量很少，线性判断即可
        for s, e in comment_spans:
            if s <= pos < e:
                return True
        return False

    root = Elem("__root__", 0, 0, None)
    stack = [root]
    for m in TAG_RE.finditer(data):
        if inside_comment(m.start()):
            continue
        is_close, tag, _attrs, is_selfclose = m.group(1), m.group(2), m.group(3), m.group(4)
        if is_selfclose:                       # <empty/> 之类的自闭合元素：直接作为叶子挂到父节点
            node = Elem(tag, line_of(m.start()), m.end(), stack[-1])
            stack[-1].children.append(node)
        elif not is_close:                     # 起始标签：入栈
            node = Elem(tag, line_of(m.start()), m.end(), stack[-1])
            stack[-1].children.append(node)
            stack.append(node)
        else:                                  # 结束标签：出栈并截取元素文本
            if len(stack) <= 1:
                continue                       # 容错：多余的结束标签
            node = stack.pop()
            node.end_line = line_of(m.start())
            raw = data[node.start:m.start()]
            # 去掉子元素与注释后，若剩余部分不含 '<'，说明这是纯文本叶子
            raw_no_comment = COMMENT_RE.sub("", raw)
            if "<" not in raw_no_comment:
                node.text = html.unescape(raw_no_comment)
    return root


def iter_elems(root):
    """深度优先遍历所有元素节点。"""
    stack = [root]
    while stack:
        node = stack.pop()
        yield node
        stack.extend(reversed(node.children))


def direct_child(elem, tag):
    """取第一个指定名字的直接子元素；不存在返回 None。"""
    for child in elem.children:
        if child.tag == tag:
            return child
    return None


# ---------------------------------------------------------------------------
# 宽度估算
# ---------------------------------------------------------------------------
def is_cjk_char(ch):
    """判断字符是否属于 CJK 相关区段（按 1.0em 估算的字形）。"""
    o = ord(ch)
    return (
        0x2E80 <= o <= 0x9FFF      # CJK 部首扩展 / 汉字 / 假名等
        or 0xF900 <= o <= 0xFAFF   # CJK 兼容汉字
        or 0x3000 <= o <= 0x303F   # CJK 标点（、。「」等）
        or 0xFF00 <= o <= 0xFFEF   # 全角字符
        or 0x20000 <= o <= 0x2FA1F # CJK 扩展 B~F
    )


def estimate_cn_width(text, em_px=EM_PX):
    """估算中文串的像素宽度：CJK ≈ 1.0em，拉丁/数字/半角符号/空格 ≈ 0.5em。

    该公式来自设计文档 §4 / 调研结论，是「字符数 × 字宽系数」的近似值，
    并非真实字体 advance，因此只用于判断是否可能溢出，并在加宽时留 16px 边距。
    """
    width = 0.0
    for ch in text:
        width += em_px if is_cjk_char(ch) else em_px * 0.5
    return width


def compute_new_width(orig_width, est_width):
    """按契约计算加宽后的宽度；返回原值表示不需要修改。

    规则：need = 估算宽 + 16；need 超过原宽时，目标宽 = ceil(need/5)*5，
    上限为 floor(原宽*1.6/5)*5（向下取整到 5 的倍数，保持与目标值的步进一致）；
    若上限仍不大于原宽，则放弃修改（例如原宽 2px 的占位 label）。
    """
    need = est_width + MARGIN_PX
    if need <= orig_width:
        return orig_width
    target = int(math.ceil(need / float(ROUND_STEP))) * ROUND_STEP
    cap = int(math.floor(orig_width * MAX_SCALE / float(ROUND_STEP))) * ROUND_STEP
    if target > cap:
        target = cap
    if target <= orig_width:
        return orig_width
    return target


# ---------------------------------------------------------------------------
# 翻译表与状态读取
# ---------------------------------------------------------------------------
def load_translations():
    """读取 dialog.xml，返回 {英文原文(strip 后): 中文译文}。

    翻译表由 _gen_dialog_xml.py 生成，<source> 已经 strip；运行时标签常带
    首尾空格，因此查找前也必须 strip。
    """
    table = {}
    root = ET.parse(str(TRANSLATION_FILE)).getroot()
    for entry in root.findall("entry"):
        source = entry.findtext("source")
        target = entry.findtext("target")
        if source is None or target is None:
            continue
        table[source.strip()] = target
    return table


def load_previous_state():
    """从上次报告末尾的 #STATE# 行读取 {「文件:行号」: 原宽}，用于幂等重跑。

    行号在「只改数字、不改行数」的编辑方式下是稳定的，因此可以作为键。
    """
    if not REPORT_FILE.exists():
        return {}
    try:
        text = REPORT_FILE.read_text(encoding="utf-8")
    except OSError:
        return {}
    for line in reversed(text.splitlines()):
        if line.startswith("#STATE#"):
            try:
                return {str(k): int(v) for k, v in json.loads(line[7:].strip()).items()}
            except (ValueError, TypeError):
                return {}
    return {}


# ---------------------------------------------------------------------------
# 主流程
# ---------------------------------------------------------------------------
def main():
    # 让 Windows 控制台的统计输出尽量使用 UTF-8，避免中文乱码/异常
    try:
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    except AttributeError:
        pass

    if not INSTALL_DIALOGS.is_dir():
        print("错误：找不到对话框目录 %s" % INSTALL_DIALOGS)
        return 1

    translations = load_translations()
    prev_state = load_previous_state()

    # 每条修改记录：(文件相对名, 行号, 元素类型, 原值, 新值, 说明)
    width_changes = []
    slider_changes = []
    # 新的状态表：记录所有被加宽 <width> 的原宽，供下一次幂等重跑
    new_state = {}
    # 内存中的待写内容：相对路径 -> (原文本, 修改后的行列表)
    pending = {}
    # 需要备份的原文件列表
    backup_dir = Path(tempfile.mkdtemp(prefix="resize_dialogs_backup_"))

    xml_files = sorted(INSTALL_DIALOGS.glob("*.xml"))
    parse_errors = []

    for path in xml_files:
        rel = path.name                    # 对话框均在顶层，相对名即文件名
        raw_bytes = path.read_bytes()
        # 这些文件为 UTF-8 无 BOM；用 UTF-8 解码并按保留换行符的方式切行
        try:
            text = raw_bytes.decode("utf-8")
        except UnicodeDecodeError:
            parse_errors.append("%s: 不是合法的 UTF-8" % rel)
            continue
        lines = text.splitlines(keepends=True)
        edits = {}                         # 行索引(0 起) -> 新行文本（含换行符）

        try:
            root = parse_structure(text)
        except Exception as exc:           # 解析失败只跳过该文件，稍后由 ElementTree 校验兜底
            parse_errors.append("%s: 结构解析失败 %s" % (rel, exc))
            continue

        for elem in iter_elems(root):
            width_node = direct_child(elem, "width")
            label_node = direct_child(elem, "label")
            if label_node is None:
                label_node = direct_child(elem, "legend")

            # ---- 规则 1：含 <legend>/<label> 且带 <width> 的控件加宽 ----
            if (width_node is not None and label_node is not None
                    and width_node.text.strip().isdigit()):
                src_text = label_node.text.strip()
                target_text = translations.get(src_text)
                if not target_text:
                    continue               # 无译文时显示英文，不做加宽
                cur_width = int(width_node.text.strip())
                # 幂等：若上一次报告记录了该行的原宽，以其为基准重算
                state_key = "%s:%d" % (rel, width_node.line)
                base_width = prev_state.get(state_key, cur_width)
                est = estimate_cn_width(target_text)
                new_width = compute_new_width(base_width, est)
                if new_width == cur_width:
                    if state_key in prev_state:
                        new_state[state_key] = base_width  # 保留状态
                    continue
                if new_width <= cur_width:
                    continue

                line_idx = width_node.line - 1
                if not (0 <= line_idx < len(lines)):
                    continue
                old_line = edits.get(line_idx, lines[line_idx])
                # 安全护栏：目标行必须恰好只有一个 <width> 标签，
                # 否则正则可能改到同一行里另一个控件的宽度，宁可不改。
                if old_line.count("<width>") != 1:
                    parse_errors.append("%s:%d 同一行存在多个 <width>，已跳过"
                                        % (rel, width_node.line))
                    continue
                # 行级最小编辑：只替换 <width> 标签之间的数字，缩进/换行原样保留
                new_line = re.sub(r"(<width>\s*)(\d+)(\s*</width>)",
                                  lambda m: m.group(1) + str(new_width) + m.group(3),
                                  old_line, count=1)
                if new_line == old_line:
                    continue               # 行内未找到可替换的 width，安全放弃
                edits[line_idx] = new_line
                width_changes.append((rel, width_node.line, elem.tag,
                                      cur_width, new_width, src_text, target_text,
                                      round(est, 1)))
                new_state[state_key] = base_width
                continue

            # ---- 规则 2：<list>/<loglist>/<textbox> 内部标量滚动条 20 -> 12 ----
            if (elem.tag == "slider" and not elem.children
                    and elem.parent is not None
                    and elem.parent.tag in SCROLL_PARENT_TAGS
                    and elem.text.strip().isdigit()
                    and int(elem.text.strip()) == SLIDER_OLD_WIDTH):
                line_idx = elem.line - 1
                if not (0 <= line_idx < len(lines)):
                    continue
                old_line = edits.get(line_idx, lines[line_idx])
                # 安全护栏：标量滚动条所在行必须只有一个 <slider>，避免误改其他控件
                if old_line.count("<slider>") != 1:
                    parse_errors.append("%s:%d 同一行存在多个 <slider>，已跳过"
                                        % (rel, elem.line))
                    continue
                new_line = re.sub(r"(<slider>\s*)(\d+)(\s*</slider>)",
                                  lambda m: m.group(1) + str(SLIDER_NEW_WIDTH) + m.group(3),
                                  old_line, count=1)
                if new_line == old_line:
                    continue
                edits[line_idx] = new_line
                slider_changes.append((rel, elem.line, elem.parent.tag,
                                       SLIDER_OLD_WIDTH, SLIDER_NEW_WIDTH))

        if edits:
            pending[rel] = (raw_bytes, lines, edits)

    # -----------------------------------------------------------------------
    # 写回文件（先备份），随后整体校验 XML 合法性
    # -----------------------------------------------------------------------
    written = []
    for rel, (raw_bytes, lines, edits) in pending.items():
        path = INSTALL_DIALOGS / rel
        # 备份原文件到临时目录（保持相对路径结构）
        backup_path = backup_dir / rel
        backup_path.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(str(path), str(backup_path))

        new_lines = list(lines)
        for idx, new_line in edits.items():
            new_lines[idx] = new_line
        new_text = "".join(new_lines)
        path.write_bytes(new_text.encode("utf-8"))
        written.append(rel)

    # 全量 XML 合法性校验：任何文件解析失败则从备份回滚并报错退出
    invalid = []
    for path in xml_files:
        try:
            ET.parse(str(path))
        except ET.ParseError as exc:
            invalid.append("%s: %s" % (path.name, exc))
    if invalid:
        for rel in written:
            shutil.copy2(str(backup_dir / rel), str(INSTALL_DIALOGS / rel))
        print("错误：修改后 XML 校验失败，已回滚：")
        for item in invalid:
            print("  " + item)
        return 1

    # -----------------------------------------------------------------------
    # 同步到 Install/fgdata_2024_1/fgdata-overlay/gui/dialogs（可提交副本）
    # -----------------------------------------------------------------------
    for rel in written:
        dst = OVERLAY_DIALOGS / rel
        dst.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(str(INSTALL_DIALOGS / rel), str(dst))

    # -----------------------------------------------------------------------
    # 生成统计报告
    # -----------------------------------------------------------------------
    report_lines = []
    report_lines.append("PUI 对话框中文加宽扫改报告")
    report_lines.append("=" * 60)
    report_lines.append("扫描对话框文件数：%d" % len(xml_files))
    report_lines.append("修改文件数：%d" % len(written))
    report_lines.append("加宽控件数：%d" % len(width_changes))
    report_lines.append("滚动条宽度修改数：%d" % len(slider_changes))
    max_grow = 0
    max_grow_desc = "无"
    for change in width_changes:
        grow = change[4] - change[3]
        if grow > max_grow:
            max_grow = grow
            max_grow_desc = "%s:%d <%s> %d -> %d" % (change[0], change[1],
                                                     change[2], change[3], change[4])
    report_lines.append("最大加宽值：+%d px（%s）" % (max_grow, max_grow_desc))
    report_lines.append("")
    report_lines.append("按文件明细（宽度修改）：")
    if not width_changes:
        report_lines.append("  （无）")
    current_file = None
    for rel, line_no, elem_tag, old_w, new_w, src, tgt, est in width_changes:
        if rel != current_file:
            report_lines.append("  %s" % rel)
            current_file = rel
        report_lines.append("    行 %-5d <%s> 宽 %d -> %d（估算中文宽 %.1fpx）"
                            % (line_no, elem_tag, old_w, new_w, est))
        report_lines.append("      原文：%r" % src)
        report_lines.append("      译文：%r" % tgt)
    report_lines.append("")
    report_lines.append("滚动条明细（<list>/<loglist>/<textbox> 的标量 <slider>）：")
    if not slider_changes:
        report_lines.append("  （无）")
    for rel, line_no, parent_tag, old_w, new_w in slider_changes:
        report_lines.append("  %s:%d <%s> 滚动条 %d -> %d"
                            % (rel, line_no, parent_tag, old_w, new_w))
    report_lines.append("")
    report_lines.append("修改文件列表：")
    for rel in written:
        report_lines.append("  %s" % rel)
    report_lines.append("")
    report_lines.append("原文件备份目录：%s" % backup_dir)
    report_lines.append("")
    if parse_errors:
        report_lines.append("解析警告（已跳过）：")
        for item in parse_errors:
            report_lines.append("  " + item)
        report_lines.append("")
    # 幂等状态：下一次运行时据此把「已加宽后的值」还原为「原宽」作为计算基准
    report_lines.append("#STATE# " + json.dumps(new_state, ensure_ascii=False, sort_keys=True))

    REPORT_FILE.write_text("\n".join(report_lines) + "\n",
                           encoding="utf-8", newline="\n")

    # 控制台摘要
    print("扫描 %d 个对话框文件，修改 %d 个文件" % (len(xml_files), len(written)))
    print("加宽控件 %d 个，滚动条修改 %d 处，最大加宽 +%d px"
          % (len(width_changes), len(slider_changes), max_grow))
    print("报告：%s" % REPORT_FILE)
    print("备份：%s" % backup_dir)
    return 0


if __name__ == "__main__":
    sys.exit(main())
