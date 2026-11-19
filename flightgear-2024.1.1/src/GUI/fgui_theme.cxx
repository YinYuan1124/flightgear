// fgui_theme.cxx - PUI/Canvas 统一主题配置管理器（ui_theme*.xml）的实现。
//
// 设计契约见 docs/pui-theme-design.md §2（XML schema）与 §3.1（接口）。
//
// 加载流程：
//   1. 读取 $FG_ROOT/gui/ui_theme_<mode>.xml（可选叠加基础文件 gui/ui_theme.xml）；
//   2. 解析 colors / metrics / font 三段到 ThemeColors / ThemeMetrics；
//   3. apply：颜色合并进 NewGUI::_colors、plib 全局调色板、主题字体、属性树镜像。
// 主题文件不存在时 enabled() 返回 false，全部路径保持 FlightGear 现有外观。

#include <config.h>

// 必须最先包含 FGFontCache.hxx 与 in-tree 的 3rdparty/fonts/fnt.h：
// 后面的 "FlightGear_pu.h" -> plib/pu.h 内部有 #include "fnt.h"，若先解析到
// vcpkg 旧版 fnt.h，就拿不到主题需要的 setLineSpacing/setOutline 等扩展接口。
// 靠着 fnt.h 的 _FNT_H_ 头文件保护，这里先包含后，plib/pu.h 不会再引入旧头。
#include "FGFontCache.hxx"
#include "fnt.h"

#include "fgui_theme.hxx"

#include <cmath>
#include <sstream>
#include <string>

#include <simgear/debug/logstream.hxx>
#include <simgear/misc/sg_path.hxx>
#include <simgear/props/props_io.hxx>
#include <simgear/structure/exception.hxx>

#include <Main/fg_props.hxx>
#include <Main/globals.hxx>

#include "FGColor.hxx"
#include "new_gui.hxx"

#if defined(HAVE_PUI)
// 提供 puSetDefaultColourScheme()/puPostRefresh()，并为 plib 类型提供声明。
#include "FlightGear_pu.h"
#endif

namespace fguiTheme
{
namespace
{
// ---------------------------------------------------------------------------
// 模块内部状态（单例语义，由 NewGUI 子系统在 GUI 线程驱动，无并发访问）
// ---------------------------------------------------------------------------

bool _initialized = false;          // init() 是否已执行过
bool _enabled = false;              // 是否成功加载到主题文件
std::string _effectiveMode = "day"; // 当前生效模式："day" 或 "night"
std::string _failedMode;            // 加载失败的模式，避免 0.5 秒一次的重复重试
ThemeColors _rawColors;             // 未预乘的原始颜色（镜像节点使用）
ThemeColors _colors;                // 实际绘制颜色（premultiplied 时已预乘）
ThemeMetrics _metrics;              // 当前排版/行为参数
std::string _fontName;              // 主题字体相对 Fonts 目录的路径，如 cn/simhei.ttf
SGPropertyNode_ptr _themeTree;      // 已合并的 <theme> 节点副本（镜像复制源）
SGPropertyNode_ptr _mirror;         // /sim/gui/theme 镜像节点
bool _needsFontRetry = false;       // 字体尚未就绪时置位，由 update() 重试
int _fontRetryCount = 0;            // 字体重试次数（限制日志与无效重试）
double _updateAccum = 0.0;          // update() 节流累加器（秒）

// ---------------------------------------------------------------------------
// 颜色键表：XML 键名 -> ThemeColors 成员，以及写入 NewGUI::_colors 时的别名。
//
// 别名策略（见设计文档 §3.4）：
//   * XML 键名原样写入 _colors（供主题控件/调试读取）；
//   * button-bg 额外写成 button-background —— FGPUIDialog::setColor 的查找键
//     是 <type>-<slot>（slot ∈ background/foreground/highlight/label/legend/
//     misc/editfield），按钮类型的背景槽正是 button-background；
//   * text 额外写成全局 label 与 legend，作为文字槽的兜底；
//   * 禁用文字色（text-disabled）没有对应的 plib 槽位，由主题控件的 draw()
//     直接读取 fguiTheme::colors().textDisabled，label/legend 键只做常态兜底。
// ---------------------------------------------------------------------------
struct ColorEntry
{
  const char* key;               // ui_theme.xml <colors> 下的键名
  osg::Vec4 ThemeColors::*member; // 对应的 ThemeColors 成员
  const char* alias1;            // 写入 NewGUI::_colors 的额外键（可为 nullptr）
  const char* alias2;            // 第二个额外键（可为 nullptr）
};

const ColorEntry colorEntries[] = {
  { "panel-bg",           &ThemeColors::panelBg,           nullptr,              nullptr },
  { "border",             &ThemeColors::border,            nullptr,              nullptr },
  { "border-hover",       &ThemeColors::borderHover,       nullptr,              nullptr },
  { "button-bg",          &ThemeColors::buttonBg,          "button-background",  nullptr },
  { "button-bg-hover",    &ThemeColors::buttonBgHover,     nullptr,              nullptr },
  { "button-bg-down",     &ThemeColors::buttonBgDown,      nullptr,              nullptr },
  { "button-bg-disabled", &ThemeColors::buttonBgDisabled,  nullptr,              nullptr },
  { "text",               &ThemeColors::text,              "label",              "legend" },
  { "text-disabled",      &ThemeColors::textDisabled,      nullptr,              nullptr },
  { "stroke-color",       &ThemeColors::strokeColor,       nullptr,              nullptr },
  { "backdrop-bg",        &ThemeColors::backdropBg,        nullptr,              nullptr },
  { "scroll-thumb",       &ThemeColors::scrollThumb,       nullptr,              nullptr },
  { "scroll-thumb-hover", &ThemeColors::scrollThumbHover,  nullptr,              nullptr },
  { "scroll-track",       &ThemeColors::scrollTrack,       nullptr,              nullptr },
  { "input-bg",           &ThemeColors::inputBg,           nullptr,              nullptr },
};

// 把分量限制在 [0, 1]，防止 XML 写出越界值。
float clamp01(float v)
{
  if (v < 0.0f) return 0.0f;
  if (v > 1.0f) return 1.0f;
  return v;
}

// 预乘 Alpha：rgb *= a，alpha 保持不变。
osg::Vec4 premultiply(const osg::Vec4& c)
{
  return osg::Vec4(c.r() * c.a(), c.g() * c.a(), c.b() * c.a(), c.a());
}

// 按 premultiplied 开关对整套颜色做（或跳过）预乘换算。
void premultiplyAll(ThemeColors& c)
{
  c.panelBg           = premultiply(c.panelBg);
  c.border            = premultiply(c.border);
  c.borderHover       = premultiply(c.borderHover);
  c.buttonBg          = premultiply(c.buttonBg);
  c.buttonBgHover     = premultiply(c.buttonBgHover);
  c.buttonBgDown      = premultiply(c.buttonBgDown);
  c.buttonBgDisabled  = premultiply(c.buttonBgDisabled);
  c.text              = premultiply(c.text);
  c.textDisabled      = premultiply(c.textDisabled);
  c.strokeColor       = premultiply(c.strokeColor);
  c.backdropBg        = premultiply(c.backdropBg);
  c.scrollThumb       = premultiply(c.scrollThumb);
  c.scrollThumbHover  = premultiply(c.scrollThumbHover);
  c.scrollTrack       = premultiply(c.scrollTrack);
  c.inputBg           = premultiply(c.inputBg);
}

// 设计文档 §2 示例中的日间主题默认值：XML 缺键时沿用这些值，保证不会出现全黑。
ThemeColors defaultRawColors()
{
  ThemeColors c;
  c.panelBg.set(0.125f, 0.141f, 0.157f, 0.88f);
  c.border.set(0.333f, 0.467f, 0.600f, 1.0f);
  c.borderHover.set(0.467f, 0.667f, 0.867f, 1.0f);
  c.buttonBg.set(0.267f, 0.298f, 0.329f, 1.0f);
  c.buttonBgHover.set(0.345f, 0.392f, 0.439f, 1.0f);
  c.buttonBgDown.set(0.188f, 0.212f, 0.231f, 1.0f);
  c.buttonBgDisabled.set(0.30f, 0.32f, 0.34f, 1.0f);
  c.text.set(1.0f, 1.0f, 1.0f, 1.0f);
  c.textDisabled.set(0.6f, 0.6f, 0.6f, 1.0f);
  c.strokeColor.set(0.0f, 0.0f, 0.0f, 0.85f);
  c.backdropBg.set(0.05f, 0.06f, 0.07f, 0.55f);
  c.scrollThumb.set(0.345f, 0.392f, 0.439f, 1.0f);
  c.scrollThumbHover.set(0.467f, 0.667f, 0.867f, 1.0f);
  c.scrollTrack.set(0.20f, 0.22f, 0.24f, 0.5f);
  c.inputBg.set(0.16f, 0.18f, 0.20f, 0.95f);
  return c;
}

// 解析一个颜色节点。优先支持 XML 约定的 "r g b a" 文本格式；
// 同时兼容 <red>/<green>/<blue>/<alpha> 子节点（与 FGColor 的格式一致）。
// 解析成功返回 true；节点为空或格式不符返回 false（调用方保留默认值）。
bool readColorValue(const SGPropertyNode* node, osg::Vec4& out)
{
  if (!node) {
    return false;
  }

  // 方案一：文本 "r g b a"（ui_theme.xml 的约定格式）。
  {
    std::istringstream iss(node->getStringValue());
    float r = 0.0f, g = 0.0f, b = 0.0f, a = out.a();
    if (iss >> r >> g >> b) {
      // 只写了 rgb 时沿用当前 alpha 默认值，避免半透明面板意外变不透明。
      if (!(iss >> a)) {
        a = out.a();
      }
      out.set(clamp01(r), clamp01(g), clamp01(b), clamp01(a));
      return true;
    }
  }

  // 方案二：<red>/<green>/<blue>/<alpha> 子节点形式。
  const SGPropertyNode* rn = node->getNode("red");
  const SGPropertyNode* gn = node->getNode("green");
  const SGPropertyNode* bn = node->getNode("blue");
  const SGPropertyNode* an = node->getNode("alpha");
  if (!rn && !gn && !bn && !an) {
    return false;
  }

  out.set(rn ? clamp01(rn->getFloatValue()) : out.r(),
          gn ? clamp01(gn->getFloatValue()) : out.g(),
          bn ? clamp01(bn->getFloatValue()) : out.b(),
          an ? clamp01(an->getFloatValue()) : out.a());
  return true;
}

// 读取数值型 metric；节点缺失时返回默认值。
float readMetricFloat(const SGPropertyNode* parent, const char* key, float def)
{
  const SGPropertyNode* n = parent ? parent->getNode(key) : nullptr;
  return n ? n->getFloatValue(def) : def;
}

// 读取布尔型 metric；节点缺失时返回默认值。
bool readMetricBool(const SGPropertyNode* parent, const char* key, bool def)
{
  const SGPropertyNode* n = parent ? parent->getNode(key) : nullptr;
  return n ? n->getBoolValue(def) : def;
}

// 读取 /sim/gui/theme-mode 并把它规范化为 auto/day/night。
std::string configuredMode()
{
  std::string m = fgGetString("/sim/gui/theme-mode", "auto");
  if (m.empty()) {
    m = "auto";
  }
  if (m != "auto" && m != "day" && m != "night") {
    SG_LOG(SG_GENERAL, SG_WARN,
           "fguiTheme: 无效的主题模式 '" << m << "'，回退为 auto");
    m = "auto";
  }
  return m;
}

// 把配置模式解析为实际生效模式。
//
// /sim/time/sun-angle-rad 是 acos(dot(本地天顶, 太阳方向)) 的无符号夹角：
//   0     = 太阳在天顶（正午，白天）
//   PI/2  = 太阳位于地平线
//   PI    = 太阳在正对面（午夜，夜晚）
// 先换算为带符号的太阳高度角 elevation = PI/2 - angle，
// 于是判据就是"白天 elevation > 0、夜晚 elevation <= 0"。
// 该属性由 FGLight 每 0.5 秒更新一次；节点尚不存在（启动早期）时按白天处理。
std::string resolveEffectiveMode(const std::string& cfg)
{
  if (cfg == "day" || cfg == "night") {
    return cfg;
  }

  // auto 模式：按太阳高度角判断日夜。
  const SGPropertyNode* sunAngle = globals->get_props()->getNode("/sim/time/sun-angle-rad");
  if (!sunAngle) {
    return "day";
  }

  const double halfPi = 1.57079632679489661923;
  const double elevation = halfPi - sunAngle->getDoubleValue();
  return (elevation > 0.0) ? "day" : "night";
}

// 读取主题 XML（基础文件 + 模式文件合并）并解析到内存结构。
// 返回 false 表示两份文件都不存在（主题未启用）。
bool loadTheme(const std::string& modeName)
{
  const SGPath fgRoot(globals->get_fg_root());
  SGPropertyNode_ptr temp = new SGPropertyNode;
  bool anyLoaded = false;

  // 可选基础文件 gui/ui_theme.xml：先读入，作为模式文件的公共默认值。
  const SGPath basePath(fgRoot, "gui/ui_theme.xml");
  if (basePath.exists()) {
    try {
      readProperties(basePath, temp);
      anyLoaded = true;
    } catch (const sg_exception& e) {
      SG_LOG(SG_GENERAL, SG_ALERT,
             "fguiTheme: 解析主题基础文件失败: " << basePath << " : " << e.getFormattedMessage());
    }
  }

  // 模式文件 gui/ui_theme_<day|night>.xml：覆盖基础文件中的同名键。
  const SGPath modePath(fgRoot, std::string("gui/ui_theme_") + modeName + ".xml");
  if (modePath.exists()) {
    try {
      readProperties(modePath, temp);
      anyLoaded = true;
    } catch (const sg_exception& e) {
      SG_LOG(SG_GENERAL, SG_ALERT,
             "fguiTheme: 解析主题文件失败: " << modePath << " : " << e.getFormattedMessage());
    }
  }

  if (!anyLoaded) {
    return false;
  }

  // 标准结构是 <PropertyList><theme>...</theme></PropertyList>；
  // 也兼容直接在根下写 <colors>/<metrics> 的简写。
  const SGPropertyNode* theme = temp->getNode("theme");
  if (!theme) {
    theme = temp.get();
  }

  // 保存一份副本，供 /sim/gui/theme 镜像节点复制（局部 temp 随后释放）。
  _themeTree = new SGPropertyNode;
  copyProperties(theme, _themeTree);

  // --- 解析颜色 -----------------------------------------------------------
  _rawColors = defaultRawColors();
  const SGPropertyNode* colorsNode = theme->getNode("colors");
  if (colorsNode) {
    for (const auto& entry : colorEntries) {
      const SGPropertyNode* colorNode = colorsNode->getNode(entry.key);
      osg::Vec4& target = _rawColors.*(entry.member);
      if (!readColorValue(colorNode, target) && colorNode) {
        SG_LOG(SG_GENERAL, SG_WARN,
               "fguiTheme: 主题颜色 '" << entry.key << "' 格式无效，使用默认值");
      }
    }
  }

  // --- 解析排版/行为参数 ---------------------------------------------------
  _metrics = ThemeMetrics();
  const SGPropertyNode* metricsNode = theme->getNode("metrics");
  if (metricsNode) {
    _metrics.cornerRadius      = readMetricFloat(metricsNode, "corner-radius", _metrics.cornerRadius);
    _metrics.strokeWidth       = readMetricFloat(metricsNode, "stroke-width", _metrics.strokeWidth);
    _metrics.lineSpacing       = readMetricFloat(metricsNode, "line-spacing", _metrics.lineSpacing);
    _metrics.menubarKeyPadding = readMetricFloat(metricsNode, "menubar-key-padding", _metrics.menubarKeyPadding);
    _metrics.scrollbarWidth    = readMetricFloat(metricsNode, "scrollbar-width", _metrics.scrollbarWidth);
    _metrics.textBackdrop      = readMetricBool(metricsNode, "text-backdrop", _metrics.textBackdrop);
    _metrics.textWrap          = readMetricBool(metricsNode, "text-wrap", _metrics.textWrap);
    _metrics.autoContrast      = readMetricBool(metricsNode, "auto-contrast", _metrics.autoContrast);
    _metrics.premultiplied     = readMetricBool(metricsNode, "premultiplied", _metrics.premultiplied);
    _metrics.stringCache       = readMetricBool(metricsNode, "string-cache", _metrics.stringCache);
    _metrics.frameGating       = readMetricBool(metricsNode, "frame-gating", _metrics.frameGating);
  }

  // --- 解析字体 -----------------------------------------------------------
  // <font><name> 相对 FG_ROOT/Fonts，例如 cn/simhei.ttf。
  _fontName = theme->getStringValue("font/name", "");

  return true;
}

// 把主题颜色合并进 NewGUI::_colors。
//
// 重要规则：写入 plib 色槽（_colors 以及 puSetDefaultColourScheme）的颜色一律使用
// alpha = 1.0 的不透明版本。原因：plib 的配色推导公式不公开，半透明只在 FG 自绘
// 控件（FGTheme*）的即时绘制路径中使用；若把半透明值塞进 plib 槽位，plib 会把
// alpha 参与内部混色，现有界面会出现不可预期的发灰/发黑。
void mergeColorsIntoNewGUI()
{
  NewGUI* gui = globals->get_subsystem<NewGUI>();
  if (!gui) {
    SG_LOG(SG_GENERAL, SG_WARN, "fguiTheme: NewGUI 子系统尚未就绪，跳过颜色合并");
    return;
  }

  for (const auto& entry : colorEntries) {
    const osg::Vec4& c = _rawColors.*(entry.member);
    gui->setThemeColor(entry.key, FGColor(c.r(), c.g(), c.b(), 1.0f));
    if (entry.alias1) {
      gui->setThemeColor(entry.alias1, FGColor(c.r(), c.g(), c.b(), 1.0f));
    }
    if (entry.alias2) {
      gui->setThemeColor(entry.alias2, FGColor(c.r(), c.g(), c.b(), 1.0f));
    }
  }
}

// 把主题字体参数（行距/描边/字符串缓存/图集尺寸）下发到 fnt 层。
//
// 只有 fntUnicodeFont（FreeType 的 .ttf/.otf 后端）支持这些扩展；.txf 纹理字体
// 返回空实现，此时只记日志，不影响颜色主题。
// 若 NewGUI::setupFont 尚未运行（字体未就绪）或字体加载失败，则只记录日志并
// 置 _needsFontRetry，等 update() 稍后重试，绝不崩溃。
bool applyThemeFont()
{
  if (_fontName.empty()) {
    _needsFontRetry = false;
    return true;
  }

  NewGUI* gui = globals->get_subsystem<NewGUI>();
  if (!gui || !gui->getDefaultFont()) {
    // 字体未就绪（NewGUI::setupFont 尚未运行）：只记日志并安排重试；
    // 重试同样限制次数，避免在没有 GUI 字体的构建配置下无限刷日志。
    _needsFontRetry = (_fontRetryCount < 5);
    _fontRetryCount += 1;
    SG_LOG(SG_GENERAL, SG_INFO,
           "fguiTheme: GUI 字体尚未就绪，主题字体 '" << _fontName
           << "' 稍后重试（第 " << _fontRetryCount << " 次）");
    return false;
  }

  fntFont* fnt = FGFontCache::instance()->loadFont(_fontName);
  if (!fnt) {
    _needsFontRetry = (_fontRetryCount < 5);
    _fontRetryCount += 1;
    SG_LOG(SG_GENERAL, SG_INFO,
           "fguiTheme: 主题字体加载失败: " << _fontName
           << "（第 " << _fontRetryCount << " 次尝试）");
    return false;
  }

  _needsFontRetry = false;
  _fontRetryCount = 0;

  fntUnicodeFont* unicodeFont = dynamic_cast<fntUnicodeFont*>(fnt);
  if (!unicodeFont) {
    SG_LOG(SG_GENERAL, SG_INFO,
           "fguiTheme: 主题字体不是 Unicode 字体，行距/描边参数不生效: " << _fontName);
    return true;
  }

  // 图集尺寸必须在首次光栅化前设置；主题字体使用较大的 2048 图集以容纳 CJK 字形。
  unicodeFont->setAtlasSize(2048);
  // 行距系数：换行步进 = pointsize × lineSpacing。
  unicodeFont->setLineSpacing(_metrics.lineSpacing);
  // 文字描边：宽度为 0 时等同于关闭；颜色使用未预乘的原始描边色。
  {
    const osg::Vec4& sc = _rawColors.strokeColor;
    unicodeFont->setOutline(_metrics.strokeWidth, sc.r(), sc.g(), sc.b(), sc.a());
  }
  // 字符串级显示列表缓存开关。
  unicodeFont->setStringCache(_metrics.stringCache);

  return true;
}

// 刷新 /sim/gui/theme 镜像节点（Canvas/Nasal 读取统一色板）。
// 这里复制的是未预乘的原始 XML 值；预乘换算由 Canvas 侧自行决定。
void refreshMirror()
{
  _mirror = globals->get_props()->getNode("sim/gui/theme", true);
  _mirror->removeAllChildren();
  if (_themeTree) {
    copyProperties(_themeTree, _mirror);
  }
}

// apply 主流程：预乘换算 -> 合并 NewGUI 颜色 -> plib 默认调色板 -> 字体 -> 镜像。
void applyTheme()
{
  _colors = _rawColors;
  if (_metrics.premultiplied) {
    premultiplyAll(_colors);
  }

  mergeColorsIntoNewGUI();

#if defined(HAVE_PUI)
  // plib 全局调色板使用面板背景的不透明版本（见 mergeColorsIntoNewGUI 的说明）。
  puSetDefaultColourScheme(_rawColors.panelBg.r(),
                           _rawColors.panelBg.g(),
                           _rawColors.panelBg.b(),
                           1.0f);
#endif

  applyThemeFont();
  refreshMirror();
}
} // anonymous namespace

void init()
{
  _initialized = true;
  _updateAccum = 0.0;
  _needsFontRetry = false;
  _fontRetryCount = 0;
  _failedMode.clear();

  // 先把结构体置为设计文档的默认值：即使主题文件缺失（enabled() == false），
  // colors()/metrics() 也返回可用数据，控件绘制不会读到未初始化的内存。
  _rawColors = defaultRawColors();
  _metrics = ThemeMetrics();
  _colors = _rawColors;
  if (_metrics.premultiplied) {
    premultiplyAll(_colors);
  }

  // 确保模式属性节点存在，默认 auto（设计文档 §2）。
  SGPropertyNode* modeNode = globals->get_props()->getNode("sim/gui/theme-mode", true);
  if (modeNode->getStringValue().empty()) {
    modeNode->setStringValue("auto");
  }

  _effectiveMode = resolveEffectiveMode(configuredMode());

  if (!loadTheme(_effectiveMode)) {
    _enabled = false;
    SG_LOG(SG_GENERAL, SG_INFO,
           "fguiTheme: 未找到 gui/ui_theme_" << _effectiveMode
           << ".xml，界面保持现有样式");
    return;
  }

  _enabled = true;
  applyTheme();

  SG_LOG(SG_GENERAL, SG_INFO,
         "fguiTheme: 已应用主题 ui_theme_" << _effectiveMode << ".xml");
}

// 预加载主题字体：在 NewGUI::setupFont 之前调用（见头文件声明注释）。
//
// 这里复制 init() 前段的模式解析：确保 theme-mode 属性存在、解析当前模式、
// loadTheme 读取配置；与 init() 的区别是不做 applyTheme（颜色/镜像），
// 只把字体文件装入 FGFontCache 的 _texFonts 表。loadFont 幂等，
// 重复调用只是命中缓存。
void preloadFont()
{
  SGPropertyNode* modeNode = globals->get_props()->getNode("sim/gui/theme-mode", true);
  if (modeNode->getStringValue().empty()) {
    modeNode->setStringValue("auto");
  }

  if (!loadTheme(resolveEffectiveMode(configuredMode()))) {
    // 主题文件不存在：字体按 initializeFonts 的根目录扫描即可，无需额外处理。
    return;
  }

  if (!_fontName.empty()) {
    // 装入 _texFonts 表；失败由 loadFont 内部 SG_WARN 报告，
    // 这里不再重复告警（init() 的 applyThemeFont 还会再试一次）。
    FGFontCache::instance()->loadFont(_fontName);
  }
}

void update(double dt)
{
  if (!_initialized) {
    return;
  }

  // 0.5 秒轮询一次即可：/sim/time/sun-angle-rad 本身也是 0.5 秒更新一次，
  // 同时避免每帧都做属性查找与字符串比较。
  _updateAccum += dt;
  if (_updateAccum < 0.5) {
    return;
  }
  _updateAccum = 0.0;

  // init 时 GUI 字体可能尚未就绪，这里按固定节奏重试。
  if (_needsFontRetry) {
    applyThemeFont();
  }

  if (!_enabled) {
    return;
  }

  // auto 模式下检测日夜；手动 day/night 则只在属性变化时重新应用。
  const std::string resolved = resolveEffectiveMode(configuredMode());
  if (resolved == _effectiveMode || resolved == _failedMode) {
    return;
  }

  if (!loadTheme(resolved)) {
    // 目标模式的主题文件缺失：记录失败模式并保持当前外观，
    // 避免每 0.5 秒重复读取文件与刷日志。
    _failedMode = resolved;
    SG_LOG(SG_GENERAL, SG_INFO,
           "fguiTheme: 切换到 " << resolved
           << " 模式时未找到主题文件，保持当前样式");
    return;
  }

  _effectiveMode = resolved;
  _failedMode.clear();
  applyTheme();

  // 运行时切换不重建对话框，只重刷颜色/字体并请求整层重绘。
  reapplyDialogs();
#if defined(HAVE_PUI)
  puPostRefresh();
#endif

  SG_LOG(SG_GENERAL, SG_INFO,
         "fguiTheme: 已切换到主题 ui_theme_" << _effectiveMode << ".xml");
}

const ThemeColors& colors()
{
  return _colors;
}

const ThemeMetrics& metrics()
{
  return _metrics;
}

bool enabled()
{
  return _enabled;
}

std::string mode()
{
  return _effectiveMode;
}

SGPropertyNode_ptr mirrorNode()
{
  return _mirror;
}

} // namespace fguiTheme
