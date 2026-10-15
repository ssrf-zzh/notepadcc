# -*- coding: utf-8 -*-
"""
把 notepadcc详细设计说明书.md 转换为排版良好的 .docx。

用法:
    python tools/md2docx.py <input.md> <output.docx> [--no-com]

支持: ATX 标题、段落、围栏代码块(可缩进)、Markdown 表格、
      有序/无序列表、引用、分隔线、行内 **加粗** 与 `代码`。
"""
import re
import sys

from docx import Document
from docx.enum.section import WD_SECTION
from docx.enum.table import WD_TABLE_ALIGNMENT
from docx.enum.text import WD_ALIGN_PARAGRAPH
from docx.oxml import OxmlElement
from docx.oxml.ns import qn
from docx.shared import Cm, Pt, RGBColor

EA_FONT = "微软雅黑"
LATIN_FONT = "Segoe UI"
CODE_FONT = "Consolas"

# 正文里 Markdown 的 h2/h3/h4 会映射为 Word 的 Heading 1/2/3，因此封面之外没有 Heading 1 冲突
HEAD_SIZE = {1: 15.5, 2: 13, 3: 11.5, 4: 11, 5: 10.5, 6: 10.5}
HEAD_COLOR = {
    1: RGBColor(0x1F, 0x3B, 0x63),
    2: RGBColor(0x1F, 0x4E, 0x79),
    3: RGBColor(0x2E, 0x5C, 0x8A),
    4: RGBColor(0x3A, 0x3A, 0x3A),
}

TOKEN_RE = re.compile(r"(\*\*.+?\*\*|`[^`]+`)")
FENCE_RE = re.compile(r"^\s*```\s*([A-Za-z0-9+#-]*)\s*$")
HEAD_RE = re.compile(r"^(#{1,6})\s+(.*?)\s*$")
UL_RE = re.compile(r"^(\s*)[-*+]\s+(.*)$")
OL_RE = re.compile(r"^(\s*)\d+\.\s+(.*)$")
HR_RE = re.compile(r"^\s*(-{3,}|\*{3,}|_{3,})\s*$")
TABLE_RE = re.compile(r"^\s*\|")


# ---------------------------------------------------------------- 低层工具
def set_run_font(run, latin=None, ea=None, size=None):
    rPr = run._element.get_or_add_rPr()
    rFonts = rPr.find(qn("w:rFonts"))
    if rFonts is None:
        rFonts = OxmlElement("w:rFonts")
        rPr.insert(0, rFonts)
    if latin:
        rFonts.set(qn("w:ascii"), latin)
        rFonts.set(qn("w:hAnsi"), latin)
    if ea:
        rFonts.set(qn("w:eastAsia"), ea)
    if size is not None:
        run.font.size = Pt(size)


RPR_AFTER_SHD = (
    "w:fitText", "w:vertAlign", "w:rtl", "w:cs", "w:em",
    "w:lang", "w:eastAsianLayout", "w:specVanish", "w:oMath",
)
PPR_AFTER_SHD = (
    "w:tabs", "w:suppressAutoHyphens", "w:kinsoku", "w:wordWrap",
    "w:overflowPunct", "w:topLinePunct", "w:autoSpaceDE", "w:autoSpaceDN",
    "w:bidi", "w:adjustRightInd", "w:snapToGrid", "w:spacing", "w:ind",
    "w:contextualSpacing", "w:mirrorIndents", "w:suppressOverlap", "w:jc",
    "w:textDirection", "w:textAlignment", "w:textboxTightWrap",
    "w:outlineLvl", "w:divId", "w:cnfStyle", "w:rPr", "w:sectPr", "w:pPrChange",
)
PPR_AFTER_PBDR = ("w:shd",) + PPR_AFTER_SHD
TCPR_AFTER_SHD = (
    "w:noWrap", "w:tcMar", "w:textDirection", "w:tcFitText",
    "w:vAlign", "w:hideMark",
)


def _shd(fill):
    shd = OxmlElement("w:shd")
    shd.set(qn("w:val"), "clear")
    shd.set(qn("w:color"), "auto")
    shd.set(qn("w:fill"), fill)
    return shd


def shade_rPr(rPr, fill):
    """底纹在 rPr 中必须位于 u/bdr 之后、fitText 之前。"""
    if rPr.find(qn("w:shd")) is not None:
        return
    rPr.insert_element_before(_shd(fill), *RPR_AFTER_SHD)


def shade_pPr(pPr, fill):
    if pPr.find(qn("w:shd")) is not None:
        return
    pPr.insert_element_before(_shd(fill), *PPR_AFTER_SHD)


def shade_tcPr(tcPr, fill):
    if tcPr.find(qn("w:shd")) is not None:
        return
    tcPr.insert_element_before(_shd(fill), *TCPR_AFTER_SHD)


def add_paragraph_border(paragraph):
    pPr = paragraph._p.get_or_add_pPr()
    pbdr = OxmlElement("w:pBdr")
    bottom = OxmlElement("w:bottom")
    bottom.set(qn("w:val"), "single")
    bottom.set(qn("w:sz"), "6")
    bottom.set(qn("w:space"), "1")
    bottom.set(qn("w:color"), "BFBFBF")
    pbdr.append(bottom)
    pPr.insert_element_before(pbdr, *PPR_AFTER_PBDR)


def style_font(style, latin, ea, size):
    rPr = style.element.get_or_add_rPr()
    rFonts = rPr.find(qn("w:rFonts"))
    if rFonts is None:
        rFonts = OxmlElement("w:rFonts")
        rPr.insert(0, rFonts)
    rFonts.set(qn("w:ascii"), latin)
    rFonts.set(qn("w:hAnsi"), latin)
    rFonts.set(qn("w:eastAsia"), ea)
    style.font.size = Pt(size)


def add_inline(paragraph, text):
    """按 **加粗** 与 `代码` 切分文本并写入段落。"""
    for part in TOKEN_RE.split(text):
        if not part:
            continue
        if part.startswith("**") and part.endswith("**") and len(part) > 4:
            run = paragraph.add_run(part[2:-2])
            run.bold = True
            set_run_font(run, LATIN_FONT, EA_FONT)
        elif part.startswith("`") and part.endswith("`") and len(part) > 2:
            run = paragraph.add_run(part[1:-1])
            set_run_font(run, CODE_FONT, EA_FONT, 9.5)
            shade_rPr(run._element.get_or_add_rPr(), "F2F2F2")
        else:
            run = paragraph.add_run(part)
            set_run_font(run, LATIN_FONT, EA_FONT)


def add_field(paragraph, instr, placeholder=""):
    """插入一个域，各 fldChar 必须分属不同的 run，否则 Word 无法解析。"""
    begin = OxmlElement("w:fldChar")
    begin.set(qn("w:fldCharType"), "begin")
    instr_el = OxmlElement("w:instrText")
    instr_el.set(qn("xml:space"), "preserve")
    instr_el.text = instr
    sep = OxmlElement("w:fldChar")
    sep.set(qn("w:fldCharType"), "separate")
    tail = OxmlElement("w:fldChar")
    tail.set(qn("w:fldCharType"), "end")

    paragraph.add_run()._r.append(begin)
    paragraph.add_run()._r.append(instr_el)
    paragraph.add_run()._r.append(sep)
    result = paragraph.add_run(placeholder or " ")
    paragraph.add_run()._r.append(tail)
    return result


# ---------------------------------------------------------------- 样式准备
def build_styles(doc):
    normal = doc.styles["Normal"]
    style_font(normal, LATIN_FONT, EA_FONT, 10.5)
    normal.paragraph_format.space_after = Pt(4)
    normal.paragraph_format.line_spacing = 1.35
    normal.paragraph_format.widow_control = True

    for level, size in HEAD_SIZE.items():
        st = doc.styles[f"Heading {level}"]
        style_font(st, LATIN_FONT, EA_FONT, size)
        st.font.bold = True
        st.font.color.rgb = HEAD_COLOR.get(level, HEAD_COLOR[4])
        pf = st.paragraph_format
        pf.space_before = Pt(14 if level <= 2 else 8)
        pf.space_after = Pt(6 if level <= 2 else 4)
        pf.keep_with_next = True
        pf.line_spacing = 1.25

    code = doc.styles.add_style("CodeBlock", 1)  # 1 = paragraph
    code.base_style = doc.styles["Normal"]
    style_font(code, CODE_FONT, EA_FONT, 9)
    cpf = code.paragraph_format
    cpf.space_before = Pt(0)
    cpf.space_after = Pt(0)
    cpf.line_spacing = 1.0
    cpf.left_indent = Cm(0.4)
    cpf.keep_together = True
    cpf.keep_with_next = True

    quote = doc.styles.add_style("QuoteBlock", 1)
    quote.base_style = doc.styles["Normal"]
    style_font(quote, LATIN_FONT, EA_FONT, 10)
    quote.font.italic = True
    qpf = quote.paragraph_format
    qpf.left_indent = Cm(0.8)
    qpf.space_before = Pt(4)
    qpf.space_after = Pt(4)


def setup_section(doc):
    sec = doc.sections[0]
    sec.page_width = Cm(21.0)
    sec.page_height = Cm(29.7)
    sec.left_margin = Cm(2.4)
    sec.right_margin = Cm(2.2)
    sec.top_margin = Cm(2.4)
    sec.bottom_margin = Cm(2.2)
    sec.header_distance = Cm(1.3)
    sec.footer_distance = Cm(1.3)

    hp = sec.header.paragraphs[0]
    hp.alignment = WD_ALIGN_PARAGRAPH.RIGHT
    hr = hp.add_run("notepadcc 详细设计说明书")
    set_run_font(hr, LATIN_FONT, EA_FONT, 8.5)
    hr.font.color.rgb = RGBColor(0x80, 0x80, 0x80)

    fp = sec.footer.paragraphs[0]
    fp.alignment = WD_ALIGN_PARAGRAPH.CENTER
    r1 = fp.add_run("第 ")
    set_run_font(r1, LATIN_FONT, EA_FONT, 8.5)
    add_field(fp, " PAGE ")
    r2 = fp.add_run(" 页 / 共 ")
    set_run_font(r2, LATIN_FONT, EA_FONT, 8.5)
    add_field(fp, " NUMPAGES ")
    r3 = fp.add_run(" 页")
    set_run_font(r3, LATIN_FONT, EA_FONT, 8.5)
    for r in fp.runs:
        set_run_font(r, LATIN_FONT, EA_FONT, 8.5)
        r.font.color.rgb = RGBColor(0x80, 0x80, 0x80)


# ---------------------------------------------------------------- 块解析
def split_table_row(line):
    line = line.strip()
    if line.startswith("|"):
        line = line[1:]
    if line.endswith("|"):
        line = line[:-1]
    return [c.strip() for c in line.split("|")]


def is_sep_row(cells):
    return all(c and set(c) <= set("-: ") for c in cells)


def parse_blocks(lines):
    blocks = []
    i, n = 0, len(lines)
    while i < n:
        line = lines[i]
        m = FENCE_RE.match(line)
        if m:
            lang = m.group(1)
            code, i = [], i + 1
            while i < n and not FENCE_RE.match(lines[i]):
                code.append(lines[i])
                i += 1
            i += 1
            blocks.append(("code", lang, code))
            continue
        if not line.strip():
            i += 1
            continue
        m = HEAD_RE.match(line)
        if m:
            blocks.append(("head", len(m.group(1)), m.group(2)))
            i += 1
            continue
        if HR_RE.match(line):
            blocks.append(("hr", 0, None))
            i += 1
            continue
        if TABLE_RE.match(line):
            rows = []
            while i < n and TABLE_RE.match(lines[i]):
                cells = split_table_row(lines[i])
                if not is_sep_row(cells):
                    rows.append(cells)
                i += 1
            blocks.append(("table", 0, rows))
            continue
        if line.lstrip().startswith(">"):
            quote = []
            while i < n and lines[i].lstrip().startswith(">"):
                quote.append(lines[i].lstrip()[1:].strip())
                i += 1
            blocks.append(("quote", 0, " ".join(quote)))
            continue
        m = UL_RE.match(line)
        if m:
            blocks.append(("ul", len(m.group(1)) // 3, m.group(2)))
            i += 1
            continue
        m = OL_RE.match(line)
        if m:
            blocks.append(("ol", len(m.group(1)) // 3, (line.strip().split(".", 1)[0], m.group(2))))
            i += 1
            continue
        blocks.append(("para", 0, line.strip()))
        i += 1
    return blocks


# ---------------------------------------------------------------- 渲染
def render_table(doc, rows):
    if not rows:
        return
    cols = max(len(r) for r in rows)
    table = doc.add_table(rows=0, cols=cols)
    table.style = "Table Grid"
    table.alignment = WD_TABLE_ALIGNMENT.CENTER
    table.autofit = True
    for ridx, row in enumerate(rows):
        cells = table.add_row().cells
        for cidx in range(cols):
            text = row[cidx] if cidx < len(row) else ""
            para = cells[cidx].paragraphs[0]
            para.paragraph_format.space_after = Pt(1)
            para.paragraph_format.space_before = Pt(1)
            para.paragraph_format.line_spacing = 1.15
            add_inline(para, text)
            for run in para.runs:
                if run.font.size is None or run.font.size > Pt(9.5):
                    set_run_font(run, LATIN_FONT, EA_FONT, 9.5)
                if ridx == 0:
                    run.bold = True
            if ridx == 0:
                shade_tcPr(cells[cidx]._tc.get_or_add_tcPr(), "DCE6F1")
    doc.add_paragraph().paragraph_format.space_after = Pt(0)


def render(doc, blocks, title_text):
    cover_done = False
    for kind, meta, payload in blocks:
        if kind == "head" and meta == 1 and not cover_done:
            p = doc.add_paragraph()
            p.alignment = WD_ALIGN_PARAGRAPH.CENTER
            p.paragraph_format.space_before = Pt(90)
            p.paragraph_format.space_after = Pt(18)
            run = p.add_run(payload)
            set_run_font(run, LATIN_FONT, EA_FONT, 26)
            run.bold = True
            run.font.color.rgb = HEAD_COLOR[1]
            cover_done = True
            continue
        if kind == "head":
            # Markdown 的 h2 起映射为 Word 的 Heading 1，保证目录有一级条目
            level = max(1, meta - 1)
            p = doc.add_paragraph(style=f"Heading {level}")
            add_inline(p, payload)
            for run in p.runs:
                run.font.color.rgb = HEAD_COLOR.get(level, HEAD_COLOR[4])
            continue
        if kind == "para":
            p = doc.add_paragraph()
            p.paragraph_format.first_line_indent = Cm(0)
            add_inline(p, payload)
            continue
        if kind == "ul":
            p = doc.add_paragraph(style="List Bullet")
            p.paragraph_format.left_indent = Cm(0.75 + 0.55 * meta)
            p.paragraph_format.space_after = Pt(2)
            add_inline(p, payload)
            continue
        if kind == "ol":
            num, text = payload
            p = doc.add_paragraph()
            p.paragraph_format.left_indent = Cm(0.95 + 0.55 * meta)
            p.paragraph_format.first_line_indent = Cm(-0.55)
            p.paragraph_format.space_after = Pt(2)
            p.add_run(f"{num}. ")
            add_inline(p, text)
            continue
        if kind == "code":
            if meta:
                cap = doc.add_paragraph()
                cap.paragraph_format.space_before = Pt(5)
                cap.paragraph_format.space_after = Pt(1)
                cap.paragraph_format.left_indent = Cm(0.4)
                cr = cap.add_run(meta)
                set_run_font(cr, CODE_FONT, EA_FONT, 8)
                cr.italic = True
                cr.font.color.rgb = RGBColor(0x90, 0x90, 0x90)
            for idx, cline in enumerate(payload):
                p = doc.add_paragraph(style="CodeBlock")
                if idx == 0:
                    p.paragraph_format.space_before = Pt(3)
                if idx == len(payload) - 1:
                    p.paragraph_format.space_after = Pt(8)
                run = p.add_run(cline if cline.strip() else " ")
                set_run_font(run, CODE_FONT, EA_FONT, 9)
                shade_pPr(p._p.get_or_add_pPr(), "F7F7F7")
            continue
        if kind == "table":
            render_table(doc, payload)
            continue
        if kind == "quote":
            p = doc.add_paragraph(style="QuoteBlock")
            add_inline(p, payload)
            continue
        if kind == "hr":
            p = doc.add_paragraph()
            p.paragraph_format.space_before = Pt(2)
            p.paragraph_format.space_after = Pt(2)
            add_paragraph_border(p)
            continue


SETTINGS_AFTER_UPDATE_FIELDS = (
    "w:hdrShapeDefaults", "w:footnotePr", "w:endnotePr", "w:compat",
    "w:docVars", "w:rsids", "w:mathPr", "w:themeFontLang",
    "w:clrSchemeMapping", "w:doNotIncludeSubdocsInStats",
    "w:doNotAutoCompressPictures", "w:forceUpgrade", "w:captions",
    "w:readModeInkLockDown", "w:smartTagType", "w:schemaLibrary",
    "w:shapeDefaults", "w:doNotEmbedSmartTags", "w:decimalSymbol",
    "w:listSeparator",
)


def set_update_fields(doc):
    """在 settings.xml 中声明"打开文档时更新域"，让目录与页码即时可见。"""
    settings = doc.settings.element
    if settings.find(qn("w:updateFields")) is not None:
        return
    el = OxmlElement("w:updateFields")
    el.set(qn("w:val"), "true")
    settings.insert_element_before(el, *SETTINGS_AFTER_UPDATE_FIELDS)


def add_toc(doc):
    """目录页：标题 + TOC 域。\\o 使用大纲级别，\\h 生成超链接，\\z 隐藏页码编辑态。"""
    p = doc.add_paragraph()
    p.paragraph_format.space_after = Pt(10)
    run = p.add_run("目　录")
    set_run_font(run, LATIN_FONT, EA_FONT, 15)
    run.bold = True
    run.font.color.rgb = HEAD_COLOR[1]

    field_p = doc.add_paragraph()
    add_field(field_p, ' TOC \\o "1-3" \\h \\z \\u ',
              "（目录在此处自动生成，若未显示请按 Ctrl+A 后 F9）")
    return p


def main():
    src = sys.argv[1]
    dst = sys.argv[2]

    with open(src, "r", encoding="utf-8") as fh:
        lines = fh.read().splitlines()

    blocks = parse_blocks(lines)

    # 封面 = 第一个二级标题之前的全部内容；目录独占一页；其余为正文
    split_at = next(
        (i for i, b in enumerate(blocks) if b[0] == "head" and b[1] == 2),
        len(blocks),
    )
    cover = [b for b in blocks[:split_at] if b[0] != "hr"]
    body = blocks[split_at:]

    doc = Document()
    build_styles(doc)
    setup_section(doc)

    render(doc, cover, src)

    # 用"段前分页"而不是空段落分页，避免新页顶部多出一个空行
    add_toc(doc).paragraph_format.page_break_before = True

    body_start = len(doc.paragraphs)
    render(doc, body, src)
    if len(doc.paragraphs) > body_start:
        doc.paragraphs[body_start].paragraph_format.page_break_before = True

    set_update_fields(doc)
    doc.save(dst)
    print(f"[ok] 已生成: {dst}")


if __name__ == "__main__":
    main()
