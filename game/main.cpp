#include "raylib.h"
#include "rlgl.h"
#include "raymath.h"
#include "jsonutil.h"
#include "npc_brain.h"
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <ctime>
#include <string>
#include <vector>
#include <algorithm>
#include <cctype>
#ifdef PLATFORM_WEB
#include <emscripten.h>
#endif

enum Scene { MENU, OUTSIDE, ROOM1, ROOM2 };
struct Person { Vector3 pos; float yaw, velY, walk; bool moving; };

// Every room-NPC is a tiny rule-based "agent": it tracks one opinion value about
// the player (nudged by how you treat it and what color you wear), and reads its
// dialogue from dialogue.json by mood. This is NOT a trained neural net - it's a
// small, transparent set of rules that produces similar-looking behavior without
// needing training data or a paid AI API call at runtime.
struct NPCInfo {
    Vector3 pos; Color color; const char* name; float yaw = 0; Scene room = ROOM1;
    float opinion = 0;                                   // -100 (furious) .. 0 (neutral) .. 100 (adoring)
    Color favColor = {0, 0, 0, 0}, dislikeColor = {0, 0, 0, 0};
    const char* dialogueId = nullptr;                    // key into dialogue.json; null = uses its own special flow
    std::vector<std::string> happy, neutral, mad;         // loaded from dialogue.json
    float curiosity = 0.5f, friendliness = 0.5f, patience = 0.5f;   // personality vector, fed into the tiny neural net
    int timesTalked = 0;                                             // memory: how often you've spoken
};
struct Flower { Vector3 pos; Color petal; float hp; float anim = 1; bool dying = false; float dyingT = 0; };
struct Stick { Vector3 pos; bool held, pickedUp; float swingT; };

const int W = 960, H = 540;
const int START_FLOWERS = 14, MAX_FLOWERS = 40;   // extra slots let happy NPCs "grow" new flowers later
const Color CYAN_C = {0, 220, 230, 255};
const Color GREEN_C = {60, 160, 60, 255}, CENTER_C = {245, 210, 40, 255};   // stem/center, never randomized

// ---------------- player identity: color picker, name, HP ----------------
const char* colorNames[5] = {"Pink", "Purple", "Cyan", "Yellow", "Green"};
Color colorValues[5] = {PINK, {140, 70, 200, 255}, CYAN_C, {240, 205, 40, 255}, {80, 180, 80, 255}};
bool ColorsEqual(Color a, Color b) { return a.r == b.r && a.g == b.g && a.b == b.b; }

Scene scene = MENU, nextScene = MENU;
Person player = {};
// npcs[0] asks your color; npcs[1] asks your name (then greets you by it); npcs[2] is a fully data-driven villager.
NPCInfo npcs[3] = {
    {{4, 0, -3}, YELLOW, "Stranger", 0, ROOM1},
    {{0, 0, 4}, ORANGE, "Elder", 0, ROOM2, 0, colorValues[3], colorValues[2], "elder"},      // favors Yellow, dislikes Cyan
    {{-4, 0, 4}, {80, 180, 80, 255}, "Villager", 0, ROOM2, 0, colorValues[0], colorValues[1], "villager"}   // favors Pink, dislikes Purple
};
Flower flowers[MAX_FLOWERS] = {};   // slots beyond START_FLOWERS start inactive (hp 0) as future grow-in spots
Stick stick = {{2.5f, 0, -4.0f}, false, false, 0};
float fade = 0, letters = 0, timer = 0, footTimer = 0, gardenTimer = 6;
int fadeDir = 0;   // 1 = fading to black, -1 = fading back in
bool talking = false;
int talkIdx = 0;
bool aiWaiting = false;
float aiTimeoutT = 0;
char curL1[128] = "", curL2[128] = "";   // the two lines the open dialogue box is currently showing
float camAngle = 0, camZoom = 1.0f;      // mouse/keyboard camera orbit + zoom

float playerHP = 100;
char playerName[16] = "???";
bool hasName = false;
bool colorPicking = false, namePrompt = false;
int colorSel = 0;
char nameBuf[16] = "";
float hurtTimer = 0;
float aggression = 0;   // 0..1, rises when you swing the stick, slowly decays - a proxy for "how you've been acting"
float dayT = 1.0f;      // 1 = full day, 0 = full night; computed once per frame, used for drawing and the brain's input
char clockStr[32] = "";
float clockTimer = 0;

// ---------------- settings ----------------
float sfxVolume = 1.0f;
int quality = 2;             // 0 low, 1 medium, 2 high - controls mesh smoothness only
bool dayNightOn = true;
bool flowerDamageOn = true;
int keyInteract = KEY_SPACE;
bool inSettings = false, rebinding = false;
int settingsSel = 0;
bool aiDialogueOn = false;   // off by default - see npc_brain.h comment and the chat explanation for why
bool hasSaveFile = false;

int MeshSides() { return quality == 0 ? 6 : quality == 1 ? 10 : 16; }

// ---------------- config.json / dialogue.json (tiny hand-written readers - see jsonutil.h) ----------------
float walkSpeed = 5, jumpVel = 9;
Color playerColor = {140, 70, 200, 255};
Color flowerRed = RED, flowerPink = PINK;

Color JColorC(const std::string& j, const char* key, Color def) {   // RGB -> raylib Color convenience wrapper
    RGB r = JColor(j, key, {def.r, def.g, def.b, def.a});
    return Color{r.r, r.g, r.b, r.a};
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
void LoadDialogue() {   // fills npcs[i].happy/neutral/mad from dialogue.json by dialogueId
    char* text = LoadFileText("dialogue.json");
    if (!text) return;
    std::string j = text;
    UnloadFileText(text);
    for (auto& n : npcs) {
        if (!n.dialogueId) continue;
        size_t p = j.find(std::string("\"") + n.dialogueId + "\"");
        if (p == std::string::npos) continue;
        size_t start = j.find('{', p);
        size_t end = FindMatchingBrace(j, start);
        if (end == std::string::npos) continue;
        std::string block = j.substr(start, end - start + 1);
        n.happy = JStrArray(block, "happy");
        n.neutral = JStrArray(block, "neutral");
        n.mad = JStrArray(block, "mad");
    }
}
std::string PickLine(NPCInfo& n) {   // chooses a line for the NPC's current mood and fills in {playerName}
    std::vector<std::string>* arr = n.opinion > 30 ? &n.happy : n.opinion < -30 ? &n.mad : &n.neutral;
    if (arr->empty()) return "...";
    return FillName((*arr)[GetRandomValue(0, (int)arr->size() - 1)], playerName);
}

// ---------------- sound (each file is optional - missing files are silently skipped) ----------------
Sound sFoot, sJump, sDoor, sBlip;
Music music;
bool hasFoot, hasJump, hasDoor, hasBlip, hasMusic;
void PlaySafe(Sound s, bool ok) { if (ok) PlaySound(s); }

void LoadAudio() {
    InitAudioDevice();
    hasFoot = FileExists("assets/sounds/footstep.wav");
    hasJump = FileExists("assets/sounds/jump.wav");
    hasDoor = FileExists("assets/sounds/door.wav");
    hasBlip = FileExists("assets/sounds/blip.wav");
    hasMusic = FileExists("assets/sounds/music.wav");
    if (hasFoot) sFoot = LoadSound("assets/sounds/footstep.wav");
    if (hasJump) sJump = LoadSound("assets/sounds/jump.wav");
    if (hasDoor) sDoor = LoadSound("assets/sounds/door.wav");
    if (hasBlip) sBlip = LoadSound("assets/sounds/blip.wav");
    if (hasMusic) { music = LoadMusicStream("assets/sounds/music.wav"); PlayMusicStream(music); SetMusicVolume(music, 0.5f); }
}

// ---------------- save / load (localStorage; only exists in the browser build) ----------------
int ColorIndex() { for (int i = 0; i < 5; i++) if (ColorsEqual(playerColor, colorValues[i])) return i; return -1; }

#ifdef PLATFORM_WEB
bool SaveExists() { return EM_ASM_INT({ return localStorage.getItem('purpleSave') ? 1 : 0; }); }
void SaveGame() {
    EM_ASM({ localStorage.setItem('purpleSave', $0 + ',' + $1 + ',' + $2 + ',' + $3 + ',' + $4 + ',' + $5 + ',' + $6 + ',' + $7); },
           (int)scene, player.pos.x, player.pos.y, player.pos.z, stick.pickedUp ? 1 : 0, (int)playerHP, ColorIndex(), hasName ? 1 : 0);
    EM_ASM({ localStorage.setItem('purpleName', UTF8ToString($0)); }, playerName);
}
void LoadGame() {
    char buf[128] = "";
    int ok = EM_ASM_INT({
        var s = localStorage.getItem('purpleSave');
        if (!s) return 0;
        stringToUTF8(s, $0, $1);
        return 1;
    }, buf, sizeof(buf));
    if (!ok) return;
    int sc, st, hp, colorIdx, named;
    float x, y, z;
    sscanf(buf, "%d,%f,%f,%f,%d,%d,%d,%d", &sc, &x, &y, &z, &st, &hp, &colorIdx, &named);
    scene = (Scene)sc;
    player.pos = {x, y, z};
    stick.pickedUp = stick.held = st;
    playerHP = hp;
    hasName = named;
    if (colorIdx >= 0 && colorIdx < 5) playerColor = colorValues[colorIdx];

    char nbuf[16] = "";
    int nok = EM_ASM_INT({
        var s = localStorage.getItem('purpleName');
        if (!s) return 0;
        stringToUTF8(s, $0, $1);
        return 1;
    }, nbuf, sizeof(nbuf));
    if (nok) strncpy(playerName, nbuf, sizeof(playerName) - 1);
}
#else
bool SaveExists() { return false; }
void SaveGame() {}
void LoadGame() {}
#endif

// ---------------- optional AI-generated dialogue flavor text (see shell.html's AIDialogue object) ----------------
// Genuinely free (DistilGPT2 running client-side via transformers.js, no server, no API key) but web-only:
// there's no JS engine in a native build, so these are no-ops there and the game always falls back
// to the curated dialogue.json lines. Off by default in Settings - see the chat for why.
#ifdef PLATFORM_WEB
void AIDialogue_Start(const std::string& prompt) {
    EM_ASM({ if (window.AIDialogue) window.AIDialogue.generate(UTF8ToString($0)); }, prompt.c_str());
}
int AIDialogue_State() {   // 0 busy, 1 ready, 2 error/unavailable
    return EM_ASM_INT({
        if (!window.AIDialogue) return 2;
        var s = window.AIDialogue.state;
        if (s === 'ready') return 1;
        if (s === 'error') return 2;
        return 0;
    });
}
std::string AIDialogue_Result() {
    char buf[256] = "";
    EM_ASM({ stringToUTF8((window.AIDialogue && window.AIDialogue.result) || '', $0, $1); }, buf, sizeof(buf));
    return std::string(buf);
}
#else
void AIDialogue_Start(const std::string&) {}
int AIDialogue_State() { return 2; }
std::string AIDialogue_Result() { return ""; }
#endif

// A little safety net over whatever the model produces - see jsonutil.h's LooksUsable().

// ---------------- drawing a person: cylinder limbs, cube body, sphere head ----------------
void Limb(float x, float y, float angle, float length, float radius, Color c) {
    rlPushMatrix();
    rlTranslatef(x, y, 0);
    rlRotatef(angle, 1, 0, 0);
    DrawCylinderEx({0, 0, 0}, {0, -length, 0}, radius, radius, MeshSides(), c);
    rlPopMatrix();
}

void DrawPerson(Person p, Color c, bool holdingStick) {
    float swing = p.moving ? sinf(p.walk * 10) * 35 : 0;
    float armL = swing, armR = -swing, legL = -swing, legR = swing;
    if (p.pos.y > 0) { armL = armR = -150; legL = -35; legR = 20; }        // jump pose
    if (stick.swingT > 0) armR = -120 + stick.swingT * 400;                // swing pose overrides right arm
    Color limb = ColorBrightness(c, -0.25f);

    float r = fmaxf(0.2f, 0.45f - p.pos.y * 0.05f);                        // shadow
    DrawCylinder({p.pos.x, 0.02f, p.pos.z}, r, r, 0.01f, 16, Fade(BLACK, 0.25f));

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
    if (holdingStick) {   // a simple stick held in the right hand
        rlPushMatrix();
        rlTranslatef(0.55f, 1.5f, 0);
        rlRotatef(armR, 1, 0, 0);
        DrawCylinderEx({0, 0, 0}, {0, -1.0f, 0.3f}, 0.05f, 0.05f, 6, BROWN);
        rlPopMatrix();
    }
    rlPopMatrix();
}

void DrawFlower(Flower f) {
    if (f.hp <= 0 && !f.dying) return;                                     // hp<=0 and not dying = an unused slot
    float scale = f.dying ? (1 - f.dyingT) : f.anim;                       // grows in, or shrinks away when killed
    if (scale <= 0.02f) return;
    Color petal = f.dying ? ColorLerp(f.petal, PURPLE, f.dyingT) : f.petal;
    float stemH = 0.5f * scale;
    DrawCylinder({f.pos.x, 0, f.pos.z}, 0.03f * scale, 0.03f * scale, stemH, 8, GREEN_C);
    Vector3 top = {f.pos.x, stemH, f.pos.z};
    float off = 0.14f * scale, r = 0.1f * scale;
    DrawSphere(Vector3Add(top, {off, 0, 0}), r, petal);                     // 4 petals
    DrawSphere(Vector3Add(top, {-off, 0, 0}), r, petal);
    DrawSphere(Vector3Add(top, {0, 0, off}), r, petal);
    DrawSphere(Vector3Add(top, {0, 0, -off}), r, petal);
    DrawSphere(top, 0.09f * scale, CENTER_C);                               // center (always yellow)
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

void Dialogue(bool blink) {
    NPCInfo n = npcs[talkIdx];
    Rectangle box = {40, H - 170.0f, W - 80.0f, 140};
    DrawRectangleRounded(box, 0.15f, 8, {255, 250, 235, 240});
    DrawRectangleRoundedLines(box, 0.15f, 8, {120, 90, 40, 255});
    DrawRectangleRounded({60, box.y - 18, 120, 34}, 0.5f, 8, n.color);
    DrawText(n.name, 76, box.y - 10, 20, {60, 40, 0, 255});

    int n1 = strlen(curL1), shown = (int)letters;
    DrawText(TextSubtext(curL1, 0, shown < n1 ? shown : n1), 70, box.y + 35, 24, DARKGRAY);
    if (shown > n1) DrawText(TextSubtext(curL2, 0, shown - n1), 70, box.y + 70, 24, DARKGRAY);
    if (shown >= n1 + (int)strlen(curL2) && blink) DrawText("[Space]", W - 150, H - 62, 20, GRAY);
}

void DrawColorPicker() {   // a simple keyboard-driven "dropdown": Up/Down to move, Space to confirm
    Rectangle box = {40, H - 170.0f, W - 80.0f, 140};
    DrawRectangleRounded(box, 0.15f, 8, {255, 250, 235, 240});
    DrawRectangleRoundedLines(box, 0.15f, 8, {120, 90, 40, 255});
    DrawText("What color are you feeling like today?", (int)box.x + 20, (int)box.y + 10, 20, DARKGRAY);
    for (int i = 0; i < 5; i++) {
        float y = box.y + 40 + i * 18.0f;
        if (i == colorSel) DrawText(">", (int)box.x + 20, (int)y - 2, 18, BLACK);
        DrawRectangle((int)box.x + 45, (int)y, 16, 16, colorValues[i]);
        DrawRectangleLines((int)box.x + 45, (int)y, 16, 16, DARKGRAY);
        DrawText(colorNames[i], (int)box.x + 68, (int)y - 2, 18, DARKGRAY);
    }
    DrawText("Up/Down to choose, Space to confirm", (int)box.x + 260, (int)box.y + 110, 14, GRAY);
}

void DrawNamePrompt() {   // types via the keyboard, confirms with Enter
    Rectangle box = {40, H - 170.0f, W - 80.0f, 140};
    DrawRectangleRounded(box, 0.15f, 8, {255, 250, 235, 240});
    DrawRectangleRoundedLines(box, 0.15f, 8, {120, 90, 40, 255});
    DrawText("Welcome! What is your name?", (int)box.x + 20, (int)box.y + 15, 22, DARKGRAY);
    Rectangle field = {box.x + 20, box.y + 55, 300, 36};
    DrawRectangleRec(field, WHITE);
    DrawRectangleLinesEx(field, 2, DARKGRAY);
    DrawText(nameBuf, (int)field.x + 8, (int)field.y + 8, 22, BLACK);
    if ((int)(timer * 2) % 2 == 0) DrawText("|", (int)field.x + 10 + MeasureText(nameBuf, 22), (int)field.y + 8, 22, BLACK);
    DrawText("Press Enter to confirm", (int)box.x + 20, (int)box.y + 105, 14, GRAY);
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
    Rectangle box = {W / 2 - 240.0f, H / 2 - 180.0f, 480, 360};
    DrawRectangleRounded(box, 0.08f, 8, {255, 250, 235, 245});
    DrawRectangleRoundedLines(box, 0.08f, 8, {120, 90, 40, 255});
    DrawText("Settings", (int)box.x + 20, (int)box.y + 16, 26, DARKGRAY);

    const char* qualityNames[3] = {"Low", "Medium", "High"};
    char labels[6][48];
    snprintf(labels[0], 48, "Volume: %d%%", (int)(sfxVolume * 100));
    snprintf(labels[1], 48, "Graphics: %s", qualityNames[quality]);
    snprintf(labels[2], 48, "Day/Night Cycle: %s", dayNightOn ? "On" : "Off");
    snprintf(labels[3], 48, "Flower Damage: %s", flowerDamageOn ? "On" : "Off");
    snprintf(labels[4], 48, "Interact Key: %s", rebinding ? "press any key..." : KeyLabel(keyInteract));
    snprintf(labels[5], 48, "AI Dialogue (experimental): %s", aiDialogueOn ? "On" : "Off");

    for (int i = 0; i < 6; i++) {
        float y = box.y + 60 + i * 40.0f;
        if (i == settingsSel) DrawRectangleRounded({box.x + 12, y - 4, box.width - 24, 32}, 0.3f, 6, Fade(playerColor, 0.25f));
        DrawText(labels[i], (int)box.x + 24, (int)y, 20, DARKGRAY);
    }
    if (settingsSel == 5) DrawText("Downloads ~100MB the first time; each line takes a few seconds", (int)box.x + 24, (int)(box.y + 300), 13, DARKGRAY);
    DrawText("Up/Down select   Left/Right change   Enter rebind   Esc back", (int)box.x + 20, (int)(box.y + box.height - 28), 13, GRAY);
}

void ControlHint() {   // desktop-only hint; touch devices get on-screen buttons instead (see shell.html)
    DrawText("Q/E or drag: look   Z/X or scroll: zoom", 12, H - 24, 14, Fade(WHITE, 0.7f));
}

void DrawHUD(float dayT) {   // top-left: live date/time, dimmed at night; top-right: name + HP; bottom-left: flower count
    Color clockColor = ColorLerp({90, 80, 110, 255}, WHITE, dayT);
    DrawText(clockStr, 12, 12, 18, clockColor);

    DrawText(playerName, W - 200, 12, 20, WHITE);
    Rectangle bar = {(float)(W - 110), 16, 90, 16};
    DrawRectangleRec(bar, Fade(BLACK, 0.5f));
    DrawRectangleRec({bar.x, bar.y, bar.width * Clamp(playerHP, 0, 100) / 100, bar.height}, RED);
    DrawRectangleLinesEx(bar, 1, WHITE);

    int alive = 0;
    for (Flower& f : flowers) if (f.hp > 0) alive++;
    DrawText(TextFormat("Flowers: %d", alive), 12, H - 46, 16, Fade(WHITE, 0.85f));
}

void DrawMiniMap() {   // a simple top-down radar in the corner
    float cx = W - 70.0f, cy = H - 70.0f, R = 55.0f;
    DrawCircle(cx, cy, R + 4, Fade(BLACK, 0.45f));
    DrawCircleLines(cx, cy, R + 4, WHITE);
    if (scene == OUTSIDE) {
        float s = R / 50.0f;
        DrawRectangle(cx - 4, cy + (-12) * s - 4, 8, 8, WHITE);            // the building
        Vector2 pl = {Clamp(cx + player.pos.x * s, cx - R, cx + R), Clamp(cy + player.pos.z * s, cy - R, cy + R)};
        DrawCircle(pl.x, pl.y, 4, playerColor);
    } else {
        float s = R / 10.0f;
        for (NPCInfo& n : npcs) if (n.room == scene) DrawCircle(cx + n.pos.x * s, cy + n.pos.z * s, 4, n.color);
        DrawCircle(cx + player.pos.x * s, cy + player.pos.z * s, 4, playerColor);
    }
}

void StartTalk(int idx) {
    NPCInfo& n = npcs[idx];
    talking = true; talkIdx = idx; letters = 0; colorPicking = false; namePrompt = false;
    player.yaw = n.yaw + 180;
    PlaySafe(sBlip, hasBlip);
    n.timesTalked++;

    float colorMatch = 0;
    if (n.favColor.a && ColorsEqual(playerColor, n.favColor)) colorMatch = 1;
    else if (n.dislikeColor.a && ColorsEqual(playerColor, n.dislikeColor)) colorMatch = -1;

    // The NPC's personality, memory and mood, plus what the player's been up to, all feed
    // the tiny trained neural net (see npc_brain.h / train_brain.py) to decide how this
    // conversation affects the relationship - not a fixed if/else table.
    BrainInput in = {n.curiosity, n.friendliness, n.patience, n.opinion, (float)n.timesTalked, colorMatch, aggression, dayT};
    BrainOutput out = RunBrain(in);
    n.opinion = Clamp(n.opinion + out.opinionDelta, -100, 100);

    bool punched = GetRandomValue(0, 999) < (int)(out.punchChance * 1000);
    if (punched) { playerHP -= 10; PlaySafe(sDoor, hasDoor); }
    const char* punchNote = "(They shove you! Ouch.)";

    if (idx == 0) {
        strcpy(curL1, "What color are you feeling like today?");
        strcpy(curL2, punched ? punchNote : "(Up/Down to pick, Space to confirm)");
    } else if (idx == 1 && !hasName) {
        strcpy(curL1, "Welcome! What is your name?");
        strcpy(curL2, punched ? punchNote : "(Type it, then press Enter)");
    } else if (aiDialogueOn) {
        // Kick off generation and show "..." while we wait; Frame() polls AIDialogue_State()
        // each frame and swaps in the real line (or falls back to PickLine) once it settles.
        const char* moodWord = n.opinion > 30 ? "happy and friendly" : n.opinion < -30 ? "annoyed and grumpy" : "calm";
        std::string prompt = std::string(n.name) + " is a " + moodWord + " villager in a small colorful game world. "
                            + n.name + " says to " + playerName + ": \"";
        AIDialogue_Start(prompt);
        aiWaiting = true; aiTimeoutT = 6.0f;
        strcpy(curL1, "...");
        strcpy(curL2, punched ? punchNote : "");
        letters = strlen(curL1) + strlen(curL2);   // show it immediately - no typewriter needed for a placeholder
    } else {
        std::string line = PickLine(n);
        strncpy(curL1, line.c_str(), sizeof(curL1) - 1); curL1[sizeof(curL1) - 1] = 0;
        strcpy(curL2, punched ? punchNote : "");
    }
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
    PlaySafe(sDoor, hasDoor);
    SaveGame();
}

bool Blocked(Vector3 p) {
    if (scene == OUTSIDE) return (fabsf(p.x) < 3.4f && p.z > -15.4f && p.z < -8.6f) || fabsf(p.x) > 60 || fabsf(p.z) > 60;
    for (NPCInfo& n : npcs) if (n.room == scene && Vector2Distance({p.x, p.z}, {n.pos.x, n.pos.z}) < 1) return true;
    return fabsf(p.x) > 9.5f || fabsf(p.z) > 9.5f;
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
        if (footTimer <= 0 && player.pos.y == 0) { PlaySafe(sFoot, hasFoot); footTimer = 0.32f; }
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

void SwingStick() {
    stick.swingT = 0.25f;
    aggression = fminf(1.0f, aggression + 0.4f);
    for (Flower& f : flowers)
        if (f.hp > 0 && !f.dying && Vector3Distance(player.pos, f.pos) < 2.0f) f.hp -= 15;
}

Vector3 RandomOutsidePos() {
    Vector3 pos;
    do { pos = {(float)GetRandomValue(-45, 45), 0, (float)GetRandomValue(-45, 45)}; }
    while (fabsf(pos.x) < 5 && pos.z > -17 && pos.z < -6);   // keep flowers off the building footprint
    return pos;
}

void KillFlowersOnDeath() {   // dying (not vanishing outright) reuses the same shrink-and-purple animation
    int toKill = 5;
    for (Flower& f : flowers) {
        if (toKill <= 0) break;
        if (f.hp > 0 && !f.dying) { f.dying = true; f.dyingT = 0; toKill--; }
    }
}

void UpdateNPCBehavior(float dt) {   // the "gossip" + gardening tick - simple rules, not a trained model
    gardenTimer -= dt;
    if (gardenTimer > 0) return;
    gardenTimer = 6.0f;

    for (NPCInfo& n : npcs) {
        if (n.opinion > 50) {                                              // happy NPCs plant a new flower
            for (Flower& f : flowers) if (f.hp <= 0 && !f.dying) {
                f = {RandomOutsidePos(), GetRandomValue(0, 1) ? flowerRed : flowerPink, 30, 0, false, 0};
                break;
            }
        }
        if (n.opinion < -50) {                                             // furious NPCs kill one
            for (Flower& f : flowers) if (f.hp > 0 && !f.dying) { f.dying = true; f.dyingT = 0; break; }
        }
    }
    float avg = (npcs[0].opinion + npcs[1].opinion + npcs[2].opinion) / 3.0f;
    for (NPCInfo& n : npcs) n.opinion += (avg - n.opinion) * 0.3f;          // gossip: opinions drift toward the group average
}

// ---------------- one frame ----------------
void Frame() {
    float dt = fminf(GetFrameTime(), 0.05f);
    timer += dt;
    if (stick.swingT > 0) stick.swingT -= dt;
    if (fadeDir) {
        fade += fadeDir * dt * 2.5f;
        if (fade >= 1) { fade = 1; fadeDir = -1; Enter(nextScene); }
        if (fade <= 0) { fade = 0; fadeDir = 0; }
    }
    if (hasMusic) UpdateMusicStream(music);
    UpdateCamera(dt);

    for (Flower& f : flowers) {   // grow-in / shrink-out animation ticks, always running
        if (f.hp <= 0 && !f.dying) continue;
        if (f.anim < 1) f.anim = fminf(1, f.anim + dt);
        if (f.dying) { f.dyingT += dt; if (f.dyingT >= 1) { f.hp = 0; f.dying = false; f.anim = 1; } }
    }
    if (scene != MENU) UpdateNPCBehavior(dt);
    aggression = fmaxf(0.0f, aggression - dt * 0.03f);   // slowly cools off if you stop being aggressive
    dayT = dayNightOn ? (sinf((timer / 60.0f) * 2 * PI - PI / 2) + 1) / 2 : 1.0f;   // used for drawing AND as a brain input

    bool interact = fadeDir == 0 && !inSettings && IsKeyPressed(keyInteract);
    bool ground = player.pos.y == 0;
    const char* prompt = nullptr;
    Vector3 p = player.pos;

    if (scene == MENU) {
        if (inSettings) {
            if (rebinding) {
                int k = GetKeyPressed();
                if (k != 0) { keyInteract = k; rebinding = false; }
            } else {
                if (IsKeyPressed(KEY_UP)) settingsSel = (settingsSel + 5) % 6;
                if (IsKeyPressed(KEY_DOWN)) settingsSel = (settingsSel + 1) % 6;
                int dir = (IsKeyPressed(KEY_RIGHT) ? 1 : 0) - (IsKeyPressed(KEY_LEFT) ? 1 : 0);
                if (dir && settingsSel == 0) { sfxVolume = Clamp(sfxVolume + dir * 0.1f, 0, 1); SetMasterVolume(sfxVolume); }
                else if (dir && settingsSel == 1) quality = (quality + dir + 3) % 3;
                else if (dir && settingsSel == 2) dayNightOn = !dayNightOn;
                else if (dir && settingsSel == 3) flowerDamageOn = !flowerDamageOn;
                else if (settingsSel == 4 && IsKeyPressed(KEY_ENTER)) rebinding = true;
                else if (dir && settingsSel == 5) aiDialogueOn = !aiDialogueOn;
                if (IsKeyPressed(KEY_ESCAPE) || IsKeyPressed(KEY_BACKSPACE)) inSettings = false;
            }
        } else {
            if (IsKeyPressed(KEY_SPACE)) Go(OUTSIDE);
            else if (hasSaveFile && IsKeyPressed(KEY_C)) LoadGame();
            else if (IsKeyPressed(KEY_S)) inSettings = true;
        }
    } else if (talking) {
        if (colorPicking) {
            if (IsKeyPressed(KEY_UP) || IsKeyPressed(KEY_W)) colorSel = (colorSel + 4) % 5;
            if (IsKeyPressed(KEY_DOWN) || IsKeyPressed(KEY_S)) colorSel = (colorSel + 1) % 5;
            if (interact) { playerColor = colorValues[colorSel]; colorPicking = false; talking = false; SaveGame(); }
        } else if (namePrompt) {
            int c;
            while ((c = GetCharPressed()) != 0) {
                size_t len = strlen(nameBuf);
                if (c != ',' && c >= 32 && c < 127 && len < sizeof(nameBuf) - 1) { nameBuf[len] = (char)c; nameBuf[len + 1] = 0; }
            }
            if (IsKeyPressed(KEY_BACKSPACE) && strlen(nameBuf) > 0) nameBuf[strlen(nameBuf) - 1] = 0;
            if (IsKeyPressed(KEY_ENTER)) {
                strncpy(playerName, strlen(nameBuf) ? nameBuf : "???", sizeof(playerName) - 1);
                hasName = true; namePrompt = false; talking = false; SaveGame();
            }
        } else if (aiWaiting) {
            aiTimeoutT -= dt;
            int state = AIDialogue_State();
            if (state == 1) {   // ready - use it if it passes the basic safety/quality check, else fall back
                std::string result = AIDialogue_Result();
                NPCInfo& n = npcs[talkIdx];
                std::string line = LooksUsable(result) ? result : PickLine(n);
                strncpy(curL1, line.c_str(), sizeof(curL1) - 1); curL1[sizeof(curL1) - 1] = 0;
                letters = 0; aiWaiting = false;
            } else if (state == 2 || aiTimeoutT <= 0) {   // error, or took too long - fall back quietly
                NPCInfo& n = npcs[talkIdx];
                std::string line = PickLine(n);
                strncpy(curL1, line.c_str(), sizeof(curL1) - 1); curL1[sizeof(curL1) - 1] = 0;
                letters = 0; aiWaiting = false;
            }
        } else {
            letters += dt * 40;
            int total = strlen(curL1) + strlen(curL2);
            if (interact) {
                if (letters < total) letters = total;
                else if (talkIdx == 0) colorPicking = true;
                else if (talkIdx == 1 && !hasName) { namePrompt = true; nameBuf[0] = 0; }
                else talking = false;
            }
        }
    } else if (fadeDir == 0) {
        Move(dt);
        p = player.pos;

        if (scene == OUTSIDE) {
            if (ground && Vector3Distance(p, {0, 0, -8.4f}) < 1.5f) {
                prompt = "Press space to enter";
                if (interact) { Go(ROOM1); interact = false; }
            } else if (stick.held) {
                prompt = "Press space to swing";
                if (interact) { SwingStick(); interact = false; }
            } else if (!stick.pickedUp && Vector3Distance(p, stick.pos) < 1.3f) {
                prompt = "Press space to pick up stick";
                if (interact) { stick.pickedUp = stick.held = true; interact = false; SaveGame(); }
            }
        } else if (scene == ROOM1 && ground && Vector3Distance(p, {0, 0, -9}) < 1.5f) {
            prompt = "Press space to exit";
            if (interact) { Go(OUTSIDE); interact = false; }
        } else if (scene == ROOM1 && ground && Vector3Distance(p, {9, 0, 0}) < 1.5f) {
            prompt = "Press space to go deeper";
            if (interact) { Go(ROOM2); interact = false; }
        } else if (scene == ROOM2 && ground && Vector3Distance(p, {-9, 0, 0}) < 1.5f) {
            prompt = "Press space to go back";
            if (interact) { Go(ROOM1); interact = false; }
        } else if (scene == ROOM1 || scene == ROOM2) {
            for (int i = 0; i < 3; i++) {
                NPCInfo& n = npcs[i];
                if (n.room != scene || !ground || Vector3Distance(p, n.pos) >= 2) continue;
                n.yaw = atan2f(p.x - n.pos.x, p.z - n.pos.z) * RAD2DEG;
                prompt = "Press space to talk";
                if (interact) { StartTalk(i); interact = false; }
                break;
            }
        }
        if (interact && ground) { player.velY = jumpVel; PlaySafe(sJump, hasJump); }

        if (scene == OUTSIDE && flowerDamageOn) {   // stepping on a flower stings a little
            hurtTimer -= dt;
            bool touching = false;
            for (Flower& f : flowers) if (f.hp > 0 && !f.dying && Vector3Distance(p, f.pos) < 0.45f) touching = true;
            if (touching && hurtTimer <= 0) { playerHP -= 6; hurtTimer = 0.4f; }
        }
    }

    if (playerHP <= 0 && fadeDir == 0) {   // death (from flowers or an angry NPC's punch) respawns you inside
        playerHP = 100;
        KillFlowersOnDeath();
        talking = colorPicking = namePrompt = false;
        Go(ROOM1);
    }

    // day/night sky colors (dayT was already computed earlier this frame, before NPC interactions needed it)
    Color skyTop = ColorLerp({30, 30, 70, 255}, {70, 130, 230, 255}, dayT);
    Color skyBot = ColorLerp({60, 40, 90, 255}, {255, 170, 210, 255}, dayT);

    clockTimer -= dt;    // real-world date/time, refreshed once a second
    if (clockTimer <= 0) {
        clockTimer = 1.0f;
        time_t now = time(nullptr);
        strftime(clockStr, sizeof(clockStr), "%Y-%m-%d %H:%M:%S", localtime(&now));
    }

    BeginDrawing();
    ClearBackground({40, 20, 35, 255});
    if (scene != ROOM1 && scene != ROOM2) DrawRectangleGradientV(0, 0, W, H, skyTop, skyBot);

    if (scene == MENU) {
        BeginMode3D(MakeCamera({0, 1.2f, 0}, 6, 2));
        Person showcase = {}; showcase.yaw = timer * 50;
        DrawPerson(showcase, playerColor, false);
        EndMode3D();
    } else if (scene == OUTSIDE) {
        BeginMode3D(MakeCamera({p.x, 2 + p.y * 0.3f, p.z - 4}, 8, 4));
        DrawPlane({0, 0, 0}, {400, 400}, {90, 190, 90, 255});
        for (Flower& f : flowers) DrawFlower(f);
        if (!stick.pickedUp) DrawCylinderEx(stick.pos, Vector3Add(stick.pos, {0.9f, 0.15f, 0}), 0.04f, 0.04f, 6, BROWN);
        DrawCube({0, 3, -12}, 6, 6, 6, WHITE);
        DrawCubeWires({0, 3, -12}, 6, 6, 6, LIGHTGRAY);
        DrawCube({0, 1.5f, -8.95f}, 1.6f, 3, 0.1f, CYAN_C);
        DrawPerson(player, playerColor, stick.held);
        EndMode3D();
    } else {   // ROOM1 or ROOM2
        Color floor = {255, 150, 190, 255}, wall = {255, 210, 228, 255}, wall2 = {250, 196, 216, 255};
        BeginMode3D(MakeCamera({p.x, 1.2f + p.y * 0.3f, p.z}, 10, 6.5f));
        DrawPlane({0, 0, 0}, {20, 20}, floor);
        DrawCube({0, 3, -10}, 20, 6, 0.2f, wall);
        DrawCube({-10, 3, 0}, 0.2f, 6, 20, wall2);
        DrawCube({10, 3, 0}, 0.2f, 6, 20, wall2);
        if (scene == ROOM1) DrawCube({0, 1.5f, -9.85f}, 1.6f, 3, 0.1f, CYAN_C);            // exit outside
        DrawCube({(scene == ROOM1 ? 1 : -1) * 9.85f, 1.5f, 0}, 0.1f, 3, 1.6f, CYAN_C);     // doorway between rooms
        for (NPCInfo& n : npcs) if (n.room == scene) {
            Person np{}; np.pos = n.pos; np.yaw = n.yaw;
            DrawPerson(np, n.color, false);
        }
        DrawPerson(player, playerColor, stick.held);
        EndMode3D();
    }

    if (scene == MENU) {
        if (inSettings) DrawSettings();
        else {
            if ((int)(timer * 2) % 2 == 0) DrawText("Press space to start", (W - MeasureText("Press space to start", 36)) / 2, H - 150, 36, WHITE);
            if (hasSaveFile) DrawText("Press C to continue", (W - MeasureText("Press C to continue", 22)) / 2, H - 100, 22, Fade(WHITE, 0.85f));
            DrawText("Press S for settings", (W - MeasureText("Press S for settings", 18)) / 2, H - 68, 18, Fade(WHITE, 0.7f));
        }
    } else {
        ControlHint(); DrawHUD(dayT); DrawMiniMap();
    }
    bool blink = (int)(timer * 2) % 2 == 0;
    if (colorPicking) DrawColorPicker();
    else if (namePrompt) DrawNamePrompt();
    else if (talking) Dialogue(blink);
    if (prompt) Prompt(prompt);
    DrawRectangle(0, 0, W, H, Fade(BLACK, fade));
    EndDrawing();
}

int main() {
    InitWindow(W, H, "Purple World");
    SetExitKey(KEY_NULL);   // ESC now only backs out of Settings, it shouldn't quit the whole game
    LoadConfig();
    LoadDialogue();
    LoadAudio();
    SetMasterVolume(sfxVolume);

    for (int i = 0; i < START_FLOWERS; i++)
        flowers[i] = {RandomOutsidePos(), GetRandomValue(0, 1) ? flowerRed : flowerPink, 30, 1, false, 0};
    // flowers[START_FLOWERS..MAX_FLOWERS) stay inactive (hp 0) - open slots for happy NPCs to grow into later

    hasSaveFile = SaveExists();
    npcs[0].curiosity = 0.8f; npcs[0].friendliness = 0.6f; npcs[0].patience = 0.3f;   // Stranger: curious, a little impatient
    npcs[1].curiosity = 0.3f; npcs[1].friendliness = 0.5f; npcs[1].patience = 0.9f;   // Elder: calm and patient
    npcs[2].curiosity = 0.5f; npcs[2].friendliness = 0.9f; npcs[2].patience = 0.4f;   // Villager: very friendly
#ifdef PLATFORM_WEB
    emscripten_set_main_loop(Frame, 0, 1);
#else
    SetTargetFPS(60);
    while (!WindowShouldClose()) Frame();
#endif
    CloseWindow();
}