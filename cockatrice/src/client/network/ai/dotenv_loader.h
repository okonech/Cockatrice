#ifndef COCKATRICE_DOTENV_LOADER_H
#define COCKATRICE_DOTENV_LOADER_H

#include <QString>

class DotenvLoader
{
public:
    // Loads a .env file once and injects variables into the process environment.
    // Existing environment variables are not overridden.
    // Returns true if a file was found and parsed (even if empty).
    static bool loadOnce();

    // Absolute, canonical path of the loaded .env file (empty if none loaded).
    static QString loadedFilePath();

    // Visible for diagnostics/testing.
    static bool loadFromFile(const QString &path, QString *errorMessage = nullptr);
};

#endif // COCKATRICE_DOTENV_LOADER_H
