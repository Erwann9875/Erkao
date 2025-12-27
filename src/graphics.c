#include "graphics.h"

#if ERKAO_HAS_GRAPHICS

#include "interpreter_internal.h"
#include "gc.h"

#include <SDL.h>
#include <SDL_image.h>
#include <SDL_ttf.h>
#include <SDL_mixer.h>

#include <string.h>
#include <math.h>

static SDL_Window* gWindow = NULL;
static SDL_Renderer* gRenderer = NULL;
static bool gInitialized = false;
static bool gRunning = false;

static Uint8 gBgR = 0, gBgG = 0, gBgB = 0, gBgA = 255;

static Uint8 gDrawR = 255, gDrawG = 255, gDrawB = 255, gDrawA = 255;

static Uint64 gStartTime = 0;
static Uint64 gLastFrameTime = 0;
static double gDeltaTime = 0.0;
static double gTargetFrameTime = 0.0;
static int gFrameCount = 0;
static double gFpsTimer = 0.0;
static int gCurrentFps = 0;

static const Uint8* gKeyState = NULL;
static Uint8* gKeyPressed = NULL;
static Uint8* gKeyPrevState = NULL;
static int gKeyCount = 0;
static int gMouseX = 0, gMouseY = 0;
static Uint32 gMouseState = 0;
static Uint32 gMousePrevState = 0;
static bool gTextInputEnabled = false;

typedef enum {
  GFX_EVENT_KEY_DOWN,
  GFX_EVENT_KEY_UP,
  GFX_EVENT_TEXT,
  GFX_EVENT_QUIT
} GfxEventType;

typedef struct {
  GfxEventType type;
  SDL_Scancode scancode;
  Uint8 repeat;
  char text[SDL_TEXTINPUTEVENT_TEXT_SIZE];
} GfxEvent;

#define GFX_EVENT_QUEUE_CAPACITY 256

static GfxEvent gEventQueue[GFX_EVENT_QUEUE_CAPACITY];
static int gEventHead = 0;
static int gEventTail = 0;

static TTF_Font* gDefaultFont = NULL;
static int gDefaultFontSize = 16;

#define MAX_CACHED_TEXTURES 256
#define MAX_CACHED_FONTS 32
#define MAX_CACHED_SOUNDS 64

typedef struct {
  char* path;
  SDL_Texture* texture;
  int width;
  int height;
} CachedTexture;

typedef struct {
  char* path;
  int size;
  TTF_Font* font;
} CachedFont;

typedef struct {
  char* path;
  Mix_Chunk* chunk;
} CachedSound;

static CachedTexture gTextures[MAX_CACHED_TEXTURES];
static int gTextureCount = 0;

static CachedFont gFonts[MAX_CACHED_FONTS];
static int gFontCount = 0;

static CachedSound gSounds[MAX_CACHED_SOUNDS];
static int gSoundCount = 0;

static Mix_Music* gCurrentMusic = NULL;

typedef struct {
  const char* name;
  Uint8 r, g, b;
} NamedColor;

static const NamedColor gColors[] = {
  {"black", 0, 0, 0},
  {"white", 255, 255, 255},
  {"red", 255, 0, 0},
  {"green", 0, 255, 0},
  {"blue", 0, 0, 255},
  {"yellow", 255, 255, 0},
  {"cyan", 0, 255, 255},
  {"magenta", 255, 0, 255},
  {"orange", 255, 165, 0},
  {"purple", 128, 0, 128},
  {"pink", 255, 192, 203},
  {"gray", 128, 128, 128},
  {"grey", 128, 128, 128},
  {"darkgray", 64, 64, 64},
  {"darkgrey", 64, 64, 64},
  {"lightgray", 192, 192, 192},
  {"lightgrey", 192, 192, 192},
  {"darkblue", 0, 0, 139},
  {"darkgreen", 0, 100, 0},
  {"darkred", 139, 0, 0},
  {"gold", 255, 215, 0},
  {"lime", 0, 255, 0},
  {"navy", 0, 0, 128},
  {"teal", 0, 128, 128},
  {"maroon", 128, 0, 0},
  {"olive", 128, 128, 0},
  {"aqua", 0, 255, 255},
  {"fuchsia", 255, 0, 255},
  {"silver", 192, 192, 192},
  {"brown", 139, 69, 19},
  {"coral", 255, 127, 80},
  {"crimson", 220, 20, 60},
  {"indigo", 75, 0, 130},
  {"violet", 238, 130, 238},
  {"turquoise", 64, 224, 208},
  {"salmon", 250, 128, 114},
  {"skyblue", 135, 206, 235},
  {NULL, 0, 0, 0}
};

typedef struct {
  const char* name;
  SDL_Scancode code;
} KeyMapping;

static const KeyMapping gKeyMappings[] = {
  {"a", SDL_SCANCODE_A}, {"b", SDL_SCANCODE_B}, {"c", SDL_SCANCODE_C},
  {"d", SDL_SCANCODE_D}, {"e", SDL_SCANCODE_E}, {"f", SDL_SCANCODE_F},
  {"g", SDL_SCANCODE_G}, {"h", SDL_SCANCODE_H}, {"i", SDL_SCANCODE_I},
  {"j", SDL_SCANCODE_J}, {"k", SDL_SCANCODE_K}, {"l", SDL_SCANCODE_L},
  {"m", SDL_SCANCODE_M}, {"n", SDL_SCANCODE_N}, {"o", SDL_SCANCODE_O},
  {"p", SDL_SCANCODE_P}, {"q", SDL_SCANCODE_Q}, {"r", SDL_SCANCODE_R},
  {"s", SDL_SCANCODE_S}, {"t", SDL_SCANCODE_T}, {"u", SDL_SCANCODE_U},
  {"v", SDL_SCANCODE_V}, {"w", SDL_SCANCODE_W}, {"x", SDL_SCANCODE_X},
  {"y", SDL_SCANCODE_Y}, {"z", SDL_SCANCODE_Z},
  {"0", SDL_SCANCODE_0}, {"1", SDL_SCANCODE_1}, {"2", SDL_SCANCODE_2},
  {"3", SDL_SCANCODE_3}, {"4", SDL_SCANCODE_4}, {"5", SDL_SCANCODE_5},
  {"6", SDL_SCANCODE_6}, {"7", SDL_SCANCODE_7}, {"8", SDL_SCANCODE_8},
  {"9", SDL_SCANCODE_9},
  {"up", SDL_SCANCODE_UP}, {"down", SDL_SCANCODE_DOWN},
  {"left", SDL_SCANCODE_LEFT}, {"right", SDL_SCANCODE_RIGHT},
  {"space", SDL_SCANCODE_SPACE}, {"enter", SDL_SCANCODE_RETURN},
  {"return", SDL_SCANCODE_RETURN}, {"escape", SDL_SCANCODE_ESCAPE},
  {"esc", SDL_SCANCODE_ESCAPE}, {"tab", SDL_SCANCODE_TAB},
  {"backspace", SDL_SCANCODE_BACKSPACE}, {"delete", SDL_SCANCODE_DELETE},
  {"insert", SDL_SCANCODE_INSERT}, {"home", SDL_SCANCODE_HOME},
  {"end", SDL_SCANCODE_END}, {"pageup", SDL_SCANCODE_PAGEUP},
  {"pagedown", SDL_SCANCODE_PAGEDOWN},
  {"lshift", SDL_SCANCODE_LSHIFT}, {"rshift", SDL_SCANCODE_RSHIFT},
  {"lctrl", SDL_SCANCODE_LCTRL}, {"rctrl", SDL_SCANCODE_RCTRL},
  {"lalt", SDL_SCANCODE_LALT}, {"ralt", SDL_SCANCODE_RALT},
  {"shift", SDL_SCANCODE_LSHIFT}, {"ctrl", SDL_SCANCODE_LCTRL},
  {"alt", SDL_SCANCODE_LALT},
  {"f1", SDL_SCANCODE_F1}, {"f2", SDL_SCANCODE_F2}, {"f3", SDL_SCANCODE_F3},
  {"f4", SDL_SCANCODE_F4}, {"f5", SDL_SCANCODE_F5}, {"f6", SDL_SCANCODE_F6},
  {"f7", SDL_SCANCODE_F7}, {"f8", SDL_SCANCODE_F8}, {"f9", SDL_SCANCODE_F9},
  {"f10", SDL_SCANCODE_F10}, {"f11", SDL_SCANCODE_F11}, {"f12", SDL_SCANCODE_F12},
  {NULL, 0}
};

static Value gfxError(VM* vm, const char* message) {
  char buf[512];
  const char* sdlErr = SDL_GetError();
  if (sdlErr && *sdlErr) {
    snprintf(buf, sizeof(buf), "%s: %s", message, sdlErr);
  } else {
    snprintf(buf, sizeof(buf), "%s", message);
  }
  Token token;
  memset(&token, 0, sizeof(Token));
  runtimeError(vm, token, buf);
  return NULL_VAL;
}

static bool parseColor(VM* vm, Value value, Uint8* r, Uint8* g, Uint8* b, Uint8* a) {
  (void)vm;
  *a = 255;

  if (isObjType(value, OBJ_STRING)) {
    ObjString* str = (ObjString*)AS_OBJ(value);
    for (int i = 0; gColors[i].name != NULL; i++) {
      if (strcmp(str->chars, gColors[i].name) == 0) {
        *r = gColors[i].r;
        *g = gColors[i].g;
        *b = gColors[i].b;
        return true;
      }
    }
    return false;
  }

  if (isObjType(value, OBJ_ARRAY)) {
    ObjArray* arr = (ObjArray*)AS_OBJ(value);
    if (arr->count >= 3) {
      if (IS_NUMBER(arr->items[0]) && IS_NUMBER(arr->items[1]) && IS_NUMBER(arr->items[2])) {
        *r = (Uint8)AS_NUMBER(arr->items[0]);
        *g = (Uint8)AS_NUMBER(arr->items[1]);
        *b = (Uint8)AS_NUMBER(arr->items[2]);
        if (arr->count >= 4 && IS_NUMBER(arr->items[3])) {
          *a = (Uint8)AS_NUMBER(arr->items[3]);
        }
        return true;
      }
    }
  }

  if (IS_NUMBER(value)) {
    Uint8 gray = (Uint8)AS_NUMBER(value);
    *r = *g = *b = gray;
    return true;
  }

  return false;
}

static SDL_Scancode getKeyCode(const char* name) {
  for (int i = 0; gKeyMappings[i].name != NULL; i++) {
    if (strcmp(name, gKeyMappings[i].name) == 0) {
      return gKeyMappings[i].code;
    }
  }
  return SDL_SCANCODE_UNKNOWN;
}

static const char* getKeyNameFromCode(SDL_Scancode code) {
  for (int i = 0; gKeyMappings[i].name != NULL; i++) {
    if (gKeyMappings[i].code == code) {
      return gKeyMappings[i].name;
    }
  }
  const char* sdlName = SDL_GetScancodeName(code);
  if (!sdlName || sdlName[0] == '\0') return NULL;
  return sdlName;
}

static void clearEventQueue(void) {
  gEventHead = 0;
  gEventTail = 0;
}

static void pushEvent(const GfxEvent* event) {
  int nextTail = (gEventTail + 1) % GFX_EVENT_QUEUE_CAPACITY;
  if (nextTail == gEventHead) {
    gEventHead = (gEventHead + 1) % GFX_EVENT_QUEUE_CAPACITY;
  }
  gEventQueue[gEventTail] = *event;
  gEventTail = nextTail;
}

static bool popEvent(GfxEvent* out) {
  if (gEventHead == gEventTail) return false;
  *out = gEventQueue[gEventHead];
  gEventHead = (gEventHead + 1) % GFX_EVENT_QUEUE_CAPACITY;
  return true;
}

static void queueKeyEvent(GfxEventType type, SDL_Scancode scancode, Uint8 repeat) {
  GfxEvent event;
  event.type = type;
  event.scancode = scancode;
  event.repeat = repeat;
  event.text[0] = '\0';
  pushEvent(&event);
}

static void queueTextEvent(const char* text) {
  GfxEvent event;
  event.type = GFX_EVENT_TEXT;
  event.scancode = SDL_SCANCODE_UNKNOWN;
  event.repeat = 0;
  strncpy(event.text, text ? text : "", SDL_TEXTINPUTEVENT_TEXT_SIZE);
  event.text[SDL_TEXTINPUTEVENT_TEXT_SIZE - 1] = '\0';
  pushEvent(&event);
}

static void queueQuitEvent(void) {
  GfxEvent event;
  event.type = GFX_EVENT_QUIT;
  event.scancode = SDL_SCANCODE_UNKNOWN;
  event.repeat = 0;
  event.text[0] = '\0';
  pushEvent(&event);
}

static Value gfxEventToValue(VM* vm, const GfxEvent* event) {
  ObjMap* result = newMap(vm);
  const char* typeName = "unknown";

  switch (event->type) {
    case GFX_EVENT_KEY_DOWN:
      typeName = "keyDown";
      break;
    case GFX_EVENT_KEY_UP:
      typeName = "keyUp";
      break;
    case GFX_EVENT_TEXT:
      typeName = "text";
      break;
    case GFX_EVENT_QUIT:
      typeName = "quit";
      break;
  }

  mapSet(result, copyString(vm, "type"), OBJ_VAL(copyString(vm, typeName)));

  if (event->type == GFX_EVENT_KEY_DOWN || event->type == GFX_EVENT_KEY_UP) {
    mapSet(result, copyString(vm, "scancode"), NUMBER_VAL((double)event->scancode));
    const char* keyName = getKeyNameFromCode(event->scancode);
    if (keyName) {
      mapSet(result, copyString(vm, "key"), OBJ_VAL(copyString(vm, keyName)));
    }
    if (event->type == GFX_EVENT_KEY_DOWN) {
      mapSet(result, copyString(vm, "repeat"), BOOL_VAL(event->repeat != 0));
    }
  }

  if (event->type == GFX_EVENT_TEXT) {
    mapSet(result, copyString(vm, "text"), OBJ_VAL(copyString(vm, event->text)));
  }

  return OBJ_VAL(result);
}

static CachedTexture* getTexture(VM* vm, const char* path) {
  (void)vm;
  for (int i = 0; i < gTextureCount; i++) {
    if (strcmp(gTextures[i].path, path) == 0) {
      return &gTextures[i];
    }
  }

  if (gTextureCount >= MAX_CACHED_TEXTURES) {
    return NULL;
  }

  SDL_Surface* surface = IMG_Load(path);
  if (!surface) return NULL;

  SDL_Texture* texture = SDL_CreateTextureFromSurface(gRenderer, surface);
  int w = surface->w;
  int h = surface->h;
  SDL_FreeSurface(surface);

  if (!texture) return NULL;

  CachedTexture* cached = &gTextures[gTextureCount++];
  cached->path = strdup(path);
  cached->texture = texture;
  cached->width = w;
  cached->height = h;
  return cached;
}

static TTF_Font* getFont(const char* path, int size) {
  for (int i = 0; i < gFontCount; i++) {
    if (strcmp(gFonts[i].path, path) == 0 && gFonts[i].size == size) {
      return gFonts[i].font;
    }
  }

  if (gFontCount >= MAX_CACHED_FONTS) return NULL;

  TTF_Font* font = TTF_OpenFont(path, size);
  if (!font) return NULL;

  CachedFont* cached = &gFonts[gFontCount++];
  cached->path = strdup(path);
  cached->size = size;
  cached->font = font;
  return font;
}

static Mix_Chunk* getSound(const char* path) {
  for (int i = 0; i < gSoundCount; i++) {
    if (strcmp(gSounds[i].path, path) == 0) {
      return gSounds[i].chunk;
    }
  }

  if (gSoundCount >= MAX_CACHED_SOUNDS) return NULL;

  Mix_Chunk* chunk = Mix_LoadWAV(path);
  if (!chunk) return NULL;

  CachedSound* cached = &gSounds[gSoundCount++];
  cached->path = strdup(path);
  cached->chunk = chunk;
  return chunk;
}

static void updateInput(void) {
  SDL_PumpEvents();

  gMousePrevState = gMouseState;
  gMouseState = SDL_GetMouseState(&gMouseX, &gMouseY);

  int numKeys = 0;
  const Uint8* sdlKeyState = SDL_GetKeyboardState(&numKeys);
  
  if (numKeys <= 0) {
    gKeyState = NULL;
    gKeyCount = 0;
    return;
  }

  if (!gKeyPressed || !gKeyPrevState || numKeys != gKeyCount) {
    if (gKeyPressed) free(gKeyPressed);
    if (gKeyPrevState) free(gKeyPrevState);
    gKeyCount = numKeys;
    gKeyPressed = (Uint8*)calloc(numKeys, sizeof(Uint8));
    gKeyPrevState = (Uint8*)calloc(numKeys, sizeof(Uint8));
    if (!gKeyPressed || !gKeyPrevState) return;
  }

  for (int i = 0; i < gKeyCount; i++) {
    Uint8 current = sdlKeyState[i];
    gKeyPressed[i] = current && !gKeyPrevState[i];
    gKeyPrevState[i] = current;
  }

  gKeyState = sdlKeyState;
}

static bool processEvents(void) {
  SDL_Event event;
  bool keepRunning = true;
  while (SDL_PollEvent(&event)) {
    switch (event.type) {
      case SDL_QUIT:
        gRunning = false;
        keepRunning = false;
        queueQuitEvent();
        break;
      case SDL_KEYDOWN:
        if (event.key.keysym.scancode == SDL_SCANCODE_ESCAPE) {
          gRunning = false;
          keepRunning = false;
          queueQuitEvent();
          break;
        }
        queueKeyEvent(GFX_EVENT_KEY_DOWN, event.key.keysym.scancode,
                      event.key.repeat ? 1 : 0);
        break;
      case SDL_KEYUP:
        queueKeyEvent(GFX_EVENT_KEY_UP, event.key.keysym.scancode, 0);
        break;
      case SDL_TEXTINPUT:
        queueTextEvent(event.text.text);
        break;
    }
  }
  return keepRunning;
}

static Value nativeGfxInit(VM* vm, int argc, Value* args) {
  if (gInitialized) {
    return gfxError(vm, "gfx.init already called");
  }

  if (!IS_NUMBER(args[0]) || !IS_NUMBER(args[1])) {
    return gfxError(vm, "gfx.init expects (width, height, title?)");
  }

  int width = (int)AS_NUMBER(args[0]);
  int height = (int)AS_NUMBER(args[1]);
  const char* title = "Erkao";

  if (argc >= 3 && isObjType(args[2], OBJ_STRING)) {
    title = ((ObjString*)AS_OBJ(args[2]))->chars;
  }

  if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) < 0) {
    return gfxError(vm, "Failed to initialize SDL");
  }

  if (IMG_Init(IMG_INIT_PNG | IMG_INIT_JPG) == 0) {
    SDL_Quit();
    return gfxError(vm, "Failed to initialize SDL_image");
  }

  if (TTF_Init() < 0) {
    IMG_Quit();
    SDL_Quit();
    return gfxError(vm, "Failed to initialize SDL_ttf");
  }

  if (Mix_OpenAudio(44100, MIX_DEFAULT_FORMAT, 2, 2048) < 0) {
    TTF_Quit();
    IMG_Quit();
    SDL_Quit();
    return gfxError(vm, "Failed to initialize SDL_mixer");
  }

  gWindow = SDL_CreateWindow(
    title,
    SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
    width, height,
    SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE
  );

  if (!gWindow) {
    Mix_CloseAudio();
    TTF_Quit();
    IMG_Quit();
    SDL_Quit();
    return gfxError(vm, "Failed to create window");
  }

  gRenderer = SDL_CreateRenderer(gWindow, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
  if (!gRenderer) {
    SDL_DestroyWindow(gWindow);
    Mix_CloseAudio();
    TTF_Quit();
    IMG_Quit();
    SDL_Quit();
    return gfxError(vm, "Failed to create renderer");
  }

  SDL_SetRenderDrawBlendMode(gRenderer, SDL_BLENDMODE_BLEND);

  clearEventQueue();
  SDL_StartTextInput();
  gTextInputEnabled = true;

  gInitialized = true;
  gRunning = true;
  gStartTime = SDL_GetPerformanceCounter();
  gLastFrameTime = gStartTime;

  return BOOL_VAL(true);
}

static Value nativeGfxQuit(VM* vm, int argc, Value* args) {
  (void)vm; (void)argc; (void)args;
  graphicsCleanup();
  return NULL_VAL;
}

static Value nativeGfxClear(VM* vm, int argc, Value* args) {
  if (!gRenderer) return gfxError(vm, "gfx.init not called");

  if (argc >= 1) {
    Uint8 r, g, b, a;
    if (parseColor(vm, args[0], &r, &g, &b, &a)) {
      gBgR = r; gBgG = g; gBgB = b; gBgA = a;
    }
  }

  SDL_SetRenderDrawColor(gRenderer, gBgR, gBgG, gBgB, gBgA);
  SDL_RenderClear(gRenderer);
  SDL_SetRenderDrawColor(gRenderer, gDrawR, gDrawG, gDrawB, gDrawA);
  return NULL_VAL;
}

static Value nativeGfxPresent(VM* vm, int argc, Value* args) {
  (void)vm; (void)argc; (void)args;
  if (!gRenderer) return NULL_VAL;

  SDL_RenderPresent(gRenderer);

  Uint64 now = SDL_GetPerformanceCounter();
  double freq = (double)SDL_GetPerformanceFrequency();
  gDeltaTime = (double)(now - gLastFrameTime) / freq;
  gLastFrameTime = now;

  if (gTargetFrameTime > 0 && gDeltaTime < gTargetFrameTime) {
    SDL_Delay((Uint32)((gTargetFrameTime - gDeltaTime) * 1000.0));
  }

  gFrameCount++;
  gFpsTimer += gDeltaTime;
  if (gFpsTimer >= 1.0) {
    gCurrentFps = gFrameCount;
    gFrameCount = 0;
    gFpsTimer = 0.0;
  }

  return NULL_VAL;
}

static Value nativeGfxRect(VM* vm, int argc, Value* args) {
  if (!gRenderer) return gfxError(vm, "gfx.init not called");
  if (argc < 5) return gfxError(vm, "gfx.rect expects (color, x, y, w, h)");

  Uint8 r, g, b, a;
  if (!parseColor(vm, args[0], &r, &g, &b, &a)) {
    return gfxError(vm, "Invalid color");
  }

  SDL_Rect rect = {
    (int)AS_NUMBER(args[1]),
    (int)AS_NUMBER(args[2]),
    (int)AS_NUMBER(args[3]),
    (int)AS_NUMBER(args[4])
  };

  SDL_SetRenderDrawColor(gRenderer, r, g, b, a);
  SDL_RenderFillRect(gRenderer, &rect);
  SDL_SetRenderDrawColor(gRenderer, gDrawR, gDrawG, gDrawB, gDrawA);
  return NULL_VAL;
}

static Value nativeGfxRectLine(VM* vm, int argc, Value* args) {
  if (!gRenderer) return gfxError(vm, "gfx.init not called");
  if (argc < 5) return gfxError(vm, "gfx.rectLine expects (color, x, y, w, h)");

  Uint8 r, g, b, a;
  if (!parseColor(vm, args[0], &r, &g, &b, &a)) {
    return gfxError(vm, "Invalid color");
  }

  SDL_Rect rect = {
    (int)AS_NUMBER(args[1]),
    (int)AS_NUMBER(args[2]),
    (int)AS_NUMBER(args[3]),
    (int)AS_NUMBER(args[4])
  };

  SDL_SetRenderDrawColor(gRenderer, r, g, b, a);
  SDL_RenderDrawRect(gRenderer, &rect);
  SDL_SetRenderDrawColor(gRenderer, gDrawR, gDrawG, gDrawB, gDrawA);
  return NULL_VAL;
}

static void drawCirclePoints(int cx, int cy, int x, int y) {
  SDL_RenderDrawPoint(gRenderer, cx + x, cy + y);
  SDL_RenderDrawPoint(gRenderer, cx - x, cy + y);
  SDL_RenderDrawPoint(gRenderer, cx + x, cy - y);
  SDL_RenderDrawPoint(gRenderer, cx - x, cy - y);
  SDL_RenderDrawPoint(gRenderer, cx + y, cy + x);
  SDL_RenderDrawPoint(gRenderer, cx - y, cy + x);
  SDL_RenderDrawPoint(gRenderer, cx + y, cy - x);
  SDL_RenderDrawPoint(gRenderer, cx - y, cy - x);
}

static void fillCircleLines(int cx, int cy, int x, int y) {
  SDL_RenderDrawLine(gRenderer, cx - x, cy + y, cx + x, cy + y);
  SDL_RenderDrawLine(gRenderer, cx - x, cy - y, cx + x, cy - y);
  SDL_RenderDrawLine(gRenderer, cx - y, cy + x, cx + y, cy + x);
  SDL_RenderDrawLine(gRenderer, cx - y, cy - x, cx + y, cy - x);
}

static Value nativeGfxCircle(VM* vm, int argc, Value* args) {
  if (!gRenderer) return gfxError(vm, "gfx.init not called");
  if (argc < 4) return gfxError(vm, "gfx.circle expects (color, x, y, radius)");

  Uint8 r, g, b, a;
  if (!parseColor(vm, args[0], &r, &g, &b, &a)) {
    return gfxError(vm, "Invalid color");
  }

  int cx = (int)AS_NUMBER(args[1]);
  int cy = (int)AS_NUMBER(args[2]);
  int radius = (int)AS_NUMBER(args[3]);

  SDL_SetRenderDrawColor(gRenderer, r, g, b, a);

  int x = 0, y = radius;
  int d = 1 - radius;
  while (x <= y) {
    fillCircleLines(cx, cy, x, y);
    if (d < 0) {
      d += 2 * x + 3;
    } else {
      d += 2 * (x - y) + 5;
      y--;
    }
    x++;
  }

  SDL_SetRenderDrawColor(gRenderer, gDrawR, gDrawG, gDrawB, gDrawA);
  return NULL_VAL;
}

static Value nativeGfxCircleLine(VM* vm, int argc, Value* args) {
  if (!gRenderer) return gfxError(vm, "gfx.init not called");
  if (argc < 4) return gfxError(vm, "gfx.circleLine expects (color, x, y, radius)");

  Uint8 r, g, b, a;
  if (!parseColor(vm, args[0], &r, &g, &b, &a)) {
    return gfxError(vm, "Invalid color");
  }

  int cx = (int)AS_NUMBER(args[1]);
  int cy = (int)AS_NUMBER(args[2]);
  int radius = (int)AS_NUMBER(args[3]);

  SDL_SetRenderDrawColor(gRenderer, r, g, b, a);

  int x = 0, y = radius;
  int d = 1 - radius;
  while (x <= y) {
    drawCirclePoints(cx, cy, x, y);
    if (d < 0) {
      d += 2 * x + 3;
    } else {
      d += 2 * (x - y) + 5;
      y--;
    }
    x++;
  }

  SDL_SetRenderDrawColor(gRenderer, gDrawR, gDrawG, gDrawB, gDrawA);
  return NULL_VAL;
}

static Value nativeGfxLine(VM* vm, int argc, Value* args) {
  if (!gRenderer) return gfxError(vm, "gfx.init not called");
  if (argc < 5) return gfxError(vm, "gfx.line expects (color, x1, y1, x2, y2)");

  Uint8 r, g, b, a;
  if (!parseColor(vm, args[0], &r, &g, &b, &a)) {
    return gfxError(vm, "Invalid color");
  }

  SDL_SetRenderDrawColor(gRenderer, r, g, b, a);
  SDL_RenderDrawLine(gRenderer,
    (int)AS_NUMBER(args[1]), (int)AS_NUMBER(args[2]),
    (int)AS_NUMBER(args[3]), (int)AS_NUMBER(args[4]));
  SDL_SetRenderDrawColor(gRenderer, gDrawR, gDrawG, gDrawB, gDrawA);
  return NULL_VAL;
}

static Value nativeGfxPixel(VM* vm, int argc, Value* args) {
  if (!gRenderer) return gfxError(vm, "gfx.init not called");
  if (argc < 3) return gfxError(vm, "gfx.pixel expects (color, x, y)");

  Uint8 r, g, b, a;
  if (!parseColor(vm, args[0], &r, &g, &b, &a)) {
    return gfxError(vm, "Invalid color");
  }

  SDL_SetRenderDrawColor(gRenderer, r, g, b, a);
  SDL_RenderDrawPoint(gRenderer, (int)AS_NUMBER(args[1]), (int)AS_NUMBER(args[2]));
  SDL_SetRenderDrawColor(gRenderer, gDrawR, gDrawG, gDrawB, gDrawA);
  return NULL_VAL;
}

static Value nativeGfxImage(VM* vm, int argc, Value* args) {
  if (!gRenderer) return gfxError(vm, "gfx.init not called");
  if (argc < 3) return gfxError(vm, "gfx.image expects (path, x, y, scale?)");
  if (!isObjType(args[0], OBJ_STRING)) return gfxError(vm, "gfx.image path must be string");

  ObjString* path = (ObjString*)AS_OBJ(args[0]);
  CachedTexture* tex = getTexture(vm, path->chars);
  if (!tex) return gfxError(vm, "Failed to load image");

  int x = (int)AS_NUMBER(args[1]);
  int y = (int)AS_NUMBER(args[2]);
  double scale = (argc >= 4 && IS_NUMBER(args[3])) ? AS_NUMBER(args[3]) : 1.0;

  SDL_Rect dst = {x, y, (int)(tex->width * scale), (int)(tex->height * scale)};
  SDL_RenderCopy(gRenderer, tex->texture, NULL, &dst);
  return NULL_VAL;
}

static Value nativeGfxImageEx(VM* vm, int argc, Value* args) {
  if (!gRenderer) return gfxError(vm, "gfx.init not called");
  if (argc < 6) return gfxError(vm, "gfx.imageEx expects (path, x, y, angle, scaleX, scaleY)");
  if (!isObjType(args[0], OBJ_STRING)) return gfxError(vm, "path must be string");

  ObjString* path = (ObjString*)AS_OBJ(args[0]);
  CachedTexture* tex = getTexture(vm, path->chars);
  if (!tex) return gfxError(vm, "Failed to load image");

  int x = (int)AS_NUMBER(args[1]);
  int y = (int)AS_NUMBER(args[2]);
  double angle = AS_NUMBER(args[3]);
  double scaleX = AS_NUMBER(args[4]);
  double scaleY = AS_NUMBER(args[5]);

  SDL_Rect dst = {x, y, (int)(tex->width * scaleX), (int)(tex->height * scaleY)};
  SDL_RenderCopyEx(gRenderer, tex->texture, NULL, &dst, angle, NULL, SDL_FLIP_NONE);
  return NULL_VAL;
}

static Value nativeGfxImageSize(VM* vm, int argc, Value* args) {
  if (argc < 1 || !isObjType(args[0], OBJ_STRING)) {
    return gfxError(vm, "gfx.imageSize expects (path)");
  }

  ObjString* path = (ObjString*)AS_OBJ(args[0]);
  CachedTexture* tex = getTexture(vm, path->chars);
  if (!tex) return gfxError(vm, "Failed to load image");

  ObjMap* result = newMap(vm);
  ObjString* wKey = copyString(vm, "w");
  ObjString* hKey = copyString(vm, "h");
  mapSet(result, wKey, NUMBER_VAL(tex->width));
  mapSet(result, hKey, NUMBER_VAL(tex->height));
  return OBJ_VAL(result);
}

static Value nativeGfxText(VM* vm, int argc, Value* args) {
  if (!gRenderer) return gfxError(vm, "gfx.init not called");
  if (argc < 3) return gfxError(vm, "gfx.text expects (str, x, y, color?, size?)");
  if (!isObjType(args[0], OBJ_STRING)) return gfxError(vm, "text must be string");

  ObjString* text = (ObjString*)AS_OBJ(args[0]);
  int x = (int)AS_NUMBER(args[1]);
  int y = (int)AS_NUMBER(args[2]);

  Uint8 r = 255, g = 255, b = 255, a = 255;
  if (argc >= 4 && !IS_NULL(args[3])) {
    parseColor(vm, args[3], &r, &g, &b, &a);
  }

  int size = gDefaultFontSize;
  if (argc >= 5 && IS_NUMBER(args[4])) {
    size = (int)AS_NUMBER(args[4]);
  }

  if (!gDefaultFont || size != gDefaultFontSize) {
    const char* fontPaths[] = {
      "C:/Windows/Fonts/arial.ttf",
      "C:/Windows/Fonts/segoeui.ttf",
      "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
      "/usr/share/fonts/TTF/DejaVuSans.ttf",
      "/Library/Fonts/Arial.ttf",
      "/System/Library/Fonts/Helvetica.ttc",
      NULL
    };

    for (int i = 0; fontPaths[i] != NULL; i++) {
      TTF_Font* font = TTF_OpenFont(fontPaths[i], size);
      if (font) {
        if (gDefaultFont) TTF_CloseFont(gDefaultFont);
        gDefaultFont = font;
        gDefaultFontSize = size;
        break;
      }
    }

    if (!gDefaultFont) {
      return gfxError(vm, "No system font found. Use gfx.font() to load a custom font.");
    }
  }

  SDL_Color color = {r, g, b, a};
  SDL_Surface* surface = TTF_RenderUTF8_Blended(gDefaultFont, text->chars, color);
  if (!surface) return gfxError(vm, "Failed to render text");

  SDL_Texture* texture = SDL_CreateTextureFromSurface(gRenderer, surface);
  SDL_Rect dst = {x, y, surface->w, surface->h};
  SDL_FreeSurface(surface);

  if (texture) {
    SDL_RenderCopy(gRenderer, texture, NULL, &dst);
    SDL_DestroyTexture(texture);
  }

  return NULL_VAL;
}

static Value nativeGfxTextSize(VM* vm, int argc, Value* args) {
  if (argc < 1 || !isObjType(args[0], OBJ_STRING)) {
    return gfxError(vm, "gfx.textSize expects (str, size?)");
  }

  ObjString* text = (ObjString*)AS_OBJ(args[0]);
  (void)argc;

  if (!gDefaultFont) {
    return gfxError(vm, "No font loaded");
  }

  int w, h;
  TTF_SizeUTF8(gDefaultFont, text->chars, &w, &h);

  ObjMap* result = newMap(vm);
  mapSet(result, copyString(vm, "w"), NUMBER_VAL(w));
  mapSet(result, copyString(vm, "h"), NUMBER_VAL(h));
  return OBJ_VAL(result);
}

static Value nativeGfxKey(VM* vm, int argc, Value* args) {
  (void)vm;
  if (argc < 1 || !isObjType(args[0], OBJ_STRING)) {
    return BOOL_VAL(false);
  }

  ObjString* keyName = (ObjString*)AS_OBJ(args[0]);
  SDL_Scancode code = getKeyCode(keyName->chars);
  if (code == SDL_SCANCODE_UNKNOWN) return BOOL_VAL(false);

  int index = (int)code;
  if (!gKeyState || index < 0 || index >= gKeyCount) {
    return BOOL_VAL(false);
  }
  return BOOL_VAL(gKeyState[index]);
}

static Value nativeGfxKeyPressed(VM* vm, int argc, Value* args) {
  (void)vm;
  if (argc < 1 || !isObjType(args[0], OBJ_STRING)) {
    return BOOL_VAL(false);
  }

  ObjString* keyName = (ObjString*)AS_OBJ(args[0]);
  SDL_Scancode code = getKeyCode(keyName->chars);
  if (code == SDL_SCANCODE_UNKNOWN) return BOOL_VAL(false);

  int index = (int)code;
  if (!gKeyPressed || index < 0 || index >= gKeyCount) {
    return BOOL_VAL(false);
  }
  return BOOL_VAL(gKeyPressed[index]);
}

static Value nativeGfxMouse(VM* vm, int argc, Value* args) {
  (void)argc; (void)args;
  ObjMap* result = newMap(vm);
  mapSet(result, copyString(vm, "x"), NUMBER_VAL(gMouseX));
  mapSet(result, copyString(vm, "y"), NUMBER_VAL(gMouseY));
  return OBJ_VAL(result);
}

static Value nativeGfxMouseDown(VM* vm, int argc, Value* args) {
  (void)vm;
  int button = (argc >= 1 && IS_NUMBER(args[0])) ? (int)AS_NUMBER(args[0]) : 1;
  Uint32 mask = SDL_BUTTON(button);
  return BOOL_VAL((gMouseState & mask) != 0);
}

static Value nativeGfxMouseClicked(VM* vm, int argc, Value* args) {
  (void)vm;
  int button = (argc >= 1 && IS_NUMBER(args[0])) ? (int)AS_NUMBER(args[0]) : 1;
  Uint32 mask = SDL_BUTTON(button);
  return BOOL_VAL((gMouseState & mask) && !(gMousePrevState & mask));
}

static Value nativeGfxRun(VM* vm, int argc, Value* args) {
  if (!gInitialized) return gfxError(vm, "gfx.init not called");

  Value updateFn = (argc >= 1 && !IS_NULL(args[0])) ? args[0] : NULL_VAL;
  Value drawFn = (argc >= 2 && !IS_NULL(args[1])) ? args[1] : NULL_VAL;

  gRunning = true;
  gLastFrameTime = SDL_GetPerformanceCounter();

  while (gRunning) {
    updateInput();
    if (!processEvents()) break;

    Uint64 now = SDL_GetPerformanceCounter();
    double freq = (double)SDL_GetPerformanceFrequency();
    gDeltaTime = (double)(now - gLastFrameTime) / freq;
    gLastFrameTime = now;

    if (!IS_NULL(updateFn)) {
      Value dt = NUMBER_VAL(gDeltaTime);
      Value result;
      if (!vmCallValue(vm, updateFn, 1, &dt, &result)) {
        return NULL_VAL;
      }
    }

    if (!IS_NULL(drawFn)) {
      Value result;
      if (!vmCallValue(vm, drawFn, 0, NULL, &result)) {
        return NULL_VAL;
      }
    }

    if (!gRunning || !gRenderer) break;

    SDL_RenderPresent(gRenderer);

    if (gTargetFrameTime > 0) {
      now = SDL_GetPerformanceCounter();
      double elapsed = (double)(now - gLastFrameTime) / freq;
      if (elapsed < gTargetFrameTime) {
        SDL_Delay((Uint32)((gTargetFrameTime - elapsed) * 1000.0));
      }
    }

    gFrameCount++;
    gFpsTimer += gDeltaTime;
    if (gFpsTimer >= 1.0) {
      gCurrentFps = gFrameCount;
      gFrameCount = 0;
      gFpsTimer = 0.0;
    }
  }

  return NULL_VAL;
}

static Value nativeGfxPoll(VM* vm, int argc, Value* args) {
  (void)vm; (void)argc; (void)args;
  if (!gInitialized) return BOOL_VAL(false);
  updateInput();
  return BOOL_VAL(processEvents());
}

static Value nativeGfxPollEvent(VM* vm, int argc, Value* args) {
  (void)argc; (void)args;
  if (!gInitialized) return NULL_VAL;

  if (gEventHead == gEventTail) {
    updateInput();
    processEvents();
  }

  GfxEvent event;
  if (!popEvent(&event)) return NULL_VAL;
  return gfxEventToValue(vm, &event);
}

static Value nativeGfxTextInput(VM* vm, int argc, Value* args) {
  (void)vm;
  if (!gInitialized) return BOOL_VAL(false);

  if (argc >= 1) {
    if (!IS_BOOL(args[0])) {
      return gfxError(vm, "gfx.textInput expects (bool?)");
    }
    bool enable = AS_BOOL(args[0]);
    if (enable && !gTextInputEnabled) {
      SDL_StartTextInput();
      gTextInputEnabled = true;
    } else if (!enable && gTextInputEnabled) {
      SDL_StopTextInput();
      gTextInputEnabled = false;
    }
    return NULL_VAL;
  }

  return BOOL_VAL(gTextInputEnabled);
}

static Value nativeGfxSound(VM* vm, int argc, Value* args) {
  if (argc < 1 || !isObjType(args[0], OBJ_STRING)) {
    return gfxError(vm, "gfx.sound expects (path)");
  }

  ObjString* path = (ObjString*)AS_OBJ(args[0]);
  Mix_Chunk* chunk = getSound(path->chars);
  if (!chunk) return gfxError(vm, "Failed to load sound");

  Mix_PlayChannel(-1, chunk, 0);
  return NULL_VAL;
}

static Value nativeGfxMusic(VM* vm, int argc, Value* args) {
  if (argc < 1 || !isObjType(args[0], OBJ_STRING)) {
    return gfxError(vm, "gfx.music expects (path, loop?)");
  }

  ObjString* path = (ObjString*)AS_OBJ(args[0]);
  bool loop = (argc < 2 || IS_NULL(args[1])) ? true : AS_BOOL(args[1]);

  if (gCurrentMusic) {
    Mix_FreeMusic(gCurrentMusic);
    gCurrentMusic = NULL;
  }

  gCurrentMusic = Mix_LoadMUS(path->chars);
  if (!gCurrentMusic) return gfxError(vm, "Failed to load music");

  Mix_PlayMusic(gCurrentMusic, loop ? -1 : 1);
  return NULL_VAL;
}

static Value nativeGfxStopMusic(VM* vm, int argc, Value* args) {
  (void)vm; (void)argc; (void)args;
  Mix_HaltMusic();
  return NULL_VAL;
}

static Value nativeGfxVolume(VM* vm, int argc, Value* args) {
  (void)vm;
  if (argc >= 1 && IS_NUMBER(args[0])) {
    int vol = (int)(AS_NUMBER(args[0]) * 128.0 / 100.0);
    if (vol < 0) vol = 0;
    if (vol > 128) vol = 128;
    Mix_Volume(-1, vol);
    Mix_VolumeMusic(vol);
  }
  return NULL_VAL;
}

static Value nativeGfxTime(VM* vm, int argc, Value* args) {
  (void)vm; (void)argc; (void)args;
  Uint64 now = SDL_GetPerformanceCounter();
  double freq = (double)SDL_GetPerformanceFrequency();
  return NUMBER_VAL((double)(now - gStartTime) / freq);
}

static Value nativeGfxFps(VM* vm, int argc, Value* args) {
  (void)vm;
  if (argc >= 1 && IS_NUMBER(args[0])) {
    int target = (int)AS_NUMBER(args[0]);
    gTargetFrameTime = target > 0 ? (1.0 / (double)target) : 0.0;
    return NULL_VAL;
  }
  return NUMBER_VAL(gCurrentFps);
}

static Value nativeGfxSize(VM* vm, int argc, Value* args) {
  (void)argc; (void)args;
  int w = 0, h = 0;
  if (gWindow) {
    SDL_GetWindowSize(gWindow, &w, &h);
  }
  ObjMap* result = newMap(vm);
  mapSet(result, copyString(vm, "w"), NUMBER_VAL(w));
  mapSet(result, copyString(vm, "h"), NUMBER_VAL(h));
  return OBJ_VAL(result);
}

static Value nativeGfxFullscreen(VM* vm, int argc, Value* args) {
  (void)vm;
  if (!gWindow) return NULL_VAL;

  bool enable = true;
  if (argc >= 1 && IS_BOOL(args[0])) {
    enable = AS_BOOL(args[0]);
  }

  SDL_SetWindowFullscreen(gWindow, enable ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
  return NULL_VAL;
}

static Value nativeGfxTitle(VM* vm, int argc, Value* args) {
  (void)vm;
  if (!gWindow) return NULL_VAL;
  if (argc >= 1 && isObjType(args[0], OBJ_STRING)) {
    ObjString* title = (ObjString*)AS_OBJ(args[0]);
    SDL_SetWindowTitle(gWindow, title->chars);
  }
  return NULL_VAL;
}

void defineGraphicsModule(VM* vm,
                          ObjInstance* (*makeModuleFn)(VM*, const char*),
                          void (*moduleAddFn)(VM*, ObjInstance*, const char*, NativeFn, int),
                          void (*defineGlobalFn)(VM*, const char*, Value)) {
  ObjInstance* gfx = makeModuleFn(vm, "gfx");

  moduleAddFn(vm, gfx, "init", nativeGfxInit, -1);
  moduleAddFn(vm, gfx, "quit", nativeGfxQuit, 0);
  moduleAddFn(vm, gfx, "run", nativeGfxRun, -1);
  moduleAddFn(vm, gfx, "poll", nativeGfxPoll, 0);
  moduleAddFn(vm, gfx, "pollEvent", nativeGfxPollEvent, 0);

  moduleAddFn(vm, gfx, "clear", nativeGfxClear, -1);
  moduleAddFn(vm, gfx, "present", nativeGfxPresent, 0);
  moduleAddFn(vm, gfx, "rect", nativeGfxRect, 5);
  moduleAddFn(vm, gfx, "rectLine", nativeGfxRectLine, 5);
  moduleAddFn(vm, gfx, "circle", nativeGfxCircle, 4);
  moduleAddFn(vm, gfx, "circleLine", nativeGfxCircleLine, 4);
  moduleAddFn(vm, gfx, "line", nativeGfxLine, 5);
  moduleAddFn(vm, gfx, "pixel", nativeGfxPixel, 3);

  moduleAddFn(vm, gfx, "image", nativeGfxImage, -1);
  moduleAddFn(vm, gfx, "imageEx", nativeGfxImageEx, 6);
  moduleAddFn(vm, gfx, "imageSize", nativeGfxImageSize, 1);

  moduleAddFn(vm, gfx, "text", nativeGfxText, -1);
  moduleAddFn(vm, gfx, "textSize", nativeGfxTextSize, -1);

  moduleAddFn(vm, gfx, "key", nativeGfxKey, 1);
  moduleAddFn(vm, gfx, "keyPressed", nativeGfxKeyPressed, 1);
  moduleAddFn(vm, gfx, "textInput", nativeGfxTextInput, -1);
  moduleAddFn(vm, gfx, "mouse", nativeGfxMouse, 0);
  moduleAddFn(vm, gfx, "mouseDown", nativeGfxMouseDown, -1);
  moduleAddFn(vm, gfx, "mouseClicked", nativeGfxMouseClicked, -1);

  moduleAddFn(vm, gfx, "sound", nativeGfxSound, 1);
  moduleAddFn(vm, gfx, "music", nativeGfxMusic, -1);
  moduleAddFn(vm, gfx, "stopMusic", nativeGfxStopMusic, 0);
  moduleAddFn(vm, gfx, "volume", nativeGfxVolume, 1);

  moduleAddFn(vm, gfx, "time", nativeGfxTime, 0);
  moduleAddFn(vm, gfx, "fps", nativeGfxFps, -1);
  moduleAddFn(vm, gfx, "size", nativeGfxSize, 0);
  moduleAddFn(vm, gfx, "fullscreen", nativeGfxFullscreen, -1);
  moduleAddFn(vm, gfx, "title", nativeGfxTitle, 1);

  defineGlobalFn(vm, "gfx", OBJ_VAL(gfx));
}

void graphicsCleanup(void) {
  for (int i = 0; i < gTextureCount; i++) {
    if (gTextures[i].texture) SDL_DestroyTexture(gTextures[i].texture);
    if (gTextures[i].path) free(gTextures[i].path);
  }
  gTextureCount = 0;

  for (int i = 0; i < gFontCount; i++) {
    if (gFonts[i].font) TTF_CloseFont(gFonts[i].font);
    if (gFonts[i].path) free(gFonts[i].path);
  }
  gFontCount = 0;

  for (int i = 0; i < gSoundCount; i++) {
    if (gSounds[i].chunk) Mix_FreeChunk(gSounds[i].chunk);
    if (gSounds[i].path) free(gSounds[i].path);
  }
  gSoundCount = 0;

  if (gCurrentMusic) {
    Mix_FreeMusic(gCurrentMusic);
    gCurrentMusic = NULL;
  }

  if (gDefaultFont) {
    TTF_CloseFont(gDefaultFont);
    gDefaultFont = NULL;
  }

  if (gKeyPressed) { free(gKeyPressed); gKeyPressed = NULL; }
  if (gKeyPrevState) { free(gKeyPrevState); gKeyPrevState = NULL; }
  gKeyState = NULL;
  gKeyCount = 0;

  if (gTextInputEnabled) {
    SDL_StopTextInput();
    gTextInputEnabled = false;
  }
  clearEventQueue();

  if (gRenderer) { SDL_DestroyRenderer(gRenderer); gRenderer = NULL; }
  if (gWindow) { SDL_DestroyWindow(gWindow); gWindow = NULL; }

  Mix_CloseAudio();
  TTF_Quit();
  IMG_Quit();
  SDL_Quit();

  gInitialized = false;
  gRunning = false;
}

#endif // ERKAO_HAS_GRAPHICS
