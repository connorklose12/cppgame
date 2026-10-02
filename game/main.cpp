#include "raylib.h"
#include "rlgl.h"
#include "raymath.h"
#include "jsonutil.h"
#include "npc_brain.h"
#include "social.h"
#include "persona.h"
#include "dynamics.h"
#include "predictions.h"
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <ctime>
#include <string>
#include <vector>
#include <algorithm>
#include <cctype>
#include <sstream>
#ifdef PLATFORM_WEB
#include <emscripten.h>
#endif

enum Scene { MENU, OUTSIDE, ROOM1, ROOM2 };
enum Ctx { CTX_NORMAL, CTX_THANK, CTX_CONFRONT, CTX_GOSSIP, CTX_KILL, CTX_RUMOR, CTX_TOPIC, CTX_STORY, CTX_MEMORY, CTX_LOVE, CTX_ASK };   // why a conversation is happening
struct Person { Vector3 pos; float yaw, velY, walk; bool moving; };

// Every NPC is a tiny agent: a personality, an opinion of the player (nudged by what you say, wear and do),
// a memory of you, its own wandering routine, and relationships with the other NPCs (see social.h) - including
// fights. Its opinion decisions come from a small trained neural net (npc_brain.h). EVERYTHING an NPC says comes
// from Google Gemini (called from the browser, see shell.html), prompted with all of that: there are no scripted lines.
struct NPCInfo {
    Vector3 pos = {0, 0, 0}; Color color = WHITE; const char* name = ""; float yaw = 0;
    Scene loc = ROOM1, goalScene = ROOM1;                  // where it is now / where it is heading
    float opinion = 0;                                     // of the PLAYER: -100 (furious) .. 100 (adoring)
    Color favColor = {0, 0, 0, 0}, dislikeColor = {0, 0, 0, 0};
    float curiosity = 0.5f, friendliness = 0.5f, patience = 0.5f;   // personality vector
    int timesTalked = 0, timesHit = 0;                     // memory of the player
    Vector3 target = {0, 0, 0};                            // wandering
    float idleT = 0, walk = 0, hitT = 0, stuckT = 0;
    bool moving = false;
    int approach = 0, approachVictim = -1; float approachT = 0;   // 1 = wants to thank you, 2 = wants to confront you
    int fightTarget = -1; float fightT = 0, fightCooldown = 0;    // going after another NPC
    float swingT = 0, knifeT = 0;                          // arm swing timer / how long the knife stays out
    float windT = 0, frozenT = 0; int windVictim = -1;     // a killer's one-second wind-up / a victim frozen in place
    const char* colorName = "";
    std::string job, food, hobby, dream, fear, humans;     // who they are beyond the fighting - what they talk about
    std::string outside, voice, premise, pid;              // how they picture the outside world, how they talk, their ongoing storyline, and a fingerprint of all of it (for cache keys)
    std::string dialect, quirk, tic, nickname;             // how they talk, a habit that shows in their talk, a tag they tack onto sentences, their pet name for friends
    float romantic = 0.5f;                                 // how easily they develop a crush
    float trust = 0, romance = 0;                          // of the PLAYER: do they believe you mean well (-100..100), and how smitten are they (0..100). Saved.
    int beats = 0;                                         // how many chapters of their storyline they've told the player
    std::vector<std::string> storyLog;                     // the last few chapters they told (so the next one continues from them)
    std::vector<std::string> memory;                       // up to 3 things the player TOLD them ("the player's favorite animal is cat"): they bring them up later, and tell each other
    float chatT = 0;                                       // held in place while chatting with another NPC
    bool dying = false, gone = false; float deathT = 0;    // killed: stands still and fades out, then is gone for good
};
struct Flower { Vector3 pos; Color petal; float hp; float anim = 1; bool dying = false; float dyingT = 0; };
struct Stick { Vector3 pos; bool held, pickedUp; float swingT; };
struct NewsItem { std::string text; float age; Color color; };

const int W = 960, H = 540;
const int NPC_COUNT = 8;
static_assert(NPC_COUNT <= MAX_NPCS, "raise MAX_NPCS in social.h");
const int START_FLOWERS = 14, MAX_FLOWERS = 120;   // spare slots let happy NPCs "grow" new flowers
const Color CYAN_C = {0, 220, 230, 255};
const Color GREEN_C = {60, 160, 60, 255}, CENTER_C = {245, 210, 40, 255};   // stem/center, never randomized

// ---------------- player identity: color picker, name, HP ----------------
const char* colorNames[5] = {"Pink", "Purple", "Cyan", "Yellow", "Green"};
Color colorValues[5] = {PINK, {140, 70, 200, 255}, CYAN_C, {240, 205, 40, 255}, {80, 180, 80, 255}};
bool ColorsEqual(Color a, Color b) { return a.r == b.r && a.g == b.g && a.b == b.b; }

Scene scene = MENU, nextScene = MENU;
Person player = {};
NPCInfo npcs[NPC_COUNT];
Social social;
Flower flowers[MAX_FLOWERS] = {};
Stick stick = {{2.5f, 0, -4.0f}, false, false, 0};
std::vector<NewsItem> news;
size_t newsSeen = 0;
float fade = 0, letters = 0, timer = 0, footTimer = 0, gardenTimer = 1.2f, socialTimer = 1.0f, saveMsgT = 0;
int fadeDir = 0;   // 1 = fading to black, -1 = fading back in
float camAngle = 0, camZoom = 1.0f;

// ---------------- what characters talk about ----------------
enum Topic { T_JOB, T_FOOD, T_OUTSIDE, T_HUMAN, T_HOBBY, T_DREAM, T_FEAR, T_OTHER, T_TIME, TOPIC_COUNT };
int convTopic = 0, convOther = -1;         // this conversation's topic, and (for "other") which character they're talking about
int convRound = 0;                         // how many rounds of back-and-forth so far
bool convFollow = false;                   // after this reply, the character has more to say
bool convWantFollow = false;               // this conversation was written with a second round
std::vector<std::string> convTree;         // the whole conversation, written in ONE request (see the Tree enum below for what each line is)
enum Tree { TR_OPEN = 0, TR_REPLY = 1, TR_FOLLOW = 4, TR_FREPLY = 5, TR_CHOICE = 8, TR_TONES = 11, TR_FACT = 12 };
std::string convMemory;                    // for a memory conversation: the thing the player told someone
std::string convSubject;                   // for a personal question: what they ask about ("favorite food")
int killMentioned = -1;                    // the newest murder (event number) the player has been told about: the first conversation after a NEW one is always about it. Saved.
unsigned personaSeed = 1;                  // this game's cast of personalities comes from this seed (saved with the game)

// Overheard chatter: villagers standing together talk to each other (written by Gemini, one request per whole exchange) in speech bubbles.
std::string bubbleText[8];
float bubbleT[8] = {};
bool chatBusy = false, chatStarted = false;
int chatA = -1, chatB = -1, chatIdx = 0;   // chatA speaks the first line of the exchange
std::string chatNote, chatKw, chatKey, chatMood;   // what this chat is about (extra facts for the AI), words it should mention, its cache key, and the mood
float chatCooldown = 6, chatWait = 0, chatPause = 0;
std::vector<std::string> chatLines;        // the whole exchange arrives in one piece and is then played line by line

// One request to the AI (Gemini). `key` describes the SITUATION in coarse bands: the same situation later is answered from the cache for free.
struct AIReq { std::string kind, key, speaker, listener, emotion, facts, keywords, opts, opts2; bool needQ = false, fresh = false, follow = false; };   // opts = the player's answers ('|' separated); fresh = write new, never cache; follow = include a second round
AIReq aiReq;

// ---------------- the goal: keep your soulmate alive ----------------
int soulmate = -1;                         // which NPC you chose at the start
int gameOver = 0;                          // 0 playing, 1 your soulmate was killed, 2 you and your soulmate are the last two alive
Predictions pred;                          // "who dies next?": your guess, and how many deaths you called right
bool guessing = false, needGuess = false;  // the guess screen is open / should open as soon as nothing else is going on
int guessSel = 0;
bool pickingSoulmate = false;              // the "Who's your soulmate?" screen
int soulSel = 0;
float killCineT = -1;                      // seconds into a killing's dark cinematic (-1 = not running)
float playerHitT = 0;                      // red "you got hit" tint on your own model, in seconds

// ---------------- the open conversation ----------------
bool talking = false;
int talkIdx = 0;
int convCtx = CTX_NORMAL, convVictim = -1, convEvent = -1;
int convSpecial = 0;                       // 1 = this NPC is asking your color, 2 = asking your name
bool convHasChoices = false, choosing = false;
int choiceSel = 0;
std::string choiceText[3]; float choiceVal[3] = {};   // the three answers on offer right now (written by the AI), and how warm each is
bool aiWaiting = false;                    // waiting for the AI to write the NPC's line
bool pickingTarget = false;                // the "who was it?" list of characters (gossip questions)
int convRumorTopic = 0, convRumorKind = 0, targetSel = 0;   // kind: 0 nasty rumor, 1 kind deed, 2 secret admirer
std::vector<int> targets;                  // who you can name
int aiTries = 0;
float aiTimeoutT = 0;
std::string pendingL2, choiceHeader;       // choiceHeader = what the NPC just said (shown above your answers)
char curL1[700] = "", curL2[700] = "";     // the two pieces of text the dialogue box is showing

float playerHP = 100;
char playerName[16] = "???";
bool hasName = false;
bool colorPicking = false, namePrompt = false;
int colorSel = 0;
char nameBuf[16] = "";
float hurtTimer = 0;
float aggression = 0;   // 0..1, rises when you swing the stick, slowly decays
float dayT = 1.0f;      // 1 = full day, 0 = full night
char clockStr[32] = "";
float clockTimer = 0;

// ---------------- settings ----------------
float sfxVolume = 1.0f;
int quality = 2;             // 0 low, 1 medium, 2 high - controls mesh smoothness only
bool dayNightOn = true;
bool flowerDamageOn = true;
bool showNews = true;
int keyInteract = KEY_SPACE, keySwing = KEY_F;
bool inSettings = false, rebinding = false;
int rebindWhich = 0, settingsSel = 0;
bool hasSaveFile = false;
int menuSel = 0;                           // title menu: 0 New Game, 1 Load Saved Game, 2 Settings
std::string menuMsg; float menuMsgT = 0; Color menuMsgColor = {255, 140, 120, 255};   // a short message on the title screen (e.g. a save that couldn't be loaded)
std::string styleNote;                     // tone/world notes from dialogue.json, sent to Gemini as part of its instructions

int MeshSides() { return quality == 0 ? 6 : quality == 1 ? 10 : 16; }

// ---------------- config.json / dialogue.json (tiny hand-written readers - see jsonutil.h) ----------------
float walkSpeed = 5, jumpVel = 9;
Color playerColor = {140, 70, 200, 255};
Color flowerRed = RED, flowerPink = PINK;

// Your answers are written by the AI along with the question (each starts with + warm, 0 neutral or - rude). Only the follow-up round uses a fixed set.
const char* SUBJECTS[] = {"favorite food", "favorite animal", "favorite color", "favorite season", "dream vacation spot", "favorite kind of music", "favorite game", "biggest fear", "favorite hobby", "dream pet"};   // what villagers ask you about: your answer is remembered, and they gossip about it
const char* FOLLOW_TEXT[3] = {"Tell me more!", "Interesting.", "That sounds boring."};
const char* FOLLOW_OPTS = "Tell me more!|Interesting.|That sounds boring.";   // the same three, as the AI is told them
float ToneValue(char tone) { return tone == '+' ? 0.7f : tone == '-' ? -0.6f : 0.2f; }   // how much an answer warms (or chills) the villager, for the neural net

// Gossip questions: an NPC asks "who was it?" and YOU choose who gets the blame (or the credit). %N = the asker's name.
struct RumorTopic { const char* text; int kind; };   // kind 0 = a nasty rumor, 1 = a kind deed, 2 = a secret admirer
const RumorTopic RUMORS[] = {
    {"heard a rumor that someone said they do not like %N's voice", 0},
    {"heard that someone was laughing at %N's outfit behind %N's back", 0},
    {"was told that someone called %N lazy", 0},
    {"heard that someone ate %N's lunch and lied about it", 0},
    {"found out that someone watered %N's favorite flower today", 1},
    {"found a small gift left outside %N's door", 1},
    {"heard that someone said really nice things about %N", 1},
    {"heard that someone has a secret crush on %N", 2},
    {"got a mysterious love note and wonders who sent it", 2},
};
const int RUMOR_COUNT = 9;

Color JColorC(const std::string& j, const char* key, Color def) {   // RGB -> raylib Color convenience wrapper
    RGB r = JColor(j, key, {def.r, def.g, def.b, def.a});
    return Color{r.r, r.g, r.b, r.a};
}
// The cast's personalities are drawn from a seed (see persona.h): a different cast every new game, the same cast when you load a save.
void ApplyPersonas(NPCInfo* arr, unsigned seed) {
    std::vector<Persona> ps = GeneratePersonas(seed, NPC_COUNT);
    for (int i = 0; i < NPC_COUNT; i++) {
        const Persona& p = ps[i];
        NPCInfo& n = arr[i];
        n.job = p.job; n.food = p.food; n.hobby = p.hobby; n.dream = p.dream; n.fear = p.fear; n.humans = p.humans;
        n.outside = p.outside; n.voice = p.voice; n.premise = p.premise;
        n.dialect = p.dialect; n.quirk = p.quirk; n.tic = p.tic; n.nickname = p.nickname; n.romantic = p.romantic;
        n.curiosity = p.curiosity; n.friendliness = p.friendliness; n.patience = p.patience;
        n.favColor = p.favColor >= 0 ? colorValues[p.favColor] : Color{0, 0, 0, 0};
        n.dislikeColor = p.dislikeColor >= 0 ? colorValues[p.dislikeColor] : Color{0, 0, 0, 0};
        n.pid = ShortHash(n.job + "|" + n.food + "|" + n.hobby + "|" + n.dream + "|" + n.fear + "|" + n.humans + "|" + n.outside + "|" + n.voice + "|" + n.premise + "|" + n.dialect + "|" + n.quirk);
    }
}
void InitNPCs() {   // who lives in this world and where they start. Everyone is simply named after their color.
    struct Spec { const char* name; Color color; Scene loc; float x, z; };
    static const Spec specs[NPC_COUNT] = {
        {"Yellow",  YELLOW,               ROOM1,    4, -3},   // asks your color
        {"Orange",  ORANGE,               ROOM2,    0,  4},   // asks your name
        {"Green",   {80, 180, 80, 255},   ROOM2,   -4,  4},
        {"Red",     {220, 60, 60, 255},   OUTSIDE,  6, -2},
        {"Blue",    {60, 110, 230, 255},  OUTSIDE, -7,  3},
        {"Brown",   {150, 100, 60, 255},  ROOM1,   -5,  2},
        {"Silver",  {185, 190, 200, 255}, OUTSIDE, 10,  8},
        {"Magenta", {200, 60, 190, 255},  ROOM2,    5, -5},
    };
    for (int i = 0; i < NPC_COUNT; i++) {
        const Spec& s = specs[i];
        NPCInfo& n = npcs[i];
        n.name = s.name; n.colorName = s.name; n.color = s.color;
        n.loc = n.goalScene = s.loc;
        n.pos = n.target = {s.x, 0, s.z};
        n.idleT = (float)GetRandomValue(0, 30) / 10.0f;
    }
    personaSeed = (unsigned)GetRandomValue(1, 1000000000);
    ApplyPersonas(npcs, personaSeed);
}
void LoadConfig() {
    char* text = LoadFileText("config.json");
    if (!text) return;   // missing file: keep the defaults above
    std::string j = text;
    UnloadFileText(text);
    walkSpeed = JNum(j, "walkSpeed", walkSpeed);
    jumpVel = JNum(j, "jumpHeight", jumpVel);
    playerColor = JColorC(j, "playerColor", playerColor);
    npcs[0].color = JColorC(j, "npc1Color", npcs[0].color);
    npcs[1].color = JColorC(j, "npc2Color", npcs[1].color);
    flowerRed = JColorC(j, "flowerRed", flowerRed);
    flowerPink = JColorC(j, "flowerPink", flowerPink);
}
void LoadDialogue() {   // dialogue.json is only a short style guide for the AI (tone and world notes) - NPCs have no scripted lines
    styleNote = "The villagers are curious about the world outside and about the human world, they gossip about each other, and they sometimes make things up.";
    char* text = LoadFileText("dialogue.json");
    if (!text) return;
    std::string j = text;
    UnloadFileText(text);
    std::vector<std::string> intro = JStrArray(j, "intro");
    if (!intro.empty()) styleNote = intro[0];
}

std::string PN() {   // what NPCs call you: the name you gave them, otherwise your own color
    if (hasName) return std::string(playerName);
    for (int i = 0; i < 5; i++) if (ColorsEqual(playerColor, colorValues[i])) return colorNames[i];
    return "friend";
}
char PlayerDyn(int idx) {   // how this villager currently sees the player: hostile / skeptical / unsure / crush / best friend... (see dynamics.h)
    const NPCInfo& n = npcs[idx];
    PlayerFeel f; f.opinion = n.opinion; f.trust = n.trust; f.romance = n.romance; f.timesTalked = n.timesTalked; f.timesHit = n.timesHit;
    return PlayerDynCode(f);
}
std::string AddressFor(int speaker) {   // friends and admirers call you by their own pet name for you; everyone else uses your name
    if (speaker >= 0 && speaker < NPC_COUNT && UsesNicknameNow(PlayerDyn(speaker), (float)GetRandomValue(0, 999) / 1000.0f)) return npcs[speaker].nickname;   // (most of the time, not in every sentence)
    std::string p = PN();
    return (!hasName && speaker >= 0 && speaker < NPC_COUNT && p == npcs[speaker].name) ? std::string("friend") : p;   // never the very same name as the one talking
}

// ---------------- sound ----------------
// In the browser every sound is made by shell.html (GameAudio, the Web Audio API): no files to find, nothing to load, and it switches
// on with the first key press or click. A desktop build plays assets/sounds/*.wav through raylib instead.
enum Sfx { SFX_FOOTSTEP, SFX_JUMP, SFX_DOOR, SFX_BLIP, SFX_TALK };
#ifdef PLATFORM_WEB
void LoadAudio() {   // read each of your sound files and hand its bytes to the browser, which decodes and plays them (a missing file falls back to a built-in sound)
    static const char* const files[] = {"footstep", "jump", "door", "blip", "talk", "music"};
    for (const char* name : files) {
        std::string path = std::string("assets/sounds/") + name + ".wav";
        int size = 0;
        unsigned char* data = LoadFileData(path.c_str(), &size);
        if (!data) continue;
        if (size > 44) EM_ASM({
            var heap = (typeof HEAPU8 !== 'undefined') ? HEAPU8 : Module.HEAPU8;
            if (window.GameAudio && heap) window.GameAudio.loadWav(UTF8ToString($0), heap.slice($1, $1 + $2));
        }, name, data, size);
        UnloadFileData(data);
    }
}
const char* SfxName(Sfx s) { switch (s) { case SFX_FOOTSTEP: return "footstep"; case SFX_JUMP: return "jump"; case SFX_DOOR: return "door"; case SFX_BLIP: return "blip"; default: return "talk"; } }
void PlaySfx(Sfx s) { EM_ASM({ if (window.GameAudio) window.GameAudio.play(UTF8ToString($0)); }, SfxName(s)); }
void SetVolume(float v) { EM_ASM({ if (window.GameAudio) window.GameAudio.setVolume($0); }, v); }
void UpdateAudio() {}
int SoundStatus() { return EM_ASM_INT({ return window.GameAudio ? window.GameAudio.status() : 0; }); }   // 0 not started, 1 waiting for a key press/click, 2 on
#else
Sound sFx[5];
Music music;
bool hasSfx[5], hasMusic;
int soundsFound = 0;   // how many of the 6 sound files were found (shown on the title screen so a missing folder is obvious)
void LoadAudio() {
    InitAudioDevice();
    static const char* const files[] = {"footstep", "jump", "door", "blip", "talk"};
    for (int i = 0; i < 5; i++) {
        std::string path = std::string("assets/sounds/") + files[i] + ".wav";
        hasSfx[i] = FileExists(path.c_str());
        if (hasSfx[i]) sFx[i] = LoadSound(path.c_str());
    }
    hasMusic = FileExists("assets/sounds/music.wav");
    soundsFound = (int)hasMusic;
    for (bool h : hasSfx) soundsFound += (int)h;
    if (hasMusic) { music = LoadMusicStream("assets/sounds/music.wav"); PlayMusicStream(music); SetMusicVolume(music, 0.5f); }
}
void PlaySfx(Sfx s) { if (s == SFX_TALK && !hasSfx[SFX_TALK]) s = SFX_BLIP; if (hasSfx[s]) PlaySound(sFx[s]); }   // (no talk chirp file: the blip stands in)
void SetVolume(float v) { SetMasterVolume(v); }
void UpdateAudio() { if (hasMusic) UpdateMusicStream(music); }
int SoundStatus() { return 2; }
#endif

// ---------------- save / load ----------------
// The whole world is saved as plain text: you, every NPC (including who has died), all their relationships and
// secrets, and the flowers. In the browser it lives in localStorage; the native build writes save.dat instead.
const int SAVE_VERSION = 4;

void ResetPlans();   // (defined with the conversation code below)

// Saved lists of text are written as: count, then for each one its length, a space, and the text (so the text may contain anything).
void WriteStrings(std::ostringstream& o, const std::vector<std::string>& v) { o << v.size() << "\n"; for (const std::string& t : v) o << t.size() << ' ' << t << "\n"; }
bool ReadStrings(std::istringstream& in, std::vector<std::string>& v) {
    size_t count;
    if (!(in >> count) || count > 8) return false;
    v.clear();
    for (size_t k = 0; k < count; k++) {
        size_t len;
        if (!(in >> len) || len > 600) return false;
        in.get();                                        // the space
        std::string t(len, ' ');
        if (len) in.read(&t[0], (std::streamsize)len);
        if (!in) return false;
        v.push_back(t);
    }
    return true;
}

std::string Serialize() {
    std::ostringstream o;
    o << "PURPLE_SAVE " << SAVE_VERSION << "\n";
    o << (int)scene << ' ' << player.pos.x << ' ' << player.pos.y << ' ' << player.pos.z << ' ' << player.yaw << ' '
      << (stick.pickedUp ? 1 : 0) << ' ' << playerHP << ' ' << (int)playerColor.r << ' ' << (int)playerColor.g << ' ' << (int)playerColor.b << ' '
      << (hasName ? 1 : 0) << ' ' << timer << ' ' << aggression << ' ' << soulmate << ' ' << gameOver << "\n";
    o << strlen(playerName) << ' ' << playerName << "\n";
    for (int i = 0; i < NPC_COUNT; i++) {
        const NPCInfo& n = npcs[i];
        o << (int)n.loc << ' ' << n.pos.x << ' ' << n.pos.z << ' ' << n.yaw << ' ' << n.opinion << ' ' << n.timesTalked << ' ' << n.timesHit << ' '
          << ((n.gone || n.dying || !social.alive[i]) ? 1 : 0) << "\n";
    }
    o << social.n << ' ' << social.rng << "\n";
    for (int i = 0; i < social.n; i++) { for (int j = 0; j < social.n; j++) o << social.affinity[i][j] << ' '; o << "\n"; }
    for (int i = 0; i < social.n; i++) { for (int j = 0; j < social.n; j++) o << social.chemistry[i][j] << ' '; o << "\n"; }
    for (int i = 0; i < social.n; i++) o << social.partner[i] << ' ' << social.crush[i] << ' ' << (social.alive[i] ? 1 : 0) << "\n";
    o << social.events.size() << "\n";
    for (const SocialEvent& e : social.events)
        o << (int)e.type << ' ' << e.a << ' ' << e.b << ' ' << e.c << ' ' << e.time << ' ' << e.knowers << ' ' << (e.discovered ? 1 : 0) << "\n";
    int active = 0;
    for (const Flower& f : flowers) if (f.hp > 0 && !f.dying) active++;
    o << active << "\n";
    for (const Flower& f : flowers)
        if (f.hp > 0 && !f.dying) o << f.pos.x << ' ' << f.pos.z << ' ' << (int)f.petal.r << ' ' << (int)f.petal.g << ' ' << (int)f.petal.b << ' ' << f.hp << "\n";
    for (int i = 0; i < social.n; i++) { for (int j = 0; j < social.n; j++) o << social.clashes[i][j] << ' '; o << "\n"; }   // the history of who has fought whom
    o << personaSeed << "\n";   // the cast's personalities are regenerated from this seed on load...
    for (int i = 0; i < NPC_COUNT; i++) { o << npcs[i].beats << ' '; WriteStrings(o, npcs[i].storyLog); }   // ...and each character's storyline so far
    for (int i = 0; i < NPC_COUNT; i++) o << npcs[i].trust << ' ' << npcs[i].romance << "\n";            // how each villager sees you
    o << killMentioned << "\n";                                                                           // the newest murder you've been told about
    for (int i = 0; i < NPC_COUNT; i++) WriteStrings(o, npcs[i].memory);                                   // what each villager remembers you telling them
    o << pred.right << ' ' << pred.total << ' ' << pred.guess << ' ' << pred.scanned << "\n";               // your 'who dies next?' score and current guess
    return o.str();
}

// Reads everything into temporaries first, so a damaged save can't leave you with half a world.
bool Deserialize(const std::string& text) {
    std::istringstream in(text);
    std::string tag; int version = 0;
    if (!(in >> tag >> version) || tag != "PURPLE_SAVE" || version != SAVE_VERSION) return false;
    int sc, picked, cr, cg, cb, named, soul, over; float px, py, pz, pyaw, hp, tm, agg;
    if (!(in >> sc >> px >> py >> pz >> pyaw >> picked >> hp >> cr >> cg >> cb >> named >> tm >> agg >> soul >> over)) return false;
    if (sc < OUTSIDE || sc > ROOM2 || soul < -1 || soul >= NPC_COUNT || over < 0 || over > 2) return false;
    size_t len;
    if (!(in >> len) || len > 15) return false;
    in.get();
    std::string name(len, ' ');
    if (len) in.read(&name[0], (std::streamsize)len);

    NPCInfo tmp[NPC_COUNT];
    for (int i = 0; i < NPC_COUNT; i++) tmp[i] = npcs[i];   // keeps each NPC's name, color and personality
    for (int i = 0; i < NPC_COUNT; i++) {
        int loc, gone; float x, z;
        if (!(in >> loc >> x >> z >> tmp[i].yaw >> tmp[i].opinion >> tmp[i].timesTalked >> tmp[i].timesHit >> gone)) return false;
        if (loc < OUTSIDE || loc > ROOM2) return false;
        tmp[i].loc = tmp[i].goalScene = (Scene)loc;
        tmp[i].pos = tmp[i].target = {x, 0, z};
        tmp[i].gone = gone != 0; tmp[i].dying = false;
        tmp[i].approach = 0; tmp[i].fightTarget = -1; tmp[i].fightCooldown = tmp[i].swingT = tmp[i].knifeT = tmp[i].hitT = 0;
        tmp[i].idleT = 1; tmp[i].moving = false; tmp[i].chatT = 0; tmp[i].windT = tmp[i].frozenT = 0; tmp[i].windVictim = -1;
    }
    Social s = social;
    int sn; unsigned rng;
    if (!(in >> sn >> rng) || sn != NPC_COUNT) return false;
    s.n = sn; s.rng = rng ? rng : 1u;
    for (int i = 0; i < sn; i++) for (int j = 0; j < sn; j++) if (!(in >> s.affinity[i][j])) return false;
    for (int i = 0; i < sn; i++) for (int j = 0; j < sn; j++) if (!(in >> s.chemistry[i][j])) return false;
    for (int i = 0; i < sn; i++) {
        int al;
        if (!(in >> s.partner[i] >> s.crush[i] >> al)) return false;
        if (s.partner[i] < -1 || s.partner[i] >= sn || s.crush[i] < -1 || s.crush[i] >= sn) return false;
        s.alive[i] = al != 0;
    }
    size_t ne;
    if (!(in >> ne) || ne > 5000) return false;
    s.events.clear();
    for (size_t k = 0; k < ne; k++) {
        int type, a, b, c, disc; float time; unsigned knowers;
        if (!(in >> type >> a >> b >> c >> time >> knowers >> disc)) return false;
        if (type < 0 || type > EV_KILL || a < -1 || a >= sn || b < -1 || b >= sn || c < -1 || c >= sn) return false;
        s.events.push_back({(EventType)type, a, b, c, time, knowers, disc != 0});
    }
    size_t nf;
    if (!(in >> nf) || nf > (size_t)MAX_FLOWERS) return false;
    std::vector<Flower> fl;
    for (size_t k = 0; k < nf; k++) {
        float x, z, fhp; int r, g, b;
        if (!(in >> x >> z >> r >> g >> b >> fhp)) return false;
        fl.push_back({{x, 0, z}, Color{(unsigned char)r, (unsigned char)g, (unsigned char)b, 255}, fhp, 1, false, 0});
    }

    std::vector<int> clash(sn * sn, 0);   // fight history (saves from before it existed just start with none)
    for (int k = 0; k < sn * sn; k++) if (!(in >> clash[k])) { std::fill(clash.begin(), clash.end(), 0); break; }
    for (int i = 0; i < sn; i++) for (int j = 0; j < sn; j++) s.clashes[i][j] = clash[i * sn + j];

    // Optional tails (older saves lack them and keep whatever they have): the cast's seed + storylines, then how each villager
    // sees you, then the newest murder you've been told about + what each villager remembers.
    unsigned seedIn = 0; bool havePersona = false;
    std::vector<int> beatsIn(NPC_COUNT, 0); std::vector<std::vector<std::string>> logsIn(NPC_COUNT), memIn(NPC_COUNT);
    std::vector<float> trustIn(NPC_COUNT, 0.0f), romIn(NPC_COUNT, 0.0f);
    int killIn = (int)s.events.size() - 1;                      // an older save: every murder so far counts as already mentioned
    Predictions predIn; predIn.scanned = (int)s.events.size();  // ...and its past deaths are not scored after the fact
    if (in >> seedIn) {
        havePersona = true;
        for (int i = 0; i < NPC_COUNT && havePersona; i++) havePersona = (bool)(in >> beatsIn[i]) && ReadStrings(in, logsIn[i]);
    }
    if (havePersona) {
        bool ok = true;
        for (int i = 0; i < NPC_COUNT && ok; i++) ok = (bool)(in >> trustIn[i] >> romIn[i]);
        if (!ok) { std::fill(trustIn.begin(), trustIn.end(), 0.0f); std::fill(romIn.begin(), romIn.end(), 0.0f); }
        int k;
        if (ok && (in >> k)) {
            for (int i = 0; i < NPC_COUNT && ok; i++) ok = ReadStrings(in, memIn[i]);
            if (ok) {
                killIn = k;
                int pr, pt, pg, ps;
                if (in >> pr >> pt >> pg >> ps) { predIn.right = pr; predIn.total = pt; predIn.guess = (pg >= 0 && pg < NPC_COUNT && s.alive[pg]) ? pg : -1; predIn.scanned = std::max(0, std::min(ps, (int)s.events.size())); }
            } else for (auto& m : memIn) m.clear();
        }
    }

    // all good - commit
    scene = (Scene)sc;
    player.pos = {px, py, pz}; player.yaw = pyaw; player.velY = 0; player.moving = false;
    stick.pickedUp = stick.held = picked != 0;
    playerHP = hp; playerColor = Color{(unsigned char)cr, (unsigned char)cg, (unsigned char)cb, 255};
    hasName = named != 0;
    strncpy(playerName, name.empty() ? "???" : name.c_str(), sizeof(playerName) - 1);
    timer = tm; aggression = agg; soulmate = soul; gameOver = over;
    killCineT = -1; playerHitT = 0; pickingSoulmate = false;
    chatBusy = chatStarted = false; chatA = chatB = -1; chatCooldown = 6; for (int i = 0; i < 8; i++) bubbleT[i] = 0;
    if (havePersona && seedIn) {   // bring back this save's cast and each character's story
        personaSeed = seedIn;
        ApplyPersonas(tmp, seedIn);
        for (int i = 0; i < NPC_COUNT; i++) { tmp[i].beats = beatsIn[i]; tmp[i].storyLog = logsIn[i]; }
    }
    killMentioned = killIn;
    pred = predIn; guessing = needGuess = false;
    ResetPlans();
    for (int i = 0; i < NPC_COUNT; i++) { tmp[i].trust = trustIn[i]; tmp[i].romance = romIn[i]; tmp[i].memory = memIn[i]; npcs[i] = tmp[i]; }
    social = s;
    newsSeen = social.events.size();   // don't re-announce old news
    news.clear();
    for (Flower& f : flowers) f = Flower{{0, 0, 0}, WHITE, 0, 1, false, 0};
    for (size_t k = 0; k < fl.size(); k++) flowers[k] = fl[k];
    talking = colorPicking = namePrompt = choosing = aiWaiting = false;
    return true;
}

#ifdef PLATFORM_WEB
bool SaveExists() { return EM_ASM_INT({ return localStorage.getItem('purpleSave3') ? 1 : 0; }) != 0; }
bool WriteSave(const std::string& s) { EM_ASM({ localStorage.setItem('purpleSave3', UTF8ToString($0)); }, s.c_str()); return true; }
std::string ReadSave() {
    int len = EM_ASM_INT({ var s = localStorage.getItem('purpleSave3'); return s ? s.length : 0; });
    if (len <= 0) return "";
    std::string out(len * 3 + 1, '\0');   // up to 3 bytes per character once story lines contain curly quotes and the like
    EM_ASM({ stringToUTF8(localStorage.getItem('purpleSave3'), $0, $1); }, &out[0], len * 3 + 1);
    out.resize(strlen(out.c_str()));
    return out;
}
#else
bool SaveExists() { return FileExists("save.dat"); }
bool WriteSave(const std::string& s) { return SaveFileText("save.dat", (char*)s.c_str()); }
std::string ReadSave() {
    char* t = LoadFileText("save.dat");
    if (!t) return "";
    std::string s = t;
    UnloadFileText(t);
    return s;
}
#endif

bool SaveGame() {   // bound to the P key; the HUD tells you so and shows "File saved!"
    if (!WriteSave(Serialize())) return false;
    saveMsgT = 2.5f; hasSaveFile = true;
    return true;
}
bool LoadGame() {   // "Press C" on the menu: drops you back where you saved, fading in
    std::string s = ReadSave();
    if (s.empty() || !Deserialize(s)) return false;
    fade = 1; fadeDir = -1;
    return true;
}

// ---------------- AI dialogue (see shell.html's AIDialogue object) ----------------
// Google Gemini, called straight from the player's browser with the player's OWN API key (never stored in the game).
// The browser side caches lines per situation and writes several variants per request, so repeated situations are
// free. In a native build there is no browser, so these are stubs and NPCs just can't talk.
#ifdef PLATFORM_WEB
void AIDialogue_SetStyle(const std::string& s) { EM_ASM({ if (window.AIDialogue) window.AIDialogue.style = UTF8ToString($0); }, s.c_str()); }
void AIDialogue_Request(const AIReq& r, bool prefetch = false) {   // prefetch = write it into the cache ahead of time (no state/result change)
    EM_ASM({
        if (window.AIDialogue) window.AIDialogue.request(UTF8ToString($0), UTF8ToString($1), UTF8ToString($2), UTF8ToString($3), UTF8ToString($4), UTF8ToString($5),
                                                         $6 !== 0, UTF8ToString($7), $8 !== 0, UTF8ToString($9), UTF8ToString($10), $11 !== 0, $12 !== 0);
    }, r.kind.c_str(), r.key.c_str(), r.speaker.c_str(), r.listener.c_str(), r.emotion.c_str(), r.facts.c_str(), r.needQ ? 1 : 0, r.keywords.c_str(),
       r.fresh ? 1 : 0, r.opts.c_str(), r.opts2.c_str(), r.follow ? 1 : 0, prefetch ? 1 : 0);
}
int AIDialogue_Status() { return EM_ASM_INT({ return (window.AIDialogue && window.AIDialogue.ready) ? 2 : 3; }); }   // 2 = has a usable key, 3 = needs a key
int AIDialogue_State() {   // 0 busy, 1 ready, 2 failed
    return EM_ASM_INT({
        if (!window.AIDialogue) return 2;
        var s = window.AIDialogue.state;
        if (s === 'ready') return 1;
        if (s === 'error') return 2;
        return 0;
    });
}
std::string AIDialogue_Result() {
    char buf[8192] = "";   // a whole conversation (13 lines of up to a few hundred characters each) fits comfortably
    EM_ASM({ stringToUTF8((window.AIDialogue && window.AIDialogue.result) || String(), $0, $1); }, buf, sizeof(buf));
    return std::string(buf);
}
int AIDialogue_Calls() { return EM_ASM_INT({ return window.AIDialogue ? window.AIDialogue.calls() : 0; }); }        // paid requests so far
int AIDialogue_Hits() { return EM_ASM_INT({ return window.AIDialogue ? window.AIDialogue.hits() : 0; }); }          // conversations/lines reused from the cache (free)
int AIDialogue_MicroUSD() { return EM_ASM_INT({ return window.AIDialogue ? window.AIDialogue.microUSD() : 0; }); }   // estimated spend, in millionths of a dollar
std::string AIDialogue_LastError() {   // why the last request failed ("" when the last one worked)
    char buf[200] = "";
    EM_ASM({ stringToUTF8((window.AIDialogue && window.AIDialogue.lastError) || String(), $0, $1); }, buf, sizeof(buf));
    return std::string(buf);
}
void AIDialogue_Reject() { EM_ASM({ if (window.AIDialogue) window.AIDialogue.reject(); }); }
int AIDialogue_Throttled() { return EM_ASM_INT({ return (window.AIDialogue && window.AIDialogue.throttled()) ? 1 : 0; }); }   // rate-limited or busy: background chatter holds off
int AIDialogue_LastMs() { return EM_ASM_INT({ return window.AIDialogue ? window.AIDialogue.lastMs() : 0; }); }               // how long the last paid answer took   // the conversation just received was unusable: forget that stored version
#else
void AIDialogue_SetStyle(const std::string&) {}
void AIDialogue_Request(const AIReq&, bool = false) {}
int AIDialogue_Status() { return 3; }
int AIDialogue_State() { return 2; }
std::string AIDialogue_Result() { return ""; }
int AIDialogue_Calls() { return 0; }
int AIDialogue_Hits() { return 0; }
int AIDialogue_MicroUSD() { return 0; }
std::string AIDialogue_LastError() { return ""; }
void AIDialogue_Reject() {}
int AIDialogue_Throttled() { return 0; }
int AIDialogue_LastMs() { return 0; }
#endif

// ---------------- drawing a person: cylinder limbs, cube body, sphere head ----------------
void Limb(float x, float y, float angle, float length, float radius, Color c) {
    rlPushMatrix();
    rlTranslatef(x, y, 0);
    rlRotatef(angle, 1, 0, 0);
    DrawCylinderEx({0, 0, 0}, {0, -length, 0}, radius, radius, MeshSides(), c);
    rlPopMatrix();
}

// held: 0 = nothing, 1 = the stick, 2 = a knife. A positive swingT plays the swing animation (same for both).
void DrawPerson(Person p, Color c, int held, float swingT) {
    float swing = p.moving ? sinf(p.walk * 10) * 35 : 0;
    float armL = swing, armR = -swing, legL = -swing, legR = swing;
    if (p.pos.y > 0) { armL = armR = -150; legL = -35; legR = 20; }        // jump pose
    if (swingT > 0) armR = -120 + swingT * 400;                            // swing pose overrides the right arm
    Color limb = ColorBrightness(c, -0.25f);

    float r = fmaxf(0.2f, 0.45f - p.pos.y * 0.05f);                        // shadow (fades along with the person)
    DrawCylinder({p.pos.x, 0.02f, p.pos.z}, r, r, 0.01f, 16, Fade(BLACK, 0.25f * c.a / 255.0f));

    rlPushMatrix();
    rlTranslatef(p.pos.x, p.pos.y, p.pos.z);
    rlRotatef(p.yaw, 0, 1, 0);
    Limb(-0.2f, 0.8f, legL, 0.8f, 0.13f, limb);
    Limb(0.2f, 0.8f, legR, 0.8f, 0.13f, limb);
    DrawCube({0, 1.25f, 0}, 0.8f, 0.9f, 0.5f, c);
    DrawCubeWires({0, 1.25f, 0}, 0.8f, 0.9f, 0.5f, ColorBrightness(c, -0.5f));
    Limb(-0.52f, 1.6f, armL, 0.75f, 0.11f, limb);
    Limb(0.52f, 1.6f, armR, 0.75f, 0.11f, limb);
    DrawSphereEx({0, 2.05f, 0}, 0.35f, MeshSides(), MeshSides(), ColorBrightness(c, 0.15f));
    if (held) {   // whatever is in the right hand swings with the arm
        rlPushMatrix();
        rlTranslatef(0.55f, 1.5f, 0);
        rlRotatef(armR, 1, 0, 0);
        if (held == 1) DrawCylinderEx({0, 0, 0}, {0, -1.0f, 0.3f}, 0.05f, 0.05f, 6, BROWN);   // the stick
        else {                                                                                  // the knife: a short handle and a pointed blade
            DrawCylinderEx({0, 0, 0}, {0, -0.25f, 0.075f}, 0.05f, 0.05f, 6, {70, 45, 25, c.a});
            DrawCylinderEx({0, -0.25f, 0.075f}, {0, -0.85f, 0.255f}, 0.06f, 0.0f, 4, {215, 220, 230, c.a});
        }
        rlPopMatrix();
    }
    rlPopMatrix();
}

void DrawNPC(const NPCInfo& n) {   // same body as the player; flashes red when hit, holds a knife when killing, fades out when killed
    if (n.gone) return;
    Person p = {}; p.pos = n.pos; p.yaw = n.yaw; p.moving = n.moving; p.walk = n.walk;
    Color c = n.hitT > 0 ? ColorLerp(n.color, RED, 0.75f * Clamp(n.hitT, 0.0f, 1.0f)) : n.color;   // red hue for ~1 second after being hit
    if (n.dying) c.a = (unsigned char)(255 * Clamp(n.deathT / 3.0f, 0, 1));
    DrawPerson(p, c, n.knifeT > 0 ? 2 : 0, n.swingT);
}

void DrawFlower(Flower f) {
    if (f.hp <= 0 && !f.dying) return;                                     // hp<=0 and not dying = an unused slot
    float scale = f.dying ? (1 - f.dyingT) : f.anim;                       // grows in, or shrinks away when killed
    if (scale <= 0.02f) return;
    Color petal = f.dying ? ColorLerp(f.petal, PURPLE, f.dyingT) : f.petal;
    float stemH = 0.5f * scale;
    DrawCylinder({f.pos.x, 0, f.pos.z}, 0.03f * scale, 0.03f * scale, stemH, 6, GREEN_C);
    Vector3 top = {f.pos.x, stemH, f.pos.z};
    float off = 0.14f * scale, r = 0.1f * scale;
    DrawSphereEx(Vector3Add(top, {off, 0, 0}), r, 6, 6, petal);             // 4 petals
    DrawSphereEx(Vector3Add(top, {-off, 0, 0}), r, 6, 6, petal);
    DrawSphereEx(Vector3Add(top, {0, 0, off}), r, 6, 6, petal);
    DrawSphereEx(Vector3Add(top, {0, 0, -off}), r, 6, 6, petal);
    DrawSphereEx(top, 0.09f * scale, 6, 6, CENTER_C);                       // center (always yellow)
    if (!f.dying && f.hp < 30) {                                           // health bar once damaged
        Vector3 barPos = {f.pos.x, stemH + 0.45f, f.pos.z};
        DrawCube(barPos, 0.6f, 0.08f, 0.02f, Fade(BLACK, 0.5f));
        float pct = f.hp / 30.0f;
        DrawCube({barPos.x - 0.3f + 0.3f * pct, barPos.y, barPos.z}, 0.6f * pct, 0.08f, 0.03f, GREEN);
    }
}

// ---------------- UI ----------------
void Prompt(const char* text) {
    int w = MeasureText(text, 26), x = (W - w) / 2, y = H - 110;
    DrawRectangleRounded({x - 20.0f, y - 12.0f, w + 40.0f, 50}, 0.4f, 8, Fade(BLACK, 0.6f));
    DrawText(text, x, y, 26, WHITE);
}

std::vector<std::string> WrapText(const std::string& text, int maxW, int fs) {   // word-wrap into lines no wider than maxW pixels
    std::vector<std::string> lines;
    std::string line, word, s = text + " ";
    for (char ch : s) {
        if (ch != ' ') { word += ch; continue; }
        if (word.empty()) continue;
        std::string test = line.empty() ? word : line + " " + word;
        if (!line.empty() && MeasureText(test.c_str(), fs) > maxW) { lines.push_back(line); line = word; }
        else line = test;
        word.clear();
    }
    if (!line.empty()) lines.push_back(line);
    return lines;
}

// Draws `text` word-wrapped to maxW, revealing at most `shown` characters (the typewriter effect). Returns the line count.
int DrawWrapped(const char* text, int x, int y, int maxW, int fs, int lineH, int shown, Color c) {
    std::vector<std::string> lines = WrapText(text, maxW, fs);
    int left = shown;
    for (size_t i = 0; i < lines.size() && left > 0; i++) {
        int n = std::min((int)lines[i].size(), left);
        DrawText(lines[i].substr(0, n).c_str(), x, y + (int)i * lineH, fs, c);
        left -= n;
    }
    return (int)lines.size();
}

void DialoguePanel(const NPCInfo& n, float extra = 0) {   // the cream box (taller by `extra` pixels for long text) and the speaker's name tag
    Rectangle box = {40, H - 185.0f - extra, W - 80.0f, 155 + extra};
    DrawRectangleRounded(box, 0.15f, 8, {255, 250, 235, 255});
    DrawRectangleRoundedLines(box, 0.15f, 8, {120, 90, 40, 255});
    DrawRectangleRounded({60, box.y - 18, 130, 34}, 0.5f, 8, n.color);
    DrawText(n.name, 76, (int)box.y - 10, 20, {40, 30, 0, 255});
}

void Dialogue(bool blink) {
    float extra = std::max(0, (int)WrapText(curL1, W - 140, 22).size() - 3) * 26.0f;   // a long line gets a taller box
    DialoguePanel(npcs[talkIdx], extra);
    float top = H - 185.0f - extra;
    int n1 = (int)strlen(curL1), n2 = (int)strlen(curL2), shown = (int)letters;
    int lines1 = DrawWrapped(curL1, 70, (int)top + 28, W - 140, 22, 26, shown, DARKGRAY);
    if (shown > n1 && n2 > 0) DrawWrapped(curL2, 70, (int)top + 28 + lines1 * 26 + 6, W - 140, 20, 24, shown - n1, {110, 90, 60, 255});
    if (!aiWaiting && shown >= n1 + n2 && blink) DrawText("[Space]", W - 150, (int)(top + 125), 20, GRAY);
}

void DrawChoices() {   // what the NPC just said (AI), then your three possible answers
    float extra = std::max(0, (int)WrapText(choiceHeader, W - 140, 20).size() - 2) * 24.0f;   // a long question gets a taller box so your answers still fit inside
    DialoguePanel(npcs[talkIdx], extra);
    float top = H - 185.0f - extra;
    int lines = DrawWrapped(choiceHeader.c_str(), 70, (int)top + 14, W - 140, 20, 24, 1000, DARKGRAY);
    float y0 = top + 20 + lines * 24;
    for (int i = 0; i < 3; i++) {
        float y = y0 + i * 26.0f;
        if (i == choiceSel) DrawRectangleRounded({90, y - 3, 440, 26}, 0.3f, 6, Fade(playerColor, 0.3f));
        if (i == choiceSel) DrawText(">", 70, (int)y, 20, BLACK);
        DrawText(choiceText[i].c_str(), 100, (int)y, 20, i == choiceSel ? BLACK : DARKGRAY);
    }
    DrawText("Up/Down to choose, Space to confirm", W - 330, (int)(top - 14), 14, Fade(WHITE, 0.8f));
}

void DrawTargetPicker() {   // a gossip question: the NPC's line, and a dropdown of the characters you can name
    DialoguePanel(npcs[talkIdx]);
    float top = H - 185.0f;
    DrawWrapped(choiceHeader.c_str(), 70, (int)top + 14, W - 140, 20, 24, 1000, DARKGRAY);
    DrawText("Up/Down to choose who, Space to confirm", 70, (int)(top + 125), 14, GRAY);
    int n = (int)targets.size();
    float rowH = 28, w = 270, h = n * rowH + 46;
    Rectangle box = {W - w - 50.0f, top - h - 14, w, h};
    DrawRectangleRounded(box, 0.08f, 8, {255, 250, 235, 247});
    DrawRectangleRoundedLines(box, 0.08f, 8, {120, 90, 40, 255});
    DrawText("Who was it?", (int)box.x + 16, (int)box.y + 10, 20, DARKGRAY);
    for (int k = 0; k < n; k++) {
        float y = box.y + 40 + k * rowH;
        if (k == targetSel) DrawRectangleRounded({box.x + 8, y - 2, box.width - 16, rowH - 2}, 0.3f, 6, Fade(playerColor, 0.3f));
        DrawRectangle((int)box.x + 18, (int)y + 3, 18, 18, npcs[targets[k]].color);
        DrawRectangleLines((int)box.x + 18, (int)y + 3, 18, 18, DARKGRAY);
        DrawText(npcs[targets[k]].name, (int)box.x + 46, (int)y + 2, 20, k == targetSel ? BLACK : DARKGRAY);
    }
}

void DrawSoulmateScreen() {   // the very first thing you see: pick who you must keep alive
    DrawRectangle(0, 0, W, H, Fade(BLACK, 0.55f));
    Rectangle box = {70, 24, W - 140.0f, 492};
    DrawRectangleRounded(box, 0.05f, 8, {255, 250, 235, 250});
    DrawRectangleRoundedLines(box, 0.05f, 8, {120, 90, 40, 255});
    int x = (int)box.x + 28;
    DrawText("Who's your soulmate?", x, (int)box.y + 18, 34, {90, 40, 120, 255});
    DrawWrapped("Make sure you survive with them. Do everything you can to keep them alive, and ensure you are the two last people surviving. Understood?",
                x, (int)box.y + 62, (int)box.width - 56, 22, 28, 1000, DARKGRAY);
    for (int i = 0; i < NPC_COUNT; i++) {   // the dropdown: every character, by color
        float y = box.y + 158 + i * 30.0f;
        if (i == soulSel) DrawRectangleRounded({box.x + 22, y - 3, 420, 28}, 0.3f, 6, Fade(playerColor, 0.3f));
        if (i == soulSel) DrawText(">", x, (int)y, 22, BLACK);
        DrawRectangle(x + 22, (int)y + 1, 22, 22, npcs[i].color);
        DrawRectangleLines(x + 22, (int)y + 1, 22, 22, DARKGRAY);
        DrawText(npcs[i].name, x + 56, (int)y, 22, i == soulSel ? BLACK : DARKGRAY);   // everyone is just named after their color
    }
    DrawText("Up/Down to choose, Space = Understood", x, (int)(box.y + box.height - 34), 18, GRAY);
}

void DrawGameOver() {   // you lost your soulmate, or you and your soulmate are the last two alive
    DrawRectangle(0, 0, W, H, Fade(BLACK, 0.72f));
    bool won = gameOver == 2;
    const char* title = won ? "You did it!" : "Your soulmate was killed.";
    std::string sub = won ? std::string("You and ") + npcs[soulmate].name + " are the last two people surviving."
                          : std::string(npcs[soulmate].name) + " is gone. You couldn't keep them alive.";
    int tw = MeasureText(title, 48), sw = MeasureText(sub.c_str(), 24);
    DrawText(title, (W - tw) / 2, H / 2 - 70, 48, won ? Color{190, 255, 200, 255} : Color{255, 130, 130, 255});
    DrawText(sub.c_str(), (W - sw) / 2, H / 2 - 5, 24, WHITE);
    std::string tally = std::string("Deaths predicted correctly: ") + std::to_string(pred.right) + "/" + std::to_string(pred.total);
    DrawText(tally.c_str(), (W - MeasureText(tally.c_str(), 22)) / 2, H / 2 + 36, 22, {255, 225, 120, 255});
    const char* again = "Press R or Space to start over";
    if ((int)(timer * 2) % 2 == 0) DrawText(again, (W - MeasureText(again, 22)) / 2, H / 2 + 82, 22, Fade(WHITE, 0.85f));
}

void DrawColorPicker() {   // a simple keyboard-driven "dropdown": Up/Down to move, Space to confirm
    Rectangle box = {40, H - 185.0f, W - 80.0f, 155};
    DrawRectangleRounded(box, 0.15f, 8, {255, 250, 235, 255});
    DrawRectangleRoundedLines(box, 0.15f, 8, {120, 90, 40, 255});
    DrawText("What color are you feeling like today?", (int)box.x + 20, (int)box.y + 10, 20, DARKGRAY);
    for (int i = 0; i < 5; i++) {
        float y = box.y + 40 + i * 21.0f;
        if (i == colorSel) DrawText(">", (int)box.x + 20, (int)y - 2, 18, BLACK);
        DrawRectangle((int)box.x + 45, (int)y, 16, 16, colorValues[i]);
        DrawRectangleLines((int)box.x + 45, (int)y, 16, 16, DARKGRAY);
        DrawText(colorNames[i], (int)box.x + 68, (int)y - 2, 18, DARKGRAY);
    }
    DrawText("Up/Down to choose, Space to confirm", (int)box.x + 260, (int)box.y + 125, 14, GRAY);
}

void DrawNamePrompt() {   // types via the keyboard, confirms with Enter
    Rectangle box = {40, H - 185.0f, W - 80.0f, 155};
    DrawRectangleRounded(box, 0.15f, 8, {255, 250, 235, 255});
    DrawRectangleRoundedLines(box, 0.15f, 8, {120, 90, 40, 255});
    DrawText("Welcome! What is your name?", (int)box.x + 20, (int)box.y + 15, 22, DARKGRAY);
    Rectangle field = {box.x + 20, box.y + 55, 300, 36};
    DrawRectangleRec(field, WHITE);
    DrawRectangleLinesEx(field, 2, DARKGRAY);
    DrawText(nameBuf, (int)field.x + 8, (int)field.y + 8, 22, BLACK);
    if ((int)(timer * 2) % 2 == 0) DrawText("|", (int)field.x + 10 + MeasureText(nameBuf, 22), (int)field.y + 8, 22, BLACK);
    DrawText("Press Enter to confirm", (int)box.x + 20, (int)box.y + 115, 14, GRAY);
}

const char* KeyLabel(int k) {   // a short display name for the handful of keys players might bind
    if (k == KEY_SPACE) return "Space";
    if (k == KEY_ENTER) return "Enter";
    if (k == KEY_LEFT_SHIFT || k == KEY_RIGHT_SHIFT) return "Shift";
    if (k == KEY_LEFT_CONTROL || k == KEY_RIGHT_CONTROL) return "Ctrl";
    if (k >= KEY_A && k <= KEY_Z) { static char buf[2]; buf[0] = (char)k; buf[1] = 0; return buf; }
    static char fallback[16]; snprintf(fallback, sizeof(fallback), "Key %d", k); return fallback;
}

void DrawSettings() {
    Rectangle box = {W / 2 - 250.0f, 40, 500, 440};
    DrawRectangleRounded(box, 0.06f, 8, {255, 250, 235, 250});
    DrawRectangleRoundedLines(box, 0.06f, 8, {120, 90, 40, 255});
    DrawText("Settings", (int)box.x + 20, (int)box.y + 14, 26, DARKGRAY);

    const char* qualityNames[3] = {"Low", "Medium", "High"};
    char labels[7][56];
    snprintf(labels[0], 56, "Volume: %d%%", (int)(sfxVolume * 100));
    snprintf(labels[1], 56, "Graphics: %s", qualityNames[quality]);
    snprintf(labels[2], 56, "Day/Night Cycle: %s", dayNightOn ? "On" : "Off");
    snprintf(labels[3], 56, "Flower Damage: %s", flowerDamageOn ? "On" : "Off");
    snprintf(labels[4], 56, "Interact Key: %s", rebinding && rebindWhich == 4 ? "press any key..." : KeyLabel(keyInteract));
    snprintf(labels[5], 56, "Swing Weapon Key: %s", rebinding && rebindWhich == 5 ? "press any key..." : KeyLabel(keySwing));
    snprintf(labels[6], 56, "Town News Feed: %s", showNews ? "On" : "Off");

    for (int i = 0; i < 7; i++) {
        float y = box.y + 58 + i * 42.0f;
        if (i == settingsSel) DrawRectangleRounded({box.x + 12, y - 4, box.width - 24, 34}, 0.3f, 6, Fade(playerColor, 0.25f));
        DrawText(labels[i], (int)box.x + 24, (int)y + 2, 20, DARKGRAY);
    }
    DrawText("Press P any time while playing to save your file.", (int)box.x + 20, (int)(box.y + box.height - 58), 14, DARKGRAY);
    DrawText("Up/Down select   Left/Right change   Enter rebind   Esc back", (int)box.x + 20, (int)(box.y + box.height - 28), 13, GRAY);
}

// The title-screen notice: dialogue is written by Google Gemini using the player's own API key.
void DrawBuildStamp() {   // the time this copy of the game was compiled: if it never changes after you rebuild, the build failed and you are playing an old copy
    std::string b = std::string("build ") + __DATE__ + " " + __TIME__;
    DrawText(b.c_str(), W - 10 - MeasureText(b.c_str(), 11), H - 15, 11, Fade(WHITE, 0.55f));
}
void DrawAINotice() {
    int st = AIDialogue_Status();
    std::string a, b;
    Color c;
    if (st == 2) {
        a = "Dialogue is written live by Google Gemini. Lines may take a moment to appear.";
        b = "Situations the game has seen before are reused for free, so it gets cheaper (and faster) the longer you play.";
        c = {170, 255, 190, 255};
    } else {
        a = "Characters need a Google Gemini API key to talk. Choose \"Get a free API key\" in the menu, then click \"API key\" at the top of the page.";
        b = "It is free, and your key is saved only in your browser.";
        c = {255, 230, 150, 255};
    }
#ifdef PLATFORM_WEB
    if (SoundStatus() < 2) {   // the browser keeps sound off until the page gets a key press or a click
        std::string w = "Sound is off: press any key or click the page (or the Sound button at the top) to turn it on.";
        DrawText(w.c_str(), (W - MeasureText(w.c_str(), 14)) / 2, 12, 14, {255, 200, 120, 255});
    }
#else
    if (soundsFound < 6) {   // on desktop, silence is almost always a missing assets/sounds folder - say so
        std::string w = std::string("Sound: only ") + std::to_string(soundsFound) + " of 6 sound files found. Put the assets folder (assets/sounds/*.wav) inside game\\ and rebuild.";
        DrawText(w.c_str(), (W - MeasureText(w.c_str(), 14)) / 2, 12, 14, {255, 140, 120, 255});   // top of the screen, out of the way
    }
#endif
    DrawRectangle(0, H - 62, W, 62, Fade(BLACK, 0.45f));
    DrawText(a.c_str(), (W - MeasureText(a.c_str(), 16)) / 2, H - 52, 16, c);
    DrawText(b.c_str(), (W - MeasureText(b.c_str(), 14)) / 2, H - 30, 14, Fade(c, 0.85f));
}

// The title menu: New Game / Load Saved Game / Settings, in a column on the right (the character stands in the middle).
const char* AI_KEY_URL = "https://aistudio.google.com/api-keys?projectFilter=gen-lang-client-0222120615";   // where to get a free Gemini key (opened from the main menu)
const char* MENU_LABELS[4] = {"New Game", "Load Saved Game", "Settings", "Get a free API key"};   // 0 new, 1 load, 2 settings, 3 = a link
const int MENU_ROWS = 4;
Rectangle MenuRowRect(int i) {   // where a menu row is on screen (used to draw the highlight and to hit-test the mouse)
    const int fs = 30, rowH = 46, top = 170;
    int w = MeasureText(MENU_LABELS[i], fs);
    return {W * 0.745f - w / 2.0f - 46, (float)(top + i * rowH) - 6, (float)w + 92, 42};
}

// The title menu: New Game / Load Saved Game / Settings / Get a free API key, in a column on the right (the character stands in the middle).
void DrawMainMenu() {
    const int fs = 30, rowH = 46, top = 170;
    const float cx = W * 0.745f;
    for (int i = 0; i < MENU_ROWS; i++) {
        bool enabled = i != 1 || hasSaveFile, sel = i == menuSel, link = i == 3;
        int y = top + i * rowH, w = MeasureText(MENU_LABELS[i], fs);
        if (sel) {
            DrawRectangleRounded(MenuRowRect(i), 0.4f, 8, {140, 70, 200, 215});
            DrawText(">", (int)(cx - w / 2.0f - 30), y, fs, WHITE);
        }
        Color c = !enabled ? Color{150, 150, 165, 150} : link ? (sel ? Color{190, 245, 255, 255} : Color{120, 215, 255, 255}) : (sel ? WHITE : Color{230, 220, 245, 255});
        DrawText(MENU_LABELS[i], (int)(cx - w / 2.0f), y, fs, c);
        if (link) DrawLine((int)(cx - w / 2.0f), y + fs + 1, (int)(cx + w / 2.0f), y + fs + 1, c);   // underlined: it is a link
        if (i == 1 && !hasSaveFile) {
            const char* n = "no saved game yet";
            DrawText(n, (int)(cx - MeasureText(n, 14) / 2.0f), y + 32, 14, {150, 150, 165, 170});
        }
    }
    const char* hint = "Up/Down or the mouse to choose, Space or a click to select";
    DrawText(hint, (int)(cx - MeasureText(hint, 16) / 2.0f), top + MENU_ROWS * rowH + 20, 16, Fade(WHITE, 0.75f));
    const char* keys = "or press N (new), C (load), S (settings), K (get a key)";
    DrawText(keys, (int)(cx - MeasureText(keys, 14) / 2.0f), top + MENU_ROWS * rowH + 42, 14, Fade(WHITE, 0.55f));
    if (hasSaveFile) {
        const char* note = "A new game keeps your saved file until you press P to save again.";
        DrawText(note, (int)(cx - MeasureText(note, 13) / 2.0f), top + MENU_ROWS * rowH + 66, 13, Fade(WHITE, 0.5f));
    }
    if (menuMsgT > 0) {
        int w = MeasureText(menuMsg.c_str(), 16);
        DrawText(menuMsg.c_str(), (int)(cx - w / 2.0f), top - 40, 16, Fade(menuMsgColor, menuMsgT > 1 ? 1.0f : menuMsgT));
    }
}

void ControlHint() {   // desktop-only hint; touch devices get on-screen buttons instead (see shell.html)
    DrawText(TextFormat("Q/E or drag: look   Z/X or scroll: zoom   %s: swing stick", KeyLabel(keySwing)), 12, H - 24, 14, Fade(WHITE, 0.7f));
}

void DrawHUD(float dayT) {   // top-left: date/time; top-right: name + HP; bottom-left: status lines; center: "File saved!"
    Color clockColor = ColorLerp({90, 80, 110, 255}, WHITE, dayT);
    DrawText(clockStr, 12, 12, 18, clockColor);

    DrawText(playerName, W - 200, 12, 20, WHITE);
    Rectangle bar = {(float)(W - 110), 16, 90, 16};
    DrawRectangleRec(bar, Fade(BLACK, 0.5f));
    DrawRectangleRec({bar.x, bar.y, bar.width * Clamp(playerHP, 0, 100) / 100, bar.height}, RED);
    DrawRectangleLinesEx(bar, 1, WHITE);

    if (soulmate >= 0) {   // the goal, always visible (deliberately NOT showing how many people are left alive)
        bool sAlive = social.alive[soulmate];
        DrawText(TextFormat("Soulmate: %s%s", npcs[soulmate].name, sAlive ? "" : " (dead)"), W - 200, 40, 16, sAlive ? Color{255, 225, 120, 255} : Color{255, 130, 130, 255});
    }

    std::string score = pred.Label();   // "3/5 deaths predicted correct", in a box so it is always easy to see
    std::string g = pred.guess >= 0 ? std::string("Your guess: ") + npcs[pred.guess].name + " dies next (G to change)" : std::string("Press G to guess who dies next");
    int bw = std::max(MeasureText(score.c_str(), 18), MeasureText(g.c_str(), 13)) + 20;
    DrawRectangleRounded({(float)(W - 6 - bw), 58, (float)bw, 50}, 0.25f, 6, Fade(BLACK, 0.5f));
    DrawText(score.c_str(), W - 16 - MeasureText(score.c_str(), 18), 62, 18, {255, 225, 120, 255});
    DrawText(g.c_str(), W - 16 - MeasureText(g.c_str(), 13), 86, 13, Fade(WHITE, 0.85f));

    int alive = 0;
    for (Flower& f : flowers) if (f.hp > 0) alive++;
    DrawText(TextFormat("Flowers: %d", alive), 12, H - 46, 16, Fade(WHITE, 0.85f));

    int st = AIDialogue_Status();   // so you can always see what the AI is costing
    if (st == 2) DrawText(TextFormat("AI dialogue (Gemini): %d paid calls, %d reused free, est. $%.4f, last answer took %.1fs", AIDialogue_Calls(), AIDialogue_Hits(), AIDialogue_MicroUSD() / 1000000.0, AIDialogue_LastMs() / 1000.0), 12, H - 68, 14, {170, 255, 190, 210});
    else DrawText("AI dialogue needs your Gemini API key: click \"API key\" at the top of the page", 12, H - 68, 14, {255, 230, 150, 230});
    DrawText("Press P to save your file", 12, H - 90, 14, Fade(WHITE, 0.85f));
    std::string aiProblem = AIDialogue_LastError();   // while the AI is failing, say why (it clears itself on the next success)
    if (st == 2 && !aiProblem.empty()) DrawText(("AI problem: " + aiProblem.substr(0, 100)).c_str(), 12, H - 108, 14, {255, 150, 130, 255});

    if (saveMsgT > 0) {
        float a = Clamp(saveMsgT, 0, 1);
        const char* msg = "File saved!";
        int w = MeasureText(msg, 40);
        DrawRectangleRounded({(W - w) / 2.0f - 24, 56, w + 48.0f, 62}, 0.4f, 8, Fade(BLACK, 0.6f * a));
        DrawText(msg, (W - w) / 2, 66, 40, Fade({190, 255, 200, 255}, a));
    }
}

void DrawNews() {   // the "town news" feed: crushes, couples, affairs, breakups, and what you've done
    if (!showNews) return;
    int y = 38;
    for (const NewsItem& it : news) {
        float a = Clamp((14.0f - it.age) / 3.0f, 0, 1);
        if (a <= 0) continue;
        DrawText(it.text.c_str(), 13, y + 1, 15, Fade(BLACK, a * 0.6f));
        DrawText(it.text.c_str(), 12, y, 15, Fade(it.color, a));
        y += 18;
    }
}

void DrawMiniMap() {   // a simple top-down radar in the corner
    float cx = W - 70.0f, cy = H - 70.0f, R = 55.0f;
    DrawCircle(cx, cy, R + 4, Fade(BLACK, 0.45f));
    DrawCircleLines(cx, cy, R + 4, WHITE);
    float s = scene == OUTSIDE ? R / 50.0f : R / 10.0f;
    if (scene == OUTSIDE) DrawRectangle(cx - 4, cy + (-12) * s - 4, 8, 8, WHITE);   // the building
    for (NPCInfo& n : npcs)
        if (n.loc == scene && !n.gone) DrawCircle(Clamp(cx + n.pos.x * s, cx - R, cx + R), Clamp(cy + n.pos.z * s, cy - R, cy + R), 4, n.color);
    DrawCircle(Clamp(cx + player.pos.x * s, cx - R, cx + R), Clamp(cy + player.pos.z * s, cy - R, cy + R), 5, playerColor);
    DrawCircleLines(Clamp(cx + player.pos.x * s, cx - R, cx + R), Clamp(cy + player.pos.z * s, cy - R, cy + R), 5, WHITE);
}

void DrawNPCLabels(Camera3D cam) {   // name above each nearby NPC, plus a line about who they love / feel about you
    for (int i = 0; i < NPC_COUNT; i++) {
        NPCInfo& n = npcs[i];
        if (n.loc != scene || n.gone || n.dying || Vector2Distance({n.pos.x, n.pos.z}, {player.pos.x, player.pos.z}) > 16) continue;
        Vector2 sp = GetWorldToScreen({n.pos.x, 2.75f, n.pos.z}, cam);
        int w = MeasureText(n.name, 16);
        DrawText(n.name, (int)sp.x - w / 2 + 1, (int)sp.y - 7, 16, Fade(BLACK, 0.6f));
        DrawText(n.name, (int)sp.x - w / 2, (int)sp.y - 8, 16, WHITE);
        if (i == soulmate) {
            int w0 = MeasureText("<3 YOUR SOULMATE", 14);
            DrawText("<3 YOUR SOULMATE", (int)sp.x - w0 / 2 + 1, (int)sp.y - 23, 14, Fade(BLACK, 0.6f));
            DrawText("<3 YOUR SOULMATE", (int)sp.x - w0 / 2, (int)sp.y - 24, 14, {255, 225, 120, 255});
        }
        if (bubbleT[i] > 0) {   // a line they're saying to another villager - you're overhearing it
            std::vector<std::string> bl = WrapText(bubbleText[i], 250, 16);
            int bw = 0;
            for (const std::string& l : bl) bw = std::max(bw, MeasureText(l.c_str(), 16));
            int bh = (int)bl.size() * 19 + 12;
            float a = Clamp(bubbleT[i], 0.0f, 1.0f);   // fades out over the last second
            Rectangle r = {sp.x - bw / 2.0f - 10, sp.y - 38.0f - bh, bw + 20.0f, (float)bh};
            r.x = Clamp(r.x, 6.0f, W - r.width - 6.0f); r.y = fmaxf(r.y, 74.0f);
            DrawRectangleRounded(r, 0.3f, 6, Fade(WHITE, 0.95f * a));
            DrawRectangleRoundedLines(r, 0.3f, 6, Fade({120, 90, 40, 255}, a));
            DrawTriangle({sp.x - 7, r.y + r.height - 1}, {sp.x + 7, r.y + r.height - 1}, {sp.x, r.y + r.height + 9}, Fade(WHITE, 0.95f * a));
            for (size_t k = 0; k < bl.size(); k++) DrawText(bl[k].c_str(), (int)r.x + 10, (int)r.y + 6 + (int)k * 19, 16, Fade({40, 30, 10, 255}, a));
        }
        std::string st; Color sc = WHITE;
        if (social.partner[i] >= 0) { st = std::string("<3 ") + npcs[social.partner[i]].name; sc = {255, 150, 200, 255}; }
        else if (social.crush[i] >= 0) { st = std::string("crush: ") + npcs[social.crush[i]].name; sc = {255, 205, 225, 255}; }
        else if (n.opinion < -30) { st = "angry at you"; sc = {255, 120, 120, 255}; }
        else if (n.opinion > 30) { st = "likes you"; sc = {150, 255, 170, 255}; }
        if (!st.empty()) {
            int w2 = MeasureText(st.c_str(), 13);
            DrawText(st.c_str(), (int)sp.x - w2 / 2 + 1, (int)sp.y + 10, 13, Fade(BLACK, 0.6f));
            DrawText(st.c_str(), (int)sp.x - w2 / 2, (int)sp.y + 9, 13, sc);
        }
    }
}

const float ATTACK_COOLDOWN = 45.0f;   // seconds before the same NPC can start another fight (raise it for an even calmer village)

// ---------------- the NPC world: doors, wandering, walking ----------------
bool WallBlocked(Scene s, Vector3 p) {
    if (s == OUTSIDE) return (fabsf(p.x) < 3.4f && p.z > -15.4f && p.z < -8.6f) || fabsf(p.x) > 60 || fabsf(p.z) > 60;   // the building, the edge of the world
    return fabsf(p.x) > 9.5f || fabsf(p.z) > 9.5f;                                                                       // room walls
}
Vector3 DoorPos(Scene from, Scene to) {      // where, inside `from`, the door leading to `to` is
    if (from == OUTSIDE && to == ROOM1) return {0, 0, -8.4f};
    if (from == ROOM1 && to == OUTSIDE) return {0, 0, -9};
    if (from == ROOM1 && to == ROOM2) return {9, 0, 0};
    return {-9, 0, 0};                       // ROOM2 -> ROOM1
}
Vector3 ArrivePos(Scene from, Scene to) {    // where you appear in `to` after coming through that door
    if (from == OUTSIDE && to == ROOM1) return {0, 0, -7};
    if (from == ROOM1 && to == OUTSIDE) return {0, 0, -6.5f};
    if (from == ROOM1 && to == ROOM2) return {-8, 0, 0};
    return {8, 0, 0};                        // ROOM2 -> ROOM1
}
Scene NextHop(Scene cur, Scene goal) {       // the three places form a line: OUTSIDE - ROOM1 - ROOM2
    if (cur == goal) return cur;
    return cur == ROOM1 ? goal : ROOM1;
}
Vector3 RandomPointIn(Scene s) {             // outdoors, NPCs hang around near the building so they actually meet each other
    if (s == OUTSIDE) return {(float)GetRandomValue(-12, 12), 0, (float)GetRandomValue(-4, 10)};
    return {(float)GetRandomValue(-8, 8), 0, (float)GetRandomValue(-8, 8)};
}
bool NearOtherNPC(int self, Vector3 p) {
    for (int j = 0; j < NPC_COUNT; j++)
        if (j != self && !npcs[j].gone && npcs[j].loc == npcs[self].loc && Vector2Distance({p.x, p.z}, {npcs[j].pos.x, npcs[j].pos.z}) < 0.9f) return true;
    return false;
}
void StepNPC(int i, Vector3 dest, float dt) {   // walk toward `dest`, but not through walls, the player, or each other
    NPCInfo& n = npcs[i];
    Vector2 d = {dest.x - n.pos.x, dest.z - n.pos.z};
    float len = Vector2Length(d);
    if (len < 0.05f) { n.moving = false; return; }
    d = Vector2Scale(d, 2.2f * dt / len);
    Vector3 np = {n.pos.x + d.x, 0, n.pos.z + d.y};
    bool blocked = WallBlocked(n.loc, np) || NearOtherNPC(i, np) ||
                   (n.loc == scene && Vector2Distance({np.x, np.z}, {player.pos.x, player.pos.z}) < 0.9f);
    if (blocked) { n.moving = false; n.stuckT += dt; return; }
    n.pos = np; n.yaw = atan2f(d.x, d.y) * RAD2DEG; n.moving = true; n.walk += dt; n.stuckT = 0;
}
bool Living(int i) { return social.alive[i] && !npcs[i].gone && !npcs[i].dying && npcs[i].windT <= 0 && npcs[i].frozenT <= 0; }   // can be talked to, hit, or pick a fight

// ---------------- "who dies next?" ----------------
// After each death (and when a new game begins) you guess who will be killed next. Every death is scored against your guess: the corner of the
// screen shows e.g. "3/5 deaths predicted correct" (a death you made no guess for counts as a miss). G changes your guess any time.
std::vector<int> GuessCandidates() { std::vector<int> c; for (int i = 0; i < NPC_COUNT; i++) if (Living(i)) c.push_back(i); return c; }
bool AnyoneDying() { for (const NPCInfo& n : npcs) if (n.dying || n.windT > 0) return true; return false; }
void AddNews(const std::string& text, Color c);
void ScanDeaths() {   // score each new death exactly once (the death is recorded when the knife comes down)
    for (; pred.scanned < (int)social.events.size(); pred.scanned++) {
        if (social.events[pred.scanned].type != EV_KILL) continue;
        int victim = social.events[pred.scanned].b, g = pred.guess;
        pred.Death(victim); needGuess = true;
        std::string v = npcs[victim].name, tally = " (" + pred.Label() + ")";   // a line in the news feed right away, before the guess screen opens
        if (g == victim) AddNews(std::string("You called it: ") + v + " died!" + tally, {255, 225, 120, 255});
        else if (g >= 0) AddNews(std::string("You guessed ") + npcs[g].name + ", but " + v + " died." + tally, {255, 200, 150, 255});
        else AddNews(std::string("You had no guess for ") + v + "'s death." + tally, {255, 200, 150, 255});
    }
}
void OpenGuess() {
    needGuess = false;
    std::vector<int> c = GuessCandidates();
    if (c.size() < 2) return;                                          // nobody to choose between
    guessing = true; guessSel = 0;
    for (size_t k = 0; k < c.size(); k++) if (c[k] == pred.guess) guessSel = (int)k;
}
void UpdateGuess() {   // one frame of the guess screen (the world is paused while it is open)
    std::vector<int> c = GuessCandidates();
    if (c.size() < 2) { guessing = false; return; }
    if (guessSel >= (int)c.size()) guessSel = 0;
    if (IsKeyPressed(KEY_UP)) guessSel = (guessSel + (int)c.size() - 1) % (int)c.size();
    if (IsKeyPressed(KEY_DOWN)) guessSel = (guessSel + 1) % (int)c.size();
    if (IsKeyPressed(KEY_SPACE) || IsKeyPressed(KEY_ENTER)) { pred.guess = c[guessSel]; guessing = false; }
    else if (IsKeyPressed(KEY_ESCAPE) || IsKeyPressed(KEY_BACKSPACE)) guessing = false;   // skip: no guess this time
}
void DrawGuessScreen() {
    DrawRectangle(0, 0, W, H, Fade(BLACK, 0.55f));
    Rectangle box = {70, 24, W - 140.0f, 492};
    DrawRectangleRounded(box, 0.05f, 8, {255, 250, 235, 250});
    DrawRectangleRoundedLines(box, 0.05f, 8, {120, 90, 40, 255});
    int x = (int)box.x + 28;
    DrawText("Who dies next?", x, (int)box.y + 18, 34, {150, 40, 60, 255});
    DrawWrapped("Someone is going to be killed next. Guess who! Every death is scored against your guess, and your score stays in the corner of the screen.",
                x, (int)box.y + 62, (int)box.width - 56, 22, 28, 1000, DARKGRAY);
    std::string score = std::string("So far: ") + pred.Label();
    DrawText(score.c_str(), (int)(box.x + box.width) - 28 - MeasureText(score.c_str(), 18), (int)box.y + 22, 18, {120, 90, 40, 255});
    std::vector<int> c = GuessCandidates();
    for (size_t k = 0; k < c.size(); k++) {   // everyone still alive, by color
        float y = box.y + 140 + k * 30.0f;
        int i = c[k];
        if ((int)k == guessSel) DrawRectangleRounded({box.x + 22, y - 3, 420, 28}, 0.3f, 6, Fade(npcs[i].color, 0.35f));
        if ((int)k == guessSel) DrawText(">", x, (int)y, 22, BLACK);
        DrawRectangle(x + 22, (int)y + 1, 22, 22, npcs[i].color);
        DrawRectangleLines(x + 22, (int)y + 1, 22, 22, DARKGRAY);
        DrawText(npcs[i].name, x + 56, (int)y, 22, (int)k == guessSel ? BLACK : DARKGRAY);
        if (i == soulmate) DrawText("(your soulmate)", x + 180, (int)y + 2, 18, {170, 120, 20, 255});
    }
    DrawText("Up/Down to choose, Space to lock in your guess, Esc to skip", x, (int)(box.y + box.height - 34), 18, GRAY);
}
void HurtPlayer(float amount) { playerHP -= amount; playerHitT = 1.0f; }   // you flash red for a second, like taking damage in Minecraft

// ---------------- the town news feed ----------------
void AddNews(const std::string& text, Color c = WHITE) {
    news.push_back({text, 0, c});
    if (news.size() > 8) news.erase(news.begin());
}
std::string NewsText(const SocialEvent& e) {
    std::string A = npcs[e.a].name, B = e.b >= 0 ? npcs[e.b].name : "", C = e.c >= 0 ? npcs[e.c].name : "";
    switch (e.type) {
        case EV_CRUSH: return A + " has a crush on " + B;
        case EV_DATING: return A + " and " + B + " are dating now";
        case EV_BREAKUP: return A + " and " + B + " broke up";
        case EV_CHEAT: return A + " cheated on " + C + " with " + B + "!";
        case EV_DISCOVER: return A + " found out " + B + " cheated with " + C + "!";
        case EV_PUNCH: return A + " punched " + B;
        default: return A + " killed " + B + "!";
    }
}
Color NewsColor(EventType t) {
    switch (t) {
        case EV_CRUSH: return {255, 200, 225, 255};
        case EV_DATING: return {255, 140, 190, 255};
        case EV_BREAKUP: return {170, 190, 230, 255};
        case EV_CHEAT: return {255, 170, 90, 255};
        case EV_PUNCH: return {255, 210, 130, 255};
        case EV_KILL: return {255, 70, 70, 255};
        default: return {255, 110, 110, 255};
    }
}
std::string Sentence(std::string s) { if (!s.empty() && s.back() != '.' && s.back() != '!' && s.back() != '?') s += '.'; return s; }

// ---------------- conversations: the AI (Gemini) writes every line ----------------
int PickKnownEvent(int npc) {   // a recent bit of gossip this NPC actually knows about
    std::vector<int> known;
    for (size_t k = 0; k < social.events.size(); k++)
        if (Knows(social.events[k], npc) && timer - social.events[k].time < 300) known.push_back((int)k);
    return known.empty() ? -1 : known[GetRandomValue(0, (int)known.size() - 1)];
}
int LatestKill() {   // a murder stays on everyone's mind for a long while
    for (int k = (int)social.events.size() - 1; k >= 0; k--)
        if (social.events[k].type == EV_KILL && timer - social.events[k].time < 1200) return k;
    return -1;
}
float ColorMatchFor(const NPCInfo& n) {
    if (n.favColor.a && ColorsEqual(playerColor, n.favColor)) return 1;
    if (n.dislikeColor.a && ColorsEqual(playerColor, n.dislikeColor)) return -1;
    return 0;
}


// ---------- describing a character to the AI as cheaply as possible ----------
// Instead of sending a full memory log, the game boils each character down to a few COARSE bands (how much they like the
// player, how well they know them, who they are dating / friends with / enemies with). The same bands form the CACHE KEY, so
// the same situation later reuses a stored line for free - and a stored line is only reused while the facts it was written
// from still hold. The player's real name never appears in a prompt: lines say {player} and it is filled in here.
int DayBand() { return dayT < 0.3f ? 0 : dayT > 0.7f ? 2 : 1; }                               // night / evening / daytime

struct NRel { int other; char code; };
std::vector<NRel> NotableRels(int idx) {   // the (up to) 3 most dramatic relationships this villager has: dating, a crush, exes, hurt, rivals, enemies, best friends
    std::vector<NRel> v;
    for (int j = 0; j < NPC_COUNT; j++) {
        if (j == idx || !social.alive[j]) continue;
        char c = PairDynCode(social, idx, j);
        if (PairDynRank(c) < 99) v.push_back({j, c});
    }
    std::sort(v.begin(), v.end(), [&](const NRel& x, const NRel& y) {
        int rx = PairDynRank(x.code), ry = PairDynRank(y.code);
        return rx != ry ? rx < ry : fabsf(social.affinity[idx][x.other]) > fabsf(social.affinity[idx][y.other]);
    });
    if (v.size() > 3) v.resize(3);
    return v;
}
std::string PairLine(int a, int b) {   // how two villagers stand with each other, from both sides, in a sentence or two (a mutual one is said once)
    char ca = PairDynCode(social, a, b), cb = PairDynCode(social, b, a);
    std::string A = npcs[a].name, B = npcs[b].name;
    std::string f = (ca == cb && (ca == 'p' || ca == 'x' || ca == 'R' || ca == 'B')) ? PairDynPhrase(ca, A, B) : PairDynPhrase(ca, A, B) + " " + PairDynPhrase(cb, B, A);
    if (social.clashes[a][b] > 0 && ca != 'R' && cb != 'R') f += " They have come to blows.";
    return f;
}
// The cache key describes everything Facts() says, as a short string: this villager's fingerprint (so a new game's different
// cast never reuses an old game's lines), how they see the player, and their notable relationships.
std::string StateKey(int idx, bool full) {
    std::string k = std::to_string(idx) + ":" + npcs[idx].pid.substr(0, 6) + ":" + PlayerDyn(idx) + (idx == soulmate ? "s" : "-");
    if (full) for (const NRel& r : NotableRels(idx)) k += ":" + std::string(1, r.code) + std::to_string(r.other);
    return k;
}
std::string Csv(const std::vector<std::string>& v) { std::string s; for (size_t i = 0; i < v.size(); i++) s += (i ? "," : "") + v[i]; return s; }

std::string Bio(const NPCInfo& n) { return std::string(n.name) + ": " + n.job + "; " + n.voice + "; " + n.dialect + "; " + n.quirk + "."; }

// Only YOUR SOULMATE is told you're their soulmate (and told to keep it secret): nobody else's facts ever mention it.
std::string Facts(int idx, const std::string& situation, bool full) {
    const NPCInfo& n = npcs[idx];
    std::string N = n.name;
    std::string f = Bio(n) + " ";
    f += N + " " + PlayerDynPhrase(PlayerDyn(idx)) + ".";   // how they currently see YOU, and how that should sound
    if (full) {
        std::vector<NRel> rels = NotableRels(idx);
        for (const NRel& r : rels) f += " " + PairDynPhrase(r.code, N, npcs[r.other].name);
        if (!rels.empty()) f += " " + N + " likes bringing these relationships up.";
    }
    if (idx == soulmate) f += " " + N + " secretly knows the player is " + N + "'s soulmate, a secret only the two of them share, so " + N + " never tells anyone else.";
    return f + situation;
}

std::string KillFacts(int idx, const SocialEvent& e) {   // how THIS speaker relates to a murder - to the victim AND to the killer
    std::string N = npcs[idx].name, K = npcs[e.a].name, V = npcs[e.b].name;
    if (idx == e.a) return " " + N + " just killed " + V + ", everyone knows, and " + N + " is cold and tense.";
    float lovedVictim = social.affinity[idx][e.b], likesKiller = social.affinity[idx][e.a];
    if (lovedVictim > 60) return " " + K + " killed " + V + ", " + N + "'s dearest friend. " + N + " is devastated and wants revenge.";
    if (lovedVictim > 25) return " " + K + " killed " + N + "'s friend " + V + ". " + N + " is furious.";
    std::string s = lovedVictim < -30 ? " " + K + " killed " + V + ", whom " + N + " hated. " + N + " is secretly glad but nervous."
                                      : " " + K + " killed " + V + ". " + N + " is terrified and shocked.";
    if (likesKiller > 40) s += " " + N + " likes " + K + " and is torn about it.";
    else if (likesKiller < -40) s += " " + N + " never trusted " + K + " and is not surprised.";
    return s;
}

// ---------------- topics: what characters talk about ----------------
int PickOther(int a, int exclude = -1) {   // somebody for `a` to talk about, favoring people they have strong feelings about
    std::vector<int> strong, any;
    for (int j = 0; j < NPC_COUNT; j++) {
        if (j == a || j == exclude || !Living(j)) continue;
        any.push_back(j);
        if (fabsf(social.affinity[a][j]) > 25 || social.partner[a] == j || social.crush[a] == j) strong.push_back(j);
    }
    const std::vector<int>& pool = (!strong.empty() && GetRandomValue(0, 99) < 70) ? strong : any;
    return pool.empty() ? -1 : pool[GetRandomValue(0, (int)pool.size() - 1)];
}
int RandomTopic() {   // their interests, the outside world, and each other come up most
    static const int w[TOPIC_COUNT] = {3, 3, 4, 3, 4, 3, 1, 10, 1};   // job food outside human hobby dream fear other time
    int total = 0; for (int x : w) total += x;
    int r = GetRandomValue(0, total - 1);
    for (int t = 0; t < TOPIC_COUNT; t++) { if (r < w[t]) return t; r -= w[t]; }
    return T_OTHER;
}

std::string TopicFacts(int a, const std::string& L, int topic, int other) {   // the situation sentence for a topic; L = who they're talking to
    const NPCInfo& n = npcs[a];
    std::string N = n.name;
    switch (topic) {
        case T_JOB: return " " + N + " is telling " + L + " about being " + n.job + ".";
        case T_FOOD: return " " + N + "'s favorite food is " + n.food + ". " + N + " is telling " + L + " about it.";
        case T_OUTSIDE: return " " + N + " pictures the outside world as " + n.outside + ", and keeps wondering what is really out there. " + N + " is telling " + L + " about it.";
        case T_HUMAN: return " " + N + " " + n.humans + ". " + N + " is telling " + L + " about the human world.";
        case T_HOBBY: return " " + N + " loves " + n.hobby + " and is telling " + L + " about it.";
        case T_DREAM: return " " + N + " dreams of " + n.dream + " and is telling " + L + " about it.";
        case T_FEAR: return " " + N + " is afraid of " + n.fear + " and is telling " + L + " about it.";
        case T_OTHER: {
            if (other < 0) return " " + N + " is telling " + L + " about being " + n.job + ".";
            return " " + N + " is talking to " + L + " about " + npcs[other].name + " and how things stand between them. " + PairLine(a, other) + " " + npcs[other].name + " is " + npcs[other].job + " who " + npcs[other].premise + ".";
        }
        default: return std::string(" It is ") + (dayT < 0.3f ? "night" : dayT > 0.7f ? "daytime" : "evening") + ". " + N + " is chatting with " + L + " about the time of day.";
    }
}
const char* TopicEmotion(int a, int topic) {   // the one-word mood printed in the prompt for a topic
    switch (topic) {
        case T_JOB: return "proud";
        case T_FOOD: return "excited";
        case T_OUTSIDE: case T_HUMAN: return "curious";
        case T_HOBBY: return "cheerful";
        case T_DREAM: return "dreamy";
        case T_FEAR: return "nervous";
        case T_TIME: return dayT < 0.3f ? "sleepy" : "relaxed";
        default: return "chatty";
    }
}
// Words a line on this topic should mention: if it mentions none of them, it wandered off-topic and gets re-rolled.
std::vector<std::string> TopicKeywords(int a, int topic, int other) {
    const NPCInfo& n = npcs[a];
    std::vector<std::string> k;
    auto addWords = [&](const std::string& phrase) {
        static const char* stop[] = {"with", "just", "that", "this", "from", "have", "been", "into", "your", "them", "they", "what", "some", "their", "there", "about", "does", "dreams", "thinks"};
        std::string w;
        for (size_t i = 0; i <= phrase.size(); i++) {
            if (i < phrase.size() && phrase[i] != ' ') { w += phrase[i]; continue; }
            bool skip = w.size() < 4;
            for (const char* sw : stop) if (w == sw) skip = true;
            if (!skip) k.push_back(w);
            w.clear();
        }
    };
    switch (topic) {
        case T_JOB: addWords(n.job); k.insert(k.end(), {"work", "job", "busy", "customer", "shift", "all day"}); break;
        case T_FOOD: addWords(n.food); k.insert(k.end(), {"food", "eat", "taste", "delicious", "hungry", "meal"}); break;
        case T_OUTSIDE: addWords(n.outside); k.insert(k.end(), {"outside", "world", "beyond", "edge", "out there", "far away", "past the", "wonder"}); break;
        case T_HUMAN: k = {"human"}; break;
        case T_HOBBY: addWords(n.hobby); k.insert(k.end(), {"love", "hobby", "fun"}); break;
        case T_DREAM: addWords(n.dream); k.insert(k.end(), {"dream", "someday", "wish", "hope"}); break;
        case T_FEAR: addWords(n.fear); k.insert(k.end(), {"afraid", "scared", "fear", "nervous"}); break;
        case T_OTHER: if (other >= 0) k.push_back(npcs[other].name); break;
        default: k = {"night", "dark", "day", "sun", "star", "sleep", "morning", "evening", "moon", "sky"}; break;
    }
    return k;
}

std::string Emotion(int idx, int ctx, int eventIdx) {   // the one-word mood printed in the prompt
    const NPCInfo& n = npcs[idx];
    if (ctx == CTX_KILL && eventIdx >= 0) {
        const SocialEvent& e = social.events[eventIdx];
        if (idx == e.a) return "cold";
        float lovedVictim = social.affinity[idx][e.b], likesKiller = social.affinity[idx][e.a];
        if (lovedVictim > 60) return "heartbroken";
        if (lovedVictim > 25) return "furious";
        if (likesKiller > 40) return "conflicted";
        if (likesKiller < -40) return "grim";
        return lovedVictim < -30 ? "secretly glad" : "terrified";
    }
    if (ctx == CTX_THANK) return "delighted";
    if (ctx == CTX_CONFRONT) return "angry";
    if (ctx == CTX_GOSSIP) return "excited";
    if (ctx == CTX_RUMOR) return convRumorKind == 0 ? "upset" : convRumorKind == 1 ? "touched" : "flustered";
    char d = PlayerDyn(idx);   // how they feel about YOU colors their mood
    const char* dm = d == 'H' ? "aggressive" : d == 'G' ? "bitter" : d == 'S' ? "wary" : d == 'C' ? "flirty" : nullptr;
    if (ctx == CTX_LOVE) return dm ? std::string(dm) : std::string("emotional");
    if (ctx == CTX_ASK) return dm ? std::string(dm) : std::string("playful");
    if (ctx == CTX_MEMORY) return dm ? std::string(dm) : std::string("excited");
    if (ctx == CTX_TOPIC) return dm ? std::string(dm) : (d == 'K' || d == 'F') ? std::string(TopicEmotion(idx, convTopic)) + " and warm" : std::string(TopicEmotion(idx, convTopic));
    return dm ? std::string(dm) : d == 'K' ? std::string("warm") : n.opinion > 30 ? std::string("happy") : std::string("calm");
}

// ---------------- what the player has told villagers (memory) ----------------
void Remember(NPCInfo& n, const std::string& fact) {   // keep the 3 newest
    n.memory.push_back(fact);
    while (n.memory.size() > 3) n.memory.erase(n.memory.begin());
}
bool PickMemory(int idx, int& owner, std::string& fact) {   // something the player told a villager: their own memory (twice as likely) or another's, which they could have heard
    std::vector<std::pair<int, std::string>> all;
    for (int j = 0; j < NPC_COUNT; j++)
        if (Living(j)) for (const std::string& m : npcs[j].memory) { all.push_back({j, m}); if (j == idx) all.push_back({j, m}); }
    if (all.empty()) return false;
    const std::pair<int, std::string>& p = all[GetRandomValue(0, (int)all.size() - 1)];
    owner = p.first; fact = p.second;
    return true;
}
std::string MemoryNote(int idx, int owner, const std::string& fact) {   // what `idx` knows, and how they stand with whoever it came from
    std::string N = npcs[idx].name, O = npcs[owner].name;
    if (owner == idx) return " " + N + " remembers that " + fact + " (the player told " + N + " this) and brings it up with feeling.";
    return " " + N + " heard from " + O + " that " + fact + " and can't help reacting to it (surprise, delight, jealousy, teasing) given how " + N + " feels about the player. " +
           PairDynPhrase(PairDynCode(social, idx, owner), N, O);
}
bool Roll(int percent) { return GetRandomValue(0, 99) < percent; }
float MurderAge(int ev) { return timer - social.events[ev].time; }   // seconds since this murder
int MurderChance(int ev) { return MurderAge(ev) < 300 ? 60 : 30; }     // a murder is on everyone's mind at first, and still comes up for a good while

// ---------------- the AI's words are shown exactly as written ----------------
// There is NO content filter: no swearing/link/@/# blocklist, no "stuck repeating a word" check, nothing cut at a quote mark, no length
// judging. (Google's own safety system still applies on its side: if it blocks an answer the game says so.) Setting this to true brings the
// old filter back (see LooksUsable and CleanGenerated in jsonutil.h).
const bool FILTER_AI_TEXT = false;
std::string Tidy(const std::string& s) {   // just trims stray spaces
    if (FILTER_AI_TEXT) return CleanGenerated(s);
    size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}
bool Usable(const std::string& s) { return FILTER_AI_TEXT ? LooksUsable(s) : !s.empty(); }   // anything at all is fine

// A speaker's catchphrase (", period.") stays a flavour, not a habit: at most once every TIC_GAP lines, and only some of the time (about one line in six).
const int TIC_GAP = 5;
TicGate ticGate[NPC_COUNT];
std::string WithTic(const std::string& line, int idx) {
    if (idx < 0 || idx >= NPC_COUNT || line.empty() || line[0] == '(' || !ticGate[idx].Ready(TIC_GAP)) return line;
    std::string out = ApplyTic(line, npcs[idx].tic, (float)GetRandomValue(0, 999) / 1000.0f, 0.45f);
    if (out != line) ticGate[idx].Used();
    return out;
}

// ---------------- villagers chatting with each other (overheard, in speech bubbles) ----------------
// One request writes (or the cache supplies) a whole 3-line exchange, which is then played out line by line.
std::string ChatFacts(int a, int b) {   // both characters, briefly, how they stand, and what they're discussing (chatNote). Never mentions the player's soulmate.
    const NPCInfo &A = npcs[a], &B = npcs[b];
    auto chatBio = [](const NPCInfo& n) { return std::string(n.name) + ": " + n.job + "; " + n.voice + "; " + n.dialect + "."; };   // (chats are short: the quirk is left out)
    return chatBio(A) + "\n" + chatBio(B) + "\n" + PairLine(a, b) + "\nThey are chatting." + chatNote;
}
std::string ChatEmotion(int a, int b, int topic) {
    if (social.partner[a] == b) return "affectionate";
    if (social.affinity[a][b] > 40) return "friendly";
    if (social.affinity[a][b] < -40) return "irritated";
    return std::string(TopicEmotion(a, topic));
}
void EndChat() {
    if (chatA >= 0) npcs[chatA].chatT = 0;
    if (chatB >= 0) npcs[chatB].chatT = 0;
    chatBusy = chatStarted = false; chatA = chatB = -1; chatLines.clear(); chatIdx = 0;
    chatCooldown = 10.0f + GetRandomValue(0, 8);
}
void FaceEachOther(int a, int b) {
    npcs[a].yaw = atan2f(npcs[b].pos.x - npcs[a].pos.x, npcs[b].pos.z - npcs[a].pos.z) * RAD2DEG;
    npcs[b].yaw = atan2f(npcs[a].pos.x - npcs[b].pos.x, npcs[a].pos.z - npcs[b].pos.z) * RAD2DEG;
}

// What are these two going to talk about? A recent murder (often), something the player told a villager, or just life.
void PlanChat() {
    const NPCInfo &A = npcs[chatA], &B = npcs[chatB];
    int kev = LatestKill(), owner = -1;
    std::string fact;
    bool killerHere = kev >= 0 && (chatA == social.events[kev].a || chatB == social.events[kev].a);   // the killer wouldn't chat about it
    if (kev >= 0 && !killerHere && Roll(MurderChance(kev))) {
        const SocialEvent& e = social.events[kev];
        chatNote = KillFacts(chatA, e) + KillFacts(chatB, e);
        chatKw = Csv({npcs[e.a].name, npcs[e.b].name, "kill", "dead", "gone", "murder"});
        chatKey = "kill" + std::to_string(kev); chatMood = "shaken";
    } else if (PairDynRank(PairDynCode(social, chatA, chatB)) < 99 && Roll(35)) {   // they have a real relationship: the chat is about it
        chatNote = " They talk about their feelings for each other and what is going on between them (as fits: flirting, tension, hurt, rivalry, friendship).";
        chatKw = ""; chatKey = "rel"; chatMood = ChatEmotion(chatA, chatB, T_OTHER);
    } else if (PickMemory(chatA, owner, fact) && owner != chatB && Roll(30)) {
        chatNote = " " + std::string(A.name) + " is telling " + B.name + " what the player once told " + npcs[owner].name + ": " + fact + ". " +
                   A.name + " " + PlayerDynPhrase(PlayerDyn(chatA)) + ". " + B.name + " " + PlayerDynPhrase(PlayerDyn(chatB)) + ".";
        chatKw = Csv(KeyWords(fact));
        chatKey = "mem" + std::to_string(owner) + ShortHash(fact) + PlayerDyn(chatA) + PlayerDyn(chatB); chatMood = "gossipy";
    } else {
        int topic = RandomTopic(), other = topic == T_OTHER ? PickOther(chatA, chatB) : -1;
        if (topic == T_OTHER && other < 0) topic = T_JOB;
        chatNote = TopicFacts(chatA, B.name, topic, other); chatKw = Csv(TopicKeywords(chatA, topic, other));
        chatKey = std::to_string(topic) + "|" + std::to_string(other) + "|" + std::to_string(topic == T_TIME ? DayBand() : 0); chatMood = ChatEmotion(chatA, chatB, topic);
    }
}

void UpdateChatter(float dt) {
    for (int i = 0; i < NPC_COUNT; i++) if (bubbleT[i] > 0) bubbleT[i] -= dt;
    if (chatCooldown > 0) chatCooldown -= dt;
    bool blocked = talking || aiWaiting || gameOver || killCineT >= 0 || fadeDir != 0 || scene == MENU || AIDialogue_Status() != 2 || AIDialogue_Throttled();   // (chatter holds off while the AI is rate-limited or busy, so it never delays YOUR conversations)
    if (chatBusy && (blocked || !Living(chatA) || !Living(chatB) || npcs[chatA].loc != scene || npcs[chatB].loc != scene)) { EndChat(); return; }   // your own conversation always wins
    if (blocked) return;

    if (!chatBusy) {   // look for two villagers standing together within earshot
        if (chatCooldown > 0) return;
        std::vector<std::pair<int, int>> pairs;
        for (int i = 0; i < NPC_COUNT; i++)
            for (int j = i + 1; j < NPC_COUNT; j++) {
                if (!Living(i) || !Living(j) || npcs[i].loc != scene || npcs[j].loc != scene) continue;
                if (npcs[i].fightTarget >= 0 || npcs[j].fightTarget >= 0 || npcs[i].approach || npcs[j].approach) continue;
                if (Vector2Distance({npcs[i].pos.x, npcs[i].pos.z}, {npcs[j].pos.x, npcs[j].pos.z}) > 4.5f) continue;
                if (Vector2Distance({npcs[i].pos.x, npcs[i].pos.z}, {player.pos.x, player.pos.z}) > 16) continue;
                pairs.push_back({i, j});
            }
        if (pairs.empty()) { chatCooldown = 1.5f; return; }
        std::pair<int, int> pr = pairs[GetRandomValue(0, (int)pairs.size() - 1)];
        chatA = pr.first; chatB = pr.second;
        if (GetRandomValue(0, 1)) std::swap(chatA, chatB);
        PlanChat();
        chatLines.clear(); chatIdx = 0; chatPause = 0;
        chatBusy = true; chatStarted = false;
        npcs[chatA].chatT = npcs[chatB].chatT = 999;   // both stand still and face each other until the chat ends
        return;
    }

    FaceEachOther(chatA, chatB);
    if (!chatStarted) {   // ask for a whole exchange once (the browser answers from its cache when it has this situation stored)
        AIReq r;
        r.kind = "chat";
        r.key = "chat|" + std::to_string(chatA) + "|" + std::to_string(chatB) + "|" + chatKey + "|" + PairDynCode(social, chatA, chatB) + PairDynCode(social, chatB, chatA) + "|" +
                npcs[chatA].pid.substr(0, 6) + npcs[chatB].pid.substr(0, 6);
        r.speaker = npcs[chatA].name; r.listener = npcs[chatB].name; r.emotion = chatMood; r.facts = ChatFacts(chatA, chatB); r.keywords = chatKw;
        AIDialogue_Request(r);
        chatStarted = true; chatWait = 35.0f;
        return;
    }
    if (chatLines.empty()) {   // waiting for the exchange to arrive
        chatWait -= dt;
        int state = AIDialogue_State();
        if (state == 0 && chatWait > 0) return;
        if (state == 1) {
            for (const std::string& l : Split(AIDialogue_Result(), '\n')) {
                std::string line = Tidy(l);
                if (Usable(line)) chatLines.push_back(line);
            }
        }
        if (chatLines.size() < 2) EndChat();   // nothing usable: skip this chat (a new one will start later)
        return;
    }
    if (chatPause > 0) { chatPause -= dt; return; }
    if (chatIdx >= (int)chatLines.size()) { EndChat(); return; }
    int speaker = (chatIdx % 2 == 0) ? chatA : chatB;   // the two take turns, starting with chatA
    bubbleText[speaker] = WithTic(chatLines[chatIdx], speaker); bubbleT[speaker] = 6.0f;   // their verbal tic, now and then
   
    chatIdx++; chatPause = 3.5f;
}

void SetLines(const std::string& l1, const std::string& l2) {
    strncpy(curL1, l1.c_str(), sizeof(curL1) - 1); curL1[sizeof(curL1) - 1] = 0;
    strncpy(curL2, l2.c_str(), sizeof(curL2) - 1); curL2[sizeof(curL2) - 1] = 0;
    letters = 0;
}
void SendRequest() { AIDialogue_Request(aiReq); }
// Ask the AI for a conversation (the browser answers instantly from its cache when this situation has been seen). Frame() waits
// for it and asks again if the opening comes back unusable. There is no scripted fallback line.
void Fizzle(const std::string& message, const std::string& l2 = "") {   // the AI couldn't answer: show why, and make sure no (empty) answer menu or follow-up opens
    SetLines(message, l2);
    aiWaiting = false; convHasChoices = pickingTarget = convFollow = false; convTree.clear();
}
std::string AIFailText(int state) {   // a plain-English reason, from the AI client's own error message
    std::string why = state == 0 ? std::string("the AI took too long") : AIDialogue_LastError();
    if (why.empty()) why = "something went wrong";
    if (why.size() > 90) why = why.substr(0, 90);
    return "(They can't think of anything to say: " + why + ". Try again in a moment.)";
}
void ShowLine(const AIReq& r, const std::string& l2) {
    aiReq = r; pendingL2 = l2; aiTries = 0;
    if (AIDialogue_Status() != 2) {   // no API key yet
        Fizzle("(Characters can't talk yet: click 'API key' at the top of the page and paste your Google Gemini key.)", l2);
        return;
    }
    SendRequest();
    aiWaiting = true; aiTimeoutT = 35.0f;   // the AI client retries a rate limit itself, so give it time
    SetLines(".", ""); letters = 3;
}
std::string Subst(std::string s, const std::string& target = "") {   // fill in the placeholders the AI was told to write
    std::string me = AddressFor(talkIdx);
    s = ReplaceAll(ReplaceAll(s, "{player}", me), "{Player}", me);
    return ReplaceAll(ReplaceAll(s, "{target}", target.empty() ? std::string("someone") : target), "{Target}", target.empty() ? std::string("someone") : target);
}
std::string Flavor(const std::string& line, int idx) { return WithTic(line, idx); }   // the speaker's verbal tic, now and then (free: done in code)
std::string TreeLine(int i) {   // one line of this conversation, cleaned and with names filled in ("" if it's missing)
    if (i < 0 || i >= (int)convTree.size()) return "";
    return Flavor(Subst(Tidy(convTree[i])), talkIdx);
}
void RememberStory(const std::string& raw) {   // a chapter the character just told becomes part of their storyline
    NPCInfo& n = npcs[talkIdx];
    n.storyLog.push_back(raw);
    while (n.storyLog.size() > 3) n.storyLog.erase(n.storyLog.begin());
    n.beats++;
}

// What does this villager want to talk about right now? All the odds live here - including the rule that the FIRST conversation
// after a NEW murder is always about it, with whoever you happen to talk to. Sets convEvent/convTopic/convOther/convMemory/convSpecial.
int ChooseContext(int idx) {
    int kev = LatestKill();
    if (kev > killMentioned) { convEvent = kev; return CTX_KILL; }
    if (idx == 0 && Roll(35)) { convSpecial = 1; return CTX_NORMAL; }                  // Yellow occasionally asks what color you're feeling
    if (idx == 1 && !hasName) { convSpecial = 2; return CTX_NORMAL; }                  // Orange asks your name the first time
    if (kev >= 0 && Roll(MurderChance(kev))) { convEvent = kev; return CTX_KILL; }     // a murder keeps coming up
    int ev = PickKnownEvent(idx);
    if (ev >= 0 && Roll(55)) { convEvent = ev; return CTX_GOSSIP; }                    // gossip about each other...
    if (targets.size() >= 2 && Roll(22)) return CTX_RUMOR;                             // ..."who did it?" - YOU decide who gets the blame (or credit)
    std::vector<NRel> rels = NotableRels(idx);
    if (!rels.empty() && Roll(45)) { convOther = rels[GetRandomValue(0, (int)rels.size() - 1)].other; return CTX_LOVE; }   // ...how they FEEL about someone they have strong feelings for
    if (PickMemory(idx, convOther, convMemory) && Roll(40)) return CTX_MEMORY;         // ...something you told them (or that they heard you told someone)
    if (Roll(22)) { convSubject = SUBJECTS[GetRandomValue(0, (int)(sizeof(SUBJECTS) / sizeof(SUBJECTS[0])) - 1)]; return CTX_ASK; }   // ...a personal question: your answer gets remembered
    if (Roll(20)) return CTX_STORY;                                                    // ...their ongoing storyline
    if (Roll(85)) {                                                                    // ...an interest, the world outside, the human world, each other
        convTopic = RandomTopic();
        convOther = convTopic == T_OTHER ? PickOther(idx) : -1;
        if (convTopic == T_OTHER && convOther < 0) convTopic = T_JOB;
        return CTX_TOPIC;
    }
    return CTX_NORMAL;
}

int ChoiceOdds(int ctx) {   // how often a conversation of this kind has you answer ("often times they'll have you respond")
    switch (ctx) {
        case CTX_THANK: case CTX_CONFRONT: case CTX_GOSSIP: case CTX_KILL: return 100;
        case CTX_STORY: return 85;
        case CTX_TOPIC: case CTX_MEMORY: case CTX_LOVE: return 70;
        case CTX_ASK: return 100;
        default: return 0;   // a plain greeting
    }
}

// The request for this conversation: the facts the AI is given about the situation, the words a good opening should mention,
// and the cache key (the same situation later is answered from the cache for free).
// Everything the dice decided about a conversation, so it can be rolled in advance and used later.
struct ConvSel { int ctx = 0, victim = -1, event = -1, special = 0, topic = 0, other = -1, rumorTopic = 0, rumorKind = 0; bool hasChoices = false, pickTarget = false, wantFollow = false; std::string memory, subject; };
ConvSel SaveSel() { return {convCtx, convVictim, convEvent, convSpecial, convTopic, convOther, convRumorTopic, convRumorKind, convHasChoices, pickingTarget, convWantFollow, convMemory, convSubject}; }
void LoadSel(const ConvSel& c) {
    convCtx = c.ctx; convVictim = c.victim; convEvent = c.event; convSpecial = c.special; convTopic = c.topic; convOther = c.other; convRumorTopic = c.rumorTopic; convRumorKind = c.rumorKind;
    convHasChoices = c.hasChoices; pickingTarget = c.pickTarget; convWantFollow = c.wantFollow; convMemory = c.memory; convSubject = c.subject;
}
// All the dice rolls for a conversation: what it is about, whether you get to answer, whether it has a second round.
void ChooseConversation(int idx, int ctx, int victim) {
    convCtx = ctx; convVictim = victim; convEvent = -1; convSpecial = 0; convHasChoices = false;
    targets.clear();                                                   // who you could name in a gossip question
    for (int j = 0; j < NPC_COUNT; j++) if (j != idx && Living(j)) targets.push_back(j);
    if (ctx == CTX_NORMAL) convCtx = ChooseContext(idx);
    if (convCtx == CTX_RUMOR) { convRumorTopic = GetRandomValue(0, RUMOR_COUNT - 1); convRumorKind = RUMORS[convRumorTopic].kind; }
    pickingTarget = convSpecial == 0 && convCtx == CTX_RUMOR && targets.size() >= 2;
    if (convCtx == CTX_RUMOR && !pickingTarget) convCtx = CTX_NORMAL;  // nobody left to blame
    convHasChoices = pickingTarget || (convSpecial == 0 && Roll(ChoiceOdds(convCtx)));
    convWantFollow = convHasChoices && !pickingTarget && convCtx != CTX_THANK && convCtx != CTX_CONFRONT && convCtx != CTX_KILL && Roll(10);   // a second round
}

AIReq BuildTalk(int idx) {
    NPCInfo& n = npcs[idx];
    std::string N = n.name, V = (convVictim >= 0 && convVictim < NPC_COUNT) ? npcs[convVictim].name : "someone";
    std::string facts, key;   // (facts never contain the player's real name: it is written {player} and filled in later, so cached conversations stay valid)
    std::vector<std::string> kw;
    bool fresh = false;
    switch (convCtx) {
        case CTX_THANK:
            facts = " The player just hit " + V + ", whom " + N + " hates, and " + N + " is delighted."; kw = {V, "hit", "thank"}; key = "thank|" + std::to_string(convVictim); break;
        case CTX_CONFRONT:
            facts = convVictim == idx ? " The player just hit " + N + ", and " + N + " is furious." : " The player just hit " + V + ", " + N + "'s friend, and " + N + " is angry.";
            kw = {V, "hit", "hurt", "why"}; key = "confront|" + std::to_string(convVictim); break;
        case CTX_GOSSIP: {
            const SocialEvent& e = social.events[convEvent];
            facts = " " + N + " wants to gossip: " + Sentence(NewsText(e));
            kw.push_back(npcs[e.a].name); if (e.b >= 0) kw.push_back(npcs[e.b].name); if (e.c >= 0) kw.push_back(npcs[e.c].name);
            key = "gossip|" + std::to_string(convEvent); break;
        }
        case CTX_KILL: {
            const SocialEvent& e = social.events[convEvent];
            facts = KillFacts(idx, e); kw = {npcs[e.a].name, npcs[e.b].name, "kill", "dead", "gone", "murder"}; key = "kill|" + std::to_string(convEvent); break;
        }
        case CTX_RUMOR: facts = " " + N + " " + ReplaceAll(RUMORS[convRumorTopic].text, "%N", N) + "."; key = "rumor|" + std::to_string(convRumorTopic); break;
        case CTX_LOVE: {   // a heart-to-heart about someone they have strong feelings for: they describe those feelings
            std::string O = npcs[convOther].name;
            facts = " " + N + " opens up to the player about " + O + ". " + PairLine(idx, convOther) + " " + N + " describes in detail how they feel about " + O + " and why.";
            kw = {O}; key = "love|" + std::to_string(convOther) + "|" + PairDynCode(social, idx, convOther) + PairDynCode(social, convOther, idx); break;
        }
        case CTX_ASK: facts = " " + N + " asks the player about their " + convSubject + " and wants to know."; key = "ask|" + convSubject; break;
        case CTX_MEMORY: facts = MemoryNote(idx, convOther, convMemory); kw = KeyWords(convMemory); key = "mem|" + std::to_string(convOther) + "|" + ShortHash(convMemory); break;
        case CTX_STORY: {   // their ongoing storyline: each visit continues it from where they left off (written fresh, never cached)
            std::string log;
            for (const std::string& l : n.storyLog) log += " " + Sentence(l);
            facts = " " + N + " " + n.premise + ". " + (n.beats == 0 ? N + " is telling the player about this for the first time, setting the scene."
                    : "What " + N + " has already told the player about it:" + log + " " + N + " now tells the next part: something new happens or is revealed, never a repeat.");
            key = "story"; fresh = true; break;
        }
        case CTX_TOPIC:
            facts = TopicFacts(idx, "the player", convTopic, convOther); kw = TopicKeywords(idx, convTopic, convOther);
            key = "topic|" + std::to_string(convTopic) + "|" + std::to_string(convOther) + "|" +
                  (convOther >= 0 ? std::string(1, PairDynCode(social, idx, convOther)) + PairDynCode(social, convOther, idx) : std::string("-")) + "|" + std::to_string(convTopic == T_TIME ? DayBand() : 0);
            break;
        default: key = "greet";
    }
    if (pickingTarget) facts += " " + N + " asks the player who it was.";
    bool full = convCtx != CTX_TOPIC && convCtx != CTX_RUMOR && convCtx != CTX_STORY && convCtx != CTX_MEMORY && convCtx != CTX_LOVE && convCtx != CTX_ASK;   // the others don't need the whole relationship picture (fewer tokens, more cache hits)
    bool needQuestion = pickingTarget || convCtx == CTX_ASK;                                                   // "who was it?" / a personal question must actually be a question
    bool follow = convWantFollow;
    // ONE request writes the whole conversation: the opening line, three answers for you to pick from (when it has choices), a reply to
    // each, a possible memory worth keeping, and optionally a follow-up round. Nothing more is generated mid-conversation.
    AIReq r;
    r.kind = "talk"; r.opts = pickingTarget ? "@target" : convCtx == CTX_ASK ? "@ask" : convHasChoices ? "@gen" : ""; r.follow = follow; r.opts2 = follow ? FOLLOW_OPTS : ""; r.fresh = fresh;
    r.key = key + "|" + ShortHash(r.opts + "/" + r.opts2) + "|" + StateKey(idx, full);   // different answer styles / follow-up => a different stored conversation
    r.speaker = N; r.listener = "{player}"; r.emotion = Emotion(idx, convCtx, convEvent);
    r.facts = Facts(idx, facts, full); r.needQ = needQuestion; r.keywords = Csv(kw);
    return r;
}

// ---------------- conversations prepared ahead of time ----------------
// While you walk up to a villager, their next conversation is rolled and written in the background and stored, so when you press Space it
// appears at once instead of after the AI's few seconds. (A pending murder, story chapters and the colour/name questions are never prepared.)
const bool PREPARE_AHEAD = true;        // set to false to switch this off
const float PREPARE_RANGE = 5.0f;       // how close you must be (in game units) before their conversation is prepared: only the NEAREST villager is
const int PREPARE_MAX = 2;              // at most this many prepared conversations waiting at once (limits money spent on ones you never use)
const float PLAN_LIFETIME = 90.0f;      // an unused prepared conversation is dropped after this many seconds (it would be stale by then)
struct TalkPlan { bool valid = false; float wait = 0, age = 0; ConvSel sel; };
TalkPlan plans[NPC_COUNT];
void ResetPlans() { for (TalkPlan& p : plans) p = TalkPlan(); }
void PrepareConversations(float dt) {
    static float tick = 0;
    int waiting = 0;
    for (TalkPlan& p : plans) {
        if (p.wait > 0) p.wait -= dt;
        if (p.valid && (p.age += dt) > PLAN_LIFETIME) p.valid = false;
        if (p.valid) waiting++;
    }
    tick -= dt; if (tick > 0) return; tick = 0.5f;
    if (!PREPARE_AHEAD || waiting >= PREPARE_MAX || talking || scene == MENU || gameOver || killCineT >= 0 || AIDialogue_Status() != 2 || AIDialogue_Throttled()) return;
    int best = -1; float bestD = PREPARE_RANGE;
    for (int i = 0; i < NPC_COUNT; i++) {   // the nearest villager who could use one
        if (plans[i].valid || plans[i].wait > 0 || !Living(i) || npcs[i].loc != scene || npcs[i].fightTarget >= 0) continue;
        float d = Vector2Distance({npcs[i].pos.x, npcs[i].pos.z}, {player.pos.x, player.pos.z});
        if (d < bestD) { bestD = d; best = i; }
    }
    if (best < 0) return;
    if (LatestKill() > killMentioned) { plans[best].wait = 10.0f; return; }             // a murder must be told first, on demand
    ConvSel saved = SaveSel(); std::vector<int> savedTargets = targets;
    ChooseConversation(best, CTX_NORMAL, -1);
    AIReq r = BuildTalk(best);
    if (convSpecial == 0 && !r.fresh) { plans[best].sel = SaveSel(); plans[best].valid = true; plans[best].age = 0; AIDialogue_Request(r, true); }
    else plans[best].wait = 15.0f;                                                      // nothing to prepare (a question the game asks itself, or a story chapter): look again later
    LoadSel(saved); targets = savedTargets;
}

void StartTalk(int idx, int ctx = CTX_NORMAL, int victim = -1) {
    NPCInfo& n = npcs[idx];
    if (chatBusy) EndChat();                                           // you walked up: the villagers stop chatting with each other
    talking = true; talkIdx = idx; letters = 0;
    colorPicking = namePrompt = choosing = pickingTarget = aiWaiting = false;
    convRound = 0; convFollow = false; convTree.clear();
    n.yaw = atan2f(player.pos.x - n.pos.x, player.pos.z - n.pos.z) * RAD2DEG;
    player.yaw = n.yaw + 180;
    n.approach = 0; n.moving = false; n.fightTarget = -1;
    PlaySfx(SFX_TALK);   // the "hello" chirp when you start talking to someone
    n.timesTalked++;

    // Use the conversation that was prepared in advance for this villager (it is already written, so it appears instantly) - unless a
    // murder has to be told first - otherwise roll a new one now.
    TalkPlan& plan = plans[idx];
    if (ctx == CTX_NORMAL && plan.valid && LatestKill() <= killMentioned) {
        LoadSel(plan.sel);
        targets.clear();
        for (int j = 0; j < NPC_COUNT; j++) if (j != idx && Living(j)) targets.push_back(j);
        if (pickingTarget && targets.size() < 2) { pickingTarget = convHasChoices = convWantFollow = false; convCtx = CTX_NORMAL; }   // everyone you could blame is gone
    } else ChooseConversation(idx, ctx, victim);
    plan.valid = false;
    if (convCtx == CTX_KILL && convEvent >= 0) killMentioned = std::max(killMentioned, convEvent);   // they've told you about this murder now

    // The NPC's personality, memory and mood, plus what you've been doing, feed the tiny trained neural net
    // (npc_brain.h) to decide how this changes their feelings and whether they shove you.
    BrainInput in = {n.curiosity, n.friendliness, n.patience, n.opinion, (float)n.timesTalked, ColorMatchFor(n), aggression, dayT, 0.0f};
    BrainOutput out = RunBrain(in);
    bool punched = GetRandomValue(0, 999) < (int)(out.punchChance * 1000);
    if (punched) { HurtPlayer(10); PlaySfx(SFX_DOOR); convHasChoices = pickingTarget = false; if (convCtx == CTX_RUMOR) convCtx = CTX_NORMAL; }
    if (!convHasChoices) n.opinion = Clamp(n.opinion + out.opinionDelta, -100, 100);   // with choices, your answer decides it instead
    if (convSpecial == 0) {   // how they see you drifts a little every time you talk: trust follows their opinion, and a romantic type can start to fall for you
        n.trust = Clamp(n.trust + (n.opinion * 0.6f - n.trust) * 0.10f, -100, 100);
        if (n.opinion > 30 && n.timesHit == 0) n.romance = Clamp(n.romance + 6.0f * n.romantic, 0, 100);
        else if (n.opinion < 15) n.romance = Clamp(n.romance - 3.0f, 0, 100);
    }
    const char* punchNote = "(They shove you! Ouch.)";

    if (convSpecial == 1) SetLines("What color are you feeling like today?", punched ? punchNote : "(Up/Down to pick, Space to confirm)");
    else if (convSpecial == 2) SetLines("Welcome! What is your name?", punched ? punchNote : "(Type it, then press Enter)");
    else ShowLine(BuildTalk(idx), punched ? punchNote : "");
}

void TakeChoices(const std::vector<std::string>& tree) {   // the three answers the AI wrote; if any is missing or unusable the conversation simply has no answer menu
    if (aiReq.opts != "@gen" && aiReq.opts != "@ask") return;
    std::string tones = tree.size() > TR_TONES ? tree[TR_TONES] : "";
    for (int i = 0; i < 3; i++) {
        choiceText[i] = tree.size() > (size_t)(TR_CHOICE + i) ? tree[TR_CHOICE + i] : "";
        choiceVal[i] = ToneValue(i < (int)tones.size() ? tones[i] : '0');
        if (!Usable(choiceText[i])) { convHasChoices = false; return; }
    }
}

void ShowFollowUp() {   // the second round: they keep going, and you answer again (all of it already written in the same request)
    convRound = 1; convFollow = false;
    std::string raw = convTree.size() > TR_FOLLOW ? Tidy(convTree[TR_FOLLOW]) : "";
    SetLines(Flavor(Subst(raw), talkIdx), "");
    for (int i = 0; i < 3; i++) { choiceText[i] = FOLLOW_TEXT[i]; choiceVal[i] = ToneValue("+0-"[i]); }
    convHasChoices = true;
    if (convCtx == CTX_STORY) RememberStory(raw);
}

void ApplyChoice(int sel) {   // you picked an answer: it changes how they feel, they may remember it, and their (already written) reply appears
    NPCInfo& n = npcs[talkIdx];
    float value = choiceVal[sel];
    BrainInput in = {n.curiosity, n.friendliness, n.patience, n.opinion, (float)n.timesTalked, ColorMatchFor(n), aggression, dayT, value};
    BrainOutput out = RunBrain(in);
    n.opinion = Clamp(n.opinion + out.opinionDelta, -100, 100);
    n.trust = Clamp(n.trust + value * 8.0f, -100, 100);                                   // a kind answer builds trust, a rude one costs it
    if (value > 0.5f) n.romance = Clamp(n.romance + 7.0f * n.romantic, 0, 100); else if (value < 0) n.romance = Clamp(n.romance - 6.0f, 0, 100);
    bool punched = value < 0 && GetRandomValue(0, 999) < (int)(out.punchChance * 1000);   // only a rude answer earns a shove
    if (punched) { HurtPlayer(10); PlaySfx(SFX_DOOR); }
    choosing = false; convHasChoices = false;
    std::string line = TreeLine((convRound == 0 ? TR_REPLY : TR_FREPLY) + sel);
    if (!Usable(line)) line = "(They think about that for a moment.)";
    SetLines(line, punched ? "(They shove you! Ouch.)" : "");
    convFollow = convRound == 0 && Usable(TreeLine(TR_FOLLOW));                      // a follow-up round was written along with the conversation
    if (convRound == 0 && convTree.size() > TR_FACT && !convTree[TR_FACT].empty())         // an answer that says something about you is remembered...
        Remember(n, ReplaceAll(convTree[TR_FACT], "{a}", choiceText[sel]));               // ...and they (and their friends) may bring it up later
}

void ApplyRumorChoice(int target) {   // you named someone: that changes how the asker feels about them - for good
    NPCInfo& n = npcs[talkIdx];
    std::string N = n.name, T = npcs[target].name;
    SocialRumor(social, talkIdx, target, convRumorKind);
    n.opinion = Clamp(n.opinion + 6, -100, 100);                                     // you were helpful
    n.trust = Clamp(n.trust + 5, -100, 100);
    if (convRumorKind == 0) AddNews(N + " is furious with " + T, {255, 150, 130, 255});
    else if (convRumorKind == 1) AddNews(N + " warmed up to " + T, {150, 255, 170, 255});
    else AddNews(N + " can't stop thinking about " + T, {255, 170, 215, 255});
    choosing = pickingTarget = false; convHasChoices = false;
    std::string line = Flavor(Subst(convTree.size() > TR_REPLY ? Tidy(convTree[TR_REPLY]) : "", T), talkIdx);   // "{target}" becomes the name you picked
    SetLines(Usable(line) ? line : "(They take that in.)", "");
}

// ---------------- scenes ----------------
void Go(Scene s) { nextScene = s; fadeDir = 1; }

void Enter(Scene s) {
    if (s == OUTSIDE) player.pos = (scene == ROOM1) ? Vector3{0, 0, -6.5f} : Vector3{0, 0, 4};
    if (s == ROOM1) player.pos = (scene == ROOM2) ? Vector3{8, 0, 0} : Vector3{0, 0, -7};
    if (s == ROOM2) player.pos = {-8, 0, 0};
    player.yaw = (s == OUTSIDE && scene == MENU) ? 180 : 0;
    player.velY = 0;
    player.moving = false;
    scene = s;
    PlaySfx(SFX_DOOR);
}

bool Blocked(Vector3 p) {
    if (WallBlocked(scene, p)) return true;
    for (NPCInfo& n : npcs) if (n.loc == scene && !n.gone && Vector2Distance({p.x, p.z}, {n.pos.x, n.pos.z}) < 1) return true;
    return false;
}

void Move(float dt) {
    Vector3 d = {(float)((IsKeyDown(KEY_D) || IsKeyDown(KEY_RIGHT)) - (IsKeyDown(KEY_A) || IsKeyDown(KEY_LEFT))), 0,
                 (float)((IsKeyDown(KEY_S) || IsKeyDown(KEY_DOWN)) - (IsKeyDown(KEY_W) || IsKeyDown(KEY_UP)))};
    player.moving = d.x != 0 || d.z != 0;
    if (player.moving) {
        d = Vector3Scale(Vector3Normalize(d), walkSpeed * dt);
        player.yaw = atan2f(d.x, d.z) * RAD2DEG;
        player.walk += dt;
        Vector3 old = player.pos;    // move one axis at a time so you slide along walls
        player.pos.x += d.x; if (Blocked(player.pos)) player.pos.x = old.x;
        player.pos.z += d.z; if (Blocked(player.pos)) player.pos.z = old.z;
        footTimer -= dt;
        if (footTimer <= 0 && player.pos.y == 0) { PlaySfx(SFX_FOOTSTEP); footTimer = 0.32f; }
    } else player.walk = 0;

    player.velY -= 25 * dt;          // gravity
    player.pos.y += player.velY * dt;
    if (player.pos.y < 0) player.pos.y = player.velY = 0;
}

void UpdateCamera(float dt) {   // mouse-drag / scroll on desktop, Q E Z X as a keyboard + touch-button alternative
    camAngle += (IsKeyDown(KEY_E) - IsKeyDown(KEY_Q)) * 90 * dt;
    camZoom += (IsKeyDown(KEY_X) - IsKeyDown(KEY_Z)) * 0.6f * dt;
    if (IsMouseButtonDown(MOUSE_BUTTON_LEFT)) camAngle += GetMouseDelta().x * 0.3f;
    camZoom -= GetMouseWheelMove() * 0.1f;
    camZoom = Clamp(camZoom, 0.5f, 1.8f);
}

Camera3D MakeCamera(Vector3 target, float dist, float height) {
    dist *= camZoom;
    Vector3 pos = {target.x + sinf(camAngle * DEG2RAD) * dist, target.y + height, target.z + cosf(camAngle * DEG2RAD) * dist};
    return {pos, target, {0, 1, 0}, 45, CAMERA_PERSPECTIVE};
}

// ---------------- the stick, flowers, and the living world ----------------
Vector3 RandomOutsidePos() {
    Vector3 pos;
    do { pos = {(float)GetRandomValue(-45, 45), 0, (float)GetRandomValue(-45, 45)}; }
    while (fabsf(pos.x) < 5 && pos.z > -17 && pos.z < -6);   // keep flowers off the building footprint
    return pos;
}

void NewGame() {   // wipe the world: everyone is alive again, relationships are re-rolled, and you pick a soulmate again
    for (int i = 0; i < NPC_COUNT; i++) npcs[i] = NPCInfo();
    InitNPCs();
    LoadConfig();
    SocialInit(social, NPC_COUNT, (unsigned)GetRandomValue(1, 1000000));
    for (Flower& f : flowers) f = Flower{{0, 0, 0}, WHITE, 0, 1, false, 0};
    for (int i = 0; i < START_FLOWERS; i++) flowers[i] = {RandomOutsidePos(), GetRandomValue(0, 1) ? flowerRed : flowerPink, 30, 1, false, 0};
    news.clear(); newsSeen = 0;
    playerHP = 100; playerHitT = 0; hasName = false; strcpy(playerName, "???");
    stick = {{2.5f, 0, -4.0f}, false, false, 0};
    player = Person{};
    aggression = 0; killCineT = -1; saveMsgT = 0; killMentioned = -1;
    ResetPlans();                                  // (conversations prepared in advance belong to the old world)
    soulmate = -1; gameOver = 0; pickingSoulmate = false; soulSel = 0;
    pred = Predictions(); guessing = needGuess = false;
    talking = colorPicking = namePrompt = choosing = pickingTarget = aiWaiting = false;
    chatBusy = chatStarted = false; chatA = chatB = -1; chatCooldown = 6; for (int i = 0; i < 8; i++) bubbleT[i] = 0;
    fade = 0; fadeDir = 0; scene = MENU;
    menuSel = hasSaveFile ? 1 : 0; menuMsgT = 0;
}

void HitNPC(int v) {   // you whacked someone with the stick
    NPCInfo& victim = npcs[v];
    if (chatBusy && (chatA == v || chatB == v)) EndChat();   // a hit interrupts their chat
    victim.hitT = 1.0f; victim.timesHit++;
    victim.trust = Clamp(victim.trust - 30, -100, 100); victim.romance = 0;   // being hit wrecks trust and ends any crush
    Vector2 away = Vector2Normalize({victim.pos.x - player.pos.x, victim.pos.z - player.pos.z});
    Vector3 pushed = {victim.pos.x + away.x * 0.8f, 0, victim.pos.z + away.y * 0.8f};
    if (!WallBlocked(victim.loc, pushed)) victim.pos = pushed;

    bool witness[MAX_NPCS] = {};
    for (int i = 0; i < NPC_COUNT; i++) witness[i] = npcs[i].loc == scene;
    HitResult r = SocialHit(social, v, witness);   // the victim's friends get angry, their enemies get happy
    AddNews(std::string("You hit ") + victim.name + "!", {255, 130, 130, 255});
    for (int i = 0; i < NPC_COUNT; i++) {
        NPCInfo& m = npcs[i];
        if (!Living(i)) continue;
        m.opinion = Clamp(m.opinion + r.opinionDelta[i], -100, 100);
        if (r.reaction[i] == REACT_NONE) continue;
        m.approach = r.reaction[i] == REACT_THANK ? 1 : 2;   // they'll come find you to say so
        m.approachT = 45; m.approachVictim = v;
        if (i != v && witness[i])
            AddNews(std::string(m.name) + (m.approach == 1 ? " is glad you hit " : " is angry you hit ") + victim.name,
                    m.approach == 1 ? Color{150, 255, 170, 255} : Color{255, 150, 130, 255});
    }
}

void SwingStick() {   // its own key, so you can still jump while holding it - works inside and out
    stick.swingT = 0.25f;
    aggression = fminf(1.0f, aggression + 0.4f);
    if (scene == OUTSIDE)
        for (Flower& f : flowers)
            if (f.hp > 0 && !f.dying && Vector3Distance(player.pos, f.pos) < 2.0f) f.hp -= 15;
    int best = -1; float bd = 2.2f;
    for (int i = 0; i < NPC_COUNT; i++) {
        if (npcs[i].loc != scene || !Living(i)) continue;
        float d = Vector2Distance({player.pos.x, player.pos.z}, {npcs[i].pos.x, npcs[i].pos.z});
        if (d < bd) { bd = d; best = i; }
    }
    if (best >= 0) HitNPC(best);
}

void KillFlowersOnDeath() {   // dying (not vanishing outright) reuses the same shrink-and-purple animation
    int alive = 0;
    for (Flower& f : flowers) if (f.hp > 0 && !f.dying) alive++;
    int toKill = std::max(5, alive * 2 / 5);   // a big chunk of your garden
    for (Flower& f : flowers) {
        if (toKill <= 0) break;
        if (f.hp > 0 && !f.dying) { f.dying = true; f.dyingT = 0; toKill--; }
    }
}

void UpdateGardening(float dt) {   // happy NPCs plant flowers (they grow in), furious ones kill them - every ~1 second
    gardenTimer -= dt;
    if (gardenTimer > 0) return;
    gardenTimer = 1.2f;
    for (int i = 0; i < NPC_COUNT; i++) {
        NPCInfo& n = npcs[i];
        if (!Living(i)) continue;
        if (n.opinion > 25 && GetRandomValue(0, 99) < 70) {
            for (Flower& f : flowers) if (f.hp <= 0 && !f.dying) {
                Vector3 pos = RandomOutsidePos();
                if (n.loc == OUTSIDE) {   // outdoors they plant right around themselves
                    Vector3 q = {n.pos.x + GetRandomValue(-30, 30) / 10.0f, 0, n.pos.z + GetRandomValue(-30, 30) / 10.0f};
                    if (!WallBlocked(OUTSIDE, q)) pos = q;
                }
                f = {pos, GetRandomValue(0, 1) ? flowerRed : flowerPink, 30, 0, false, 0};
                break;
            }
        }
        if (n.opinion < -25 && GetRandomValue(0, 99) < 70) {
            int kills = n.opinion < -60 ? 2 : 1;
            for (Flower& f : flowers) if (kills > 0 && f.hp > 0 && !f.dying) { f.dying = true; f.dyingT = 0; kills--; }
        }
    }
}

SocialView MakeView() {
    SocialView v = {};
    for (int i = 0; i < NPC_COUNT; i++) { v.loc[i] = (int)npcs[i].loc; v.x[i] = npcs[i].pos.x; v.z[i] = npcs[i].pos.z; v.patience[i] = npcs[i].patience; }
    return v;
}

void UpdateNPCs(float dt, bool busy) {   // wandering, walking between rooms, coming to find you, and fighting
    for (int i = 0; i < NPC_COUNT; i++) {
        NPCInfo& n = npcs[i];
        if (n.gone) continue;
        if (n.hitT > 0) n.hitT -= dt;
        if (n.swingT > 0) n.swingT -= dt;
        if (n.knifeT > 0) n.knifeT -= dt;
        if (n.fightCooldown > 0) n.fightCooldown -= dt;
        if (n.dying) {   // a killed NPC stands perfectly still while fading out, then is gone for good
            n.deathT -= dt; n.moving = false;
            if (n.deathT <= 0) { n.dying = false; n.gone = true; }
            continue;
        }
        if (n.frozenT > 0) { n.frozenT -= dt; n.moving = false; continue; }              // the victim of a killing stands perfectly still
        if (n.windT > 0) {                                                               // the killer's wind-up: knife out, the screen dark...
            n.windT -= dt; n.moving = false;
            if (n.windT <= 0) {                                                          // ...and the knife comes down
                int v = n.windVictim; n.windVictim = -1;
                if (v >= 0 && social.alive[v] && !npcs[v].gone) {
                    NPCInfo& t = npcs[v];
                    n.swingT = 0.25f;                                                    // the same swing animation as the stick
                    SocialKill(social, i, v, timer);                                     // permanent: dead for the rest of the game
                    t.dying = true; t.deathT = 3.0f; t.hitT = 1.0f; t.moving = false; t.frozenT = 0;   // red, then fades away, standing still
                    if (n.loc == scene) PlaySfx(SFX_DOOR);
                }
            }
            continue;
        }
        if (n.chatT > 0) { n.moving = false; continue; }                                  // chatting with another villager: stands and talks
        if ((talking && talkIdx == i) || n.swingT > 0) { n.moving = false; continue; }   // mid-conversation or mid-swing: planted

        Vector3 dest = n.pos; bool go = false;
        if (n.approach) {
            n.approachT -= dt;
            if (n.approachT <= 0) n.approach = 0;
            else {
                n.goalScene = scene;   // head to wherever the player is
                if (n.loc == scene) {
                    dest = player.pos; go = true;
                    if (!busy && Vector2Distance({n.pos.x, n.pos.z}, {player.pos.x, player.pos.z}) < 2.2f) {
                        StartTalk(i, n.approach == 1 ? CTX_THANK : CTX_CONFRONT, n.approachVictim);
                        continue;
                    }
                }
            }
        }
        if (!go && n.fightTarget >= 0) {   // out to hurt another NPC
            NPCInfo& t = npcs[n.fightTarget];
            n.fightT -= dt;
            if (!Living(n.fightTarget) || t.loc != n.loc || n.fightT <= 0 || (talking && talkIdx == n.fightTarget)) n.fightTarget = -1;
            else {
                dest = t.pos; go = true;
                if (Vector2Distance({n.pos.x, n.pos.z}, {t.pos.x, t.pos.z}) < 1.5f) {
                    int v = n.fightTarget;
                    n.fightTarget = -1; n.fightCooldown = ATTACK_COOLDOWN;
                    n.yaw = atan2f(t.pos.x - n.pos.x, t.pos.z - n.pos.z) * RAD2DEG;
                    t.yaw = atan2f(n.pos.x - t.pos.x, n.pos.z - t.pos.z) * RAD2DEG;
                    if (SocialWillKill(social, i, v)) {      // decided NOW, so the screen can go dark a full second before the knife comes down
                        n.windT = 1.0f; n.windVictim = v; n.knifeT = 3.0f; n.moving = false;
                        t.frozenT = 1.3f; t.moving = false;
                        if (n.loc == scene) killCineT = 0;
                    } else {
                        SocialPunch(social, MakeView(), i, v, timer);
                        n.swingT = 0.25f;
                        t.hitT = 1.0f;
                        Vector2 away = Vector2Normalize({t.pos.x - n.pos.x, t.pos.z - n.pos.z});
                        Vector3 pushed = {t.pos.x + away.x * 0.7f, 0, t.pos.z + away.y * 0.7f};
                        if (!WallBlocked(t.loc, pushed)) t.pos = pushed;
                        if (n.loc == scene) PlaySfx(SFX_DOOR);
                    }
                    continue;
                }
            }
        }
        if (!go) {
            if (n.loc != n.goalScene) { dest = DoorPos(n.loc, NextHop(n.loc, n.goalScene)); go = true; }
            else if (n.idleT > 0) { n.idleT -= dt; n.moving = false; }
            else if (Vector2Distance({n.pos.x, n.pos.z}, {n.target.x, n.target.z}) < 0.6f) {   // arrived: rest, then pick somewhere new
                n.idleT = 1.5f + GetRandomValue(0, 30) / 10.0f;
                n.target = RandomPointIn(n.loc);
                if (GetRandomValue(0, 99) < 20) n.goalScene = (Scene)GetRandomValue(OUTSIDE, ROOM2);   // sometimes change rooms or go outside
            } else { dest = n.target; go = true; }
        }
        if (go) {
            StepNPC(i, dest, dt);
            if (n.loc != n.goalScene) {   // at the door? walk through it
                Scene hop = NextHop(n.loc, n.goalScene);
                Vector3 door = DoorPos(n.loc, hop);
                if (Vector2Distance({n.pos.x, n.pos.z}, {door.x, door.z}) < 1.0f) {
                    if (n.loc == scene || hop == scene) PlaySfx(SFX_DOOR);
                    n.pos = ArrivePos(n.loc, hop); n.loc = hop; n.target = n.pos; n.idleT = 0.3f;
                }
            }
        } else n.moving = false;
        if (n.stuckT > 0.8f) {   // boxed in by a wall or another NPC: sidestep and choose a new spot
            n.stuckT = 0; n.target = RandomPointIn(n.loc);
            Vector3 q = {n.pos.x + GetRandomValue(-10, 10) / 10.0f, 0, n.pos.z + GetRandomValue(-10, 10) / 10.0f};
            if (!WallBlocked(n.loc, q) && !NearOtherNPC(i, q)) n.pos = q;
        }
    }
}

void UpdateSocial(float dt) {   // once a second: crushes, couples, affairs, gossip, and who is angry enough to start a fight
    socialTimer -= dt;
    if (socialTimer > 0) return;
    socialTimer = 1.0f;
    SocialView v = MakeView();
    SocialTick(social, v, timer);
    float op[MAX_NPCS] = {};
    for (int i = 0; i < NPC_COUNT; i++) op[i] = npcs[i].opinion;
    GossipOpinions(social, op);
    for (int i = 0; i < NPC_COUNT; i++) npcs[i].opinion = op[i];
    for (int i = 0; i < NPC_COUNT; i++) {   // angry NPCs pick a target and go after them
        NPCInfo& n = npcs[i];
        if (!Living(i) || n.fightTarget >= 0 || n.fightCooldown > 0 || n.approach || n.chatT > 0 || (talking && talkIdx == i)) continue;
        int t = SocialPickTarget(social, v, i);
        if (t >= 0 && Living(t) && npcs[t].chatT <= 0 && !(talking && talkIdx == t)) { n.fightTarget = t; n.fightT = 15; }
    }
    for (; newsSeen < social.events.size(); newsSeen++)
        AddNews(NewsText(social.events[newsSeen]), NewsColor(social.events[newsSeen].type));
}

// How dark the screen is during a killing: it goes suddenly, insanely dark a second BEFORE the knife comes down
// (the strike lands at t = 1.0) and the lights return about a second AFTER the kill.
float KillDark() {
    if (killCineT < 0) return 0;
    float t = killCineT;
    if (t < 0.25f) return 0.80f * t / 0.25f;
    if (t < 1.75f) return 0.80f;
    if (t < 2.15f) return 0.80f * (1.0f - (t - 1.75f) / 0.4f);
    return 0;
}

// ---------------- one frame ----------------
// ---- one frame of the title screen: settings, the soulmate picker, and the New Game / Load Saved Game / Settings menu ----
void UpdateMenu(float dt) {
    if (inSettings) {
        if (rebinding) {
            int k = GetKeyPressed();
            if (k != 0) { if (rebindWhich == 4) keyInteract = k; else keySwing = k; rebinding = false; }
        } else {
            if (IsKeyPressed(KEY_UP)) settingsSel = (settingsSel + 6) % 7;
            if (IsKeyPressed(KEY_DOWN)) settingsSel = (settingsSel + 1) % 7;
            int dir = (IsKeyPressed(KEY_RIGHT) ? 1 : 0) - (IsKeyPressed(KEY_LEFT) ? 1 : 0);
            if (dir && settingsSel == 0) { sfxVolume = Clamp(sfxVolume + dir * 0.1f, 0, 1); SetVolume(sfxVolume); }
            else if (dir && settingsSel == 1) quality = (quality + dir + 3) % 3;
            else if (dir && settingsSel == 2) dayNightOn = !dayNightOn;
            else if (dir && settingsSel == 3) flowerDamageOn = !flowerDamageOn;
            else if ((settingsSel == 4 || settingsSel == 5) && IsKeyPressed(KEY_ENTER)) { rebinding = true; rebindWhich = settingsSel; }
            else if (dir && settingsSel == 6) showNews = !showNews;
            if (IsKeyPressed(KEY_ESCAPE) || IsKeyPressed(KEY_BACKSPACE)) inSettings = false;
        }
    } else if (pickingSoulmate) {   // "Who's your soulmate?" - pick the one you must keep alive
        if (IsKeyPressed(KEY_UP) || IsKeyPressed(KEY_W)) soulSel = (soulSel + NPC_COUNT - 1) % NPC_COUNT;
        if (IsKeyPressed(KEY_DOWN) || IsKeyPressed(KEY_S)) soulSel = (soulSel + 1) % NPC_COUNT;
        if (IsKeyPressed(KEY_SPACE) || IsKeyPressed(KEY_ENTER)) { soulmate = soulSel; pickingSoulmate = false; needGuess = true; Go(OUTSIDE); }   // (and you make your first guess: who dies first?)
        else if (IsKeyPressed(KEY_ESCAPE) || IsKeyPressed(KEY_BACKSPACE)) pickingSoulmate = false;
    } else {   // the title menu
        if (menuMsgT > 0) menuMsgT -= dt;
        auto enabled = [&](int i) { return i != 1 || hasSaveFile; };
        if (!enabled(menuSel)) menuSel = 0;
        if (IsKeyPressed(KEY_UP)) { do menuSel = (menuSel + MENU_ROWS - 1) % MENU_ROWS; while (!enabled(menuSel)); }   // greyed-out "Load" is skipped
        if (IsKeyPressed(KEY_DOWN)) { do menuSel = (menuSel + 1) % MENU_ROWS; while (!enabled(menuSel)); }
        int choice = -1;
        Vector2 mouse = GetMousePosition();                                                          // the mouse works too: hover to select, click to choose
        for (int i = 0; i < MENU_ROWS; i++)
            if (enabled(i) && CheckCollisionPointRec(mouse, MenuRowRect(i))) {
                if (Vector2Length(GetMouseDelta()) > 0) menuSel = i;
                if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) choice = i;
            }
        if (choice < 0) {
            if (IsKeyPressed(KEY_SPACE) || IsKeyPressed(KEY_ENTER)) choice = menuSel;
            else if (IsKeyPressed(KEY_N)) choice = 0;
            else if (hasSaveFile && IsKeyPressed(KEY_C)) choice = 1;
            else if (IsKeyPressed(KEY_S)) choice = 2;
            else if (IsKeyPressed(KEY_K)) choice = 3;
        }
        if (choice == 0) { menuSel = 0; pickingSoulmate = true; }                                  // New Game: pick your soulmate, then start
        else if (choice == 1) {
            menuSel = 1;
            if (!LoadGame()) { menuMsg = "Couldn't load that save (older version?)"; menuMsgColor = {255, 140, 120, 255}; menuMsgT = 4; }
        }
        else if (choice == 2) { menuSel = 2; inSettings = true; }
        else if (choice == 3) {                                                                      // a link: opens the page where you get a free key
            menuSel = 3; OpenURL(AI_KEY_URL);
            menuMsg = "Opened in a new tab. Copy your key, then click \"API key\"."; menuMsgColor = {170, 255, 190, 255}; menuMsgT = 8;
        }
    }
}

// ---- one frame of a conversation: the color picker, the name prompt, your answer choices, waiting for the AI, and the typewriter text ----
void UpdateTalking(float dt, bool interact) {
    if (colorPicking) {
        if (IsKeyPressed(KEY_UP) || IsKeyPressed(KEY_W)) colorSel = (colorSel + 4) % 5;
        if (IsKeyPressed(KEY_DOWN) || IsKeyPressed(KEY_S)) colorSel = (colorSel + 1) % 5;
        if (interact) { playerColor = colorValues[colorSel]; colorPicking = false; talking = false; }
    } else if (namePrompt) {
        int c;
        while ((c = GetCharPressed()) != 0) {
            size_t len = strlen(nameBuf);
            if (c != ',' && c >= 32 && c < 127 && len < sizeof(nameBuf) - 1) { nameBuf[len] = (char)c; nameBuf[len + 1] = 0; }
        }
        if (IsKeyPressed(KEY_BACKSPACE) && strlen(nameBuf) > 0) nameBuf[strlen(nameBuf) - 1] = 0;
        if (IsKeyPressed(KEY_ENTER)) {
            strncpy(playerName, strlen(nameBuf) ? nameBuf : "???", sizeof(playerName) - 1);
            hasName = true; namePrompt = false; talking = false;
        }
    } else if (choosing) {   // pick what to say back
        int count = pickingTarget ? (int)targets.size() : 3;
        int &sel = pickingTarget ? targetSel : choiceSel;
        if (IsKeyPressed(KEY_UP) || IsKeyPressed(KEY_W)) sel = (sel + count - 1) % count;
        if (IsKeyPressed(KEY_DOWN) || IsKeyPressed(KEY_S)) sel = (sel + 1) % count;
        if (interact) { if (pickingTarget) ApplyRumorChoice(targets[targetSel]); else ApplyChoice(choiceSel); }
    } else if (aiWaiting) {  // waiting for the AI: instant if this situation is already stored, otherwise about a second. Esc cancels.
        if (IsKeyPressed(KEY_ESCAPE)) { aiWaiting = false; talking = false; }
        else {
            aiTimeoutT -= dt;
            if (aiTimeoutT < 30.0f) snprintf(curL1, sizeof(curL1), "... (the AI is taking a while)");   // after a few seconds, say so
            else snprintf(curL1, sizeof(curL1), "%.*s", 1 + (int)(timer * 3) % 3, "...");
            letters = 1000;   // the dots show at once - no typewriter
            int state = AIDialogue_State();
            if (state != 0 || aiTimeoutT <= 0) {
                std::vector<std::string> tree = state == 1 ? Split(AIDialogue_Result(), '\n') : std::vector<std::string>();
                std::string raw = tree.empty() ? std::string() : Tidy(tree[0]);
                std::string line = Subst(raw);                                   // {player} -> the name you gave, or your color
                if (AIDialogue_Status() != 2) Fizzle("(Characters can't talk: click 'API key' at the top of the page and paste your Google Gemini key.)", pendingL2);
                else if (state == 1 && Usable(line)) {
                    TakeChoices(tree);                                           // (no usable answers: the conversation just has no menu)
                    convTree = tree;                                             // the whole conversation arrived: replies and follow-up are ready
                    SetLines(Flavor(line, talkIdx), pendingL2); aiWaiting = false;
                    if (convCtx == CTX_STORY) RememberStory(raw);
                }
                else if (state == 1 && ++aiTries < 2) { AIDialogue_Reject(); SendRequest(); aiTimeoutT = 35.0f; }   // an unusable answer: forget that stored version and ask again
                else Fizzle(AIFailText(state), pendingL2);   // it failed (the AI client already retried) or took too long: say why
            }
        }
    } else {
        letters += dt * 40;
        int total = (int)(strlen(curL1) + strlen(curL2));
        if (interact) {
            if (letters < total) letters = (float)total;
            else if (convSpecial == 1) colorPicking = true;
            else if (convSpecial == 2) { namePrompt = true; nameBuf[0] = 0; }
            else if (convHasChoices) { choosing = true; choiceSel = 0; targetSel = 0; choiceHeader = curL1; }
            else if (convFollow) ShowFollowUp();                  // they have more to say
            else talking = false;
        }
    }
}

void Frame() {
    float dt = fminf(GetFrameTime(), 0.05f);
    timer += dt;
    if (stick.swingT > 0) stick.swingT -= dt;
    if (saveMsgT > 0) saveMsgT -= dt;
    if (playerHitT > 0) playerHitT -= dt;
    if (killCineT >= 0) { killCineT += dt; if (killCineT > 2.3f) killCineT = -1; }
    if (fadeDir) {
        fade += fadeDir * dt * 2.5f;
        if (fade >= 1) { fade = 1; fadeDir = -1; Enter(nextScene); }
        if (fade <= 0) { fade = 0; fadeDir = 0; }
    }
    UpdateAudio();
    UpdateCamera(dt);

    for (Flower& f : flowers) {   // grow-in / shrink-out animation ticks, always running
        if (f.hp <= 0 && !f.dying) continue;
        if (f.anim < 1) f.anim = fminf(1, f.anim + dt);
        if (f.dying) { f.dyingT += dt; if (f.dyingT >= 1) { f.hp = 0; f.dying = false; f.anim = 1; } }
    }
    for (NewsItem& it : news) it.age += dt;
    while (!news.empty() && news.front().age > 14) news.erase(news.begin());
    if (scene != MENU && !gameOver && !guessing) {
        UpdateGardening(dt);
        UpdateNPCs(dt, fadeDir != 0 || talking || inSettings);
        UpdateSocial(dt);
        UpdateChatter(dt);
        PrepareConversations(dt);
    }
    if (scene != MENU) ScanDeaths();   // (even on the frame that ends the game: that last death counts too)
    aggression = fmaxf(0.0f, aggression - dt * 0.03f);   // slowly cools off if you stop being aggressive
    dayT = dayNightOn ? (sinf((timer / 60.0f) * 2 * PI - PI / 2) + 1) / 2 : 1.0f;   // used for drawing AND as a brain input

    bool interact = fadeDir == 0 && !inSettings && IsKeyPressed(keyInteract);
    bool swing = fadeDir == 0 && !inSettings && !talking && IsKeyPressed(keySwing);
    bool ground = player.pos.y == 0;
    const char* prompt = nullptr;
    Vector3 p = player.pos;

    if (!gameOver && scene != MENU && !talking && !guessing && fadeDir == 0 && !inSettings && IsKeyPressed(KEY_G)) needGuess = true;   // G: make or change your guess
    if (needGuess && !guessing && !gameOver && scene != MENU && !talking && fadeDir == 0 && killCineT < 0 && !inSettings && !AnyoneDying()) OpenGuess();
    if (gameOver) {
        if (IsKeyPressed(KEY_R) || interact) NewGame();   // start over from the menu
    } else if (scene == MENU) {
        UpdateMenu(dt);
    } else if (guessing) {
        UpdateGuess();
    } else if (talking) {
        UpdateTalking(dt, interact);
    } else if (fadeDir == 0) {
        Move(dt);
        p = player.pos;

        auto talkNearest = [&]() {   // the closest NPC in reach, in whatever room (or outside) you're in
            int best = -1; float bd = 2.0f;
            for (int i = 0; i < NPC_COUNT; i++) {
                if (npcs[i].loc != scene || !Living(i)) continue;
                float d = Vector2Distance({p.x, p.z}, {npcs[i].pos.x, npcs[i].pos.z});
                if (d < bd) { bd = d; best = i; }
            }
            if (best < 0 || !ground) return;
            prompt = "Press space to talk";
            if (interact) { StartTalk(best); interact = false; }
        };

        if (scene == OUTSIDE) {
            if (ground && Vector3Distance(p, {0, 0, -8.4f}) < 1.5f) {
                prompt = "Press space to enter";
                if (interact) { Go(ROOM1); interact = false; }
            } else if (!stick.pickedUp && Vector3Distance(p, stick.pos) < 1.3f) {
                prompt = "Press space to pick up stick";
                if (interact) {
                    stick.pickedUp = stick.held = true; interact = false;
                    AddNews(std::string("You picked up the stick! Press ") + KeyLabel(keySwing) + " to swing it.", {255, 230, 150, 255});
                }
            } else talkNearest();
        } else if (scene == ROOM1 && ground && Vector3Distance(p, {0, 0, -9}) < 1.5f) {
            prompt = "Press space to exit";
            if (interact) { Go(OUTSIDE); interact = false; }
        } else if (scene == ROOM1 && ground && Vector3Distance(p, {9, 0, 0}) < 1.5f) {
            prompt = "Press space to go deeper";
            if (interact) { Go(ROOM2); interact = false; }
        } else if (scene == ROOM2 && ground && Vector3Distance(p, {-9, 0, 0}) < 1.5f) {
            prompt = "Press space to go back";
            if (interact) { Go(ROOM1); interact = false; }
        } else talkNearest();

        if (interact && ground) { player.velY = jumpVel; PlaySfx(SFX_JUMP); }   // jumping works the same with or without the stick
        if (swing && stick.held) SwingStick();

        if (scene == OUTSIDE && flowerDamageOn) {   // stepping on a flower stings a little
            hurtTimer -= dt;
            bool touching = false;
            for (Flower& f : flowers) if (f.hp > 0 && !f.dying && Vector3Distance(p, f.pos) < 0.45f) touching = true;
            if (touching && hurtTimer <= 0) { HurtPlayer(6); hurtTimer = 0.4f; }
        }
    }

    if (!gameOver && soulmate >= 0 && scene != MENU) {   // the goal: you and your soulmate, the last two alive
        if (npcs[soulmate].gone) gameOver = 1;
        else if (social.alive[soulmate] && killCineT < 0) {
            int others = 0;
            for (int i = 0; i < NPC_COUNT; i++) if (i != soulmate && social.alive[i]) others++;
            if (others == 0) gameOver = 2;
        }
        if (gameOver) { talking = colorPicking = namePrompt = choosing = pickingTarget = aiWaiting = false; guessing = needGuess = false; }   // (the game is over: no more guessing)
    }

    if (scene != MENU && !namePrompt && !gameOver && IsKeyPressed(KEY_P)) SaveGame();   // "Press P to save your file" is shown on the HUD

    if (playerHP <= 0 && fadeDir == 0) {   // death (from flowers or an angry NPC's punch) respawns you inside
        playerHP = 100;
        KillFlowersOnDeath();
        talking = colorPicking = namePrompt = choosing = pickingTarget = aiWaiting = false;
        Go(ROOM1);
    }

    // day/night sky colors (dayT was already computed earlier this frame)
    Color skyTop = ColorLerp({30, 30, 70, 255}, {70, 130, 230, 255}, dayT);
    Color skyBot = ColorLerp({60, 40, 90, 255}, {255, 170, 210, 255}, dayT);

    clockTimer -= dt;    // real-world date/time, refreshed once a second
    if (clockTimer <= 0) {
        clockTimer = 1.0f;
        time_t now = time(nullptr);
        strftime(clockStr, sizeof(clockStr), "%Y-%m-%d %H:%M:%S", localtime(&now));
    }

    Color pcol = playerHitT > 0 ? ColorLerp(playerColor, RED, 0.75f * Clamp(playerHitT, 0.0f, 1.0f)) : playerColor;   // red hue when you get hit
    BeginDrawing();
    ClearBackground({40, 20, 35, 255});
    if (scene != ROOM1 && scene != ROOM2) DrawRectangleGradientV(0, 0, W, H, skyTop, skyBot);

    if (scene == MENU) {
        BeginMode3D(MakeCamera({0, 1.2f, 0}, 6, 2));
        Person showcase = {}; showcase.yaw = timer * 50;
        DrawPerson(showcase, playerColor, 0, 0);
        EndMode3D();
    } else if (scene == OUTSIDE) {
        Camera3D cam3 = MakeCamera({p.x, 2 + p.y * 0.3f, p.z - 4}, 8, 4);
        BeginMode3D(cam3);
        DrawPlane({0, 0, 0}, {400, 400}, {90, 190, 90, 255});
        for (Flower& f : flowers)
            if (Vector2Distance({f.pos.x, f.pos.z}, {p.x, p.z}) < 50) DrawFlower(f);   // skip far-away flowers
        if (!stick.pickedUp) DrawCylinderEx(stick.pos, Vector3Add(stick.pos, {0.9f, 0.15f, 0}), 0.04f, 0.04f, 6, BROWN);
        DrawCube({0, 3, -12}, 6, 6, 6, WHITE);
        DrawCubeWires({0, 3, -12}, 6, 6, 6, LIGHTGRAY);
        DrawCube({0, 1.5f, -8.95f}, 1.6f, 3, 0.1f, CYAN_C);
        for (NPCInfo& n : npcs) if (n.loc == OUTSIDE) DrawNPC(n);
        DrawPerson(player, pcol, stick.held ? 1 : 0, stick.swingT);
        EndMode3D();
        DrawNPCLabels(cam3);
    } else {   // ROOM1 or ROOM2
        Color floor = {255, 150, 190, 255}, wall = {255, 210, 228, 255}, wall2 = {250, 196, 216, 255};
        Camera3D cam3 = MakeCamera({p.x, 1.2f + p.y * 0.3f, p.z}, 10, 6.5f);
        BeginMode3D(cam3);
        DrawPlane({0, 0, 0}, {20, 20}, floor);
        DrawCube({0, 3, -10}, 20, 6, 0.2f, wall);
        DrawCube({-10, 3, 0}, 0.2f, 6, 20, wall2);
        DrawCube({10, 3, 0}, 0.2f, 6, 20, wall2);
        if (scene == ROOM1) DrawCube({0, 1.5f, -9.85f}, 1.6f, 3, 0.1f, CYAN_C);            // exit outside
        DrawCube({(scene == ROOM1 ? 1 : -1) * 9.85f, 1.5f, 0}, 0.1f, 3, 1.6f, CYAN_C);     // doorway between rooms
        for (NPCInfo& n : npcs) if (n.loc == scene) DrawNPC(n);
        DrawPerson(player, pcol, stick.held ? 1 : 0, stick.swingT);
        EndMode3D();
        DrawNPCLabels(cam3);
    }

    float dark = KillDark();   // the lights go out around a killing
    if (dark > 0) DrawRectangle(0, 0, W, H, Fade(BLACK, dark));

    if (scene == MENU) {
        if (pickingSoulmate) DrawSoulmateScreen();
        else if (inSettings) DrawSettings();
        else {
            DrawMainMenu();
            DrawAINotice();
            DrawBuildStamp();
        }
    } else {
        ControlHint(); DrawHUD(dayT); DrawNews(); DrawMiniMap();
    }
    bool blink = (int)(timer * 2) % 2 == 0;
    if (guessing) DrawGuessScreen();
    else if (colorPicking) DrawColorPicker();
    else if (namePrompt) DrawNamePrompt();
    else if (choosing) { if (pickingTarget) DrawTargetPicker(); else DrawChoices(); }
    else if (talking) Dialogue(blink);
    if (prompt) Prompt(prompt);
    if (gameOver) DrawGameOver();
    DrawRectangle(0, 0, W, H, Fade(BLACK, fade));
    EndDrawing();
}

int main() {
    InitWindow(W, H, "Purple World");
    SetExitKey(KEY_NULL);   // ESC only backs out of Settings, it shouldn't quit the whole game
    SetRandomSeed((unsigned int)time(nullptr));   // a different flower layout and a different set of relationships every run
    InitNPCs();
    LoadConfig();
    LoadDialogue();
    LoadAudio();
    SetVolume(sfxVolume);
    SocialInit(social, NPC_COUNT, (unsigned)GetRandomValue(1, 1000000));

    for (int i = 0; i < START_FLOWERS; i++)
        flowers[i] = {RandomOutsidePos(), GetRandomValue(0, 1) ? flowerRed : flowerPink, 30, 1, false, 0};
    // flowers[START_FLOWERS..MAX_FLOWERS) stay inactive (hp 0) - open slots for happy NPCs to grow into later

    hasSaveFile = SaveExists();
    menuSel = hasSaveFile ? 1 : 0;   // a returning player lands on "Load Saved Game"
    AIDialogue_SetStyle(styleNote);   // tone and world notes for the AI (from dialogue.json)
#ifdef PLATFORM_WEB
    emscripten_set_main_loop(Frame, 0, 1);
#else
    SetTargetFPS(60);
    while (!WindowShouldClose()) Frame();
#endif
    CloseWindow();
}