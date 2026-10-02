// Relationship "dynamics": turns the raw numbers the game stores (affinity, chemistry, fights, partner, crush, the event
// history) into named relationships that dialogue can actually use - "best friends", "bitter rivals", "exes", "still hurt
// by the cheating" - and the same for how each villager currently sees THE PLAYER ("skeptical", "has a crush on you",
// "unsure", "thinks you're best friends"...). No raylib here, so tests.cpp can run it.
//
// Nothing new is stored for villager-to-villager dynamics: they are DERIVED from what social.h already saves, so they can
// never drift out of sync with it. How a villager sees the player uses three saved numbers per villager (opinion, trust,
// romance) plus how often you've talked to them and hit them.
#pragma once
#include <string>
#include "social.h"

// ============================ villager <-> villager ============================
// One letter describing how `a` sees `b` (it can differ the other way round: a one-sided crush, a one-sided grudge).
//   p dating   c secret crush   h still hurt (b cheated on a)   x exes   R bitter rivals (they have come to blows)
//   e enemies  B best friends   f good friends   l friendly   d wary   n barely know each other
inline char PairDynCode(const Social& s, int a, int b) {
    if (s.partner[a] == b) return 'p';
    if (s.crush[a] == b) return 'c';
    for (const SocialEvent& e : s.events) {
        if (e.type == EV_DISCOVER && e.a == a && e.b == b) return 'h';   // a found out that b cheated
    }
    for (const SocialEvent& e : s.events) {
        if (e.type == EV_BREAKUP && ((e.a == a && e.b == b) || (e.a == b && e.b == a))) return 'x';   // they used to date (and aren't dating now: that was checked above)
    }
    float aff = s.affinity[a][b];
    if (s.clashes[a][b] > 0 && aff < -20) return 'R';
    if (aff < -40) return 'e';
    if (aff > 65) return 'B';
    if (aff > 40) return 'f';
    if (aff > 15) return 'l';
    if (aff < -15) return 'd';
    return 'n';
}

inline std::string PairDynPhrase(char code, const std::string& A, const std::string& B) {
    switch (code) {
        case 'p': return A + " is dating " + B + ".";
        case 'c': return A + " has a secret crush on " + B + ".";
        case 'h': return A + " is still hurt that " + B + " cheated on them.";
        case 'x': return A + " and " + B + " used to date and broke up.";
        case 'R': return A + " and " + B + " are bitter rivals who have come to blows.";
        case 'e': return A + " hates " + B + ".";
        case 'B': return A + " and " + B + " are best friends.";
        case 'f': return A + " is good friends with " + B + ".";
        case 'l': return A + " likes " + B + ".";
        case 'd': return A + " is wary of " + B + ".";
        default:  return A + " barely knows " + B + ".";
    }
}

// How dramatic a relationship is (lower = more worth talking about). Bland ones (l, d, n) are never "notable".
inline int PairDynRank(char code) {
    switch (code) { case 'p': return 0; case 'c': return 1; case 'h': return 2; case 'x': return 3; case 'R': return 4; case 'e': return 5; case 'B': return 6; case 'f': return 7; default: return 99; }
}

// ============================ villager -> the player ============================
struct PlayerFeel {
    float opinion = 0;     // -100 .. 100: how much they like you
    float trust = 0;       // -100 .. 100: whether they believe you mean well (hitting them hurts this badly)
    float romance = 0;     // 0 .. 100: how smitten they are
    int timesTalked = 0, timesHit = 0;
};

//   H hostile   G grudge (you hit them)   C crush on you   S skeptical   N just met   U unsure
//   W warming up   F good friend   K best friend
inline char PlayerDynCode(const PlayerFeel& f) {
    if (f.opinion < -55) return 'H';
    if (f.timesHit > 0 && (f.opinion < 10 || f.trust < 0)) return 'G';
    if (f.romance >= 45 && f.opinion > 25 && f.timesHit == 0) return 'C';
    if (f.trust < -15 || f.opinion < -15) return 'S';
    if (f.timesTalked <= 1 && f.opinion <= 40) return 'N';
    if (f.opinion > 70 && f.timesTalked >= 8) return 'K';
    if (f.opinion > 40) return 'F';
    if (f.opinion > 15) return 'W';
    return 'U';
}

// The sentence the AI is given (starts with a verb: "<Name> " + this) - the feeling AND how it should sound.
inline const char* PlayerDynPhrase(char code) {
    switch (code) {
        case 'H': return "hates the player and is openly aggressive toward them (insults, threats, snarling)";
        case 'G': return "is still angry that the player hit them (bitter, picks a fight, throws it in their face)";
        case 'C': return "has a crush on the player and flirts with them (compliments, teasing, blushing, hopeful hints)";
        case 'S': return "is skeptical of the player and does not trust them yet (suspicious questions, doubtful, guarded)";
        case 'N': return "has just met the player and is curious but cautious (polite, probing)";
        case 'W': return "is warming up to the player (friendlier, a bit teasing)";
        case 'F': return "likes the player as a good friend (warm, easy, joking)";
        case 'K': return "thinks of the player as a best friend (affectionate, inside jokes, honest)";
        default:  return "is unsure what to make of the player (hesitant, noncommittal)";
    }
}

// Friends and admirers call you by a pet name (their own nickname for you); everyone else uses your actual name.
inline bool UsesNickname(char code) { return code == 'K' || code == 'F' || code == 'C' || code == 'W'; }

// ============================ verbal tic ============================
// Tacks the speaker's tag ("..., mark my words.") onto the end of a sentence now and then. Done in code, so it costs nothing
// and it reliably sounds like THEM. `roll` is a random number 0..1 (passed in so tests are repeatable).
// Keeps a verbal tic a flavour instead of a catchphrase: it may appear at most once every few lines (call Ready once per line spoken).
struct TicGate {
    int since = 99;                                 // lines spoken since the tic was last used
    bool Ready(int gap) { return ++since >= gap; }
    void Used() { since = 0; }
};
// Friends and admirers call you by their pet name most of the time, not in every single sentence.
inline bool UsesNicknameNow(char code, float roll) { return UsesNickname(code) && roll < 0.6f; }

inline std::string ApplyTic(const std::string& line, const std::string& tic, float roll, float chance) {
    if (roll >= chance || tic.empty() || line.size() < 20) return line;
    char last = line.back();
    if (last != '.' && last != '!') return line;                           // not on questions
    if (line.size() >= 3 && line.compare(line.size() - 3, 3, "...") == 0) return line;
    int words = 1;
    for (char c : line) if (c == ' ') words++;
    if (words < 6 || line.find(tic.substr(2, tic.size() - 3)) != std::string::npos) return line;   // too short, or already says it
    return line.substr(0, line.size() - 1) + tic;
}