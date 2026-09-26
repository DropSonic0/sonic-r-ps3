/**
 * platform_ps3.c — PS3 platform implementation & stubs
 *
 * Implements platform API, audio stubs, CD audio stubs, and POSIX/sys helper stubs
 * when building for PS3.
 */

#if defined(SONICR_PS3) || defined(__CELLOS_LV2__) || defined(SN_TARGET_PS3) || defined(__SNC__) || defined(__CELL_ASSERT__) || defined(__PPU__) || defined(_PS3) || defined(PS3) || defined(__PS3__)

#include <PSGL/psgl.h>
#include <PSGL/psglu.h>
#include <cell/sysmodule.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/sys_time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <time.h>

#include "platform.h"
#include "sonicr_types.h"
#include "sonicr_globals.h"

unsigned char s_keystate[256];

/* =====================================================================
 * Platform API Implementation for PS3
 * ===================================================================== */

int platform_init(int width, int height, int fullscreen, const char *title)
{
    (void)width;
    (void)height;
    (void)fullscreen;
    (void)title;
    memset(s_keystate, 0, sizeof(s_keystate));
    
    /* Load system modules required by PSGL */
    cellSysmoduleLoadModule(CELL_SYSMODULE_GCM_SYS);

    PSGLinitOptions initOpts;
    memset(&initOpts, 0, sizeof(PSGLinitOptions));
    initOpts.enable = PSGL_INIT_MAX_SPUS | PSGL_INIT_INITIALIZE_SPUS | PSGL_INIT_HOST_MEMORY_SIZE;
    initOpts.maxSPUs = 1;
    initOpts.initializeSPUs = 0;
    initOpts.persistentMemorySize = 0;
    initOpts.transientMemorySize = 0;
    initOpts.errorConsole = 0;
    initOpts.fifoSize = 0;
    initOpts.hostMemorySize = 8 * 1024 * 1024;

    printf("[PS3 PSGL] Initializing PSGL with psglInit(&initOpts)...\n");
    psglInit(&initOpts);

    PSGLdeviceParameters params;
    memset(&params, 0, sizeof(PSGLdeviceParameters));
    params.enable = PSGL_DEVICE_PARAMETERS_COLOR_FORMAT |
                    PSGL_DEVICE_PARAMETERS_DEPTH_FORMAT |
                    PSGL_DEVICE_PARAMETERS_MULTISAMPLING_MODE |
                    PSGL_DEVICE_PARAMETERS_BUFFERING_MODE |
                    PSGL_DEVICE_PARAMETERS_RESC_ADJUST_ASPECT_RATIO |
                    PSGL_DEVICE_PARAMETERS_RESC_RATIO_MODE;
    params.bufferingMode = PSGL_BUFFERING_MODE_TRIPLE;
    params.colorFormat = GL_ARGB_SCE;
    params.depthFormat = GL_NONE;
    params.multisamplingMode = GL_MULTISAMPLING_NONE_SCE;
    params.rescRatioMode = RESC_RATIO_MODE_FULLSCREEN;

    printf("[PS3 PSGL] Creating PSGL device with psglCreateDeviceExtended...\n");
    PSGLdevice *device = psglCreateDeviceExtended(&params);
    if (!device) {
        printf("[PS3 PSGL] psglCreateDeviceExtended failed, trying GL_DEPTH_COMPONENT24...\n");
        params.depthFormat = GL_DEPTH_COMPONENT24;
        device = psglCreateDeviceExtended(&params);
    }
    if (!device) {
        printf("[PS3 PSGL] ERROR: psglCreateDeviceExtended failed!\n");
        return -1;
    }

    GLuint glWidth = 0, glHeight = 0;
    psglGetDeviceDimensions(device, &glWidth, &glHeight);
    printf("[PS3 PSGL] Device created (%u x %u)\n", glWidth, glHeight);

    PSGLcontext *context = psglCreateContext();
    if (!context) {
        printf("[PS3 PSGL] ERROR: psglCreateContext failed!\n");
        return -1;
    }

    psglMakeCurrent(context, device);
    psglResetCurrentContext();

    printf("[PS3 PSGL] PSGL Context initialized successfully!\n");
    return 0;
}

void platform_shutdown(void)
{
    PSGLcontext *context = psglGetCurrentContext();
    PSGLdevice *device = psglGetCurrentDevice();
    if (context) {
        psglDestroyContext(context);
    }
    if (device) {
        psglDestroyDevice(device);
    }
    psglExit();
}

static char s_ps3ExeDir[1024] = "";

void ps3_set_exe_path(const char *argv0)
{
    if (argv0 == NULL || argv0[0] == '\0') return;
    strncpy(s_ps3ExeDir, argv0, sizeof(s_ps3ExeDir) - 1);
    s_ps3ExeDir[sizeof(s_ps3ExeDir) - 1] = '\0';

    char *lastSlash = strrchr(s_ps3ExeDir, '/');
    char *lastBackslash = strrchr(s_ps3ExeDir, '\\');
    if (lastBackslash > lastSlash) lastSlash = lastBackslash;
    if (lastSlash != NULL) {
        *(lastSlash + 1) = '\0';
    } else {
        s_ps3ExeDir[0] = '\0';
    }
    printf("[PS3 fOpen] Set EXE directory: '%s'\n", s_ps3ExeDir);
}

FILE *ps3_fOpen(const char *path, const char *mode)
{
    if (path == NULL) return NULL;

    if (path[0] == '/' || path[0] == '\\') {
        FILE *fp = fopen(path, mode);
        printf("[PS3 fOpen] Absolute path '%s' -> %s\n", path, fp ? "OK" : "FAILED");
        return fp;
    }

    const char *relPath = path;
    if (relPath[0] == '.' && (relPath[1] == '/' || relPath[1] == '\\')) {
        relPath += 2;
    }

    char fullPath[1024];

    /* 1. Dynamically walk up parent directories from EXE path */
    if (s_ps3ExeDir[0] != '\0') {
        char currentLevel[1024];
        strncpy(currentLevel, s_ps3ExeDir, sizeof(currentLevel) - 1);
        currentLevel[sizeof(currentLevel) - 1] = '\0';

        for (int depth = 0; depth <= 5; depth++) {
            snprintf(fullPath, sizeof(fullPath), "%s%s", currentLevel, relPath);
            FILE *fp = fopen(fullPath, mode);
            printf("[PS3 fOpen] Probing '%s' -> %s\n", fullPath, fp ? "SUCCESS" : "failed");
            if (fp != NULL) return fp;

            snprintf(fullPath, sizeof(fullPath), "%sDATA/%s", currentLevel, relPath);
            fp = fopen(fullPath, mode);
            printf("[PS3 fOpen] Probing '%s' -> %s\n", fullPath, fp ? "SUCCESS" : "failed");
            if (fp != NULL) return fp;

            size_t len = strlen(currentLevel);
            if (len <= 1) break;
            if (currentLevel[len - 1] == '/' || currentLevel[len - 1] == '\\') {
                currentLevel[len - 1] = '\0';
            }
            char *slash = strrchr(currentLevel, '/');
            char *bslash = strrchr(currentLevel, '\\');
            if (bslash > slash) slash = bslash;
            if (slash != NULL) {
                *(slash + 1) = '\0';
            } else {
                break;
            }
        }
    }

    /* 2. Static fallback paths */
    static const char *staticPrefixes[] = {
        "/app_home/",
        "/app_home/DATA/",
        "/dev_hdd0/game/SONICR001/USRDIR/",
        "/dev_hdd0/game/SONICR001/USRDIR/DATA/",
        "./",
        "",
        NULL
    };

    for (int i = 0; staticPrefixes[i] != NULL; i++) {
        snprintf(fullPath, sizeof(fullPath), "%s%s", staticPrefixes[i], relPath);
        FILE *fp = fopen(fullPath, mode);
        printf("[PS3 fOpen] Probing '%s' -> %s\n", fullPath, fp ? "SUCCESS" : "failed");
        if (fp != NULL) return fp;
    }

    return NULL;
}

const char *platform_base_path(void)
{
    return "/app_home/";
}

int platform_poll_events(unsigned char *keystateOut, int keystateSize)
{
    if (keystateOut && keystateSize > 0) {
        int copySize = keystateSize < 256 ? keystateSize : 256;
        memcpy(keystateOut, s_keystate, copySize);
    }
    return 0;
}

void platform_pump_events(void)
{
    /* PS3 event pump / pad poll stub */
}

int platform_init_gamepads(void)
{
    return 0;
}

int platform_poll_gamepads(unsigned short *joySlotState, int maxSlots)
{
    if (joySlotState) {
        for (int i = 0; i < maxSlots; i++) {
            joySlotState[i] = 0;
        }
    }
    return 0;
}

uint32_t platform_get_time_ms(void)
{
    static uint64_t start_us = 0;
    uint64_t current_us = sys_time_get_system_time();
    if (start_us == 0) {
        start_us = current_us;
    }
    return (uint32_t)((current_us - start_us) / 1000);
}

void platform_sleep_ms(int ms)
{
    (void)ms;
}

int platform_audio_init(void)
{
    return 0;
}

void platform_audio_shutdown(void)
{
}

void platform_gl_swap(void)
{
    psglSwap();
}

void platform_get_drawable_size(int *w, int *h)
{
    if (w) *w = 640;
    if (h) *h = 480;
}

int platform_net_init(void)
{
    return 0;
}

void platform_net_shutdown(void)
{
}

int platform_net_is_modem(void)
{
    return 0;
}

int platform_get_region(void)
{
    return 0;
}

/* =====================================================================
 * Audio / Sound Stubs for PS3
 * ===================================================================== */

void InitDirectSound(void)
{
}

void CloseDirectSound(void)
{
}

void PlaySoundEffect(int soundCmd, int distance, int freqParam)
{
    (void)soundCmd;
    (void)distance;
    (void)freqParam;
}

void LoadSoundEffect(int soundId, const char *filename)
{
    (void)soundId;
    (void)filename;
}

void SFX_Stop(int soundId)
{
    (void)soundId;
}

void SFX_SetVolume(int soundId, int volume)
{
    (void)soundId;
    (void)volume;
}

void Music_SetVolume(int volume)
{
    (void)volume;
}

void Music_SetDucked(int ducked)
{
    (void)ducked;
}

/* =====================================================================
 * CD Audio / Playback Stubs for PS3
 * ===================================================================== */

void OpenCDDevice(void)
{
}

void CloseCDDevice(void)
{
}

void UpdateCDPlayback(void)
{
}

int GetLogicalCDTrack(int track)
{
    return track;
}

void StopCD(void)
{
}

/* =====================================================================
 * POSIX / Pthreads Stubs for PS3
 * ===================================================================== */

int pthread_create(void *thread, const void *attr, void *(*start_routine)(void *), void *arg)
{
    (void)thread; (void)attr; (void)start_routine; (void)arg;
    return -1;
}

int pthread_join(void *thread, void **retval)
{
    (void)thread; (void)retval;
    return 0;
}

int pthread_mutex_lock(void *mutex)
{
    (void)mutex;
    return 0;
}

int pthread_mutex_unlock(void *mutex)
{
    (void)mutex;
    return 0;
}

int usleep(unsigned int usec)
{
    (void)usec;
    return 0;
}

char *getenv(const char *name)
{
    (void)name;
    return NULL;
}

#endif /* SONICR_PS3 */
