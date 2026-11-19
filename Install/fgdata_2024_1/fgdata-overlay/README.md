# fgdata-overlay（随 FG_ROOT 发布的覆盖包）

中文本地化与 PUI/Canvas 统一主题相关的 fgdata（基础数据包）附加文件。

> **位置说明（2026-11-19 起）**：本目录已从工作区根目录移入
> `$FG_ROOT/fgdata-overlay/`（本工作区即 `Install/fgdata_2024_1/fgdata-overlay/`），
> 随 FG_ROOT 一起打包发布。本目录内的文件**同时已合并到上层 FG_ROOT 的对应运行时位置**
> （如 `Fonts/cn/simhei.ttf`、`gui/ui_theme_day.xml`），本目录是这些文件的版本化源头与记录，
> 修改时请两处同步（或改完这里再覆盖到对应路径）。
> 为保持版本控制，仓库根 `.gitignore` 对 `Install/` 的忽略规则中已为本目录加了例外。

## 文件清单（全量归纳）

```
fgdata-overlay/
├── README.md                          # 本说明
├── Fonts/
│   └── cn/
│       ├── simhei.ttf                 # 中文字体（Noto Sans SC Regular，17.7MB）
│       └── .dirindex                  # cn 子目录的 TerraSync 索引（sha1/大小登记）
├── Nasal/
│   └── canvas/
│       ├── theme.nas                  # uiTheme：读取 /sim/gui/theme 统一色板/字体（新增）
│       └── gui/
│           └── styles/
│               └── DefaultStyle.nas   # Canvas GUI 样式：控件字体 cn/simhei.ttf、字号 14→15
├── Translations/
│   ├── locale.xml                     # 在 zh_CN 语言项上增加 dialog-translations 指针
│   └── zh_CN/
│       └── dialog.xml                 # PUI 对话框静态字符串英→中对照表
└── gui/
    ├── ui_theme_day.xml               # 日间主题（新增，设计文档 §2 schema）
    ├── ui_theme_night.xml             # 夜间主题（同 schema 的暗色变体）
    ├── dialogs/                       # 中文加宽后的对话框覆盖副本（10 个，脚本 _resize_dialogs_cn.py 产出）
    │   ├── air.xml
    │   ├── chat_full.xml
    │   ├── doc-browser.xml
    │   ├── error-report.xml
    │   ├── joystick-info.xml
    │   ├── local_weather_config.xml
    │   ├── nasal-console.xml
    │   ├── radios.xml
    │   ├── random-failures.xml
    │   └── tutorial.xml
    └── styles/
        └── anthrax.xml                # 默认样式：中文字体 cn/simhei.ttf + 字号加大 + 深底浅字配色
```

## 与上层 FG_ROOT 运行时位置的对应关系

| overlay 源文件 | 运行时位置（上层 FG_ROOT） |
|---|---|
| `Fonts/cn/simhei.ttf` | `Fonts/cn/simhei.ttf` |
| `Fonts/cn/.dirindex` | `Fonts/cn/.dirindex` |
| `gui/ui_theme_day.xml` / `ui_theme_night.xml` | `gui/ui_theme_*.xml` |
| `gui/styles/anthrax.xml` | `gui/styles/anthrax.xml` |
| `gui/dialogs/*.xml`（10 个） | `gui/dialogs/*.xml` |
| `Nasal/canvas/theme.nas` | `Nasal/canvas/theme.nas` |
| `Nasal/canvas/gui/styles/DefaultStyle.nas` | `Nasal/canvas/gui/styles/DefaultStyle.nas` |
| `Translations/locale.xml` | `Translations/locale.xml` |
| `Translations/zh_CN/dialog.xml` | `Translations/zh_CN/dialog.xml` |

> 安装侧 `Fonts/.dirindex` 根索引不属于 overlay 交付内容（overlay 只有 cn 子目录索引，
> 根索引须是完整版）。根索引已按 TerraSync 规则更新：删除原根目录 `simhei.ttf` 记录，
> 改为 `d:cn:<cn/.dirindex 的 sha1>` 目录记录。

## 说明

- **字体打包**：中文字体统一放在 `Fonts/cn/simhei.ttf`（原根目录 `Fonts/simhei.ttf` 已删除）。
  样式、主题中的字体名一律写相对 `$FG_ROOT/Fonts` 的路径 `cn/simhei.ttf`；
  C++ 侧 `FGFontCache::loadFont()` 支持带子目录的字体名（`fguiTheme::preloadFont()` 在
  `NewGUI::setupFont` 之前显式加载，避免静默回退内置位图字体）。
- **主题配置**：`gui/ui_theme_day.xml`（日间）与 `gui/ui_theme_night.xml`（夜间）遵循设计文档
  `docs/pui-theme-design.md` §2 的键名契约，由 C++ 侧 `FGUIThemeManager` 读取，通过
  `/sim/gui/theme-mode`（auto/day/night）切换；auto 模式按太阳角度自动判定日夜。
  主题颜色为直通 alpha 的 `"R G B A"` 文本，加载时按 `metrics/premultiplied` 决定是否预乘。
- **Canvas 统一色板**：`Nasal/canvas/theme.nas` 提供全局 `uiTheme.getColor(name)`、
  `getMetric(name, def)`、`getFontName()`、`getFontSize()`，读取 C++ 侧镜像的
  `/sim/gui/theme/{colors,metrics,font}`，供 Canvas GUI 与 PUI 共用同一套色号和字体。
- **Canvas 样式**：`DefaultStyle.nas` 仅改 GUI 控件字体（统一 `cn/simhei.ttf`）与基准字号
  （character-size 14→15，与 PUI 对齐）；slider/dial/list 专用字号与仪表字体不动。
- **对话框加宽**：`gui/dialogs/` 下的 10 个覆盖副本由工作区根脚本
  `_resize_dialogs_cn.py` 生成（中文宽度估算 + `<slider>` 20→12），重跑前先看
  `_resize_dialogs_cn_report.txt` 的幂等说明；其余 115 个对话框无需改动。
- **对话框翻译机制**：`FGPUIDialog::setupObject` 经 `fgTranslateDialogString()` 按英文原文
  查 `Translations/zh_CN/dialog.xml`，无匹配回退英文；布局测宽也走同一翻译（避免测英文宽度偏小）。
- **可读性修复**：`anthrax.xml` 把 `fonts/gui` 的 size 加大到 20、`nasal-editor` 6→14，
  文字槽改为近白、input/slider/textbox 主题色改深灰蓝（plib 的 `load_colour_scheme` 对亮色主题
  会导出黑色 LEGEND 导致"黑字灰底"）。C++ 侧自动对比度调整受主题 `auto-contrast` 开关控制。
- **TerraSync 索引**：`Fonts/cn/.dirindex` 登记 `f:simhei.ttf:0a2195ed94416288d4c6ffc6cfcbed27b7043318:17772300`；
  目录记录的 sha1 是子目录 `.dirindex` 文件内容的 SHA1。改字体后需重算并同步根索引。
- **未改动**：`defaults.xml`（主题独立于 plib style 索引，避免 current-style 的 userarchive 迁移问题）、
  `Translations/zh_CN/dialog.xml` 之外的翻译资源。
