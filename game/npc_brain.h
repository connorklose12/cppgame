// A tiny, hand-written feedforward neural net: 8 inputs -> 8 hidden (tanh) -> 2 outputs (linear).
// No ML library at runtime - just dot products. The weights in npc_brain_weights.h were
// trained offline by train_brain.py (see that file for what it was trained on and why).
#pragma once
#include "npc_brain_weights.h"
#include <cmath>
#include <algorithm>

struct BrainInput {
    float curiosity, friendliness, patience;   // the NPC's personality (0..1 each)
    float opinion;                             // the NPC's current opinion of the player (-100..100)
    float timesTalked;                         // how many times the player has talked to this NPC
    float colorMatch;                          // -1 disliked color, 0 neither, 1 player's favorite color
    float aggression;                          // 0..1, how hostile the player has been acting
    float dayNight;                            // 0 (night) .. 1 (day)
};
struct BrainOutput {
    float opinionDelta;   // how much to shift this NPC's opinion after this interaction (-10..10)
    float punchChance;    // 0..1 chance this furious NPC takes a swing at the player
};

inline BrainOutput RunBrain(const BrainInput& in) {
    float x[8] = {in.curiosity, in.friendliness, in.patience, in.opinion / 100.0f,
                  in.timesTalked / 20.0f, in.colorMatch, in.aggression, in.dayNight};

    float hidden[HIDDEN_SIZE];
    for (int j = 0; j < HIDDEN_SIZE; j++) {
        float sum = B1[j];
        for (int i = 0; i < 8; i++) sum += x[i] * W1[i][j];
        hidden[j] = tanhf(sum);
    }

    float out[2];
    for (int k = 0; k < 2; k++) {
        float sum = B2[k];
        for (int j = 0; j < HIDDEN_SIZE; j++) sum += hidden[j] * W2[j][k];
        out[k] = sum;   // linear output layer (matches scikit-learn's MLPRegressor default)
    }

    BrainOutput result;
    result.opinionDelta = std::max(-10.0f, std::min(10.0f, out[0] * 10.0f));
    result.punchChance = std::max(0.0f, std::min(1.0f, out[1]));
    return result;
}