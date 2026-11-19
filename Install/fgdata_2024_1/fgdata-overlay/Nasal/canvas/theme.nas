# theme.nas - PUI/Canvas 统一主题读取接口（中文适配补丁新增）
#
# 背景：C++ 侧 FGUIThemeManager（src/GUI/fgui_theme.cxx）加载
#   $FG_ROOT/gui/ui_theme_<day|night>.xml 后，会把整个 <theme> 子树镜像到
#   属性树 /sim/gui/theme 下，结构为：
#     /sim/gui/theme/name                主题标识（ui-theme-day / ui-theme-night）
#     /sim/gui/theme/colors/<键名>       颜色，文本格式 "R G B A"（0..1 浮点，
#                                        直通 alpha，未预乘；Canvas 侧按需自行换算）
#     /sim/gui/theme/metrics/<键名>      排版/行为参数（corner-radius、line-spacing 等）
#     /sim/gui/theme/font/name           主题字体名（相对 $FG_ROOT/Fonts）
#     /sim/gui/theme/font/size           主题字号
#
# 本文件为 Canvas GUI（Nasal 脚本）提供统一的只读访问入口 uiTheme，
# 使 Canvas 对话框与 PUI 控件共用同一套色板与字体，避免两套界面风格割裂。
# 注意：本文件位于 Nasal/canvas/ 下，由 canvas 模块在启动时加载（defaults.xml 中
#   /nasal/canvas/enabled=true），因此 uiTheme 在 Canvas GUI 创建时已可使用。

var uiTheme = {

  # 读取主题颜色。
  # 参数 name：ui_theme*.xml 中 <colors> 下的键名，例如 "panel-bg"、"border"、
  #            "button-bg-hover"、"text"。
  # 返回：属性树中的字符串值（"R G B A"），主题未加载或键不存在时返回 nil，
  #       调用方应自行决定回退色（不要假设一定有值）。
  getColor: func(name)
  {
    return getprop("/sim/gui/theme/colors/" ~ name);
  },

  # 读取排版/行为参数（metrics）。
  # 参数 name：如 "corner-radius"、"line-spacing"、"scrollbar-width"。
  # 参数 def ：可选默认值；键不存在时返回 def（未传时为 nil）。
  # 注意：属性树中的值是按 XML 原样保存的字符串/数值，数值比较前建议
  #       num(...) 转换，例如 num(uiTheme.getMetric("corner-radius", 5))。
  getMetric: func(name, def = nil)
  {
    var v = getprop("/sim/gui/theme/metrics/" ~ name);
    if (v == nil)
      return def;
    return v;
  },

  # 读取主题字体名（相对 $FG_ROOT/Fonts）。
  # 主题未加载或字体键为空时回退 "cn/simhei.ttf"，保证 Canvas 控件始终有中文字体可用。
  getFontName: func()
  {
    var font = getprop("/sim/gui/theme/font/name");
    if (font == nil or font == "")
      return "cn/simhei.ttf";
    return font;
  },

  # 读取主题字号（pointsize）；未设置时回退 20（与 ui_theme*.xml 的 <font><size> 一致）。
  getFontSize: func()
  {
    var size = getprop("/sim/gui/theme/font/size");
    if (size == nil or size == "")
      return 20;
    return num(size);
  },

  # 读取当前主题标识（如 "ui-theme-day"），用于调试/日志；未加载时返回 nil。
  getName: func()
  {
    return getprop("/sim/gui/theme/name");
  }
};
