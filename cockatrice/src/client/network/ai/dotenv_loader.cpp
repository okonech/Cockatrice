#include "dotenv_loader.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSet>
#include <QStandardPaths>
#include <QTextStream>

#include <mutex>

namespace {
std::mutex &dotenvMutex()
{
    static std::mutex m;
    return m;
}

QString &loadedPathStorage()
{
    static QString s_loadedPath;
    return s_loadedPath;
}

QString canonicalOrAbsolutePath(const QString &path)
{
    const QFileInfo fi(path);
    const QString canonical = fi.canonicalFilePath();
    if (!canonical.isEmpty()) {
        return canonical;
    }
    return fi.absoluteFilePath();
}

QString stripExport(QString key);

bool dotenvFileContainsAnyKey(const QString &path, const QSet<QString> &keys)
{
    if (path.isEmpty() || keys.isEmpty()) {
        return false;
    }

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return false;
    }

    QTextStream in(&file);
    while (!in.atEnd()) {
        QString line = in.readLine().trimmed();
        if (line.isEmpty() || line.startsWith('#')) {
            continue;
        }
        const int equalsIndex = line.indexOf('=');
        if (equalsIndex <= 0) {
            continue;
        }
        const QString key = stripExport(line.left(equalsIndex));
        if (keys.contains(key)) {
            return true;
        }
    }

    return false;
}

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

QString DotenvLoader::loadedFilePath()
{
    std::lock_guard<std::mutex> lock(dotenvMutex());
    return loadedPathStorage();
}

bool DotenvLoader::loadOnce()
{
    static bool loadedAny = false;

    std::lock_guard<std::mutex> lock(dotenvMutex());
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

    // Common run location during development (lowest priority): current working directory.
    // This is intentionally last so launching from a desktop shortcut (cwd=HOME) doesn't accidentally
    // pick up ~/.env when a project-local .env exists near the executable.
    addCandidatesFromDir(QDir::currentPath());

    candidatePaths.removeDuplicates();

    // For diagnostics: track the resolved/canonical paths we attempt.
    QStringList resolvedCandidates;
    resolvedCandidates.reserve(candidatePaths.size());
    for (const QString &p : candidatePaths) {
        if (p.isEmpty()) {
            continue;
        }
        resolvedCandidates.append(canonicalOrAbsolutePath(p));
    }
    resolvedCandidates.removeDuplicates();

    QStringList failures;

    // Prefer dotenv files that actually define AI Coach keys. This avoids selecting a generic ~/.env
    // that doesn't contain COCKATRICE_AI_* when a project-local .env does.
    const QSet<QString> preferredKeys = {
        QStringLiteral("COCKATRICE_AI_API_KEY"),
        QStringLiteral("COCKATRICE_AI_ENDPOINT"),
        QStringLiteral("COCKATRICE_AI_MODEL"),
    };

    QStringList preferredExisting;
    QStringList otherExisting;
    for (const QString &path : candidatePaths) {
        if (path.isEmpty() || !QFileInfo::exists(path)) {
            continue;
        }
        const QString resolvedPath = canonicalOrAbsolutePath(path);
        if (dotenvFileContainsAnyKey(resolvedPath, preferredKeys)) {
            preferredExisting.append(resolvedPath);
        } else {
            otherExisting.append(resolvedPath);
        }
    }
    preferredExisting.removeDuplicates();
    otherExisting.removeDuplicates();

    const auto tryLoadList = [&](const QStringList &paths) -> bool {
        for (const QString &resolvedPath : paths) {
            QString error;
            if (DotenvLoader::loadFromFile(resolvedPath, &error)) {
                loadedAny = true;
                loadedPathStorage() = resolvedPath;
                qInfo().noquote() << "DotenvLoader: loaded .env from" << resolvedPath;
                return true;
            }
            if (!error.isEmpty()) {
                failures.append(QStringLiteral("- %1 (%2)").arg(resolvedPath, error));
            } else {
                failures.append(QStringLiteral("- %1 (failed to parse)").arg(resolvedPath));
            }
        }
        return false;
    };

    if (tryLoadList(preferredExisting)) {
        return true;
    }
    if (tryLoadList(otherExisting)) {
        return true;
    }

    // If we didn't load anything, log what we resolved/checked so users can see the exact paths.
    if (!loadedAny) {
        qInfo().noquote() << "DotenvLoader: no .env loaded. Resolved search paths:";
        const int maxToPrint = 25;
        for (int i = 0; i < resolvedCandidates.size() && i < maxToPrint; ++i) {
            qInfo().noquote() << "-" << resolvedCandidates[i];
        }
        if (resolvedCandidates.size() > maxToPrint) {
            qInfo().noquote() << QStringLiteral("- … (%1 more)").arg(resolvedCandidates.size() - maxToPrint);
        }

        if (!failures.isEmpty()) {
            qWarning().noquote() << "DotenvLoader: .env file(s) were found but could not be loaded:";
            const int maxFailuresToPrint = 10;
            for (int i = 0; i < failures.size() && i < maxFailuresToPrint; ++i) {
                qWarning().noquote() << failures[i];
            }
            if (failures.size() > maxFailuresToPrint) {
                qWarning().noquote() << QStringLiteral("… (%1 more)").arg(failures.size() - maxFailuresToPrint);
            }
        }
    }

    return false;
}

bool DotenvLoader::loadFromFile(const QString &path, QString *errorMessage)
{
    const QString resolvedPath = canonicalOrAbsolutePath(path);
    QFile file(resolvedPath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        if (errorMessage) {
            *errorMessage = QString("Failed to open %1").arg(resolvedPath);
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
