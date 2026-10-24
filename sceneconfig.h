#ifndef SCENECONFIG_H
#define SCENECONFIG_H

#include <QString>
#include <QVector>
#include <QList>
#include <QTime>

// 场景中受控的服务（scope: system / user）
struct SceneService {
    QString name;
    QString scope = "system";
};

// 单个场景：time 模式按时间段调度；manual 模式只由用户手动启动 / 结束
struct Scene {
    QString name;
    QString mode = "time";   // "time" 或 "manual"
    bool    enabled = true;
    QTime   startTime = QTime(8, 0);
    QTime   endTime   = QTime(23, 0);

    // 生效的星期（ISO：1=周一 … 7=周日），为空表示每天
    QList<int> days;

    QVector<SceneService> startServices;
    QVector<SceneService> stopServices;
    QStringList startVms;
    QStringList stopVms;
};

// 场景配置管理器 —— 读写 ~/.config/ServerToolbox/scenes.toml
class SceneConfigManager {
public:
    explicit SceneConfigManager(const QString &configPath = QString());

    bool load();
    bool save() const;

    const QVector<Scene> &scenes() const { return m_scenes; }
    QVector<Scene> &scenes() { return m_scenes; }

    void addScene(const Scene &scene);
    void removeScene(int index);
    void updateScene(int index, const Scene &scene);

    QString configPath() const { return m_path; }
    static QString defaultConfigPath();

private:
    QString m_path;
    QVector<Scene> m_scenes;
};

#endif // SCENECONFIG_H
