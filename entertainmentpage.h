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
        QString id;
        QString name;
        QString friendlyDesc;
        int volumePercent = 50;
    };

    void runCmd(const QString &cmd, const QStringList &args,
                std::function<void(const QString &)> cb);
    void parseSinks();

    QString detectAudioBackend() const;
    QString friendlyAudioDescription(const QString &desc) const;
    void parsePactlSinks(const QString &out, const QString &defaultSinkName);
    void parseWpctlSinks(const QString &out);
    void fillSinkCombo(int defaultIdx);

    QString detectResolutionBackend() const;
    void parseNiriResolution(const QString &out);
    void parseHyprResolution(const QString &out);
    void parseXrandrResolution(const QString &out);
    void fillResolutionCombo(const QString &currentRes);
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
    QString m_currentSinkId;
    QString m_audioBackend;
    int m_currentVolume = 50;

    // 分辨率
    QComboBox *m_resolutionCombo;
    QLabel *m_currentResLabel;
    QLabel *m_resStatus;
    QPushButton *m_resApplyBtn;

    QStringList m_availableModes;
    QString m_monitorName;
    QString m_resolutionBackend;

    // 手柄
    QLabel         *m_ctrlStatusLabel;
    QPushButton    *m_ctrlDetectBtn;
    QLabel         *m_ctrlListLabel;
    QProcess       *m_ctrlProcess;

    void detectControllers();
};

#endif // ENTERTAINMENTPAGE_H
