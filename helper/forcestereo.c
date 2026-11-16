// forcestereo — drains the ForceStereo virtual device and plays it on two channels
// of a real output device. Output-only (AUHAL with input disabled), so macOS never
// asks for microphone access and never shows the recording indicator.

#include <AudioToolbox/AudioToolbox.h>
#include <CoreAudio/CoreAudio.h>
#include <CoreFoundation/CoreFoundation.h>
#include <dispatch/dispatch.h>
#include <mach/mach_time.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <math.h>
#include <time.h>
#include <unistd.h>

#include "../shared/ForceStereoShared.h"

#define FIFO_FRAMES 65536u
#define FIFO_MASK   (FIFO_FRAMES - 1)
#define MAIN        kAudioObjectPropertyElementMain
#define GLOBAL      kAudioObjectPropertyScopeGlobal
#define OUTPUT      kAudioObjectPropertyScopeOutput

#pragma mark Options

static const char* gOptTarget = NULL;   // UID or name substring; NULL = automatic
static UInt32 gOptChL = 1, gOptChR = 2; // 1-based device channels
static double gOptLatencyMs = 10.0;     // safety buffer kept in the helper
static UInt32 gOptBuffer = 0;           // device IO buffer in frames, 0 = automatic
static bool   gOptGrabDefault = true;
static bool   gOptVerbose = false;
static bool   gOptNull = false;         // drain and measure only, play nothing

#pragma mark State

static _Atomic AudioObjectID gVDev = 0;
static _Atomic AudioObjectID gTarget = 0;
static Float64 gCfgVRate = 0, gCfgTRate = 0;
static double  gTargetSince = 0;
static bool    gFirstConfig = true;
static bool    gWarnedNoDriver = false, gWarnedNoTarget = false;

static pthread_mutex_t gAUMutex = PTHREAD_MUTEX_INITIALIZER;
static AudioUnit gAU = NULL;
static bool      gAURunning = false;
static UInt32    gOutCh = 2;

static Float32 gFifo[FIFO_FRAMES * kFS_Channels];
static _Atomic UInt64 gFifoW = 0, gFifoR = 0;
static _Atomic int    gFlush = 0;
static _Atomic int    gPlaying = 0;
static _Atomic UInt64 gTrough = UINT64_MAX;
static _Atomic UInt64 gUnderruns = 0, gSkipped = 0;
static _Atomic UInt64 gSetpoint = 480, gStartThresh = 736, gSoftSkip = 1280, gHardSkip = 12000;
static _Atomic UInt32 gDevBufV = 256; // device IO buffer expressed in virtual-device frames
static _Atomic int    gResetCtl = 0;

static AudioObjectPropertyListenerBlock gListener;
static AudioObjectID gListenTarget = 0, gListenVDev = 0;
static _Atomic int gPending = 0;

#pragma mark Utilities

static double Now(void)
{
    static mach_timebase_info_data_t tb;
    if (tb.denom == 0) mach_timebase_info(&tb);
    return (double)mach_absolute_time() * tb.numer / tb.denom / 1e9;
}

static void Log(const char* fmt, ...)
{
    char ts[32];
    time_t t = time(NULL);
    struct tm tmv;
    localtime_r(&t, &tmv);
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tmv);
    fprintf(stderr, "%s  ", ts);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

static bool GetU32(AudioObjectID obj, AudioObjectPropertySelector sel, AudioObjectPropertyScope scope, UInt32* out)
{
    AudioObjectPropertyAddress a = { sel, scope, MAIN };
    UInt32 size = sizeof(UInt32);
    return AudioObjectGetPropertyData(obj, &a, 0, NULL, &size, out) == noErr;
}

static Float64 GetF64(AudioObjectID obj, AudioObjectPropertySelector sel)
{
    AudioObjectPropertyAddress a = { sel, GLOBAL, MAIN };
    Float64 v = 0;
    UInt32 size = sizeof(v);
    if (AudioObjectGetPropertyData(obj, &a, 0, NULL, &size, &v) != noErr) return 0;
    return v;
}

static void GetString(AudioObjectID obj, AudioObjectPropertySelector sel, char* buf, size_t len)
{
    AudioObjectPropertyAddress a = { sel, GLOBAL, MAIN };
    CFStringRef s = NULL;
    UInt32 size = sizeof(s);
    buf[0] = 0;
    if (AudioObjectGetPropertyData(obj, &a, 0, NULL, &size, &s) == noErr && s) {
        CFStringGetCString(s, buf, (CFIndex)len, kCFStringEncodingUTF8);
        CFRelease(s);
    }
}

static AudioObjectID DeviceForUID(const char* uid)
{
    AudioObjectPropertyAddress a = { kAudioHardwarePropertyTranslateUIDToDevice, GLOBAL, MAIN };
    CFStringRef s = CFStringCreateWithCString(NULL, uid, kCFStringEncodingUTF8);
    AudioObjectID dev = kAudioObjectUnknown;
    UInt32 size = sizeof(dev);
    AudioObjectGetPropertyData(kAudioObjectSystemObject, &a, sizeof(s), &s, &size, &dev);
    CFRelease(s);
    return dev;
}

static UInt32 OutputChannels(AudioObjectID dev)
{
    AudioObjectPropertyAddress a = { kAudioDevicePropertyStreamConfiguration, OUTPUT, MAIN };
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize(dev, &a, 0, NULL, &size) != noErr || size == 0) return 0;
    AudioBufferList* abl = (AudioBufferList*)malloc(size);
    UInt32 ch = 0;
    if (AudioObjectGetPropertyData(dev, &a, 0, NULL, &size, abl) == noErr)
        for (UInt32 i = 0; i < abl->mNumberBuffers; i++) ch += abl->mBuffers[i].mNumberChannels;
    free(abl);
    return ch;
}

static UInt32 CopyDevices(AudioObjectID** out)
{
    AudioObjectPropertyAddress a = { kAudioHardwarePropertyDevices, GLOBAL, MAIN };
    UInt32 size = 0;
    *out = NULL;
    if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &a, 0, NULL, &size) != noErr || size == 0) return 0;
    AudioObjectID* ids = (AudioObjectID*)malloc(size);
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &a, 0, NULL, &size, ids) != noErr) { free(ids); return 0; }
    *out = ids;
    return size / sizeof(AudioObjectID);
}

static bool SupportsRate(AudioObjectID dev, Float64 rate)
{
    AudioObjectPropertyAddress a = { kAudioDevicePropertyAvailableNominalSampleRates, GLOBAL, MAIN };
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize(dev, &a, 0, NULL, &size) != noErr || size == 0) return false;
    AudioValueRange* r = (AudioValueRange*)malloc(size);
    bool ok = false;
    if (AudioObjectGetPropertyData(dev, &a, 0, NULL, &size, r) == noErr)
        for (UInt32 i = 0; i < size / sizeof(AudioValueRange); i++)
            if (rate >= r[i].mMinimum && rate <= r[i].mMaximum) ok = true;
    free(r);
    return ok;
}

static bool SetRateAndWait(AudioObjectID dev, Float64 rate)
{
    AudioObjectPropertyAddress a = { kAudioDevicePropertyNominalSampleRate, GLOBAL, MAIN };
    if (AudioObjectSetPropertyData(dev, &a, 0, NULL, sizeof(rate), &rate) != noErr) return false;
    for (int i = 0; i < 60; i++) {
        if (GetF64(dev, kAudioDevicePropertyNominalSampleRate) == rate) return true;
        usleep(50000);
    }
    return false;
}

static AudioObjectID GetDefault(AudioObjectPropertySelector sel)
{
    AudioObjectID dev = 0;
    GetU32(kAudioObjectSystemObject, sel, GLOBAL, &dev);
    return dev;
}

static void SetDefault(AudioObjectPropertySelector sel, AudioObjectID dev)
{
    AudioObjectPropertyAddress a = { sel, GLOBAL, MAIN };
    AudioObjectSetPropertyData(kAudioObjectSystemObject, &a, 0, NULL, sizeof(dev), &dev);
}

static void SetVDevNumber(AudioObjectID vdev, AudioObjectPropertySelector sel, CFNumberType type, const void* value)
{
    AudioObjectPropertyAddress a = { sel, GLOBAL, MAIN };
    CFNumberRef n = CFNumberCreate(NULL, type, value);
    AudioObjectSetPropertyData(vdev, &a, 0, NULL, sizeof(n), &n);
    CFRelease(n);
}

static const char* TransportName(UInt32 t)
{
    switch (t) {
    case kAudioDeviceTransportTypeBuiltIn: return "built-in";
    case kAudioDeviceTransportTypeUSB: return "USB";
    case kAudioDeviceTransportTypeFireWire: return "FireWire";
    case kAudioDeviceTransportTypeThunderbolt: return "Thunderbolt";
    case kAudioDeviceTransportTypePCI: return "PCI";
    case kAudioDeviceTransportTypeAVB: return "AVB";
    case kAudioDeviceTransportTypeBluetooth: case kAudioDeviceTransportTypeBluetoothLE: return "Bluetooth";
    case kAudioDeviceTransportTypeHDMI: return "HDMI";
    case kAudioDeviceTransportTypeDisplayPort: return "DisplayPort";
    case kAudioDeviceTransportTypeAirPlay: return "AirPlay";
    case kAudioDeviceTransportTypeAggregate: return "aggregate";
    case kAudioDeviceTransportTypeVirtual: return "virtual";
    default: return "other";
    }
}

#pragma mark Target selection

static int TransportScore(UInt32 t)
{
    switch (t) {
    case kAudioDeviceTransportTypeUSB: case kAudioDeviceTransportTypeFireWire:
    case kAudioDeviceTransportTypeThunderbolt: case kAudioDeviceTransportTypePCI:
    case kAudioDeviceTransportTypeAVB: return 3;
    case kAudioDeviceTransportTypeBuiltIn: return 1;
    default: return 0;
    }
}

static AudioObjectID PickTarget(AudioObjectID vdev)
{
    AudioObjectID* ids;
    UInt32 n = CopyDevices(&ids);
    UInt32 need = gOptChL > gOptChR ? gOptChL : gOptChR;
    AudioObjectID best = 0, pinned = 0;
    int bestScore = 0;
    for (UInt32 i = 0; i < n; i++) {
        AudioObjectID d = ids[i];
        if (d == vdev || OutputChannels(d) < need) continue;
        char uid[256], name[256];
        GetString(d, kAudioDevicePropertyDeviceUID, uid, sizeof(uid));
        GetString(d, kAudioObjectPropertyName, name, sizeof(name));
        if (strcmp(uid, kFS_DeviceUID) == 0) continue;
        if (gOptTarget && !pinned && (strcmp(uid, gOptTarget) == 0 || strcasestr(name, gOptTarget))) pinned = d;
        UInt32 t = 0;
        GetU32(d, kAudioDevicePropertyTransportType, GLOBAL, &t);
        int score = TransportScore(t);
        if (score > bestScore) { bestScore = score; best = d; }
    }
    free(ids);
    return pinned ? pinned : best;
}

#pragma mark Playback

static OSStatus Render(void* ref, AudioUnitRenderActionFlags* flags, const AudioTimeStamp* ts, UInt32 bus, UInt32 n, AudioBufferList* io)
{
    (void)ref; (void)flags; (void)ts; (void)bus;
    Float32* out = (Float32*)io->mBuffers[0].mData;
    const UInt32 nch = gOutCh, cl = gOptChL - 1, cr = gOptChR - 1;
    memset(out, 0, io->mBuffers[0].mDataByteSize);
    if (io->mBuffers[0].mDataByteSize < n * nch * sizeof(Float32)) return noErr;

    UInt64 w = atomic_load_explicit(&gFifoW, memory_order_acquire);
    UInt64 r = atomic_load_explicit(&gFifoR, memory_order_relaxed);
    if (atomic_exchange(&gFlush, 0)) { r = w; atomic_store(&gPlaying, 0); }
    UInt64 avail = w - r;

    if (!atomic_load_explicit(&gPlaying, memory_order_relaxed)) {
        if (avail < atomic_load_explicit(&gStartThresh, memory_order_relaxed)) {
            atomic_store_explicit(&gFifoR, r, memory_order_release);
            return noErr;
        }
        atomic_store(&gPlaying, 1);
    }

    // Catch up when more is buffered than needed: drop silence first, audio only as a last resort.
    UInt64 setpoint = atomic_load_explicit(&gSetpoint, memory_order_relaxed);
    if (avail > n + setpoint) {
        UInt64 excess = avail - n - setpoint;
        UInt64 soft = atomic_load_explicit(&gSoftSkip, memory_order_relaxed);
        UInt64 skip = 0;
        if (excess > atomic_load_explicit(&gHardSkip, memory_order_relaxed)) {
            skip = excess;
        } else if (excess > soft) {
            UInt64 max = excess - soft / 2;
            while (skip < max) {
                const Float32* f = &gFifo[((r + skip) & FIFO_MASK) * kFS_Channels];
                if (fabsf(f[0]) > 1e-5f || fabsf(f[1]) > 1e-5f) break;
                skip++;
            }
        }
        if (skip) { r += skip; avail -= skip; atomic_fetch_add(&gSkipped, skip); }
    }

    UInt32 take = (UInt32)(avail < n ? avail : n);
    for (UInt32 i = 0; i < take; i++) {
        const Float32* f = &gFifo[((r + i) & FIFO_MASK) * kFS_Channels];
        out[i * nch + cl] = f[0];
        out[i * nch + cr] = f[1];
    }
    r += take;
    atomic_store_explicit(&gFifoR, r, memory_order_release);

    UInt64 left = avail - take;
    if (left < atomic_load_explicit(&gTrough, memory_order_relaxed)) atomic_store(&gTrough, left);
    if (take < n) { atomic_store(&gPlaying, 0); atomic_fetch_add(&gUnderruns, 1); }
    return noErr;
}

// gAUMutex held.
static void TeardownAU(void)
{
    if (!gAU) return;
    AudioOutputUnitStop(gAU);
    AudioUnitUninitialize(gAU);
    AudioComponentInstanceDispose(gAU);
    gAU = NULL;
    gAURunning = false;
}

// gAUMutex held. vrate = virtual device rate (rate of the frames we feed).
static bool SetupAU(AudioObjectID vdev, AudioObjectID target, Float64 vrate, Float64 trate)
{
    AudioComponentDescription desc = { kAudioUnitType_Output, kAudioUnitSubType_HALOutput, kAudioUnitManufacturer_Apple, 0, 0 };
    AudioComponent comp = AudioComponentFindNext(NULL, &desc);
    AudioUnit au = NULL;
    if (!comp || AudioComponentInstanceNew(comp, &au) != noErr) return false;

    UInt32 one = 1, zero = 0;
    OSStatus err = AudioUnitSetProperty(au, kAudioOutputUnitProperty_EnableIO, kAudioUnitScope_Output, 0, &one, sizeof(one));
    AudioUnitSetProperty(au, kAudioOutputUnitProperty_EnableIO, kAudioUnitScope_Input, 1, &zero, sizeof(zero));
    if (!err) err = AudioUnitSetProperty(au, kAudioOutputUnitProperty_CurrentDevice, kAudioUnitScope_Global, 0, &target, sizeof(target));

    // IO buffer: small enough for low latency, scaled with the sample rate.
    UInt32 buf = gOptBuffer ? gOptBuffer : (trate > 100000 ? 1024 : (trate > 50000 ? 512 : 256));
    AudioObjectPropertyAddress ba = { kAudioDevicePropertyBufferFrameSize, GLOBAL, MAIN };
    AudioObjectSetPropertyData(target, &ba, 0, NULL, sizeof(buf), &buf);
    GetU32(target, kAudioDevicePropertyBufferFrameSize, GLOBAL, &buf);

    AudioStreamBasicDescription devf = {0};
    UInt32 size = sizeof(devf);
    if (!err) err = AudioUnitGetProperty(au, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output, 0, &devf, &size);
    UInt32 nch = devf.mChannelsPerFrame;
    if (!err && (nch < gOptChL || nch < gOptChR)) err = kAudioUnitErr_FormatNotSupported;

    AudioStreamBasicDescription f = {0};
    f.mSampleRate = vrate;
    f.mFormatID = kAudioFormatLinearPCM;
    f.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagsNativeEndian | kAudioFormatFlagIsPacked;
    f.mChannelsPerFrame = nch;
    f.mBitsPerChannel = 32;
    f.mBytesPerFrame = f.mBytesPerPacket = nch * sizeof(Float32);
    f.mFramesPerPacket = 1;
    if (!err) err = AudioUnitSetProperty(au, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &f, sizeof(f));

    AURenderCallbackStruct cb = { Render, NULL };
    if (!err) err = AudioUnitSetProperty(au, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0, &cb, sizeof(cb));
    if (!err) { gOutCh = nch; err = AudioUnitInitialize(au); }
    if (err) {
        Log("could not open output device (error %d)", (int)err);
        AudioComponentInstanceDispose(au);
        return false;
    }

    // Buffering thresholds, in virtual-device frames.
    UInt32 devBufV = (UInt32)ceil(buf * vrate / trate);
    UInt64 setpoint = (UInt64)(gOptLatencyMs * vrate / 1000.0);
    atomic_store(&gDevBufV, devBufV);
    atomic_store(&gSetpoint, setpoint);
    atomic_store(&gStartThresh, setpoint + devBufV);
    atomic_store(&gSoftSkip, (UInt64)(vrate * 0.027));
    atomic_store(&gHardSkip, (UInt64)(vrate * 0.25));
    atomic_store(&gFlush, 1);
    atomic_store(&gResetCtl, 1);

    // Tell the virtual device how late its audio really comes out (for A/V sync).
    UInt32 lat = 0, safety = 0;
    GetU32(target, kAudioDevicePropertyLatency, OUTPUT, &lat);
    GetU32(target, kAudioDevicePropertySafetyOffset, OUTPUT, &safety);
    SInt32 total = (SInt32)(setpoint + 2 * devBufV + (UInt64)((lat + safety) * vrate / trate));
    SetVDevNumber(vdev, kFS_PropLatency, kCFNumberSInt32Type, &total);

    gAU = au;
    gAURunning = false;
    return true;
}

#pragma mark Poll thread

static void* PollThread(void* arg)
{
    (void)arg;
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);

    UInt64 drvR = UINT64_MAX; // next index expected from the driver
    double lastData = 0, lastCtl = Now(), lastStats = Now();
    double integ = 0, base = 1.0, lastRatio = 0, corr = 0;
    UInt64 lastUnder = 0, discont = 0, received = 0;
    Float32 peak = 0;
    UInt32 chunkMax = 0;

    for (;;) {
        AudioObjectID vdev = atomic_load(&gVDev);
        if (!vdev) { drvR = UINT64_MAX; usleep(200000); continue; }

        AudioObjectPropertyAddress a = { kFS_PropRead, GLOBAL, MAIN };
        SInt64 want = (SInt64)drvR;
        CFNumberRef q = CFNumberCreate(NULL, kCFNumberSInt64Type, &want);
        CFPropertyListRef pl = NULL;
        UInt32 size = sizeof(pl);
        OSStatus err = AudioObjectGetPropertyData(vdev, &a, sizeof(q), &q, &size, &pl);
        CFRelease(q);
        if (err != noErr || !pl || CFGetTypeID(pl) != CFDataGetTypeID() ||
            CFDataGetLength((CFDataRef)pl) < (CFIndex)sizeof(FSReadHeader)) {
            if (pl) CFRelease(pl);
            drvR = UINT64_MAX;
            usleep(200000);
            continue;
        }
        FSReadHeader h;
        const UInt8* bytes = CFDataGetBytePtr((CFDataRef)pl);
        memcpy(&h, bytes, sizeof(h));
        UInt32 frames = h.frames;
        if (h.magic != kFS_ReadMagic ||
            CFDataGetLength((CFDataRef)pl) < (CFIndex)(sizeof(h) + (size_t)frames * kFS_Channels * sizeof(Float32)))
            frames = 0;
        if (drvR != UINT64_MAX && h.startIndex != drvR) discont++;
        drvR = h.startIndex + frames;

        double now = Now();
        if (frames) {
            const Float32* src = (const Float32*)(bytes + sizeof(h));
            for (UInt32 i = 0; i < frames * kFS_Channels; i++) {
                Float32 v = fabsf(src[i]);
                if (v > peak) peak = v;
            }
            UInt64 w = atomic_load_explicit(&gFifoW, memory_order_relaxed);
            UInt64 r = atomic_load_explicit(&gFifoR, memory_order_acquire);
            if (gOptNull && getenv("FS_DUMP")) {
                static FILE* dump;
                if (!dump) dump = fopen(getenv("FS_DUMP"), "wb");
                if (dump) { fwrite(src, sizeof(Float32) * kFS_Channels, frames, dump); fflush(dump); }
            }
            if (gOptNull) {
                atomic_store(&gFifoR, w); // nothing plays: discard
            } else if (w - r + frames <= FIFO_FRAMES) {
                for (UInt32 i = 0; i < frames; i++) {
                    Float32* d = &gFifo[((w + i) & FIFO_MASK) * kFS_Channels];
                    d[0] = src[i * 2];
                    d[1] = src[i * 2 + 1];
                }
                atomic_store_explicit(&gFifoW, w + frames, memory_order_release);
            }
            received += frames;
            lastData = now;
            if (frames > chunkMax && frames < kFS_MaxReadFrames) chunkMax = frames;
        }
        CFRelease(pl);

        // Run the output only while something is playing, so the Mac can still idle-sleep.
        bool active = h.running || (now - lastData) < 3.0;
        pthread_mutex_lock(&gAUMutex);
        if (gAU && active && !gAURunning) {
            if (AudioOutputUnitStart(gAU) == noErr) { gAURunning = true; atomic_store(&gResetCtl, 1); }
        } else if (gAU && !active && gAURunning) {
            AudioOutputUnitStop(gAU);
            gAURunning = false;
        }
        bool running = gAURunning;
        pthread_mutex_unlock(&gAUMutex);

        // Clock control: keep the trough of the buffer at the setpoint by steering the
        // virtual device's clock. No samples are altered.
        if (now - lastCtl >= 1.0) {
            double dt = now - lastCtl;
            lastCtl = now;
            UInt64 trough = atomic_exchange(&gTrough, UINT64_MAX);
            UInt64 under = atomic_load(&gUnderruns);
            AudioObjectID target = atomic_load(&gTarget);
            if (atomic_exchange(&gResetCtl, 0)) { corr = 0; lastUnder = under; trough = UINT64_MAX; }
            if (running && target) {
                Float64 nominal = GetF64(target, kAudioDevicePropertyNominalSampleRate);
                Float64 actual = GetF64(target, kAudioDevicePropertyActualSampleRate);
                if (nominal > 0 && actual > 0 && fabs(actual / nominal - 1.0) < 0.005)
                    base += 0.2 * (actual / nominal - base);
                if (trough != UINT64_MAX && under == lastUnder && h.sampleRate > 0) {
                    double setpoint = (double)atomic_load(&gSetpoint);
                    double e = (double)trough - setpoint;
                    double kp = 1.0 / (10.0 * h.sampleRate), ki = kp / 100.0;
                    integ += e * dt;
                    double lim = 300e-6 / ki;
                    if (integ > lim) integ = lim;
                    if (integ < -lim) integ = -lim;
                    corr = -(kp * e + ki * integ);
                    if (corr > 1000e-6) corr = 1000e-6;
                    if (corr < -1000e-6) corr = -1000e-6;
                }
                lastUnder = under;
                double ratio = base * (1.0 + corr);
                if (fabs(ratio - lastRatio) > 1e-7) {
                    SetVDevNumber(vdev, kFS_PropRate, kCFNumberFloat64Type, &ratio);
                    lastRatio = ratio;
                }
            }
            if (chunkMax) {
                UInt64 soft = (UInt64)chunkMax + atomic_load(&gDevBufV) + 256;
                atomic_store(&gSoftSkip, soft);
            }
            if (gOptVerbose && now - lastStats >= 5.0) {
                lastStats = now;
                UInt64 w = atomic_load(&gFifoW), r = atomic_load(&gFifoR);
                Log("%s  rate %.0f  recv %llu  buffered %llu  trough %lld  clock %+.1f ppm  peak %.1f dBFS  underruns %llu  skipped %llu  resyncs %llu",
                    running ? "playing" : (gOptNull ? "null" : "idle"), h.sampleRate, received, w - r,
                    trough == UINT64_MAX ? -1LL : (long long)trough, (lastRatio ? lastRatio - 1.0 : 0.0) * 1e6,
                    peak > 0 ? 20.0 * log10(peak) : -999.0, under, atomic_load(&gSkipped), discont);
                peak = 0;
            }
        }

        if (frames == kFS_MaxReadFrames) continue; // more waiting
        usleep(active ? 4000 : 10000);
    }
    return NULL;
}

#pragma mark Configuration

static void Reconfigure(void);

static void Schedule(void)
{
    if (atomic_exchange(&gPending, 1)) return;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 250 * NSEC_PER_MSEC), dispatch_get_main_queue(), ^{
        atomic_store(&gPending, 0);
        Reconfigure();
    });
}

static void Listen(AudioObjectID obj, AudioObjectPropertySelector sel, bool add)
{
    AudioObjectPropertyAddress a = { sel, GLOBAL, MAIN };
    if (add) AudioObjectAddPropertyListenerBlock(obj, &a, dispatch_get_main_queue(), gListener);
    else AudioObjectRemovePropertyListenerBlock(obj, &a, dispatch_get_main_queue(), gListener);
}

static void WatchDevice(AudioObjectID* slot, AudioObjectID dev)
{
    if (*slot == dev) return;
    if (*slot) {
        Listen(*slot, kAudioDevicePropertyNominalSampleRate, false);
        Listen(*slot, kAudioDevicePropertyDeviceIsAlive, false);
    }
    *slot = dev;
    if (dev) {
        Listen(dev, kAudioDevicePropertyNominalSampleRate, true);
        Listen(dev, kAudioDevicePropertyDeviceIsAlive, true);
    }
}

static void Reconfigure(void)
{
    AudioObjectID vdev = DeviceForUID(kFS_DeviceUID);
    if (!vdev) {
        if (!gWarnedNoDriver) { Log("ForceStereo device not found — is the driver installed?"); gWarnedNoDriver = true; }
        atomic_store(&gVDev, 0);
        atomic_store(&gTarget, 0);
        WatchDevice(&gListenVDev, 0);
        WatchDevice(&gListenTarget, 0);
        pthread_mutex_lock(&gAUMutex);
        TeardownAU();
        pthread_mutex_unlock(&gAUMutex);
        gCfgVRate = gCfgTRate = 0;
        return;
    }
    gWarnedNoDriver = false;
    WatchDevice(&gListenVDev, vdev);

    AudioObjectID target = gOptNull ? 0 : PickTarget(vdev);
    AudioObjectID prev = atomic_load(&gTarget);
    if (!target && !gOptNull) {
        if (!gWarnedNoTarget) { Log("no output device to forward to"); gWarnedNoTarget = true; }
        atomic_store(&gVDev, vdev);
        atomic_store(&gTarget, 0);
        WatchDevice(&gListenTarget, 0);
        pthread_mutex_lock(&gAUMutex);
        TeardownAU();
        pthread_mutex_unlock(&gAUMutex);
        gCfgTRate = 0;
        return;
    }
    gWarnedNoTarget = false;
    if (gOptNull) { atomic_store(&gVDev, vdev); return; }

    // When the target (re)appears macOS may make it the default output: take that over.
    if (target != prev || gFirstConfig) gTargetSince = Now();
    if (gOptGrabDefault && Now() - gTargetSince < 5.0) {
        if (GetDefault(kAudioHardwarePropertyDefaultOutputDevice) == target) {
            SetDefault(kAudioHardwarePropertyDefaultOutputDevice, vdev);
            Log("default output switched to ForceStereo");
        }
        if (GetDefault(kAudioHardwarePropertyDefaultSystemOutputDevice) == target)
            SetDefault(kAudioHardwarePropertyDefaultSystemOutputDevice, vdev);
    }
    gFirstConfig = false;

    Float64 vrate = GetF64(vdev, kAudioDevicePropertyNominalSampleRate);
    Float64 trate = GetF64(target, kAudioDevicePropertyNominalSampleRate);
    pthread_mutex_lock(&gAUMutex);
    bool haveAU = (gAU != NULL);
    pthread_mutex_unlock(&gAUMutex);
    if (vdev == atomic_load(&gVDev) && target == prev && haveAU && vrate == gCfgVRate && trate == gCfgTRate) return;

    pthread_mutex_lock(&gAUMutex);
    TeardownAU();
    pthread_mutex_unlock(&gAUMutex);

    // Keep both devices at the same rate so nothing is resampled. A rate chosen on
    // ForceStereo is pushed to the hardware; otherwise ForceStereo follows the hardware.
    if (vrate != trate) {
        bool userChangedV = (gCfgVRate != 0 && vrate != gCfgVRate && target == prev);
        if (userChangedV && SupportsRate(target, vrate)) SetRateAndWait(target, vrate);
        else if (SupportsRate(vdev, trate)) SetRateAndWait(vdev, trate);
        vrate = GetF64(vdev, kAudioDevicePropertyNominalSampleRate);
        trate = GetF64(target, kAudioDevicePropertyNominalSampleRate);
    }

    char name[256];
    GetString(target, kAudioObjectPropertyName, name, sizeof(name));
    pthread_mutex_lock(&gAUMutex);
    bool ok = SetupAU(vdev, target, vrate, trate);
    pthread_mutex_unlock(&gAUMutex);
    WatchDevice(&gListenTarget, target);
    atomic_store(&gTarget, target);
    atomic_store(&gVDev, vdev);
    if (ok) {
        gCfgVRate = vrate;
        gCfgTRate = trate;
        Log("forwarding ForceStereo -> \"%s\" channels %u/%u of %u at %.0f Hz%s", name, gOptChL, gOptChR, gOutCh, trate,
            vrate != trate ? " (rates differ: converting)" : "");
    } else {
        gCfgVRate = gCfgTRate = 0; // retried by the health timer
    }
}

#pragma mark Commands

static int ListDevices(void)
{
    AudioObjectID vdev = DeviceForUID(kFS_DeviceUID);
    AudioObjectID target = PickTarget(vdev);
    AudioObjectID* ids;
    UInt32 n = CopyDevices(&ids);
    printf("ForceStereo driver: %s\n\n", vdev ? "installed" : "NOT installed");
    for (UInt32 i = 0; i < n; i++) {
        UInt32 ch = OutputChannels(ids[i]);
        if (!ch) continue;
        char uid[256], name[256];
        GetString(ids[i], kAudioDevicePropertyDeviceUID, uid, sizeof(uid));
        GetString(ids[i], kAudioObjectPropertyName, name, sizeof(name));
        UInt32 t = 0, stereo[2] = { 0, 0 };
        GetU32(ids[i], kAudioDevicePropertyTransportType, GLOBAL, &t);
        AudioObjectPropertyAddress a = { kAudioDevicePropertyPreferredChannelsForStereo, OUTPUT, MAIN };
        UInt32 size = sizeof(stereo);
        AudioObjectGetPropertyData(ids[i], &a, 0, NULL, &size, stereo);
        printf("%s %s\n    uid: %s\n    %s, %u output channels, %.0f Hz, system stereo pair %u/%u\n",
               ids[i] == target ? "->" : "  ", name, uid, TransportName(t), ch,
               GetF64(ids[i], kAudioDevicePropertyNominalSampleRate), stereo[0], stereo[1]);
    }
    free(ids);
    printf("\n-> marks the device audio would be forwarded to.\n");
    return 0;
}

static void Usage(void)
{
    puts("usage: forcestereo [options]        forward ForceStereo to a real output device\n"
         "       forcestereo list             show output devices and the chosen target\n"
         "\n"
         "  --target <uid|name>   pin the output device (default: first USB/Thunderbolt/FireWire/PCI\n"
         "                        interface, falling back to the built-in output)\n"
         "  --channels <L,R>      device channels to play on (default 1,2)\n"
         "  --latency <ms>        safety buffer in the helper (default 10)\n"
         "  --buffer <frames>     IO buffer size on the output device (default 256 at 44.1/48 kHz)\n"
         "  --no-default          do not make ForceStereo the default output when the target appears\n"
         "  --null                play nothing; only drain the device and print statistics\n"
         "  -v                    print statistics every 5 seconds");
}

int main(int argc, char** argv)
{
    setvbuf(stderr, NULL, _IOLBF, 0);
    bool list = false;
    for (int i = 1; i < argc; i++) {
        const char* s = argv[i];
        const char* next = (i + 1 < argc) ? argv[i + 1] : NULL;
        if (!strcmp(s, "list")) list = true;
        else if (!strcmp(s, "--target") && next) { gOptTarget = next; i++; }
        else if (!strcmp(s, "--channels") && next && sscanf(next, "%u,%u", &gOptChL, &gOptChR) == 2 &&
                 gOptChL >= 1 && gOptChR >= 1 && gOptChL <= 64 && gOptChR <= 64) i++;
        else if (!strcmp(s, "--latency") && next) { gOptLatencyMs = atof(next); i++; }
        else if (!strcmp(s, "--buffer") && next) { gOptBuffer = (UInt32)atoi(next); i++; }
        else if (!strcmp(s, "--no-default")) gOptGrabDefault = false;
        else if (!strcmp(s, "--null")) gOptNull = true;
        else if (!strcmp(s, "-v")) gOptVerbose = true;
        else { Usage(); return strcmp(s, "-h") && strcmp(s, "--help") ? 2 : 0; }
    }
    if (gOptLatencyMs < 2) gOptLatencyMs = 2;
    if (gOptLatencyMs > 200) gOptLatencyMs = 200;
    if (list) return ListDevices();

    gListener = ^(UInt32 n, const AudioObjectPropertyAddress* a) { (void)n; (void)a; Schedule(); };
    Listen(kAudioObjectSystemObject, kAudioHardwarePropertyDevices, true);
    Listen(kAudioObjectSystemObject, kAudioHardwarePropertyDefaultOutputDevice, true);
    Listen(kAudioObjectSystemObject, kAudioHardwarePropertyDefaultSystemOutputDevice, true);

    signal(SIGINT, SIG_DFL);
    signal(SIGTERM, SIG_DFL);

    Reconfigure();

    pthread_t th;
    pthread_create(&th, NULL, PollThread, NULL);

    // Health check: recovers from coreaudiod restarts, failed opens and missed notifications.
    dispatch_source_t timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, dispatch_get_main_queue());
    dispatch_source_set_timer(timer, dispatch_time(DISPATCH_TIME_NOW, 2 * NSEC_PER_SEC), 2 * NSEC_PER_SEC, NSEC_PER_SEC / 2);
    dispatch_source_set_event_handler(timer, ^{ Reconfigure(); });
    dispatch_resume(timer);

    dispatch_main();
    return 0;
}
