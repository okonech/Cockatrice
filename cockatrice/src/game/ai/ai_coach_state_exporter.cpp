#include "ai_coach_state_exporter.h"

#include "../abstract_game.h"
#include "../board/translate_counter_name.h"
#include "../game_state.h"
#include "../player/player.h"
#include "../player/player_info.h"
#include "../player/player_manager.h"
#include "../zones/logic/card_zone_logic.h"
#include "../board/card_item.h"

#include <libcockatrice/card/card_info.h>
#include <libcockatrice/card/database/card_database_manager.h>

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

QString zoneNameForOutput(const QString &rawZoneName)
{
    if (rawZoneName == QStringLiteral("deck")) {
        return QStringLiteral("Library");
    }
    if (rawZoneName == QStringLiteral("table")) {
        return QStringLiteral("Battlefield");
    }
    if (rawZoneName == QStringLiteral("hand")) {
        return QStringLiteral("Hand");
    }
    if (rawZoneName == QStringLiteral("grave")) {
        return QStringLiteral("Graveyard");
    }
    if (rawZoneName == QStringLiteral("rfg")) {
        return QStringLiteral("Exile");
    }
    if (rawZoneName == QStringLiteral("sb")) {
        return QStringLiteral("Command Zone");
    }
    if (rawZoneName == QStringLiteral("stack")) {
        return QStringLiteral("Stack");
    }
    return rawZoneName;
}

bool isBattlefieldZone(const QString &rawZoneName)
{
    return rawZoneName == QStringLiteral("table");
}

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

QJsonObject exportZoneValue(const CardZoneLogic *zone,
                            bool includeCardsEvenIfHidden,
                            const QList<AiCoachStateExporter::DumpedCard> *overrideCards)
{
    QJsonObject obj;
    const QString rawZoneName = zone->getName();
    const bool battlefield = isBattlefieldZone(rawZoneName);

    // For goldfishing/local games, we want the LLM to see full zone contents even if the UI considers the
    // zone hidden. Since AI Coach is only enabled for local games, this is safe and is essential for
    // opening hand and line planning.
    const bool contentsKnownForExport = includeCardsEvenIfHidden ? true : zone->contentsKnown();

    const CardList &cards = zone->getCards();
    const int effectiveCount = (overrideCards != nullptr) ? overrideCards->size() : cards.size();
    obj.insert("count", effectiveCount);

    if (contentsKnownForExport) {
        QJsonArray cardArray;
        if (battlefield) {
            for (const CardItem *card : cards) {
                cardArray.append(exportCard(card));
            }
        } else {
            if (overrideCards != nullptr) {
                for (const auto &c : *overrideCards) {
                    cardArray.append(c.name);
                }
            } else {
                for (const CardItem *card : cards) {
                    cardArray.append(card->getName());
                }
            }
        }
        obj.insert("cards", cardArray);
    }

    return obj;
}

struct ExportedPlayerResources
{
    QJsonObject floatingMana;
    QJsonObject otherCounters;
    bool hasLife = false;
    int life = 0;
    bool hasStorm = false;
    int storm = 0;
};

ExportedPlayerResources exportPlayerResources(Player *player)
{
    ExportedPlayerResources out;

    const QMap<int, AbstractCounter *> counters = player->getCounters();
    for (auto it = counters.cbegin(); it != counters.cend(); ++it) {
        const AbstractCounter *counter = it.value();
        if (!counter) {
            continue;
        }

        const QString rawName = counter->getName();
        const int value = counter->getValue();

        if (rawName == QStringLiteral("life")) {
            out.hasLife = true;
            out.life = value;
            continue;
        }
        if (rawName == QStringLiteral("storm")) {
            out.hasStorm = true;
            out.storm = value;
            continue;
        }

        const bool isFloatingMana = (rawName == QStringLiteral("w") || rawName == QStringLiteral("u") ||
                                    rawName == QStringLiteral("b") || rawName == QStringLiteral("r") ||
                                    rawName == QStringLiteral("g") || rawName == QStringLiteral("x"));
        if (isFloatingMana) {
            QString key = TranslateCounterName::getDisplayName(rawName);
            if (key.isEmpty() || key == QStringLiteral("Other")) {
                key = rawName;
            }
            if (!key.isEmpty()) {
                key[0] = key[0].toUpper();
            }
            if (!key.isEmpty()) {
                out.floatingMana.insert(key, value);
            }
            continue;
        }

        QString key = TranslateCounterName::getDisplayName(rawName);
        if (key.isEmpty() || key == QStringLiteral("Other")) {
            key = rawName;
        }
        if (!key.isEmpty()) {
            key[0] = key[0].toUpper();
        }
        if (!key.isEmpty()) {
            // Avoid collisions; fall back to raw name if needed.
            if (out.otherCounters.contains(key) && !rawName.isEmpty()) {
                out.otherCounters.insert(rawName, value);
            } else {
                out.otherCounters.insert(key, value);
            }
        }
    }

    return out;
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

    // Export a simplified view that is easy for the LLM to reason about.
    const ExportedPlayerResources resources = exportPlayerResources(player);
    obj.insert("floating_mana", resources.floatingMana);
    if (resources.hasLife) {
        obj.insert("life", resources.life);
    }
    if (resources.hasStorm) {
        obj.insert("storm", resources.storm);
    }
    if (!resources.otherCounters.isEmpty()) {
        obj.insert("other_counters", resources.otherCounters);
    }

    QJsonObject zones;

    const bool isLocalGame = (game && game->getGameState() && game->getGameState()->getIsLocalGame());
    const bool includeHiddenZones = isLocalGame && isPerspective;

    const auto &zoneMap = player->getZones();
    for (auto it = zoneMap.cbegin(); it != zoneMap.cend(); ++it) {
        const QString zoneName = it.key();
        const QString outputZoneName = zoneNameForOutput(zoneName);
        const CardZoneLogic *zone = it.value();
        if (!zone) {
            continue;
        }

        const QList<AiCoachStateExporter::DumpedCard> *overrideCards = nullptr;
        if (includeHiddenZones) {
            const auto overrideIt = zoneCardOverrides.constFind(zoneName);
            if (overrideIt != zoneCardOverrides.constEnd()) {
                overrideCards = &overrideIt.value();
            } else {
                // If the caller stored overrides under a human-friendly renamed zone name,
                // accept that too.
                const auto renamedIt = zoneCardOverrides.constFind(outputZoneName);
                if (renamedIt != zoneCardOverrides.constEnd()) {
                    overrideCards = &renamedIt.value();
                }
            }
        }

        // Respect visibility for non-local games; in goldfishing/local games include full card lists for the
        // perspective player across all zones (deck/library, sideboard/command proxy, etc.).
        zones.insert(outputZoneName, exportZoneValue(zone, includeHiddenZones, overrideCards));
    }
    obj.insert("zones", zones);

    Q_UNUSED(game);
    return obj;
}

QJsonObject exportTurnContext(const QString &messageHistoryText)
{
    QJsonObject obj;

    const QStringList lines = messageHistoryText.split('\n');

    static const QRegularExpression turnStartRe(
        QStringLiteral("^(Player\\s+\\d+)'s\\s+turn\\.?$"),
        QRegularExpression::CaseInsensitiveOption);

    int lastTurnLineIndex = -1;
    QString currentTurnPlayer;

    for (int i = 0; i < lines.size(); ++i) {
        const QString stripped = stripTimestampPrefix(lines[i]);
        const QRegularExpressionMatch m = turnStartRe.match(stripped);
        if (m.hasMatch()) {
            lastTurnLineIndex = i;
            currentTurnPlayer = m.captured(1);
        }
    }

    if (lastTurnLineIndex < 0 || currentTurnPlayer.isEmpty()) {
        obj.insert("current_turn_player", QString());
        obj.insert("current_turn_log_text", QString());
        obj.insert("land_drop_made", false);
        obj.insert("land_drop_cards", QJsonArray());
        return obj;
    }

    obj.insert("current_turn_player", currentTurnPlayer);

    QStringList turnLines;
    turnLines.reserve(qMax(0, lines.size() - (lastTurnLineIndex + 1)));
    for (int i = lastTurnLineIndex + 1; i < lines.size(); ++i) {
        const QString stripped = stripTimestampPrefix(lines[i]);
        if (!stripped.isEmpty()) {
            turnLines.append(stripped);
        }
    }
    obj.insert("current_turn_log_text", turnLines.join('\n'));

    const QRegularExpression landDropRe(
        QStringLiteral("^%1\\s+puts\\s+(.+)\\s+into\\s+play\\s+from\\s+their\\s+hand\\.?$")
            .arg(QRegularExpression::escape(currentTurnPlayer)),
        QRegularExpression::CaseInsensitiveOption);

    auto isLandCardName = [](const QString &cardName) -> bool {
        auto *q = CardDatabaseManager::query();
        if (!q) {
            return false;
        }
        const CardInfoPtr info = q->getCardInfo(cardName);
        if (!info) {
            return false;
        }
        const QString typeLine = info->getCardType();
        return typeLine.contains(QStringLiteral("Land"), Qt::CaseInsensitive);
    };

    QJsonArray landDropCards;
    for (const QString &line : turnLines) {
        const QRegularExpressionMatch m = landDropRe.match(line);
        if (m.hasMatch()) {
            const QString playedName = m.captured(1).trimmed();
            if (isLandCardName(playedName)) {
                landDropCards.append(playedName);
            }
        }
    }

    obj.insert("land_drop_cards", landDropCards);
    obj.insert("land_drop_made", !landDropCards.isEmpty());

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

    // Derived helpers from the message log to make turn/land-drop reasoning easier.
    root.insert("turn_context", exportTurnContext(history));

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

    // Prompt engineering for GPT 5.2 / Azure OpenAI Responses API.
    // Structured to elicit careful reasoning about mulligan decisions and play sequencing.
    QString prompt;
    prompt += "You are an expert Magic: The Gathering cEDH (competitive Commander) goldfishing coach.\n";
    prompt += "Analyze the Cockatrice game state JSON below and provide strategic guidance for the perspective player.\n";
    prompt += "This is GOLDFISHING (solitaire practice), so there are no opponents to interact with—focus on speed and consistency.\n\n";

    prompt += "=== CRITICAL: CARD KNOWLEDGE ===\n";
    prompt += "If you encounter a card you do not recognize or are uncertain about, explicitly say:\n";
    prompt += "  \"I am not familiar with [Card Name]. Please provide its Oracle text.\"\n";
    prompt += "DO NOT guess or hallucinate card abilities. Accuracy is paramount.\n\n";

    prompt += "=== RULES / LEGALITY ===\n";
    prompt += "Do not recommend illegal actions. Respect timing restrictions, costs, and rules text.\n";
    prompt += "If the legality depends on missing info (e.g., commander tax, cards' Oracle text, whether a permanent is untapped, etc.), call it out and ask.\n";
    prompt += "Only use information present in the JSON; do not assume extra cards, mana sources, or effects not shown.\n\n";

    prompt += "=== GAME LOG INTERPRETATION ===\n";
    prompt += "The JSON includes message_history_text (the in-game log).\n";
    prompt += "- \"Player N's turn.\" marks a turn boundary.\n";
    prompt += "- \"Player N puts <Card> into play from their hand.\" indicates a land drop.\n";
    prompt += "- Startup lines (game started, phase labels, looking at sideboard, setting counters, shuffles/draws) are mulligan context.\n";
    prompt += "- If only startup/mulligan lines exist, treat this as the MULLIGAN DECISION phase.\n\n";

    prompt += "=== COMMANDER MULLIGAN RULES (Goldfishing) ===\n";
    prompt += "- 1 free mulligan (draw 7, keep 7).\n";
    prompt += "- After that: draw 7, keep one fewer each time (2nd mull => keep 6, 3rd => keep 5, etc.).\n";
    prompt += QString("- Heuristic hints: is_initial_mulligan_phase=%1, new_hand_events=%2, recommended_keep_size=%3.\n\n")
                  .arg(mulligan.isInitialMulliganPhase ? QStringLiteral("true") : QStringLiteral("false"))
                  .arg(mulligan.newHandEvents)
                  .arg(mulligan.recommendedKeepSize);

    prompt += "=== MODE SELECTION (IMPORTANT) ===\n";
    prompt += "First, decide which mode applies and state it explicitly:\n";
    prompt += "- **MULLIGAN MODE** if we're still deciding to keep an opening hand (even if the heuristic is wrong).\n";
    prompt += "- **PLAY MODE** if the game has progressed and we are sequencing plays.\n";
    prompt += "Use the heuristic plus sanity checks (battlefield empty, only startup actions in log, etc.).\n\n";

    prompt += "=== MULLIGAN MODE: EVALUATION FRAMEWORK ===\n";
    prompt += "Evaluate the hand using these criteria (priority order for cEDH goldfishing):\n";
    prompt += "1) Mana sources (lands + fast mana/rituals): do you have at least one reliable way to cast spells?\n";
    prompt += "2) Early acceleration: can you do something meaningful on T1-T2?\n";
    prompt += "3) Card advantage/selection: can you find missing pieces (tutors/draw/wheels)?\n";
    prompt += "4) Win access: do you have a clear win attempt or tutors toward one?\n";
    prompt += "5) Commander synergy: does the hand leverage commanders as part of the plan?\n";
    prompt += "Then decide KEEP or MULLIGAN and explain what the hand is working toward (mana development, card advantage, win attempt, etc.).\n\n";

    prompt += "=== PLAY MODE: SEQUENCING FRAMEWORK ===\n";
    prompt += "Determine the optimal line using:\n";
    prompt += "1) Immediate goal this turn (develop mana / draw / tutor / win).\n";
    prompt += "2) Resource assessment (lands, rocks, floating mana, storm, hand).\n";
    prompt += "3) Sequencing: order matters (land drop, rocks, draw, tutors).\n";
    prompt += "4) Win attempt: if a win line exists, write it step-by-step with exact mana accounting.\n";
    prompt += "5) If no win line now: best setup for next turn.\n\n";

    prompt += "=== OUTPUT FORMAT (USE ONLY THE CHOSEN MODE) ===\n";
    prompt += "If **MULLIGAN MODE**:\n";
    prompt += "- Hand analysis: mana sources; acceleration; card advantage; win access; commander synergy\n";
    prompt += "- Decision: KEEP or MULLIGAN\n";
    prompt += "- Reasoning: careful explanation of why, and what the plan is working toward\n";
    prompt += "- If KEEP: first 1-2 turns plan (with mana math)\n";
    prompt += "- If MULLIGAN: what to look for next hand\n";
    prompt += "- Alternatives: only if there is a plausible keep line\n";
    prompt += "- Key risks / missing info\n\n";
    prompt += "If **PLAY MODE**:\n";
    prompt += "- Situation assessment (turn/phase/resources/hand/board)\n";
    prompt += "- Recommended line: numbered steps with mana spent/remaining\n";
    prompt += "- Goal: what the line accomplishes\n";
    prompt += "- Alternative line(s) and why\n";
    prompt += "- Key risks / missing info\n\n";

    prompt += "=== GAME STATE JSON ===\n";
    prompt += json;

    return prompt;
}
