// "Who dies next?": after each death you guess who will be killed next, and the game keeps score of how many deaths you called right.
// (No raylib here, so tests.cpp can run it.)
#pragma once
#include <string>

struct Predictions {
    int right = 0, total = 0;   // deaths you guessed right / deaths so far (a death you made no guess for counts as a miss)
    int guess = -1;             // who you currently think dies next (-1 = no guess)
    int scanned = 0;            // how many events the game has already looked through for deaths, so each death is scored exactly once

    void Death(int victim) {    // someone just died: score your guess, then it is cleared and you guess again
        total++;
        if (guess == victim) right++;
        guess = -1;
    }
    std::string Label() const { return std::to_string(right) + "/" + std::to_string(total) + " deaths predicted correct"; }
};