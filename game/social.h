// NPC-to-NPC social simulation. Plain rules and probabilities - no raylib, so tests.cpp can run it.
//
// Every ordered pair (i, j) has an "affinity" (how much i likes j, -100..100) that drifts toward a
// fixed random "chemistry" value whenever the two are standing near each other, and fades back
// toward 0 when they're apart. Everything else - crushes, dating, cheating, finding out, breakups,
// enemies - falls out of those numbers plus personality-driven dice rolls.
#pragma once
#include <vector>
#include <cmath>
#include <algorithm>

constexpr int MAX_NPCS = 8;

// ---- Pacing knobs: raise these and things happen sooner, lower them and the village stays calmer for longer ----
constexpr float AFFINITY_PULL = 0.06f;        // how fast two NPCs' feelings settle while they stand together (lower = relationships build up more slowly)
constexpr float DRAMA_CHANCE = 0.025f;        // per second: chance that two random NPCs spontaneously start liking/disliking each other
constexpr float FIGHT_PICK_FURIOUS = 0.04f;   // per second: an NPC who is really angry at someone in the room decides to go after them
constexpr float FIGHT_PICK_PROVOKE = 0.01f;   // per second: a short-tempered, mildly annoyed NPC starts something
constexpr float KILL_CHANCE = 0.30f;          // when a far-gone NPC reaches their enemy: how likely it ends in a killing rather than a punch
constexpr float CRUSH_AFFINITY = 30.0f;       // an NPC develops a crush on someone they like at least this much (it fades below CRUSH_AFFINITY - 10)
constexpr float CHEAT_CHANCE = 0.30f;         // per second: a partnered NPC alone with someone they really like gives in to temptation (scaled down by loyalty = patience)
constexpr float CHEAT_AFFINITY = 30.0f;       // ...if they like that person at least this much
constexpr int CLASHES_BEFORE_KILL = 2;        // two NPCs must have fought at least this many times (punches either way) before one can kill the other

enum EventType { EV_CRUSH, EV_DATING, EV_BREAKUP, EV_CHEAT, EV_DISCOVER, EV_PUNCH, EV_KILL };

// a, b, c depend on the type:
//   CRUSH    a has a crush on b            DATING   a and b started dating
//   BREAKUP  a and b broke up              CHEAT    a cheated on c with b
//   DISCOVER a (the betrayed) found out that b (the cheater) cheated with c
//   PUNCH    a punched b                   KILL     a killed b (everyone knows)
struct SocialEvent {
    EventType type;
    int a, b, c;
    float time;
    unsigned knowers;   // bitmask of the NPCs who know about it (crushes start out secret)
    bool discovered;    // CHEAT only: has the betrayed partner found out yet?
};

// A snapshot of where everyone is, handed in by the game each tick.
struct SocialView {
    int loc[MAX_NPCS];
    float x[MAX_NPCS], z[MAX_NPCS];
    float patience[MAX_NPCS];   // doubles as loyalty: patient NPCs cheat less
};

struct Social {
    int n = 0;
    float affinity[MAX_NPCS][MAX_NPCS] = {};
    float chemistry[MAX_NPCS][MAX_NPCS] = {};
    int partner[MAX_NPCS] = {};
    int crush[MAX_NPCS] = {};
    bool alive[MAX_NPCS] = {};
    bool drama = true;   // random spontaneous likes/dislikes (tests switch it off)
    int clashes[MAX_NPCS][MAX_NPCS] = {};   // how many times each pair has come to blows (counted both ways)
    std::vector<SocialEvent> events;
    unsigned rng = 2463534242u;

    float Rand01() {   // tiny xorshift so tests are repeatable
        rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
        return (rng & 0xFFFFFF) / 16777216.0f;
    }
};

inline void SocialInit(Social& s, int n, unsigned seed) {
    s.n = n;
    s.rng = seed ? seed : 1u;
    s.events.clear();
    for (int i = 0; i < MAX_NPCS; i++) {
        s.partner[i] = s.crush[i] = -1;
        s.alive[i] = true;
        for (int j = 0; j < MAX_NPCS; j++) {
            s.affinity[i][j] = 0;
            s.clashes[i][j] = 0;
            s.chemistry[i][j] = (i == j) ? 0.0f : s.Rand01() * 2.0f - 1.0f;   // i's feelings about j are independent of j's about i
        }
    }
}

inline bool Together(const Social& s, const SocialView& v, int i, int j) {
    if (!s.alive[i] || !s.alive[j] || v.loc[i] != v.loc[j]) return false;
    float dx = v.x[i] - v.x[j], dz = v.z[i] - v.z[j];
    return dx * dx + dz * dz < 5.5f * 5.5f;
}
inline bool Knows(const SocialEvent& e, int npc) { return (e.knowers >> npc) & 1u; }
inline bool IsEnemy(const Social& s, int i, int j) { return s.affinity[i][j] < -30; }

inline void AddEvent(Social& s, EventType t, int a, int b, int c, float now, unsigned knowers) {
    s.events.push_back({t, a, b, c, now, knowers, false});
}

// Being wronged changes how i feels about j right now AND where those feelings settle in the long run, so a
// punch, a betrayal or a murder leaves a lasting grudge instead of fading back to "we're basically friends".
inline void Sour(Social& s, int i, int j, float amount) {
    s.affinity[i][j] = std::max(-100.0f, s.affinity[i][j] - amount);
    s.chemistry[i][j] = std::max(-1.0f, s.chemistry[i][j] - amount / 100.0f * 0.8f);
}

// The opposite of Sour: a kind act warms feelings now and for good (friends first, and with enough of it, lovers).
inline void Warm(Social& s, int i, int j, float amount) {
    s.affinity[i][j] = std::min(100.0f, s.affinity[i][j] + amount);
    s.chemistry[i][j] = std::min(1.0f, s.chemistry[i][j] + amount / 100.0f * 0.8f);
}

inline bool HasOpenCheat(const Social& s, int a, int b, int c) {
    for (const SocialEvent& e : s.events)
        if (e.type == EV_CHEAT && !e.discovered && e.a == a && e.b == b && e.c == c) return true;
    return false;
}

// Call about once a second.
inline void SocialTick(Social& s, const SocialView& v, float now) {
    const unsigned everyone = (1u << s.n) - 1;

    // 0) random drama: now and then two NPCs simply start to dislike (or like) each other for no reason at all
    if (s.drama && s.Rand01() < DRAMA_CHANCE) {
        int i = (int)(s.Rand01() * s.n) % s.n, j = (int)(s.Rand01() * s.n) % s.n;
        if (i != j && s.alive[i] && s.alive[j])
            s.chemistry[i][j] = std::max(-1.0f, std::min(1.0f, s.chemistry[i][j] + (s.Rand01() < 0.6f ? -0.4f : 0.3f)));
    }

    // 1) feelings drift toward chemistry when together, and fade when apart
    for (int i = 0; i < s.n; i++)
        for (int j = 0; j < s.n; j++) {
            if (i == j || !s.alive[i] || !s.alive[j]) continue;
            float& a = s.affinity[i][j];
            if (Together(s, v, i, j)) a += (s.chemistry[i][j] * 80.0f - a) * AFFINITY_PULL;
            else a *= 0.998f;
            a = std::max(-100.0f, std::min(100.0f, a));
        }

    // 2) crushes (secret until someone gossips about them)
    for (int i = 0; i < s.n; i++) {
        if (!s.alive[i]) continue;
        if (s.crush[i] != -1 && s.affinity[i][s.crush[i]] < CRUSH_AFFINITY - 10) s.crush[i] = -1;
        if (s.crush[i] != -1) continue;
        int best = -1;
        for (int j = 0; j < s.n; j++)
            if (j != i && s.alive[j] && j != s.partner[i] && s.affinity[i][j] > CRUSH_AFFINITY && (best == -1 || s.affinity[i][j] > s.affinity[i][best])) best = j;
        if (best != -1) { s.crush[i] = best; AddEvent(s, EV_CRUSH, i, best, -1, now, 1u << i); }
    }

    // 3) two single NPCs with crushes on each other start dating (public news)
    for (int i = 0; i < s.n; i++)
        for (int j = i + 1; j < s.n; j++)
            if (s.alive[i] && s.alive[j] && s.partner[i] == -1 && s.partner[j] == -1 &&
                ((s.crush[i] == j && s.crush[j] == i) || (s.crush[i] == j && s.affinity[j][i] > CRUSH_AFFINITY) || (s.crush[j] == i && s.affinity[i][j] > CRUSH_AFFINITY))) {
                s.partner[i] = j; s.partner[j] = i; s.crush[i] = j; s.crush[j] = i;
                AddEvent(s, EV_DATING, i, j, -1, now, everyone);
            }

    // 4) couples that have drifted far apart break up on their own
    for (int i = 0; i < s.n; i++) {
        int j = s.partner[i];
        if (j > i && s.affinity[i][j] < 20 && s.affinity[j][i] < 20) {
            s.partner[i] = s.partner[j] = -1;
            s.crush[i] = s.crush[j] = -1;
            AddEvent(s, EV_BREAKUP, i, j, -1, now, everyone);
        }
    }

    // 5) cheating: a partnered NPC alone with someone they really like, while their partner is elsewhere
    for (int i = 0; i < s.n; i++) {
        int p = s.partner[i];
        if (!s.alive[i] || p == -1 || Together(s, v, i, p)) continue;
        for (int k = 0; k < s.n; k++) {
            if (k == i || k == p || !Together(s, v, i, k) || s.affinity[i][k] <= CHEAT_AFFINITY) continue;
            if (!HasOpenCheat(s, i, k, p) && s.Rand01() < CHEAT_CHANCE * (1.2f - v.patience[i])) {
                unsigned knowers = (1u << i) | (1u << k);                         // the two of them...
                for (int w = 0; w < s.n; w++)
                    if (w != p && Together(s, v, i, w)) knowers |= 1u << w;          // ...plus anyone who saw
                AddEvent(s, EV_CHEAT, i, k, p, now, knowers);
                s.crush[i] = k;
            }
            break;   // one candidate per tick
        }
    }

    // 6) the betrayed partner finds out: caught in the act, or a witness tells them
    for (size_t idx = 0; idx < s.events.size(); idx++) {   // index loop: AddEvent below can reallocate the vector
        if (s.events[idx].type != EV_CHEAT || s.events[idx].discovered) continue;
        const int cheater = s.events[idx].a, other = s.events[idx].b, betrayed = s.events[idx].c;
        const unsigned knowers = s.events[idx].knowers;
        if (s.partner[betrayed] != cheater) { s.events[idx].discovered = true; continue; }   // already split up - stale

        bool found = false;
        if (Together(s, v, betrayed, cheater) && Together(s, v, betrayed, other)) found = s.Rand01() < 0.5f;
        else
            for (int w = 0; w < s.n && !found; w++)
                if (w != cheater && w != other && w != betrayed && ((knowers >> w) & 1u) && Together(s, v, w, betrayed))
                    found = s.Rand01() < 0.3f;
        if (!found) continue;

        s.events[idx].discovered = true;
        s.events[idx].knowers |= 1u << betrayed;
        Sour(s, betrayed, cheater, 70);
        Sour(s, betrayed, other, 50);
        s.partner[betrayed] = s.partner[cheater] = -1;
        s.crush[betrayed] = -1;
        AddEvent(s, EV_DISCOVER, betrayed, cheater, other, now, everyone);
    }
}

// ---------------- the player hits an NPC with the stick ----------------
enum Reaction { REACT_NONE, REACT_THANK, REACT_CONFRONT };
struct HitResult {
    float opinionDelta[MAX_NPCS];   // change to each NPC's opinion of the player
    int reaction[MAX_NPCS];         // who wants to thank the player / confront the player
};

// The victim and the victim's friends and partner get angry; the victim's enemies are delighted
// and want to thank you. People who saw it react fully; word reaches everyone else a bit softer.
inline HitResult SocialHit(const Social& s, int victim, const bool witness[MAX_NPCS]) {
    HitResult r = {};
    for (int i = 0; i < s.n; i++) {
        if (!s.alive[i]) continue;
        const float mult = witness[i] ? 1.0f : 0.4f;
        if (i == victim) { r.opinionDelta[i] = -35; r.reaction[i] = REACT_CONFRONT; continue; }
        const float a = s.affinity[i][victim];
        const bool isPartner = s.partner[i] == victim;
        if (a < -30) { r.opinionDelta[i] = 20 * mult; r.reaction[i] = REACT_THANK; }
        else if (isPartner || a > 30) { r.opinionDelta[i] = (isPartner ? -40 : -25) * mult; r.reaction[i] = REACT_CONFRONT; }
        else { r.opinionDelta[i] = -8 * mult; r.reaction[i] = REACT_NONE; }
    }
    return r;
}

// "Learning from each other": each NPC's opinion of the player drifts toward the opinions of
// the NPCs they like (weighted by how much), so a well-liked NPC's view spreads through their friends.
inline void GossipOpinions(const Social& s, float* opinion) {
    float next[MAX_NPCS];
    for (int i = 0; i < s.n; i++) {
        float wsum = 0, acc = 0;
        for (int j = 0; j < s.n; j++) {
            if (j == i || !s.alive[j]) continue;
            float w = std::max(0.0f, s.affinity[i][j]) / 100.0f;
            wsum += w; acc += w * opinion[j];
        }
        next[i] = (s.alive[i] && wsum > 0.01f) ? opinion[i] + (acc / wsum - opinion[i]) * std::min(1.0f, wsum) * 0.08f : opinion[i];
    }
    for (int i = 0; i < s.n; i++) opinion[i] = next[i];
}

// ---------------- NPCs fighting each other ----------------
// Anger is just affinity going very negative. Really angry NPCs go after their worst enemy; short-tempered
// ones that are only mildly annoyed sometimes throw a punch just to provoke. Punches make the victim madder
// at the attacker (which is how feuds snowball); someone far enough gone will kill. NPCs have no HP.
enum Attack { ATTACK_NONE, ATTACK_PUNCH, ATTACK_KILL };

// Call once a second per NPC. Returns who `a` wants to go after right now (same room only), or -1.
inline int SocialPickTarget(Social& s, const SocialView& v, int a) {
    if (!s.alive[a]) return -1;
    int worst = -1;
    for (int j = 0; j < s.n; j++)
        if (j != a && s.alive[j] && v.loc[j] == v.loc[a] && s.affinity[a][j] < -40 && (worst == -1 || s.affinity[a][j] < s.affinity[a][worst])) worst = j;
    if (worst != -1) return s.Rand01() < FIGHT_PICK_FURIOUS ? worst : -1;           // furious: sooner or later, but not right away
    for (int j = 0; j < s.n; j++)
        if (j != a && s.alive[j] && v.loc[j] == v.loc[a] && s.affinity[a][j] < -15 && s.Rand01() < FIGHT_PICK_PROVOKE * (1.2f - v.patience[a])) return j;   // provoking
    return -1;
}

// Does `a` kill `victim` this time? (The game rolls this the moment `a` reaches them, so it can darken the
// screen a second BEFORE the knife comes down; nothing changes until SocialKill is called.)
inline bool SocialWillKill(Social& s, int a, int victim) {
    return s.alive[a] && s.alive[victim] && s.clashes[a][victim] >= CLASHES_BEFORE_KILL && s.affinity[a][victim] < -85 && s.Rand01() < KILL_CHANCE;   // a feud has to build up first
}

inline void SocialPunch(Social& s, const SocialView& v, int a, int victim, float now) {
    Sour(s, victim, a, 25);                                                       // getting punched makes you mad at the puncher - for good
    s.clashes[a][victim]++; s.clashes[victim][a]++;                               // and it counts toward how serious this feud is
    unsigned seen = (1u << a) | (1u << victim);
    for (int w = 0; w < s.n; w++) if (s.alive[w] && v.loc[w] == v.loc[a]) seen |= 1u << w;
    AddEvent(s, EV_PUNCH, a, victim, -1, now, seen);
}

inline void SocialKill(Social& s, int a, int victim, float now) {
    s.alive[victim] = false;                                                      // permanent: dead for the rest of the game
    const unsigned everyone = (1u << s.n) - 1;
    AddEvent(s, EV_KILL, a, victim, -1, now, everyone);                           // word of a murder reaches everyone
    const int widow = s.partner[victim];
    if (widow != -1) { s.partner[widow] = -1; s.crush[widow] = -1; s.affinity[widow][a] = -100; s.chemistry[widow][a] = -1; }   // a partner never forgives
    s.partner[victim] = s.crush[victim] = -1;
    for (int w = 0; w < s.n; w++) {
        if (s.crush[w] == victim) s.crush[w] = -1;
        if (w == a || w == victim || !s.alive[w]) continue;
        const float loved = s.affinity[w][victim];
        const float drop = loved > 30 ? 70.0f : (loved < -30 ? 10.0f : 25.0f);    // friends take it hardest; enemies shrug
        Sour(s, w, a, drop);
    }
    const int ownPartner = s.partner[a];
    if (ownPartner != -1) Sour(s, ownPartner, a, 30);
}

// `a` has reached `victim` and swings. Applies all the consequences and logs the event.
inline Attack SocialAttack(Social& s, const SocialView& v, int a, int victim, float now) {
    if (!s.alive[a] || !s.alive[victim]) return ATTACK_NONE;
    if (SocialWillKill(s, a, victim)) { SocialKill(s, a, victim, now); return ATTACK_KILL; }
    SocialPunch(s, v, a, victim, now);
    return ATTACK_PUNCH;
}

// ---------------- the player stirring things up (gossip questions) ----------------
// An NPC asks "who said / did X?" and the player names someone. kind 0 = a nasty rumor, 1 = a kind deed,
// 2 = a secret admirer. Whoever the player names becomes the asker's enemy, friend, or (repeatedly) lover.
inline void SocialRumor(Social& s, int asker, int target, int kind) {
    if (asker == target || !s.alive[asker] || !s.alive[target]) return;
    if (kind == 0) Sour(s, asker, target, 45);
    else Warm(s, asker, target, kind == 2 ? 50.0f : 25.0f);
}