#ifndef ADMINHELPER_H
#define ADMINHELPER_H

#include <QString>

class QWidget;

namespace AdminHelper {

bool passwordlessSudoAvailable();

bool isAuthFailure(const QString &error);

bool runAsRootScriptWithPassword(const QString &script, const QString &password,
                                 QString *output = nullptr, QString *error = nullptr);

bool runAsRootScript(const QString &script, QWidget *parent,
                     QString *output = nullptr, QString *error = nullptr);

}

#endif // ADMINHELPER_H
