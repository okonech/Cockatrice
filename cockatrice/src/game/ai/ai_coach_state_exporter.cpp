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

namespace {
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

QJsonObject exportZone(const CardZoneLogic *zone)
{
    QJsonObject obj;
    obj.insert("name", zone->getName());
    obj.insert("contents_known", zone->contentsKnown());

    const CardList &cards = zone->getCards();
    obj.insert("count", cards.size());

    if (zone->contentsKnown()) {
        QJsonArray cardArray;
        for (const CardItem *card : cards) {
            cardArray.append(exportCard(card));
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

QJsonObject exportPlayer(AbstractGame *game, Player *player, bool isPerspective)
{
    QJsonObject obj;
    obj.insert("id", player->getPlayerInfo()->getId());
    obj.insert("name", player->getPlayerInfo()->getName());
    obj.insert("is_perspective", isPerspective);
    obj.insert("is_local", player->getPlayerInfo()->getLocalOrJudge());

    // Counters include mana counters (w/u/b/r/g/x) and life.
    obj.insert("counters", exportPlayerCounters(player));

    QJsonArray zones;
    const auto &zoneMap = player->getZones();
    for (auto it = zoneMap.cbegin(); it != zoneMap.cend(); ++it) {
        const QString zoneName = it.key();
        const CardZoneLogic *zone = it.value();
        if (!zone) {
            continue;
        }

        // Respect visibility (even in non-local games).
        // In local games (goldfish), everything should be known for the local player.
        zones.append(exportZone(zone));
    }
    obj.insert("zones", zones);

    Q_UNUSED(game);
    return obj;
}
} // namespace

QString AiCoachStateExporter::exportStateJson(AbstractGame *game, Player *perspectivePlayer)
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

    QJsonArray players;
    if (game && game->getPlayerManager()) {
        const auto &playerMap = game->getPlayerManager()->getPlayers();
        for (auto it = playerMap.cbegin(); it != playerMap.cend(); ++it) {
            Player *p = it.value();
            if (!p) {
                continue;
            }
            const bool isPerspective = (p == perspectivePlayer);
            players.append(exportPlayer(game, p, isPerspective));
        }
    }
    root.insert("players", players);

    return QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Compact));
}

QString AiCoachStateExporter::buildPromptText(AbstractGame *game, Player *perspectivePlayer)
{
    const QString json = exportStateJson(game, perspectivePlayer);

    // Keep this prompt short and very explicit; the JSON is the important payload.
    QString prompt;
    prompt += "You are an expert Magic: The Gathering Commander goldfishing coach.\n";
    prompt += "Given the following Cockatrice game state JSON, recommend the best next play sequence for the perspective player.\n";
    prompt += "Prioritize deterministic lines and maximizing chance to win; if info is missing, state assumptions.\n\n";
    prompt += "Output format:\n";
    prompt += "1) Recommended line (bullet steps)\n";
    prompt += "2) Why this line\n";
    prompt += "3) Alternative line(s)\n";
    prompt += "4) Key risks / missing info\n\n";
    prompt += "Game state JSON:\n";
    prompt += json;

    return prompt;
}
