/**
 * sound_ps3.c — Sound and Music implementation using Cell Audio for PS3
 *
 * Implements 64-slot SFX mixer and streamed music playback (ADX, ADP, SON, WAV)
 * using PS3 Cell Audio API (<cell/audio.h>) with a dedicated asynchronous I/O thread.
 */

#if defined(SONICR_PS3) || defined(__CELLOS_LV2__) || defined(SN_TARGET_PS3) || defined(__SNC__) || defined(__CELL_ASSERT__) || defined(__PPU__) || defined(_PS3) || defined(PS3) || defined(__PS3__)

#include <cell/audio.h>
#include <cell/sysmodule.h>
#include <sys/ppu_thread.h>
#include <sys/event.h>
#include <sys/timer.h>
#include <sys/sys_time.h>
#include <sys/synchronization.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <errno.h>

#include "sonicr_types.h"
#include "sonicr_globals.h"
#include "sonicr_functions.h"
#include "fileio.h"
#include "replay_voice.h"
#include "adx.h"
#include "adp.h"

#define PS3_AUDIO_SAMPLE_RATE 48000
#define SFX_MAX_SLOTS 64
#define BGM_BUFFER_SIZE_FRAMES 8192

extern void *g_soundBuffers[64];
extern int g_soundActive[64];
extern void SetAllSoundVolumes(void);
extern void SFX_DuckStop(void);

static DWORD GetTimeMs(void)
{
    return (DWORD)(sys_time_get_system_time() / 1000);
}

/* =====================================================================
 * SFX Structure
 * ===================================================================== */
typedef struct {
    int16_t  *samples;        /* Interleaved S16 PCM (stereo or mono) */
    uint32_t  totalFrames;    /* Sample count (frames) */
    uint32_t  nativeRate;     /* Native rate (e.g. 22050 Hz) */
    int       channels;       /* 1 = Mono, 2 = Stereo */
    
    double    phase;          /* Current frame cursor */
    int       isPlaying;
    int       isLooping;
    int       targetFreq;     /* Live target frequency in Hz */
    float     volume;         /* Linear volume 0.0f..1.0f */
    float     panLeft;        /* Gain left 0.0f..1.0f */
    float     panRight;       /* Gain right 0.0f..1.0f */
} PS3_SfxSlot;

static PS3_SfxSlot s_sfx[SFX_MAX_SLOTS];

/* =====================================================================
 * Music Format Enum & Stream Structure
 * ===================================================================== */
typedef enum {
    MUSIC_FMT_NONE = 0,
    MUSIC_FMT_WAV,
    MUSIC_FMT_SON,
    MUSIC_FMT_ADX,
    MUSIC_FMT_ADP
} MusicFormat;

typedef struct {
    MusicFormat type;
    FILE       *fp;
    long        dataStart;
    uint32_t    totalFrames;
    uint32_t    sampleRate;
    int         channels;
    
    AdxDecoder  adxDec;
    AdpDecoder  adpDec;

    /* Double buffering for async IO */
    int16_t     streamCache[BGM_BUFFER_SIZE_FRAMES * 2 * 2]; /* 2 buffers x BGM_BUFFER_SIZE_FRAMES x 2 channels */
    uint32_t    streamCacheValid[2]; /* Valid frames in buffer 0 and 1 */
    uint32_t    streamCachePos;      /* Read cursor in active buffer (in frames) */
    uint32_t    activeBuffer;        /* 0 or 1 */
    volatile int bufferBusy[2];      /* Set to 1 when buffer needs refill */
} MusicStream;

static MusicStream s_musicStream;
static int s_currentTrack = 0;
static int s_musicReady = 0;
static int s_currentTrackIsFanfare = 0;

static int s_logicalTrack = 0;
static DWORD s_trackStartMs = 0;

/* Asynchronous pending track change queue */
static volatile int s_pendingTrackCmd = -1; /* -1 = no pending request, 0 = stop, 2..21 = track num */

static const int s_trackDurationMs[23] = {
         0,      0,  43210,   6993,  10170,  55580, 305305, 283904,
    270474, 237737, 295113, 241929, 240949, 164269, 208959, 210448,
    202700, 210360, 238142,   3921,   6451,   5910, 5448657
};

static int s_musicDucked = 0;
static int s_musicLevel = 8;
static double s_musicPhase = 0.0;

/* =====================================================================
 * Cell Audio Subsystem State
 * Lock Order Rule: ALWAYS acquire s_bgmIoMutex FIRST, then s_audioMutex.
 * ===================================================================== */
static uint32_t s_audioPortNum = 0xFFFFFFFF;
static sys_event_queue_t s_audioEventQueue;
static sys_ipc_key_t s_audioEventKey;
static sys_ppu_thread_t s_audioThreadHandle;
static sys_ppu_thread_t s_bgmIoThreadHandle;
static sys_mutex_t s_audioMutex;
static sys_mutex_t s_bgmIoMutex;
static volatile int s_audioTerminate = 0;
static int s_cellAudioInitialized = 0;

/* Forward declarations */
static void CloseMusicStream_NoLock(void);
static int OpenMusicTrack_NoLock(int trackNum);

/* =====================================================================
 * Table of default SFX WAV filenames
 * ===================================================================== */
static const struct { int slot; const char *filename; } s_sfxTable[] = {
    { 0x00, "PAUSE.WAV"    },
    { 0x01, "CHOOSE.WAV"   },
    { 0x02, "SELECT.WAV"   },
    { 0x03, "RUNLEFT.WAV"  },
    { 0x04, "RUNRIGHT.WAV" },
    { 0x05, "AMY.WAV"      },
    { 0x06, "JET.WAV"      },
    { 0x07, "JUMP.WAV"     },
    { 0x08, "SPIN.WAV"     },
    { 0x09, "SPINGO.WAV"   },
    { 0x0A, "SPINREV.WAV"  },
    { 0x0B, "TAILS.WAV"    },
    { 0x0D, "JUMP.WAV"     },
    { 0x0E, "FIRE.WAV"     },
    { 0x0F, "EXPLODE.WAV"  },
    { 0x10, "AMYSKID.WAV"  },
    { 0x11, "AMYWATER.WAV" },
    { 0x12, "WATERRUN.WAV" },
    { 0x13, "WATERRUN.WAV" },
    { 0x14, "BUBBLE.WAV"   },
    { 0x15, "SPLASH.WAV"   },
    { 0x16, "POP.WAV"      },
    { 0x18, "HITCHAR.WAV"  },
    { 0x1A, "BONUS.WAV"    },
    { 0x1B, "GETTOKEN.WAV" },
    { 0x1C, "GETCHAOS.WAV" },
    { 0x1D, "RING1.WAV"    },
    { 0x1E, "RING1.WAV"    },
    { 0x1F, "WARP.WAV"     },
    { 0x20, "SKID1.WAV"    },
    { 0x21, "DOOR.WAV"     },
    { 0x22, "RECORD.WAV"   },
    { 0x23, "GOTALL.WAV"   },
    { 0x24, "BONUS.WAV"    },
    { 0x27, "TAG.WAV"      },
    { 0x2D, "THUNDER.WAV"  },
    { 0x32, "SPRING.WAV"   },
    { 0x33, "BUMPER1.WAV"  },
    { 0x34, "BUMPER2.WAV"  },
    { 0x35, "READY.WAV"    },
    { 0x36, "SET.WAV"      },
    { 0x37, "GO.WAV"       },
    { -1,   NULL           }
};

/* =====================================================================
 * Helper: Conversions
 * ===================================================================== */
#define SFX_CURVE_EXP 1.5f
static float sfx_ds_linear(int dsVolume)
{
    if (dsVolume <= g_volumeBase) {
        return 0.0f;
    }
    if (dsVolume >= 0) {
        return 1.0f;
    }
    float t = (float)(dsVolume - g_volumeBase) / (float)(-g_volumeBase);
    return powf(t, SFX_CURVE_EXP);
}

static float Music_EffectiveVolume(void)
{
    if (g_musicEnabled == 0) {
        return 0.0f;
    }
    float base = s_musicDucked ? 0.5f : 1.0f;
    return base * ((float)s_musicLevel / 8.0f);
}

/* =====================================================================
 * Music Decoding Helper (called on BGM IO thread)
 * Reads up to `framesWanted` stereo S16 samples into `outPcm`.
 * ===================================================================== */
static uint32_t Music_ReadFrames_Internal(int16_t *outPcm, uint32_t framesWanted)
{
    if (s_musicStream.type == MUSIC_FMT_NONE) {
        return 0;
    }

    uint32_t framesRead = 0;
    int rewound = 0;

    while (framesRead < framesWanted) {
        size_t nBytes = (framesWanted - framesRead) * 4; /* stereo s16 = 4 bytes/frame */
        size_t bytesDone = 0;

        if (s_musicStream.type == MUSIC_FMT_ADX) {
            bytesDone = Adx_Read(&s_musicStream.adxDec, (uint8_t *)(outPcm + framesRead * 2), nBytes);
        }
        else if (s_musicStream.type == MUSIC_FMT_ADP) {
            bytesDone = Adp_Read(&s_musicStream.adpDec, (uint8_t *)(outPcm + framesRead * 2), nBytes);
        }
        else if (s_musicStream.type == MUSIC_FMT_SON || s_musicStream.type == MUSIC_FMT_WAV) {
            if (s_musicStream.fp) {
                size_t r = fread(outPcm + framesRead * 2, 2, (framesWanted - framesRead) * 2, s_musicStream.fp);
                bytesDone = r * 2;
#if defined(__BIG_ENDIAN__) || defined(__PPU__)
                for (size_t k = 0; k < r; k++) {
                    uint16_t v = (uint16_t)outPcm[framesRead * 2 + k];
                    outPcm[framesRead * 2 + k] = (int16_t)((v << 8) | (v >> 8));
                }
#endif
            }
        }

        uint32_t fGot = (uint32_t)(bytesDone / 4);
        if (fGot > 0) {
            framesRead += fGot;
        } else {
            if (s_currentTrackIsFanfare || rewound) {
                break;
            }
            rewound = 1;
            if (s_musicStream.type == MUSIC_FMT_ADX) {
                Adx_Rewind(&s_musicStream.adxDec);
            } else if (s_musicStream.type == MUSIC_FMT_ADP) {
                Adp_Rewind(&s_musicStream.adpDec);
            } else if (s_musicStream.fp) {
                fseek(s_musicStream.fp, s_musicStream.dataStart, SEEK_SET);
            } else {
                break;
            }
        }
    }

    return framesRead;
}

/* =====================================================================
 * Asynchronous BGM I/O Thread
 * Processes pending track changes and refills streamCache buffers off-main-thread.
 * ===================================================================== */
static void ps3_bgm_io_thread(uint64_t arg)
{
    (void)arg;

    while (!s_audioTerminate) {
        /* Process pending track requests in background IO thread */
        if (s_pendingTrackCmd != -1) {
            int cmd = s_pendingTrackCmd;
            s_pendingTrackCmd = -1;

            sys_mutex_lock(s_bgmIoMutex, 0);
            sys_mutex_lock(s_audioMutex, 0);

            CloseMusicStream_NoLock();

            if (cmd > 0) {
                if (OpenMusicTrack_NoLock(cmd)) {
                    s_currentTrack = cmd;
                    s_currentTrackIsFanfare = (cmd == 2 || cmd == 3 || cmd == 4 ||
                                               cmd == 0x13 || cmd == 0x14 || cmd == 0x15);
                    s_logicalTrack = cmd;
                    s_trackStartMs = GetTimeMs();
                } else {
                    s_currentTrack = 0;
                    s_currentTrackIsFanfare = 0;
                    s_logicalTrack = 0;
                }
            } else {
                s_currentTrack = 0;
                s_currentTrackIsFanfare = 0;
                s_logicalTrack = 0;
                SFX_DuckStop();
            }

            sys_mutex_unlock(s_audioMutex);
            sys_mutex_unlock(s_bgmIoMutex);
        }

        /* Buffer refill */
        sys_mutex_lock(s_bgmIoMutex, 0);

        if (s_musicStream.type != MUSIC_FMT_NONE) {
            for (int b = 0; b < 2; b++) {
                if (s_musicStream.bufferBusy[b]) {
                    int16_t *bufPtr = s_musicStream.streamCache + (b * BGM_BUFFER_SIZE_FRAMES * 2);
                    uint32_t readCount = Music_ReadFrames_Internal(bufPtr, BGM_BUFFER_SIZE_FRAMES);

                    sys_mutex_lock(s_audioMutex, 0);
                    s_musicStream.streamCacheValid[b] = readCount;
                    s_musicStream.bufferBusy[b] = 0;
                    sys_mutex_unlock(s_audioMutex);
                }
            }
        }

        sys_mutex_unlock(s_bgmIoMutex);
        sys_timer_usleep(2000); /* 2ms sleep */
    }

    sys_ppu_thread_exit(0);
}

/* =====================================================================
 * Audio Mixing Callback Thread
 * Runs whenever Cell Audio signals an event queue notification.
 * Uses 50ms timeout so thread terminates cleanly on shutdown.
 * ===================================================================== */
static void ps3_audio_thread(uint64_t arg)
{
    (void)arg;
    sys_event_t event;

    static float mixBufferFloat[CELL_AUDIO_BLOCK_SAMPLES * 2] __attribute__((aligned(16)));
    static float sfxAccumLeft[CELL_AUDIO_BLOCK_SAMPLES];
    static float sfxAccumRight[CELL_AUDIO_BLOCK_SAMPLES];

    while (!s_audioTerminate) {
        int ret = sys_event_queue_receive(s_audioEventQueue, &event, 50000); /* 50ms timeout */
        if (s_audioTerminate) {
            break;
        }
        if (ret != CELL_OK && ret != 0x8001000C) { /* 0x8001000C = SYS_ETIMEDOUT */
            continue;
        }

        sys_mutex_lock(s_audioMutex, 0);

        memset(sfxAccumLeft, 0, sizeof(sfxAccumLeft));
        memset(sfxAccumRight, 0, sizeof(sfxAccumRight));

        /* -------------------------------------------------------------
         * 1. Mix SFX slots
         * ------------------------------------------------------------- */
        for (int i = 0; i < SFX_MAX_SLOTS; i++) {
            PS3_SfxSlot *slot = &s_sfx[i];
            if (!slot->isPlaying || slot->samples == NULL || slot->totalFrames == 0) {
                continue;
            }

            int targetHz = slot->targetFreq > 0 ? slot->targetFreq : (int)slot->nativeRate;
            double step = (double)targetHz / (double)PS3_AUDIO_SAMPLE_RATE;
            float gainL = slot->volume * slot->panLeft;
            float gainR = slot->volume * slot->panRight;

            for (uint32_t f = 0; f < CELL_AUDIO_BLOCK_SAMPLES; f++) {
                uint32_t idx0 = (uint32_t)slot->phase;
                if (idx0 >= slot->totalFrames) {
                    if (slot->isLooping) {
                        slot->phase = fmod(slot->phase, (double)slot->totalFrames);
                        idx0 = (uint32_t)slot->phase;
                    } else {
                        slot->isPlaying = 0;
                        break;
                    }
                }

                uint32_t idx1 = idx0 + 1;
                if (idx1 >= slot->totalFrames) {
                    idx1 = slot->isLooping ? 0 : idx0;
                }
                double frac = slot->phase - (double)idx0;

                float sampleL = 0.0f, sampleR = 0.0f;
                if (slot->channels == 2) {
                    int16_t s0_l = slot->samples[idx0 * 2];
                    int16_t s1_l = slot->samples[idx1 * 2];
                    int16_t s0_r = slot->samples[idx0 * 2 + 1];
                    int16_t s1_r = slot->samples[idx1 * 2 + 1];

                    sampleL = ((float)s0_l + (float)(s1_l - s0_l) * (float)frac) / 32768.0f;
                    sampleR = ((float)s0_r + (float)(s1_r - s0_r) * (float)frac) / 32768.0f;
                } else {
                    int16_t s0 = slot->samples[idx0];
                    int16_t s1 = slot->samples[idx1];
                    float s = ((float)s0 + (float)(s1 - s0) * (float)frac) / 32768.0f;
                    sampleL = s;
                    sampleR = s;
                }

                sfxAccumLeft[f]  += sampleL * gainL;
                sfxAccumRight[f] += sampleR * gainR;

                slot->phase += step;
            }
        }

        /* -------------------------------------------------------------
         * 2. Mix Music Stream from async double-buffer
         * ------------------------------------------------------------- */
        float musicVol = Music_EffectiveVolume();

        for (uint32_t f = 0; f < CELL_AUDIO_BLOCK_SAMPLES; f++) {
            float mLeft = 0.0f, mRight = 0.0f;

            if (s_musicStream.type != MUSIC_FMT_NONE && musicVol > 0.001f) {
                double mStep = (double)s_musicStream.sampleRate / (double)PS3_AUDIO_SAMPLE_RATE;
                uint32_t activeBuf = s_musicStream.activeBuffer;

                if (s_musicStream.streamCachePos >= s_musicStream.streamCacheValid[activeBuf]) {
                    uint32_t nextBuf = 1 - activeBuf;
                    s_musicStream.bufferBusy[activeBuf] = 1;

                    if (!s_musicStream.bufferBusy[nextBuf] && s_musicStream.streamCacheValid[nextBuf] > 0) {
                        s_musicStream.activeBuffer = nextBuf;
                        activeBuf = nextBuf;
                        s_musicStream.streamCachePos = 0;
                        s_musicPhase = 0.0;
                    }
                }

                if (s_musicStream.streamCachePos < s_musicStream.streamCacheValid[activeBuf]) {
                    int16_t *curBuf = s_musicStream.streamCache + (activeBuf * BGM_BUFFER_SIZE_FRAMES * 2);
                    uint32_t cIdx0 = s_musicStream.streamCachePos + (uint32_t)s_musicPhase;

                    if (cIdx0 < s_musicStream.streamCacheValid[activeBuf]) {
                        uint32_t cIdx1 = cIdx0 + 1;
                        if (cIdx1 >= s_musicStream.streamCacheValid[activeBuf]) cIdx1 = cIdx0;
                        double frac = s_musicPhase - floor(s_musicPhase);

                        int16_t s0_l = curBuf[cIdx0 * 2];
                        int16_t s1_l = curBuf[cIdx1 * 2];
                        int16_t s0_r = curBuf[cIdx0 * 2 + 1];
                        int16_t s1_r = curBuf[cIdx1 * 2 + 1];

                        mLeft  = (((float)s0_l + (float)(s1_l - s0_l) * (float)frac) / 32768.0f) * musicVol;
                        mRight = (((float)s0_r + (float)(s1_r - s0_r) * (float)frac) / 32768.0f) * musicVol;
                    }

                    s_musicPhase += mStep;
                    while (s_musicPhase >= 1.0) {
                        s_musicPhase -= 1.0;
                        s_musicStream.streamCachePos++;
                    }
                }
            }

            /* Combine SFX + Music */
            float outL = sfxAccumLeft[f]  + mLeft;
            float outR = sfxAccumRight[f] + mRight;

            /* Hard clip [-1.0f, 1.0f] */
            if (outL > 1.0f)  outL = 1.0f;
            if (outL < -1.0f) outL = -1.0f;
            if (outR > 1.0f)  outR = 1.0f;
            if (outR < -1.0f) outR = -1.0f;

            mixBufferFloat[f * 2]     = outL;
            mixBufferFloat[f * 2 + 1] = outR;
        }

        sys_mutex_unlock(s_audioMutex);

        /* Send 256 stereo frames to Cell Audio port */
        cellAudioAdd2chData(s_audioPortNum, mixBufferFloat, CELL_AUDIO_BLOCK_SAMPLES, 1.0f);
    }

    sys_ppu_thread_exit(0);
}

/* =====================================================================
 * Init & Shutdown
 * ===================================================================== */
int platform_audio_init(void)
{
    if (s_cellAudioInitialized) {
        return 0;
    }

    printf("[PS3 CELL AUDIO] Initializing cellAudioInit()...\n");
    int res = cellAudioInit();
    if (res != CELL_OK && res != CELL_AUDIO_ERROR_ALREADY_INIT) {
        printf("[PS3 CELL AUDIO] ERROR: cellAudioInit failed 0x%08x\n", res);
        return -1;
    }

    sys_mutex_attribute_t attr;
    sys_mutex_attribute_initialize(attr);
    attr.attr_recursive = SYS_SYNC_RECURSIVE;
    sys_mutex_create(&s_audioMutex, &attr);
    sys_mutex_create(&s_bgmIoMutex, &attr);

    CellAudioPortParam portParam;
    memset(&portParam, 0, sizeof(CellAudioPortParam));
    portParam.nChannel = CELL_AUDIO_PORT_2CH;
    portParam.nBlock = 32;
    portParam.attr = CELL_AUDIO_PORTATTR_INITLEVEL;
    portParam.level = 1.0f;

    res = cellAudioPortOpen(&portParam, &s_audioPortNum);
    if (res != CELL_OK) {
        printf("[PS3 CELL AUDIO] ERROR: cellAudioPortOpen failed 0x%08x\n", res);
        return -1;
    }

    res = cellAudioCreateNotifyEventQueue(&s_audioEventQueue, &s_audioEventKey);
    if (res != CELL_OK) {
        printf("[PS3 CELL AUDIO] ERROR: cellAudioCreateNotifyEventQueue failed 0x%08x\n", res);
        return -1;
    }

    res = cellAudioSetNotifyEventQueue(s_audioEventKey);
    if (res != CELL_OK) {
        printf("[PS3 CELL AUDIO] ERROR: cellAudioSetNotifyEventQueue failed 0x%08x\n", res);
        return -1;
    }

    s_audioTerminate = 0;
    res = sys_ppu_thread_create(&s_audioThreadHandle, ps3_audio_thread, 0, 500, 128 * 1024, SYS_PPU_THREAD_CREATE_JOINABLE, "AudioThread");
    if (res != CELL_OK) {
        printf("[PS3 CELL AUDIO] ERROR: sys_ppu_thread_create AudioThread failed 0x%08x\n", res);
        return -1;
    }

    res = sys_ppu_thread_create(&s_bgmIoThreadHandle, ps3_bgm_io_thread, 0, 1000, 128 * 1024, SYS_PPU_THREAD_CREATE_JOINABLE, "BgmIoThread");
    if (res != CELL_OK) {
        printf("[PS3 CELL AUDIO] ERROR: sys_ppu_thread_create BgmIoThread failed 0x%08x\n", res);
        return -1;
    }

    res = cellAudioPortStart(s_audioPortNum);
    if (res != CELL_OK) {
        printf("[PS3 CELL AUDIO] ERROR: cellAudioPortStart failed 0x%08x\n", res);
        return -1;
    }

    s_cellAudioInitialized = 1;
    printf("[PS3 CELL AUDIO] Audio subsystem initialized successfully!\n");
    return 0;
}

void platform_audio_shutdown(void)
{
    if (!s_cellAudioInitialized) {
        return;
    }

    s_audioTerminate = 1;
    if (s_audioThreadHandle) {
        uint64_t exitCode;
        sys_ppu_thread_join(s_audioThreadHandle, &exitCode);
        s_audioThreadHandle = 0;
    }
    if (s_bgmIoThreadHandle) {
        uint64_t exitCode;
        sys_ppu_thread_join(s_bgmIoThreadHandle, &exitCode);
        s_bgmIoThreadHandle = 0;
    }

    CloseCDDevice();
    CloseDirectSound();

    if (s_audioPortNum != 0xFFFFFFFF) {
        cellAudioPortStop(s_audioPortNum);
        cellAudioPortClose(s_audioPortNum);
        s_audioPortNum = 0xFFFFFFFF;
    }

    cellAudioRemoveNotifyEventQueue(s_audioEventKey);
    cellAudioQuit();
    sys_mutex_destroy(s_audioMutex);
    sys_mutex_destroy(s_bgmIoMutex);

    s_cellAudioInitialized = 0;
    printf("[PS3 CELL AUDIO] Audio subsystem shut down.\n");
}

/* =====================================================================
 * SFX Loader and Management
 * ===================================================================== */
static int LoadWAVIntoSlot(int slot, const char *filename)
{
    if (slot < 0 || slot >= SFX_MAX_SLOTS) {
        return 0;
    }

    sys_mutex_lock(s_bgmIoMutex, 0);
    sys_mutex_lock(s_audioMutex, 0);

    if (s_sfx[slot].samples != NULL) {
        free(s_sfx[slot].samples);
        s_sfx[slot].samples = NULL;
        s_sfx[slot].totalFrames = 0;
        s_sfx[slot].isPlaying = 0;
        g_soundBuffers[slot] = NULL;
        g_soundActive[slot] = 0;
    }

    FILE *fp = ps3_fOpen(filename, "rb");
    if (!fp) {
        char fullPath[512];
        snprintf(fullPath, sizeof(fullPath), "SOUND/SFX/%s", filename);
        fp = ps3_fOpen(fullPath, "rb");
    }

    if (!fp) {
        sys_mutex_unlock(s_audioMutex);
        sys_mutex_unlock(s_bgmIoMutex);
        return 0;
    }

    char id[4];
    uint32_t chunkSize = 0;
    if (fread(id, 1, 4, fp) != 4 || memcmp(id, "RIFF", 4) != 0) {
        fclose(fp);
        sys_mutex_unlock(s_audioMutex);
        sys_mutex_unlock(s_bgmIoMutex);
        return 0;
    }

    fread(&chunkSize, 4, 1, fp);
    if (fread(id, 1, 4, fp) != 4 || memcmp(id, "WAVE", 4) != 0) {
        fclose(fp);
        sys_mutex_unlock(s_audioMutex);
        sys_mutex_unlock(s_bgmIoMutex);
        return 0;
    }

    uint16_t channels = 1, bitsPerSample = 16;
    uint32_t sampleRate = 22050;
    uint32_t dataSize = 0;
    long dataPos = 0;

    while (fread(id, 1, 4, fp) == 4) {
        uint32_t sz = 0;
        if (fread(&sz, 4, 1, fp) != 1) break;
#if defined(__BIG_ENDIAN__) || defined(__PPU__)
        sz = (sz << 24) | ((sz & 0xFF00) << 8) | ((sz >> 8) & 0xFF00) | (sz >> 24);
#endif
        if (memcmp(id, "fmt ", 4) == 0) {
            uint16_t fmtTag = 1;
            fread(&fmtTag, 2, 1, fp);
            fread(&channels, 2, 1, fp);
            fread(&sampleRate, 4, 1, fp);
            uint32_t byteRate = 0; uint16_t blockAlign = 0;
            fread(&byteRate, 4, 1, fp);
            fread(&blockAlign, 2, 1, fp);
            fread(&bitsPerSample, 2, 1, fp);

#if defined(__BIG_ENDIAN__) || defined(__PPU__)
            channels = (channels << 8) | (channels >> 8);
            sampleRate = (sampleRate << 24) | ((sampleRate & 0xFF00) << 8) | ((sampleRate >> 8) & 0xFF00) | (sampleRate >> 24);
            bitsPerSample = (bitsPerSample << 8) | (bitsPerSample >> 8);
#endif
            if (sz > 16) fseek(fp, sz - 16, SEEK_CUR);
        } else if (memcmp(id, "data", 4) == 0) {
            dataSize = sz;
            dataPos = ftell(fp);
            break;
        } else {
            fseek(fp, sz + (sz & 1), SEEK_CUR);
        }
    }

    if (dataPos == 0 || dataSize == 0) {
        fclose(fp);
        sys_mutex_unlock(s_audioMutex);
        sys_mutex_unlock(s_bgmIoMutex);
        return 0;
    }

    fseek(fp, dataPos, SEEK_SET);

    uint32_t bytesPerFrame = channels * (bitsPerSample / 8);
    uint32_t totalFrames = dataSize / bytesPerFrame;

    int16_t *pcmBuf = (int16_t *)malloc(totalFrames * channels * sizeof(int16_t));
    if (!pcmBuf) {
        fclose(fp);
        sys_mutex_unlock(s_audioMutex);
        sys_mutex_unlock(s_bgmIoMutex);
        return 0;
    }

    if (bitsPerSample == 8) {
        uint8_t *raw8 = (uint8_t *)malloc(dataSize);
        if (raw8) {
            fread(raw8, 1, dataSize, fp);
            for (uint32_t k = 0; k < totalFrames * channels; k++) {
                pcmBuf[k] = (int16_t)(((int32_t)raw8[k] - 128) << 8);
            }
            free(raw8);
        }
    } else {
        fread(pcmBuf, 2, totalFrames * channels, fp);
#if defined(__BIG_ENDIAN__) || defined(__PPU__)
        for (uint32_t k = 0; k < totalFrames * channels; k++) {
            uint16_t v = (uint16_t)pcmBuf[k];
            pcmBuf[k] = (int16_t)((v << 8) | (v >> 8));
        }
#endif
    }

    fclose(fp);

    s_sfx[slot].samples = pcmBuf;
    s_sfx[slot].totalFrames = totalFrames;
    s_sfx[slot].nativeRate = sampleRate;
    s_sfx[slot].channels = channels;
    s_sfx[slot].phase = 0.0;
    s_sfx[slot].isPlaying = 0;
    s_sfx[slot].isLooping = 0;
    s_sfx[slot].targetFreq = (int)sampleRate;
    s_sfx[slot].volume = 1.0f;
    s_sfx[slot].panLeft = 1.0f;
    s_sfx[slot].panRight = 1.0f;

    g_soundBuffers[slot] = (void *)(intptr_t)1;
    g_soundActive[slot] = 1;

    sys_mutex_unlock(s_audioMutex);
    sys_mutex_unlock(s_bgmIoMutex);
    return 1;
}

void InitDirectSound(void)
{
    platform_audio_init();

    g_lpDirectSound = (void *)(intptr_t)1;
    g_initFeatureB = 1;

    for (int i = 0; s_sfxTable[i].slot >= 0; i++) {
        LoadWAVIntoSlot(s_sfxTable[i].slot, s_sfxTable[i].filename);
    }

    SetAllSoundVolumes();
}

void CloseDirectSound(void)
{
    sys_mutex_lock(s_bgmIoMutex, 0);
    sys_mutex_lock(s_audioMutex, 0);
    for (int i = 0; i < SFX_MAX_SLOTS; i++) {
        if (s_sfx[i].samples) {
            free(s_sfx[i].samples);
            s_sfx[i].samples = NULL;
        }
        s_sfx[i].totalFrames = 0;
        s_sfx[i].isPlaying = 0;
        g_soundBuffers[i] = NULL;
        g_soundActive[i] = 0;
    }
    g_lpDirectSound = NULL;
    sys_mutex_unlock(s_audioMutex);
    sys_mutex_unlock(s_bgmIoMutex);
}

void SFX_Play(int slot, int loop, int freq)
{
    if (slot < 0 || slot >= SFX_MAX_SLOTS || s_sfx[slot].samples == NULL) {
        return;
    }

    sys_mutex_lock(s_bgmIoMutex, 0);
    sys_mutex_lock(s_audioMutex, 0);

    float vol = sfx_ds_linear(g_masterVolume);
    if (IS_REPLAY_VOICE_SLOT(slot)) {
        vol = 1.0f;
    }

    s_sfx[slot].volume = vol;
    s_sfx[slot].isLooping = loop ? 1 : 0;

    if (slot == 0xD && freq == 0) {
        freq = 22050 + 5512;
    }

    if (freq > 0) {
        s_sfx[slot].targetFreq = freq;
    } else {
        s_sfx[slot].targetFreq = (int)s_sfx[slot].nativeRate;
    }

    if (!loop || !s_sfx[slot].isPlaying) {
        s_sfx[slot].phase = 0.0;
        s_sfx[slot].isPlaying = 1;
    }

    sys_mutex_unlock(s_audioMutex);
    sys_mutex_unlock(s_bgmIoMutex);
}

void SFX_Stop(int slot)
{
    if (slot < 0 || slot >= SFX_MAX_SLOTS) {
        return;
    }
    sys_mutex_lock(s_bgmIoMutex, 0);
    sys_mutex_lock(s_audioMutex, 0);
    s_sfx[slot].isPlaying = 0;
    s_sfx[slot].phase = 0.0;
    sys_mutex_unlock(s_audioMutex);
    sys_mutex_unlock(s_bgmIoMutex);
}

void SFX_StopAll(void)
{
    sys_mutex_lock(s_bgmIoMutex, 0);
    sys_mutex_lock(s_audioMutex, 0);
    for (int i = 0; i < SFX_MAX_SLOTS; i++) {
        s_sfx[i].isPlaying = 0;
        s_sfx[i].phase = 0.0;
    }
    sys_mutex_unlock(s_audioMutex);
    sys_mutex_unlock(s_bgmIoMutex);
}

void SFX_SetVolume(int slot, int dsVolume)
{
    if (slot < 0 || slot >= SFX_MAX_SLOTS) {
        return;
    }
    sys_mutex_lock(s_bgmIoMutex, 0);
    sys_mutex_lock(s_audioMutex, 0);
    s_sfx[slot].volume = sfx_ds_linear(dsVolume);
    sys_mutex_unlock(s_audioMutex);
    sys_mutex_unlock(s_bgmIoMutex);
}

void SFX_SetPan(int slot, int dsPan)
{
    if (slot < 0 || slot >= SFX_MAX_SLOTS) {
        return;
    }
    float pan = (float)dsPan / 10000.0f;
    if (pan < -1.0f) pan = -1.0f;
    if (pan > 1.0f)  pan = 1.0f;

    sys_mutex_lock(s_bgmIoMutex, 0);
    sys_mutex_lock(s_audioMutex, 0);
    s_sfx[slot].panLeft  = (1.0f - pan) / 2.0f;
    s_sfx[slot].panRight = (1.0f + pan) / 2.0f;
    sys_mutex_unlock(s_audioMutex);
    sys_mutex_unlock(s_bgmIoMutex);
}

void SFX_SetPosition(int slot, int pos)
{
    (void)slot; (void)pos;
}

void LoadSoundEffect(const char *filename, int slot)
{
    LoadWAVIntoSlot(slot, filename);
}

int SFX_ClipDurationMs(int slot)
{
    if (slot < 0 || slot >= SFX_MAX_SLOTS || s_sfx[slot].samples == NULL) {
        return 0;
    }
    if (s_sfx[slot].nativeRate == 0) return 0;
    return (int)(((uint64_t)s_sfx[slot].totalFrames * 1000u) / (uint64_t)s_sfx[slot].nativeRate);
}

static int PlaySoundSimple(int slot)
{
    if (slot < 0 || slot >= SFX_MAX_SLOTS) return 0;
    if (g_soundActive[slot] == 0) return 0;
    if (g_optSfxVolume == 0) return 1;

    SFX_Play(slot, 0, 0);
    return 1;
}

static int PlaySoundWithParams(int slot, int distance, int freqParam)
{
    if (slot < 0 || slot >= SFX_MAX_SLOTS) return 0;
    if (g_soundActive[slot] == 0) return 0;
    if (g_optSfxVolume == 0) return 1;

    if (distance != 0x100) {
        int d = distance;
        if (d < 0x40) {
            d = 0xFF;
        } else {
            d = 0xFF - d;
        }

        int volDiff = g_masterVolume - g_volumeBase;
        if (volDiff < 0) volDiff = -volDiff;

        int divisor = (g_demoMode == 2) ? 0x12c : 0xff;
        int attenVol = (d * volDiff) / divisor + g_volumeBase;

        SFX_SetVolume(slot, attenVol);
    }

    if (freqParam != 0) {
        freqParam = (freqParam * 99900) / 255 + 100;
    }

    SFX_Play(slot, 1, freqParam);
    return 1;
}

static int g_ringAlternate;

void PlaySoundEffect(int soundCmd, int distance, int freqParam)
{
    int slot = soundCmd & 0xFFFF;

    if (soundCmd & 0xFFFF0000) {
        PlaySoundWithParams(slot, distance, freqParam);
    } else {
        PlaySoundSimple(slot);
    }

    if (g_demoMode == DEMO_REPLAY) {
        return;
    }

    if ((soundCmd & 0xFFFF) == 0x1D) {
        slot = 0x1D + g_ringAlternate;
        g_ringAlternate = (g_ringAlternate + 1) & 1;
    }
}

/* =====================================================================
 * CD Music Playback Implementation
 * ===================================================================== */
int OpenCDDevice(void)
{
    s_musicReady = 1;
    g_mciDeviceId = 1;
    return 1;
}

void CloseCDDevice(void)
{
    StopCD();
    s_musicReady = 0;
    g_mciDeviceId = 0;
}

void Music_SetVolume(int level)
{
    if (level < 0) level = 0;
    if (level > 8) level = 8;
    s_musicLevel = level;
}

void Music_SetDucked(int ducked)
{
    s_musicDucked = ducked ? 1 : 0;
}

/* Lock Order Rule: Caller must hold s_bgmIoMutex and s_audioMutex in order */
static void CloseMusicStream_NoLock(void)
{
    if (s_musicStream.type == MUSIC_FMT_ADX) {
        Adx_Close(&s_musicStream.adxDec);
    } else if (s_musicStream.type == MUSIC_FMT_ADP) {
        Adp_Close(&s_musicStream.adpDec);
    } else if (s_musicStream.fp) {
        fclose(s_musicStream.fp);
        s_musicStream.fp = NULL;
    }

    memset(&s_musicStream, 0, sizeof(MusicStream));
    s_musicPhase = 0.0;
}

/* Lock Order Rule: Caller must hold s_bgmIoMutex and s_audioMutex in order */
static int OpenMusicTrack_NoLock(int trackNum)
{
    char path[512];

    /* 1. Try ADX */
    snprintf(path, sizeof(path), "MUSIC/track%d.adx", trackNum);
    if (Adx_Open(&s_musicStream.adxDec, path) != 0) {
        snprintf(path, sizeof(path), "MUSIC/track%02d.adx", trackNum);
        Adx_Open(&s_musicStream.adxDec, path);
    }
    if (s_musicStream.adxDec.fp != NULL) {
        s_musicStream.type = MUSIC_FMT_ADX;
        s_musicStream.sampleRate = s_musicStream.adxDec.sampleRate;
        s_musicStream.channels = s_musicStream.adxDec.channels;
        s_musicStream.totalFrames = s_musicStream.adxDec.totalSamples;
        s_musicStream.bufferBusy[0] = 1;
        s_musicStream.bufferBusy[1] = 1;
        s_musicStream.activeBuffer = 0;
        s_musicStream.streamCachePos = 0;
        return 1;
    }

    /* 2. Try ADP */
    snprintf(path, sizeof(path), "MUSIC/track%d.adp", trackNum);
    if (Adp_Open(&s_musicStream.adpDec, path, 44100, 2) != 0) {
        snprintf(path, sizeof(path), "MUSIC/track%02d.adp", trackNum);
        Adp_Open(&s_musicStream.adpDec, path, 44100, 2);
    }
    if (s_musicStream.adpDec.fp != NULL) {
        s_musicStream.type = MUSIC_FMT_ADP;
        s_musicStream.sampleRate = 44100;
        s_musicStream.channels = 2;
        s_musicStream.totalFrames = (uint32_t)(Adp_PcmBytes(&s_musicStream.adpDec) / 4);
        s_musicStream.bufferBusy[0] = 1;
        s_musicStream.bufferBusy[1] = 1;
        s_musicStream.activeBuffer = 0;
        s_musicStream.streamCachePos = 0;
        return 1;
    }

    /* 3. Try SON */
    snprintf(path, sizeof(path), "MUSIC/track%d.son", trackNum);
    FILE *fp = ps3_fOpen(path, "rb");
    if (!fp) {
        snprintf(path, sizeof(path), "MUSIC/track%02d.son", trackNum);
        fp = ps3_fOpen(path, "rb");
    }
    if (fp) {
        fseek(fp, 0, SEEK_END);
        long sz = ftell(fp);
        fseek(fp, 0, SEEK_SET);

        s_musicStream.type = MUSIC_FMT_SON;
        s_musicStream.fp = fp;
        s_musicStream.dataStart = 0;
        s_musicStream.sampleRate = 44100;
        s_musicStream.channels = 2;
        s_musicStream.totalFrames = sz / 4;
        s_musicStream.bufferBusy[0] = 1;
        s_musicStream.bufferBusy[1] = 1;
        s_musicStream.activeBuffer = 0;
        s_musicStream.streamCachePos = 0;
        return 1;
    }

    /* 4. Try WAV / other ext */
    static const char *exts[] = { "wav", "WAV", "mp3", "flac", "ogg" };
    for (int i = 0; i < 5; i++) {
        snprintf(path, sizeof(path), "MUSIC/track%d.%s", trackNum, exts[i]);
        fp = ps3_fOpen(path, "rb");
        if (!fp) {
            snprintf(path, sizeof(path), "MUSIC/track%02d.%s", trackNum, exts[i]);
            fp = ps3_fOpen(path, "rb");
        }
        if (fp) {
            char id[4];
            if (fread(id, 1, 4, fp) == 4 && memcmp(id, "RIFF", 4) == 0) {
                fseek(fp, 8, SEEK_CUR);
                if (fread(id, 1, 4, fp) == 4 && memcmp(id, "WAVE", 4) == 0) {
                    uint32_t dataSize = 0;
                    long dataPos = 0;
                    uint32_t sRate = 44100;

                    while (fread(id, 1, 4, fp) == 4) {
                        uint32_t sz = 0;
                        if (fread(&sz, 4, 1, fp) != 1) break;
#if defined(__BIG_ENDIAN__) || defined(__PPU__)
                        sz = (sz << 24) | ((sz & 0xFF00) << 8) | ((sz >> 8) & 0xFF00) | (sz >> 24);
#endif
                        if (memcmp(id, "fmt ", 4) == 0) {
                            fseek(fp, 4, SEEK_CUR);
                            fread(&sRate, 4, 1, fp);
#if defined(__BIG_ENDIAN__) || defined(__PPU__)
                            sRate = (sRate << 24) | ((sRate & 0xFF00) << 8) | ((sRate >> 8) & 0xFF00) | (sRate >> 24);
#endif
                            if (sz > 8) fseek(fp, sz - 8, SEEK_CUR);
                        } else if (memcmp(id, "data", 4) == 0) {
                            dataSize = sz;
                            dataPos = ftell(fp);
                            break;
                        } else {
                            fseek(fp, sz + (sz & 1), SEEK_CUR);
                        }
                    }

                    if (dataPos > 0) {
                        fseek(fp, dataPos, SEEK_SET);
                        s_musicStream.type = MUSIC_FMT_WAV;
                        s_musicStream.fp = fp;
                        s_musicStream.dataStart = dataPos;
                        s_musicStream.sampleRate = sRate;
                        s_musicStream.channels = 2;
                        s_musicStream.totalFrames = dataSize / 4;
                        s_musicStream.bufferBusy[0] = 1;
                        s_musicStream.bufferBusy[1] = 1;
                        s_musicStream.activeBuffer = 0;
                        s_musicStream.streamCachePos = 0;
                        return 1;
                    }
                }
            }
            fclose(fp);
        }
    }

    return 0;
}

void PlayCD(int trackNum)
{
    if (trackNum < 2 || trackNum > 21) {
        return;
    }
    if (g_musicEnabled == 0) {
        StopCD();
        return;
    }

    if (s_currentTrack == trackNum && (s_musicStream.type != MUSIC_FMT_NONE || s_pendingTrackCmd == trackNum)) {
        s_logicalTrack = trackNum;
        s_trackStartMs = GetTimeMs();
        return;
    }

    /* Instantly queue request for background IO thread so main thread returns in 0ms! */
    s_logicalTrack = trackNum;
    s_trackStartMs = GetTimeMs();
    s_pendingTrackCmd = trackNum;
}

void StopCD(void)
{
    s_logicalTrack = 0;
    s_trackStartMs = 0;
    s_pendingTrackCmd = 0; /* 0 = request stop in background IO thread */
}

int GetLogicalCDTrack(void)
{
    if (!s_musicReady || s_logicalTrack == 0) {
        return 0;
    }

    int elapsed = (int)(GetTimeMs() - s_trackStartMs);
    if (elapsed < 0) elapsed = -elapsed;

    if (s_logicalTrack > 0 && s_logicalTrack < 23 &&
        elapsed > s_trackDurationMs[s_logicalTrack]) {
        s_logicalTrack++;
    }

    return s_logicalTrack;
}

void UpdateCDPlayback(int trackNum)
{
    if (g_musicEnabled == 0) {
        return;
    }
    PlayCD(trackNum);
}

#endif /* SONICR_PS3 */
