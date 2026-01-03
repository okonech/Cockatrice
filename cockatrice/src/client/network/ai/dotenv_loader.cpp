#include "dotenv_loader.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <QTextStream>

#include <mutex>

namespace {
QString unquoteValue(QString value)
{
    value = value.trimmed();
    if (value.size() >= 2) {
        const QChar first = value.front();
        const QChar last = value.back();
        if ((first == '"' && last == '"') || (first == '\'' && last == '\'')) {
            value = value.mid(1, value.size() - 2);
        }
    }
    return value;
}

QString stripExport(QString key)
{
    key = key.trimmed();
    if (key.startsWith("export ")) {
        key = key.mid(QString("export ").size()).trimmed();
    }
    return key;
}
} // namespace

bool DotenvLoader::loadOnce()
{
    static std::mutex m;
    static bool loadedAny = false;

    std::lock_guard<std::mutex> lock(m);
    if (loadedAny) {
        return true;
    }

    const QString configDir = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);

    QStringList candidatePaths;

    auto addCandidatesFromDir = [&candidatePaths](const QString &dirPath) {
        if (dirPath.isEmpty()) {
            return;
        }
        const QDir d(dirPath);
        candidatePaths.append(d.filePath(".env"));
        candidatePaths.append(d.filePath("cockatrice/.env"));
    };

    // Common run locations during development.
    addCandidatesFromDir(QDir::currentPath());

    // Executable directory and a couple parents (e.g. build/cockatrice -> build -> repo root).
    const QString appDir = QCoreApplication::applicationDirPath();
    addCandidatesFromDir(appDir);
    if (!appDir.isEmpty()) {
        const QDir appQDir(appDir);
        addCandidatesFromDir(appQDir.absoluteFilePath(".."));
        addCandidatesFromDir(appQDir.absoluteFilePath("../.."));
    }

    // User config directory.
    addCandidatesFromDir(configDir);

    candidatePaths.removeDuplicates();

    for (const QString &path : candidatePaths) {
        if (path.isEmpty()) {
            continue;
        }
        if (!QFileInfo::exists(path)) {
            continue;
        }
        QString error;
        if (DotenvLoader::loadFromFile(path, &error)) {
            loadedAny = true;
            return true;
        }
    }

    return false;
}

bool DotenvLoader::loadFromFile(const QString &path, QString *errorMessage)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        if (errorMessage) {
            *errorMessage = QString("Failed to open %1").arg(path);
        }
        return false;
    }

    QTextStream in(&file);
    while (!in.atEnd()) {
        QString line = in.readLine();
        line = line.trimmed();

        if (line.isEmpty() || line.startsWith('#')) {
            continue;
        }

        const int equalsIndex = line.indexOf('=');
        if (equalsIndex <= 0) {
            continue;
        }

        QString key = stripExport(line.left(equalsIndex));
        QString value = unquoteValue(line.mid(equalsIndex + 1));
        if (key.isEmpty()) {
            continue;
        }

        if (!qEnvironmentVariableIsSet(key.toUtf8().constData())) {
            qputenv(key.toUtf8().constData(), value.toUtf8());
        }
    }

    return true;
}
