// Defined characters (see ARCHETYPES) with random but repeatable life details. Every new game rolls a seed; the same seed always produces the same cast
// (the seed is saved with the game, so loading a save brings back the same people). No raylib here, so tests.cpp can run it.
//
// Each character gets: a job, a favorite food, a hobby, a dream, a fear, a view of the human world, a picture of what the
// outside world is like, a way of speaking, an ongoing storyline ("premise"), and personality numbers. Values are drawn
// WITHOUT replacement, so no two characters share a job, a hobby, a voice, and so on.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

struct Persona {
    std::string job, food, hobby, dream, fear, humans, outside, voice, premise;
    std::string dialect, quirk, tic, nickname;                      // how they talk, a habit that shows in their talk, a tag they tack onto sentences, what they call a friend
    float curiosity = 0.5f, friendliness = 0.5f, patience = 0.5f;   // 0.15 .. 0.95
    float romantic = 0.5f;                                          // 0.10 .. 1.0: how easily they develop a crush
    int favColor = -1, dislikeColor = -1;                           // index 0..4 into the player's color list, or -1 for none
};

namespace persona_data {
static const char* const JOBS[] = {"a baker", "a fisherman", "a librarian", "a night watchman", "a traveling musician", "a gardener", "a mapmaker", "a clockmaker",
    "a beekeeper", "a lamplighter", "a potter", "a storyteller", "an innkeeper", "a bridge builder", "a messenger", "a weaver", "a candle maker", "a bell ringer"};
static const char* const FOODS[] = {"spicy noodles", "warm apple pie", "roasted chestnuts", "pumpkin soup", "chocolate cake", "strawberry ice cream", "grilled fish",
    "honey toast", "cheese dumplings", "mango rice", "blueberry pancakes", "mushroom stew", "fried rice balls", "lemon tarts", "sweet potato fries", "garlic bread"};
static const char* const HOBBIES[] = {"painting tiny pictures on stones", "collecting colorful pebbles", "knitting long scarves", "counting the stars", "whistling to the birds",
    "folding paper boats", "pressing flowers into a book", "carving little wooden animals", "practicing card tricks", "keeping a diary of dreams", "racing snails",
    "stacking rocks into towers", "writing songs nobody has heard", "chasing butterflies", "building tiny houses out of twigs", "making up names for clouds"};
static const char* const DREAMS[] = {"seeing the sea", "opening a shop in the human world", "drawing a map of the whole world", "hearing music from far away", "building a lighthouse",
    "growing a tree taller than the roof", "walking past the edge of the village", "flying a kite higher than anyone ever has", "tasting every food there is",
    "finding the place where rainbows begin", "meeting a real human", "hearing a thunderstorm from the top of a hill", "writing a book everyone reads", "having a quiet garden of their own"};
static const char* const FEARS[] = {"the dark", "deep water", "being forgotten", "thunder", "crowds", "heights", "silence", "getting lost", "spiders", "running out of ideas",
    "being laughed at", "the edge of the village"};
static const char* const HUMANS[] = {"thinks humans are giants who paint the sky", "believes humans eat nothing but pizza", "wonders if humans really carry glowing phones",
    "does not trust humans", "thinks humans would adore their cooking", "imagines humans live on giant boats", "is sure humans never sleep",
    "thinks humans talk to little glowing rectangles", "believes humans can fly in metal birds", "thinks humans collect everything they can find",
    "is convinced humans are very loud", "imagines humans are always in a hurry", "believes humans have a word for every color", "suspects humans are secretly shy"};
static const char* const OUTSIDE_VIEWS[] = {"an endless meadow where the sky touches the ground", "a giant market where everyone sells something strange", "a place with no walls and no doors",
    "a city that glows all night", "a sea of tall grass with a single tree", "a desert that sings when the wind blows", "mountains so tall they hold up the clouds",
    "a river so wide you cannot see the other side", "a forest where the trees whisper secrets", "a world full of villages like this one, only different",
    "a place where it is always sunset", "an ocean that goes on forever", "a land where the ground is made of music", "a floating island that drifts with the wind"};
static const char* const PREMISES[] = {"is secretly preparing a surprise for the whole village", "is trying to finish a big project before the harvest festival",
    "is sure someone has been leaving small gifts around the village and wants to find out who", "is planning a trip to find out what lies past the edge of the village",
    "has been having the same strange dream every night and is trying to understand it", "is trying to patch things up with someone they argued with long ago",
    "made a promise to a friend and is trying to keep it", "found a mysterious old key and is hunting for the lock", "is writing a letter to a human and does not know how to send it",
    "is training for the big village race against a rival", "has a secret recipe they are afraid to share", "is building something strange in the shed that they refuse to talk about",
    "lost something precious and is searching everywhere for it", "wants to impress someone they admire and is overthinking it",
    "keeps hearing a faint melody from beyond the edge of the village", "is trying to learn a difficult new skill and keeps failing in funny ways"};
// Each color is a DEFINED character with a personality and a way of talking (these are original archetypes), indexed by character slot:
// 0 Yellow, 1 Orange, 2 Green, 3 Red, 4 Blue, 5 Brown, 6 Silver, 7 Magenta. Jobs, hobbies, fears and storylines are still rolled fresh every game.
struct Archetype {
    const char *voice, *dialect, *quirk, *tic, *nickname;   // personality, how they talk, a habit, a tag tacked onto sentences, their pet name for friends
    float curiosity, friendliness, patience, romantic;
};
static const Archetype ARCHETYPES[] = {
    {"a performative pick-me emo e-boy who constantly puts himself down",
     "talks like a moody emo guy: lowercase, 'ughh', sighs, trailing off..., constant self-deprecation ('ughh girls dont wanna date nice matcha lover guys like me, my life is meaningless'), brings up his badass lone werewolf persona and his sad emo playlist, uses MASCULINE slang only (bro, dude, poser, underground, nettspend, bruh, man, fr, twin, ngl, gng, lowkey) and never feminine slang (no bestie, tea), melodramatic",
     "puts himself down after every compliment and quotes his sad playlist", ", ughh.", "bro", 0.50f, 0.50f, 0.30f, 0.95f},
    {"a grand, self-important polymath who adores their own wisdom", "speaks in flowery Renaissance English (thou, hark, verily, forsooth, 'tis)", "quotes philosophers who may not exist", ", verily.", "dear fellow", 0.90f, 0.60f, 0.80f, 0.30f},
    {"a pedantic, anime-obsessed discord moderator who corrects everyone and loves threatening people with mod powers",
     "talks like a nerd-emoji discord mod: starts with 'ermmm actually', fancy words used slightly wrong, lowercase with typos and no apostrophes, anime-obsessed ('my splendid anime muse isnt a figure its my girlfirend'), ominous warnings ('or youll see my dark side'), sometimes ends with :nerd:; openings ramble in one long run-on of about 22 words",
     "treats every disagreement like a rules violation", ", ermmm actually.", "user", 0.70f, 0.50f, 0.30f, 0.30f},
    {"a cocky, confident rapper with a big heart", "talks like a rapper (rhymes and slang: yo, fam, no cap, bussin')", "turns everyday things into freestyle bars", ", no cap.", "dawg", 0.50f, 0.70f, 0.30f, 0.50f},
    {"a chronically online, hyperactive chaotic kpop stan who treats every chat like a group chat, has insanely creative reads and insults",
     "types like a chronically online person with intentionally bad grammar: uwu, haiii, u/ur/sry, endddd, xd rofl kek kek, etc., run-ons with no punctuation, whacky ASCII emoticons (>_< :3 OwO ^_^), blames 'my self diagnosed adhd kicking in' for tangents, calls people problematic for silly reasons, invites everyone to the furrycon; openings ramble in about a 35 word run-on, but isn't that repetitive with her dialogue vocab",
     "talks about her yumeship, manhwa, etc.", ", kek kek.", "chile", 0.80f, 0.90f, 0.60f, 0.60f},
    {"an old fashioned dad who is permanently sick of everyone's BS", "talks like a grumpy old dad: calls people buster/pal/sport, mangles idioms ('hey buster u barking around the wrong horse'), grumbles about kids these days, taxes and gas prices, 'back in my day', sighs 'Lord give me strength'",
     "threatens to turn the car around", ", buster.", "sport", 0.30f, 0.35f, 0.25f, 0.20f},
    {"cool, precise and robotic, fascinated by human feelings", "speaks like a robot: stiff exact sentences, no contractions, cites percentages and exact numbers", "analyzes emotions like data and gets them slightly wrong", ", per my calculations.", "human", 0.95f, 0.40f, 0.80f, 0.40f},
      {"a sassy, witty, girly glamour queen, loves drama, incredibly dumb, flirty with everyone and a total airhead", "talks like a sassy, over-the-top girly TikTok queen: period, ate, oop, LA, my hydroflasksksks, campf, fierce hunnie, lip filler, boots, mama, yass, slay, babes, quick witty clapbacks, along with other creative slang words, uses many creative puns, brags about how she looks today, dramatic gasps, flirts shamelessly, mixes up words, gets distracted",
     "turns every compliment or insult into a clapback", ", period.", "babes", 0.70f, 0.80f, 0.20f, 0.90f},
};
template <class T, size_t N> constexpr size_t Count(const T (&)[N]) { return N; }
}  // namespace persona_data

// A tiny xorshift generator so the same seed gives the same cast on every platform (the std distributions are not portable).
struct PersonaRng {
    uint32_t s;
    uint32_t next() { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return s; }
    float unit() { return (next() & 0xFFFFFF) / 16777216.0f; }
    int pick(int n) { return (int)(next() % (uint32_t)n); }
};

inline std::vector<Persona> GeneratePersonas(uint32_t seed, int count) {
    using namespace persona_data;
    PersonaRng r{seed ? seed : 1u};
    std::vector<Persona> out(count);
    auto order = [&](size_t n) {   // a shuffled 0..n-1: taking the first `count` gives values with no repeats
        std::vector<int> v(n);
        for (size_t i = 0; i < n; i++) v[i] = (int)i;
        for (size_t i = n - 1; i > 0; i--) std::swap(v[i], v[r.next() % (uint32_t)(i + 1)]);
        return v;
    };
    auto fill = [&](const char* const* pool, size_t n, std::string Persona::*field) {
        std::vector<int> o = order(n);
        for (int i = 0; i < count; i++) out[i].*field = pool[o[i % n]];
    };
    fill(JOBS, Count(JOBS), &Persona::job);          fill(FOODS, Count(FOODS), &Persona::food);
    fill(HOBBIES, Count(HOBBIES), &Persona::hobby);  fill(DREAMS, Count(DREAMS), &Persona::dream);
    fill(FEARS, Count(FEARS), &Persona::fear);       fill(HUMANS, Count(HUMANS), &Persona::humans);
    fill(OUTSIDE_VIEWS, Count(OUTSIDE_VIEWS), &Persona::outside);
    order(16);   // (formerly the random voices: still drawn so that seeds saved by older versions keep their other details)
    fill(PREMISES, Count(PREMISES), &Persona::premise);
    for (int i = 0; i < count; i++) {
        r.unit(); r.unit(); r.unit();       // (the three personality numbers used to be drawn here - kept so saved seeds still give the same colors)
        out[i].favColor = r.pick(6) - 1;
        out[i].dislikeColor = r.pick(6) - 1;
        if (out[i].dislikeColor == out[i].favColor) out[i].dislikeColor = -1;
        const Archetype& a = ARCHETYPES[i % Count(ARCHETYPES)];
        out[i].voice = a.voice; out[i].dialect = a.dialect; out[i].quirk = a.quirk; out[i].tic = a.tic; out[i].nickname = a.nickname;
        out[i].curiosity = a.curiosity; out[i].friendliness = a.friendliness; out[i].patience = a.patience; out[i].romantic = a.romantic;
    }
    return out;
}