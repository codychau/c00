#ifndef AIPAGE_H
#define AIPAGE_H

#include <QWidget>
#include <QLabel>
#include <QPushButton>
#include <QLineEdit>
#include <QSpinBox>
#include <QDoubleSpinBox>
#include <QTimer>
#include <QProcess>
#include <QList>
#include <QString>

class NoWheelFilter;

class AIPage : public QWidget
{
    Q_OBJECT

public:
    explicit AIPage(QWidget *parent = nullptr);

private slots:
    void refresh();
    void onApply(int index);
    void onReset(int index);

private:
    enum WidgetType { WT_String, WT_Int, WT_Double };

    struct ParamDef {
        QString key;
        QString shortKey;
        QString label;
        WidgetType wtype = WT_String;
        QString defaultValue;
        int intMin = 0, intMax = 999999;
        double dblMin = 0, dblMax = 1;
        int decimals = 2;
        double step = 0.05;
        QStringList choices;
        bool isToggle = false;
        QWidget *widget = nullptr;
    };

    struct AICard {
        QString name;
        QString systemdSvc;
        QString procName;
        int port = 0;

        QLabel *statusLbl = nullptr;
        QLabel *detailLbl = nullptr;
        QLabel *methodLbl = nullptr;
        QWidget *controlsWidget = nullptr;
        QPushButton *applyBtn = nullptr;
        QPushButton *resetBtn = nullptr;

        QList<ParamDef> paramDefs;
        bool running = false;
        QString startMethod;
        QString svcFilePath;
        QString pidStr;
    };

    void buildCard(int idx);
    QWidget *createParamWidget(ParamDef &def, const QString &value);
    QString getParamValue(const ParamDef &def);
    void setParamValue(ParamDef &def, const QString &value);
    void checkService(int idx);
    void finishSystemdCheck(int idx, bool userScope);
    void finishProcessCheck(int idx);
    void tryFindServiceFile(int idx);
    void findServiceFile(int idx);
    void readServiceFile(int idx);
    void applyService(int idx);
    void applyProcess(int idx);
    void updateControlStates(int idx);
    void runCmd(const QString &cmd, const QStringList &args,
                std::function<void(const QString &, int)> cb);

    NoWheelFilter *m_nowheel;
    QList<AICard> m_cards;
    QLabel *m_info;
    QPushButton *m_refreshBtn;
    QTimer *m_timer;
    QString m_configDir;
};

#endif
