
#ifdef HAVE_CONFIG_H
#  include <config.h>
#endif

#include "layout.hxx"

#include <algorithm>

// 中文化辅助在 FGPUIDialog.cxx 中实现（声明见 FGPUIDialog.hxx）。
// 这里只做前向声明，避免把 plib/puAux 的头文件带进纯布局代码。
std::string fgTranslateDialogString(const std::string& raw);

// 主题排版参数（textWrap/lineSpacing）；该头文件只依赖 osg/Vec4 与
// simgear props，不会引入 plib 依赖。
#include "fgui_theme.hxx"


// This file contains the actual layout engine.  It has no dependence
// on outside libraries; see layout-props.cxx for the glue code.

// Note, property names with leading double-underscores (__bx, etc...)
// are debugging information, and can be safely removed.

int LayoutWidget::UNIT = 5;

bool LayoutWidget::eq(const char* a, const char* b)
{
    while(*a && (*a == *b)) { a++; b++; }
    return *a == *b;
}

// Normal widgets get a padding of 4 pixels.  Layout groups shouldn't
// have visible padding by default, except for top-level dialog groups
// which need to leave two pixels for the puFrame's border.  This
// value can, of course, be overriden by the parent groups
// <default-padding> property, or per widget with <padding>.
int LayoutWidget::padding()
{
    int pad = (isType("group") || isType("frame")) ? 0 : 4;
    // As comments above note,  this was being set to 2.  For some
    // reason this causes the dialogs to shrink on subsequent pops
    // so for now we'll make "dialog" padding 0.
    if(isType("dialog")) pad = 0;
    if(hasParent() && parent().hasField("default-padding"))
        pad = parent().getNum("default-padding");
    if(hasField("padding"))
        pad = getNum("padding");
    return pad;
}

void LayoutWidget::calcPrefSize(int* w, int* h)
{
    *w = *h = 0; // Ask for nothing by default

    if (!getBool("enabled", true) || isType("nasal"))
        return;

    // 中文适配：测宽必须使用译文而不是 XML 英文原文，否则中文变宽后
    // 布局分配的首选尺寸偏小（译文更宽时会溢出）。纯空白串不翻译。
    int legw = stringLength(fgTranslateDialogString(getStr("legend")).c_str());
    int labw = stringLength(fgTranslateDialogString(getStr("label")).c_str());

    if(isType("dialog") || isType("group") || isType("frame")) {
        if(!hasField("layout")) {
            // Legacy support for groups without layout managers.
            if(hasField("width")) *w = getNum("width");
            if(hasField("height")) *h = getNum("height");
        } else {
            std::string layout = getStr("layout");
            if     (layout == "hbox")  doHVBox(false, false, w, h);
            else if(layout == "vbox")  doHVBox(false, true, w, h);
            else if(layout == "table") doTable(false, w, h);
        }
    } else if (isType("text")) {
        *w = labw;
        // FIXME: multi line height?
        // 中文适配：按自动换行后的行数估算高度，替代固定的 3*UNIT。
        // 可用宽度取控件自身的 <width>，其次取父容器已分配的宽度
        // （vbox/hbox 在 layout() 中先写父宽度、再算子控件首选尺寸）。
        // 行高 = 字号 × lineSpacing；保留最小 3*UNIT 防止单行变矮。
        int textHeight = 3 * UNIT;
        if (fguiTheme::enabled() && fguiTheme::metrics().textWrap) {
            const float lineSpacing = fguiTheme::metrics().lineSpacing;
            // 本文件刻意不包含 plib/pu.h（纯布局代码），puFont 在此只是前向
            // 声明，不能调用 getPointSize()。UNIT 由 setDefaultFont 按
            // pointsize/3 取整而来，故用 3*UNIT 反推字号（误差 <1px，
            // 仅用于高度估算，不影响最终布局精度）。
            const int lineHeight = int(3.0f * UNIT * lineSpacing + 0.5f);

            int availWidth = 0;
            // 注意：layout() 会为 text 写入 width=0（标签画在盒子右侧），
            // 所以不能把 width<=0 当作"有显式宽度"，必须回退到父容器宽度，
            // 否则重排（resize）后换行行数会被错误地重置为 1 行。
            if (hasField("width") && getNum("width") > 0) {
                availWidth = getNum("width");
            } else if (hasParent() && parent().hasField("width")) {
                availWidth = parent().getNum("width") - 2 * padding();
            }

            int lines = 1;
            if (availWidth > 0 && labw > availWidth)
                lines = (labw + availWidth - 1) / availWidth; // 向上取整

            textHeight = std::max(textHeight, lines * lineHeight);
        }
        *h = textHeight;
    } else if (isType("button")) {
        *w = legw + 6*UNIT + (labw ? labw + UNIT : 0);
        *h = 6*UNIT;
    } else if (isType("checkbox") || isType("radio")) {
        *w = 3*UNIT + (labw ? (3*UNIT + labw) : 0);
        *h = 3*UNIT;
    } else if (isType("input") || isType("combo") || isType("select")) {
        *w = 17*UNIT;
        *h = 6*UNIT;
    } else if (isType("slider")) {
        *w = *h = 17*UNIT;
        if(getBool("vertical")) *w = 4*UNIT;
        else                    *h = 4*UNIT;
    } else if (isType("list") || isType("airport-list")
            || isType("property-list") || isType("dial") || isType("waypointlist")) {
        *w = *h = 12*UNIT;
    } else if (isType("hrule")) {
        *h = 1;
    } else if (isType("vrule")) {
        *w = 1;
    }

    // Throw it all out if the user specified a fixed preference
    if(hasField("pref-width"))  *w = getNum("pref-width");
    if(hasField("pref-height")) *h = getNum("pref-height");

    // And finally correct for cell padding
    int pad = 2*padding();
    *w += pad;
    *h += pad;

    // Store what we calculated
    setNum("__pw", *w);
    setNum("__ph", *h);
}

// Set up geometry such that the widget lives "inside" the specified 
void LayoutWidget::layout(int x, int y, int w, int h)
{
    if (!getBool("enabled", true) || isType("nasal"))
        return;

    setNum("__bx", x);
    setNum("__by", y);
    setNum("__bw", w);
    setNum("__bh", h);

    // Correct for padding.
    int pad = padding();
    x += pad;
    y += pad;
    w -= 2*pad;
    h -= 2*pad;

    int prefw = 0, prefh = 0;
    calcPrefSize(&prefw, &prefh);
    prefw -= 2*pad;
    prefh -= 2*pad;

    // "Parent Set" values override widget preferences
    if(hasField("_psw")) prefw = getNum("_psw");
    if(hasField("_psh")) prefh = getNum("_psh");

    bool isGroup = isType("dialog") || isType("group") || isType("frame");

    // Correct our box for alignment.  The values above correspond to
    // a "fill" alignment.
    std::string halign = (isGroup || isType("hrule")) ? "fill" : "center";
    if(hasField("halign")) halign = getStr("halign");
    if(halign == "left") {
        w = prefw;
    } else if(halign == "right") {
        x += w - prefw;
        w = prefw;
    } else if(halign == "center") {
        x += (w - prefw)/2;
        w = prefw;
    }
    std::string valign = (isGroup || isType("vrule")) ? "fill" : "center";
    if(hasField("valign")) valign = getStr("valign");
    if(valign == "bottom") {
        h = prefh;
    } else if(valign == "top") {
        y += h - prefh;
        h = prefh;
    } else if(valign == "center") {
        y += (h - prefh)/2;
        h = prefh;
    }

    // PUI widgets interpret their size differently depending on
    // type, so diddle the values as needed to fit the widget into
    // the x/y/w/h box we have calculated.
    if (isType("text")) {
        // puText labels are layed out to the right of the box, so
        // zero the width. Also subtract PUSTR_RGAP from the x
        // coordinate to compensate for the added gap between the
        // label and its empty puObject.
        x -= 5;
        w = 0;
    } else if (isType("input") || isType("combo") || isType("select")) {
        // Fix the height to a constant
        y += (h - 6*UNIT) / 2;
        h = 6*UNIT;
    } else if (isType("checkbox") || isType("radio")) {
        // The PUI dimensions are of the check area only.  Center it
        // on the left side of our box.
        y += (h - 3*UNIT) / 2;
        w = h = 3*UNIT;
    } else if (isType("slider")) {
        // Fix the thickness to a constant
        if(getBool("vertical")) { x += (w-4*UNIT)/2; w = 4*UNIT; }
        else                    { y += (h-4*UNIT)/2; h = 4*UNIT; }
    }

    // Set out output geometry
    setNum("x", x);
    setNum("y", y);
    setNum("width", w);
    setNum("height", h);

    // Finally, if we are ourselves a layout object, do the actual layout.
    if(isGroup && hasField("layout")) {
        std::string layout = getStr("layout");
        if     (layout == "hbox")  doHVBox(true, false);
        else if(layout == "vbox")  doHVBox(true, true);
        else if(layout == "table") doTable(true);
    }
}

// Convention: the "A" cooridinate refers to the major axis of the
// container (width, for an hbox), "B" is minor.
void LayoutWidget::doHVBox(bool doLayout, bool vertical, int* w, int* h)
{
    int nc = nChildren();
    int* prefA = doLayout ? new int[nc] : 0;
    int i, totalA = 0, maxB = 0, nStretch = 0;
    int nEq = 0, eqA = 0, eqB = 0, eqTotalA = 0;
    for(i=0; i<nc; i++) {
        LayoutWidget child = getChild(i);
        if (!child.getBool("enabled", true))
            continue;

        int a, b;
        child.calcPrefSize(vertical ? &b : &a, vertical ? &a : &b);
        if(doLayout) prefA[i] = a;
        totalA += a;
        if(b > maxB) maxB = b;
        if(child.getBool("stretch")) {
            nStretch++;
        } else if(child.getBool("equal")) {
            int pad = child.padding();
            nEq++;
            eqTotalA += a - 2*pad;
            if(a-2*pad > eqA) eqA = a - 2*pad;
            if(b-2*pad > eqB) eqB = b - 2*pad;
        }
    }
    if(nStretch == 0) nStretch = nc;
    totalA += nEq * eqA - eqTotalA; 

    if(!doLayout) {
        if(vertical) { *w = maxB;   *h = totalA; }
        else         { *w = totalA; *h = maxB; }
        return;
    }

    int currA = 0;
    int availA = getNum(vertical ? "height" : "width");
    int availB = getNum(vertical ? "width" : "height");
    bool stretchAll = nStretch == nc ? true : false;
    int stretch = availA - totalA;
    if(stretch < 0) stretch = 0;
    for(i=0; i<nc; i++) {
        // Swap the child order for vertical boxes, so we lay out
        // from top to bottom instead of along the cartesian Y axis.
        int idx = vertical ? (nc-i-1) : i;
        LayoutWidget child = getChild(idx);
        if (!child.getBool("enabled", true))
            continue;

        if(child.getBool("equal")) {
            int pad = child.padding();
            prefA[idx] = eqA + 2*pad;
            // Use "parent set" values to communicate the setting to
            // the child.
            child.setNum(vertical ? "_psh" : "_psw", eqA);
            child.setNum(vertical ? "_psw" : "_psh", eqB);
        }
        if(stretchAll || child.getBool("stretch")) {
            int chunk = stretch / nStretch;
            stretch -= chunk;
            nStretch--;
            prefA[idx] += chunk;
            child.setNum("__stretch", chunk);
        }
        if(vertical) child.layout(    0, currA,   availB, prefA[idx]);
        else         child.layout(currA,     0, prefA[idx],   availB);
        currA += prefA[idx];
    }

    delete[] prefA;
}

struct TabCell {
    TabCell() {}
    LayoutWidget child;
    int w, h, row, col, rspan, cspan;
};

void LayoutWidget::doTable(bool doLayout, int* w, int* h)
{
    int i, j, nc = nChildren();
    TabCell* children = new TabCell[nc];
    
    // Pass 1: initialize bookeeping structures
    int rows = 0, cols = 0;
    for(i=0; i<nc; i++) {
        TabCell* cell = &children[i];
        cell->child = getChild(i);
        cell->child.calcPrefSize(&cell->w, &cell->h);
        cell->row = cell->child.getNum("row");
        cell->col = cell->child.getNum("col");
        cell->rspan = cell->child.hasField("rowspan") ? cell->child.getNum("rowspan") : 1;
        cell->cspan = cell->child.hasField("colspan") ? cell->child.getNum("colspan") : 1;
        if(cell->row + cell->rspan > rows) rows = cell->row + cell->rspan;
        if(cell->col + cell->cspan > cols) cols = cell->col + cell->cspan;
    }
    int* rowSizes = new int[rows];
    int* colSizes = new int[cols];
    for(i=0; i<rows; i++) rowSizes[i] = 0;
    for(i=0; i<cols; i++) colSizes[i] = 0;

    // Pass 1a (hack): we want row zero to be the top, not the
    // (cartesian: y==0) bottom, so reverse the sense of the row
    // numbers.
    for(i=0; i<nc; i++) {
        TabCell* cell = &children[i];
        cell->row = rows - cell->row - cell->rspan;
    }
    
    // Pass 2: get sizes for single-cell children
    for(i=0; i<nc; i++) {
        TabCell* cell = &children[i];
        if(cell->rspan < 2 && cell->h > rowSizes[cell->row])
            rowSizes[cell->row] = cell->h;
        if(cell->cspan < 2 && cell->w > colSizes[cell->col])
            colSizes[cell->col] = cell->w;
    }
    
    // Pass 3: multi-cell children, make space as needed
    for(i=0; i<nc; i++) {
        TabCell* cell = &children[i];
        if(cell->rspan > 1) {
            int total = 0;
            for(j=0; j<cell->rspan; j++)
                total += rowSizes[cell->row + j];
            int extra = cell->h - total;
            if(extra > 0) {
                for(j=0; j<cell->rspan; j++) {
                    int chunk = extra / (cell->rspan - j);
                    rowSizes[cell->row + j] += chunk;
                    extra -= chunk;
                }
            }
        }
        if(cell->cspan > 1) {
            int total = 0;
            for(j=0; j<cell->cspan; j++)
                total += colSizes[cell->col + j];
            int extra = cell->w - total;
            if(extra > 0) {
                for(j=0; j<cell->cspan; j++) {
                    int chunk = extra / (cell->cspan - j);
                    colSizes[cell->col + j] += chunk;
                    extra -= chunk;
                }
            }
        }
    }

    // Calculate our preferred sizes, and return if we aren't doing layout
    int prefw=0, prefh=0;
    for(i=0; i<cols; i++) prefw += colSizes[i];
    for(i=0; i<rows; i++) prefh += rowSizes[i];

    if(!doLayout) {
        *w = prefw; *h = prefh;
        delete[] children; delete[] rowSizes; delete[] colSizes;
        return;
    }

    // Allocate extra space
    int pad = 2*padding();
    int extra = getNum("height") - pad - prefh;
    for(i=0; i<rows; i++) {
        int chunk = extra / (rows - i);
        rowSizes[i] += chunk;
        extra -= chunk;
    }
    extra = getNum("width") - pad - prefw;
    for(i=0; i<cols; i++) {
        int chunk = extra / (cols - i);
        colSizes[i] += chunk;
        extra -= chunk;
    }

    // Finally, lay out the children (with just two more temporary
    // arrays for calculating coordinates)
    int* rowY = new int[rows];
    int total = 0;
    for(i=0; i<rows; i++) { rowY[i] = total; total += rowSizes[i]; }

    int* colX = new int[cols];
    total = 0;
    for(i=0; i<cols; i++) { colX[i] = total; total += colSizes[i]; }

    for(i=0; i<nc; i++) {
        TabCell* cell = &children[i];
        int w = 0, h = 0;
        for(j=0; j<cell->rspan; j++) h += rowSizes[cell->row + j];
        for(j=0; j<cell->cspan; j++) w += colSizes[cell->col + j];
        int x = colX[cell->col];
        int y = rowY[cell->row];
        cell->child.layout(x, y, w, h);
    }    

    // Clean up
    delete[] children;
    delete[] rowSizes;
    delete[] colSizes;
    delete[] rowY;
    delete[] colX;
}
