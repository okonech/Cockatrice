#include "ai_coach_client.h"

#include "dotenv_loader.h"

#include <QDebug>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>

namespace {
QString extractOutputText(const QJsonObject &root)
{
    if (root.contains("output_text") && root.value("output_text").isString()) {
        return root.value("output_text").toString();
    }

    if (!root.contains("output") || !root.value("output").isArray()) {
        return {};
    }

    QStringList parts;
    const QJsonArray output = root.value("output").toArray();
    for (const QJsonValue &itemValue : output) {
        if (!itemValue.isObject()) {
            continue;
        }
        const QJsonObject item = itemValue.toObject();
        if (!item.contains("content") || !item.value("content").isArray()) {
            continue;
        }
        const QJsonArray content = item.value("content").toArray();
        for (const QJsonValue &contentValue : content) {
            if (!contentValue.isObject()) {
                continue;
            }
            const QJsonObject contentObj = contentValue.toObject();
            const QString type = contentObj.value("type").toString();
            if (type == "output_text" && contentObj.value("text").isString()) {
                parts.append(contentObj.value("text").toString());
            }
        }
    }

    return parts.join("\n").trimmed();
}

QString extractErrorMessage(const QJsonObject &root)
{
    if (!root.contains("error") || !root.value("error").isObject()) {
        return {};
    }
    const QJsonObject err = root.value("error").toObject();
    const QString message = err.value("message").toString();
    return message.isEmpty() ? QStringLiteral("Unknown error") : message;
}
} // namespace

AiCoachClient::AiCoachClient(QObject *parent) : QObject(parent), nam(new QNetworkAccessManager(this))
{
    connect(nam, &QNetworkAccessManager::finished, this, &AiCoachClient::onReplyFinished);
}

AiCoachClient::Config AiCoachClient::loadConfig(QString *errorMessage)
{
    DotenvLoader::loadOnce();

    const QByteArray endpointEnv = qgetenv("COCKATRICE_AI_ENDPOINT");
    const QByteArray apiKeyEnv = qgetenv("COCKATRICE_AI_API_KEY");
    const QByteArray modelEnv = qgetenv("COCKATRICE_AI_MODEL");

    const QString endpointStr = endpointEnv.isEmpty()
        ? QStringLiteral(
              "https://aihub7481090944.cognitiveservices.azure.com/openai/responses?api-version=2025-04-01-preview")
        : QString::fromUtf8(endpointEnv);

    Config cfg;
    cfg.endpoint = QUrl(endpointStr);
    cfg.apiKey = QString::fromUtf8(apiKeyEnv);
    cfg.model = QString::fromUtf8(modelEnv);

    if (!cfg.endpoint.isValid()) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("COCKATRICE_AI_ENDPOINT is invalid");
        }
        return {};
    }
    if (cfg.apiKey.isEmpty()) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Missing COCKATRICE_AI_API_KEY (set it in .env)");
        }
        return {};
    }
    if (cfg.model.isEmpty()) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Missing COCKATRICE_AI_MODEL (Azure deployment/model name)");
        }
        return {};
    }

    return cfg;
}

void AiCoachClient::requestRecommendation(const QString &promptText)
{
    QString error;
    const Config cfg = loadConfig(&error);
    if (!error.isEmpty()) {
        emit recommendationError(error);
        return;
    }

    qInfo().noquote() << "AiCoachClient: preparing request (endpoint=" << cfg.endpoint.toString() << ", model=" << cfg.model
                      << ", promptChars=" << promptText.size() << ")";

    QNetworkRequest req(cfg.endpoint);
    req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    req.setRawHeader("api-key", cfg.apiKey.toUtf8());

    QJsonObject payload;
    payload.insert("model", cfg.model);
    payload.insert("input", promptText);

    // Keep output reasonably bounded.
    payload.insert("max_output_tokens", 700);

    const QByteArray body = QJsonDocument(payload).toJson(QJsonDocument::Compact);

    requestTimer.start();
    QNetworkReply *reply = nam->post(req, body);
    qInfo() << "AiCoachClient: POST sent (bytes:" << body.size() << ", reply=" << reply << ")";
}

void AiCoachClient::onReplyFinished(QNetworkReply *reply)
{
    const QVariant statusAttr = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute);
    const int httpStatus = statusAttr.isValid() ? statusAttr.toInt() : -1;
    const qint64 elapsedMs = requestTimer.isValid() ? requestTimer.elapsed() : -1;

    const QByteArray bytes = reply->readAll();

    QJsonParseError parseError{};
    const QJsonDocument doc = QJsonDocument::fromJson(bytes, &parseError);

    if (reply->error() != QNetworkReply::NoError) {
        qInfo() << "AiCoachClient: reply finished with network error (httpStatus:" << httpStatus
                << ", elapsedMs:" << elapsedMs << ", error:" << reply->error() << ")";
        QString msg = reply->errorString();
        if (doc.isObject()) {
            const QString apiMsg = extractErrorMessage(doc.object());
            if (!apiMsg.isEmpty()) {
                msg = apiMsg;
            }
        }
        reply->deleteLater();
        emit recommendationError(msg);
        return;
    }

    qInfo() << "AiCoachClient: reply finished (httpStatus:" << httpStatus << ", elapsedMs:" << elapsedMs
            << ", bytes:" << bytes.size() << ")";

    if (!doc.isObject()) {
        reply->deleteLater();
        emit recommendationError(QStringLiteral("Unexpected response (not JSON object)"));
        return;
    }

    const QJsonObject root = doc.object();
    const QString apiError = extractErrorMessage(root);
    if (!apiError.isEmpty()) {
        reply->deleteLater();
        emit recommendationError(apiError);
        return;
    }

    const QString text = extractOutputText(root);
    reply->deleteLater();

    if (text.isEmpty()) {
        emit recommendationError(QStringLiteral("No output_text found in response"));
        return;
    }

    emit recommendationReady(text);
}
