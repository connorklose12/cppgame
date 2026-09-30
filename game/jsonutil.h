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

// Replaces every occurrence of {playerName} with the given name.
inline std::string FillName(std::string s, const std::string& name) {
    size_t pos;
    while ((pos = s.find("{playerName}")) != std::string::npos) s.replace(pos, 12, name);
    return s;
}

// A safety net over AI-generated text (see shell.html's AIDialogue object): rejects
// anything too short/long, stuck repeating one word, or containing a small blocklist
// of words we never want a player to see. The caller falls back to a curated line
// whenever this returns false.
inline bool LooksUsable(const std::string& s) {
    if (s.size() < 3 || s.size() > 90) return false;
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
    if (maxRun >= 3) return false;   // the same word three-plus times in a row = broken output
    static const char* blocked[] = {"fuck", "shit", "bitch", "nigger", "cunt", "rape"};
    std::string lower; for (char c : s) lower += (char)tolower((unsigned char)c);
    for (const char* b : blocked) if (lower.find(b) != std::string::npos) return false;
    return true;
}