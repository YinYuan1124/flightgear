// new_gui.cxx: implementation of XML-configurable GUI support.

/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <config.h>

#include "new_gui.hxx"

#include <algorithm>
#include <iostream>
#include <cstring>
#include <sys/types.h>

#include <simgear/compiler.h>
#include <simgear/debug/ErrorReportingCallback.hxx>
#include <simgear/misc/sg_dir.hxx>
#include <simgear/props/props_io.hxx>
#include <simgear/structure/exception.hxx>

#include <Add-ons/AddonManager.hxx>
#include <Main/fg_props.hxx>
#include <Main/sentryIntegration.hxx>
#include <Scripting/NasalSys.hxx>

#if defined(SG_UNIX) && !defined(SG_MAC) 
#include "GL/glx.h"
#endif


#if defined(SG_MAC)
#include "FGCocoaMenuBar.hxx"
#endif

#if defined(SG_WINDOWS)
#include "FGWindowsMenuBar.hxx"
#endif

// ensure we include this before puAux.h, so that
// #define _PU_H_ 1 has been done, and hence we don't
// include the un-modified system pu.h
#include "FlightGear_pu.h"

#include <plib/puAux.h>


#include "FGPUIDialog.hxx"
#include "FGPUIMenuBar.hxx"

#include "FGFontCache.hxx"
#include "FGColor.hxx"

// PUI/Canvas 统一主题管理器（ui_theme*.xml）：setStyle 末尾加载，update 中做
// auto 日夜切换检测；同时声明 fguiHoverUpdate() 供每帧轮询鼠标 hover 使用。
#include "fgui_theme.hxx"

#include "Highlight.hxx"

// ignore the word Navaid here, it's a DataCache
#include <Navaids/NavDataCache.hxx>

using std::map;
using std::string;

extern void puCleanUpJunk(void);

////////////////////////////////////////////////////////////////////////
// Implementation of NewGUI.
////////////////////////////////////////////////////////////////////////


NewGUI::NewGUI()
    // 显式初始化为空指针：主题/字体逻辑需要据此判断 NewGUI::setupFont 是否已经
    // 执行（未执行时不能把未初始化的指针当作有效字体使用）。
    : _font(nullptr)
{
}

NewGUI::~NewGUI ()
{
    for (_itt_t it = _colors.begin(); it != _colors.end(); ++it)
        delete it->second;
}

// Recursively finds all nodes called <leaf> and appends the string values of
// these nodes to <out>.
//
static void findAllLeafValues(SGPropertyNode* node, const std::string& leaf, std::vector<std::string>& out)
{
    string name = node->getNameString();
    if (name == leaf) {
        out.push_back(node->getStringValue());
    }
    for (int i=0; i<node->nChildren(); ++i) {
        findAllLeafValues(node->getChild(i), leaf, out);
    }
}


/* Registers menu-dialog associations with Highlight::add_menu_dialog(). */
static void scanMenus()
{
    /* We find all nodes called 'dialog-name' in
    sim/menubar/default/menu[]/item[]. */
    SGPropertyNode* menubar = globals->get_props()->getNode("sim/menubar/default");
    assert(menubar);
    auto highlight = globals->get_subsystem<Highlight>();
    if (!highlight) {
        return;
    }
    for (int menu_p=0; menu_p<menubar->nChildren(); ++menu_p) {
        SGPropertyNode* menu = menubar->getChild(menu_p);
        if (menu->getNameString() != "menu") continue;
        for (int item_p=0; item_p<menu->nChildren(); ++item_p) {
            SGPropertyNode* item = menu->getChild(item_p);
            if (item->getNameString() != "item") continue;
            std::vector<std::string> dialog_names;
            findAllLeafValues(item, "dialog-name", dialog_names);
            for (auto dialog_name: dialog_names) {
                highlight->addMenuDialog(HighlightMenu(menu->getIndex(), item->getIndex()), dialog_name);
            }
        }
    }
}

void
NewGUI::init ()
{
    createMenuBarImplementation();
    fgTie("/sim/menubar/visibility", this,
          &NewGUI::getMenuBarVisible, &NewGUI::setMenuBarVisible);
    
    fgTie("/sim/menubar/overlap-hide", this,
          &NewGUI::getMenuBarOverlapHide, &NewGUI::setMenuBarOverlapHide);

    setStyle();
    SGPath p(globals->get_fg_root(), "gui/dialogs");
    readDir(p);
    
    if (fgGetBool("/sim/gui/startup") == false) {
        SGPath aircraftDialogDir(fgGetString("/sim/aircraft-dir"), "gui/dialogs");
        if (aircraftDialogDir.exists()) {
            readDir(aircraftDialogDir);
        }

        // Read XML dialogs made available by registered add-ons
        const auto& addonManager = flightgear::addons::AddonManager::instance();
        for (const auto& addon: addonManager->registeredAddons()) {
            SGPath addonDialogDir = addon->getBasePath() / "gui/dialogs";

            if (addonDialogDir.exists()) {
                readDir(addonDialogDir);
            }
        }
    }

    // Fix for http://code.google.com/p/flightgear-bugs/issues/detail?id=947
    fgGetNode("sim/menubar")->setAttribute(SGPropertyNode::PRESERVE, true);
    _menubar->init();
    scanMenus();
}

void
NewGUI::shutdown()
{
    _active_dialogs.clear();
    _active_dialog.clear();

    fgUntie("/sim/menubar/visibility");
    fgUntie("/sim/menubar/overlap-hide");
    _menubar.reset();
    _dialog_props.clear();

#if defined(HAVE_PUI)
    puCleanUpJunk();
#endif
}

void
NewGUI::reinit ()
{
    reset(true);
    fgSetBool("/sim/signals/reinit-gui", true);
}

void
NewGUI::redraw ()
{
    reset(false);
}

void
NewGUI::createMenuBarImplementation()
{
#if defined(SG_MAC)
    if (fgGetBool("/sim/menubar/native", true)) {
        _menubar.reset(new FGCocoaMenuBar);
    }
#endif
#if defined(SG_WINDOWS)
	if (fgGetBool("/sim/menubar/native", true)) {
	// Windows-native menubar disabled for the moment, fall-through
	// to PUI version
   //     _menubar.reset(new FGWindowsMenuBar);
    }
#endif
    if (!_menubar) {
        _menubar.reset(new FGPUIMenuBar);
    }
}

void
NewGUI::reset (bool reload)
{
    DialogDict::iterator iter;
    string_list openDialogs;
    // close all open dialogs and remember them ...
    for (iter = _active_dialogs.begin(); iter != _active_dialogs.end(); ++iter)
        openDialogs.push_back(iter->first);

    for (auto d : openDialogs)
        closeDialog(d);

    setStyle();

    unbind();

    if (reload) {
        _dialog_props.clear();
        _dialog_names.clear();
        init();
    } else {
        createMenuBarImplementation();
        _menubar->init();
    }

    bind();

    // open dialogs again
    for (auto d : openDialogs)
        showDialog(d);
}

void
NewGUI::bind ()
{
}

void
NewGUI::unbind ()
{
}

void NewGUI::postinit()
{
    _menubar->postinit();
}

void
NewGUI::update (double delta_time_sec)
{
    // 主题管理：auto 模式下检测日夜并在切换时重载主题；内部有 0.5 秒节流，
    // 可以安全地每帧调用。
    fguiTheme::update(delta_time_sec);

    // hover 状态轮询：读取鼠标在窗口内的像素坐标（PUI 坐标系 y 轴向下），
    // 由控件层遍历当前打开的对话框做命中测试并切换 highlight()/lowlight()。
    fguiHoverUpdate(fgGetInt("/devices/status/mice/mouse/x", 0),
                    fgGetInt("/devices/status/mice/mouse/y", 0));

    auto iter = _active_dialogs.begin();
    for(/**/; iter != _active_dialogs.end(); iter++)
        iter->second->update();
}

bool
NewGUI::showDialog (const string &name)
{
    if (name.empty()) {
        SG_LOG(SG_GENERAL, SG_ALERT, "showDialog: no dialog name provided");
        return false;
    }

    // first, check if it's already shown
    if (_active_dialogs.find(name) != _active_dialogs.end()){
        _active_dialogs[name]->bringToFront();
        return true;
    }
  
    // check we know about the dialog by name
    if (_dialog_names.find(name) == _dialog_names.end()) {
        simgear::reportFailure(simgear::LoadFailure::NotFound, simgear::ErrorCode::GUIDialog, "Dialog not found:" + name);
        return false;
    }

    flightgear::addSentryBreadcrumb("showing GUI dialog:" + name, "info");
    try {
        _active_dialogs[name] = new FGPUIDialog(getDialogProperties(name));

        fgSetString("/sim/gui/dialogs/current-dialog", name);

        //    setActiveDialog(new FGPUIDialog(getDialogProperties(name)));
        return true;
    } catch (sg_exception& e) {
        simgear::reportFailure(simgear::LoadFailure::Misconfigured, simgear::ErrorCode::GUIDialog, "Dialog failed to show:" + name + ":" + e.getFormattedMessage(), e.getLocation());
        return false;
    }
}

bool
NewGUI::toggleDialog (const string &name)
{
    if (_active_dialogs.find(name) == _active_dialogs.end()) {
        showDialog(name);
        return true;
    }
    else {
        closeDialog(name);
        return false;
    }
}

bool
NewGUI::closeActiveDialog ()
{
    if (_active_dialog == 0)
        return false;

    // TODO support a request-close callback here, optionally
    // many places in code assume this code-path does an
    // immediate close, but for some UI paths it would be nice to
    // allow some dialogs the chance to inervene

    // Kill any entries in _active_dialogs...  Is there an STL
    // algorithm to do (delete map entries by value, not key)?  I hate
    // the STL :) -Andy
    auto iter = _active_dialogs.begin();
    for(/**/; iter != _active_dialogs.end(); iter++) {
        if(iter->second == _active_dialog) {
            _active_dialog->close();

            _active_dialogs.erase(iter);
            // iter is no longer valid
            break;
        }
    }

    _active_dialog = 0;
    if (!_active_dialogs.empty()) {
        fgSetString("/sim/gui/dialogs/current-dialog", _active_dialogs.begin()->second->getName());
    }
    return true;
}

bool
NewGUI::closeDialog (const string& name)
{
    if(_active_dialogs.find(name) != _active_dialogs.end()) {
        flightgear::addSentryBreadcrumb("closing GUI dialog:" + name, "info");

        if(_active_dialog == _active_dialogs[name])
            _active_dialog.clear();

        _active_dialogs.erase(name);
        return true;
    }
    return false; // dialog wasn't open...
}

SGPropertyNode_ptr
NewGUI::getDialogProperties (const string &name)
{
    if (_dialog_names.find(name) == _dialog_names.end()) {
      SG_LOG(SG_GENERAL, SG_ALERT, "Dialog " << name << " not defined");
      return NULL;
    }
  
    NameDialogDict::iterator it = _dialog_props.find(name);
    if (it == _dialog_props.end()) {
      // load the XML
      SGPath path = _dialog_names[name];
      SGPropertyNode_ptr props = new SGPropertyNode;
      try {
        readProperties(path, props);
      } catch (const sg_exception &) {
        SG_LOG(SG_INPUT, SG_ALERT, "Error parsing dialog " << path);
        return NULL;
      }
      
      it = _dialog_props.insert(it, std::make_pair(name, props));
    }

    return it->second;
}

NewGUI::FGDialogRef
NewGUI::getDialog(const string& name)
{
    if(_active_dialogs.find(name) != _active_dialogs.end())
        return _active_dialogs[name];

    SG_LOG(SG_GENERAL, SG_DEBUG, "dialog '" << name << "' missing");
    return 0;
}

void
NewGUI::setActiveDialog (FGDialog * dialog)
{
    if (dialog){
        fgSetString("/sim/gui/dialogs/current-dialog", dialog->getName());
    }
    _active_dialog = dialog;
}

NewGUI::FGDialogRef
NewGUI::getActiveDialog()
{
    return _active_dialog;
}

FGMenuBar *
NewGUI::getMenuBar ()
{
    return _menubar.get();
}

bool
NewGUI::getMenuBarVisible () const
{
    return _menubar->isVisible();
}

void
NewGUI::setMenuBarVisible (bool visible)
{
    if (visible)
        _menubar->show();
    else
        _menubar->hide();
}

bool
NewGUI::getMenuBarOverlapHide() const
{
    return _menubar->getHideIfOverlapsWindow();
}

void
NewGUI::setMenuBarOverlapHide(bool hide)
{
    _menubar->setHideIfOverlapsWindow(hide);
}

void
NewGUI::newDialog (SGPropertyNode* props)
{
    string name = props->getStringValue("name", "");
    if(name.empty()) {
        SG_LOG(SG_GENERAL, SG_ALERT, "New dialog has no <name> property");
        return;
    }
  
    if(_active_dialogs.find(name) == _active_dialogs.end()) {
        _dialog_props[name] = props;
    // add a dummy path entry, so we believe the dialog exists
        _dialog_names[name] = SGPath();
    }
}


void
NewGUI::readDir (const SGPath& path)
{
    simgear::Dir dir(path);
    if( !dir.exists() )
    {
      SG_LOG(SG_INPUT, SG_INFO, "directory does not exist: " << path);
      return;
    }

    flightgear::NavDataCache* cache = flightgear::NavDataCache::instance();
    flightgear::NavDataCache::Transaction txn(cache);
    auto highlight = globals->get_subsystem<Highlight>();
    for (SGPath xmlPath : dir.children(simgear::Dir::TYPE_FILE, ".xml")) {

      SGPropertyNode_ptr props = new SGPropertyNode;
      SGPropertyNode *nameprop = nullptr;
      std::string name;

      /* Always parse the dialog even if cache->isCachedFileModified() says
      it is cached, so that we can register property-dialog associations with
      Highlight::add_property_dialog(). */
      try {
        readProperties(xmlPath, props);
      } catch (const sg_exception &) {
        SG_LOG(SG_INPUT, SG_ALERT, "Error parsing dialog " << xmlPath);
        props.reset();
      }
      if (props) {
        nameprop = props->getNode("name");
        if (nameprop) {
          name = nameprop->getStringValue();
          // Look for 'property' nodes within the dialog. (could
          // also look for "dialog-name" to catch things like
          // dialog-show.)
          std::vector<std::string> property_paths;
          findAllLeafValues(props, "property", property_paths);
          for (auto property_path: property_paths) {
            // We could maybe hoist this test for hightlight to avoid reaching
            // here if it is null, but that's difficult to test, so we do it
            // here instead.
            if (highlight) {
              highlight->addPropertyDialog(property_path, name);
            }
          }
        }
      }
        
      if (!cache->isCachedFileModified(xmlPath)) {
        // cached, easy
        string name = cache->readStringProperty(xmlPath.utf8Str());
        _dialog_names[name] = xmlPath;
        continue;
      }
      
      // we need to parse the actual XML
      if (!props) {
        SG_LOG(SG_INPUT, SG_ALERT, "Error parsing dialog " << xmlPath);
        continue;
      }
      
      if (!nameprop) {
        SG_LOG(SG_INPUT, SG_WARN, "dialog " << xmlPath << " has no name; skipping.");
        continue;
      }
      
      _dialog_names[name] = xmlPath;
      // update cached values
      if (!cache->isReadOnly()) {
        cache->stampCacheFile(xmlPath);
        cache->writeStringProperty(xmlPath.utf8Str(), name);
      }
    } // of directory children iteration
  
    txn.commit();
}
////////////////////////////////////////////////////////////////////////
// Style handling.
////////////////////////////////////////////////////////////////////////

void
NewGUI::setStyle (void)
{
    _itt_t it;
    for (it = _colors.begin(); it != _colors.end(); ++it)
      delete it->second;
    _colors.clear();

    // set up the traditional colors as default
    _colors["background"] = new FGColor(0.8f, 0.8f, 0.9f, 0.85f);
    _colors["foreground"] = new FGColor(0.0f, 0.0f, 0.0f, 1.0f);
    _colors["highlight"]  = new FGColor(0.7f, 0.7f, 0.7f, 1.0f);
    // 文字槽兜底色改为近白（原为纯黑）：样式未定义 label/legend/misc 时，
    // 深色背景样式（如默认的 anthrax.xml）不再出现"黑字灰底"。若某个
    // 浅色样式未定义这些键，FGPUIDialog::setColor 里的自动对比度调整
    // 会把它们再自动改回深色，两种背景方向都安全。
    _colors["label"]      = new FGColor(0.95f, 0.95f, 0.97f, 1.0f);
    _colors["legend"]     = new FGColor(0.95f, 0.95f, 0.97f, 1.0f);
    _colors["misc"]       = new FGColor(0.95f, 0.95f, 0.97f, 1.0f);
    _colors["inputfield"] = new FGColor(0.8f, 0.7f, 0.7f, 1.0f);

    //puSetDefaultStyle();

    
    if (0) {
        // Re-read gui/style/*.xml files so that one can edit them and see the
        // results without restarting flightgear.
        SGPath  p(globals->get_fg_root(), "gui/styles");
        SGPropertyNode* sim_gui = globals->get_props()->getNode("sim/gui/", true);
        simgear::Dir dir(p);
        int i = 0;
        for (SGPath xml: dir.children(simgear::Dir::TYPE_FILE, ".xml")) {
            SGPropertyNode_ptr node = sim_gui->getChild("style", i, true);
            node->removeAllChildren();
            SG_LOG(SG_GENERAL, SG_WARN, "reading from " << xml << " into " << node->getPath());
            readProperties(xml, node);
            i += 1;
        }
    }
    
    int which = fgGetInt("/sim/gui/current-style", 0);
    SGPropertyNode *sim = globals->get_props()->getNode("sim/gui", true);
    SGPropertyNode *n = sim->getChild("style", which);
    if (!n)
        n = sim->getChild("style", 0, true);
    
    SGPropertyNode *selected_style = globals->get_props()->getNode("sim/gui/selected-style", true);

    // n->copy() doesn't delete existing nodes, so need to clear them all
    // first.
    selected_style->removeAllChildren();
    copyProperties(n, selected_style);

    //if (selected_style && n)
    //    n->alias(selected_style);

    // 主题字体必须先于 setupFont 装入 FGFontCache：anthrax.xml 的字体名是
    // 带子目录的相对路径（如 cn/simhei.ttf），若等本函数末尾的
    // fguiTheme::init() 才加载，setupFont 查不到该字体时会静默回退到
    // 内置位图字体，中文界面将完全不可读。
    fguiTheme::preloadFont();
    setupFont(n->getNode("fonts/gui", true));
    n = n->getNode("colors", true);

    for (int i = 0; i < n->nChildren(); i++) {
        SGPropertyNode *child = n->getChild(i);
        _colors[child->getNameString()] = new FGColor(child);
    }

    FGColor *c = _colors["background"];
#if defined(HAVE_PUI)
    puSetDefaultColourScheme(c->red(), c->green(), c->blue(), c->alpha());
#endif

    // 样式色作为基础配色，主题色（ui_theme*.xml）在其上覆盖。
    // 主题文件不存在时 fguiTheme::init() 不修改任何颜色，全部路径保持现状。
    fguiTheme::init();
}


void
NewGUI::setupFont (SGPropertyNode *node)
{
#if defined(HAVE_PUI)
    _font = FGFontCache::instance()->get(node);

    // 主题启用时强制使用主题的 Unicode 字体（如 cn/simhei.ttf）。
    //
    // 内置位图字体（guiFonts）只有 256 个单字节 Latin-1 字形：一旦样式表
    // 解析不到中文字体、或字体文件尚未装入 FGFontCache，get() 会回退到它，
    // 整个界面（菜单栏、对话框）的中文就会被逐字节渲染成 "æ–‡ä»¶" 这类乱码。
    // GUI 重置后会重新走一遍 setStyle()，是最容易触发该回退的时机
    //（2026-11-20 实际事故：点击重置后菜单栏变成乱码）。
    // 这里显式 loadFont 后按同一字号/斜体重新解析，保证拿到的是主题字体。
    if (fguiTheme::enabled()) {
        const std::string themeFontName = fguiTheme::fontName();
        if (!themeFontName.empty()) {
            // 字号优先取主题配置（<font><size>），避免切换 GUI 样式后
            // 跟着样式表变成 tortola 的 12px、整个界面缩小错位。
            float size = fguiTheme::fontSize();
            if (size <= 0.0f)
                size = node->getFloatValue("size", 15.0f);
            FGFontCache::instance()->loadFont(themeFontName);
            puFont* themeFont =
                FGFontCache::instance()->get(themeFontName, size,
                                             node->getFloatValue("slant", 0.0f));
            if (themeFont)
                _font = themeFont;
        }
    }

    // 诊断日志：GUI 重置/换样式时会重新走本函数，若这里解析到的不是主题字体，
    // 界面中文就会变成 "æ–‡ä»¶" 这类乱码，日志能直接定位到出问题的字号/名字。
    SG_LOG(SG_GUI, SG_INFO,
           "NewGUI::setupFont: 样式字体 '" << node->getStringValue("name")
           << "'，主题字体 '"
           << (fguiTheme::enabled() ? fguiTheme::fontName() : std::string("<主题未启用>"))
           << "'");

    puSetDefaultFonts(*_font, *_font);
#endif
    return;
}


void
NewGUI::setThemeColor (const std::string &name, const FGColor &color)
{
    // 颜色表按值管理 FGColor*，覆盖前先释放旧对象，避免 setStyle 之外的
    // 运行时主题切换反复泄漏。
    ColourDict::iterator it = _colors.find(name);
    if (it != _colors.end()) {
        delete it->second;
    }

    // 用分量显式构造，避免依赖 FGColor 的隐式拷贝语义。
    _colors[name] = new FGColor(color.red(), color.green(), color.blue(), color.alpha());
}


// Register the subsystem.
SGSubsystemMgr::Registrant<NewGUI> registrantNewGUI(
    SGSubsystemMgr::INIT);

// end of new_gui.cxx
