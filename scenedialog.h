#ifndef SCENEDIALOG_H
#define SCENEDIALOG_H

#include <QDialog>

#include "sceneconfig.h"

class QLineEdit;
class QTableWidget;
class QCheckBox;

// 新建 / 编辑场景：选择需要启动或停止的服务与虚机
class SceneDialog : public QDialog
{
    Q_OBJECT

public:
    explicit SceneDialog(const Scene &scene, QWidget *parent = nullptr);

    Scene scene() const;

private:
    void loadServices();
    void loadVms();
    void filterServices(const QString &text);
    void setSelectedAction(QTableWidget *table, const QString &action);
    void addServiceRow(const QString &name, const QString &scope,
                       const QString &action);
    void addVmRow(const QString &name, const QString &action);
    static QString actionText(const QString &action);
    static QString actionKey(const QString &scope, const QString &name);

    Scene m_scene;

    QLineEdit    *m_nameEdit;
    QCheckBox    *m_enabledCheck;
    QLineEdit    *m_serviceSearch;
    QTableWidget *m_serviceTable;
    QTableWidget *m_vmTable;
};

#endif // SCENEDIALOG_H
