#ifndef SERVICEEDITDIALOG_H
#define SERVICEEDITDIALOG_H

#include <QDialog>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QProcess>

class ServiceEditDialog : public QDialog
{
    Q_OBJECT

public:
    explicit ServiceEditDialog(const QString &serviceName,
                               const QString &filePath,
                               const QString &scope,
                               QWidget *parent = nullptr);
    ~ServiceEditDialog() override;

signals:
    void saved(const QString &serviceName);

private slots:
    void onSave();
    void onCancel();

private:
    void loadFile();
    void writeDirect();
    void writeViaRoot();
    void reloadUnit(bool asUser);
    void saveFailed(const QString &msg);

    QString m_serviceName;
    QString m_filePath;
    QString m_scope;
    QString m_initialContent;
    QString m_tempPath;

    QPlainTextEdit *m_editor;
    QLabel *m_status;
    QPushButton *m_saveBtn;
    QPushButton *m_cancelBtn;
    QProcess *m_proc = nullptr;
};

#endif // SERVICEEDITDIALOG_H