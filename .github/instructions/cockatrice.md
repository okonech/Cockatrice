---
applyTo: "**/*"
---

Act as a software architect to do the following:

Recommended Approach (Fits Cockatrice’s shape)

Put the entrypoint in the in-game UI: add a QAction (e.g. “AI Coach: Recommend Next Play…”) in tab_game.cpp where game-specific menus/actions already live, and bind its shortcut in TabGame::refreshShortcuts() using a new key name like Player/aAiRecommend.
Gate it to “goldfish” by default: only enable the action when game->getGameState()->getIsLocalGame() is true (local game = safe to export full info without leaking hidden multiplayer data). This matches how local-visibility logic is already treated in the engine.
State Export (What to send to the LLM)

Implement a small “exporter” module that takes a Player* perspective (active local player) and emits either JSON or deterministic plain text.
Use the existing model APIs you already found:
Zones: Player → CardZoneLogic → getCards() (hand/deck/grave/rfg/table/stack/sb), but I’d recommend iterating the zones map to also include any custom zones instead of hardcoding only the getters.
Per-card state: CardItem fields (tapped/facedown/PT/annotation/attachments) + CardItem::getCounters().
Player counters: Player::getCounters() → AbstractCounter name/value; for display names, reuse translate_counter_name.cpp.
Visibility policy (important if you ever allow non-local games):
If zone->contentsKnown() is false, export only counts (“Deck: 62 cards (unknown)”) and never card names.
Always fully export battlefield/stack for the perspective player; for opponents, only export what is visible.
LLM Integration: HTTP First, “MCP” via Local Companion

Best practical choice inside a Qt desktop app: an HTTP client provider built on QNetworkAccessManager (Cockatrice already uses Qt networking elsewhere), with settings for endpoint/model/key.
If you want “MCP server locally”: I’d still make Cockatrice talk HTTP to a local companion service (e.g. http://127.0.0.1:PORT/recommend) that can internally run MCP/tooling orchestration. Keeping MCP details out of Cockatrice avoids inventing a new in-process MCP runtime in C++/Qt.
Provider abstraction suggestion: IAiCoachProvider::recommend(stateJson) -> QString, with:
HttpAiCoachProvider (OpenAI-compatible or your own local service)
optional LocalProcessProvider (uses QProcess to spawn a helper), but I’d only do this if you need “one-click start”; otherwise let users run the companion service themselves.
UX (Minimal but usable)

Trigger action → show a small modal/dock with: “Captured state”, “Send”, streaming/response text, and “Copy to clipboard”.
Keep the prompt template versioned (“cockatrice.ai_state.v1”) so you can evolve the schema without breaking the companion service.