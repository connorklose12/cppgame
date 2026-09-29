#include "raylib.h"
#include "rlgl.h"
#include "raymath.h"
#include <cstring>
#ifdef PLATFORM_WEB
#include <emscripten/emscripten.h>
#endif

enum Scene { MENU, OUTSIDE, INSIDE };
struct Person { Vector3 pos; float yaw, velY, walk; bool moving; };

const int W = 960, H = 540;
const Color PURPLE_C = {140, 70, 200, 255}, YELLOW_C = {240, 205, 40, 255}, CYAN_C = {0, 220, 230, 255};
const char* LINE1 = "Hello human, quite the detailed world.";
const char* LINE2 = "Isn't it nice to see how far civilization has progressed?";

Scene scene = MENU, nextScene = MENU;
Person player = {}, npc = {{4, 0, -3}};
float fade = 0, letters = 0, timer = 0;
int fadeDir = 0;          // 1 = fading to black, -1 = fading back in
bool talking = false;

// ---------- drawing a person: cylinder limbs, cube body, sphere head ----------
void Limb(float x, float y, float angle, float length, float radius, Color c) {
    rlPushMatrix();
    rlTranslatef(x, y, 0);
    rlRotatef(angle, 1, 0, 0);
    DrawCylinderEx({0, 0, 0}, {0, -length, 0}, radius, radius, 12, c);
    rlPopMatrix();
}

void DrawPerson(Person p, Color c) {
    float swing = p.moving ? sinf(p.walk * 10) * 35 : 0;           // walk animation
    float armL = swing, armR = -swing, legL = -swing, legR = swing;
    if (p.pos.y > 0) { armL = armR = -150; legL = -35; legR = 20; } // jump pose
    Color limb = ColorBrightness(c, -0.25f);

    float r = fmaxf(0.2f, 0.45f - p.pos.y * 0.05f);                 // shadow
    DrawCylinder({p.pos.x, 0.02f, p.pos.z}, r, r, 0.01f, 16, Fade(BLACK, 0.25f));

    rlPushMatrix();
    rlTranslatef(p.pos.x, p.pos.y, p.pos.z);
    rlRotatef(p.yaw, 0, 1, 0);
    Limb(-0.2f, 0.8f, legL, 0.8f, 0.13f, limb);
    Limb( 0.2f, 0.8f, legR, 0.8f, 0.13f, limb);
    DrawCube({0, 1.25f, 0}, 0.8f, 0.9f, 0.5f, c);
    DrawCubeWires({0, 1.25f, 0}, 0.8f, 0.9f, 0.5f, ColorBrightness(c, -0.5f));
    Limb(-0.52f, 1.6f, armL, 0.75f, 0.11f, limb);
    Limb( 0.52f, 1.6f, armR, 0.75f, 0.11f, limb);
    DrawSphere({0, 2.05f, 0}, 0.35f, ColorBrightness(c, 0.15f));
    rlPopMatrix();
}

// ---------- UI ----------
void Prompt(const char* text) {
    int w = MeasureText(text, 26), x = (W - w) / 2, y = H - 110;
    DrawRectangleRounded({x - 20.0f, y - 12.0f, w + 40.0f, 50}, 0.4f, 8, Fade(BLACK, 0.6f));
    DrawText(text, x, y, 26, WHITE);
}

void Dialogue(bool blink) {
    Rectangle box = {40, H - 170.0f, W - 80.0f, 140};
    DrawRectangleRounded(box, 0.15f, 8, {255, 250, 235, 240});
    DrawRectangleRoundedLines(box, 0.15f, 8, {120, 90, 40, 255});
    DrawRectangleRounded({60, box.y - 18, 120, 34}, 0.5f, 8, YELLOW_C);
    DrawText("Stranger", 76, box.y - 10, 20, {60, 40, 0, 255});

    int n = letters, n1 = strlen(LINE1);                             // typewriter effect
    DrawText(TextSubtext(LINE1, 0, n), 70, box.y + 35, 24, DARKGRAY);
    if (n > n1) DrawText(TextSubtext(LINE2, 0, n - n1), 70, box.y + 70, 24, DARKGRAY);
    if (n >= n1 + (int)strlen(LINE2) && blink) DrawText("[Space]", W - 150, H - 62, 20, GRAY);
}

// ---------- scenes ----------
void Go(Scene s) { nextScene = s; fadeDir = 1; }

void Enter(Scene s) {
    if (s == OUTSIDE) player.pos = (scene == INSIDE) ? Vector3{0, 0, -6.5f} : Vector3{0, 0, 4};
    if (s == INSIDE)  player.pos = {0, 0, -7};
    player.yaw = (s == OUTSIDE && scene == MENU) ? 180 : 0;
    player.velY = 0;
    player.moving = false;
    scene = s;
}

bool Blocked(Vector3 p) {
    if (scene == OUTSIDE)  // the building, and the edge of the world
        return (fabsf(p.x) < 3.4f && p.z > -15.4f && p.z < -8.6f) || fabsf(p.x) > 60 || fabsf(p.z) > 60;
    // room walls, and the yellow person
    return fabsf(p.x) > 9.5f || fabsf(p.z) > 9.5f || Vector2Distance({p.x, p.z}, {npc.pos.x, npc.pos.z}) < 1;
}

void Move(float dt) {
    Vector3 d = {
        (float)((IsKeyDown(KEY_D) || IsKeyDown(KEY_RIGHT)) - (IsKeyDown(KEY_A) || IsKeyDown(KEY_LEFT))), 0,
        (float)((IsKeyDown(KEY_S) || IsKeyDown(KEY_DOWN))  - (IsKeyDown(KEY_W) || IsKeyDown(KEY_UP)))};
    player.moving = d.x != 0 || d.z != 0;
    if (player.moving) {
        d = Vector3Scale(Vector3Normalize(d), 5 * dt);
        player.yaw = atan2f(d.x, d.z) * RAD2DEG;
        player.walk += dt;
        Vector3 old = player.pos;   // move one axis at a time so you slide along walls
        player.pos.x += d.x; if (Blocked(player.pos)) player.pos.x = old.x;
        player.pos.z += d.z; if (Blocked(player.pos)) player.pos.z = old.z;
    } else player.walk = 0;

    player.velY -= 25 * dt;         // gravity
    player.pos.y += player.velY * dt;
    if (player.pos.y < 0) player.pos.y = player.velY = 0;
}

// ---------- one frame ----------
void Frame() {
    float dt = fminf(GetFrameTime(), 0.05f);
    timer += dt;
    bool blink = (int)(timer * 2) % 2 == 0;

    if (fadeDir) {
        fade += fadeDir * dt * 2.5f;
        if (fade >= 1) { fade = 1; fadeDir = -1; Enter(nextScene); }
        if (fade <= 0) { fade = 0; fadeDir = 0; }
    }

    bool space = fadeDir == 0 && IsKeyPressed(KEY_SPACE);
    bool ground = player.pos.y == 0;
    const char* prompt = nullptr;

    if (scene == MENU) {
        if (space) Go(OUTSIDE);
    } else if (talking) {
        letters += dt * 40;
        if (space) {
            int total = strlen(LINE1) + strlen(LINE2);
            if (letters < total) letters = total; else talking = false;
        }
    } else if (fadeDir == 0) {
        Move(dt);
        Vector3 p = player.pos;
        if (scene == OUTSIDE && ground && Vector3Distance(p, {0, 0, -8.4f}) < 1.5f) {
            prompt = "Press space to enter";
            if (space) { Go(INSIDE); space = false; }
        }
        if (scene == INSIDE && ground && Vector3Distance(p, npc.pos) < 2) {
            npc.yaw = atan2f(p.x - npc.pos.x, p.z - npc.pos.z) * RAD2DEG;
            prompt = "Press space to talk";
            if (space) { talking = true; letters = 0; player.yaw = npc.yaw + 180; player.moving = false; space = false; }
        } else if (scene == INSIDE && ground && Vector3Distance(p, {0, 0, -9}) < 1.5f) {
            prompt = "Press space to exit";
            if (space) { Go(OUTSIDE); space = false; }
        }
        if (space && ground) player.velY = 9;   // jump
    }

    // camera
    Vector3 p = player.pos;
    Camera3D cam = {{0, 2, 6}, {0, 1.2f, 0}, {0, 1, 0}, 45, CAMERA_PERSPECTIVE};
    if (scene == OUTSIDE) cam = {{p.x, 4, p.z + 8}, {p.x, 2 + p.y * 0.3f, p.z - 4}, {0, 1, 0}, 45, CAMERA_PERSPECTIVE};
    if (scene == INSIDE)  cam = {{p.x, 6.5f, p.z + 10}, {p.x, 1.2f + p.y * 0.3f, p.z}, {0, 1, 0}, 45, CAMERA_PERSPECTIVE};

    BeginDrawing();
    ClearBackground({40, 20, 35, 255});
    if (scene != INSIDE) DrawRectangleGradientV(0, 0, W, H, {70, 130, 230, 255}, {255, 170, 210, 255}); // sky

    BeginMode3D(cam);
    if (scene == MENU) {
        Person showcase = {};
        showcase.yaw = timer * 50;
        DrawPerson(showcase, PURPLE_C);
    }
    if (scene == OUTSIDE) {
        DrawPlane({0, 0, 0}, {400, 400}, {90, 190, 90, 255});          // grass
        DrawCube({0, 3, -12}, 6, 6, 6, WHITE);                         // building
        DrawCubeWires({0, 3, -12}, 6, 6, 6, LIGHTGRAY);
        DrawCube({0, 1.5f, -8.95f}, 1.6f, 3, 0.1f, CYAN_C);            // door
        DrawPerson(player, PURPLE_C);
    }
    if (scene == INSIDE) {
        DrawPlane({0, 0, 0}, {20, 20}, {255, 150, 190, 255});          // pink floor
        DrawCube({0, 3, -10}, 20, 6, 0.2f, {255, 210, 228, 255});      // back wall
        DrawCube({-10, 3, 0}, 0.2f, 6, 20, {250, 196, 216, 255});      // side walls
        DrawCube({ 10, 3, 0}, 0.2f, 6, 20, {250, 196, 216, 255});
        DrawCube({0, 1.5f, -9.85f}, 1.6f, 3, 0.1f, CYAN_C);            // exit door
        DrawPerson(npc, YELLOW_C);
        DrawPerson(player, PURPLE_C);
    }
    EndMode3D();

    if (scene == MENU && blink) DrawText("Press space to start", (W - MeasureText("Press space to start", 36)) / 2, H - 120, 36, WHITE);
    if (talking) Dialogue(blink);
    if (prompt) Prompt(prompt);
    DrawRectangle(0, 0, W, H, Fade(BLACK, fade));
    EndDrawing();
}

int main() {
    InitWindow(W, H, "Purple World");
#ifdef PLATFORM_WEB
    emscripten_set_main_loop(Frame, 0, 1);
#else
    SetTargetFPS(60);
    while (!WindowShouldClose()) Frame();
#endif
    CloseWindow();
}