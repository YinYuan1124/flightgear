// FGPUIDialog.hxx - XML-configured dialog box.
/*
 * SPDX-FileCopyrightText: 2012 (C) James Turner <james@flightgear.org>
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#pragma once

#include "dialog.hxx"

// ensure we include this before puAux.h, so that 
// #define _PU_H_ 1 has been done, and hence we don't
// include the un-modified system pu.h
#include "FlightGear_pu.h"

#include <plib/puAux.h>

#include <simgear/misc/sg_path.hxx>
#include <simgear/props/condition.hxx>
#include <simgear/props/props.hxx>

#include <set>
#include <string>
#include <vector>

// 中文化辅助：把对话框 XML 中的 <label>/<legend> 原文翻译成当前语言的译文。
//
// 行为：纯空白字符串原样返回（布局占位，不能 trim）；其余字符串去掉首尾
// 空白后精确查 FGLocale::translateDialogString，命中时把译文放回原来的
// 首尾空白之间（保留布局间距），未命中返回原文。布局测宽（layout.cxx）
// 与控件创建（setupObject）共用此函数，保证"测量用的串"就是"显示用的串"。
std::string fgTranslateDialogString(const std::string& raw);


// ugly temporary workaround for plib's lack of user defined class ids  FIXME
#define FGCLASS_LIST 0x00000001
#define FGCLASS_AIRPORTLIST 0x00000002
#define FGCLASS_PROPERTYLIST 0x00000004
#define FGCLASS_WAYPOINTLIST 0x00000008
#define FGCLASS_LOGLIST 0x00000010

class GUI_ID
{
public:
    GUI_ID(int id) : id(id) {}
    virtual ~GUI_ID() {}
    int id;
};

class NewGUI;
class FGColor;
class puObject;
class puFont;

/**
 * An XML-configured dialog box.
 *
 * The GUI manager stores only the property tree for the dialog
 * boxes.  This class creates a PUI dialog box on demand from
 * the properties in that tree.  The manager recreates the dialog
 * every time it needs to show it.
 */
class FGPUIDialog : public FGDialog
{
public:
    /**
     * Construct a new GUI widget configured by a property tree.
     *
     * The configuration properties are not part of the main
     * FlightGear property tree; the GUI manager reads them
     * from individual configuration files.
     *
     * @param props A property tree describing the dialog.
     */
    FGPUIDialog(SGPropertyNode* props);


    /**
     * Destructor.
     */
    virtual ~FGPUIDialog();


    /**
     * Update the values of all GUI objects with a specific name,
     * or all if an empty name is given (default).
     *
     * This method copies values from the FlightGear property tree to
     * the GUI object(s).
     *
     * @param objectName The name of the GUI object(s) to update.
     *        Use the empty name for all objects.
     */
    virtual void updateValues(const std::string& objectName = "");


    /**
     * Apply the values of all GUI objects with a specific name,
     * or all if an empty name is given (default).
     *
     * This method copies values from the GUI object(s) to the
     * FlightGear property tree.
     *
     * @param objectName The name of the GUI object(s) to update.
     *        Use the empty name for all objects.
     */
    virtual void applyValues(const std::string& objectName = "");


    /**
     * Update state.  Called on active dialogs before rendering.
     */
    virtual void update();

    void close() override;

    /**
     * Recompute the dialog's layout
     */
    void relayout();


    void setNeedsLayout()
    {
        _needsRelayout = true;
    }
    virtual const char* getName();
    virtual void bringToFront();

    class ActiveWidget
    {
    public:
        virtual void update() = 0;
    };

    void runCallback(const std::string& name, SGPropertyNode_ptr args) override;

    /**
     * 返回对话框的顶层 PUI 对象（puPopup/puDialogBox）。
     *
     * hover 轮询需要从对话框外部遍历控件树做命中测试，因此开放此只读访问器；
     * 返回的指针由本对话框持有，调用方不得删除，也不得在对话框析构后使用。
     */
    puObject* getObject() const { return _object; }

    /**
     * 运行时主题切换后重新给本对话框的控件上色。
     *
     * 遍历 _object 树，对每个带 GUIInfo 的对象按缓存的属性节点与颜色掩码
     * 重跑 setColor()；没有 user_data 的对象（如对话框背景框）使用最近
     * 祖先节点的属性重放，与首次创建时的调用保持一致。
     */
    void reapplyTheme();

    /**
     * 当前打开（尚未析构）的全部 PUI 对话框。
     *
     * 注册表在 FGPUIDialog 构造末尾插入、析构开头移除，生命周期完全由
     * NewGUI 管理；hover 轮询与 fguiTheme::reapplyDialogs() 遍历此集合，
     * 因此不会出现悬垂指针。
     */
    static const std::set<FGPUIDialog*>& openDialogs();

    /**
     * z 序：对话框的置顶顺序号（创建或鼠标按下时刷新）。
     *
     * hover 轮询在多对话框重叠时按此值取最上层命中：PUI 内部把被点击的
     * 对话框移到绘制栈顶，这里用单调计数器近似同一顺序，避免注册表
     * （std::set 按指针地址排序）的遍历顺序与绘制顺序无关的问题。
     */
    unsigned zOrder() const { return _zOrder; }
    void bumpZOrder() { _zOrder = ++s_zCounter; }


private:
    enum {
        BACKGROUND = 0x01,
        FOREGROUND = 0x02,
        HIGHLIGHT = 0x04,
        LABEL = 0x08,
        LEGEND = 0x10,
        MISC = 0x20,
        EDITFIELD = 0x40
    };

    // Show the dialog.
    void display(SGPropertyNode* props);

    // Build the dialog or a subobject of it.
    puObject* makeObject(SGPropertyNode* props,
                         int parentWidth, int parentHeight);

    // Common configuration for all GUI objects.
    void setupObject(puObject* object, SGPropertyNode* props);

    // Common configuration for all GUI group objects.
    void setupGroup(puGroup* group, SGPropertyNode* props,
                    int width, int height, bool makeFrame = false);

    // Set object colors: the "which" argument defines which color qualities
    // (PUCOL_LABEL, etc.) should pick up the <color> property.
    void setColor(puObject* object, SGPropertyNode* props, int which = 0);

    // return key code number for keystring
    int getKeyCode(const char* keystring);

    /**
     * Apply layout sizes to a tree of puObjects
     */
    void applySize(puObject* object);

    /**
     * reapplyTheme() 的递归实现。
     *
     * inheritedProps 是最近祖先节点的属性（用于没有 GUIInfo 的背景框），
     * 顶层调用传 nullptr。
     */
    void reapplyTheme(puObject* object, SGPropertyNode* inheritedProps);

    // The top-level PUI object.
    puObject* _object;

    // 对话框置顶顺序号（hover 多对话框重叠时按它取最上层命中；
    // 创建/鼠标按下时经 bumpZOrder() 刷新）。
    unsigned _zOrder = 0;
    // z 序单调计数器（跨对话框共享；定义在 FGPUIDialog.cxx）。
    static unsigned s_zCounter;

    // The GUI subsystem.
    NewGUI* _gui;

    // The dialog font. Defaults to the global gui font, but can get
    // overridden by a top level font definition.
    puFont* _font;

    // The source xml tree, so that we can pass data back, such as the
    // last position.
    SGPropertyNode_ptr _props;

    bool _needsRelayout;

    // Nasal module.
    std::string _module;
    SGPropertyNode_ptr _nasal_close;

    std::string _name;
    // PUI provides no way for userdata to be deleted automatically
    // with a GUI object, so we have to keep track of all the special
    // data we allocated and then free it manually when the dialog
    // closes.
    std::vector<void*> _info;
    struct PropertyObject {
        PropertyObject(std::string name,
                       puObject* object,
                       SGPropertyNode_ptr node);
        std::string name;
        puObject* object;
        SGPropertyNode_ptr node;
    };
    std::vector<PropertyObject*> _propertyObjects;
    std::vector<PropertyObject*> _liveObjects;

    class ConditionalObject : public SGConditional
    {
    public:
        ConditionalObject(const std::string& aName, puObject* aPu) : _name(aName),
                                                                     _pu(aPu)
        {
            ;
        }

        void update(FGPUIDialog* aDlg);

    private:
        const std::string _name;
        puObject* _pu;
    };

    typedef SGSharedPtr<ConditionalObject> ConditionalObjectRef;
    std::vector<ConditionalObjectRef> _conditionalObjects;

    std::vector<ActiveWidget*> _activeWidgets;
};
