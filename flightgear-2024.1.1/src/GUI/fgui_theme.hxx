// fgui_theme.hxx - PUI/Canvas 统一主题配置管理器（ui_theme*.xml）。
//
// 本模块独立于 plib 的样式系统：样式 XML（gui/styles/*.xml）只有 7 个颜色槽，
// 无法表达圆角、描边、行距等排版参数；主题 XML（gui/ui_theme_day.xml /
// gui/ui_theme_night.xml）承载这些扩展配置，并作为 PUI 与 Canvas 共用的色板来源。
//
// 设计契约见 docs/pui-theme-design.md §3.1。

#ifndef FGUI_THEME_HXX
#define FGUI_THEME_HXX 1

#include <string>

#include <osg/Vec4>

#include <simgear/props/props.hxx>

namespace fguiTheme
{
  /**
   * 主题颜色集合。
   *
   * 成员保存的是"可直接绘制"的 RGBA：当 metrics().premultiplied 为 true 时，
   * 加载阶段已把 rgb 乘以 alpha（预乘 Alpha）；为 false 时保存直通 alpha 原值。
   * 注意：属性树镜像节点 /sim/gui/theme/colors/* 保存的始终是未预乘的原值，
   * 以便 Canvas/Nasal 侧按自己的约定换算。
   */
  struct ThemeColors
  {
    osg::Vec4 panelBg;          // 面板/对话框背景（panel-bg）
    osg::Vec4 border;           // 边框主色（border）
    osg::Vec4 borderHover;      // 边框 hover 提亮色（border-hover）
    osg::Vec4 buttonBg;         // 按钮常态背景（button-bg）
    osg::Vec4 buttonBgHover;    // 按钮 hover 背景（button-bg-hover）
    osg::Vec4 buttonBgDown;     // 按钮按下背景（button-bg-down）
    osg::Vec4 buttonBgDisabled; // 按钮禁用背景（button-bg-disabled）
    osg::Vec4 text;             // 正文文字色（text）
    osg::Vec4 textDisabled;     // 禁用文字色（text-disabled）
    osg::Vec4 strokeColor;      // 文字描边颜色（stroke-color）
    osg::Vec4 backdropBg;       // 文字底条背景（backdrop-bg，方案2）
    osg::Vec4 scrollThumb;      // 滚动条滑块（scroll-thumb）
    osg::Vec4 scrollThumbHover; // 滚动条滑块 hover（scroll-thumb-hover）
    osg::Vec4 scrollTrack;      // 滚动条轨道（scroll-track）
    osg::Vec4 inputBg;          // 输入框背景（input-bg）
  };

  /**
   * 主题排版/行为参数。
   * 各字段的默认值与设计文档 §2 的 XML 示例一致；XML 中缺省的键沿用默认值。
   */
  struct ThemeMetrics
  {
    float cornerRadius = 5;       // 圆角半径 px（corner-radius）
    float strokeWidth = 1;        // 文字描边宽度 px，0 = 关闭（stroke-width）
    float lineSpacing = 1.3f;     // 行距系数：换行步进 = pointsize × 该值（line-spacing）
    float menubarKeyPadding = 4;  // 菜单项快捷键提示前的空格数（menubar-key-padding）
    float scrollbarWidth = 12;    // 独立滑条/滚动条宽度 px（scrollbar-width）
    bool textBackdrop = false;    // 文字底条开关（text-backdrop）
    bool textWrap = true;         // 自动换行开关（text-wrap）
    bool autoContrast = false;    // FGPUIDialog::setColor 对比度 hack 开关（auto-contrast）
    bool premultiplied = true;    // 预乘 Alpha 合成开关（premultiplied）
    bool stringCache = true;      // 字符串级显示列表缓存开关（string-cache）
    bool frameGating = false;     // puNeedRefresh 整层跳帧门控（frame-gating）
  };

  /**
   * 初始化：在 NewGUI::setStyle() 末尾调用一次。
   *
   * 按当前模式（/sim/gui/theme-mode，默认 auto）读取
   * $FG_ROOT/gui/ui_theme_<mode>.xml（同时叠加可选的 ui_theme.xml 基础文件），
   * 解析颜色/排版/字体并应用到 NewGUI 颜色表、plib 全局调色板与主题字体。
   * 主题文件不存在时不做任何修改，enabled() 返回 false，界面保持现状。
   */
  void init();

  /**
   * 预加载主题字体（NewGUI::setStyle 中在 setupFont 之前调用）。
   *
   * 只解析主题配置并把字体文件装入 FGFontCache（_texFonts 表），使
   * setupFont 对带子目录的字体名（如 cn/simhei.ttf）能命中缓存，避免
   * 静默回退到内置位图字体导致中文不可读。颜色的完整应用仍由 init() 完成。
   */
  void preloadFont();

  /**
   * 每帧调用（由 NewGUI::update 转发）。
   *
   * auto 模式下节流检测日夜（/sim/time/sun-angle-rad），模式变化时重新加载主题
   * 并刷新已打开对话框；若字体在 init 时尚未就绪，也会在这里重试。
   */
  void update(double dt);

  /**
   * 重新给当前所有打开的对话框应用主题颜色/字体。
   *
   * 声明在本头文件，实现由控件层（fgui_style_widgets.cxx / FGPUIDialog.cxx）提供，
   * 避免主题模块反向依赖对话框实现。
   */
  void reapplyDialogs();

  /** 返回当前主题颜色（premultiplied 时已预乘）。 */
  const ThemeColors& colors();

  /** 返回当前主题排版/行为参数。 */
  const ThemeMetrics& metrics();

  /** 主题文件（ui_theme*.xml）是否成功加载。为 false 时 colors()/metrics() 仅为默认值。 */
  bool enabled();

  /**
   * 主题字体名（相对 $FG_ROOT/Fonts 的路径，如 "cn/simhei.ttf"）。
   * 主题未加载或未配置 <font><name> 时返回空串。
   * NewGUI::setupFont 用它强制使用 Unicode 字体，避免样式回退到位图字体后
   * 中文被按 Latin-1 渲染成乱码。
   */
  std::string fontName();

  /**
   * 当前生效的模式：init 之后为解析后的 "day" 或 "night"。
   * （/sim/gui/theme-mode 为 auto 时，这里返回自动检测得到的实际值。）
   */
  std::string mode();

  /**
   * 属性树镜像节点 /sim/gui/theme。
   *
   * 结构：colors/<XML 原键名>（未预乘的原始值）、metrics/*、font/name，
   * 供 Canvas/Nasal 读取统一色板，例如 getprop("/sim/gui/theme/colors/panel-bg")。
   */
  SGPropertyNode_ptr mirrorNode();
}

/**
 * hover 状态轮询（控件层实现，见 fgui_style_widgets.cxx）。
 *
 * x/y 为窗口像素坐标（PUI y 轴向下），由 NewGUI::update 从
 * /devices/status/mice/mouse/x、/y 读取后传入；命中控件与上一帧不同时，
 * 实现方负责切换 highlight()/lowlight() 并触发 puPostRefresh()。
 */
extern void fguiHoverUpdate(int x, int y);

#endif // FGUI_THEME_HXX
