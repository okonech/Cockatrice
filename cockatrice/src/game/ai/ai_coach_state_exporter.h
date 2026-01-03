#ifndef COCKATRICE_AI_COACH_STATE_EXPORTER_H
#define COCKATRICE_AI_COACH_STATE_EXPORTER_H

#include <QList>
#include <QMap>
#include <QString>

class AbstractGame;
class Player;

class AiCoachStateExporter
{
public:
    struct DumpedCard
    {
        int id = -1;
        QString name;
        QString providerId;
    };

    using ZoneCardOverrides = QMap<QString, QList<DumpedCard>>;

    // Produces a stable, machine-readable JSON snapshot of state.
    static QString exportStateJson(AbstractGame *game, Player *perspectivePlayer, const QString &messageHistoryText);

    // Like exportStateJson, but allows overriding hidden zone card lists (e.g. deck/sideboard) with
    // data returned by Command_DumpZone / Response_DumpZone.
    static QString exportStateJson(AbstractGame *game,
                                  Player *perspectivePlayer,
                                  const QString &messageHistoryText,
                                  const ZoneCardOverrides &zoneCardOverrides);

    // Produces the full prompt text that will be sent to the LLM.
    static QString buildPromptText(AbstractGame *game,
                                   Player *perspectivePlayer,
                                   const QString &messageHistoryText);

    static QString buildPromptText(AbstractGame *game,
                                   Player *perspectivePlayer,
                                   const QString &messageHistoryText,
                                   const ZoneCardOverrides &zoneCardOverrides);
};

#endif // COCKATRICE_AI_COACH_STATE_EXPORTER_H
