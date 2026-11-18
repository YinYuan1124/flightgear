# fgdata-overlay

中文本地化相关的 fgdata（基础数据包）附加文件。这些文件在构建/安装后被复制到 `$FG_ROOT`（本工作区为 `Install/fgdata_2024_1/`），使界面支持中文显示。

目录结构镜像 `$FG_ROOT` 的相对路径：

```
fgdata-overlay/
├── Fonts/
│   └── simhei.ttf                 # 中文字体（Noto Sans SC Regular，按 anthrax.xml 样式命名）
├── Translations/
│   ├── locale.xml                 # 在 zh_CN 语言项上增加 dialog-translations 指针
│   └── zh_CN/
│       └── dialog.xml             # PUI 对话框静态字符串英→中对照表
└── gui/
    └── styles/
        └── anthrax.xml            # 默认样式：中文字体 + 字号加大 + 深底浅字高对比配色
```

## 应用方式

把本目录下的文件覆盖到 `$FG_ROOT` 对应路径即可（`Fonts/simhei.ttf`、`Translations/locale.xml`、`Translations/zh_CN/dialog.xml`、`gui/styles/anthrax.xml`）。

## 说明

- 字体：GUI 默认样式 `anthrax.xml`（`/sim/gui/current-style=1`）已引用 `simhei.ttf`；`FGFontCache` 会扫描 `Fonts/` 下的 `.ttf`/`.otf` 并通过 FreeType 渲染 CJK 字形。
- 对话框本地化机制：`FGPUIDialog::setupObject` 读取 `<label>`/`<legend>` 后调用 `FGLocale::translateDialogString()`，按英文原文查找中文翻译；无匹配则回退英文。翻译表就是 `Translations/zh_CN/dialog.xml`。
- 可读性修复：`anthrax.xml` 把 `fonts/gui` 的 `size` 加大到 20（原缺省 15）、`nasal-editor` 由 6 改为 14，并把文字槽颜色改为近白、把 input/slider/textbox 主题色由亮橙改为深灰蓝（plib 的 `load_colour_scheme` 对亮色主题会导出黑色 LEGEND，导致"黑字灰底"）。配合 C++ 侧的自动对比度调整（`FGPUIDialog::setColor` 末尾）与 `gui.cxx`/`new_gui.cxx` 的默认配色修正。
- `Fonts/.dirindex` 是为 TerraSync/包完整性校验服务的目录索引，运行时字体加载不依赖它；如需保持一致，可把 `simhei.ttf` 追加一条 `f:<name>:<sha1>:<size>` 记录。
