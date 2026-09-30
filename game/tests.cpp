// Unit tests for jsonutil.h. Zero dependencies (no raylib, no test framework)
// so it builds and runs anywhere: g++ -std=c++17 tests.cpp -o tests && ./tests
#include "jsonutil.h"
#include "npc_brain.h"
#include <cstdio>
#include <cmath>

static int failures = 0;

void check(bool condition, const char* name) {
    if (condition) printf("PASS: %s\n", name);
    else { printf("FAIL: %s\n", name); failures++; }
}

int main() {
    std::string j = "{\"walkSpeed\": 7.5, \"playerColor\": \"#8C46C8\", \"happy\": [\"Hi!\", \"Hello there.\"]}";

    check(JNum(j, "walkSpeed", 1) == 7.5f, "JNum reads a number");
    check(JNum(j, "missingKey", 42) == 42, "JNum falls back to the default when the key is absent");

    RGB c = JColor(j, "playerColor", {0, 0, 0, 255});
    check(c.r == 0x8C && c.g == 0x46 && c.b == 0xC8, "JColor parses a hex color");

    RGB fallback = JColor(j, "missingKey", {1, 2, 3, 255});
    check(fallback.r == 1 && fallback.g == 2 && fallback.b == 3, "JColor falls back to the default when the key is absent");

    std::vector<std::string> lines = JStrArray(j, "happy");
    check(lines.size() == 2, "JStrArray reads the right number of lines");
    check(lines.size() == 2 && lines[0] == "Hi!" && lines[1] == "Hello there.", "JStrArray reads the line text correctly");

    check(JStrArray(j, "missingKey").empty(), "JStrArray returns empty for a missing key");

    check(FillName("Hello, {playerName}!", "Kate") == "Hello, Kate!", "FillName substitutes the player's name");
    check(FillName("No placeholder here", "Kate") == "No placeholder here", "FillName leaves text without a placeholder unchanged");

    check(LooksUsable("Nice to see you again!"), "LooksUsable accepts a normal short line");
    check(!LooksUsable("hi"), "LooksUsable rejects text that's too short");
    check(!LooksUsable(std::string(200, 'a')), "LooksUsable rejects text that's too long");
    check(!LooksUsable("the the the the the the"), "LooksUsable rejects text stuck repeating one word");
    check(!LooksUsable("you are a fucking idiot"), "LooksUsable rejects blocked words");

    // Regression test: {playerName} inside a string literal contains braces of its own,
    // which must not confuse the object-boundary matcher.
    std::string multi = "{\"elder\": {\"happy\": [\"Hi, {playerName}!\"]}, \"villager\": {\"happy\": [\"Hey {playerName}\"]}}";
    size_t p = multi.find("\"elder\"");
    size_t start = multi.find('{', p);
    size_t end = FindMatchingBrace(multi, start);
    std::string block = multi.substr(start, end - start + 1);
    check(JStrArray(block, "happy").size() == 1 && block.find("villager") == std::string::npos,
          "FindMatchingBrace stops at the right brace even when {playerName} appears inside the object");

    // Regression tests for the trained NPC brain (npc_brain.h) - these exact numbers came
    // straight out of train_brain.py's own sanity check, so this also catches anyone
    // accidentally retraining with a very different result and not noticing.
    BrainOutput friendly = RunBrain({0.8f, 0.9f, 0.5f, 0, 2, 1, 0, 0.5f});
    check(fabsf(friendly.opinionDelta - 6.894f) < 0.05f, "brain: friendly NPC + favorite color gives a strongly positive opinion delta");
    check(friendly.punchChance < 0.3f, "brain: friendly NPC + favorite color gives a low punch chance");

    BrainOutput hostile = RunBrain({0.3f, 0.3f, 0.1f, -60, 10, -1, 1, 0.5f});
    check(fabsf(hostile.opinionDelta - (-6.316f)) < 0.05f, "brain: impatient NPC + disliked color + aggression gives a strongly negative opinion delta");
    check(hostile.punchChance > 0.5f, "brain: impatient NPC + disliked color + aggression gives an elevated punch chance");

    printf(failures == 0 ? "\nAll tests passed!\n" : "\n%d test(s) FAILED.\n", failures);
    return failures == 0 ? 0 : 1;
}