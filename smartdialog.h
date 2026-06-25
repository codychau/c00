#ifndef SMARTDIALOG_H
#define SMARTDIALOG_H

#include <QDialog>
#include <QLabel>
#include <QTableWidget>
#include <QTextEdit>

class SmartDialog : public QDialog
{
    Q_OBJECT

public:
    explicit SmartDialog(const QString &devPath, QWidget *parent = nullptr);

private:
    void loadSmartData(const QString &devPath);
    void parseAndDisplay(const QString &raw);

    QLabel       *m_titleLabel;
    QLabel       *m_healthLabel;
    QLabel       *m_infoLabel;
    QTableWidget *m_attrTable;
    QTextEdit    *m_rawView;
};

#endif // SMARTDIALOG_H
