#ifndef DESKTOPPAGE_H
#define DESKTOPPAGE_H

#include <QWidget>
#include <QLabel>
#include <QPushButton>
#include <QLineEdit>
#include <QComboBox>
#include <QSpinBox>
#include <QScrollArea>
#include <QProcess>
#include <QVBoxLayout>
#include <QStringList>
#include <QTableWidget>
#include <QCheckBox>

#include "keybindingconfig.h"

class DesktopPage : public QWidget
{
    Q_OBJECT

public:
    explicit DesktopPage(QWidget *parent = nullptr);

private slots:
    void detectCompositor();

    // 快捷键
    void refreshBindings();
    void clearEditor();
    void loadEditor(const Keybinding &b);
    void saveEditor(Keybinding &b);
    void onAdd();
    void onDelete();
    void onApply();

    // 后景：壁纸
    void checkTool();
    void applyWallpaper();
    void refreshCurrent();
    void pickWallpaper();

    // 中：Waybar
    void applyWaybar();
    void refreshWaybar();
    void applyWaybarDefaults(const QString &compositor);

    // 前：窗口样式（Hyprland）
    void applyWindowAnimation();
    void toggleBlur();

private:
    void runCmd(const QString &cmd, const QStringList &args,
                std::function<void(const QString &)> cb);
    QString detectCompositorName();

    void addSectionHeader(QVBoxLayout *target, const QString &text);
    void renderTable();
    void setEditorEnabled(bool en);
    void reloadAfterSave(const QString &path);

    // 兼容性
    QLabel *m_status;

    // 后景：壁纸（静态 / 动态）
    QLabel *m_currentLabel;
    QLabel *m_currentWallpaper;
    QLineEdit *m_pathEdit;
    QPushButton *m_applyBtn;
    QPushButton *m_pickBtn;
    QPushButton *m_refreshBtn;
    QComboBox *m_transitionCombo;

    // 中：Waybar
    QComboBox *m_waybarPosition;
    QComboBox *m_waybarLayer;
    QSpinBox *m_waybarHeight;
    QSpinBox *m_waybarSpacing;
    QLineEdit *m_waybarLeft;
    QLineEdit *m_waybarCenter;
    QLineEdit *m_waybarRight;
    QPushButton *m_waybarApplyBtn;
    QPushButton *m_waybarRefreshBtn;
    QLabel *m_waybarStatus;

    // 前：窗口样式（Hyprland）
    QComboBox *m_animCombo;
    QPushButton *m_animApplyBtn;
    QPushButton *m_blurToggleBtn;
    QLabel *m_windowStyleNote;

    // 快捷键
    QList<Keybinding> m_bindings;
    Compositor m_comp = Compositor::Unknown;
    NiriFormat m_niriFmt = NiriFormat::Kdl;
    bool m_addMode = false;

    QTableWidget *m_table;
    QLabel *m_bindStatus;

    QCheckBox *m_chkSuper;
    QCheckBox *m_chkAlt;
    QCheckBox *m_chkShift;
    QCheckBox *m_chkControl;
    QComboBox *m_keyCombo;
    QComboBox *m_typeCombo;
    QLineEdit *m_cmdEdit;
    QSpinBox *m_wsSpin;
    QPushButton *m_btnAdd;
    QPushButton *m_btnDelete;
    QPushButton *m_btnRefresh;
    QPushButton *m_btnApply;
};

#endif // DESKTOPPAGE_H
