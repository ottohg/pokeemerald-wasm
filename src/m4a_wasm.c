// WebAssembly C reimplementation of the m4a functions that exist only as ARM
// assembly in src/m4a_1.s (which cannot be built for wasm32). This covers the
// MIDI sequencer (MPlayMain, ply_note and the ply_* command handlers) and the
// DirectSound software mixer (SoundMain/SoundMainRAM). The rest of the m4a
// engine (src/m4a.c) is ordinary C and is compiled for wasm directly.
//
// The port is faithful to the original engine's data structures and behaviour;
// it only departs from the assembly where the GBA hardware model does not apply
// (no real-time scanline budget, no Direct Sound DMA double buffering - the JS
// frontend reads the freshly mixed PCM out of SoundInfo.pcmBuffer each frame).

#include "global.h"
#include "gba/m4a_internal.h"

#if WASM

// Constants that live in constants/m4a_constants.inc on the GBA asm side.
#define TONEDATA_TYPE_REV         0x10
#define TONEDATA_TYPE_CMP         0x20
#define SOUND_CHANNEL_SF_SPECIAL  0x20
#define WAVE_DATA_FLAG_LOOP       0xC0

extern const u8 gClockTable[];
extern const u8 gCgb3Vol[];
extern u32 MidiKeyToFreq(struct WaveData *wav, u8 key, u8 fineAdjust);

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

u32 umul3232H32(u32 multiplier, u32 multiplicand)
{
    return (u32)(((u64)multiplier * (u64)multiplicand) >> 32);
}

static inline u8 TrackReadByte(struct MusicPlayerTrack *track)
{
    return *track->cmdPtr++;
}

void RealClearChain(void *x)
{
    struct SoundChannel *chan = x;
    struct MusicPlayerTrack *track = chan->track;

    if (track == NULL)
        return;

    struct SoundChannel *next = chan->nextChannelPointer;
    struct SoundChannel *prev = chan->prevChannelPointer;

    if (prev != NULL)
        prev->nextChannelPointer = next;
    else
        track->chan = next;

    if (next != NULL)
        next->prevChannelPointer = prev;

    chan->track = NULL;
}

// Clear64byte's jump-table target (BIOS SoundMainBTM): zero 64 bytes.
static void Clear64(void *x)
{
    u32 *p = x;
    for (s32 i = 0; i < 16; i++)
        p[i] = 0;
}

static void ClearModMod(struct MusicPlayerTrack *track)
{
    track->modM = 0;
    track->lfoSpeedC = 0;
    if (track->modT == 0)
        track->flags |= MPT_FLG_PITCHG;
    else
        track->flags |= MPT_FLG_VOLCHG;
}

// ---------------------------------------------------------------------------
// Track command handlers (ply_*)
// ---------------------------------------------------------------------------

void ply_fine(struct MusicPlayerInfo *mplayInfo, struct MusicPlayerTrack *track)
{
    struct SoundChannel *chan = track->chan;
    while (chan != NULL)
    {
        if (chan->statusFlags & SOUND_CHANNEL_SF_ON)
            chan->statusFlags |= SOUND_CHANNEL_SF_STOP;
        struct SoundChannel *next = chan->nextChannelPointer;
        RealClearChain(chan);
        chan = next;
    }
    track->flags = 0;
}

void ply_goto(struct MusicPlayerInfo *mplayInfo, struct MusicPlayerTrack *track)
{
    u8 *base = track->cmdPtr;
    u32 addr = base[0] | (base[1] << 8) | (base[2] << 16) | (base[3] << 24);
    track->cmdPtr = (u8 *)addr;
}

void ply_patt(struct MusicPlayerInfo *mplayInfo, struct MusicPlayerTrack *track)
{
    u8 level = track->patternLevel;
    if (level >= 3)
    {
        ply_fine(mplayInfo, track);
        return;
    }
    track->patternStack[level] = track->cmdPtr + 4;
    track->patternLevel = level + 1;
    ply_goto(mplayInfo, track);
}

void ply_pend(struct MusicPlayerInfo *mplayInfo, struct MusicPlayerTrack *track)
{
    if (track->patternLevel != 0)
    {
        u8 level = track->patternLevel - 1;
        track->patternLevel = level;
        track->cmdPtr = track->patternStack[level];
    }
}

void ply_rept(struct MusicPlayerInfo *mplayInfo, struct MusicPlayerTrack *track)
{
    if (track->cmdPtr[0] == 0)
    {
        track->cmdPtr += 1;
        ply_goto(mplayInfo, track);
        return;
    }
    if (++track->repN < track->cmdPtr[0])
    {
        track->cmdPtr += 1;
        ply_goto(mplayInfo, track);
        return;
    }
    track->repN = 0;
    track->cmdPtr += 5;
}

void ply_prio(struct MusicPlayerInfo *mplayInfo, struct MusicPlayerTrack *track)
{
    track->priority = TrackReadByte(track);
}

void ply_tempo(struct MusicPlayerInfo *mplayInfo, struct MusicPlayerTrack *track)
{
    u16 bpm = TrackReadByte(track) << 1;
    mplayInfo->tempoD = bpm;
    mplayInfo->tempoI = (bpm * mplayInfo->tempoU) >> 8;
}

void ply_keysh(struct MusicPlayerInfo *mplayInfo, struct MusicPlayerTrack *track)
{
    track->keyShift = TrackReadByte(track);
    track->flags |= MPT_FLG_PITCHG | MPT_FLG_PITSET;
}

void ply_voice(struct MusicPlayerInfo *mplayInfo, struct MusicPlayerTrack *track)
{
    u8 voice = TrackReadByte(track);
    const struct ToneData *tone = &mplayInfo->tone[voice];
    track->tone = *tone;
}

void ply_vol(struct MusicPlayerInfo *mplayInfo, struct MusicPlayerTrack *track)
{
    track->vol = TrackReadByte(track);
    track->flags |= MPT_FLG_VOLCHG;
}

void ply_pan(struct MusicPlayerInfo *mplayInfo, struct MusicPlayerTrack *track)
{
    track->pan = (s8)(TrackReadByte(track) - C_V);
    track->flags |= MPT_FLG_VOLCHG;
}

void ply_bend(struct MusicPlayerInfo *mplayInfo, struct MusicPlayerTrack *track)
{
    track->bend = (s8)(TrackReadByte(track) - C_V);
    track->flags |= MPT_FLG_PITCHG;
}

void ply_bendr(struct MusicPlayerInfo *mplayInfo, struct MusicPlayerTrack *track)
{
    track->bendRange = TrackReadByte(track);
    track->flags |= MPT_FLG_PITCHG;
}

void ply_lfodl(struct MusicPlayerInfo *mplayInfo, struct MusicPlayerTrack *track)
{
    track->lfoDelay = TrackReadByte(track);
}

void ply_modt(struct MusicPlayerInfo *mplayInfo, struct MusicPlayerTrack *track)
{
    u8 type = TrackReadByte(track);
    if (track->modT != type)
    {
        track->modT = type;
        track->flags |= MPT_FLG_VOLCHG | MPT_FLG_PITCHG;
    }
}

void ply_tune(struct MusicPlayerInfo *mplayInfo, struct MusicPlayerTrack *track)
{
    track->tune = (s8)(TrackReadByte(track) - C_V);
    track->flags |= MPT_FLG_PITCHG;
}

void ply_port(struct MusicPlayerInfo *mplayInfo, struct MusicPlayerTrack *track)
{
    // This command (jump-table slot 27) is a raw CGB-register poke: byte 0 is an
    // offset from REG_SOUND1CNT_L and byte 1 the value written there (see ply_port
    // in m4a_1.s). It is not used by standard song data, and the registers it would
    // target don't exist under the software-synthesised CGB path here, so we only
    // consume the two bytes to keep the command stream in sync.
    TrackReadByte(track);
    TrackReadByte(track);
}

void ply_lfos(struct MusicPlayerInfo *mplayInfo, struct MusicPlayerTrack *track)
{
    track->lfoSpeed = TrackReadByte(track);
    if (track->lfoSpeed == 0)
        ClearModMod(track);
}

void ply_mod(struct MusicPlayerInfo *mplayInfo, struct MusicPlayerTrack *track)
{
    track->mod = TrackReadByte(track);
    if (track->mod == 0)
        ClearModMod(track);
}

void ply_endtie(struct MusicPlayerInfo *mplayInfo, struct MusicPlayerTrack *track)
{
    u8 key = track->cmdPtr[0];
    if (key < 0x80)
    {
        track->key = key;
        track->cmdPtr += 1;
    }
    else
    {
        key = track->key;
    }

    struct SoundChannel *chan = track->chan;
    while (chan != NULL)
    {
        if ((chan->statusFlags & (SOUND_CHANNEL_SF_START | SOUND_CHANNEL_SF_ENV))
            && !(chan->statusFlags & SOUND_CHANNEL_SF_STOP)
            && chan->midiKey == key)
        {
            chan->statusFlags |= SOUND_CHANNEL_SF_STOP;
            return;
        }
        chan = chan->nextChannelPointer;
    }
}

static void ChnVolSetAsm(struct SoundChannel *chan, struct MusicPlayerTrack *track)
{
    s32 vel = chan->velocity;
    s32 pan = (s8)chan->rhythmPan;

    s32 right = (track->volMR * ((0x80 + pan) * vel)) >> 14;
    chan->rightVolume = (right > 0xFF) ? 0xFF : right;

    s32 left = (track->volML * ((0x7F - pan) * vel)) >> 14;
    chan->leftVolume = (left > 0xFF) ? 0xFF : left;
}

// ---------------------------------------------------------------------------
// ply_note: allocate a Direct Sound (or CGB) channel and start a note.
// ---------------------------------------------------------------------------

void ply_note(u32 noteCmd, struct MusicPlayerInfo *mplayInfo, struct MusicPlayerTrack *track)
{
    struct SoundInfo *soundInfo = SOUND_INFO_PTR;

    track->gateTime = gClockTable[noteCmd];

    // Read key / velocity / optional gate-time extension from the stream.
    if (track->cmdPtr[0] < 0x80)
    {
        track->key = track->cmdPtr[0];
        track->cmdPtr++;
        if (track->cmdPtr[0] < 0x80)
        {
            track->velocity = track->cmdPtr[0];
            track->cmdPtr++;
            if (track->cmdPtr[0] < 0x80)
            {
                track->gateTime += track->cmdPtr[0];
                track->cmdPtr++;
            }
        }
    }

    s32 rhythmPan = 0;
    struct ToneData *tone = &track->tone;
    const struct ToneData *toneFinal = tone;

    if (tone->type & (TONEDATA_TYPE_RHY | TONEDATA_TYPE_SPL))
    {
        u8 key = track->key;
        u32 index;
        if (tone->type & TONEDATA_TYPE_SPL)
        {
            // Key-split table pointer is stored in the ToneData attack slot
            // (offset 8); it maps each MIDI key to a sub-voice index.
            u8 *keysplit = *(u8 **)&tone->attack;
            index = keysplit[key];
        }
        else
        {
            index = key;
        }
        // The split/rhythm sub-voice table is pointed to by tone->wav.
        struct ToneData *sub = &((struct ToneData *)tone->wav)[index];
        toneFinal = sub;
        if (sub->type & (TONEDATA_TYPE_SPL | TONEDATA_TYPE_RHY))
            return; // nested split is invalid
        if (tone->type & TONEDATA_TYPE_RHY)
        {
            u8 ps = sub->pan_sweep;
            if (ps & 0x80)
                rhythmPan = (s32)((ps - TONEDATA_P_S_PAN) << 1);
        }
    }

    // Rhythm sub-voices play at a FIXED pitch stored in toneFinal->key (each
    // percussion hit has its own root key and ignores the track key). Key-split
    // melodic instruments select a sample based on the note but still play at
    // the track pitch — using toneFinal->key (60 for every trumpet sub-voice)
    // here made all keysplit notes sound like Cn3 regardless of what was played.
    s32 keyForFreq = (tone->type & TONEDATA_TYPE_RHY)
                         ? toneFinal->key
                         : track->key;

    s32 priority = mplayInfo->priority + track->priority;
    if (priority > 0xFF)
        priority = 0xFF;

    u32 cgbType = toneFinal->type & TONEDATA_TYPE_CGB;

    struct SoundChannel *chan = NULL;

    if (cgbType != 0)
    {
        struct CgbChannel *cgb = soundInfo->cgbChans;
        if (cgb == NULL)
            return;
        cgb += cgbType - 1;
        if ((cgb->statusFlags & SOUND_CHANNEL_SF_ON)
            && !(cgb->statusFlags & SOUND_CHANNEL_SF_STOP))
        {
            if (cgb->priority > priority)
                return;
            if (cgb->priority == priority && (struct MusicPlayerTrack *)cgb->track < track)
                return;
        }
        chan = (struct SoundChannel *)cgb;
    }
    else
    {
        // Direct Sound: pick a free channel, else steal the lowest-priority one.
        s32 maxChans = soundInfo->maxChans;
        struct SoundChannel *chans = soundInfo->chans;
        s32 bestPriority = priority;
        struct MusicPlayerTrack *bestTrack = track;
        s32 stopFound = 0;
        for (s32 i = 0; i < maxChans; i++)
        {
            struct SoundChannel *c = &chans[i];
            if (!(c->statusFlags & SOUND_CHANNEL_SF_ON))
            {
                chan = c;
                break;
            }
            if (c->statusFlags & SOUND_CHANNEL_SF_STOP)
            {
                // Releasing channels are preferred steal targets. The first one
                // switches the search into "stopping only" mode; subsequent
                // releasing channels still compete on priority/track below
                // (matching ply_note in m4a_1.s, which keeps scanning them).
                if (!stopFound)
                {
                    stopFound = 1;
                    bestPriority = c->priority;
                    bestTrack = c->track;
                    chan = c;
                    continue;
                }
            }
            else if (stopFound)
            {
                // Once a releasing channel is in hand, never steal a playing one.
                continue;
            }
            if (c->priority < bestPriority)
            {
                bestPriority = c->priority;
                bestTrack = c->track;
                chan = c;
            }
            else if (c->priority == bestPriority && c->track > bestTrack)
            {
                bestTrack = c->track;
                chan = c;
            }
        }
        if (chan == NULL)
            return;
    }

    RealClearChain(chan);
    chan->prevChannelPointer = NULL;
    chan->nextChannelPointer = track->chan;
    if (track->chan != NULL)
        track->chan->prevChannelPointer = chan;
    track->chan = chan;
    chan->track = track;

    track->lfoDelayC = track->lfoDelay;
    if (track->lfoDelay != 0)
        ClearModMod(track);

    TrkVolPitSet(mplayInfo, track);

    chan->gateTime = track->gateTime;
    chan->midiKey = track->key;
    chan->velocity = track->velocity;
    chan->priority = priority;
    chan->key = keyForFreq;
    chan->rhythmPan = rhythmPan;
    chan->type = toneFinal->type;
    chan->wav = toneFinal->wav;
    chan->attack = toneFinal->attack;
    chan->decay = toneFinal->decay;
    chan->sustain = toneFinal->sustain;
    chan->release = toneFinal->release;
    chan->pseudoEchoVolume = track->pseudoEchoVolume;
    chan->pseudoEchoLength = track->pseudoEchoLength;

    ChnVolSetAsm(chan, track);

    s32 midiKey = chan->key + track->keyM;
    if (midiKey < 0)
        midiKey = 0;

    if (cgbType != 0)
    {
        chan->count = 0;
        chan->frequency = soundInfo->MidiKeyToCgbFreq((u8)cgbType, (u8)midiKey, track->pitM);
    }
    else
    {
        chan->count = track->unk_3C;
        chan->frequency = MidiKeyToFreq(chan->wav, midiKey, track->pitM);
    }

    chan->statusFlags = SOUND_CHANNEL_SF_START;
    track->flags &= 0xF0;
}

// ---------------------------------------------------------------------------
// Jump table + TrackStop
// ---------------------------------------------------------------------------

void TrackStop(struct MusicPlayerInfo *mplayInfo, struct MusicPlayerTrack *track)
{
    if (!(track->flags & MPT_FLG_EXIST))
        return;

    struct SoundChannel *chan = track->chan;
    while (chan != NULL)
    {
        if (chan->statusFlags != 0)
        {
            if (chan->type & TONEDATA_TYPE_CGB)
            {
                struct SoundInfo *soundInfo = SOUND_INFO_PTR;
                soundInfo->CgbOscOff(((struct CgbChannel *)chan)->type);
            }
            chan->statusFlags = 0;
        }
        chan->track = NULL;
        chan = chan->nextChannelPointer;
    }
    track->chan = NULL;
}

void MPlayJumpTableCopy(MPlayFunc *t)
{
    for (s32 i = 0; i < 36; i++)
        t[i] = (MPlayFunc)ply_fine;

    t[0]  = (MPlayFunc)ply_fine;
    t[1]  = (MPlayFunc)ply_goto;
    t[2]  = (MPlayFunc)ply_patt;
    t[3]  = (MPlayFunc)ply_pend;
    t[4]  = (MPlayFunc)ply_rept;
    t[8]  = (MPlayFunc)ply_memacc;
    t[9]  = (MPlayFunc)ply_prio;
    t[10] = (MPlayFunc)ply_tempo;
    t[11] = (MPlayFunc)ply_keysh;
    t[12] = (MPlayFunc)ply_voice;
    t[13] = (MPlayFunc)ply_vol;
    t[14] = (MPlayFunc)ply_pan;
    t[15] = (MPlayFunc)ply_bend;
    t[16] = (MPlayFunc)ply_bendr;
    t[17] = (MPlayFunc)ply_lfos;
    t[18] = (MPlayFunc)ply_lfodl;
    t[19] = (MPlayFunc)ply_mod;
    t[20] = (MPlayFunc)ply_modt;
    t[23] = (MPlayFunc)ply_tune;
    t[27] = (MPlayFunc)ply_port;
    t[28] = (MPlayFunc)ply_xcmd;
    t[29] = (MPlayFunc)ply_endtie;
    t[30] = (MPlayFunc)SampleFreqSet;
    t[31] = (MPlayFunc)TrackStop;
    t[32] = (MPlayFunc)FadeOutBody;
    t[33] = (MPlayFunc)TrkVolPitSet;
    t[34] = (MPlayFunc)RealClearChain;
    t[35] = (MPlayFunc)Clear64;
}

// ---------------------------------------------------------------------------
// MPlayMain: advance one music player by the elapsed tempo ticks.
// ---------------------------------------------------------------------------

void MPlayMain(struct MusicPlayerInfo *mplayInfo)
{
    struct SoundInfo *soundInfo = SOUND_INFO_PTR;

    if (mplayInfo->ident != ID_NUMBER)
        return;
    mplayInfo->ident++;

    // A negative status means the player is paused (MUSICPLAYER_STATUS_PAUSE,
    // bit 31): skip all processing. Otherwise always advance any active fade,
    // and bail out if the fade just completed and paused the player. This
    // mirrors the original MPlayMain (m4a_1.s); inverting it leaves fades
    // (FadeOutBGM, title-screen music stop, cutscene transitions) stuck.
    if ((s32)mplayInfo->status < 0)
    {
        mplayInfo->ident = ID_NUMBER;
        return;
    }

    FadeOutBody(mplayInfo);
    if ((s32)mplayInfo->status < 0)
    {
        mplayInfo->ident = ID_NUMBER;
        return;
    }

    mplayInfo->tempoC += mplayInfo->tempoI;

    while (mplayInfo->tempoC >= 150)
    {
        mplayInfo->tempoC -= 150;

        u32 trackBits = 0;
        u32 trackCount = mplayInfo->trackCount;
        struct MusicPlayerTrack *track = mplayInfo->tracks;

        for (u32 i = 0; i < trackCount; i++, track++)
        {
            if (!(track->flags & MPT_FLG_EXIST))
                continue;

            trackBits |= (1u << i);

            // Tick channel gate timers; stop channels whose gate expired and
            // unlink channels that already turned off.
            struct SoundChannel *chan = track->chan;
            while (chan != NULL)
            {
                struct SoundChannel *next = chan->nextChannelPointer;
                if (chan->statusFlags & SOUND_CHANNEL_SF_ON)
                {
                    if (chan->gateTime != 0 && --chan->gateTime == 0)
                        chan->statusFlags |= SOUND_CHANNEL_SF_STOP;
                }
                else
                {
                    RealClearChain(chan);
                }
                chan = next;
            }

            if (track->flags & MPT_FLG_START)
            {
                Clear64(track);
                track->flags = MPT_FLG_EXIST;
                track->bendRange = 2;
                track->volX = 0x40;
                track->lfoSpeed = 0x16;
                track->tone.type = 1;
            }

            // Run commands until a wait is queued.
            while (track->wait == 0)
            {
                u8 cmd = track->cmdPtr[0];
                if (cmd < 0x80)
                {
                    cmd = track->runningStatus;
                }
                else
                {
                    track->cmdPtr++;
                    if (cmd >= 0xBD)
                        track->runningStatus = cmd;
                }

                if (cmd >= 0xCF)
                {
                    ply_note(cmd - 0xCF, mplayInfo, track);
                }
                else if (cmd >= 0xB1)
                {
                    mplayInfo->cmd = cmd - 0xB1;
                    MPlayFunc fn = soundInfo->MPlayJumpTable[cmd - 0xB1];
                    ((void (*)(struct MusicPlayerInfo *, struct MusicPlayerTrack *))fn)(mplayInfo, track);
                    if (track->flags == 0)
                        break; // track ended (ply_fine)
                }
                else
                {
                    track->wait = gClockTable[cmd - 0x80];
                }
            }

            if (track->flags == 0)
                continue;

            if (track->wait != 0)
                track->wait--;

            // LFO / modulation update.
            if (track->lfoSpeed != 0 && track->mod != 0)
            {
                if (track->lfoDelayC != 0)
                {
                    track->lfoDelayC--;
                }
                else
                {
                    track->lfoSpeedC += track->lfoSpeed;
                    s32 r;
                    u8 c = track->lfoSpeedC;
                    if ((s8)(c - 0x40) >= 0)
                        r = (s8)(0x80 - c);
                    else
                        r = (s8)c;
                    s32 m = (track->mod * r) >> 6;
                    if ((u8)(track->modM ^ m) != 0)
                    {
                        track->modM = m;
                        if (track->modT == 0)
                            track->flags |= MPT_FLG_PITCHG;
                        else
                            track->flags |= MPT_FLG_VOLCHG;
                    }
                }
            }
        }

        mplayInfo->clock++;

        if (trackBits == 0)
        {
            mplayInfo->status = MUSICPLAYER_STATUS_PAUSE;
            mplayInfo->ident = ID_NUMBER;
            return;
        }
        mplayInfo->status = trackBits;
    }

    // Apply queued volume/pitch changes to live channels.
    u32 trackCount = mplayInfo->trackCount;
    struct MusicPlayerTrack *track = mplayInfo->tracks;
    for (u32 i = 0; i < trackCount; i++, track++)
    {
        if (!(track->flags & MPT_FLG_EXIST))
            continue;
        if (!(track->flags & (MPT_FLG_VOLCHG | MPT_FLG_PITCHG)))
            continue;

        TrkVolPitSet(mplayInfo, track);

        struct SoundChannel *chan = track->chan;
        while (chan != NULL)
        {
            struct SoundChannel *next = chan->nextChannelPointer;
            if (!(chan->statusFlags & SOUND_CHANNEL_SF_ON))
            {
                RealClearChain(chan);
            }
            else
            {
                u32 cgbType = chan->type & TONEDATA_TYPE_CGB;
                if (track->flags & MPT_FLG_VOLCHG)
                    ChnVolSetAsm(chan, track);
                if (track->flags & MPT_FLG_PITCHG)
                {
                    s32 midiKey = chan->key + track->keyM;
                    if (midiKey < 0)
                        midiKey = 0;
                    if (cgbType == 0)
                        chan->frequency = MidiKeyToFreq(chan->wav, midiKey, track->pitM);
                    else
                        chan->frequency = soundInfo->MidiKeyToCgbFreq((u8)cgbType, (u8)midiKey, track->pitM);
                }
            }
            chan = next;
        }
        track->flags &= 0xF0;
    }

    mplayInfo->ident = ID_NUMBER;
}

// ---------------------------------------------------------------------------
// SoundMainRAM: the Direct Sound software mixer.
// ---------------------------------------------------------------------------

#define MIX_MAX_SAMPLES PCM_DMA_BUF_SIZE

static s32 sMixL[MIX_MAX_SAMPLES];
static s32 sMixR[MIX_MAX_SAMPLES];

// Parallel float accumulators for the JS output path. These accumulate the
// same mix as sMixL/R but without the integer >> 8 truncation that each
// channel contribution applies to the s32 path, eliminating the ~-40 dB
// quantisation noise floor that is audible as background hiss.
static float sMixLF[MIX_MAX_SAMPLES];
static float sMixRF[MIX_MAX_SAMPLES];

// Float32 output buffers read by JS via a Float32Array view into WASM memory.
float gWasmPcmL[MIX_MAX_SAMPLES];
float gWasmPcmR[MIX_MAX_SAMPLES];
static u32 sCgbPhase[4] = {0, 0, 0, 0};
static u16 sNoiseLfsr = 0x7FFF; // CGB channel 4 shift register state

// CGB channel 1 (square + sweep) NR10 sweep-unit state. The GBA performs this
// in hardware; here it is reproduced in software over the swept frequency.
static u8  sSweepPrevOn = 0;
static u8  sSweepMuted = 0;
static u32 sSweepShadow = 0;  // 11-bit shadow frequency register
static s32 sSweepTimer = 0;   // ticks until next sweep step
static s32 sSweepTickAcc = 0; // 128 Hz tick accumulator (scaled by frame rate)

static inline s8 ClampS8(s32 v)
{
    if (v > 127) return 127;
    if (v < -128) return -128;
    return (s8)v;
}

// ---------------------------------------------------------------------------
// BDPCM (compressed) + reversed sample playback.
//
// Compressed waves store 64 samples per 33-byte block: 1 absolute base byte
// followed by 32 bytes of two 4-bit deltas each (high nibble first), indexed
// through gDeltaEncodingTable. The first byte after the base contributes only
// its low nibble (its high nibble is padding), matching SoundMainRAM_Unk2.
// chan->xpi caches the currently decoded block index into the shared buffer.
// ---------------------------------------------------------------------------

extern const s8 gDeltaEncodingTable[];

static s8 sDpcmBuf[64];

static s32 DpcmSampleAt(struct SoundChannel *chan, struct WaveData *wav, s32 idx)
{
    u32 blk = (u32)idx >> 6;
    if (blk != chan->xpi)
    {
        chan->xpi = blk;
        const u8 *src = (const u8 *)wav->data + blk * 0x21u;
        s32 acc = (s8)src[0];
        sDpcmBuf[0] = (s8)acc;
        acc += gDeltaEncodingTable[src[1] & 0xF];
        sDpcmBuf[1] = (s8)acc;
        s32 n = 2;
        const u8 *p = src + 2;
        while (n < 64)
        {
            u32 b = *p++;
            acc += gDeltaEncodingTable[b >> 4];
            sDpcmBuf[n++] = (s8)acc;
            if (n >= 64)
                break;
            acc += gDeltaEncodingTable[b & 0xF];
            sDpcmBuf[n++] = (s8)acc;
        }
    }
    return sDpcmBuf[idx & 0x3F];
}

static void MixCompressedOrReversed(struct SoundChannel *chan, struct WaveData *wav,
                                    s32 numSamples, u32 divFreq, s32 envL, s32 envR, u8 flags)
{
    s32 isCmp = chan->type & TONEDATA_TYPE_CMP;
    s32 isRev = chan->type & TONEDATA_TYPE_REV;
    s32 size = (s32)wav->size;
    s32 count = chan->count;
    s32 loopFlag = flags & SOUND_CHANNEL_SF_LOOP;
    s32 loopLen = size - (s32)wav->loopStart;

    // The hardware shares one decode buffer across channels, so the cached
    // block must be invalidated whenever this channel takes over the buffer.
    if (isCmp)
        chan->xpi = 0xFFFF;

    u32 fw = chan->fw;
    u32 inc = (chan->type & TONEDATA_TYPE_FIX) ? 0x800000u : divFreq * chan->frequency;
    s32 stopped = 0;

    for (s32 i = 0; i < numSamples; i++)
    {
        if (count <= 0)
        {
            if (loopFlag)
            {
                count += loopLen;
                if (count <= 0)
                {
                    stopped = 1;
                    break;
                }
            }
            else
            {
                stopped = 1;
                break;
            }
        }

        // pos = samples consumed; reversed playback reads from the tail back.
        s32 pos = size - count;
        s32 idx0 = isRev ? (size - 1 - pos) : pos;
        if (idx0 < 0) idx0 = 0;
        else if (idx0 >= size) idx0 = size - 1;
        s32 nidx = isRev ? (idx0 - 1) : (idx0 + 1);
        if (nidx < 0) nidx = 0;
        else if (nidx >= size) nidx = size - 1;

        s32 s0, s1;
        if (isCmp)
        {
            s0 = DpcmSampleAt(chan, wav, idx0);
            s1 = (count > 1) ? DpcmSampleAt(chan, wav, nidx) : s0;
        }
        else
        {
            s0 = wav->data[idx0];
            s1 = (count > 1) ? wav->data[nidx] : s0;
        }

        s32 interp = s0 + (((s32)fw * (s1 - s0)) >> 23);
        sMixL[i] += (interp * envL) >> 8;
        sMixR[i] += (interp * envR) >> 8;
        float interpF = (float)s0 + ((float)fw * (float)(s1 - s0)) * (1.0f/8388608.0f);
        sMixLF[i] += interpF * (float)envL * (1.0f/256.0f);
        sMixRF[i] += interpF * (float)envR * (1.0f/256.0f);

        fw += inc;
        u32 step = fw >> 23;
        if (step != 0)
        {
            fw &= 0x7FFFFF;
            count -= step;
        }
    }

    chan->fw = fw;
    chan->count = count;
    if (stopped)
        chan->statusFlags = 0;
}

static void WasmSoundMainRAM(struct SoundInfo *soundInfo)
{
    s32 numSamples = soundInfo->pcmSamplesPerVBlank;
    if (numSamples > MIX_MAX_SAMPLES)
        numSamples = MIX_MAX_SAMPLES;

    // Reverb: seed the mix buffer from the previous frame's output scaled by the
    // reverb factor. The hardware engine (SoundMainRAM_Reverb in m4a_1.s) sums
    // four samples - the current buffer's L+R and the alternate DMA buffer's L+R -
    // multiplies by reverb, shifts right by 9, and writes that single value to
    // *both* L and R. That mono blend is what gives MP2K reverb its centred,
    // "wet" tail. There is no DMA double-buffering here, so the just-played frame
    // stands in for both the current and alternate buffers: cur==other==prev,
    // which makes the 4-sample sum 2*(prevL+prevR) and the >>9 collapse to
    // (prevL+prevR)*reverb >> 8, seeded identically into L and R.
    u8 reverb = soundInfo->reverb;
    if (reverb > 0)
    {
        s8 *pcmPrev = soundInfo->pcmBuffer;
        float rvScale = (float)reverb * (1.0f / 256.0f);
        for (s32 i = 0; i < numSamples; i++)
        {
            s32 rv = (((s32)pcmPrev[i] + (s32)pcmPrev[PCM_DMA_BUF_SIZE + i]) * (s32)reverb) >> 8;
            // The reverb writes rv to both L and R, so the per-sample feedback is
            // self-correlated and its loop gain is reverb/128 -- exactly 1.0 at
            // the common reverb value 0x80. At unity gain the feedback is only
            // marginally stable: any residual left in pcmBuffer when the last
            // channel stops freezes instead of decaying, and that frozen buffer
            // replays every frame as a faint high-frequency ring (now audible on
            // the JS float path, which no longer has the s8 hiss floor that used
            // to mask it). Bleed the feedback strictly toward zero by 1 LSB/frame
            // so reverb tails always reach true silence. The nudge is within the
            // s8 DAC's own +-1 noise, so it does not perceptibly alter live tails.
            if (rv > 0) rv--;
            else if (rv < 0) rv++;
            sMixL[i] = rv;
            sMixR[i] = rv;
            float rvf = (float)rv;
            sMixLF[i] = rvf;
            sMixRF[i] = rvf;
        }
    }
    else
    {
        for (s32 i = 0; i < numSamples; i++)
        {
            sMixL[i] = 0;
            sMixR[i] = 0;
            sMixLF[i] = 0.0f;
            sMixRF[i] = 0.0f;
        }
    }

    u32 divFreq = soundInfo->divFreq;
    s32 maxChans = soundInfo->maxChans;

    for (s32 ci = 0; ci < maxChans; ci++)
    {
        struct SoundChannel *chan = &soundInfo->chans[ci];
        u8 flags = chan->statusFlags;
        if (!(flags & SOUND_CHANNEL_SF_ON))
            continue;

        struct WaveData *wav = chan->wav;

        // --- envelope / lifecycle state machine -----------------------------
        u8 env;
        if (flags & SOUND_CHANNEL_SF_START)
        {
            if (flags & SOUND_CHANNEL_SF_STOP)
            {
                chan->statusFlags = 0;
                continue;
            }
            flags = SOUND_CHANNEL_SF_ENV_ATTACK;
            chan->currentPointer = wav->data + chan->count;
            chan->count = wav->size - chan->count;
            env = 0;
            chan->fw = 0;
            // The loop flag is WaveData byte offset 3 (o_WaveData_flags in
            // m4a_constants.inc) - the high byte of the u16 `status` field, NOT
            // byte 1 of `type`. Reading the wrong byte left every looped sample
            // unflagged, so sustained instruments (e.g. the intro trumpets)
            // played their sample once and fell silent instead of looping.
            if (((u8 *)wav)[3] & WAVE_DATA_FLAG_LOOP)
                flags |= SOUND_CHANNEL_SF_LOOP;
            // The original SoundMainRAM applies the attack step on the very same
            // frame as SF_START (falls through from the start setup to _081DCFF8),
            // so the first audible frame has env = attack, not 0.
            {
                s32 a = (s32)chan->attack;
                if (a >= 0xFF)
                {
                    a = 0xFF;
                    flags = (flags & ~SOUND_CHANNEL_SF_ENV) | SOUND_CHANNEL_SF_ENV_DECAY;
                }
                env = (u8)a;
            }
            chan->statusFlags = flags;
        }
        else
        {
            env = chan->envelopeVolume;
            if (flags & SOUND_CHANNEL_SF_IEC)
            {
                if (--chan->pseudoEchoLength == 0)
                {
                    chan->statusFlags = 0;
                    continue;
                }
                // keep current env
            }
            else if (flags & SOUND_CHANNEL_SF_STOP)
            {
                env = (env * chan->release) >> 8;
                if (env <= chan->pseudoEchoVolume)
                {
                    if (chan->pseudoEchoVolume == 0)
                    {
                        chan->statusFlags = 0;
                        continue;
                    }
                    env = chan->pseudoEchoVolume;
                    flags |= SOUND_CHANNEL_SF_IEC;
                    chan->statusFlags = flags;
                }
            }
            else
            {
                u8 envState = flags & SOUND_CHANNEL_SF_ENV;
                if (envState == SOUND_CHANNEL_SF_ENV_DECAY)
                {
                    env = (env * chan->decay) >> 8;
                    if (env <= chan->sustain)
                    {
                        env = chan->sustain;
                        if (env == 0 && chan->pseudoEchoVolume == 0)
                        {
                            chan->statusFlags = 0;
                            continue;
                        }
                        if (env == 0)
                        {
                            env = chan->pseudoEchoVolume;
                            flags |= SOUND_CHANNEL_SF_IEC;
                            chan->statusFlags = flags;
                        }
                        else
                        {
                            chan->statusFlags = flags - 1; // -> SUSTAIN
                        }
                    }
                }
                else if (envState == SOUND_CHANNEL_SF_ENV_ATTACK)
                {
                    s32 a = env + chan->attack;
                    if (a >= 0xFF)
                    {
                        a = 0xFF;
                        chan->statusFlags = flags - 1; // -> DECAY
                    }
                    env = a;
                }
            }
        }

        chan->envelopeVolume = env;

        s32 masterScaled = ((soundInfo->masterVolume + 1) * env) >> 4;
        chan->envelopeVolumeRight = (chan->rightVolume * masterScaled) >> 8;
        chan->envelopeVolumeLeft = (chan->leftVolume * masterScaled) >> 8;

        flags = chan->statusFlags;

        // --- mixing ---------------------------------------------------------
        s32 envR = chan->envelopeVolumeRight;
        s32 envL = chan->envelopeVolumeLeft;

        if (chan->type & (TONEDATA_TYPE_CMP | TONEDATA_TYPE_REV))
        {
            MixCompressedOrReversed(chan, wav, numSamples, divFreq, envL, envR, flags);
            continue;
        }

        s8 *ptr = chan->currentPointer;
        s32 count = chan->count;

        s32 loopFlag = flags & SOUND_CHANNEL_SF_LOOP;
        s8 *loopPtr = wav->data + wav->loopStart;
        s32 loopLen = wav->size - wav->loopStart;

        if (chan->type & TONEDATA_TYPE_FIX)
        {
            // No resampling: one source sample per output sample.
            for (s32 i = 0; i < numSamples; i++)
            {
                if (count <= 0)
                {
                    if (loopFlag)
                    {
                        ptr = loopPtr;
                        count = loopLen;
                    }
                    else
                    {
                        chan->statusFlags = 0;
                        break;
                    }
                }
                s32 s = *ptr++;
                count--;
                sMixL[i] += (s * envL) >> 8;
                sMixR[i] += (s * envR) >> 8;
                float sf = (float)s;
                sMixLF[i] += sf * (float)envL * (1.0f/256.0f);
                sMixRF[i] += sf * (float)envR * (1.0f/256.0f);
            }
        }
        else
        {
            u32 fw = chan->fw;
            u32 inc = divFreq * chan->frequency; // .23 fixed-point phase step
            s32 stopped = 0;
            for (s32 i = 0; i < numSamples; i++)
            {
                if (count <= 0)
                {
                    if (loopFlag)
                    {
                        ptr = loopPtr;
                        count += loopLen;
                        if (count <= 0)
                        {
                            stopped = 1;
                            break;
                        }
                    }
                    else
                    {
                        stopped = 1;
                        break;
                    }
                }
                s32 s0 = ptr[0];
                s32 s1 = (count > 1) ? ptr[1] : (loopFlag ? loopPtr[0] : s0);
                s32 interp = s0 + (((s32)(fw) * (s1 - s0)) >> 23);
                sMixL[i] += (interp * envL) >> 8;
                sMixR[i] += (interp * envR) >> 8;
                float interpF = (float)s0 + ((float)fw * (float)(s1 - s0)) * (1.0f/8388608.0f);
                sMixLF[i] += interpF * (float)envL * (1.0f/256.0f);
                sMixRF[i] += interpF * (float)envR * (1.0f/256.0f);

                fw += inc;
                u32 step = fw >> 23;
                if (step != 0)
                {
                    fw &= 0x7FFFFF;
                    ptr += step;
                    count -= step;
                }
            }
            chan->fw = fw;
            if (stopped)
                chan->statusFlags = 0;
        }

        chan->count = count;
        chan->currentPointer = ptr;
    }

    // ----- CGB channel software synthesis -----
    // Duty-cycle high-time thresholds in .23 fixed point (fraction of period = "high"):
    // 0=12.5%, 1=25%, 2=50%, 3=75%
    static const u32 cgbDutyThresh[4] = {1048576u, 2097152u, 4194304u, 6291456u};

    struct CgbChannel *cgbChans = soundInfo->cgbChans;
    if (cgbChans != NULL)
    {
        u32 pcmHz = (u32)soundInfo->pcmFreq;
        if (pcmHz == 0) pcmHz = 13379;
        s32 masterAdj = (s32)soundInfo->masterVolume + 1;

        // Capture whether channel 1 was already sounding last frame so a fresh
        // note (OFF->ON) can re-arm the sweep unit below.
        s32 sweepWasOn = sSweepPrevOn;
        sSweepPrevOn = (cgbChans[0].statusFlags & SOUND_CHANNEL_SF_ON) != 0;

        // Channels 1 and 2: square wave
        for (s32 ci = 0; ci < 2; ci++)
        {
            struct CgbChannel *ch = &cgbChans[ci];
            if (!(ch->statusFlags & SOUND_CHANNEL_SF_ON))
                continue;

            u32 freqX = ch->frequency & 0x7FFu;

            // Channel 1 frequency sweep (NR10): period=bits6-4, dir=bit3
            // (1=decrease), shift=bits2-0. Ticks at 128 Hz against a shadow
            // frequency; an upward overflow past 2047 silences the channel.
            if (ci == 0)
            {
                u32 sw = ch->sweep;
                u32 period = (sw >> 4) & 7u;
                u32 negate = (sw >> 3) & 1u;
                u32 shift  = sw & 7u;

                if (!sweepWasOn) // new note: reload the sweep unit
                {
                    sSweepShadow = freqX;
                    sSweepTimer = period ? (s32)period : 8;
                    sSweepTickAcc = 0;
                    sSweepMuted = 0;
                }

                if (period != 0 && shift != 0 && !sSweepMuted)
                {
                    sSweepTickAcc += 12800; // 128 Hz scaled; frame rate ~59.73 Hz
                    while (sSweepTickAcc >= 5973)
                    {
                        sSweepTickAcc -= 5973;
                        if (--sSweepTimer <= 0)
                        {
                            sSweepTimer = (s32)period;
                            s32 delta = (s32)(sSweepShadow >> shift);
                            s32 nf = negate ? (s32)sSweepShadow - delta
                                            : (s32)sSweepShadow + delta;
                            if (nf > 2047) { sSweepMuted = 1; break; }
                            if (nf < 0) nf = 0;
                            sSweepShadow = (u32)nf;
                        }
                    }
                    if (sSweepMuted)
                        continue;
                    freqX = sSweepShadow & 0x7FFu;
                }
                else if (period == 0)
                {
                    // Sweep idle: track the engine frequency so pitch bends work.
                    sSweepShadow = freqX;
                }
                else
                {
                    freqX = sSweepShadow & 0x7FFu;
                }
            }

            s32 p2048 = 2048 - (s32)freqX;
            if (p2048 <= 0)
                continue;

            // inc = 131072 * 2^23 / (pcmHz * p2048)  [.23 fixed-point phase step]
            u32 inc = (u32)((131072ULL << 23) / ((u64)pcmHz * (u64)p2048));
            if (inc == 0) inc = 1;

            u32 duty = (u32)(uintptr_t)ch->wavePointer;
            if (duty > 3u) duty = 2u;
            u32 thresh = cgbDutyThresh[duty];

            // CGB envelope is 0-15; scale matches hardware: CGB at env=15 contributes
            // 15 units to the DAC, the same range as the DS PCM buffer (s8, max 127).
            s32 amp = ((s32)ch->envelopeVolume * masterAdj) >> 4;
            s32 ampR = (ch->pan & 0x0Fu) ? amp : 0;
            s32 ampL = (ch->pan & 0xF0u) ? amp : 0;

            float ampLF = (float)ampL, ampRF = (float)ampR;
            u32 phase = sCgbPhase[ci];
            for (s32 i = 0; i < numSamples; i++)
            {
                s32 hi = (phase < thresh) ? 1 : -1;
                sMixL[i] += hi * ampL;
                sMixR[i] += hi * ampR;
                sMixLF[i] += (float)hi * ampLF;
                sMixRF[i] += (float)hi * ampRF;
                phase = (phase + inc) & 0x7FFFFFu;
            }
            sCgbPhase[ci] = phase;
        }

        // Channel 3: wave table (32 nibble samples, freq = 65536 / (2048 - X) Hz)
        {
            struct CgbChannel *ch = &cgbChans[2];
            if ((ch->statusFlags & SOUND_CHANNEL_SF_ON) && ch->wavePointer != NULL)
            {
                u32 freqX = ch->frequency & 0x7FFu;
                s32 p2048 = 2048 - (s32)freqX;
                if (p2048 > 0)
                {
                    // phase in [0, 2^29) covers 32 samples at .24 frac
                    // inc = 2^45 / (pcmHz * p2048)
                    u32 inc = (u32)((1ULL << 45) / ((u64)pcmHz * (u64)p2048));
                    if (inc == 0) inc = 1;

                    // The wave channel volume is not the linear 0-15 envelope; the
                    // engine maps it through gCgb3Vol to one of the hardware NR32
                    // output levels (mute / 25% / 50% / 75% / 100%, where 75% is the
                    // GBA-only extension at bit 7). Reproduce that quantisation so
                    // ch3's loudness curve matches hardware instead of fading
                    // smoothly across all 16 steps.
                    s32 volQuarters;
                    switch (gCgb3Vol[ch->envelopeVolume & 0xF])
                    {
                    case 0x20: volQuarters = 4; break; // 100%
                    case 0x80: volQuarters = 3; break; // 75% (GBA extension)
                    case 0x40: volQuarters = 2; break; // 50%
                    case 0x60: volQuarters = 1; break; // 25%
                    default:   volQuarters = 0; break; // mute (0x00)
                    }
                    // 100% matches a max-volume square channel's peak amplitude.
                    s32 fullAmp = ((s32)15 * masterAdj) >> 4;
                    s32 amp = (fullAmp * volQuarters) >> 2;
                    s32 ampR = (ch->pan & 0x0Fu) ? amp : 0;
                    s32 ampL = (ch->pan & 0xF0u) ? amp : 0;

                    u8 *waveBytes = (u8 *)ch->wavePointer;
                    u32 phase = sCgbPhase[2];
                    for (s32 i = 0; i < numSamples; i++)
                    {
                        u32 sIdx = phase >> 24; // 0-31
                        u8 bval = waveBytes[sIdx >> 1];
                        // GBA plays high nibble first (MSB→LSB per byte)
                        s32 nib = (sIdx & 1u) ? (s32)(bval & 0x0Fu) : (s32)(bval >> 4);
                        s32 sample = nib - 8; // centre at 0, range -8..7
                        // Divide by 8 to normalize: wave table range is ±8, not ±1 like square wave
                        sMixL[i] += (sample * ampL) >> 3;
                        sMixR[i] += (sample * ampR) >> 3;
                        float sf = (float)sample * (1.0f/8.0f);
                        sMixLF[i] += sf * (float)ampL;
                        sMixRF[i] += sf * (float)ampR;
                        phase = (phase + inc) & 0x1FFFFFFFu;
                    }
                    sCgbPhase[2] = phase;
                }
            }
        }

        // Channel 4: noise (LFSR). ch->frequency holds the NR43 register value
        // (bits 7-4 = shift clock s, bits 2-0 = divisor code r); the counter
        // width (15- vs 7-bit) comes from wavePointer&1 (written to NR43 bit 3).
        {
            struct CgbChannel *ch = &cgbChans[3];
            if (ch->statusFlags & SOUND_CHANNEL_SF_ON)
            {
                u32 nr43 = ch->frequency & 0xFFu;
                u32 r = nr43 & 7u;
                u32 s = (nr43 >> 4) & 0xFu;
                u32 width = (u32)(uintptr_t)ch->wavePointer & 1u; // 1 => 7-bit

                // LFSR shift-clock frequency in Hz:
                //   f = 524288 / r / 2^(s+1)   (r == 0 means r = 0.5)
                u32 clockHz = (r == 0) ? (524288u >> s) : ((524288u / r) >> (s + 1));
                if (clockHz == 0) clockHz = 1;

                // .16 fixed-point LFSR steps per output sample.
                u32 inc = (u32)(((u64)clockHz << 16) / pcmHz);
                if (inc == 0) inc = 1;

                s32 amp = ((s32)ch->envelopeVolume * masterAdj) >> 4;
                s32 ampR = (ch->pan & 0x0Fu) ? amp : 0;
                s32 ampL = (ch->pan & 0xF0u) ? amp : 0;

                u32 phase = sCgbPhase[3];
                u16 lfsr = sNoiseLfsr;
                for (s32 i = 0; i < numSamples; i++)
                {
                    // Output is the inverted low bit: bit0==0 -> high level.
                    s32 hi = (lfsr & 1u) ? -1 : 1;
                    sMixL[i] += hi * ampL;
                    sMixR[i] += hi * ampR;
                    sMixLF[i] += (float)hi * (float)ampL;
                    sMixRF[i] += (float)hi * (float)ampR;

                    phase += inc;
                    u32 steps = phase >> 16;
                    phase &= 0xFFFFu;
                    while (steps--)
                    {
                        u32 fb = (lfsr ^ (lfsr >> 1)) & 1u;
                        lfsr >>= 1;
                        lfsr |= fb << 14;
                        if (width)
                            lfsr = (lfsr & ~0x40u) | (fb << 6); // 7-bit mode
                    }
                }
                sCgbPhase[3] = phase;
                sNoiseLfsr = lfsr ? lfsr : 0x7FFF; // never let it lock at 0
            }
        }
    }

    // Write the mixed frame.
    // pcmBuffer (s8) uses the s32 path — kept for the reverb seed next frame.
    // gWasmPcmL/R use the float path (sMixLF/RF) which accumulated the same
    // contributions without the >> 8 and >> 23 integer truncations, removing
    // the ~-40 dB quantisation noise floor audible as background hiss.
    s8 *pcm = soundInfo->pcmBuffer;
    for (s32 i = 0; i < numSamples; i++)
    {
        s32 l = sMixL[i];
        s32 r = sMixR[i];
        if (l >  127) l =  127;
        if (l < -128) l = -128;
        if (r >  127) r =  127;
        if (r < -128) r = -128;
        pcm[i]                    = (s8)l;
        pcm[PCM_DMA_BUF_SIZE + i] = (s8)r;
        float fl = sMixLF[i] * (1.0f / 128.0f);
        float fr = sMixRF[i] * (1.0f / 128.0f);
        if (fl >  1.0f) fl =  1.0f;
        if (fl < -1.0f) fl = -1.0f;
        if (fr >  1.0f) fr =  1.0f;
        if (fr < -1.0f) fr = -1.0f;
        gWasmPcmL[i] = fl;
        gWasmPcmR[i] = fr;
    }
}

void SoundMain(void)
{
    struct SoundInfo *soundInfo = SOUND_INFO_PTR;
    if (soundInfo->ident != ID_NUMBER)
        return;
    soundInfo->ident++;

    if (soundInfo->MPlayMainHead != NULL)
    {
        struct MusicPlayerInfo *mp = soundInfo->musicPlayerHead;
        MPlayMainFunc fn = soundInfo->MPlayMainHead;
        while (fn != NULL && mp != NULL)
        {
            fn(mp);
            fn = mp->MPlayMainNext;
            mp = mp->musicPlayerNext;
        }
    }

    soundInfo->CgbSound();
    WasmSoundMainRAM(soundInfo);

    soundInfo->ident = ID_NUMBER;
}

// No hardware V-blank/DMA in the browser; the JS frontend pulls PCM directly.
void m4aSoundVSync(void) {}

#endif // WASM
