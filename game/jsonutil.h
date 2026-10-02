// Tiny hand-written JSON readers. No library, no nesting support beyond what
// this game's own config/dialogue files need. Deliberately dependency-free
// (no raylib types) so tests.cpp can include this without linking graphics.
#pragma once
#include <string>
#include <vector>
#include <cstdlib>
#include <cstdio>
#include <algorithm>
#include <cctype>

struct RGB { unsigned char r, g, b, a; };

inline float JNum(const std::string& j, const char* key, float def) {
    size_t p = j.find(std::string("\"") + key + "\"");
    if (p == std::string::npos) return def;
    return (float)atof(j.c_str() + j.find(':', p) + 1);
}

inline RGB JColor(const std::string& j, const char* key, RGB def) {
    size_t p = j.find(std::string("\"") + key + "\"");
    if (p == std::string::npos) return def;
    p = j.find('"', j.find(':', p) + 1) + 1;
    int r, g, b;
    if (sscanf(j.c_str() + p, "#%2x%2x%2x", &r, &g, &b) != 3) return def;
    return RGB{(unsigned char)r, (unsigned char)g, (unsigned char)b, 255};
}

// Finds the closing '{' matching the one at `start`, skipping over the
// contents of string literals so a placeholder like {playerName} inside a
// quoted string doesn't get mistaken for real JSON structure.
inline size_t FindMatchingBrace(const std::string& j, size_t start) {
    int depth = 0;
    bool inStr = false;
    for (size_t i = start; i < j.size(); i++) {
        char c = j[i];
        if (inStr) {
            if (c == '\\') { i++; continue; }   // skip an escaped character
            if (c == '"') inStr = false;
            continue;
        }
        if (c == '"') inStr = true;
        else if (c == '{') depth++;
        else if (c == '}') { if (--depth == 0) return i; }
    }
    return std::string::npos;
}

// Reads a flat array of strings, e.g. "happy": ["Hi!", "Hello there."]
// No escaped quotes or nested brackets - fine for hand-written dialogue files.
inline std::vector<std::string> JStrArray(const std::string& j, const char* key) {
    std::vector<std::string> out;
    size_t p = j.find(std::string("\"") + key + "\"");
    if (p == std::string::npos) return out;
    size_t start = j.find('[', p), end = j.find(']', start);
    if (start == std::string::npos || end == std::string::npos) return out;
    size_t i = start + 1;
    while (i < end) {
        size_t q1 = j.find('"', i);
        if (q1 == std::string::npos || q1 > end) break;
        size_t q2 = j.find('"', q1 + 1);
        out.push_back(j.substr(q1 + 1, q2 - q1 - 1));
        i = q2 + 1;
    }
    return out;
}

// Splits "a|b|c" into {"a", "b", "c"}.
inline std::vector<std::string> Split(const std::string& s, char sep) {
    std::vector<std::string> out;
    size_t start = 0;
    for (;;) {
        size_t p = s.find(sep, start);
        if (p == std::string::npos) { out.push_back(s.substr(start)); break; }
        out.push_back(s.substr(start, p - start));
        start = p + 1;
    }
    return out;
}

// Replaces every occurrence of `from` in `s` with `to`.
inline std::string ReplaceAll(std::string s, const std::string& from, const std::string& to) {
    if (from.empty()) return s;
    size_t pos = 0;
    while ((pos = s.find(from, pos)) != std::string::npos) { s.replace(pos, from.size(), to); pos += to.size(); }
    return s;
}

// Replaces every occurrence of {playerName} with the given name.
inline std::string FillName(const std::string& s, const std::string& name) { return ReplaceAll(s, "{playerName}", name); }

// Raw text from the language model is "whatever came after the prompt": cut it at the first closing
// quote or line break (that's where the spoken line ends), trim it, and drop any half-finished
// trailing sentence so the NPC doesn't stop mid-thought.
inline std::string CleanGenerated(std::string s) {
    // Instruction-tuned models like to start with a "Name (mood) says to Sam:" label and/or wrap the reply in quotes: drop those first.
    size_t b0 = s.find_first_not_of(" \t");
    s = (b0 == std::string::npos) ? std::string() : s.substr(b0);
    if (!s.empty() && (s[0] == '\n' || s[0] == '\r')) return std::string();   // a new line straight away = the model moved on to someone else's turn
    size_t says = s.find(" says to ");
    if (says != std::string::npos && says < 60) {
        size_t colon = s.find(':', says);
        if (colon != std::string::npos && colon < 120) s = s.substr(colon + 1);
    }
    while (!s.empty() && (s[0] == '"' || s[0] == ' ')) s.erase(0, 1);
    size_t cut = s.find_first_of("\"\n\r");
    if (cut != std::string::npos) s = s.substr(0, cut);
    size_t b = s.find_first_not_of(" \t");
    size_t e = s.find_last_not_of(" \t");
    s = (b == std::string::npos) ? std::string() : s.substr(b, e - b + 1);
    if (!s.empty() && (s.back() == ',' || s.back() == ';' || s.back() == ':')) s.back() = '.';   // "...every morning," -> "...every morning."
    return s;
}

// A safety net over AI-generated text (see shell.html's AIDialogue object): rejects
// anything too short/long, stuck repeating one word, or containing a small blocklist
// of words we never want a player to see. The caller falls back to a curated line
// whenever this returns false. (There are no scripted fallback lines any more: the game just asks GPT-2 again.)
inline bool LooksUsable(const std::string& s) {
    if (s.size() < 3 || s.size() > 230) return false;
    int maxRun = 1, run = 1;
    std::string lastWord, word;
    for (size_t i = 0; i <= s.size(); i++) {
        if (i == s.size() || s[i] == ' ') {
            if (!word.empty()) {
                if (word == lastWord) { run++; maxRun = std::max(maxRun, run); } else run = 1;
                lastWord = word;
            }
            word.clear();
        } else word += (char)tolower((unsigned char)s[i]);
    }
    if (maxRun >= 4) return false;   // the same word four-plus times in a row = broken output ("kek kek kek" is fine)
    static const char* blocked[] = {"fuck", "shit", "bitch", "nigger", "cunt", "rape", "http", "www.", "@", "#", "kill yourself"};
    std::string lower; for (char c : s) lower += (char)tolower((unsigned char)c);
    for (const char* b : blocked) if (lower.find(b) != std::string::npos) return false;
    return true;
}

inline int CountWords(const std::string& s) {
    int n = 0; bool in = false;
    for (char c : s) { if (c == ' ') in = false; else if (!in) { in = true; n++; } }
    return n;
}

// Does the line mention at least one of the keywords (case-insensitive)? Used to check a line is actually
// about what the character was supposed to be talking about (their job, a friend, the human world...).
inline bool MentionsAny(const std::string& line, const std::vector<std::string>& keywords) {
    std::string lower; for (char c : line) lower += (char)tolower((unsigned char)c);
    for (const std::string& k : keywords) {
        std::string kl; for (char c : k) kl += (char)tolower((unsigned char)c);
        if (kl.size() >= 3 && lower.find(kl) != std::string::npos) return true;
    }
    return false;
}

// The stricter check applied to the first few attempts at a line: long enough to say something, a real
// question when the character is meant to ask one, and on topic. (The last attempt only needs LooksUsable.)
inline bool LineIsGood(const std::string& line, bool needQuestion, const std::vector<std::string>& keywords) {
    if (CountWords(line) < 5) return false;
    if (needQuestion && line.find('?') == std::string::npos) return false;
    if (!keywords.empty() && !MentionsAny(line, keywords)) return false;
    return true;
}

// An 8-character fingerprint of a string (djb2). Used in cache keys so a stored reply is only reused when the line it
// answers is the same line.
inline std::string ShortHash(const std::string& s) {
    unsigned h = 5381u;
    for (unsigned char c : s) h = h * 33u + c;
    char buf[16];
    snprintf(buf, sizeof(buf), "%08x", h);
    return std::string(buf);
}

// The meaningful words of a sentence (3+ letters, no filler): a good line about it mentions at least one of them.
inline std::vector<std::string> KeyWords(const std::string& text) {
    std::vector<std::string> out;
    for (const std::string& w : Split(text, ' ')) if (w.size() >= 3 && w != "the" && w != "player's" && w != "are" && w != "was" && w != "and") out.push_back(w);
    return out;
}