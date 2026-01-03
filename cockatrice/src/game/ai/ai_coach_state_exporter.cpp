#include "ai_coach_state_exporter.h"

#include "../abstract_game.h"
#include "../board/translate_counter_name.h"
#include "../game_state.h"
#include "../player/player.h"
#include "../player/player_info.h"
#include "../player/player_manager.h"
#include "../zones/logic/card_zone_logic.h"
#include "../board/card_item.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>

namespace {
QString stripTimestampPrefix(QString line)
{
    static const QRegularExpression tsRe(QStringLiteral("^\\[[0-9]{2}:[0-9]{2}:[0-9]{2}\\]\\s*"));
    line.remove(tsRe);
    return line.trimmed();
}

struct MulliganPhaseInfo
{
    bool isInitialMulliganPhase = false;
    int newHandEvents = 0;
    int recommendedKeepSize = 7;
};

MulliganPhaseInfo analyzeMulliganPhase(const QString &messageHistoryText)
{
    MulliganPhaseInfo info;

    const QStringList lines = messageHistoryText.split('\n');

    static const QRegularExpression newHandRe(
        QStringLiteral("shuffles their deck and draws a new hand( of (\\d+))? card\\(s\\)\\.?$"),
        QRegularExpression::CaseInsensitiveOption);

    // Allowlist of "startup/mulligan context" lines.
    static const QList<QRegularExpression> allowedLineRes = {
        QRegularExpression(QStringLiteral("^The game has started\\.?$"), QRegularExpression::CaseInsensitiveOption),
        QRegularExpression(QStringLiteral("^Player\\s+\\d+'s\\s+turn\\.?$"), QRegularExpression::CaseInsensitiveOption),
        QRegularExpression(QStringLiteral("^(Untap|Upkeep|Draw|First Main Phase|Start Combat|Attack|Block|Damage|End Combat|Second Main Phase|End)$"),
                          QRegularExpression::CaseInsensitiveOption),
        QRegularExpression(QStringLiteral("^Player\\s+\\d+\\s+is looking at their sideboard\\.?$"),
                          QRegularExpression::CaseInsensitiveOption),
        QRegularExpression(QStringLiteral("^Player\\s+\\d+\\s+sets counter\\s+.+\\s+to\\s+[-0-9]+\\s+\\(.+\\)\\.?$"),
                          QRegularExpression::CaseInsensitiveOption),
        QRegularExpression(QStringLiteral("^Player\\s+\\d+\\s+sets counter\\s+.+\\s+to\\s+[-0-9]+\\.?$"),
                          QRegularExpression::CaseInsensitiveOption),
        QRegularExpression(QStringLiteral("^Player\\s+\\d+\\s+shuffles their deck and draws a new hand( of (\\d+))? card\\(s\\)\\.?$"),
                          QRegularExpression::CaseInsensitiveOption),
    };

    bool sawAnyNonEmpty = false;
    bool sawDisallowed = false;

    for (QString line : lines) {
        line = stripTimestampPrefix(std::move(line));
        if (line.isEmpty()) {
            continue;
        }
        sawAnyNonEmpty = true;

        const QRegularExpressionMatch mh = newHandRe.match(line);
        if (mh.hasMatch()) {
            info.newHandEvents++;
        }

        bool allowed = false;
        for (const auto &re : allowedLineRes) {
            if (re.match(line).hasMatch()) {
                allowed = true;
                break;
            }
        }
        if (!allowed) {
            sawDisallowed = true;
        }
    }

    // Consider it mulligan phase only if:
    // - we have at least one "new hand" event AND
    // - there are no other "non-startup" actions in history.
    info.isInitialMulliganPhase = sawAnyNonEmpty && (info.newHandEvents >= 1) && !sawDisallowed;

    // Commander mulligan:
    // - Opening hand: 7
    // - 1 free mulligan: draw 7, keep 7
    // - subsequent mulligans: draw 7, keep one fewer each time
    // If message history shows N "new hand" events, then:
    // - N=1 => no mulligan yet, keep 7
    // - N=2 => 1 mulligan (free), keep 7
    // - N=3 => 2 mulligans, keep 6
    // - N=4 => 3 mulligans, keep 5
    const int keepPenalty = qMax(0, info.newHandEvents - 2);
    info.recommendedKeepSize = 7 - keepPenalty;

    return info;
}

QJsonObject exportCard(const CardItem *card)
{
    QJsonObject obj;

    obj.insert("id", card->getId());
    obj.insert("name", card->getName());
    obj.insert("provider_id", card->getProviderId());
    obj.insert("tapped", card->getTapped());
    obj.insert("face_down", card->getFaceDown());
    obj.insert("attacking", card->getAttacking());

    if (!card->getAnnotation().isEmpty()) {
        obj.insert("annotation", card->getAnnotation());
    }
    if (!card->getPT().isEmpty()) {
        obj.insert("pt", card->getPT());
    }

    if (!card->getCounters().isEmpty()) {
        QJsonArray counters;
        for (auto it = card->getCounters().cbegin(); it != card->getCounters().cend(); ++it) {
            QJsonObject c;
            c.insert("id", it.key());
            c.insert("value", it.value());
            counters.append(c);
        }
        obj.insert("counters", counters);
    }

    if (!card->getAttachedCards().isEmpty()) {
        QJsonArray attached;
        for (const CardItem *a : card->getAttachedCards()) {
            QJsonObject ref;
            ref.insert("id", a->getId());
            ref.insert("name", a->getName());
            attached.append(ref);
        }
        obj.insert("attached_cards", attached);
    }

    if (card->getAttachedTo()) {
        QJsonObject ref;
        ref.insert("id", card->getAttachedTo()->getId());
        ref.insert("name", card->getAttachedTo()->getName());
        obj.insert("attached_to", ref);
    }

    return obj;
}

QJsonObject exportDumpedCard(const AiCoachStateExporter::DumpedCard &card)
{
    QJsonObject obj;
    obj.insert("id", card.id);
    obj.insert("name", card.name);
    obj.insert("provider_id", card.providerId);
    return obj;
}

QJsonObject exportZone(const CardZoneLogic *zone,
                       bool includeCardsEvenIfHidden,
                       const QList<AiCoachStateExporter::DumpedCard> *overrideCards)
{
    QJsonObject obj;
    obj.insert("name", zone->getName());

    // For goldfishing/local games, we want the LLM to see full zone contents even if the UI considers the
    // zone hidden. Since AI Coach is only enabled for local games, this is safe and is essential for
    // opening hand and line planning.
    const bool contentsKnownForExport = includeCardsEvenIfHidden ? true : zone->contentsKnown();
    obj.insert("contents_known", contentsKnownForExport);

    const CardList &cards = zone->getCards();
    const int effectiveCount = (overrideCards != nullptr) ? overrideCards->size() : cards.size();
    obj.insert("count", effectiveCount);

    if (contentsKnownForExport) {
        QJsonArray cardArray;
        if (overrideCards != nullptr) {
            for (const auto &c : *overrideCards) {
                cardArray.append(exportDumpedCard(c));
            }
        } else {
            for (const CardItem *card : cards) {
                cardArray.append(exportCard(card));
            }
        }
        obj.insert("cards", cardArray);
    }

    return obj;
}

QJsonObject exportPlayerCounters(Player *player)
{
    QJsonObject obj;

    const QMap<int, AbstractCounter *> counters = player->getCounters();
    for (auto it = counters.cbegin(); it != counters.cend(); ++it) {
        const AbstractCounter *counter = it.value();
        if (!counter) {
            continue;
        }
        const QString rawName = counter->getName();
        const QString displayName = TranslateCounterName::getDisplayName(rawName);

        QJsonObject c;
        c.insert("id", it.key());
        c.insert("name", rawName);
        c.insert("display_name", displayName);
        c.insert("value", counter->getValue());

        obj.insert(rawName, c);
    }

    return obj;
}

QJsonObject exportPlayer(AbstractGame *game,
                         Player *player,
                         bool isPerspective,
                         const AiCoachStateExporter::ZoneCardOverrides &zoneCardOverrides)
{
    QJsonObject obj;
    obj.insert("id", player->getPlayerInfo()->getId());
    obj.insert("name", player->getPlayerInfo()->getName());
    obj.insert("is_perspective", isPerspective);
    obj.insert("is_local", player->getPlayerInfo()->getLocalOrJudge());

    // Counters include mana counters (w/u/b/r/g/x) and life.
    obj.insert("counters", exportPlayerCounters(player));

    QJsonArray zones;

    const bool isLocalGame = (game && game->getGameState() && game->getGameState()->getIsLocalGame());
    const bool includeHiddenZones = isLocalGame && isPerspective;

    const auto &zoneMap = player->getZones();
    for (auto it = zoneMap.cbegin(); it != zoneMap.cend(); ++it) {
        const QString zoneName = it.key();
        const CardZoneLogic *zone = it.value();
        if (!zone) {
            continue;
        }

        const QList<AiCoachStateExporter::DumpedCard> *overrideCards = nullptr;
        if (includeHiddenZones) {
            const auto overrideIt = zoneCardOverrides.constFind(zoneName);
            if (overrideIt != zoneCardOverrides.constEnd()) {
                overrideCards = &overrideIt.value();
            }
        }

        // Respect visibility for non-local games; in goldfishing/local games include full card lists for the
        // perspective player across all zones (deck/library, sideboard/command proxy, etc.).
        zones.append(exportZone(zone, includeHiddenZones, overrideCards));
    }
    obj.insert("zones", zones);

    Q_UNUSED(game);
    return obj;
}
} // namespace

QString AiCoachStateExporter::exportStateJson(AbstractGame *game,
                                              Player *perspectivePlayer,
                                              const QString &messageHistoryText)
{
    return exportStateJson(game, perspectivePlayer, messageHistoryText, ZoneCardOverrides());
}

QString AiCoachStateExporter::exportStateJson(AbstractGame *game,
                                              Player *perspectivePlayer,
                                              const QString &messageHistoryText,
                                              const ZoneCardOverrides &zoneCardOverrides)
{
    QJsonObject root;
    root.insert("schema", "cockatrice.ai_state.v1");

    if (game && game->getGameState()) {
        root.insert("is_local_game", game->getGameState()->getIsLocalGame());
        root.insert("active_player_id", game->getGameState()->getActivePlayer());
        root.insert("current_phase", game->getGameState()->getCurrentPhase());
    }

    if (perspectivePlayer && perspectivePlayer->getPlayerInfo()) {
        root.insert("perspective_player_id", perspectivePlayer->getPlayerInfo()->getId());
    }

    // Message history is included as context for mulligan / opening decisions.
    // Keep it bounded to avoid huge prompts.
    const int maxHistoryChars = 24000;
    QString history = messageHistoryText;
    bool truncated = false;
    if (history.size() > maxHistoryChars) {
        history = history.right(maxHistoryChars);
        truncated = true;
    }
    root.insert("message_history_text", history);
    root.insert("message_history_truncated", truncated);

    const MulliganPhaseInfo mulligan = analyzeMulliganPhase(history);
    QJsonObject mulliganObj;
    mulliganObj.insert("is_initial_mulligan_phase", mulligan.isInitialMulliganPhase);
    mulliganObj.insert("new_hand_events", mulligan.newHandEvents);
    mulliganObj.insert("commander_recommended_keep_size", mulligan.recommendedKeepSize);
    root.insert("mulligan", mulliganObj);

    QJsonArray players;
    if (game && game->getPlayerManager()) {
        const auto &playerMap = game->getPlayerManager()->getPlayers();
        for (auto it = playerMap.cbegin(); it != playerMap.cend(); ++it) {
            Player *p = it.value();
            if (!p) {
                continue;
            }
            const bool isPerspective = (p == perspectivePlayer);
            players.append(exportPlayer(game, p, isPerspective, zoneCardOverrides));
        }
    }
    root.insert("players", players);

    // Pretty-print for readability in the prompt and trace log.
    return QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Indented));
}

QString AiCoachStateExporter::buildPromptText(AbstractGame *game,
                                              Player *perspectivePlayer,
                                              const QString &messageHistoryText)
{
    return buildPromptText(game, perspectivePlayer, messageHistoryText, ZoneCardOverrides());
}

QString AiCoachStateExporter::buildPromptText(AbstractGame *game,
                                              Player *perspectivePlayer,
                                              const QString &messageHistoryText,
                                              const ZoneCardOverrides &zoneCardOverrides)
{
    const QString json = exportStateJson(game, perspectivePlayer, messageHistoryText, zoneCardOverrides);

    const MulliganPhaseInfo mulligan = analyzeMulliganPhase(messageHistoryText);

    // Keep this prompt short and very explicit; the JSON is the important payload.
    QString prompt;
    prompt += "You are an expert Magic: The Gathering Commander goldfishing coach.\n";
    prompt += "Given the following Cockatrice game state JSON, recommend the best next play sequence for the perspective player.\n";
    prompt += "Prioritize deterministic lines and maximizing chance to win; if info is missing, state assumptions.\n\n";

    prompt += "Important: The JSON includes message_history_text (the in-game log).\n";
    prompt += "If the message history contains only startup/mulligan actions such as:\n";
    prompt += "- The game has started\n";
    prompt += "- Player N's turn / phase labels (e.g. Untap)\n";
    prompt += "- looking at sideboard\n";
    prompt += "- setting counters (including Life and mana counters like Blue/White/etc)\n";
    prompt += "- \"shuffles their deck and draws a new hand of 7 card(s)\" (possibly repeated)\n";
    prompt += "and there are no other actions, then treat this as the initial mulliganing phase.\n";
    prompt += "In that case, your primary task is: decide whether to keep the current hand and how to proceed.\n";
    prompt += "Commander mulligan rule (goldfishing): 1 free mulligan. After that, you always draw 7 but keep one fewer each time.\n";
    prompt += "Examples: 1 mulligan => keep 7; 2 mulligans => keep 6; 3 mulligans => keep 5.\n";
    if (mulligan.isInitialMulliganPhase) {
        prompt += QString("Heuristic: message history suggests mulligan phase with %1 new-hand event(s); keep size would be %2.\n\n")
                      .arg(mulligan.newHandEvents)
                      .arg(mulligan.recommendedKeepSize);
    } else {
        prompt += "\n";
    }
    prompt += "Output format:\n";
    prompt += "1) Recommended line (bullet steps)\n";
    prompt += "2) Why this line\n";
    prompt += "3) Alternative line(s)\n";
    prompt += "4) Key risks / missing info\n\n";
    prompt += "Game state JSON:\n";
    prompt += json;

    return prompt;
}
