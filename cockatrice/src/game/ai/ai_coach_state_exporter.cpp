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
    int keepSize = 7;
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
    info.keepSize = qBound(1, 7 - keepPenalty, 7);

    return info;
}

QJsonObject exportCard(const CardItem *card)
{
    QJsonObject obj;

    const QString cardName = card->getName();

    CardInfoPtr info;
    {
        auto *q = CardDatabaseManager::query();
        info = q ? q->getCardInfo(cardName) : CardInfoPtr();
    }

    // Card database fields (for rules accuracy).
    obj.insert("name", cardName);
    if (info) {
        obj.insert("Mana Cost", info->getManaCost());
        obj.insert("Type", info->getCardType());
        obj.insert("Text", info->getText());
    } else {
        obj.insert("unknown_card", true);
        obj.insert("Mana Cost", QJsonValue());
        obj.insert("Type", QJsonValue());
        obj.insert("Text", QJsonValue());
    }

    obj.insert("tapped", card->getTapped());

    if (!card->getAnnotation().isEmpty()) {
        obj.insert("annotation", card->getAnnotation());
    }
    if (!card->getPT().isEmpty()) {
        bool isCreature = false;
        if (info) {
            isCreature = info->getCardType().contains("Creature", Qt::CaseInsensitive);
        }
        if (isCreature) {
            obj.insert("pt", card->getPT());
        }
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
            ref.insert("name", a->getName());
            attached.append(ref);
        }
        obj.insert("attached_cards", attached);
    }

    if (card->getAttachedTo()) {
        QJsonObject ref;
        ref.insert("name", card->getAttachedTo()->getName());
        obj.insert("attached_to", ref);
    }

    return obj;
}

QJsonObject exportCardInfoOnly(const QString &cardName)
{
    QJsonObject obj;
    obj.insert("name", cardName);

    auto *q = CardDatabaseManager::query();
    const CardInfoPtr info = q ? q->getCardInfo(cardName) : CardInfoPtr();
    if (!info) {
        obj.insert("unknown_card", true);
        obj.insert("Mana Cost", QJsonValue());
        obj.insert("Type", QJsonValue());
        obj.insert("Text", QJsonValue());
        return obj;
    }

    obj.insert("Mana Cost", info->getManaCost());
    obj.insert("Type", info->getCardType());
    obj.insert("Text", info->getText());
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
                    cardArray.append(exportCardInfoOnly(c.name));
                }
            } else {
                for (const CardItem *card : cards) {
                    cardArray.append(exportCardInfoOnly(card->getName()));
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

QJsonObject computePlayerState(Player *player)
{
    QJsonObject state;
    int treasureCount = 0;
    QStringList untappedLands;
    QStringList untappedArtifacts;

    const CardZoneLogic *table = player->getZones().value("table");
    if (table) {
        for (const CardItem *card : table->getCards()) {
            if (card->getName() == "Treasure") {
                treasureCount++;
            }

            if (!card->getTapped()) {
                auto *q = CardDatabaseManager::query();
                const CardInfoPtr info = q ? q->getCardInfo(card->getName()) : CardInfoPtr();
                if (info) {
                    const QString type = info->getCardType();
                    if (type.contains("Land", Qt::CaseInsensitive)) {
                        untappedLands.append(card->getName());
                    } else if (type.contains("Artifact", Qt::CaseInsensitive)) {
                        untappedArtifacts.append(card->getName());
                    }
                }
            }
        }
    }

    state.insert("treasure_count", treasureCount);
    state.insert("untapped_lands", QJsonArray::fromStringList(untappedLands));
    state.insert("untapped_artifacts", QJsonArray::fromStringList(untappedArtifacts));

    return state;
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

    if (isPerspective) {
        obj.insert("computed_state", computePlayerState(player));
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
    // Commander mulligan keep size after accounting for the free mulligan.
    // This is the number of cards the player must keep if this is a mulligan decision.
    mulliganObj.insert("keep_size", mulligan.keepSize);
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

    QString prompt;
    prompt += "You are a cEDH Logic Engine and Coach.\n";
    prompt += "Your goal is to find the optimal line of play given a Cockatrice game state.\n";
    prompt += "This is GOLDFISHING (solitaire practice), but we will use realistic cEDH assumptions to approximate a real pod.\n";
    prompt += "Assume opponents exist, interaction is likely, and sequencing a protected win is preferable to an unprotected win.\n";
    prompt += "You must adhere to strict Rules Enforcement Level (REL).\n\n";

    prompt += "=== GOLDFISH ASSUMPTIONS (Use for Evaluation) ===\n";
    prompt += "When giving recommendations, apply these simplifying assumptions unless the JSON contradicts them:\n";
    prompt += "- Plan as if you have opponents and will need to respect interaction; prioritize protected wins when possible.\n";
    prompt += "- Dockside Extortionist: assume it makes 3 Treasures in the first 3 turns of the game, then 4 Treasures after that.\n";
    prompt += "- Mystic Remora: assume it draws 3 cards on T1, 2 cards on T2, and 1 card each turn after that while it remains on the battlefield.\n";
    prompt += "- Rhystic Study: assume it draws 3 cards on T1, 2 cards on T2-T4, and 1 card each turn after that while it remains on the battlefield.\n";
    prompt += "- Ragavan, Nimble Pilferer and Praetor's Grasp: assume they can generate value because opponents have similar wincons and staples as our deck (treat the opponent pool as a typical cEDH pod).\n\n";

    prompt += "=== CRITICAL RULES & LOGIC ===\n";
    prompt += "1. **The Ledger is Law:** You cannot cast a spell unless you explicitly identify the untapped source in the \"Current State\" ledger.\n";
    prompt += "2. **Color Strictness:** You cannot spend {C} (Colorless) or {R} (Red) to pay for {U} (Blue) pips. Treat \"Mana Cost\" in JSON as authoritative.\n";
    prompt += "3. **State Updates:** Every action updates the ledger. You must track the mana pool AND hand contents step-by-step. If a card is discarded, it is gone.\n";
    prompt += "4. **Action Validity:** If you cannot pay for a spell, you cannot cast it. If no win line is mathematically possible, do not hallucinate one. Suggest the best defensive setup instead.\n";
    prompt += "5. **Counter Verification:** Do not assume a permanent has a counter (e.g., 'luck counter') unless it is explicitly listed in the 'counters' array of that card object.\n";
    prompt += "6. **Land Drops:** Check `turn_context.land_drop_made`. If false, you should generally play a land. If you play a Fetch Land, you MUST look at the 'Library' zone and specify exactly which land to fetch.\n";
    prompt += "7. **Card Knowledge:** If you do not recognize a card, ask for Oracle text. Do not guess.\n";
    prompt += "8. **Tutor Specificity:** If you cast a Tutor, you MUST specify exactly what card you are finding and WHY. Do not say 'find a wincon'. Say 'find Underworld Breach'.\n";
    prompt += "9. **Cost Consequences:** If you Sacrifice a permanent as a cost (e.g., Diabolic Intent), immediately update your mental state to reflect that it is gone. Check if this disables other cards (e.g., Fierce Guardianship requires a Commander).\n\n";

    prompt += "=== INPUT DATA EXPLANATION ===\n";
    prompt += "- \"tapped\": true means the permanent provides NO MANA.\n";
    prompt += "- \"floating_mana\": The mana currently in the pool before any actions are taken.\n";
    prompt += "- \"Hand\": Cards available to cast.\n";
    prompt += "- \"Graveyard\": Cards available for Escape/Flashback/Recursion.\n";
    prompt += "- \"Computed_State\": Pre-calculated counts of treasures and untapped lands/artifacts to help you avoid hallucinations.\n";
    prompt += "- \"message_history_text\": The in-game log. \"Player N's turn\" marks turn boundaries. Startup lines are mulligan context.\n\n";

    prompt += "=== COMMANDER MULLIGAN RULES (Goldfishing) ===\n";
    prompt += "- 1 free mulligan (draw 7, keep 7).\n";
    prompt += "- After that: draw 7, keep one fewer each time (2nd mull => keep 6, 3rd => keep 5, etc.).\n";
    prompt += "- Keep size sequence: 7, 7, 6, 5, 4, 3, 2, 1 …\n";
    prompt += QString("- Mulligan info: is_initial_mulligan_phase=%1, new_hand_events=%2, keep_size=%3 (authoritative).\n\n")
                  .arg(mulligan.isInitialMulliganPhase ? QStringLiteral("true") : QStringLiteral("false"))
                  .arg(mulligan.newHandEvents)
                  .arg(mulligan.keepSize);

    prompt += "=== MODE SELECTION ===\n";
    prompt += "First, decide which mode applies and state it explicitly:\n";
    prompt += "- **MULLIGAN MODE** if we're still deciding to keep an opening hand (even if the heuristic is wrong).\n";
    prompt += "- **PLAY MODE** if the game has progressed and we are sequencing plays.\n";
    prompt += "Use the heuristic plus sanity checks (battlefield empty, only startup actions in log, etc.).\n\n";

    prompt += "=== MULLIGAN MODE: EVALUATION FRAMEWORK ===\n";
    prompt += "Adopt the following philosophy verbatim:\n\n";
    prompt += "1. The Core Philosophy: \"Default to Mull\"\n";
    prompt += "In 60-card formats, you keep unless the hand is unplayable. In cEDH, Mulligan is the default action.\n";
    prompt += "- The 3-to-1 Problem: You must outperform three other starting hands. A \"fair\" hand will almost never be the best of four.\n";
    prompt += "- No Partial Credit: Having the \"second best\" game is the same as losing. You don't win by \"not losing\"; you win by doing something powerful.\n";
    prompt += "- The Power Gap: The disparity between your best 10 cards and your 50th best is massive. Mull to find the top 10%.\n\n";

    prompt += "2. Leverage Your \"Safety Nets\"\n";
    prompt += "- The First One is Free: Use the free mulligan aggressively to see 14 cards.\n";
    prompt += "- The Command Zone Offset: A mulligan to 5 still gives you access to 6 or 7 cards with commanders.\n";
    prompt += "- The Turn 1 Draw: Even at 4 cards, you'll have 7-8 cards available on T1.\n\n";

    prompt += "3. The Hierarchy of Keeping (Ranked)\n";
    prompt += "I. Truly Broken Things (The \"Final Destination\")\n";
    prompt += "   If your hand breaks parity (e.g., T1/T2 Rhystic/Remora, fast Ad Naus), KEEP regardless of deficiencies.\n";
    prompt += "II. Development (The \"Engine\")\n";
    prompt += "   Prioritize Mana Development. The \"Pants\" Rule: If you have spells but no fast mana, MULLIGAN.\n";
    prompt += "--- THE BREAKPOINT: Hands below this line are usually Mulligans on 7 ---\n";
    prompt += "III. Interaction (The \"Shared Responsibility\")\n";
    prompt += "   Don't be the \"Police\". If you have interaction but no engine, MULLIGAN.\n";
    prompt += "IV. Card Advantage (The \"Trap\")\n";
    prompt += "   Slow engines (Sylvan Library) without board development are a trap. MULLIGAN.\n";
    prompt += "V. Non-Functional-Hand Avoidance (The \"Fear\")\n";
    prompt += "   Do not keep a hand just because it \"works\" (lands + spells). Better to mull to a glass cannon 4 than a mediocre 7.\n\n";

    prompt += "4. Practical Steps\n";
    prompt += "- Look for a Coherent Plan: Can this hand win or establish a dominant engine by Turn 3?\n";
    prompt += "- The \"Greedy\" Test: Ask, \"Is a random 5-card hand from my deck more likely to win than this 7?\" If yes, throw it back.\n";
    prompt += "Then decide KEEP or MULLIGAN and explain what the hand is working toward.\n";
    prompt += "IMPORTANT: If you KEEP, you must choose EXACTLY keep_size cards to keep.\n\n";

    prompt += "=== PLAY MODE: SEQUENCING FRAMEWORK ===\n";
    prompt += "Determine the optimal line using:\n";
    prompt += "1) Immediate goal: Do something POWERFUL. Don't just 'not lose'.\n";
    prompt += "2) Resource assessment: Prioritize Mana Development (the primary bottleneck).\n";
    prompt += "3) Sequencing: order matters (land drop, rocks, draw, tutors).\n";
    prompt += "4) Win attempt: if a win line exists, write it step-by-step with exact mana accounting. If using a tutor, specify the target.\n";
    prompt += "5) If no win line now: best setup for next turn.\n\n";

    prompt += "=== FEW-SHOT EXAMPLES (LEARN FROM THESE) ===\n";
    prompt += "Study these examples to understand how to track the ledger and avoid hallucinations.\n\n";

    prompt += "### Example 1: The \"Color Screw\" (Respect Pips)\n";
    prompt += "**Input:** { \"Floating\": \"{C}{C}\", \"Hand\": [\"Demonic Tutor\"], \"Board\": [\"Swamp (Untapped)\"] }\n";
    prompt += "**Correct Output:**\n";
    prompt += "**Phase 1: State Audit**\n";
    prompt += "* Untapped Sources: Swamp ({B}).\n";
    prompt += "* Floating Mana: {C}{C}.\n";
    prompt += "**Phase 2: The Line**\n";
    prompt += "> **Step 1: Cast Demonic Tutor**\n";
    prompt += "> * Cost: {1}{B}\n";
    prompt += "> * Payment: Pay {1} with {C}. Pay {B} with Swamp.\n";
    prompt += "> * Mana Change: {C}{C} -> {C}\n";
    prompt += "**Phase 3: Strategic Summary**\n";
    prompt += "We have enough mana. Cast Demonic Tutor.\n\n";

    prompt += "### Example 2: The \"Empty Tank\" (Stop when empty)\n";
    prompt += "**Input:** { \"Floating\": \"{0}\", \"Hand\": [\"Underworld Breach\", \"Lion's Eye Diamond\"], \"Board\": [] }\n";
    prompt += "**Correct Output:**\n";
    prompt += "**Phase 1: State Audit**\n";
    prompt += "* Untapped Sources: None.\n";
    prompt += "**Phase 2: The Line**\n";
    prompt += "> **Step 1: Cast Lion's Eye Diamond**\n";
    prompt += "> * Cost Paid: {0}\n";
    prompt += "> * New Pool: {0}\n";
    prompt += "> **Step 2: Cast Underworld Breach**\n";
    prompt += "> * Cost: {1}{R}\n";
    prompt += "> * Current Pool: {0}\n";
    prompt += "> * RESULT: Impossible to cast Breach. Stop.\n";
    prompt += "**Phase 3: Strategic Summary**\n";
    prompt += "Cannot cast win condition. Pass turn.\n\n";

    prompt += "### Example 3: The \"Ritual Math\" (Ledger Updates)\n";
    prompt += "**Input:** { \"Floating\": \"{0}\", \"Hand\": [\"Dark Ritual\", \"Ad Nauseam\"], \"Board\": [\"Underground Sea (Untapped)\", \"Chrome Mox (Black, Untapped)\"] }\n";
    prompt += "**Correct Output:**\n";
    prompt += "**Phase 1: State Audit**\n";
    prompt += "* Untapped Sources: Underground Sea ({U}/{B}), Chrome Mox ({B}).\n";
    prompt += "**Phase 2: The Line**\n";
    prompt += "> **Step 1: Cast Dark Ritual**\n";
    prompt += "> * Source Used: Underground Sea ({B})\n";
    prompt += "> * Cost Paid: {B}\n";
    prompt += "> * Effect: Add {B}{B}{B}\n";
    prompt += "> * Mana Change: {0} -> {B}{B}{B}\n";
    prompt += "> **Step 2: Cast Ad Nauseam**\n";
    prompt += "> * Cost: {3}{B}{B}\n";
    prompt += "> * Current Pool: {B}{B}{B} + Chrome Mox ({B}) = {B}{B}{B}{B}\n";
    prompt += "> * Missing: {1} generic mana.\n";
    prompt += "> * RESULT: Cannot cast Ad Nauseam.\n";
    prompt += "**Phase 3: Strategic Summary**\n";
    prompt += "We are 1 mana short. Do not cast Dark Ritual.\n\n";

    prompt += "### Example 4: The \"Wrong Color\" (Color Failure)\n";
    prompt += "**Input:** { \"Floating\": \"{R}\", \"Hand\": [\"Swan Song\"], \"Board\": [] }\n";
    prompt += "**Correct Output:**\n";
    prompt += "**Phase 1: State Audit**\n";
    prompt += "* Untapped Sources: None.\n";
    prompt += "* Floating Mana: {R}.\n";
    prompt += "**Phase 2: The Line**\n";
    prompt += "> **Step 1: Cast Swan Song**\n";
    prompt += "> * Cost: {U}\n";
    prompt += "> * Current Pool: {R}\n";
    prompt += "> * RESULT: Failure. Cannot pay {U} with {R}.\n";
    prompt += "**Phase 3: Strategic Summary**\n";
    prompt += "Wrong colors. Pass turn.\n\n";

    prompt += "### Example 5: The \"Gone Forever\" (Discard Costs)\n";
    prompt += "**Input:** { \"Floating\": \"{0}\", \"Hand\": [\"Mox Diamond\", \"Ancient Tomb\"], \"Board\": [] }\n";
    prompt += "**Correct Output:**\n";
    prompt += "**Phase 1: State Audit**\n";
    prompt += "* Untapped Sources: None.\n";
    prompt += "**Phase 2: The Line**\n";
    prompt += "> **Step 1: Cast Mox Diamond**\n";
    prompt += "> * Cost Paid: {0}, Discard Ancient Tomb\n";
    prompt += "> * Hand Update: Removed Mox Diamond, Ancient Tomb\n";
    prompt += "> * Effect: Mox Diamond enters.\n";
    prompt += "> **Step 2: Play Ancient Tomb**\n";
    prompt += "> * RESULT: Failure. Ancient Tomb is in Graveyard.\n";
    prompt += "**Phase 3: Strategic Summary**\n";
    prompt += "Cannot play Tomb after discarding it.\n\n";

    prompt += "=== REQUIRED OUTPUT FORMAT (USE ONLY THE CHOSEN MODE) ===\n";
    
    prompt += "If **MULLIGAN MODE**:\n";
    prompt += "- Hand analysis: mana sources; acceleration; card advantage; win access; commander synergy\n";
    prompt += "- Decision: KEEP or MULLIGAN\n";
    prompt += "- If KEEP: explicitly list KEEP (exactly keep_size cards) and BOTTOM (the other cards)\n";
    prompt += "- **Hypothetical Line (Chain-of-State)**:\n";
    prompt += "  Simulate the first 1-2 turns (or failure to act) using the exact 'Phase 2: The Line' format below.\n";
    prompt += "  Show the mana math to prove the hand works or fails.\n";
    prompt += "- Reasoning: Explain why the simulated line justifies the decision.\n\n";

    prompt += "If **PLAY MODE** (Use Chain-of-State):\n";
    prompt += "**Phase 1: State Audit**\n";
    prompt += "List *only* the untapped mana sources and their production capabilities.\n";
    prompt += "List the current floating mana.\n\n";
    prompt += "**Phase 2: The Line (Step-by-Step)**\n";
    prompt += "For every step, follow this exact format:\n";
    prompt += "> **Step [N]: [Action Name]**\n";
    prompt += "> *   **Source Used:** [Name of Land/Rock] (or \"Floating Mana\")\n";
    prompt += "> *   **Cost Paid:** [Specific Symbols, e.g., {1}{U}]\n";
    prompt += "> *   **Mana Change:** [Old Pool] -> [New Pool]\n";
    prompt += "> *   **Hand Update:** [List cards removed (played/discarded)]\n";
    prompt += "> *   **Effect:** [Description]\n\n";
    prompt += "**Phase 3: Strategic Summary**\n";
    prompt += "Explain why this line was chosen and what the backup plan is if it fails.\n\n";

    prompt += "=== FINAL VERIFICATION STEP ===\n";
    prompt += "After you write your \"Recommended Line,\" you must act as a Judge.\n";
    prompt += "Read your own steps backwards.\n";
    prompt += "1. Did you spend mana that wasn't in the pool?\n";
    prompt += "2. Did you use a Tapped land?\n";
    prompt += "3. Did you use Colorless mana for a Colored pip?\n";
    prompt += "If you find an error, write: **\"CORRECTION: The previous line is invalid due to [Reason]. The correct play is...\"**\n\n";

    prompt += "### GAME STATE JSON\n";
    prompt += json;

    return prompt;
}
