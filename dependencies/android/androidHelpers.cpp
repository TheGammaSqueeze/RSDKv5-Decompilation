#include <RSDK/Core/RetroEngine.hpp>
#include <main.hpp>
#include <pthread.h>
#include <sched.h>
// NEW: Oboe backend lifecycle hooks
#include <RSDK/Audio/Oboe/OboeAudioDevice.hpp>

#include <atomic>
#include <cstring>
#include <cstdlib>
#include <unistd.h>

bool32 launched = false;
// Make sure you assign this to the engine's main loop thread when you create it.
// If you never set it, the priority helpers will no-op rather than touch random threads.
pthread_t mainthread = 0;

using namespace RSDK;

static struct JNISetup _jni_setup = { 0 };

android_app *app = NULL;

jmethodID getFD    = { 0 };
jmethodID writeLog = { 0 };

jmethodID showLoading = { 0 };
jmethodID hideLoading = { 0 };
jmethodID setLoading = { 0 };
jmethodID setPixSize = { 0 };

#if RETRO_USE_MOD_LOADER
jmethodID fsExists      = { 0 };
jmethodID fsIsDir       = { 0 };
jmethodID fsDirIter     = { 0 };
jmethodID fsRecurseIter = { 0 };
#endif

#include <game-activity/GameActivity.cpp>
#include <game-text-input/gametextinput.cpp>
extern "C" {
#include <game-activity/native_app_glue/android_native_app_glue.c>
}

// --- Persist/Resume support ----------------------------------------------
struct PersistBlob {
    uint32_t magic;      // 'RSDK'
    uint16_t version;    // 1
    uint16_t reserved;
    int32_t  activeCategory;
    int32_t  listPos;
};
static PersistBlob gResume{};
static std::atomic<bool> gHasResume{false};
static std::atomic<bool> gAppliedResume{false};

// tiny pump that retries resume until the engine is ready
static std::atomic<bool> gResumePumpRunning{false};
static pthread_t gResumePumpThread = 0;

static inline uint32_t fourcc(char a, char b, char c, char d)
{
    return ((uint32_t)a << 24) | ((uint32_t)b << 16) | ((uint32_t)c << 8) | ((uint32_t)d);
}

static void LoadPersistIfAny(android_app *a)
{
    if (a->savedState && a->savedStateSize == sizeof(PersistBlob)) {
        memcpy(&gResume, a->savedState, sizeof(PersistBlob));
        if (gResume.magic == fourcc('R', 'S', 'D', 'K') && gResume.version == 1) {
            gHasResume.store(true, std::memory_order_release);
            gAppliedResume.store(false, std::memory_order_release);
        }
    }
}

// Safe to call repeatedly; will no-op once applied.
// Now waits for scene list to exist so we can override Logos reliably.
static void TryApplyPendingResume()
{
    if (!gHasResume.load(std::memory_order_acquire) || gAppliedResume.load())
        return;

    // Need a window to bind GL; without it we're too early.
    if (!RenderDevice::window)
        return;

    // Wait until the engine has enumerated scenes (after GameConfig load).
    if (sceneInfo.categoryCount <= 0 || !sceneInfo.listCategory)
        return;

    int c = gResume.activeCategory;
    if (c < 0 || c >= sceneInfo.categoryCount)
        return;

    // Clamp listPos to the saved category range (defensive).
    int start = sceneInfo.listCategory[c].sceneOffsetStart;
    int end   = sceneInfo.listCategory[c].sceneOffsetEnd;
    if (start >= end) // empty category? wait and retry later
        return;

    int p = gResume.listPos;
    if (p < start) p = start;
    if (p >= end)  p = end - 1;

    // Mirror the F1/F2 debug path: set target and load.
    sceneInfo.activeCategory = c;
    sceneInfo.listPos        = p;
    LoadScene();

    gAppliedResume.store(true, std::memory_order_release);
    gHasResume.store(false, std::memory_order_release);
}

// Background pump thread that re-tries resume until it succeeds or is cancelled.
static void* ResumePumpMain(void*)
{
    // Keep poking ~60 Hz for a short period; stop once applied or told to stop.
    int ticks = 0;
    while (gResumePumpRunning.load(std::memory_order_acquire) && !gAppliedResume.load()) {
        TryApplyPendingResume();
        usleep(16 * 1000); // ~16ms
        if (++ticks > 1200 && !gAppliedResume.load()) { // ~20s hard cap, bail out
            break;
        }
    }
    gResumePumpRunning.store(false, std::memory_order_release);
    return nullptr;
}

static void StartResumePumpIfNeeded()
{
    if (!gHasResume.load(std::memory_order_acquire) || gResumePumpRunning.load())
        return;
    gResumePumpRunning.store(true, std::memory_order_release);
    if (pthread_create(&gResumePumpThread, nullptr, &ResumePumpMain, nullptr) != 0) {
        gResumePumpRunning.store(false, std::memory_order_release);
    }
}

static void StopResumePump()
{
    if (gResumePumpRunning.exchange(false)) {
        // If the thread was running, let it exit and join it.
        if (gResumePumpThread) {
            pthread_join(gResumePumpThread, nullptr);
            gResumePumpThread = 0;
        }
    }
}

// ---------- Priority helpers (thread-scoped, conservative) ----------
static inline void setThreadBgIfKnown()
{
    if (!mainthread)
        return; // avoid touching the wrong thread

    struct sched_param sp {};
    sp.sched_priority = 0;
#ifdef SCHED_IDLE
    if (pthread_setschedparam(mainthread, SCHED_IDLE, &sp) != 0) {
        // fallback to default timesharing (no priority boost)
        pthread_setschedparam(mainthread, SCHED_OTHER, &sp);
    }
#else
    pthread_setschedparam(mainthread, SCHED_OTHER, &sp);
#endif
}

static inline void setThreadFgIfKnown()
{
    if (!mainthread)
        return;

    struct sched_param sp {};
    sp.sched_priority = 0;
    pthread_setschedparam(mainthread, SCHED_OTHER, &sp);
}

JNISetup *GetJNISetup()
{
    // Ensure this thread has a JNIEnv*
    app->activity->vm->AttachCurrentThread(&_jni_setup.env, NULL);

    JNIEnv *env = _jni_setup.env;
    // Current GameActivity instance from GameActivity/native_app_glue (a global ref owned by the glue)
    jobject currentActivity = app->activity->javaGameActivity;

    // Track the last seen GameActivity handle from the glue so we only refresh when it actually changes.
    static jobject s_lastGlueActivity = nullptr;

    // Refresh cached references if they don't exist or if the activity instance changed.
    if (_jni_setup.thiz == nullptr || s_lastGlueActivity != currentActivity) {
        // Drop old globals if any (they become invalid after Activity destroy).
        if (_jni_setup.thiz) {
            env->DeleteGlobalRef(_jni_setup.thiz);
            _jni_setup.thiz = nullptr;
        }
        if (_jni_setup.clazz) {
            env->DeleteGlobalRef(_jni_setup.clazz);
            _jni_setup.clazz = nullptr;
        }

        // Promote current activity and its class to globals so they remain valid across threads/calls.
        _jni_setup.thiz = env->NewGlobalRef(currentActivity);

        jclass localCls = env->GetObjectClass(currentActivity);
        _jni_setup.clazz = (jclass)env->NewGlobalRef(localCls);
        env->DeleteLocalRef(localCls);

        // Remember which glue Activity we mirrored, so we can detect future changes without touching JNI.
        s_lastGlueActivity = currentActivity;
    }
    return &_jni_setup;
}

FileIO *fOpen(const char *path, const char *mode)
{
    app->activity->vm->AttachCurrentThread(&_jni_setup.env, NULL);
    jbyteArray jpath = _jni_setup.env->NewByteArray(strlen(path));
    _jni_setup.env->SetByteArrayRegion(jpath, 0, strlen(path), (jbyte *)path);
    int fd = _jni_setup.env->CallIntMethod(_jni_setup.thiz, getFD, jpath, mode[0]);
    if (!fd)
        return NULL;
    return fdopen(fd, mode);
}

/*
int fSeek(FileIO* file, long offset, int whence) {
    return fseek(file, offset, whence);
}

int fTell(FileIO* file) {
    return ftell(file);
}//*/

int32 AndroidToWinAPIMappings(int32 mapping)
{
    switch (mapping) {
        default: return KEYMAP_NO_MAPPING;
        // case AKEYCODE_HOME: return VK_HOME;
        case AKEYCODE_0: return VK_0;
        case AKEYCODE_1: return VK_1;
        case AKEYCODE_2: return VK_2;
        case AKEYCODE_3: return VK_3;
        case AKEYCODE_4: return VK_4;
        case AKEYCODE_5: return VK_5;
        case AKEYCODE_6: return VK_6;
        case AKEYCODE_7: return VK_7;
        case AKEYCODE_8: return VK_8;
        case AKEYCODE_9: return VK_9;
        case AKEYCODE_DPAD_UP: return VK_UP;
        case AKEYCODE_DPAD_DOWN: return VK_DOWN;
        case AKEYCODE_DPAD_LEFT: return VK_LEFT;
        case AKEYCODE_DPAD_RIGHT: return VK_RIGHT;
        case AKEYCODE_DPAD_CENTER: return VK_SELECT;
        // case AKEYCODE_VOLUME_UP: return VK_VOLUME_UP;
        // case AKEYCODE_VOLUME_DOWN: return VK_VOLUME_DOWN;
        case AKEYCODE_CLEAR: return VK_CLEAR;
        case AKEYCODE_A: return VK_A;
        case AKEYCODE_B: return VK_B;
        case AKEYCODE_C: return VK_C;
        case AKEYCODE_D: return VK_D;
        case AKEYCODE_E: return VK_E;
        case AKEYCODE_F: return VK_F;
        case AKEYCODE_G: return VK_G;
        case AKEYCODE_H: return VK_H;
        case AKEYCODE_I: return VK_I;
        case AKEYCODE_J: return VK_J;
        case AKEYCODE_K: return VK_K;
        case AKEYCODE_L: return VK_L;
        case AKEYCODE_M: return VK_M;
        case AKEYCODE_N: return VK_N;
        case AKEYCODE_O: return VK_O;
        case AKEYCODE_P: return VK_P;
        case AKEYCODE_Q: return VK_Q;
        case AKEYCODE_R: return VK_R;
        case AKEYCODE_S: return VK_S;
        case AKEYCODE_T: return VK_T;
        case AKEYCODE_U: return VK_U;
        case AKEYCODE_V: return VK_V;
        case AKEYCODE_W: return VK_W;
        case AKEYCODE_X: return VK_X;
        case AKEYCODE_Y: return VK_Y;
        case AKEYCODE_Z: return VK_Z;
        case AKEYCODE_COMMA: return VK_OEM_COMMA;
        case AKEYCODE_PERIOD: return VK_OEM_PERIOD;
        case AKEYCODE_ALT_LEFT: return VK_LMENU;
        case AKEYCODE_ALT_RIGHT: return VK_RMENU;
        case AKEYCODE_SHIFT_LEFT: return VK_LSHIFT;
        case AKEYCODE_SHIFT_RIGHT: return VK_RSHIFT;
        case AKEYCODE_TAB: return VK_TAB;
        case AKEYCODE_SPACE: return VK_SPACE;
        case AKEYCODE_ENVELOPE: return VK_LAUNCH_MAIL;
        case AKEYCODE_ENTER: return VK_RETURN;
        case AKEYCODE_MINUS: return VK_OEM_MINUS;
        case AKEYCODE_MENU: return VK_MENU;
        case AKEYCODE_MEDIA_PLAY_PAUSE: return VK_MEDIA_PLAY_PAUSE;
        case AKEYCODE_MEDIA_STOP: return VK_MEDIA_STOP;
        case AKEYCODE_MEDIA_NEXT: return VK_MEDIA_NEXT_TRACK;
        case AKEYCODE_MEDIA_PREVIOUS: return VK_MEDIA_PREV_TRACK;
        case AKEYCODE_MUTE: return VK_VOLUME_MUTE;
        case AKEYCODE_PAGE_UP: return VK_PRIOR;
        case AKEYCODE_PAGE_DOWN: return VK_NEXT;
        case AKEYCODE_ESCAPE: return VK_ESCAPE;
        case AKEYCODE_DEL: return VK_BACK; // ???????????? i hate android
        case AKEYCODE_FORWARD_DEL: return VK_DELETE;
        case AKEYCODE_CTRL_LEFT: return VK_LCONTROL;
        case AKEYCODE_CTRL_RIGHT: return VK_RCONTROL;
        case AKEYCODE_CAPS_LOCK: return VK_CAPITAL;
        case AKEYCODE_SCROLL_LOCK: return VK_SCROLL;
        case AKEYCODE_SYSRQ: return VK_SNAPSHOT;
        case AKEYCODE_BREAK: return VK_PAUSE;
        case AKEYCODE_MOVE_HOME: return VK_HOME;
        case AKEYCODE_MOVE_END: return VK_END;
        case AKEYCODE_INSERT: return VK_INSERT;
        case AKEYCODE_F1: return VK_F1;
        case AKEYCODE_F2: return VK_F2;
        case AKEYCODE_F3: return VK_F3;
        case AKEYCODE_F4: return VK_F4;
        case AKEYCODE_F5: return VK_F5;
        case AKEYCODE_F6: return VK_F6;
        case AKEYCODE_F7: return VK_F7;
        case AKEYCODE_F8: return VK_F8;
        case AKEYCODE_F9: return VK_F9;
        case AKEYCODE_F10: return VK_F10;
        case AKEYCODE_F11: return VK_F11;
        case AKEYCODE_F12: return VK_F12;
        case AKEYCODE_NUM_LOCK: return VK_NUMLOCK;
        case AKEYCODE_NUMPAD_0: return VK_NUMPAD0;
        case AKEYCODE_NUMPAD_1: return VK_NUMPAD1;
        case AKEYCODE_NUMPAD_2: return VK_NUMPAD2;
        case AKEYCODE_NUMPAD_3: return VK_NUMPAD3;
        case AKEYCODE_NUMPAD_4: return VK_NUMPAD4;
        case AKEYCODE_NUMPAD_5: return VK_NUMPAD5;
        case AKEYCODE_NUMPAD_6: return VK_NUMPAD6;
        case AKEYCODE_NUMPAD_7: return VK_NUMPAD7;
        case AKEYCODE_NUMPAD_8: return VK_NUMPAD8;
        case AKEYCODE_NUMPAD_9: return VK_NUMPAD9;
        case AKEYCODE_NUMPAD_DIVIDE: return VK_DIVIDE;
        case AKEYCODE_NUMPAD_MULTIPLY: return VK_MULTIPLY;
        case AKEYCODE_NUMPAD_SUBTRACT: return VK_SUBTRACT;
        case AKEYCODE_NUMPAD_ADD: return VK_ADD;
        case AKEYCODE_NUMPAD_DOT: return VK_DECIMAL;
        case AKEYCODE_NUMPAD_COMMA: return VK_OEM_COMMA;
        case AKEYCODE_NUMPAD_ENTER: return VK_RETURN;
        // case AKEYCODE_VOLUME_MUTE: return VK_VOLUME_MUTE;
        case AKEYCODE_ZOOM_IN: return VK_ZOOM;
        case AKEYCODE_ZOOM_OUT: return VK_ZOOM;
        case AKEYCODE_SLEEP: return VK_SLEEP;
        case AKEYCODE_HELP: return VK_HELP;
    }
}

JNIEXPORT void JNICALL jnifunc(nativeOnTouch, RSDK, jint finger, jint action, jfloat x, jfloat y)
{
    if (finger > 0x10)
        return; // nah cause how tf

    bool32 down = (action == AMOTION_EVENT_ACTION_DOWN) || (action == AMOTION_EVENT_ACTION_MOVE) || (action == AMOTION_EVENT_ACTION_POINTER_DOWN);

    if (down) {
        touchInfo.x[finger]    = x;
        touchInfo.y[finger]    = y;
        touchInfo.down[finger] = true;
        if (touchInfo.count < finger + 1)
            touchInfo.count = finger + 1;
    }
    else {
        touchInfo.down[finger] = false;
        if (touchInfo.count >= finger + 1) {
            for (; touchInfo.count > 0; touchInfo.count--) {
                if (touchInfo.down[touchInfo.count - 1])
                    break;
            }
        }
    }
}

JNIEXPORT jbyteArray JNICALL jnifunc(nativeLoadFile, RSDK, jstring file)
{
    const char *path = env->GetStringUTFChars(file, NULL);
    FileInfo info;
    InitFileInfo(&info);
    if (LoadFile(&info, path, FMODE_RB)) {
        jbyteArray ret = env->NewByteArray(info.fileSize);
        jbyte *array   = env->GetByteArrayElements(ret, NULL);
        ReadBytes(&info, array, info.fileSize);
        CloseFile(&info);
        env->ReleaseByteArrayElements(ret, array, 0);
        env->ReleaseStringUTFChars(file, path);
        return ret;
    }
    env->ReleaseStringUTFChars(file, path);
    return NULL;
}

void ShowLoadingIcon()
{
    auto *jni = GetJNISetup();
    jni->env->CallVoidMethod(jni->thiz, showLoading);
}

void HideLoadingIcon()
{
    auto *jni = GetJNISetup();
    jni->env->CallVoidMethod(jni->thiz, hideLoading);
}

void SetLoadingIcon()
{
    auto *jni = GetJNISetup();
    // cheating time
    jstring name           = jni->env->NewStringUTF("Data/Sprites/Android/Loading.bin");
    jbyteArray waitSpinner = jniname(nativeLoadFile, RSDK)(jni->env, jni->clazz, name);
    if (!waitSpinner) {
        name        = jni->env->NewStringUTF("Data/Sprites/UI/WaitSpinner.bin");
        waitSpinner = jniname(nativeLoadFile, RSDK)(jni->env, jni->clazz, name);
    }
    jni->env->CallVoidMethod(jni->thiz, setLoading, waitSpinner);
}

// ---------- Foreground/background state (no render deinit here) ----------
static inline void enterBackground()
{
#if RETRO_REV02
    if (SKU::userCore) SKU::userCore->focusState = 1;
#else
    engine.focusState &= (~1);
#endif
    videoSettings.windowState = WINDOWSTATE_INACTIVE;

    // Only lower priority for the main engine thread if known.
    setThreadBgIfKnown();
}

static inline void enterForeground()
{
#if RETRO_REV02
    if (SKU::userCore) SKU::userCore->focusState = 0;
#else
    engine.focusState |= 1;
#endif
    videoSettings.windowState = WINDOWSTATE_ACTIVE;

    setThreadFgIfKnown();
}

void AndroidCommandCallback(android_app *app, int32 cmd)
{
    PrintLog(PRINT_NORMAL, "COMMAND %d %d", cmd, app->window ? 1 : 0);

    // NEW: Inform the Oboe audio backend about lifecycle, in parallel with existing handling.
    switch (cmd) {
        case APP_CMD_PAUSE:
        case APP_CMD_STOP:
            RSDK::AudioDevice::NotifyAppBackground();
            break;

        case APP_CMD_TERM_WINDOW:
            RSDK::AudioDevice::NotifyWindowAvailable(false);
            RSDK::AudioDevice::NotifyAppBackground();
            break;

        case APP_CMD_INIT_WINDOW:
            RSDK::AudioDevice::NotifyWindowAvailable(true);
            break;

        case APP_CMD_RESUME:
            RSDK::AudioDevice::NotifyAppForeground();
            break;

        case APP_CMD_START:
            // The glue has populated app->savedState (if any) by now.
            LoadPersistIfAny(app);
            // Begin retrying resume in the background (idempotent).
            StartResumePumpIfNeeded();
            RSDK::AudioDevice::NotifyAppForeground();
            break;

        case APP_CMD_GAINED_FOCUS:
            RSDK::AudioDevice::NotifyFocusChanged(true);
            break;

        case APP_CMD_LOST_FOCUS:
            RSDK::AudioDevice::NotifyFocusChanged(false);
            break;

        case APP_CMD_SAVE_STATE: {
            // Snapshot just enough to get back to the same scene quickly.
            PersistBlob blob{};
            blob.magic          = fourcc('R', 'S', 'D', 'K');
            blob.version        = 1;
            blob.activeCategory = sceneInfo.activeCategory;
            blob.listPos        = sceneInfo.listPos;

            // Portable fallback (works with your glued-in native_app_glue.c)
            if (app->savedState) {
                free(app->savedState);
                app->savedState = nullptr;
                app->savedStateSize = 0;
            }
            app->savedState = malloc(sizeof(PersistBlob));
            if (app->savedState) {
                memcpy(app->savedState, &blob, sizeof(PersistBlob));
                app->savedStateSize = sizeof(PersistBlob);
            }
            break;
        }

        case APP_CMD_DESTROY: {
            // Activity is going away; drop any cached global refs so we don't
            // hold invalid JNI references across re-creation.
            JNIEnv *env_destroy = nullptr;
            app->activity->vm->AttachCurrentThread(&env_destroy, nullptr);
            if (_jni_setup.thiz) { env_destroy->DeleteGlobalRef(_jni_setup.thiz); _jni_setup.thiz = nullptr; }
            if (_jni_setup.clazz) { env_destroy->DeleteGlobalRef(_jni_setup.clazz); _jni_setup.clazz = nullptr; }
            StopResumePump();
            break;
        }

        default:
            break;
    }

    // Existing engine/platform handling (unchanged)
    switch (cmd) {
        // ---- Window/surface lifecycle ----
        case APP_CMD_INIT_WINDOW:
        case APP_CMD_WINDOW_RESIZED:
        case APP_CMD_CONFIG_CHANGED: {
            // Fresh/broadcasted window from glue. Safe to adopt & (re)bind.
            RenderDevice::isInitialized = false;
            RenderDevice::window        = app->window;
            if (RenderDevice::window) {
#if RETRO_REV02
                if (SKU::userCore) SKU::userCore->focusState = 0;
#else
                engine.focusState |= 1;
#endif
                videoSettings.windowState = WINDOWSTATE_ACTIVE;
                SwappyGL_setWindow(RenderDevice::window);

                // If we have a pending resume token, apply it now (and the pump will keep trying if too early).
                TryApplyPendingResume();
                StartResumePumpIfNeeded();
            }
            break;
        }

        case APP_CMD_TERM_WINDOW: {
            // The glue is about to null out app->window; do NOT copy the stale pointer.
            // Proactively detach everything from the window and mark inactive.
            SwappyGL_setWindow(nullptr);
            RenderDevice::isInitialized = false;
            RenderDevice::window        = nullptr;
#if RETRO_REV02
            if (SKU::userCore) SKU::userCore->focusState = 1;
#else
            engine.focusState &= (~1);
#endif
            videoSettings.windowState = WINDOWSTATE_INACTIVE;
            break;
        }

        // App lifecycle (don’t twiddle render init flags here)
        case APP_CMD_START:
            Paddleboat_onStart(GetJNISetup()->env);
            break;
        case APP_CMD_STOP:
            Paddleboat_onStop(GetJNISetup()->env);
            break;

        case APP_CMD_PAUSE:
        case APP_CMD_LOST_FOCUS:
            enterBackground();
            break;

        case APP_CMD_RESUME:
            enterForeground();
            // Safety net: try again on resume
            TryApplyPendingResume();
            StartResumePumpIfNeeded();
            break;

        case APP_CMD_GAINED_FOCUS:
            enterForeground();
            // Another good moment to apply, post-init.
            TryApplyPendingResume();
            StartResumePumpIfNeeded();
            break;

        case APP_CMD_WINDOW_REDRAW_NEEDED:
            // Some devices deliver this just after GL init; it’s another good moment to apply.
            TryApplyPendingResume();
            StartResumePumpIfNeeded();
            break;

        case APP_CMD_DESTROY: {
            // Also make sure no stale window survives a full activity destroy.
            SwappyGL_setWindow(nullptr);
            RenderDevice::isInitialized = false;
            RenderDevice::window        = nullptr;
            break;
        }

        default:
            break;
    }
}

bool AndroidKeyDownCallback(GameActivity *activity, const GameActivityKeyEvent *event)
{
    if (Paddleboat_processGameActivityKeyInputEvent(event, sizeof(GameActivityKeyEvent)))
        return true;
    int32 keycode = event->keyCode;

#if !RETRO_REV02
    ++RSDK::SKU::buttonDownCount;
#endif
    switch (keycode) {
        case AKEYCODE_ENTER:

#if !RETRO_REV02 && RETRO_INPUTDEVICE_KEYBOARD
            RSDK::SKU::specialKeyStates[1] = true;
#endif
            // [fallthrough]

        default:
            if (AndroidToWinAPIMappings(keycode)) {

#if RETRO_INPUTDEVICE_KEYBOARD
                SKU::UpdateKeyState(AndroidToWinAPIMappings(keycode));
#endif
                return true;
            }
            return false;

        case AKEYCODE_ESCAPE:
            if (engine.devMenu) {
#if RETRO_REV0U
                if (sceneInfo.state == ENGINESTATE_DEVMENU || RSDK::Legacy::gameMode == RSDK::Legacy::ENGINE_DEVMENU)
#else
                if (sceneInfo.state == ENGINESTATE_DEVMENU)
#endif
                    CloseDevMenu();
                else
                    OpenDevMenu();
            }
            else {
#if RETRO_INPUTDEVICE_KEYBOARD
                SKU::UpdateKeyState(AndroidToWinAPIMappings(keycode));
#endif
            }

#if !RETRO_REV02 && RETRO_INPUTDEVICE_KEYBOARD
            RSDK::SKU::specialKeyStates[0] = true;
#endif
            return true;

#if !RETRO_USE_ORIGINAL_CODE
        case AKEYCODE_F1:
            sceneInfo.listPos--;
            if (sceneInfo.listPos < sceneInfo.listCategory[sceneInfo.activeCategory].sceneOffsetStart) {
                sceneInfo.activeCategory--;
                if (sceneInfo.activeCategory >= sceneInfo.categoryCount) {
                    sceneInfo.activeCategory = sceneInfo.categoryCount - 1;
                }
                sceneInfo.listPos = sceneInfo.listCategory[sceneInfo.activeCategory].sceneOffsetEnd - 1;
            }

            LoadScene();
            return true;

        case AKEYCODE_F2:
            sceneInfo.listPos++;
            if (sceneInfo.listPos >= sceneInfo.listCategory[sceneInfo.activeCategory].sceneOffsetEnd) {
                sceneInfo.activeCategory++;
                if (sceneInfo.activeCategory >= sceneInfo.categoryCount) {
                    sceneInfo.activeCategory = 0;
                }
                sceneInfo.listPos = sceneInfo.listCategory[sceneInfo.activeCategory].sceneOffsetStart;
            }

            LoadScene();
            return true;
#endif

        case AKEYCODE_F3:
            if (userShaderCount)
                videoSettings.shaderID = (videoSettings.shaderID + 1) % userShaderCount;
            return true;

#if !RETRO_USE_ORIGINAL_CODE
        case AKEYCODE_F5:
            // Quick-Reload
            LoadScene();
            return true;

        case AKEYCODE_F6:
            if (engine.devMenu && videoSettings.screenCount > 1)
                videoSettings.screenCount--;
            return true;

        case AKEYCODE_F7:
            if (engine.devMenu && videoSettings.screenCount < SCREEN_COUNT)
                videoSettings.screenCount++;
            return true;

        case AKEYCODE_F9:
            if (engine.devMenu)
                showHitboxes ^= 1;
            return true;

        case AKEYCODE_F10:
            if (engine.devMenu)
                engine.showPaletteOverlay ^= 1;
            return true;
#endif
        case AKEYCODE_DEL:
            if (engine.devMenu)
                engine.gameSpeed = engine.fastForwardSpeed;
            return true;

        case AKEYCODE_F11:
        case AKEYCODE_INSERT:
            if (engine.devMenu)
                engine.frameStep = true;
            return true;

        case AKEYCODE_F12:
        case AKEYCODE_BREAK:
            if (engine.devMenu) {
#if RETRO_REV0U
                switch (engine.version) {
                    default: break;
                    case 5:
                        if (sceneInfo.state != ENGINESTATE_NONE)
                            sceneInfo.state ^= ENGINESTATE_STEPOVER;
                        break;
                    case 4:
                    case 3:
                        if (RSDK::Legacy::stageMode != ENGINESTATE_NONE)
                            RSDK::Legacy::stageMode ^= RSDK::Legacy::STAGEMODE_STEPOVER;
                        break;
                }
#else
                if (sceneInfo.state != ENGINESTATE_NONE)
                    sceneInfo.state ^= ENGINESTATE_STEPOVER;
#endif
            }
            return true;
    }
    return false;
}

bool AndroidKeyUpCallback(GameActivity *activity, const GameActivityKeyEvent *event)
{
    if (Paddleboat_processGameActivityKeyInputEvent(event, sizeof(GameActivityKeyEvent)))
        return true;

    int32 keycode = event->keyCode;
#if !RETRO_REV02
    --RSDK::SKU::buttonDownCount;
#endif
    switch (keycode) {
        default:
#if RETRO_INPUTDEVICE_KEYBOARD
            SKU::ClearKeyState(AndroidToWinAPIMappings(keycode));
#endif
            return true;

#if !RETRO_REV02 && RETRO_INPUTDEVICE_KEYBOARD
        case AKEYCODE_ESCAPE: RSDK::SKU::specialKeyStates[0] = false; return true;
        case AKEYCODE_ENTER: RSDK::SKU::specialKeyStates[1] = false; return true;
#endif
        case AKEYCODE_DEL: engine.gameSpeed = 1; return true;
    }
    return false;
}

// -----------------------------------------------------------------------------
// Android warm-resume bridge for the engine boot code.
// The engine will call these BEFORE it selects the initial scene.
// -----------------------------------------------------------------------------
extern "C" bool AndroidHasResumeToken()
{
    // A token exists if Android saved state (scene ids) and we haven't applied it yet.
    return gHasResume.load(std::memory_order_acquire) && !gAppliedResume.load();
}

extern "C" bool AndroidConsumeResumeToken(int* outCategory, int* outListPos)
{
    if (!outCategory || !outListPos)
        return false;
    if (!AndroidHasResumeToken())
        return false;

    // Hand the saved scene ids to the engine and mark them as consumed.
    *outCategory = gResume.activeCategory;
    *outListPos  = gResume.listPos;

    gAppliedResume.store(true, std::memory_order_release);
    gHasResume.store(false, std::memory_order_release);
    return true;
}