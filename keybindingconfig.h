#ifndef KEYBINDINGCONFIG_H
#define KEYBINDINGCONFIG_H
#include <QString>
#include <QStringList>
#include <QList>
#include <QVector>
#include <QMetaType>
class Keybinding;
enum class Compositor {
    Unknown,
    Sway,
    Hyprland,
    Niri
};
// niri 配置文件存在两代格式：旧版 YAML(config.yml) 与新版 KDL(config.kdl)。
enum class NiriFormat {
    Unknown,
    Kdl,
    Yaml
};
enum class ActionType {
    Launch,
    LaunchShell,
    FocusWorkspace,
    Quit,
    Screenshot,
    ScreenshotScreen,
    ScreenshotWindow,
    Other
};
struct Keybinding {
    QString id;
    QVector<QString> modifiers;
    QString key;
    ActionType type = ActionType::Other;
    QStringList spawnArgs;
    QString shellCommand;
    int workspace = 0;
    QString customBody;
    bool allowWhenLocked = false;
    bool repeat = true;
    bool allowInhibiting = true;
    QString overlayTitle;
    bool isValid() const { return !id.isEmpty(); }

    // 实现细节：不参与序列化/比较
    bool _hasKeycode = false;
    bool isScriptAction = false; // niri：script-action（非 command）
    QStringList _extra; // 保留未管理字段以原样回写
};
QString keybindingDescribe(const Keybinding &b, Compositor c);
QString renderHotkey(const QVector<QString> &mods, const QString &key, Compositor c);

class KeybindingConfig {
public:
    static Compositor detect();
    static QString configPath(Compositor c);
    static NiriFormat niriFormat(const QString &text);
    static QList<Keybinding> parse(Compositor c, const QString &text);
    static QString render(Compositor c, const QList<Keybinding> &binds,
                          NiriFormat niriFmt = NiriFormat::Kdl);
    static QStringList reloadCommand(Compositor c);
    static QString backupPath(const QString &configFile);
    static QString niriRenderCustom(const QString &raw);

    // 可编辑区域（基于原始文本）：[起始行, 结束行)，用于就地改写而非整体覆盖，
    // 避免丢失用户配置文件的其它内容。未找到时返回 {-1, -1}。
    static QPair<int, int> editableRegion(Compositor c, const QString &text);
};
#endif