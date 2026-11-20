// fgui_style_widgets.cxx - 主题化 PUI 控件的绘制与 hover 轮询实现。
//
// 设计契约见 docs/pui-theme-design.md §3.3。所有颜色优先取 fguiTheme::colors()
// （主题加载时已按 <premultiplied> 做预乘），主题未启用时退回 plib 的
// PUCOL_* 槽位色，保证没有 ui_theme*.xml 的现有安装外观不变。

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include "fgui_style_widgets.hxx"

// FGPUIDialog.hxx 提供当前打开对话框的注册表（FGPUIDialog::openDialogs），
// hover 轮询与主题重刷都通过它遍历对话框树。
#include "FGPUIDialog.hxx"
#include "fgui_theme.hxx"

#include <algorithm>
#include <cmath>
#include <string>
#include <utility>
#include <vector>

#include <Main/fg_props.hxx>

// ---------------------------------------------------------------------------
// 模块内部辅助
// ---------------------------------------------------------------------------
namespace
{

// 圆角/圆弧的每角分段数：6 段在 5px 半径下已足够平滑，且顶点数很少。
const int ROUND_SEGMENTS = 6;

// 圆周率常量（避免依赖非标准的 M_PI）。
const float FGUI_PI = 3.14159265358979323846f;

// 读取 plib 颜色槽位（PUCOL_*）；槽值无效时使用 fallback。
// 槽位色是 setColor() 写入的"最终覆盖色"：主题启用时其中多数来自主题别名，
// 主题未启用时则来自样式 XML，从而保持原有配色行为。
osg::Vec4 fguiSlotColor(puObject* object, int which, const osg::Vec4& fallback)
{
    float r = fallback.r();
    float g = fallback.g();
    float b = fallback.b();
    float a = fallback.a();
    object->getColour(which, &r, &g, &b, &a);
    return osg::Vec4(r, g, b, a);
}

// 主题启用时返回主题色，否则返回 plib 槽位色（原外观回退）。
osg::Vec4 fguiThemedColor(puObject* object, int slot, const osg::Vec4& themeColor)
{
    if (fguiTheme::enabled())
        return themeColor;
    return fguiSlotColor(object, slot, themeColor);
}

// 控件文字的最终颜色：禁用时使用 text-disabled / 槽位色，常态使用 text。
osg::Vec4 fguiTextColor(puObject* object, int slot)
{
    if (!object->isActive()) {
        if (fguiTheme::enabled())
            return fguiTheme::colors().textDisabled;
        return fguiSlotColor(object, slot, osg::Vec4(0.6f, 0.6f, 0.6f, 1.0f));
    }
    if (fguiTheme::enabled())
        return fguiTheme::colors().text;
    return fguiSlotColor(object, slot, osg::Vec4(1.0f, 1.0f, 1.0f, 1.0f));
}

// 边框颜色：hover 时使用 border-hover 提亮；未启用主题时用 HIGHLIGHT 槽。
osg::Vec4 fguiBorderColor(puObject* object)
{
    if (fguiTheme::enabled())
        return object->isHighlighted() ? fguiTheme::colors().borderHover
                                       : fguiTheme::colors().border;
    return fguiSlotColor(object, PUCOL_HIGHLIGHT,
                         osg::Vec4(0.5f, 0.5f, 0.5f, 1.0f));
}

// 当前主题的圆角半径；未启用主题时不做圆角（保持 plib 的直角矩形）。
float fguiCornerRadius()
{
    return fguiTheme::enabled() ? fguiTheme::metrics().cornerRadius : 0.0f;
}

// 绘制前的 GL 状态准备：关闭纹理（这里只画纯色几何体），开启混合，
// 并按主题的 <premultiplied> 选择与 PUICamera 一致的混合函数。
void fguiPrepareSolid()
{
    glDisable(GL_TEXTURE_2D);
    glEnable(GL_BLEND);
    if (fguiTheme::enabled() && fguiTheme::metrics().premultiplied)
        glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA); // 预乘 Alpha 合成
    else
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA); // 直通 Alpha（原路径）
}

// 用当前颜色填充一个普通矩形（不圆角）。
void fguiFillRect(float x1, float y1, float x2, float y2)
{
    glDisable(GL_TEXTURE_2D);
    glBegin(GL_QUADS);
    glVertex2f(x1, y1);
    glVertex2f(x2, y1);
    glVertex2f(x2, y2);
    glVertex2f(x1, y2);
    glEnd();
}

// 用当前颜色填充一个圆角矩形。
void fguiFillRoundRect(float x1, float y1, float x2, float y2, float r)
{
    fguiDrawRoundRect(x1, y1, x2, y2, r);
}

// 填充圆形（单选框底/圆点）；以当前颜色绘制。
void fguiFillCircle(float cx, float cy, float r)
{
    glDisable(GL_TEXTURE_2D);
    glBegin(GL_TRIANGLE_FAN);
    glVertex2f(cx, cy);
    for (int i = 0; i <= ROUND_SEGMENTS * 4; ++i) {
        const float ang = 2.0f * FGUI_PI * float(i) / float(ROUND_SEGMENTS * 4);
        glVertex2f(cx + r * std::cos(ang), cy + r * std::sin(ang));
    }
    glEnd();
}

// 画圆形轮廓线（1px）；以当前颜色绘制。
void fguiDrawCircleOutline(float cx, float cy, float r)
{
    glDisable(GL_TEXTURE_2D);
    glLineWidth(1.0f);
    glBegin(GL_LINE_LOOP);
    for (int i = 0; i < ROUND_SEGMENTS * 4; ++i) {
        const float ang = 2.0f * FGUI_PI * float(i) / float(ROUND_SEGMENTS * 4);
        glVertex2f(cx + r * std::cos(ang), cy + r * std::sin(ang));
    }
    glEnd();
}

// 把字节位置收敛到 UTF-8 字符边界（修复审查发现的问题）：plib 的
// cursor_position/select_start/end 都按字节移动，CJK 字符占 3 字节，
// 光标可能落在码点中间；直接 substr 截断的 UTF-8 序列会让测宽与绘制
// 错乱（续字节以 0x80-0xBF 开头，据此回退到序列起点）。
static int fguiUtf8ClampPos(const std::string& s, int pos)
{
    pos = std::min<int>(std::max(pos, 0), int(s.size()));
    while (pos > 0 && ((unsigned char)s[pos] & 0xC0) == 0x80)
        --pos;
    return pos;
}

// 判断对象是否值得作为 hover 目标：只有可自绘 hover 状态的 FGTheme* 控件。
// FGThemeGroup 只是容器（由子控件各自响应），不在此列。
bool fguiIsHoverTarget(puObject* object)
{
    return dynamic_cast<FGThemeButton*>(object) != nullptr ||
           dynamic_cast<FGThemeOneShot*>(object) != nullptr ||
           dynamic_cast<FGThemeInput*>(object) != nullptr ||
           dynamic_cast<FGThemeSlider*>(object) != nullptr ||
           dynamic_cast<FGThemeFrame*>(object) != nullptr;
}

// 递归寻找 (x,y) 命中的最深控件。
//
// 只下钻两类分组：顶层对话框（puPopup/puDialogBox/puInterface，PUI 把
// 对话框子控件直接挂在它下面）与 FGThemeGroup（XML 的 group/frame）。
// 其他 puGroup（puaList/puaComboBox/ScrolledWaypointList 等复合控件的
// 内部容器）跳过——其子控件由 plib 创建，高亮它们没有意义。
// 命中仍使用 puObject::isHit()（pu.h 内联实现，自动排除隐藏/禁用对象）。
puObject* fguiFindHit(puObject* object, int x, int y)
{
    if (!object || !object->isVisible())
        return nullptr;

    // 坐标约定（修复审查发现的问题）：PUI 里每个对象的 abox 都是"父容器
    // 局部坐标"（plib 的 puGroup::checkHit 在递归子对象前先做
    // x -= abox.min[0]; y -= abox.min[1]，绘制时也是 dx + abox.min）。
    // 因此这里必须把传入坐标减去本对象 abox 原点，再传给子对象/自身
    // isHit 比较；否则窗口绝对坐标永远对不上局部 abox，hover 全面失效。
    const int lx = x - int(object->getABox()->min[0]);
    const int ly = y - int(object->getABox()->min[1]);

    puObject* best = nullptr;
    const int type = object->getType();

    if (type & PUCLASS_GROUP) {
        const bool isDialogRoot = (type & (PUCLASS_POPUP | PUCLASS_INTERFACE)) != 0;
        FGThemeGroup* themeGroup = dynamic_cast<FGThemeGroup*>(object);

        if (isDialogRoot || themeGroup) {
            if (themeGroup) {
                // FGThemeGroup 使用下标访问，避免依赖 dlist 的迭代方式。
                for (int i = 0; i < themeGroup->childCount(); ++i) {
                    puObject* hit = fguiFindHit(themeGroup->childAt(i), lx, ly);
                    if (hit)
                        best = hit;
                }
            } else {
                puGroup* group = static_cast<puGroup*>(object);
                for (puObject* child = group->getFirstChild(); child;
                     child = child->getNextObject()) {
                    puObject* hit = fguiFindHit(child, lx, ly);
                    if (hit)
                        best = hit;
                }
            }
        }
    }

    // 子控件优先（"最深命中"）；没有子控件命中时才考虑自身。
    // 注意（修复审查发现的问题）：isHit 与 plib 的 puGroup::checkHit 约定
    // 一致——用"父坐标系"的坐标与 abox.min 比较（父容器递归子控件前才
    // 减 abox.min）。因此自身命中用传入的 x/y，只有递归子控件时才用
    // 已减去本对象 abox 原点的 lx/ly。
    if (!best && fguiIsHoverTarget(object) && object->isHit(x, y))
        best = object;

    return best;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// FGThemeGroup
// ---------------------------------------------------------------------------
int FGThemeGroup::childCount() const
{
    return num_children;
}

puObject* FGThemeGroup::childAt(int index) const
{
    if (index < 0)
        return nullptr;

    puObject* child = dlist;
    while (child && index-- > 0)
        child = child->getNextObject();
    return child;
}

// ---------------------------------------------------------------------------
// 圆角矩形绘制
//
// PUI 的绘制坐标系 y 轴向上（abox.min[1] 为底边、max[1] 为顶边），
// draw(dx,dy) 的 dx/dy 是父容器偏移，绝对坐标 = dx/dy + abox 分量
// （与 WaypointList::draw 的用法一致）。
// ---------------------------------------------------------------------------
void fguiDrawRoundRect(float x1, float y1, float x2, float y2, float r)
{
    if (x2 < x1)
        std::swap(x1, x2);
    if (y2 < y1)
        std::swap(y1, y2);

    const float maxRadius = std::min((x2 - x1) * 0.5f, (y2 - y1) * 0.5f);
    if (r > maxRadius)
        r = maxRadius;
    if (r < 0.0f)
        r = 0.0f;

    glDisable(GL_TEXTURE_2D);

    if (r < 0.5f) {
        // 半径为 0（或主题未启用）：退化为普通矩形。
        glBegin(GL_QUADS);
        glVertex2f(x1, y1);
        glVertex2f(x2, y1);
        glVertex2f(x2, y2);
        glVertex2f(x1, y2);
        glEnd();
        return;
    }

    // 四个圆心的角度起点：左下 180°、右下 270°、右上 0°、左上 90°，
    // 每角扫过 90°；三角扇中心到相邻圆弧端点的三角形自然补全直边。
    const float cx[4] = { x1 + r, x2 - r, x2 - r, x1 + r };
    const float cy[4] = { y1 + r, y1 + r, y2 - r, y2 - r };
    const float startAngle[4] = { 180.0f, 270.0f, 0.0f, 90.0f };

    glBegin(GL_TRIANGLE_FAN);
    glVertex2f((x1 + x2) * 0.5f, (y1 + y2) * 0.5f);
    for (int corner = 0; corner < 4; ++corner) {
        for (int i = 0; i <= ROUND_SEGMENTS; ++i) {
            const float ang = (startAngle[corner] +
                               float(i) * 90.0f / float(ROUND_SEGMENTS)) *
                              FGUI_PI / 180.0f;
            glVertex2f(cx[corner] + r * std::cos(ang),
                       cy[corner] + r * std::sin(ang));
        }
    }
    // 闭合三角扇：补回左下角圆弧的起点（即 corner==0、i==0 那个顶点）。
    // 不补这一笔时，扇形缺少「扇心 → 左边框上端 → 左边框下端」这块三角形，
    // 表现为对话框左侧缺一大块斜三角、直接透出 3D 场景（2026-11-20 实测）。
    glVertex2f(x1, y1 + r);
    glEnd();
}

// ---------------------------------------------------------------------------
// FGThemeFrame
// ---------------------------------------------------------------------------
void FGThemeFrame::draw(int dx, int dy)
{
    // 主题未加载（ui_theme*.xml 不存在）时完全退回 plib 原绘制，
    // 保证"没有主题文件的安装外观与升级前一致"。
    if (!fguiTheme::enabled()) {
        puFrame::draw(dx, dy);
        return;
    }

    const float x1 = float(dx + abox.min[0]);
    const float y1 = float(dy + abox.min[1]);
    const float x2 = float(dx + abox.max[0]);
    const float y2 = float(dy + abox.max[1]);
    if (x2 <= x1 || y2 <= y1)
        return;

    fguiPrepareSolid();

    // hrule/vrule：布局把其中一条边压到 1px。分隔线必须尊重 XML <color>
    // （setColor 以 BACKGROUND 掩码写入），因此这里统一使用 BACKGROUND 槽位色。
    const bool thin = (x2 - x1 <= 2.0f) || (y2 - y1 <= 2.0f);
    if (thin) {
        const osg::Vec4 lineColor = fguiSlotColor(this, PUCOL_BACKGROUND,
                                                  osg::Vec4(0.5f, 0.5f, 0.5f, 1.0f));
        glColor4f(lineColor.r(), lineColor.g(), lineColor.b(), lineColor.a());
        fguiFillRect(x1, y1, x2, y2);
        return;
    }

    const osg::Vec4 fill = fguiThemedColor(this, PUCOL_BACKGROUND,
                                           fguiTheme::colors().panelBg);
    const osg::Vec4 border = fguiBorderColor(this);
    const float radius = fguiCornerRadius();

    // 先画外圈（边框色），再用填充色覆盖内部 1px，得到 1px 圆角边框。
    const float borderWidth = 1.0f;
    glColor4f(border.r(), border.g(), border.b(), border.a());
    fguiFillRoundRect(x1, y1, x2, y2, radius);

    if (x2 - x1 > 2.0f * borderWidth && y2 - y1 > 2.0f * borderWidth) {
        glColor4f(fill.r(), fill.g(), fill.b(), fill.a());
        fguiFillRoundRect(x1 + borderWidth, y1 + borderWidth,
                          x2 - borderWidth, y2 - borderWidth,
                          std::max(0.0f, radius - borderWidth));
    }
}

// ---------------------------------------------------------------------------
// 按钮（FGThemeButton / FGThemeOneShot 共用）
// ---------------------------------------------------------------------------
void fguiDrawButtonBody(puButton* button, int dx, int dy)
{
    puBox* box = button->getABox();
    if (!box || box->isEmpty())
        return;

    const float x1 = float(dx + box->min[0]);
    const float y1 = float(dy + box->min[1]);
    const float x2 = float(dx + box->max[0]);
    const float y2 = float(dy + box->max[1]);

    fguiPrepareSolid();

    // 四态背景：禁用 > 按下 > hover > 常态。
    osg::Vec4 background;
    if (fguiTheme::enabled()) {
        const fguiTheme::ThemeColors& tc = fguiTheme::colors();
        if (!button->isActive())
            background = tc.buttonBgDisabled;
        else if (puActiveWidget() == button)
            background = tc.buttonBgDown;
        else if (button->isHighlighted())
            background = tc.buttonBgHover;
        else
            background = tc.buttonBg;
    } else {
        // 主题未启用：按下用 HIGHLIGHT 槽，其余用 BACKGROUND 槽（plib 原语义）。
        background = (puActiveWidget() == button)
                         ? fguiSlotColor(button, PUCOL_HIGHLIGHT,
                                         osg::Vec4(0.4f, 0.4f, 0.4f, 1.0f))
                         : fguiSlotColor(button, PUCOL_BACKGROUND,
                                         osg::Vec4(0.3f, 0.3f, 0.3f, 1.0f));
    }

    const osg::Vec4 border = fguiBorderColor(button);
    const osg::Vec4 textColor = fguiTextColor(button, PUCOL_LEGEND);
    const float radius = fguiCornerRadius();

    const int buttonType = button->getButtonType();
    const bool isCheck = (buttonType == PUBUTTON_XCHECK ||
                          buttonType == PUBUTTON_VCHECK);
    const bool isRadio = (buttonType == PUBUTTON_CIRCLE ||
                          buttonType == PUBUTTON_RADIO);

    puFont legendFont = button->getLegendFont();
    puFont labelFont = button->getLabelFont();
    const float pointSize = legendFont.getPointSize();
    const float centerY = (y1 + y2) * 0.5f;
    // 基线位置：把文字视觉盒大致垂直居中（下行距离约 0.35 倍字号）。
    const int baseline = int(centerY - pointSize * 0.35f + 0.5f);

    if (isCheck || isRadio) {
        // 复选框/单选框：abox 就是标记区域本身，标记画满该区域。
        if (isRadio) {
            const float cx = (x1 + x2) * 0.5f;
            const float cy = (y1 + y2) * 0.5f;
            const float r = std::min(x2 - x1, y2 - y1) * 0.5f;

            glColor4f(background.r(), background.g(), background.b(), background.a());
            fguiFillCircle(cx, cy, r);
            glColor4f(border.r(), border.g(), border.b(), border.a());
            fguiDrawCircleOutline(cx, cy, r);

            if (button->getIntegerValue() != 0) {
                glColor4f(textColor.r(), textColor.g(), textColor.b(), textColor.a());
                fguiFillCircle(cx, cy, r * 0.5f);
            }
        } else {
            glColor4f(background.r(), background.g(), background.b(), background.a());
            fguiFillRoundRect(x1 + 0.5f, y1 + 0.5f, x2 - 0.5f, y2 - 0.5f, radius);

            glColor4f(border.r(), border.g(), border.b(), border.a());
            glLineWidth(1.0f);
            glBegin(GL_LINE_LOOP);
            glVertex2f(x1 + 0.5f, y1 + 0.5f);
            glVertex2f(x2 - 0.5f, y1 + 0.5f);
            glVertex2f(x2 - 0.5f, y2 - 0.5f);
            glVertex2f(x1 + 0.5f, y2 - 0.5f);
            glEnd();

            if (button->getIntegerValue() != 0) {
                glColor4f(textColor.r(), textColor.g(), textColor.b(), textColor.a());
                glLineWidth(2.0f);
                glBegin(GL_LINES);
                if (buttonType == PUBUTTON_VCHECK) {
                    glVertex2f(x1 + (x2 - x1) * 0.25f, y1 + (y2 - y1) * 0.55f);
                    glVertex2f(x1 + (x2 - x1) * 0.45f, y1 + (y2 - y1) * 0.30f);
                    glVertex2f(x1 + (x2 - x1) * 0.45f, y1 + (y2 - y1) * 0.30f);
                    glVertex2f(x1 + (x2 - x1) * 0.78f, y1 + (y2 - y1) * 0.75f);
                } else {
                    glVertex2f(x1 + (x2 - x1) * 0.28f, y1 + (y2 - y1) * 0.28f);
                    glVertex2f(x1 + (x2 - x1) * 0.72f, y1 + (y2 - y1) * 0.72f);
                    glVertex2f(x1 + (x2 - x1) * 0.28f, y1 + (y2 - y1) * 0.72f);
                    glVertex2f(x1 + (x2 - x1) * 0.72f, y1 + (y2 - y1) * 0.28f);
                }
                glEnd();
                glLineWidth(1.0f);
            }
        }

        // 复选框/单选框的 label 画在标记右侧（setupObject 统一设置为
        // PUPLACE_CENTERED_RIGHT 的语义）。
        const char* label = button->getLabel();
        if (label && label[0]) {
            glColor4f(textColor.r(), textColor.g(), textColor.b(), textColor.a());
            puFont font = labelFont;
            font.drawString(label, int(x2) + PUSTR_LGAP, baseline);
        }
        return;
    }

    // 普通按钮：圆角底 + 1px 边框 + 居中 legend。
    glColor4f(background.r(), background.g(), background.b(), background.a());
    fguiFillRoundRect(x1, y1, x2, y2, radius);

    glColor4f(border.r(), border.g(), border.b(), border.a());
    glLineWidth(1.0f);
    glBegin(GL_LINE_LOOP);
    glVertex2f(x1 + 0.5f, y1 + 0.5f);
    glVertex2f(x2 - 0.5f, y1 + 0.5f);
    glVertex2f(x2 - 0.5f, y2 - 0.5f);
    glVertex2f(x1 + 0.5f, y2 - 0.5f);
    glEnd();

    const char* legend = button->getLegend();
    if (legend && legend[0]) {
        const float textWidth = legendFont.getFloatStringWidth(legend);
        const int textX = int((x1 + x2) * 0.5f - textWidth * 0.5f + 0.5f);
        glColor4f(textColor.r(), textColor.g(), textColor.b(), textColor.a());
        puFont font = legendFont;
        font.drawString(legend, textX, baseline);
    }

    // 普通按钮若同时带 <label>，仍按 CENTERED_RIGHT 语义画在按钮右侧
    // （覆写 draw() 后原有标签绘制不会自动发生）。
    const char* label = button->getLabel();
    if (label && label[0]) {
        glColor4f(textColor.r(), textColor.g(), textColor.b(), textColor.a());
        puFont font = labelFont;
        font.drawString(label, int(x2) + PUSTR_LGAP, baseline);
    }
}

void FGThemeButton::draw(int dx, int dy)
{
    // 主题未加载时退回 plib 按钮绘制（含复选框/单选框标记的原有画法）。
    if (!fguiTheme::enabled()) {
        puButton::draw(dx, dy);
        return;
    }
    fguiDrawButtonBody(this, dx, dy);
}

void FGThemeOneShot::draw(int dx, int dy)
{
    // 主题未加载时退回 plib 按钮绘制（puOneShot 本身不覆写 draw）。
    if (!fguiTheme::enabled()) {
        puButton::draw(dx, dy);
        return;
    }
    fguiDrawButtonBody(this, dx, dy);
}

// ---------------------------------------------------------------------------
// FGThemeInput
// ---------------------------------------------------------------------------
void FGThemeInput::draw(int dx, int dy)
{
    // 主题未加载时退回 plib 输入框绘制（含选区/光标/标签的原有画法）。
    if (!fguiTheme::enabled()) {
        puInput::draw(dx, dy);
        return;
    }

    const float x1 = float(dx + abox.min[0]);
    const float y1 = float(dy + abox.min[1]);
    const float x2 = float(dx + abox.max[0]);
    const float y2 = float(dy + abox.max[1]);
    if (x2 <= x1 || y2 <= y1)
        return;

    fguiPrepareSolid();

    const osg::Vec4 fill = fguiThemedColor(this, PUCOL_EDITFIELD,
                                           fguiTheme::colors().inputBg);
    const osg::Vec4 border = fguiBorderColor(this);
    const osg::Vec4 textColor = fguiTextColor(this, PUCOL_LABEL);
    const float radius = fguiCornerRadius();

    glColor4f(border.r(), border.g(), border.b(), border.a());
    fguiFillRoundRect(x1, y1, x2, y2, radius);
    glColor4f(fill.r(), fill.g(), fill.b(), fill.a());
    fguiFillRoundRect(x1 + 1.0f, y1 + 1.0f, x2 - 1.0f, y2 - 1.0f,
                      std::max(0.0f, radius - 1.0f));

    const char* raw = getStringValue();
    const std::string text = raw ? raw : std::string();

    // 文本绘制原点与可用裁剪区域。
    const float textX = x1 + PUSTR_LGAP;
    const int baseline = int((y1 + y2) * 0.5f -
                             getLabelFont().getPointSize() * 0.35f + 0.5f);

    // PUI 没有裁剪能力，用 glScissor 把文字限制在输入框内（坐标与
    // WaypointList::draw 的用法一致：PUI 绘制坐标 y 轴向上，可直接传给
    // glScissor 的窗口坐标）。尺寸过小时跳过裁剪，避免传入非法负值。
    const int scissorW = int(x2 - x1) - 2;
    const int scissorH = int(y2 - y1) - 2;
    const bool scissorEnabled = (scissorW > 0 && scissorH > 0);
    if (scissorEnabled) {
        glEnable(GL_SCISSOR_TEST);
        glScissor(int(x1) + 1, int(y1) + 1, scissorW, scissorH);
    }

    puFont font = getLabelFont();

    // 水平滚动（修复审查发现的问题）：文本超出输入框时把绘制起点左移，
    // 使 I 光标始终保持在框内可见（plib 原 draw 也有同样的显示偏移逻辑）。
    // 偏移对选区/文字/光标三者统一生效，保证它们仍然对齐。
    const int cursor = fguiUtf8ClampPos(text, cursor_position);
    const float availTextW = std::max(0.0f, (x2 - x1) - 2.0f * PUSTR_LGAP - 4.0f);
    const float cursorFromStart =
        font.getFloatStringWidth(text.substr(0, cursor).c_str());
    float textOffset = 0.0f;
    if (cursorFromStart > availTextW)
        textOffset = -(cursorFromStart - availTextW);
    const float textXEff = textX + textOffset;

    // 选区（select_start_position <= select_end_position 时高亮）。
    if (select_start_position >= 0 &&
        select_end_position > select_start_position) {
        // 选区端点同样收敛到 UTF-8 字符边界（理由见 fguiUtf8ClampPos）。
        const int start = fguiUtf8ClampPos(text, select_start_position);
        const int end = fguiUtf8ClampPos(text, select_end_position);
        if (start < end) {
            const float sx1 = textXEff + font.getFloatStringWidth(
                                              text.substr(0, start).c_str());
            const float sx2 = textXEff + font.getFloatStringWidth(
                                              text.substr(0, end).c_str());
            osg::Vec4 selectColor = fguiThemedColor(this, PUCOL_HIGHLIGHT,
                                                    fguiTheme::colors().borderHover);
            // 选区需要半透明覆盖，避免完全遮住文字；预乘色按比例缩小 rgb。
            selectColor = osg::Vec4(selectColor.r() * 0.6f,
                                    selectColor.g() * 0.6f,
                                    selectColor.b() * 0.6f,
                                    selectColor.a() * 0.6f);
            glColor4f(selectColor.r(), selectColor.g(), selectColor.b(),
                      selectColor.a());
            fguiFillRect(sx1, y1 + 2.0f, sx2, y2 - 2.0f);
        }
    }

    // 文字本体。
    glColor4f(textColor.r(), textColor.g(), textColor.b(), textColor.a());
    font.drawString(text.c_str(), int(textXEff), baseline);

    // I 型光标（仅接受输入时显示）；颜色沿用 plib 的 PUCOL_MISC 槽。
    if (isAcceptingInput()) {
        const float cx = textXEff + font.getFloatStringWidth(
                                        text.substr(0, cursor).c_str());
        const osg::Vec4 cursorColor = fguiSlotColor(this, PUCOL_MISC,
                                                    osg::Vec4(0.1f, 0.1f, 1.0f, 1.0f));
        glColor4f(cursorColor.r(), cursorColor.g(), cursorColor.b(),
                  cursorColor.a());
        glDisable(GL_TEXTURE_2D);
        glLineWidth(1.0f);
        glBegin(GL_LINES);
        glVertex2f(cx, y1 + 3.0f);
        glVertex2f(cx, y2 - 3.0f);
        glEnd();
    }

    if (scissorEnabled)
        glDisable(GL_SCISSOR_TEST);

    // 标签画在输入框右侧：FGPUIDialog::setupObject() 对所有控件统一设置
    // PUPLACE_CENTERED_RIGHT，原 puInput::draw 也按此位置绘制 label。
    // 这里自行绘制，避免子类覆写 draw() 后丢失 XML <label>。
    const char* label = getLabel();
    if (label && label[0]) {
        puFont labelFont = getLabelFont();
        glColor4f(textColor.r(), textColor.g(), textColor.b(), textColor.a());
        labelFont.drawString(label, int(x2) + PUSTR_LGAP, baseline);
    }
}

// ---------------------------------------------------------------------------
// UTF-8 断行
// ---------------------------------------------------------------------------
std::vector<std::string> fguiWrapTextLines(const puFont& font,
                                           const std::string& text,
                                           float maxWidth)
{
    std::vector<std::string> lines;
    std::string line;

    // 快速路径：无显式换行且整体宽度未超限时直接单行返回，
    // 避免逐码点重复测宽（对话框每帧重绘，短标签占绝大多数）。
    if (text.find('\n') == std::string::npos) {
        if (maxWidth <= 0.0f ||
            font.getFloatStringWidth(text.c_str()) <= maxWidth) {
            lines.push_back(text);
            return lines;
        }
    }

    size_t i = 0;
    while (i <= text.size()) {
        // 段落结束或字符串结束：提交当前行。
        if (i == text.size() || text[i] == '\n') {
            lines.push_back(line);
            line.clear();
            if (i == text.size())
                break;
            ++i; // 跳过 '\n'
            continue;
        }

        // 取一个完整 UTF-8 码点（前导字节 + 若干 10xxxxxx 续字节）。
        size_t j = i + 1;
        while (j < text.size() &&
               (static_cast<unsigned char>(text[j]) & 0xC0) == 0x80)
            ++j;
        const size_t codePointLength = j - i;
        line.append(text, i, codePointLength);
        i = j;

        // 未超宽（或未启用换行）则继续。
        if (maxWidth <= 0.0f || font.getFloatStringWidth(line.c_str()) <= maxWidth)
            continue;

        // 超宽：优先在最后一个空格处断（保留单词完整性）。
        const size_t space = line.rfind(' ');
        if (space != std::string::npos && space > 0 &&
            space + 1 < line.size()) {
            lines.push_back(line.substr(0, space));
            line = line.substr(space + 1);
        } else {
            // 没有空格：按码点断（CJK 可任意断行），把刚加入的码点留到下一行。
            const std::string lastCodePoint = line.substr(line.size() - codePointLength);
            line.resize(line.size() - codePointLength);
            if (!line.empty())
                lines.push_back(line);
            line = lastCodePoint;
        }
    }

    return lines;
}

// ---------------------------------------------------------------------------
// FGThemeText
// ---------------------------------------------------------------------------
void FGThemeText::draw(int dx, int dy)
{
    // 主题未加载时退回 plib 文本绘制，保持原有单行标签外观与位置。
    if (!fguiTheme::enabled()) {
        puText::draw(dx, dy);
        return;
    }

    const char* raw = getLabel();
    if (!raw || !raw[0])
        return;

    const std::string text(raw);
    puFont font = getLabelFont();
    const float pointSize = font.getPointSize();

    const float x1 = float(dx + abox.min[0]);
    const float y1 = float(dy + abox.min[1]);
    const float x2 = float(dx + abox.max[0]);
    const float y2 = float(dy + abox.max[1]);

    // 可用换行宽度：优先使用 FGPUIDialog 传入的布局盒宽（__bw），其次才是
    // 控件自身 abox 宽度（预设尺寸的 text 才有宽度）。
    //
    // __bw 包含 LayoutWidget 的左右内边距（calcPrefSize 会加 2×padding），
    // 换行宽度必须减去它。内边距不再硬编码 8：FGPUIDialog 按控件的
    // <padding>/父容器 <default-padding> 规则计算后经 setWrapInset 传入
    //（例如标题栏 default-padding=1 时内边距只有 2，硬编码 8 会把单行
    // 标题误断成两行）。
    float wrapWidth = float(_wrapWidth);
    if (wrapWidth > 0.0f)
        wrapWidth -= float(_wrapInset);
    else
        wrapWidth = (x2 - x1) - 2.0f * PUSTR_LGAP;

    const bool wrap = fguiTheme::enabled() && fguiTheme::metrics().textWrap &&
                      wrapWidth > pointSize * 0.5f;

    std::vector<std::string> lines =
        fguiWrapTextLines(font, text, wrap ? wrapWidth : 0.0f);
    if (lines.empty())
        return;

    // 行距系数：换行步进 = 字号 × lineSpacing；主题未启用时保持原 1.0 行距。
    const float lineSpacing = fguiTheme::enabled()
                                  ? fguiTheme::metrics().lineSpacing
                                  : 1.0f;
    const float lineHeight = pointSize * lineSpacing;

    // 文本块整体在 abox 内垂直居中（puText 的 abox 通常是一条布局盒）。
    const float blockHeight = lineHeight * float(lines.size());
    const float centerY = (y1 + y2) * 0.5f;
    const float top = centerY + blockHeight * 0.5f;

    // puText 的标签语义：标签画在盒子右侧（layout.cxx 为 text 设置了
    // x -= 5 / width = 0），这里在左边缘补回 PUSTR_LGAP 的间距。
    const float textX = x1 + PUSTR_LGAP;

    fguiPrepareSolid();

    // 方案 2：文字底条（<text-backdrop>），在文字之前画一块半透明底。
    if (fguiTheme::enabled() && fguiTheme::metrics().textBackdrop) {
        float maxLineWidth = 0.0f;
        for (const auto& l : lines)
            maxLineWidth = std::max(maxLineWidth,
                                    font.getFloatStringWidth(l.c_str()));
        const osg::Vec4 backdrop = fguiTheme::colors().backdropBg;
        glColor4f(backdrop.r(), backdrop.g(), backdrop.b(), backdrop.a());
        fguiFillRect(textX - 2.0f, top - blockHeight,
                     textX + maxLineWidth + 2.0f, top);
    }

    const osg::Vec4 textColor = fguiTextColor(this, PUCOL_LABEL);
    glColor4f(textColor.r(), textColor.g(), textColor.b(), textColor.a());

    for (size_t i = 0; i < lines.size(); ++i) {
        // 基线：行槽顶部下移一个视觉字高（上升部约 0.8 倍字号），
        // 并把 (lineHeight - pointSize) 的额外行距平均分配到上下。
        const float lineTop = top - float(i) * lineHeight;
        const int baseline = int(lineTop - pointSize * 0.8f -
                                 (lineHeight - pointSize) * 0.5f + 0.5f);
        font.drawString(lines[i].c_str(), int(textX), baseline);
    }
}

// ---------------------------------------------------------------------------
// FGThemeSlider
// ---------------------------------------------------------------------------
void FGThemeSlider::draw(int dx, int dy)
{
    // 主题未加载时退回 plib 滑条绘制，保持原有轨道/滑块外观。
    if (!fguiTheme::enabled()) {
        puSlider::draw(dx, dy);
        return;
    }

    const float x1 = float(dx + abox.min[0]);
    const float y1 = float(dy + abox.min[1]);
    const float x2 = float(dx + abox.max[0]);
    const float y2 = float(dy + abox.max[1]);
    if (x2 <= x1 || y2 <= y1)
        return;

    fguiPrepareSolid();

    const bool vertical = isVertical() != 0;

    // 数值位置（0..1）；PUI 垂直滑条的最小值在底部（y1 一侧）。
    const float minValue = getMinValue();
    const float maxValue = getMaxValue();
    float fraction = 0.0f;
    if (maxValue != minValue)
        fraction = (getFloatValue() - minValue) / (maxValue - minValue);
    fraction = std::max(0.0f, std::min(1.0f, fraction));

    // 轨道弱化：横向取高度的 30%，纵向取宽度的 30%，限制在 4~8px。
    const float trackThickness = std::max(
        4.0f, std::min(8.0f, (vertical ? (x2 - x1) : (y2 - y1)) * 0.3f));

    osg::Vec4 trackColor = fguiThemedColor(this, PUCOL_FOREGROUND,
                                           fguiTheme::colors().scrollTrack);
    osg::Vec4 thumbColor;
    if (!isActive()) {
        thumbColor = fguiThemedColor(this, PUCOL_BACKGROUND,
                                     fguiTheme::colors().buttonBgDisabled);
    } else if (isHighlighted() &&
               fguiTheme::enabled()) {
        thumbColor = fguiTheme::colors().scrollThumbHover;
    } else {
        thumbColor = fguiThemedColor(this, PUCOL_BACKGROUND,
                                     fguiTheme::colors().scrollThumb);
    }

    // 滑块长度：slider_fraction 为占轨道长度的比例，至少 10px。
    const float trackLength = vertical ? (y2 - y1) : (x2 - x1);
    float thumbLength = slider_fraction * trackLength;
    thumbLength = std::max(10.0f, std::min(trackLength, thumbLength));

    glColor4f(trackColor.r(), trackColor.g(), trackColor.b(), trackColor.a());

    if (vertical) {
        const float cx = (x1 + x2) * 0.5f;
        fguiFillRoundRect(cx - trackThickness * 0.5f, y1,
                          cx + trackThickness * 0.5f, y2,
                          trackThickness * 0.5f);

        const float center = y1 + fraction * (y2 - y1);
        float thumbY1 = center - thumbLength * 0.5f;
        float thumbY2 = center + thumbLength * 0.5f;
        thumbY1 = std::max(y1, std::min(y2 - thumbLength, thumbY1));
        thumbY2 = thumbY1 + thumbLength;

        glColor4f(thumbColor.r(), thumbColor.g(), thumbColor.b(), thumbColor.a());
        const float r = std::min(fguiCornerRadius(), (x2 - x1) * 0.5f);
        fguiFillRoundRect(x1, thumbY1, x2, thumbY2, r);
    } else {
        const float cy = (y1 + y2) * 0.5f;
        fguiFillRoundRect(x1, cy - trackThickness * 0.5f,
                          x2, cy + trackThickness * 0.5f,
                          trackThickness * 0.5f);

        const float center = x1 + fraction * (x2 - x1);
        float thumbX1 = center - thumbLength * 0.5f;
        float thumbX2 = center + thumbLength * 0.5f;
        thumbX1 = std::max(x1, std::min(x2 - thumbLength, thumbX1));
        thumbX2 = thumbX1 + thumbLength;

        glColor4f(thumbColor.r(), thumbColor.g(), thumbColor.b(), thumbColor.a());
        const float r = std::min(fguiCornerRadius(), (y2 - y1) * 0.5f);
        fguiFillRoundRect(thumbX1, y1, thumbX2, y2, r);
    }

    // 标签画在滑条右侧（与 plib 的 CENTERED_RIGHT 语义一致）；
    // 覆写 draw() 后必须自行绘制，否则 XML <label> 会消失。
    const char* label = getLabel();
    if (label && label[0]) {
        puFont labelFont = getLabelFont();
        const osg::Vec4 textColor = fguiTextColor(this, PUCOL_LABEL);
        glColor4f(textColor.r(), textColor.g(), textColor.b(), textColor.a());
        labelFont.drawString(label, int(x2) + PUSTR_LGAP,
                             int((y1 + y2) * 0.5f -
                                 labelFont.getPointSize() * 0.35f + 0.5f));
    }
}

// ---------------------------------------------------------------------------
// hover 轮询
// ---------------------------------------------------------------------------
void fguiHoverUpdate(int x, int y)
{
    // 上一帧命中控件；静态保存以便比较。注意只保存指针用于比较，
    // 在触碰旧控件前必须先确认其所属对话框仍然存活（见下方注册表检查）。
    static FGPUIDialog* lastDialog = nullptr;
    static puObject* lastWidget = nullptr;

    // 对话框已被析构时，旧控件指针可能悬垂：只做指针比较，绝不访问。
    if (lastDialog &&
        FGPUIDialog::openDialogs().find(lastDialog) ==
            FGPUIDialog::openDialogs().end()) {
        lastDialog = nullptr;
        lastWidget = nullptr;
    }

    // 坐标换算（修复审查发现的问题）：
    // 1) /devices/status/mice/mouse/x、/y 是"窗口像素、y 轴向下"（来源
    //    FGMouseInput::doMouseMotion → FGEventHandler::eventToViewport）；
    // 2) PUI 内部是"左下原点、y 轴向上"（plib 的 puMouse 入口会做
    //    h - y 翻转）。本路径绕过 puMouse 直接调 isHit，必须自己做翻转：
    //    puiY = 窗口高 - 鼠标y；窗口高取 /sim/startup/ysize（与
    //    FGPUIDialog::display 计算屏幕尺寸的口径一致）；
    // 3) 两者都除以 gui-pixel-ratio 得到 PUI 逻辑坐标（与
    //    PUICamera::handle() 的 scaledX/scaledY 相同）。
    const double ratio = fgGetDouble("/sim/rendering/gui-pixel-ratio", 1.0);
    const int winH = fgGetInt("/sim/startup/ysize", 768);
    const int puiX = (ratio > 0.0001) ? int(double(x) / ratio) : x;
    const int puiY = (ratio > 0.0001) ? int(double(winH - y) / ratio)
                                      : (winH - y);

    FGPUIDialog* hitDialog = nullptr;
    puObject* hitWidget = nullptr;

    if (puiX >= 0 && puiY >= 0) {
        // 遍历当前打开的全部对话框（注册表由 FGPUIDialog 构造/析构维护，
        // 生命周期完全由 NewGUI 管理，遍历期间不会出现悬垂指针）。
        // 多个对话框重叠时，取 z 序最高（最后被点击/置顶）的命中，
        // 而不是注册表遍历顺序——std::set 按指针地址排序，与绘制顺序无关。
        for (FGPUIDialog* dialog : FGPUIDialog::openDialogs()) {
            puObject* hit = fguiFindHit(dialog->getObject(), puiX, puiY);
            if (hit && (!hitDialog || dialog->zOrder() > hitDialog->zOrder())) {
                hitDialog = dialog;
                hitWidget = hit;
            }
        }
    }

    if (hitWidget == lastWidget)
        return;

    // 旧控件仍在存活对话框中，才允许 lowlight()。
    if (lastWidget && lastDialog &&
        FGPUIDialog::openDialogs().find(lastDialog) !=
            FGPUIDialog::openDialogs().end()) {
        lastWidget->lowlight();
    }
    if (hitWidget)
        hitWidget->highlight();

    lastWidget = hitWidget;
    lastDialog = hitDialog;
    puPostRefresh();
}

// ---------------------------------------------------------------------------
// 主题切换后的对话框重刷（fgui_theme.hxx 声明、控件层实现）
// ---------------------------------------------------------------------------
namespace fguiTheme
{

void reapplyDialogs()
{
    // 对每个仍然打开的对话框重跑 FGPUIDialog::reapplyTheme()：
    // 按 GUIInfo 缓存的属性节点与颜色掩码重新执行 setColor。
    for (FGPUIDialog* dialog : FGPUIDialog::openDialogs())
        dialog->reapplyTheme();

    // 颜色变化后强制 PUI 全量重绘一次。
    puPostRefresh();
}

} // namespace fguiTheme
