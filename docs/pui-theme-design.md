# PUI 界面美化 + 中文适配补丁设计文档

本文档是「PUI 系统菜单美化」与「中文适配」补丁的**接口契约**：所有实现 agent 必须按本文的类名、函数签名、XML schema 与文件归属实现，不得自行改名。行号锚点来自 2026-11-19 的代码调研（工作区 `flightgear-2024.1.1`，commit 99915f9）。

## 1. 架构决策（调研结论）

| 需求（用户规格） | 落点 | 原因 |
|---|---|---|
| 一.2 行距 / 一.3 描边 / 三.1 自动换行 / 四.4 文本缓存 | `3rdparty/fonts/fntUnicode.cxx` + `fnt.h` 新增虚函数 | PUI 文字最终走 `fntFont::puts`；`puFont` 是 plib 值类型（pu.h:42-78），控件按值拷贝，扩展会被切片 |
| 二 控件四态/圆角/滚动条 | FG 侧子类覆写 `draw()`（新文件 `fgui_style_widgets.*`），替换 [FGPUIDialog.cxx:905-1123](flightgear-2024.1.1/src/GUI/FGPUIDialog.cxx#L905-L1123) 工厂中的 `new pu*` | PUI 是 vcpkg 预编译库（plib_pui.lib），源码不在工作区；`draw()` 是纯虚函数（pu.h:669），必须整体重绘 |
| 二 hover 状态 | `NewGUI::update` 每帧轮询鼠标属性 + 对话框树 `isHit` 命中测试（pu.h:593 内联 virtual） | `fgPopup::checkHit` 对 PU_DRAG 不进基类（FGPUIDialog.cxx:326-338），PUI 无 hover 通路；PUI 每帧全量重绘，`highlight()`/`lowlight()`（pu.h:736-738）立即生效 |
| 四.1 预乘 Alpha | `PUICamera.cxx:86` 改 `glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA)` + 主题色预乘 | 调研发现 FBO 内 alpha 被平方（A=a²）的既有缺陷；`glBlendFuncSeparate` 不在 Windows GL 1.1 头（opengl32.lib 只导出 1.1），单函数 GL_ONE 混合 + 预乘色即可达成严格预乘合成 |
| 四.2/四.3 渲染顺序/裁剪 | `PUICamera.cxx` quad bin 1001→1002、`glScissor`、`glDepthMask(GL_FALSE)` | PUI 已在 GUI 相机 PRE_RENDER；QML 同 bin 1001 后加入会盖在 PUI 上 |
| 六 ui_theme.xml | 新文件 `fgui_theme.{hxx,cxx}`（GUI 模块），独立于 plib 样式系统 | plib 样式 XML 只有 7 色槽，无法承载圆角/描边/行距；`<baseline-height>` 在全仓无消费者（死配置） |
| 五 Canvas 统一 | 色板经属性树 `/sim/gui/theme/colors/*` 镜像 + `FGCanvasSystemAdapter::getFont` 加主题键映射与缓存 + fgdata `DefaultStyle.nas` 字体收敛 | Canvas 走 osgText（与 PUI 完全不同字体栈），C++ 侧最小侵入 |

## 2. 主题配置 XML（ui_theme.xml）

位置：`$FG_ROOT/gui/ui_theme.xml`（基础，可选）、`ui_theme_day.xml`、`ui_theme_night.xml`。
运行时由 `FGUIThemeManager::load()` 读取；模式开关属性 `/sim/gui/theme-mode`（`auto`/`day`/`night`，默认 `auto`）。auto 模式按 `/sim/time/sun-angle-rad`（> 0 为白天）切换。
源码版与覆盖版文件：`fgdata-overlay/gui/ui_theme_day.xml`、`fgdata-overlay/gui/ui_theme_night.xml`（覆盖安装到 `Install/fgdata_2024_1/gui/`）。

```xml
<?xml version="1.0"?>
<!-- PUI/Canvas 统一主题配置（日间模式）。所有颜色为直通 alpha 的 RGBA，加载时预乘。 -->
<PropertyList>
  <theme>
    <name>ui-theme-day</name>          <!-- 主题标识，用于日志与调试 -->

    <colors>
      <!-- 面板/对话框背景：rgba(32,36,40,0.88)，半透明深灰 -->
      <panel-bg>0.125 0.141 0.157 0.88</panel-bg>
      <!-- 边框主色 #557799；hover 提亮 #77aadd -->
      <border>0.333 0.467 0.600 1.0</border>
      <border-hover>0.467 0.667 0.867 1.0</border-hover>
      <!-- 按钮四态：#444c54 / #586470 / #30363b / 低饱和灰 -->
      <button-bg>0.267 0.298 0.329 1.0</button-bg>
      <button-bg-hover>0.345 0.392 0.439 1.0</button-bg-hover>
      <button-bg-down>0.188 0.212 0.231 1.0</button-bg-down>
      <button-bg-disabled>0.30 0.32 0.34 1.0</button-bg-disabled>
      <!-- 文字：#ffffff / 禁用 #999999 -->
      <text>1.0 1.0 1.0 1.0</text>
      <text-disabled>0.6 0.6 0.6 1.0</text>
      <!-- 描边颜色（方案1）与文字底条（方案2） -->
      <stroke-color>0.0 0.0 0.0 0.85</stroke-color>
      <backdrop-bg>0.05 0.06 0.07 0.55</backdrop-bg>
      <!-- 滚动条 -->
      <scroll-thumb>0.345 0.392 0.439 1.0</scroll-thumb>
      <scroll-thumb-hover>0.467 0.667 0.867 1.0</scroll-thumb-hover>
      <scroll-track>0.20 0.22 0.24 0.5</scroll-track>
      <!-- 输入框 -->
      <input-bg>0.16 0.18 0.20 0.95</input-bg>
    </colors>

    <metrics>
      <corner-radius>5</corner-radius>          <!-- 圆角半径 px，4~6 -->
      <stroke-width>1.0</stroke-width>          <!-- 文字描边宽度 px，0 = 关闭，0.8~1 -->
      <line-spacing>1.3</line-spacing>          <!-- 行距系数：换行步进 = pointsize × 该值；1.0 = 现状 -->
      <text-backdrop>0</text-backdrop>          <!-- 方案2 文字底条开关（1=开） -->
      <text-wrap>1</text-wrap>                  <!-- 自动换行开关 -->
      <auto-contrast>0</auto-contrast>          <!-- FGPUIDialog::setColor 对比度 hack 开关 -->
      <premultiplied>1</premultiplied>          <!-- 预乘 Alpha 合成开关（0 = 退回直通 alpha） -->
      <string-cache>1</string-cache>            <!-- 字符串级显示列表缓存开关 -->
      <frame-gating>0</frame-gating>            <!-- puNeedRefresh 整层跳帧门控（默认关，见 §8） -->
      <menubar-key-padding>4</menubar-key-padding> <!-- 菜单项快捷键提示的空格填充数（原 11） -->
      <scrollbar-width>12</scrollbar-width>     <!-- 独立滑条/滚动条宽度 px（原 20） -->
    </metrics>

    <font>
      <name>cn/simhei.ttf</name>                <!-- 相对 FG_ROOT/Fonts 的路径 -->
      <size>20</size>                           <!-- 菜单栏/对话框默认字号 -->
    </font>
  </theme>
</PropertyList>
```

## 3. C++ 接口契约

### 3.1 FGUIThemeManager（新文件 `flightgear-2024.1.1/src/GUI/fgui_theme.{hxx,cxx}`）

```cpp
namespace fguiTheme
{
  // 预乘后的 RGBA（加载时按 premultiplied 开关换算；premultiplied=0 时原值存放）
  struct ThemeColors { osg::Vec4 panelBg, border, borderHover,
    buttonBg, buttonBgHover, buttonBgDown, buttonBgDisabled,
    text, textDisabled, strokeColor, backdropBg,
    scrollThumb, scrollThumbHover, scrollTrack, inputBg; };

  struct ThemeMetrics { float cornerRadius=5, strokeWidth=1, lineSpacing=1.3f,
    menubarKeyPadding=4, scrollbarWidth=12;
    bool textBackdrop=false, textWrap=true, autoContrast=false,
         premultiplied=true, stringCache=true, frameGating=false; };

  void init();                  // NewGUI::setStyle 末尾调用一次（读当前 mode 文件）
  void update(double dt);       // NewGUI::update 调用：auto 模式检测日夜并切换
  void reapplyDialogs();        // 运行时主题切换后重刷已打开对话框的颜色
  const ThemeColors& colors();  // 预乘后颜色
  const ThemeMetrics& metrics();
  bool enabled();               // ui_theme_*.xml 是否存在
  std::string mode();
  // 属性树镜像：/sim/gui/theme/colors/*（Canvas Nasal 读 getprop("/sim/gui/theme/colors/panel-bg")）
  SGPropertyNode_ptr mirrorNode();
}
```

加载实现：`globals->get_fg_root()/gui/ui_theme_<mode>.xml` 经 `readProperties` 读入属性树后解析到结构体；颜色预乘（若 premultiplied）；随后 apply：把 colors 合并进 `NewGUI::_colors`（键名 = XML 键名，使 FGPUIDialog::setColor 直接可见，见 §3.4）、调 `puSetDefaultColourScheme(panel-bg)`、对 theme 字体（经 FGFontCache 按 `fonts/gui` 节点取 fntFont）下发 setLineSpacing/setOutline/setStringCache/setAtlasSize、刷新镜像节点。主题不存在时 `enabled()==false`，全部路径回退现状。

### 3.2 fnt 层（`3rdparty/fonts/fnt.h` + 各后端）

**ABI 铁律**：新虚函数一律追加在 `hasGlyph`（fnt.h:66）之后、给出**非纯虚默认实现**；不得重排既有虚函数（与预编译 plib_pui.lib 的 vtable 槽位兼容）。所有被改文件若加中文注释必须补 UTF-8 BOM（fnt.cxx / fntBitmap.cxx / fntTXF.cxx / fntLocal.h 目前无 BOM；fnt.h 与 fntUnicode.cxx 已有）。

```cpp
// fnt.h 追加（hasGlyph 之后）：
virtual void  setLineSpacing ( float v ) { }                       // 行距系数；1.0=现状
virtual float getLineSpacing () const { return 1.0f ; }
virtual void  setOutline ( float widthPx, float r, float g, float b, float a ) { }  // 0=关
virtual void  setStringCache ( bool on ) { }                       // 字符串显示列表缓存
virtual void  setAtlasSize ( int px ) { }                          // 首次光栅化前调用生效
virtual std::string wrapString ( const char * s, float pointsize, float maxWidth ) ; // 默认原样返回
```

- **fntUnicode.cxx**（主实现）：
  - `puts`/`getBBox` 换行步进 × `lineSpacing`（现 fntUnicode.cxx:412 / :338，硬编码 pointsize / 1.0）。
  - 描边：`puts`/`putch` 中先以 `glGetFloatv(GL_CURRENT_COLOR)` 保存颜色，按 ±widthPx/pointsize 的归一化偏移（4 方向）以描边色画字形四边形，再以原色画主字形，结束恢复颜色。图集、mip 不动。
  - `wrapString`：逐码点累加 advance 断行；断点优先空格、CJK 任意断；`\n` 保留；结果插 `\n`。
  - 字符串缓存：`std::unordered_map<缓存键, GLuint>` 显示列表 + 字形生成代数计数。键 = (字符串, pointsize 量化, 颜色 8bit 量化, 描边开/关)。列表内录 `glBindTexture` + 白色顶点四边形；`glColor` 由列表内首条设置（显示列表顶点颜色编译期固化，键含颜色）。新字形光栅化使代数 +1 → 旧列表全部失效重建并 `glDeleteLists`。析构清理全部列表。`setStringCache(false)` 时直绘。
  - 图集：`setAtlasSize`（默认 1024；主题字体 2048）。图集满（现 :184 静默空白）改为：2048 以下时扩容重建（清空 glyphs 重光栅化），已 2048 时 SG_WARN 并占位。图集分配改为惰性（首个字形时才 `glGenTextures`）。
- **fnt.cxx（fntTexFont）**：`puts` 换行（:232）与 `getBBox` 换行（:168 魔数 1.333）统一改为 × `lineSpacing`，默认 1.0（修复 puts/getBBox 不一致）。
- **fntBitmap.cxx**：`puts`（:93）与 `getBBox`（:125）行距 = `height × lineSpacing`（像素单位），默认 1.0。

### 3.3 控件子类（新文件 `flightgear-2024.1.1/src/GUI/fgui_style_widgets.{hxx,cxx}`）

```cpp
// 全部继承 plib 类并覆写 draw()（纯虚）；构造函数签名与基类一致、直接委托。
class FGThemeFrame  : public puFrame  { ... draw(dx,dy) override; };  // 圆角面板 + 边框；hrule/vrule/对话框背景通用
class FGThemeButton : public puButton { ... draw(dx,dy) override; };  // 含 XCHECK/CIRCLE 标记自绘（getButtonType()）；工厂中 button/checkbox/radio/one-shot 全用它
class FGThemeInput  : public puInput  { ... draw(dx,dy) override; };  // 圆角输入框 + 选区 + I 光标（protected 成员 cursor_position/select_start/end 可直接用）
class FGThemeText   : public puText   { ... draw(dx,dy) override; };  // 多行 + 自动换行 + 底条（text-backdrop）
class FGThemeSlider : public puSlider { ... draw(dx,dy) override; };  // 细圆角滚动条：轨道弱化 + 滑块 hover（getSliderFraction()）
// 公共绘制辅助：
void fguiDrawRoundRect(float x1, float y1, float x2, float y2, float r);  // 立即模式三角扇
void fguiHoverUpdate(int x, int y);  // 声明；实现见 hover 段
```

- 状态判定：hover = `isHighlighted()`；按下 = `puActiveWidget() == this`；禁用 = `!isActive()`（plib 为 `greyOut()`）。
- 所有颜色取 `fguiTheme::colors()`（已预乘）+ 本控件 `getColour(PUCOL_*)`（plib 槽位，由 FGPUIDialog::setColor 写入的最终覆盖色）。文字经 `legendFont/labelFont` 的 `puts` 绘制；行距/描边由 fnt 层生效。
- 禁止持有需要析构的成员（`~puObject` 非虚，派生析构不会执行——沿用 FG 既有子类的做法）。
- 工厂替换（FGPUIDialog.cxx）：940/946（group/frame/hrule/vrule）、981（input）、987（text）、1001/1008（checkbox/radio）、1017/1019（button/one-shot）、1046（slider）、1255（对话框背景框）→ 对应 FGTheme* 类。dialog 外壳 926/928（fgPopup/puDialogBox）保持不动（它们没有自绘背景）。
- 复合控件（list/combo/select/loglist/textbox 内部 puaList/puaLargeInput 由 plib 创建，无法子类化）：颜色走既有 `setChildColourScheme`（pu.h:925）路径 + XML `<slider>` 宽度由数据扫改（§4）；本轮不重绘其内部滑块。
- **hover 实现**：`fguiHoverUpdate(x,y)` —— 从 `/devices/status/mice/mouse/x` 与 `/y` 读窗口像素坐标，换算成 PUI y 向下坐标（换算规则与 PUICamera.cxx:119-123 对齐，实现时核对 FGMouseInput.cxx:737 的写入约定）；遍历 `NewGUI` 打开对话框列表（复用其活动对话框访问器）→ `FGPUIDialog::_object` 树递归 `isHit(x,y)`（pu.h:593，public inline）→ 记录命中控件；与上一帧命中不同时：旧 `lowlight()`、新 `highlight()`、`puPostRefresh()`。对话框列表保证无悬垂指针（由 NewGUI 管理生命周期）。
- `FGPUIDialog::reapplyTheme()`：遍历控件树，对每个带 user_data(GUIInfo) 的对象按缓存 props 重跑 `setColor`（实现时把 props 指针存入 GUIInfo）；供运行时主题切换使用。
- **自动换行接入**（FGPUIDialog.cxx + layout.cxx）：
  - 新增 `fgTranslateDialogString(const std::string&)` 公共辅助（封装 trim 保留 + `translateDialogString`），`setupObject`（:1137-1151）与 `layout.cxx calcPrefSize`（:49-50）都改调它 —— 布局测宽前即用译文（修复测英文宽度偏小）。
  - FGThemeText::draw 内：`wrapString` 按 abox 宽断行 + 多行绘制；`calcPrefSize` text 分支（:63-65）行数 × 行高（`getBBox` + lineSpacing）替代固定 3×UNIT。
- FGPUIMenuBar.cxx：菜单项快捷键空格数（:199-206 写死 11）改读 `fguiTheme::metrics().menubarKeyPadding`。
- `setColor` 对比度 hack（FGPUIDialog.cxx:1315-1351）：受 `auto-contrast` 开关控制，`0` 时整体跳过。

### 3.4 NewGUI 集成（new_gui.cxx / new_gui.hxx）

- `setStyle()` 末尾（:604 后）调 `fguiTheme::init()`；`update()`（:253-260，现为空体）调 `fguiTheme::update(dt)` + `fguiHoverUpdate(x,y)`。
- 主题 apply 把颜色键合并进 `_colors`（`getColor("panel-bg")` 等键名 = XML 键名；`background`/`foreground`/`label`/`legend`/`misc`/`editfield` 六键仍由样式提供，主题额外提供 `<type>-<slot>` 键覆盖，例如 `button-bg` 同时作为 `button-background` 的别名写进 `_colors`，由 FGPUIDialog::setColor 的既有 `<type>-<slot>` 查找直接命中）。
- 运行时切换（theme-mode 变化）→ `fguiTheme::apply + reapplyDialogs + puPostRefresh`，不重建对话框。

### 3.5 渲染管线（PUICamera.cxx / hxx）

- :86 `glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA)` → 主题 `premultiplied` 时 `glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA)`（1.1 可用，配合预乘色修正 alpha 平方缺陷；`premultiplied=0` 时保持原函数）。
- :85 后补 `glDepthMask(GL_FALSE)`（绘制期禁深度写入）。
- :94 前补 `glEnable(GL_SCISSOR_TEST)` + `glScissor(0, 0, 窗口宽, 窗口高)`（用 puGetWindowSize 语义的 scaled 尺寸；pixelRatio 恒 1.0）。
- 合成 quad :325 RenderBin 1001 → 1002（压过 QML，PUI 绝对置顶）；quad StateSet 补 `osg::Depth(ALWAYS, 0, 1, false)`。
- `frame-gating=1` 时：`puNeedRefresh()`（pu.h:323）为假则跳过 `puDisplay()` 且跳过 clearMask（camera 设置动态调整），resize/主题切换强制 `puPostRefresh()`；默认 0（不启用，避免未知刷新路径遗漏）。
- 不改投影矩阵、不加 `glUseProgram`/`glBindVertexArray`（Windows GL 1.1 链接限制，OSG 状态由其自身管理）。

### 3.6 Canvas（FGCanvasSystemAdapter.cxx/hxx）

- `getFont(name)`：加 `std::map<std::string, osg::ref_ptr<osgText::Font>>` 缓存；`name == "ui-theme-gui"` 时改为解析 `/sim/gui/theme/font/name` 属性（缺省 `cn/simhei.ttf`）再走既有 `Fonts/ + name` 解析；其余名字行为不变（保护 7-Segment/BoeingCDU 等专用仪表字体）。

## 4. 数据面变更（fgdata / fgdata-overlay）

- **字体打包**：`fgdata-overlay/Fonts/cn/simhei.ttf`（由 `Fonts/simhei.ttf` 移动而来）；删除 overlay 与 Install 中的根目录副本；`gui/styles/anthrax.xml` 的 `fonts/gui/name` 改为 `cn/simhei.ttf`；`Fonts/.dirindex` 相应更新（cn/ 子目录记录）。
- **对话框扫改**（125 个 `gui/dialogs/*.xml`）：新增脚本 `_resize_dialogs_cn.py`（工作区根，与 `_gen_dialog_xml.py` 并列，中文注释）：
  1. 以 `Translations/zh_CN/dialog.xml` 为源，对每个 `<legend>/<label>` 估算中文宽度（CJK 字符 ≈ 1.0em、拉丁 ≈ 0.5em，按字号 20 换算），与 XML 现 `<width>` 比较；
  2. 不足时加宽该控件 `<width>`（保留已有布局，只增不减，加宽上限 1.6×）；
  3. 全部 `<list>`/`<loglist>`/`<textbox>` 的 `<slider>` 由 20 改 12；
  4. 产出直接写 `Install/fgdata_2024_1/gui/dialogs/` 并同步 `fgdata-overlay/gui/dialogs/`（保留可提交副本）；
  5. 输出改动统计（改了几个文件、最大加宽值），供人工复核。
- **Canvas 数据**：`fgdata-overlay/Nasal/canvas/gui/styles/DefaultStyle.nas`（自 Install 同名文件复制后修改）：GUI 控件字体字符串统一为 `cn/simhei.ttf`（仅 DefaultStyle 的 GUI 控件，不动仪表字体）；字号梯度与 PUI 对齐（character-size 14→15 基准档）。新增 `fgdata-overlay/Nasal/canvas/theme.nas`：`var uiTheme = { getColor: func(name) getprop("/sim/gui/theme/colors/" ~ name) ... }` 供 Canvas GUI 读统一色板。
- **不修改** defaults.xml（主题独立于 plib style 索引，避免 current-style userarchive 迁移问题）。

## 5. 文件归属表（实现阶段并行隔离）

| Agent | 独占写文件 | 只读 |
|---|---|---|
| theme-core | `src/GUI/fgui_theme.{hxx,cxx}`、`src/GUI/new_gui.{cxx,hxx}`、`src/GUI/CMakeLists.txt` | fgui_style_widgets.hxx（声明引用） |
| font-layer | `3rdparty/fonts/fnt.h`、`fntUnicode.cxx`、`fnt.cxx`、`fntBitmap.cxx`、`src/GUI/FGFontCache.{cxx,hxx}` | fgui_theme.hxx |
| widgets | `src/GUI/fgui_style_widgets.{hxx,cxx}`、`src/GUI/FGPUIDialog.{cxx,hxx}`、`src/GUI/layout.cxx`、`src/GUI/FGPUIMenuBar.cxx` | fgui_theme.hxx、fnt.h |
| pipeline | `src/Viewer/PUICamera.{cxx,hxx}` | fgui_theme.hxx |
| canvas | `src/Canvas/FGCanvasSystemAdapter.{cxx,hxx}` | — |
| data-dialogs | `Install/fgdata_2024_1/gui/dialogs/*.xml`、`fgdata-overlay/gui/dialogs/*.xml`、`_resize_dialogs_cn.py` | Translations |
| data-theme | `fgdata-overlay/gui/ui_theme*.xml`、`fgdata-overlay/Fonts/cn/*`、`fgdata-overlay/gui/styles/anthrax.xml`、`fgdata-overlay/Nasal/canvas/**`、`Install/fgdata_2024_1` 对应副本、`Fonts/.dirindex` | — |

## 6. 编译与验收

- 构建：`cmake --build Build/flightgear --target fgfs`（仅 FG 侧改动，SimGear 不重建）。
- 静态验收：新增/修改 .cxx/.hxx 含中文注释者必须 UTF-8 BOM；`fnt.h` 虚函数顺序未变；无跨 Agent 文件冲突。
- 运行验收（用户执行 `Install/LaunchFlightGear.bat`）：菜单中文无豆腐块、行距不重叠；按钮 hover/按下/禁用四态正确；面板半透明不发灰；对话框长文本自动换行不溢出；滚动条细圆角；夜视主题一键切换；Canvas 对话框与 PUI 同字体同色板。

## 7. 已知限制（本轮不做的点）

- plib 内部创建的复合控件（puaList 滑块、puPopupMenu 菜单项）外观仅颜色可配，宽度/圆角需 slider 数据与字号配合。
- `~puObject` 非虚 → 子类禁止持堆资源。
- 运行时主题切换对 puaLargeInput 等复合控件仅重刷颜色。
- `frame-gating` 默认关闭（PUI 刷新路径无法全量审计）。
- 夜视模式为深色主题，不做绿色单色滤镜（后续可按 /sim/rendering/ 滤镜扩展）。
