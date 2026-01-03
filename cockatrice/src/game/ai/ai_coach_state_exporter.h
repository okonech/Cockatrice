#ifndef COCKATRICE_AI_COACH_STATE_EXPORTER_H
#define COCKATRICE_AI_COACH_STATE_EXPORTER_H

#include <QString>

class AbstractGame;
class Player;

class AiCoachStateExporter
{
public:
    // Produces a stable, machine-readable JSON snapshot of state.
    static QString exportStateJson(AbstractGame *game, Player *perspectivePlayer);

    // Produces the full prompt text that will be sent to the LLM.
    static QString buildPromptText(AbstractGame *game, Player *perspectivePlayer);
};

#endif // COCKATRICE_AI_COACH_STATE_EXPORTER_H
