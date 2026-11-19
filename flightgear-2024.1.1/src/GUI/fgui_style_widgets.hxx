// fgui_style_widgets.hxx - 主题化 PUI 控件子类与 hover 轮询接口。
//
// 背景：PUI（plib）只有预编译库，控件外观由库内 draw() 决定，无法通过样式
// XML 配置圆角、边框、hover/按下/禁用四态或自动换行。本文件声明一组薄子类：
// 构造函数与基类完全一致、直接委托，仅覆写纯虚的 draw() 重新绘制外观；
// 另外声明 hover 命中轮询入口 fguiHoverUpdate()。
//
// 设计契约见 docs/pui-theme-design.md §3.3。
//
// 注意：puObject 的析构函数不是 virtual（plib 既有缺陷），因此这些子类
// 禁止持有需要析构的成员（如 std::string / std::vector / 堆资源）；
// 需要的数据都放在基类的 protected 成员或外部静态结构中。

#ifndef FGUI_STYLE_WIDGETS_HXX
#define FGUI_STYLE_WIDGETS_HXX 1

#include "FlightGear_pu.h"

#include <string>
#include <vector>

/**
 * 主题化分组容器。
 *
 * puGroup 的子链表头 dlist 与子控件计数 num_children 都是 protected，
 * 外部遍历只能拿到 getFirstChild()/getNextObject()。hover 轮询需要按下标
 * 访问子控件（覆盖 plib 内部的 dlist 顺序），因此这里暴露 childCount()/
 * childAt() 两个只读访问器；绘制仍完全使用基类 puGroup::draw。
 */
class FGThemeGroup : public puGroup
{
public:
    FGThemeGroup(int x, int y) : puGroup(x, y) {}

    // 子控件数量（直接返回基类的 num_children）。
    int childCount() const;

    // 按下标取子控件；越界返回 nullptr（从基类 dlist 起顺序数 i 个）。
    puObject* childAt(int index) const;
};

/**
 * 圆角半透明面板。
 *
 * 用于 XML 的 <frame> 背景框、<hrule>/<vrule> 分隔线以及对话框背景框。
 * hrule/vrule 与普通面板的区分在 draw() 内通过"是否极细"判断：
 * 极细时只填色（使用 setColor 写入的 BACKGROUND 槽，XML <color> 生效），
 * 否则绘制圆角面板底 + 1px 边框（hover 时提亮）。
 */
class FGThemeFrame : public puFrame
{
public:
    FGThemeFrame(int minx, int miny, int maxx, int maxy)
        : puFrame(minx, miny, maxx, maxy) {}

    void draw(int dx, int dy) override;
};

/**
 * 主题化按钮（普通按钮与复选框/单选框共用）。
 *
 * 外观自绘：常态/hover/按下/禁用四态背景，圆角边框，文字居中。
 * 复选框（PUBUTTON_XCHECK/PUBUTTON_VCHECK）与单选框（PUBUTTON_CIRCLE）
 * 通过 getButtonType() 判断，自行绘制勾选/圆点标记，label 画在标记右侧。
 */
class FGThemeButton : public puButton
{
public:
    // 文字构造：尺寸由基类按 legend 字体测量（与 puButton 行为一致）。
    FGThemeButton(int minx, int miny, const char* l)
        : puButton(minx, miny, l) {}

    // 尺寸构造：复选框/单选框与预设尺寸按钮使用。
    FGThemeButton(int minx, int miny, int maxx, int maxy,
                  int btype = PUBUTTON_NORMAL)
        : puButton(minx, miny, maxx, maxy, btype) {}

    void draw(int dx, int dy) override;
};

/**
 * 主题化一次性按钮（按下即回调的 <button> 默认实现）。
 * 绘制委托给与 FGThemeButton 共用的 fguiDrawButtonBody()。
 */
class FGThemeOneShot : public puOneShot
{
public:
    FGThemeOneShot(int minx, int miny, const char* l)
        : puOneShot(minx, miny, l) {}

    FGThemeOneShot(int minx, int miny, int maxx, int maxy)
        : puOneShot(minx, miny, maxx, maxy) {}

    void draw(int dx, int dy) override;
};

/**
 * 主题化单行输入框。
 *
 * 绘制圆角输入框底、文字、选区（puInputBase 的 select_start_position /
 * select_end_position）与 I 型光标（cursor_position / PUCOL_MISC 槽色）。
 * 文字超出可视区域时用 glScissor 裁剪（PUI 自身没有裁剪 API）。
 */
class FGThemeInput : public puInput
{
public:
    FGThemeInput(int minx, int miny, int maxx, int maxy)
        : puInput(minx, miny, maxx, maxy) {}

    void draw(int dx, int dy) override;
};

/**
 * 主题化静态文本。
 *
 * 支持 '\n' 显式换行与按宽度自动换行（主题 <text-wrap> 开启时），
 * 可选绘制半透明文字底条（<text-backdrop>）。puText::isHit 恒为 false，
 * 因此文本控件不参与鼠标命中与 hover。
 *
 * 自动换行宽度来源：PUI 布局会把 text 的 abox 宽度置 0（标签画在盒子右侧），
 * 所以这里额外记录一个 _wrapWidth —— 由 FGPUIDialog 从布局写入的 __bw
 * （layout.cxx 分配的盒宽）或 XML <width> 传入。
 */
class FGThemeText : public puText
{
public:
    FGThemeText(int x, int y) : puText(x, y), _wrapWidth(0) {}

    void draw(int dx, int dy) override;

    // 设置自动换行可用宽度（像素）；<= 0 表示不换行。
    void setWrapWidth(int width) { _wrapWidth = width; }

    // 设置盒宽内的左右内边距之和（= 2 × LayoutWidget 的有效 padding）。
    // __bw 里包含这部分内边距（calcPrefSize 会加 2×padding），换行宽度
    // 必须把它减掉；padding 由 FGPUIDialog 按 <padding>/<default-padding>
    // 规则计算后传入。缺省 8 = 2 × 默认 padding 4，兼容未显式设置的路径。
    void setWrapInset(int inset) { _wrapInset = inset; }

private:
    int _wrapWidth;
    int _wrapInset = 8;
};

/**
 * 主题化滑条（水平/垂直）。
 *
 * 轨道弱化为细圆角条，滑块为圆角块并带 hover 高亮；位置由
 * getFloatValue() 相对 min/max 计算，滑块长度由 slider_fraction 决定。
 * 垂直方向遵循 PUI 约定：最小值在底部。
 */
class FGThemeSlider : public puSlider
{
public:
    FGThemeSlider(int minx, int miny, int sz, int vertical = FALSE)
        : puSlider(minx, miny, sz, vertical) {}

    void draw(int dx, int dy) override;
};

// ---------------------------------------------------------------------------
// 公共绘制辅助
// ---------------------------------------------------------------------------

/**
 * 以当前 GL 颜色画一个填充的圆角矩形（立即模式三角扇）。
 *
 * x1/y1 为左下角、x2/y2 为右上角（PUI 绘制坐标系 y 轴向上）；
 * r 为圆角半径像素，会自动限制为不超过短边的一半；r <= 0 时退化为矩形。
 * 调用方负责先设置 glColor4f 并关闭 GL_TEXTURE_2D。
 */
void fguiDrawRoundRect(float x1, float y1, float x2, float y2, float r);

/**
 * 按钮主体绘制（FGThemeButton 与 FGThemeOneShot 共用）。
 *
 * 依据 hover（isHighlighted()）、按下（puActiveWidget() == button）与禁用
 * （!isActive()）选择四态背景，再按 getButtonType() 自行绘制复选框 X 标记
 * 或单选框圆点，最后画 legend（居中）或 label（标记右侧）。
 * dx/dy 为父容器偏移，控件绝对坐标 = dx/dy + abox 分量。
 */
void fguiDrawButtonBody(puButton* button, int dx, int dy);

/**
 * 每帧 hover 命中轮询。
 *
 * x/y 为窗口像素坐标（y 轴向下），由 NewGUI::update 从
 * /devices/status/mice/mouse/x、/y 读取后传入；实现内部按
 * gui-pixel-ratio 换算到 PUI 逻辑坐标。命中控件与上一帧不同时，
 * 依次调用旧控件的 lowlight()、新控件的 highlight() 并 puPostRefresh()。
 */
void fguiHoverUpdate(int x, int y);

/**
 * 按最大宽度对 UTF-8 文本做自动断行。
 *
 * 返回断行后的各行（不含换行符）；保留原文中的 '\n'。断点优先空格，
 * 没有空格时按完整 UTF-8 码点断开（适合 CJK 任意断行），不会把
 * 多字节字符拆成半个。maxWidth <= 0 时只按 '\n' 分行。
 */
std::vector<std::string> fguiWrapTextLines(const puFont& font,
                                           const std::string& text,
                                           float maxWidth);

#endif // FGUI_STYLE_WIDGETS_HXX
