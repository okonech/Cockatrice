#include "ai_coach_client.h"

#include "dotenv_loader.h"

#include <QDebug>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>

#include <QPointer>

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

QString extractStreamErrorMessage(const QJsonObject &event)
{
    // Azure Responses streaming error events generally carry an "error" object.
    if (event.contains("error") && event.value("error").isObject()) {
        return extractErrorMessage(event);
    }
    const QString message = event.value("message").toString();
    return message;
}

struct SseParseResult
{
    QStringList deltas;
    QString error;
};

SseParseResult consumeSseFrames(QByteArray *buffer)
{
    SseParseResult out;
    if (!buffer) {
        return out;
    }

    // Process complete SSE frames separated by a blank line.
    while (true) {
        int sep = buffer->indexOf("\n\n");
        int sepLen = 2;
        if (sep < 0) {
            sep = buffer->indexOf("\r\n\r\n");
            sepLen = 4;
        }
        if (sep < 0) {
            break;
        }

        const QByteArray frame = buffer->left(sep);
        buffer->remove(0, sep + sepLen);

        QStringList dataLines;
        const QList<QByteArray> lines = frame.split('\n');
        for (QByteArray line : lines) {
            line = line.trimmed();
            if (line.startsWith("data:")) {
                QByteArray data = line.mid(5);
                if (data.startsWith(' ')) {
                    data = data.mid(1);
                }
                dataLines.append(QString::fromUtf8(data));
            }
        }

        if (dataLines.isEmpty()) {
            continue;
        }

        const QString dataStr = dataLines.join('\n').trimmed();
        if (dataStr.isEmpty() || dataStr == QStringLiteral("[DONE]")) {
            continue;
        }

        QJsonParseError parseError{};
        const QJsonDocument doc = QJsonDocument::fromJson(dataStr.toUtf8(), &parseError);
        if (!doc.isObject()) {
            continue;
        }

        const QJsonObject evt = doc.object();
        const QString type = evt.value("type").toString();
        if (type == QStringLiteral("response.output_text.delta")) {
            const QString delta = evt.value("delta").toString();
            if (!delta.isEmpty()) {
                out.deltas.append(delta);
            }
        } else if (type == QStringLiteral("response.output_text")) {
            // Some deployments may stream full chunks as output_text items.
            const QString text = evt.value("text").toString();
            if (!text.isEmpty()) {
                out.deltas.append(text);
            }
        } else if (type == QStringLiteral("response.error")) {
            const QString msg = extractStreamErrorMessage(evt);
            if (!msg.isEmpty()) {
                out.error = msg;
            }
        }
    }

    return out;
}
} // namespace

AiCoachClient::AiCoachClient(QObject *parent) : QObject(parent), nam(new QNetworkAccessManager(this))
{
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
    // Prefer streaming (SSE) when supported.
    req.setRawHeader("Accept", "text/event-stream");

    QJsonObject payload;
    payload.insert("model", cfg.model);
    payload.insert("input", promptText);

    payload.insert("stream", true);

    // Keep output reasonably bounded.
    // The AI Coach prompt asks for structured, careful reasoning (mulligan framework, alternatives, risks).
    // Use a higher cap to avoid truncation.
    payload.insert("max_output_tokens", 1500);

    const QByteArray body = QJsonDocument(payload).toJson(QJsonDocument::Compact);

    requestTimer.start();
    QNetworkReply *reply = nam->post(req, body);
    qInfo() << "AiCoachClient: POST sent (bytes:" << body.size() << ", reply=" << reply << ")";

    // Track streaming parse state per request.
    struct ReplyState
    {
        QByteArray buffer;
        QString accumulated;
        QString streamError;
        bool sawAnyDelta = false;
        bool sawSse = false;
    };
    auto state = QSharedPointer<ReplyState>::create();

    auto handleJsonFallback = [this, reply](const QByteArray &bytes) {
        QJsonParseError parseError{};
        const QJsonDocument doc = QJsonDocument::fromJson(bytes, &parseError);
        if (!doc.isObject()) {
            emit recommendationError(QStringLiteral("Unexpected response (not JSON object)"));
            return;
        }

        const QJsonObject root = doc.object();
        const QString apiError = extractErrorMessage(root);
        if (!apiError.isEmpty()) {
            emit recommendationError(apiError);
            return;
        }

        const QString text = extractOutputText(root);
        if (text.isEmpty()) {
            emit recommendationError(QStringLiteral("No output_text found in response"));
            return;
        }
        emit recommendationReady(text);
    };

    connect(reply, &QNetworkReply::readyRead, this, [this, reply, state]() {
        const QByteArray chunk = reply->readAll();
        if (chunk.isEmpty()) {
            return;
        }

        // Detect SSE mode opportunistically.
        const QVariant ctAttr = reply->header(QNetworkRequest::ContentTypeHeader);
        if (ctAttr.isValid()) {
            const QString ct = ctAttr.toString();
            if (ct.contains(QStringLiteral("text/event-stream"), Qt::CaseInsensitive)) {
                state->sawSse = true;
            }
        }

        state->buffer.append(chunk);

        if (!state->sawSse) {
            // Not event-stream (or server didn't send CT yet); wait until finished.
            return;
        }

        const SseParseResult parsed = consumeSseFrames(&state->buffer);
        if (!parsed.error.isEmpty()) {
            state->streamError = parsed.error;
        }
        for (const QString &delta : parsed.deltas) {
            state->sawAnyDelta = true;
            state->accumulated += delta;
            emit recommendationDelta(delta);
        }
    });

    connect(reply, &QNetworkReply::finished, this, [this, reply, state, handleJsonFallback]() {
        const QVariant statusAttr = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute);
        const int httpStatus = statusAttr.isValid() ? statusAttr.toInt() : -1;
        const qint64 elapsedMs = requestTimer.isValid() ? requestTimer.elapsed() : -1;

        // Consume any remaining bytes.
        const QByteArray tail = reply->readAll();
        if (!tail.isEmpty()) {
            state->buffer.append(tail);
        }

        if (state->sawSse) {
            const SseParseResult parsed = consumeSseFrames(&state->buffer);
            if (!parsed.error.isEmpty()) {
                state->streamError = parsed.error;
            }
            for (const QString &delta : parsed.deltas) {
                state->sawAnyDelta = true;
                state->accumulated += delta;
                emit recommendationDelta(delta);
            }
        }

        if (reply->error() != QNetworkReply::NoError) {
            qInfo() << "AiCoachClient: reply finished with network error (httpStatus:" << httpStatus
                    << ", elapsedMs:" << elapsedMs << ", error:" << reply->error() << ")";
            const QString msg = state->streamError.isEmpty() ? reply->errorString() : state->streamError;
            reply->deleteLater();
            emit recommendationError(msg);
            return;
        }

        qInfo() << "AiCoachClient: reply finished (httpStatus:" << httpStatus << ", elapsedMs:" << elapsedMs
                << ", streaming:" << state->sawSse << ")";

        if (!state->streamError.isEmpty()) {
            reply->deleteLater();
            emit recommendationError(state->streamError);
            return;
        }

        if (state->sawAnyDelta) {
            const QString text = state->accumulated.trimmed();
            reply->deleteLater();
            if (text.isEmpty()) {
                emit recommendationError(QStringLiteral("Streaming response contained no output_text"));
            } else {
                emit recommendationReady(text);
            }
            return;
        }

        // Fallback: server didn't stream; parse full JSON response.
        const QByteArray allBytes = state->buffer;
        reply->deleteLater();
        handleJsonFallback(allBytes);
    });
}
