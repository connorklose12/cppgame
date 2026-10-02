// Unit tests. Zero dependencies (no raylib, no test framework) so it builds and runs anywhere:
//   g++ -std=c++17 tests.cpp -o tests && ./tests
#include "jsonutil.h"
#include "npc_brain.h"
#include "social.h"
#include "persona.h"
#include "dynamics.h"
#include "predictions.h"
#include <set>
#include <cstdio>
#include <cmath>

static int failures = 0;

void check(bool condition, const char* name) {
    if (condition) printf("PASS: %s\n", name);
    else { printf("FAIL: %s\n", name); failures++; }
}

// A world where everyone is in the same room, standing together, unless a test moves them.
static SocialView TogetherView(int n) {
    SocialView v = {};
    for (int i = 0; i < n; i++) { v.loc[i] = 1; v.x[i] = i * 0.5f; v.z[i] = 0; v.patience[i] = 0.5f; }
    return v;
}
static Social BlankSocial(int n, unsigned seed) {
    Social s; SocialInit(s, n, seed);
    for (int i = 0; i < MAX_NPCS; i++) for (int j = 0; j < MAX_NPCS; j++) s.chemistry[i][j] = 0;   // no random feelings
    s.drama = false;                                                                                  // and no random drama
    return s;
}
static int CountEvents(const Social& s, EventType t) {
    int c = 0; for (const SocialEvent& e : s.events) if (e.type == t) c++; return c;
}

int main() {
    // ---------------- JSON helpers ----------------
    std::string j = "{\"walkSpeed\": 7.5, \"playerColor\": \"#8C46C8\", \"happy\": [\"Hi!\", \"Hello there.\"]}";
    check(JNum(j, "walkSpeed", 1) == 7.5f, "JNum reads a number");
    check(JNum(j, "missingKey", 42) == 42, "JNum falls back to the default when the key is absent");
    RGB c = JColor(j, "playerColor", {0, 0, 0, 255});
    check(c.r == 0x8C && c.g == 0x46 && c.b == 0xC8, "JColor parses a hex color");
    RGB fallback = JColor(j, "missingKey", {1, 2, 3, 255});
    check(fallback.r == 1 && fallback.g == 2 && fallback.b == 3, "JColor falls back to the default when the key is absent");
    std::vector<std::string> lines = JStrArray(j, "happy");
    check(lines.size() == 2 && lines[0] == "Hi!" && lines[1] == "Hello there.", "JStrArray reads the lines");
    check(JStrArray(j, "missingKey").empty(), "JStrArray returns empty for a missing key");

    std::string multi = "{\"elder\": {\"happy\": [\"Hi, {playerName}!\"]}, \"villager\": {\"happy\": [\"Hey {playerName}\"]}}";
    size_t start = multi.find('{', multi.find("\"elder\""));
    std::string block = multi.substr(start, FindMatchingBrace(multi, start) - start + 1);
    check(JStrArray(block, "happy").size() == 1 && block.find("villager") == std::string::npos,
          "FindMatchingBrace stops at the right brace even when {playerName} appears inside the object");

    // ---------------- text helpers ----------------
    check(FillName("Hello, {playerName}!", "Kate") == "Hello, Kate!", "FillName substitutes the player's name");
    check(ReplaceAll("{a} loves {b}, {a}!", "{a}", "Ruby") == "Ruby loves {b}, Ruby!", "ReplaceAll replaces every occurrence");
    check(Split("a|bb||c", '|').size() == 4 && Split("a|bb||c", '|')[1] == "bb" && Split("a|bb||c", '|')[2].empty(), "Split cuts a string at every separator");
    check(CleanGenerated("Hello there, my friend!\" Then more junk") == "Hello there, my friend!", "CleanGenerated cuts at the closing quote");
    check(CleanGenerated("  Nice day today. I really love it\nsecond line") == "Nice day today. I really love it", "CleanGenerated keeps the whole line and drops extra lines");
    check(CleanGenerated("uwu haiii!! do u look like my yumeship?? xd :3") == "uwu haiii!! do u look like my yumeship?? xd :3", "CleanGenerated keeps trailing emoticons and slang");
    check(CleanGenerated("\"\"") == "", "CleanGenerated copes with an empty line");
    check(CleanGenerated("\"I bake bread every morning,\" said the baker") == "I bake bread every morning.", "CleanGenerated turns a dangling comma into a full stop");
    check(CleanGenerated("\"Hello there, friend!\" she said") == "Hello there, friend!", "CleanGenerated removes quotes a chat model wraps around its reply");
    check(CleanGenerated("Ruby (calm) says to Traveler: \"I bake bread every morning.\"") == "I bake bread every morning.", "CleanGenerated drops a 'Name says to X:' label");
    check(CleanGenerated("\nBlake (calm) says to Ruby: \"Another speaker's line.\"") == "", "CleanGenerated never borrows the NEXT speaker's line when GPT-2 starts a new line");
    check(LooksUsable("Nice to see you again!"), "LooksUsable accepts a normal short line");
    check(!LooksUsable("hi"), "LooksUsable rejects text that's too short");
    check(!LooksUsable(std::string(300, 'a')), "LooksUsable rejects text that's too long");
    check(LooksUsable("uwu haiiii has anyone said u look like my yumeship xd rofl anyways u coming to the furrycon w me next week sry just my self diagnosed adhd kicking in kek kek >_< ok ur problematic for not coming"), "LooksUsable accepts a long rambling run-on line (about 210 characters)");
    check(CountWords("Silly me!") == 2 && CountWords("  I like   noodles a lot ") == 5, "CountWords counts words");
    check(MentionsAny("I bake bread before sunrise", {"baker", "bread"}) && !MentionsAny("Silly me!", {"baker", "bread"}), "MentionsAny finds a keyword and rejects off-topic lines");
    check(MentionsAny("BREAD is life", {"bread"}), "MentionsAny ignores case");
    check(!MentionsAny("anything", {}), "MentionsAny with no keywords matches nothing");
    check(!LineIsGood("Silly me!", false, {}), "LineIsGood rejects the kind of non-answer a weak model gives (too short)");
    check(!LineIsGood("I think the weather is lovely today.", true, {}), "LineIsGood rejects a statement when a question was required");
    check(LineIsGood("Do you know who said that about me?", true, {}), "LineIsGood accepts a real question");
    check(!LineIsGood("The weather is lovely today, friend.", false, {"baker"}) && LineIsGood("Being a baker means early mornings, friend.", false, {"baker"}), "LineIsGood requires the line to be on topic");
    check(!LooksUsable("the the the the the the"), "LooksUsable rejects text stuck repeating one word");
    check(!LooksUsable("you are a fucking idiot"), "LooksUsable rejects blocked words");
    check(!LooksUsable("go to www.example.com now"), "LooksUsable rejects links");

    // ---------------- the trained NPC brain ----------------
    BrainOutput favorite = RunBrain({0.8f, 0.9f, 0.5f, 0, 2, 1, 0, 0.5f, 0});
    check(fabsf(favorite.opinionDelta - 8.243f) < 0.05f, "brain: friendly NPC + favorite color gives a strongly positive delta");
    BrainOutput happyAns = RunBrain({0.8f, 0.9f, 0.5f, 0, 2, 0, 0, 0.5f, 1});
    BrainOutput madAns = RunBrain({0.8f, 0.9f, 0.5f, 0, 2, 0, 0, 0.5f, -1});
    check(happyAns.opinionDelta > 5.0f, "brain: a happy answer makes an NPC like you a lot more");
    check(madAns.opinionDelta < happyAns.opinionDelta - 8.0f, "brain: a mad answer is received far worse than a happy one");
    BrainOutput hostile = RunBrain({0.3f, 0.3f, 0.1f, -60, 10, -1, 1, 0.5f, -1});
    check(hostile.opinionDelta < -8.0f && hostile.punchChance > 0.8f, "brain: impatient NPC + disliked color + hostile + rude answer = very negative and likely to shove");

    // ---------------- NPC relationships ----------------
    {   // mutual chemistry -> crushes -> dating
        Social s = BlankSocial(4, 7);
        s.chemistry[0][1] = s.chemistry[1][0] = 1.0f;
        SocialView v = TogetherView(4);
        for (int t = 0; t < 120; t++) SocialTick(s, v, (float)t);
        check(s.crush[0] == 1 && s.crush[1] == 0, "social: two NPCs who click both develop crushes on each other");
        check(s.partner[0] == 1 && s.partner[1] == 0 && CountEvents(s, EV_DATING) == 1, "social: mutual crushes turn into exactly one dating event");
    }
    {   // one-sided chemistry stays a secret crush
        Social s = BlankSocial(3, 9);
        s.chemistry[0][1] = 1.0f;
        SocialView v = TogetherView(3);
        for (int t = 0; t < 120; t++) SocialTick(s, v, (float)t);
        check(s.crush[0] == 1 && s.partner[0] == -1, "social: an unreturned crush does not become a relationship");
        check(!Knows(s.events[0], 1) && Knows(s.events[0], 0), "social: a crush starts out secret (only the crusher knows)");
    }
    {   // cheating, a witness telling the betrayed partner, and the breakup
        Social s = BlankSocial(4, 11);
        s.partner[0] = 1; s.partner[1] = 0;
        s.affinity[0][1] = s.affinity[1][0] = 80;
        s.chemistry[0][2] = 1.0f;
        SocialView v = TogetherView(4);
        v.loc[1] = 2;                 // 1 (the partner) is in another room
        v.patience[0] = 0.0f;         // 0 has no loyalty
        for (int t = 0; t < 600 && CountEvents(s, EV_CHEAT) == 0; t++) SocialTick(s, v, (float)t);
        check(CountEvents(s, EV_CHEAT) == 1, "social: a partnered NPC alone with someone they like eventually cheats");
        check(Knows(s.events[s.events.size() - 1], 3) || Knows(s.events[0], 3), "social: a bystander in the room knows about the affair");
        for (int t = 0; t < 50; t++) SocialTick(s, v, 300.0f + t);
        check(CountEvents(s, EV_CHEAT) == 1, "social: the same affair is not logged over and over");
        v.loc[3] = 2;                 // the witness walks into the partner's room
        for (int t = 0; t < 800 && CountEvents(s, EV_DISCOVER) == 0; t++) SocialTick(s, v, 400.0f + t);
        check(CountEvents(s, EV_DISCOVER) == 1, "social: a witness tells the betrayed partner");
        check(s.partner[0] == -1 && s.partner[1] == -1, "social: finding out ends the relationship");
        check(s.affinity[1][0] < 20 && s.affinity[1][2] < 0, "social: the betrayed NPC now dislikes both the cheater and the other person");
    }
    {   // a couple that drifts apart breaks up on its own
        Social s = BlankSocial(2, 5);
        s.partner[0] = 1; s.partner[1] = 0; s.affinity[0][1] = s.affinity[1][0] = 15;
        SocialView v = TogetherView(2);
        SocialTick(s, v, 1.0f);
        check(CountEvents(s, EV_BREAKUP) == 1 && s.partner[0] == -1, "social: a couple with fading feelings breaks up");
    }
    {   // hitting someone with the stick
        Social s = BlankSocial(5, 3);
        s.affinity[2][1] = -50;                       // 2 is 1's enemy
        s.affinity[3][1] = 50;                        // 3 is 1's friend
        s.partner[4] = 1; s.partner[1] = 4;           // 4 is 1's partner
        bool witness[MAX_NPCS] = {true, true, true, true, true};
        HitResult r = SocialHit(s, 1, witness);
        check(r.opinionDelta[1] < 0 && r.reaction[1] == REACT_CONFRONT, "hit: the victim is angry at you");
        check(r.opinionDelta[2] > 0 && r.reaction[2] == REACT_THANK, "hit: the victim's enemy is happy and wants to thank you");
        check(r.opinionDelta[3] < 0 && r.reaction[3] == REACT_CONFRONT, "hit: the victim's friend is angry");
        check(r.opinionDelta[4] < r.opinionDelta[3] && r.reaction[4] == REACT_CONFRONT, "hit: the victim's partner is angrier than a friend");
        check(r.reaction[0] == REACT_NONE && r.opinionDelta[0] < 0, "hit: a stranger is mildly put off");
        bool away[MAX_NPCS] = {false, false, false, false, false};
        HitResult far = SocialHit(s, 1, away);
        check(fabsf(far.opinionDelta[2]) < fabsf(r.opinionDelta[2]), "hit: NPCs who only heard about it react less than the ones who saw it");
    }
    {   // NPC vs NPC: a really angry NPC goes after their enemy, and a punch makes the victim madder
        Social s = BlankSocial(3, 21);
        SocialView v = TogetherView(3);
        s.affinity[0][1] = -60;
        int target = -1;
        for (int t = 0; t < 1000 && target == -1; t++) target = SocialPickTarget(s, v, 0);
        check(target == 1, "fight: an NPC who is really mad at someone picks them as a target");
        check(SocialPickTarget(s, v, 2) == -1, "fight: a calm NPC picks nobody");
        float before = s.affinity[1][0];
        Attack a = SocialAttack(s, v, 0, 1, 5.0f);
        check(a == ATTACK_PUNCH && CountEvents(s, EV_PUNCH) == 1, "fight: a moderately angry NPC punches (no kill)");
        check(s.affinity[1][0] == before - 25, "fight: getting punched makes the victim madder at the attacker");
        check(s.alive[1], "fight: NPCs survive punches");
        check(fabsf(s.chemistry[1][0] - (-0.2f)) < 1e-4f, "fight: a punch also lowers the victim's long-term feelings, not just today's");
        v.loc[1] = 2;   // victim leaves the room
        bool picked = false;
        for (int t = 0; t < 1000; t++) if (SocialPickTarget(s, v, 0) != -1) picked = true;
        check(!picked, "fight: you can't go after someone who isn't in your room");
    }
    {   // grudges stick: a friendship that gets punched enough turns into a feud even if they keep standing together
        Social s = BlankSocial(2, 61);
        s.chemistry[1][0] = 0.5f; s.affinity[1][0] = 40;
        SocialView v = TogetherView(2);
        for (int p = 0; p < 5; p++) SocialAttack(s, v, 0, 1, (float)p);
        for (int t = 0; t < 100; t++) SocialTick(s, v, 10.0f + t);
        check(s.affinity[1][0] < -30, "fight: after enough punches the victim stays angry even when they keep standing together");
    }
    {   // provoking: a short-tempered NPC who is only slightly annoyed sometimes starts a fight
        Social s = BlankSocial(2, 33);
        SocialView v = TogetherView(2);
        v.patience[0] = 0.0f;
        s.affinity[0][1] = -20;
        bool provoked = false;
        for (int t = 0; t < 3000 && !provoked; t++) provoked = SocialPickTarget(s, v, 0) == 1;
        check(provoked, "fight: a short-tempered, mildly annoyed NPC will sometimes provoke a fight");
        int hotCount = 0, calmCount = 0;   // same situation, same dice: how often does each personality start a fight?
        Social hot = BlankSocial(2, 77), calm = BlankSocial(2, 77);
        SocialView hv = TogetherView(2), cv = TogetherView(2);
        hv.patience[0] = 0.0f; cv.patience[0] = 1.0f;
        hot.affinity[0][1] = calm.affinity[0][1] = -20;
        for (int t = 0; t < 20000; t++) {
            if (SocialPickTarget(hot, hv, 0) != -1) hotCount++;
            if (SocialPickTarget(calm, cv, 0) != -1) calmCount++;
        }
        check(hotCount > calmCount * 3 && hotCount > 50, "fight: patient NPCs start far fewer fights than short-tempered ones");
    }
    {   // murder and its fallout
        Social s = BlankSocial(5, 41);
        SocialView v = TogetherView(5);
        s.partner[1] = 2; s.partner[2] = 1;               // victim 1 is dating 2
        s.affinity[3][1] = 60;                            // 3 is the victim's friend
        s.affinity[4][1] = -60;                           // 4 hated the victim
        s.crush[3] = 1;
        s.affinity[0][1] = -100;                          // killer 0 is beyond furious
        s.clashes[0][1] = s.clashes[1][0] = CLASHES_BEFORE_KILL;   // ...and they have a history of fighting
        Attack a = ATTACK_NONE;
        for (int t = 0; t < 200 && a != ATTACK_KILL; t++) a = SocialAttack(s, v, 0, 1, 9.0f);
        check(a == ATTACK_KILL && !s.alive[1], "kill: someone who is far enough gone kills their enemy");
        check(CountEvents(s, EV_KILL) == 1 && Knows(s.events.back(), 3) && Knows(s.events.back(), 4) && Knows(s.events.back(), 2), "kill: everyone knows about the murder");
        check(s.partner[2] == -1 && s.affinity[2][0] == -100 && s.chemistry[2][0] == -1, "kill: the victim's partner is single again and will never forgive the killer");
        check(s.crush[3] == -1, "kill: crushes on the dead person disappear");
        check(s.affinity[3][0] <= -70, "kill: the victim's friend now despises the killer");
        check(s.affinity[4][0] > s.affinity[3][0], "kill: the victim's enemy is less upset than the victim's friend");
        check(SocialAttack(s, v, 0, 1, 10.0f) == ATTACK_NONE, "kill: you can't attack someone who is already dead");
        // the dead are out of the social world
        SocialView together = TogetherView(5);
        float aff = s.affinity[3][1];
        for (int t = 0; t < 20; t++) SocialTick(s, together, 20.0f + t);
        check(s.affinity[3][1] == aff && s.partner[1] == -1, "kill: a dead NPC's feelings and relationships stop changing");
        bool targetsDead = false;
        for (int t = 0; t < 200; t++) if (SocialPickTarget(s, together, 3) == 1) targetsDead = true;
        check(!targetsDead, "kill: nobody goes after the dead");
        bool witness[MAX_NPCS] = {true, true, true, true, true};
        HitResult r = SocialHit(s, 2, witness);
        check(r.reaction[1] == REACT_NONE && r.opinionDelta[1] == 0, "kill: the dead don't react when you hit someone");
    }
    {   // gossip questions: the player names someone, and the asker's feelings about them change for good
        Social s = BlankSocial(3, 51);
        SocialView v = TogetherView(3);
        SocialRumor(s, 0, 1, 0);
        check(s.affinity[0][1] == -45 && s.chemistry[0][1] < 0, "rumor: naming someone as the one who insulted you makes you dislike them");
        SocialRumor(s, 0, 2, 1);
        check(s.affinity[0][2] == 25 && s.chemistry[0][2] > 0, "rumor: naming someone as the kind one makes you warm to them");
        for (int t = 0; t < 40; t++) SocialTick(s, v, (float)t);
        check(s.affinity[0][1] < -20, "rumor: the grudge doesn't fade just because they stand near each other");
        check(s.crush[0] != 2, "rumor: one kind deed makes a friend, not a crush");
        Social love = BlankSocial(3, 52);
        SocialRumor(love, 0, 2, 2);
        for (int t = 0; t < 5; t++) SocialTick(love, v, (float)t);
        check(love.crush[0] == 2, "rumor: a 'secret admirer' rumor makes the asker fall for whoever you name");
        SocialRumor(love, 2, 0, 2);
        for (int t = 0; t < 5; t++) SocialTick(love, v, 10.0f + t);
        check(love.partner[0] == 2 && love.partner[2] == 0, "rumor: do it for both of them and you've made a couple");
        Social dead = BlankSocial(2, 53);
        dead.alive[1] = false;
        SocialRumor(dead, 0, 1, 0);
        check(dead.affinity[0][1] == 0, "rumor: the dead can't be blamed");
    }
    {   // random drama: left alone, NPCs will sometimes just start disliking each other
        Social s; SocialInit(s, 6, 99);
        for (int i = 0; i < MAX_NPCS; i++) for (int j = 0; j < MAX_NPCS; j++) s.chemistry[i][j] = 0;
        SocialView v = TogetherView(6);
        for (int i = 0; i < 6; i++) v.loc[i] = i;   // never together, so only the random drama can change anything
        for (int t = 0; t < 2000; t++) SocialTick(s, v, (float)t);
        int changed = 0;
        for (int i = 0; i < 6; i++) for (int j = 0; j < 6; j++) if (i != j && s.chemistry[i][j] != 0) changed++;
        check(changed > 5, "drama: over time, NPCs spontaneously start liking or disliking each other");
    }
    {   // kill is decided before it happens, so the game can darken the screen first
        Social s = BlankSocial(2, 5);
        s.affinity[0][1] = -100;
        s.clashes[0][1] = s.clashes[1][0] = CLASHES_BEFORE_KILL;
        bool willKill = false;
        for (int t = 0; t < 200 && !willKill; t++) willKill = SocialWillKill(s, 0, 1);
        check(willKill && s.alive[1] && s.events.empty(), "kill: the roll alone changes nothing (the knife hasn't come down yet)");
        SocialKill(s, 0, 1, 3.0f);
        check(!s.alive[1] && s.events.size() == 1 && s.events[0].type == EV_KILL, "kill: SocialKill is what actually does it");
        Social mild = BlankSocial(2, 5);
        mild.affinity[0][1] = -60;
        mild.clashes[0][1] = CLASHES_BEFORE_KILL;
        bool any = false;
        for (int t = 0; t < 200; t++) if (SocialWillKill(mild, 0, 1)) any = true;
        check(!any, "kill: merely angry NPCs never kill - only the ones who are far gone");
    }
    {   // a feud has to escalate: no killing out of the blue, however angry someone is
        Social s = BlankSocial(2, 8);
        s.affinity[0][1] = -100;
        bool early = false;
        for (int t = 0; t < 2000; t++) if (SocialWillKill(s, 0, 1)) early = true;
        check(!early, "escalation: someone furious but with no history of fighting cannot kill");
        SocialView v = TogetherView(2);
        for (int p = 0; p < CLASHES_BEFORE_KILL; p++) { s.affinity[0][1] = -100; SocialPunch(s, v, 1, 0, (float)p); }   // 1 keeps punching 0
        s.affinity[0][1] = -100;
        bool later = false;
        for (int t = 0; t < 2000 && !later; t++) later = SocialWillKill(s, 0, 1);
        check(later, "escalation: after enough clashes the feud can turn deadly");
    }
    {   // learning from each other: you pick up the views of the people you like
        Social s = BlankSocial(3, 1);
        s.affinity[0][1] = 100;
        float opinion[MAX_NPCS] = {0, 60, -60};
        GossipOpinions(s, opinion);
        check(opinion[0] > 0 && opinion[0] < 60, "gossip: an NPC's opinion of you moves toward their close friend's");
        check(opinion[2] == -60, "gossip: an NPC with no friends is not influenced");
    }


    // ---------------- random personalities ----------------
    {
        std::vector<Persona> a = GeneratePersonas(12345, 8), b = GeneratePersonas(12345, 8), c = GeneratePersonas(54321, 8);
        bool same = true, differs = false;
        for (int i = 0; i < 8; i++) {
            same = same && a[i].job == b[i].job && a[i].voice == b[i].voice && a[i].premise == b[i].premise && a[i].curiosity == b[i].curiosity;
            differs = differs || a[i].job != c[i].job || a[i].voice != c[i].voice;
        }
        check(same, "persona: the same seed always produces the same cast (so a save reloads the same people)");
        check(differs, "persona: a different seed produces a different cast");

        bool allUnique = true, allFilled = true, boundsOk = true, colorsOk = true;
        for (unsigned seed = 1; seed <= 300; seed++) {   // many seeds: the rules must hold for every one
            std::vector<Persona> p = GeneratePersonas(seed * 7919u, 8);
            std::set<std::string> jobs, foods, hobbies, dreams, fears, humans, outside, voices, premises;
            for (const Persona& x : p) {
                jobs.insert(x.job); foods.insert(x.food); hobbies.insert(x.hobby); dreams.insert(x.dream); fears.insert(x.fear);
                humans.insert(x.humans); outside.insert(x.outside); voices.insert(x.voice); premises.insert(x.premise);
                allFilled = allFilled && !x.job.empty() && !x.food.empty() && !x.hobby.empty() && !x.dream.empty() && !x.fear.empty() &&
                            !x.humans.empty() && !x.outside.empty() && !x.voice.empty() && !x.premise.empty();
                for (float v : {x.curiosity, x.friendliness, x.patience}) boundsOk = boundsOk && v >= 0.15f && v <= 0.95f;
                colorsOk = colorsOk && x.favColor >= -1 && x.favColor <= 4 && x.dislikeColor >= -1 && x.dislikeColor <= 4 && (x.favColor < 0 || x.favColor != x.dislikeColor);
            }
            allUnique = allUnique && jobs.size() == 8 && foods.size() == 8 && hobbies.size() == 8 && dreams.size() == 8 && fears.size() == 8 &&
                        humans.size() == 8 && outside.size() == 8 && voices.size() == 8 && premises.size() == 8;
        }
        check(allUnique, "persona: no two characters share a job, food, hobby, dream, fear, view, outside-world picture, voice or storyline (300 seeds)");
        check(allFilled, "persona: every field is filled in");
        check(boundsOk, "persona: personality numbers stay inside 0.15 .. 0.95");
        check(colorsOk, "persona: favorite and disliked colors are valid and never the same color");
        std::set<std::string> firstJobs;
        for (unsigned seed = 1; seed <= 60; seed++) firstJobs.insert(GeneratePersonas(seed * 31u, 8)[0].job);
        check(firstJobs.size() >= 8, "persona: the same character slot gets genuinely different jobs across games");
    }
    {   // defined characters: the same personality in the same slot, whatever the seed
        bool fixed = true;
        for (unsigned seed = 1; seed <= 100; seed++) {
            std::vector<Persona> p = GeneratePersonas(seed * 7777u, 8);
            fixed = fixed && p[3].dialect.find("rapper") != std::string::npos && p[1].dialect.find("Renaissance") != std::string::npos &&
                    p[2].dialect.find("discord mod") != std::string::npos && p[7].dialect.find("TikTok") != std::string::npos && p[5].dialect.find("dad") != std::string::npos && p[6].dialect.find("robot") != std::string::npos &&
                    p[3].voice == GeneratePersonas(1u, 8)[3].voice;
        }
        check(fixed, "persona: every slot keeps its defined character whatever the seed: Red rapper, Orange Renaissance scholar, Green discord mod, Blue chronically online, Brown grumpy dad, Silver robot, Magenta TikTok diva");
        std::vector<Persona> a = GeneratePersonas(11, 8), b = GeneratePersonas(22, 8);
        check(a[3].job != b[3].job || a[3].premise != b[3].premise, "persona: ...but their jobs and storylines still change from game to game");
    }
    {   // dialects, quirks, tics and nicknames
        bool uniq = true, filled = true, romOk = true;
        for (unsigned seed = 1; seed <= 300; seed++) {
            std::vector<Persona> p = GeneratePersonas(seed * 104729u, 8);
            std::set<std::string> d, q, t, nk;
            for (const Persona& x : p) {
                d.insert(x.dialect); q.insert(x.quirk); t.insert(x.tic); nk.insert(x.nickname);
                filled = filled && !x.dialect.empty() && !x.quirk.empty() && !x.tic.empty() && !x.nickname.empty();
                romOk = romOk && x.romantic >= 0.10f && x.romantic <= 1.0f;
            }
            uniq = uniq && d.size() == 8 && q.size() == 8 && t.size() == 8 && nk.size() == 8;
        }
        check(uniq, "persona: every character has their own dialect, quirk, verbal tic and nickname for friends (300 seeds)");
        check(filled && romOk, "persona: dialect/quirk/tic/nickname are filled in and the romantic-ness stays in range");
    }

    // ---------------- relationship dynamics ----------------
    {
        Social s; SocialInit(s, 8, 99u); s.drama = false;
        for (int i = 0; i < 8; i++) for (int j = 0; j < 8; j++) { s.affinity[i][j] = 0; s.clashes[i][j] = 0; }
        auto add = [&](EventType ty, int a, int b, int c) { s.events.push_back(SocialEvent{ty, a, b, c, 0.0f, 0xFFu, false}); };

        s.partner[0] = 1; s.partner[1] = 0; s.crush[0] = 2; s.affinity[0][1] = -90;
        check(PairDynCode(s, 0, 1) == 'p', "dynamics: a dating couple is 'dating' no matter what else is true");
        s.partner[0] = s.partner[1] = -1;
        check(PairDynCode(s, 0, 2) == 'c' && PairDynCode(s, 2, 0) == 'n', "dynamics: a crush is one-sided (the crush-ee barely notices)");
        add(EV_BREAKUP, 0, 2, -1);
        check(PairDynCode(s, 0, 2) == 'c', "dynamics: a current crush outranks having once dated");
        s.crush[0] = -1;
        check(PairDynCode(s, 0, 2) == 'x' && PairDynCode(s, 2, 0) == 'x', "dynamics: exes are exes from both sides");
        s.partner[0] = 2; s.partner[2] = 0;
        check(PairDynCode(s, 0, 2) == 'p', "dynamics: exes who got back together are dating again");
        s.partner[0] = s.partner[2] = -1;

        add(EV_DISCOVER, 3, 4, 5);   // 3 found out that 4 cheated
        check(PairDynCode(s, 3, 4) == 'h' && PairDynCode(s, 4, 3) != 'h', "dynamics: only the betrayed one is 'still hurt'");

        s.affinity[5][6] = -30; check(PairDynCode(s, 5, 6) == 'd', "dynamics: mild dislike with no fights is just wary");
        s.clashes[5][6] = s.clashes[6][5] = 1; check(PairDynCode(s, 5, 6) == 'R', "dynamics: dislike plus a fight makes them bitter rivals");
        s.clashes[5][6] = 0; s.affinity[5][6] = -50; check(PairDynCode(s, 5, 6) == 'e', "dynamics: deep dislike with no fights is enemies");
        s.clashes[5][6] = 2; check(PairDynCode(s, 5, 6) == 'R', "dynamics: any fight makes real dislike a rivalry");
        s.clashes[5][6] = 0;

        s.affinity[6][7] = 80; s.affinity[7][6] = -50;
        check(PairDynCode(s, 6, 7) == 'B' && PairDynCode(s, 7, 6) == 'e', "dynamics: feelings can differ each way (best friend vs enemy)");
        s.affinity[1][3] = 50; check(PairDynCode(s, 1, 3) == 'f', "dynamics: good friends");
        s.affinity[1][3] = 20; check(PairDynCode(s, 1, 3) == 'l', "dynamics: friendly");
        s.affinity[1][3] = 5;  check(PairDynCode(s, 1, 3) == 'n', "dynamics: barely know each other");

        std::string ph = PairDynPhrase('R', "Red", "Blue");
        check(ph.find("Red") != std::string::npos && ph.find("Blue") != std::string::npos && ph.find("rival") != std::string::npos, "dynamics: phrases name both people");
        bool phrasesOk = true; std::set<std::string> seen;
        for (char c : std::string("pchxReBfldn")) { std::string p = PairDynPhrase(c, "A", "B"); phrasesOk = phrasesOk && !p.empty(); seen.insert(p); }
        check(phrasesOk && seen.size() == 11, "dynamics: every relationship has its own distinct phrase");
        check(PairDynRank('p') < PairDynRank('c') && PairDynRank('c') < PairDynRank('R') && PairDynRank('R') < PairDynRank('B') && PairDynRank('n') == 99 && PairDynRank('l') == 99,
              "dynamics: dramatic relationships rank before friendly ones; bland ones are never 'notable'");

        // ---- how a villager sees the player ----
        auto F = [](float op, float tr, float ro, int talked, int hit) { PlayerFeel f; f.opinion = op; f.trust = tr; f.romance = ro; f.timesTalked = talked; f.timesHit = hit; return f; };
        check(PlayerDynCode(F(-70, 0, 90, 5, 0)) == 'H', "player dynamic: someone who hates you is hostile even if they once liked you");
        check(PlayerDynCode(F(5, 10, 0, 3, 1)) == 'G', "player dynamic: you hit them and they haven't warmed up: grudge");
        check(PlayerDynCode(F(50, 10, 0, 6, 1)) == 'F', "player dynamic: a grudge fades once they like you again and trust you");
        check(PlayerDynCode(F(50, -20, 0, 6, 1)) == 'G', "player dynamic: ...but not while they still don't trust you");
        check(PlayerDynCode(F(40, 20, 60, 5, 0)) == 'C', "player dynamic: a crush (romance, liking, never hit)");
        check(PlayerDynCode(F(40, 20, 60, 5, 1)) != 'C', "player dynamic: hitting someone ends the crush");
        check(PlayerDynCode(F(5, -30, 0, 4, 0)) == 'S' && PlayerDynCode(F(-20, 0, 0, 4, 0)) == 'S', "player dynamic: low trust or mild dislike = skeptical");
        check(PlayerDynCode(F(10, 0, 0, 1, 0)) == 'N' && PlayerDynCode(F(60, 0, 0, 1, 0)) == 'F', "player dynamic: a first meeting is 'just met', unless they already adore you");
        check(PlayerDynCode(F(80, 30, 0, 10, 0)) == 'K' && PlayerDynCode(F(80, 30, 0, 3, 0)) == 'F', "player dynamic: best friends need both high opinion and history");
        check(PlayerDynCode(F(25, 0, 0, 4, 0)) == 'W' && PlayerDynCode(F(0, 0, 0, 4, 0)) == 'U', "player dynamic: warming up vs unsure");
        bool all = true; std::set<std::string> pp;
        for (char c : std::string("HGCSNWFKU")) { std::string p = PlayerDynPhrase(c); all = all && !p.empty(); pp.insert(p); }
        check(all && pp.size() == 9, "player dynamic: nine different feelings, each with its own wording and tone");
        check(UsesNickname('K') && UsesNickname('F') && UsesNickname('C') && UsesNickname('W') && !UsesNickname('H') && !UsesNickname('G') && !UsesNickname('S') && !UsesNickname('N') && !UsesNickname('U'),
              "player dynamic: friends and admirers use a pet name; others use your name");

        // ---- verbal tic ----
        std::string tic = ", mark my words.";
        check(ApplyTic("Well, that was quite the storm last night.", tic, 0.1f, 0.3f) == "Well, that was quite the storm last night, mark my words.", "tic: tacks the tag on in place of the final period");
        check(ApplyTic("Well, that was quite the storm last night.", tic, 0.9f, 0.3f) == "Well, that was quite the storm last night.", "tic: only some of the time");
        check(ApplyTic("Did you see that storm last night, friend?", tic, 0.1f, 0.3f) == "Did you see that storm last night, friend?", "tic: never on a question");
        check(ApplyTic("Oh well, that is just how it goes...", tic, 0.1f, 0.3f) == "Oh well, that is just how it goes...", "tic: never after a trailing-off ellipsis");
        check(ApplyTic("Too short here.", tic, 0.1f, 0.3f) == "Too short here.", "tic: not on very short lines");
        check(ApplyTic("I said it before, mark my words that is true.", tic, 0.1f, 0.3f) == "I said it before, mark my words that is true.", "tic: not if the line already says it");
        check(ApplyTic("Well, that was quite the storm last night.", "", 0.1f, 0.3f) == "Well, that was quite the storm last night.", "tic: no tic, no change");
    }
    {   // key words of a remembered fact
        std::vector<std::string> k = KeyWords("the player's favorite animal is cat");
        check(k.size() == 3 && k[0] == "favorite" && k[1] == "animal" && k[2] == "cat", "KeyWords: keeps the meaningful words of a memory (favorite, animal, cat) and drops filler");
        check(KeyWords("the was and").empty() && KeyWords("").empty(), "KeyWords: filler-only or empty text gives nothing");
    }
    {   // the chronically-online one, and how they treat you
        std::vector<Persona> p = GeneratePersonas(5, 8);
        check(p[4].dialect.find("emoticons") != std::string::npos && p[4].dialect.find("uwu") != std::string::npos && p[4].dialect.find("@") == std::string::npos && p[4].dialect.find("#") == std::string::npos,
              "persona: Blue is the chronically online one (uwu, kek, emoticons), and never told to use @ or # (those are filtered out)");
        check(std::string(PlayerDynPhrase('H')).find("aggressive") != std::string::npos && std::string(PlayerDynPhrase('C')).find("flirts") != std::string::npos &&
              std::string(PlayerDynPhrase('G')).find("picks a fight") != std::string::npos, "player dynamic: someone who hates you is told to be aggressive, someone with a crush to flirt");
        check(LooksUsable("uwu haiii kek kek has anyone said u look like my yumeship xd") && !LooksUsable("kek kek kek kek that is funny"), "LooksUsable: allows a doubled word like 'kek kek' but not four in a row");
    }
    {   // the game font only draws plain ASCII, and the game filters @ and # - so no style text may use them
        bool clean = true;
        for (const Persona& x : GeneratePersonas(3, 8))
            for (const std::string* f : {&x.voice, &x.dialect, &x.quirk, &x.tic, &x.nickname}) {
                for (unsigned char ch : *f) clean = clean && ch < 128;
                clean = clean && f->find('@') == std::string::npos && f->find('#') == std::string::npos;
            }
        check(clean, "persona: all character style text is plain ASCII with no @ or # (the font can't draw emoji, and the filter blocks those two)");
    }
    {   // Yellow is the performative pick-me e-boy; Magenta is also flirty and dumb (everything else about her stays)
        std::vector<Persona> p = GeneratePersonas(9, 8);
        check(p[0].voice.find("pick-me") != std::string::npos && p[0].voice.find("emo") != std::string::npos && p[0].voice.find("puts himself down") != std::string::npos &&
              p[0].dialect.find("matcha") != std::string::npos && p[0].dialect.find("ughh") != std::string::npos && p[0].dialect.find("self-deprecation") != std::string::npos && p[0].dialect.find("hiyaaa") == std::string::npos,
              "persona: Yellow is the self-deprecating emo pick-me e-boy (matcha, sad playlist, constant self-put-downs) - not the old bubbly one");
        check(p[0].dialect.find("MASCULINE slang only") != std::string::npos && p[0].dialect.find("bro") != std::string::npos && p[0].dialect.find("never feminine slang") != std::string::npos && p[0].nickname == "bro",
              "persona: Yellow uses masculine slang only (bro, dude, bruh...) and calls friends 'bro'");
        check(p[7].dialect.find("TikTok") != std::string::npos && p[7].dialect.find("period") != std::string::npos && p[7].dialect.find("clapbacks") != std::string::npos &&
              p[7].voice.find("flirty") != std::string::npos && p[7].voice.find("airhead") != std::string::npos && p[7].dialect.find("mixes up words") != std::string::npos,
              "persona: Magenta keeps her TikTok/sassy/witty personality AND is now flirty and dumb");
    }
    {   // "who dies next?" scoring
        Predictions p;
        check(p.right == 0 && p.total == 0 && p.guess == -1 && p.Label() == "0/0 deaths predicted correct", "predictions: a new game starts at 0/0 with no guess");
        p.guess = 3; p.Death(3);
        check(p.right == 1 && p.total == 1 && p.guess == -1, "predictions: guessing right scores a point, counts the death, and clears the guess");
        p.guess = 2; p.Death(5);
        check(p.right == 1 && p.total == 2 && p.guess == -1, "predictions: guessing wrong still counts the death but scores nothing");
        p.Death(1);
        check(p.right == 1 && p.total == 3, "predictions: a death with no guess counts as a miss");
        p.guess = 4; p.Death(4); p.guess = 0; p.Death(0);
        check(p.Label() == "3/5 deaths predicted correct", "predictions: the label reads like '3/5 deaths predicted correct'");
        Predictions q; q.guess = 6; q.Death(6); q.Death(6);
        check(q.right == 1 && q.total == 2, "predictions: one guess can only score once (it is cleared after a death)");
    }
    {   // catchphrases are a flavour, not in every line
        TicGate g;
        check(g.Ready(4), "tic gate: a speaker who has not used their tic yet may use it right away");
        g.Used();
        bool blocked3 = !g.Ready(4) && !g.Ready(4) && !g.Ready(4);
        check(blocked3 && g.Ready(4), "tic gate: after using it, the next 3 lines go without, then it is allowed again");
        g.Used(); g.Ready(4); g.Used();
        check(!g.Ready(4), "tic gate: using it resets the wait every time");
        check(UsesNicknameNow('K', 0.3f) && !UsesNicknameNow('K', 0.9f) && !UsesNicknameNow('N', 0.1f) && !UsesNicknameNow('H', 0.0f),
              "nicknames: friends use the pet name about 60% of the time, strangers and enemies never do");
        int tics = 0, lines = 4000; TicGate gg; PersonaRng rr{12345};
        for (int i = 0; i < lines; i++) if (gg.Ready(4)) { std::string out = ApplyTic("Nice weather we are having today.", ", period.", rr.unit(), 0.45f); if (out != "Nice weather we are having today.") { gg.Used(); tics++; } }
        check(tics > lines / 7 && tics < lines / 4, "tic gate: with the gate, a tic shows up in roughly one line in five (it used to be about one in three)");
    }
    check(ShortHash("hello") == ShortHash("hello") && ShortHash("hello") != ShortHash("hellp") && ShortHash("hello").size() == 8, "ShortHash is stable, sensitive to changes, and 8 characters long");

    printf(failures == 0 ? "\nAll tests passed!\n" : "\n%d test(s) FAILED.\n", failures);
    return failures == 0 ? 0 : 1;
}