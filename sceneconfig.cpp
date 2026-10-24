#include "sceneconfig.h"

#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>

#include <toml++/toml.h>

#include <fstream>

QString SceneConfigManager::defaultConfigPath()
{
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    return dir + "/scenes.toml";
}

SceneConfigManager::SceneConfigManager(const QString &configPath)
    : m_path(configPath.isEmpty() ? defaultConfigPath() : configPath)
{
}

bool SceneConfigManager::load()
{
    m_scenes.clear();

    if (!QFileInfo::exists(m_path))
        return true;

    try {
        auto tbl = toml::parse_file(m_path.toStdString());

        auto *sceneArr = tbl.get("scene");
        if (!sceneArr || !sceneArr->is_array())
            return true;

        for (auto &elem : *sceneArr->as_array()) {
            auto *st = elem.as_table();
            if (!st) continue;

            Scene scene;
            if (auto *v = st->get("name"))
                scene.name = QString::fromStdString(v->value_or(""));
            if (auto *v = st->get("mode"))
                scene.mode = QString::fromStdString(v->value_or("time"));
            if (scene.mode != "manual")
                scene.mode = "time";

            if (auto *v = st->get("enabled"))
                scene.enabled = v->value_or(true);

            if (auto *v = st->get("start")) {
                const QTime t = QTime::fromString(
                    QString::fromStdString(v->value_or("08:00")), "HH:mm");
                if (t.isValid()) scene.startTime = t;
            }
            if (auto *v = st->get("end")) {
                const QTime t = QTime::fromString(
                    QString::fromStdString(v->value_or("23:00")), "HH:mm");
                if (t.isValid()) scene.endTime = t;
            }

            if (auto *v = st->get("days")) {
                if (auto *arr = v->as_array()) {
                    for (auto &el : *arr) {
                        const int day = static_cast<int>(el.value_or(int64_t(0)));
                        if (day >= 1 && day <= 7 && !scene.days.contains(day))
                            scene.days.append(day);
                    }
                }
            }

            auto readServices = [](const toml::table *st, const char *key,
                                   QVector<SceneService> &out) {
                auto *v = st->get(key);
                if (!v) return;
                auto *arr = v->as_array();
                if (!arr) return;
                for (auto &el : *arr) {
                    auto *svcTbl = el.as_table();
                    if (!svcTbl) continue;
                    SceneService svc;
                    if (auto *nv = svcTbl->get("name"))
                        svc.name = QString::fromStdString(nv->value_or(""));
                    if (auto *sv = svcTbl->get("scope"))
                        svc.scope = QString::fromStdString(sv->value_or("system"));
                    if (!svc.name.isEmpty())
                        out.append(svc);
                }
            };
            readServices(st, "start_services", scene.startServices);
            readServices(st, "stop_services", scene.stopServices);

            auto readVms = [](const toml::table *st, const char *key,
                              QStringList &out) {
                auto *v = st->get(key);
                if (!v) return;
                auto *arr = v->as_array();
                if (!arr) return;
                for (auto &el : *arr) {
                    const QString name = QString::fromStdString(el.value_or(""));
                    if (!name.isEmpty()) out.append(name);
                }
            };
            readVms(st, "start_vms", scene.startVms);
            readVms(st, "stop_vms", scene.stopVms);

            if (!scene.name.isEmpty())
                m_scenes.append(scene);
        }
    } catch (const toml::parse_error &) {
        return false;
    }

    return true;
}

bool SceneConfigManager::save() const
{
    toml::table root;
    toml::array sceneArr;

    for (const auto &scene : m_scenes) {
        toml::table st;
        st.emplace("name", scene.name.toStdString());
        st.emplace("mode", scene.mode.toStdString());
        st.emplace("enabled", scene.enabled);
        st.emplace("start", scene.startTime.toString("HH:mm").toStdString());
        st.emplace("end", scene.endTime.toString("HH:mm").toStdString());

        toml::array daysArr;
        for (int day : scene.days)
            daysArr.push_back(day);
        st.emplace("days", std::move(daysArr));

        auto writeServices = [](toml::table &st, const char *key,
                                const QVector<SceneService> &list) {
            toml::array arr;
            for (const auto &svc : list) {
                toml::table svcTbl;
                svcTbl.emplace("name", svc.name.toStdString());
                svcTbl.emplace("scope", svc.scope.toStdString());
                arr.push_back(std::move(svcTbl));
            }
            st.emplace(key, std::move(arr));
        };
        writeServices(st, "start_services", scene.startServices);
        writeServices(st, "stop_services", scene.stopServices);

        auto writeVms = [](toml::table &st, const char *key,
                           const QStringList &list) {
            toml::array arr;
            for (const auto &name : list)
                arr.push_back(name.toStdString());
            st.emplace(key, std::move(arr));
        };
        writeVms(st, "start_vms", scene.startVms);
        writeVms(st, "stop_vms", scene.stopVms);

        sceneArr.push_back(std::move(st));
    }

    root.emplace("scene", std::move(sceneArr));

    QDir().mkpath(QFileInfo(m_path).absolutePath());

    std::ofstream ofs(m_path.toStdString());
    if (!ofs) return false;
    ofs << root << "\n";
    return true;
}

void SceneConfigManager::addScene(const Scene &scene)
{
    m_scenes.append(scene);
}

void SceneConfigManager::removeScene(int index)
{
    if (index >= 0 && index < m_scenes.size())
        m_scenes.removeAt(index);
}

void SceneConfigManager::updateScene(int index, const Scene &scene)
{
    if (index >= 0 && index < m_scenes.size())
        m_scenes[index] = scene;
}
