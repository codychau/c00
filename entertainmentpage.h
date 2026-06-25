#ifndef ENTERTAINMENTPAGE_H
#define ENTERTAINMENTPAGE_H

#include <QWidget>
#include <QLabel>
#include <QSlider>
#include <QPushButton>
#include <QComboBox>
#include <QScrollArea>
#include <QProcess>
#include <QList>
#include <QString>
#include <QStringList>

class EntertainmentPage : public QWidget
{
    Q_OBJECT

public:
    explicit EntertainmentPage(QWidget *parent = nullptr);

private slots:
    void refreshStatus();
    void onSwitchSink(int index);
    void onVolumeSliderReleased();
    void onVolumeUp();
    void onVolumeDown();

    // 分辨率
    void onResolutionChanged();
    void refreshResolution();

private:
    struct SinkInfo {
        QString name;
        QString friendlyDesc;
        int volumePercent = 50;
    };

    void runCmd(const QString &cmd, const QStringList &args,
                std::function<void(const QString &)> cb);
    void parseSinks();
    void setVolume(int percent);
    void refreshVolumeOnly();
    void updateVolumeLabel();

    // 音频
    QLabel *m_status;
    QLabel *m_sinkLabel;
    QComboBox *m_sinkCombo;
    QPushButton *m_refreshBtn;

    QSlider *m_volumeSlider;
    QLabel *m_volumeLabel;
    QPushButton *m_volUp;
    QPushButton *m_volDown;

    QList<SinkInfo> m_sinks;
    QString m_currentSinkName;
    int m_currentVolume = 50;

    // 分辨率
    QComboBox *m_resolutionCombo;
    QLabel *m_currentResLabel;
    QLabel *m_resStatus;
    QPushButton *m_resApplyBtn;

    QStringList m_availableModes;
    QString m_monitorName;
};

#endif // ENTERTAINMENTPAGE_H
