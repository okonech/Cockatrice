#ifndef COCKATRICE_AI_COACH_CLIENT_H
#define COCKATRICE_AI_COACH_CLIENT_H

#include <QElapsedTimer>
#include <QObject>
#include <QUrl>

class QNetworkAccessManager;
class QNetworkReply;

class AiCoachClient : public QObject
{
    Q_OBJECT
public:
    explicit AiCoachClient(QObject *parent = nullptr);

    struct Config
    {
        QUrl endpoint;
        QString apiKey;
        QString model;
    };

    // Loads configuration from environment variables (optionally via .env).
    // Env vars:
    // - COCKATRICE_AI_ENDPOINT (defaults to Azure Responses endpoint)
    // - COCKATRICE_AI_API_KEY (required)
    // - COCKATRICE_AI_MODEL (required)
    static Config loadConfig(QString *errorMessage = nullptr);

    // Starts an async request. Emits recommendationReady / recommendationError.
    void requestRecommendation(const QString &promptText);

signals:
    void recommendationReady(const QString &text);
    void recommendationError(const QString &message);

private slots:
    void onReplyFinished(QNetworkReply *reply);

private:
    QNetworkAccessManager *nam;
    QElapsedTimer requestTimer;
};

#endif // COCKATRICE_AI_COACH_CLIENT_H
