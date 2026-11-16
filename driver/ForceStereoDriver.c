// ForceStereo — AudioServerPlugIn exposing a 2-channel output device.
//
// Audio mixed by the system into this device is appended to a FIFO. A user-space
// helper drains the FIFO through a custom property (no input stream, so no
// microphone permission is involved) and plays it on channels 1-2 of a real device.
// The helper also steers this device's clock (kFS_PropRate) so that it runs at the
// exact rate of the real hardware; no resampling happens anywhere.

#include <CoreAudio/AudioServerPlugIn.h>
#include <CoreFoundation/CoreFoundation.h>
#include <dispatch/dispatch.h>
#include <mach/mach_time.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stddef.h>
#include <string.h>
#include <math.h>

#include "../shared/ForceStereoShared.h"

enum {
    kObj_PlugIn = kAudioObjectPlugInObject,
    kObj_Device = 2,
    kObj_Stream = 3,
    kObj_Volume = 4,
    kObj_Mute   = 5
};

#define kManufacturer        "ForceStereo"
#define kZeroTimeStampPeriod 16384
#define kRingFrames          65536u /* power of two */
#define kRingMask            (kRingFrames - 1)
#define kMinDB               (-96.0f)
#define kNumRates            6

static const Float64 kRates[kNumRates] = { 44100, 48000, 88200, 96000, 176400, 192000 };

#pragma mark State

static pthread_mutex_t gStateMutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t gIOMutex    = PTHREAD_MUTEX_INITIALIZER;

static UInt32                    gRefCount = 0;
static AudioServerPlugInHostRef  gHost = NULL;

static Float64 gSampleRate  = 48000.0;
static Float64 gRateRatio   = 1.0;
static Float64 gTicksPerSec = 0;
static Float64 gTicksPerFrame = 0;
static UInt64  gAnchorHost  = 0;
static Float64 gAnchorSample = 0;
static UInt64  gSeed        = 1;
static UInt32  gIOCount     = 0;
static _Atomic UInt32 gRunning = 0;

static Float32 gVolume = 1.0f;   // scalar 0..1
static bool    gMute   = false;
static _Atomic Float32 gGain = 1.0f;
static SInt32  gLatency = 0;

static Float32 gRing[kRingFrames * kFS_Channels];
static _Atomic UInt64 gWriteIndex = 0;
static UInt64  gReadCursor = 0;

#pragma mark Helpers

static Float32 ScalarToDB(Float32 s)
{
    if (s <= 0.0f) return kMinDB;
    Float32 db = 60.0f * log10f(s);
    return db < kMinDB ? kMinDB : (db > 0.0f ? 0.0f : db);
}

static Float32 DBToScalar(Float32 db)
{
    if (db <= kMinDB) return 0.0f;
    if (db >= 0.0f) return 1.0f;
    return powf(10.0f, db / 60.0f);
}

static void UpdateGain(void)
{
    Float32 g = 0.0f;
    if (!gMute && gVolume > 0.0f) {
        Float32 db = ScalarToDB(gVolume);
        g = (db <= kMinDB) ? 0.0f : powf(10.0f, db / 20.0f);
    }
    atomic_store(&gGain, g);
}

static void UpdateTicks(void) { gTicksPerFrame = gTicksPerSec / (gSampleRate * gRateRatio); }

// Move the clock anchor to "now" keeping the sample timeline continuous. gIOMutex held.
static void ReAnchor(void)
{
    UInt64 now = mach_absolute_time();
    if (gIOCount > 0 && gTicksPerFrame > 0 && now > gAnchorHost)
        gAnchorSample += (Float64)(now - gAnchorHost) / gTicksPerFrame;
    gAnchorHost = now;
}

static bool IsSupportedRate(Float64 r)
{
    for (int i = 0; i < kNumRates; i++) if (kRates[i] == r) return true;
    return false;
}

static AudioStreamBasicDescription MakeFormat(Float64 rate)
{
    AudioStreamBasicDescription f = {0};
    f.mSampleRate = rate;
    f.mFormatID = kAudioFormatLinearPCM;
    f.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagsNativeEndian | kAudioFormatFlagIsPacked;
    f.mBytesPerPacket = sizeof(Float32) * kFS_Channels;
    f.mFramesPerPacket = 1;
    f.mBytesPerFrame = sizeof(Float32) * kFS_Channels;
    f.mChannelsPerFrame = kFS_Channels;
    f.mBitsPerChannel = 32;
    return f;
}

static void StoreNumber(CFStringRef key, CFNumberType type, const void* value)
{
    if (!gHost) return;
    CFNumberRef n = CFNumberCreate(NULL, type, value);
    if (n) { gHost->WriteToStorage(gHost, key, n); CFRelease(n); }
}

static bool LoadNumber(CFStringRef key, CFNumberType type, void* value)
{
    if (!gHost) return false;
    CFPropertyListRef p = NULL;
    bool ok = false;
    if (gHost->CopyFromStorage(gHost, key, &p) == 0 && p) {
        if (CFGetTypeID(p) == CFNumberGetTypeID()) ok = CFNumberGetValue((CFNumberRef)p, type, value);
        CFRelease(p);
    }
    return ok;
}

static void NotifyChanged(AudioObjectID obj, UInt32 n, const AudioObjectPropertyAddress* addrs)
{
    if (gHost && n) gHost->PropertiesChanged(gHost, obj, n, addrs);
}

static void RequestRateChange(Float64 rate)
{
    AudioServerPlugInHostRef host = gHost;
    if (!host) return;
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_DEFAULT, 0), ^{
        host->RequestDeviceConfigurationChange(host, kObj_Device, (UInt64)rate, NULL);
    });
}

#pragma mark Property table

// Returns true when the object has the property; fills size and settability.
static bool PropInfo(AudioObjectID obj, const AudioObjectPropertyAddress* a, UInt32* size, bool* settable)
{
    UInt32 sz = 0; bool set = false; bool has = true;
    const UInt32 ref = sizeof(CFTypeRef);
    switch (obj) {
    case kObj_PlugIn:
        switch (a->mSelector) {
        case kAudioObjectPropertyBaseClass: case kAudioObjectPropertyClass: case kAudioObjectPropertyOwner:
        case kAudioObjectPropertyOwnedObjects: case kAudioPlugInPropertyDeviceList:
        case kAudioPlugInPropertyTranslateUIDToDevice: sz = 4; break;
        case kAudioObjectPropertyManufacturer: case kAudioPlugInPropertyResourceBundle: sz = ref; break;
        default: has = false;
        }
        break;
    case kObj_Device:
        switch (a->mSelector) {
        case kAudioObjectPropertyBaseClass: case kAudioObjectPropertyClass: case kAudioObjectPropertyOwner:
        case kAudioDevicePropertyTransportType: case kAudioDevicePropertyRelatedDevices:
        case kAudioDevicePropertyClockDomain: case kAudioDevicePropertyDeviceIsAlive:
        case kAudioDevicePropertyDeviceIsRunning: case kAudioDevicePropertyDeviceCanBeDefaultDevice:
        case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice: case kAudioDevicePropertyLatency:
        case kAudioDevicePropertySafetyOffset: case kAudioDevicePropertyIsHidden:
        case kAudioDevicePropertyZeroTimeStampPeriod: sz = 4; break;
        case kAudioObjectPropertyName: case kAudioObjectPropertyManufacturer:
        case kAudioDevicePropertyDeviceUID: case kAudioDevicePropertyModelUID: sz = ref; break;
        case kAudioObjectPropertyOwnedObjects:
            sz = (a->mScope == kAudioObjectPropertyScopeInput) ? 0 : 12; break;
        case kAudioDevicePropertyStreams:
            sz = (a->mScope == kAudioObjectPropertyScopeInput) ? 0 : 4; break;
        case kAudioObjectPropertyControlList: sz = 8; break;
        case kAudioDevicePropertyNominalSampleRate: sz = sizeof(Float64); set = true; break;
        case kAudioDevicePropertyAvailableNominalSampleRates: sz = kNumRates * sizeof(AudioValueRange); break;
        case kAudioDevicePropertyPreferredChannelsForStereo: sz = 8; break;
        case kAudioDevicePropertyPreferredChannelLayout:
            sz = (UInt32)(offsetof(AudioChannelLayout, mChannelDescriptions) + kFS_Channels * sizeof(AudioChannelDescription)); break;
        case kAudioObjectPropertyCustomPropertyInfoList: sz = 3 * sizeof(AudioServerPlugInCustomPropertyInfo); break;
        case kFS_PropRead: sz = ref; break;
        case kFS_PropRate: case kFS_PropLatency: sz = ref; set = true; break;
        default: has = false;
        }
        break;
    case kObj_Stream:
        switch (a->mSelector) {
        case kAudioObjectPropertyBaseClass: case kAudioObjectPropertyClass: case kAudioObjectPropertyOwner:
        case kAudioStreamPropertyDirection: case kAudioStreamPropertyTerminalType:
        case kAudioStreamPropertyStartingChannel: case kAudioStreamPropertyLatency: sz = 4; break;
        case kAudioObjectPropertyOwnedObjects: sz = 0; break;
        case kAudioStreamPropertyIsActive: sz = 4; set = true; break;
        case kAudioStreamPropertyVirtualFormat: case kAudioStreamPropertyPhysicalFormat:
            sz = sizeof(AudioStreamBasicDescription); set = true; break;
        case kAudioStreamPropertyAvailableVirtualFormats: case kAudioStreamPropertyAvailablePhysicalFormats:
            sz = kNumRates * sizeof(AudioStreamRangedDescription); break;
        default: has = false;
        }
        break;
    case kObj_Volume:
        switch (a->mSelector) {
        case kAudioObjectPropertyBaseClass: case kAudioObjectPropertyClass: case kAudioObjectPropertyOwner:
        case kAudioControlPropertyScope: case kAudioControlPropertyElement:
        case kAudioLevelControlPropertyConvertScalarToDecibels:
        case kAudioLevelControlPropertyConvertDecibelsToScalar: sz = 4; break;
        case kAudioObjectPropertyOwnedObjects: sz = 0; break;
        case kAudioLevelControlPropertyScalarValue: case kAudioLevelControlPropertyDecibelValue: sz = 4; set = true; break;
        case kAudioLevelControlPropertyDecibelRange: sz = sizeof(AudioValueRange); break;
        default: has = false;
        }
        break;
    case kObj_Mute:
        switch (a->mSelector) {
        case kAudioObjectPropertyBaseClass: case kAudioObjectPropertyClass: case kAudioObjectPropertyOwner:
        case kAudioControlPropertyScope: case kAudioControlPropertyElement: sz = 4; break;
        case kAudioObjectPropertyOwnedObjects: sz = 0; break;
        case kAudioBooleanControlPropertyValue: sz = 4; set = true; break;
        default: has = false;
        }
        break;
    default: has = false;
    }
    if (size) *size = sz;
    if (settable) *settable = set;
    return has;
}

#pragma mark FIFO

// Real-time: append the mixed output to the FIFO.
static void FifoWrite(const Float32* src, UInt32 frames)
{
    if (frames > kRingFrames) { src += (frames - kRingFrames) * kFS_Channels; frames = kRingFrames; }
    UInt64 w = atomic_load_explicit(&gWriteIndex, memory_order_relaxed);
    Float32 g = atomic_load_explicit(&gGain, memory_order_relaxed);
    UInt32 pos = (UInt32)(w & kRingMask);
    UInt32 first = kRingFrames - pos;
    if (first > frames) first = frames;
    Float32* d = &gRing[pos * kFS_Channels];
    if (g == 1.0f) {
        memcpy(d, src, first * kFS_Channels * sizeof(Float32));
        memcpy(gRing, src + first * kFS_Channels, (frames - first) * kFS_Channels * sizeof(Float32));
    } else {
        UInt32 n1 = first * kFS_Channels, n2 = (frames - first) * kFS_Channels;
        for (UInt32 i = 0; i < n1; i++) d[i] = src[i] * g;
        for (UInt32 i = 0; i < n2; i++) gRing[i] = src[n1 + i] * g;
    }
    atomic_store_explicit(&gWriteIndex, w + frames, memory_order_release);
}

// Non-real-time: hand the frames after `want` (or after the internal cursor) to the helper.
static CFDataRef FifoCopy(bool haveWant, UInt64 want)
{
    pthread_mutex_lock(&gStateMutex);
    UInt64 r = haveWant ? want : gReadCursor;
    UInt64 w = atomic_load_explicit(&gWriteIndex, memory_order_acquire);
    const UInt64 guard = 8192;
    if (r > w || w - r > kRingFrames - guard) r = w; // helper restarted or fell behind: resync
    UInt32 frames = (UInt32)((w - r) > kFS_MaxReadFrames ? kFS_MaxReadFrames : (w - r));

    CFIndex bytes = (CFIndex)(sizeof(FSReadHeader) + frames * kFS_Channels * sizeof(Float32));
    CFMutableDataRef data = CFDataCreateMutable(NULL, bytes);
    if (!data) { pthread_mutex_unlock(&gStateMutex); return NULL; }
    CFDataSetLength(data, bytes);
    UInt8* p = CFDataGetMutableBytePtr(data);
    Float32* dst = (Float32*)(p + sizeof(FSReadHeader));

    UInt32 pos = (UInt32)(r & kRingMask);
    UInt32 first = kRingFrames - pos;
    if (first > frames) first = frames;
    memcpy(dst, &gRing[pos * kFS_Channels], first * kFS_Channels * sizeof(Float32));
    memcpy(dst + first * kFS_Channels, gRing, (frames - first) * kFS_Channels * sizeof(Float32));

    UInt64 w2 = atomic_load_explicit(&gWriteIndex, memory_order_acquire);
    if (w2 - r > kRingFrames) { // overwritten while copying
        r = w2; frames = 0;
        CFDataSetLength(data, sizeof(FSReadHeader));
        p = CFDataGetMutableBytePtr(data);
    }
    FSReadHeader h = {0};
    h.magic = kFS_ReadMagic;
    h.frames = frames;
    h.startIndex = r;
    h.writeIndex = w2;
    h.sampleRate = gSampleRate;
    h.running = atomic_load(&gRunning);
    memcpy(p, &h, sizeof(h));
    gReadCursor = r + frames;
    pthread_mutex_unlock(&gStateMutex);
    return data;
}

#pragma mark COM plumbing

static AudioServerPlugInDriverInterface  gInterface;
static AudioServerPlugInDriverInterface* gInterfacePtr = &gInterface;
static AudioServerPlugInDriverRef        gDriverRef = &gInterfacePtr;

static HRESULT FS_QueryInterface(void* drv, REFIID iid, LPVOID* out)
{
    if (drv != gDriverRef || !out) return kAudioHardwareBadObjectError;
    CFUUIDRef req = CFUUIDCreateFromUUIDBytes(NULL, iid);
    HRESULT res = E_NOINTERFACE;
    if (req) {
        if (CFEqual(req, IUnknownUUID) || CFEqual(req, kAudioServerPlugInDriverInterfaceUUID)) {
            pthread_mutex_lock(&gStateMutex);
            ++gRefCount;
            pthread_mutex_unlock(&gStateMutex);
            *out = gDriverRef;
            res = S_OK;
        }
        CFRelease(req);
    }
    return res;
}

static ULONG FS_AddRef(void* drv)
{
    if (drv != gDriverRef) return 0;
    pthread_mutex_lock(&gStateMutex);
    ULONG r = ++gRefCount;
    pthread_mutex_unlock(&gStateMutex);
    return r;
}

static ULONG FS_Release(void* drv)
{
    if (drv != gDriverRef) return 0;
    pthread_mutex_lock(&gStateMutex);
    if (gRefCount > 0) --gRefCount;
    ULONG r = gRefCount;
    pthread_mutex_unlock(&gStateMutex);
    return r;
}

static OSStatus FS_Initialize(AudioServerPlugInDriverRef drv, AudioServerPlugInHostRef host)
{
    if (drv != gDriverRef) return kAudioHardwareBadObjectError;
    gHost = host;

    struct mach_timebase_info tb;
    mach_timebase_info(&tb);
    gTicksPerSec = 1e9 * (Float64)tb.denom / (Float64)tb.numer;

    Float64 rate; Float32 vol; SInt32 mute;
    if (LoadNumber(CFSTR("rate"), kCFNumberFloat64Type, &rate) && IsSupportedRate(rate)) gSampleRate = rate;
    if (LoadNumber(CFSTR("volume"), kCFNumberFloat32Type, &vol) && vol >= 0.0f && vol <= 1.0f) gVolume = vol;
    if (LoadNumber(CFSTR("mute"), kCFNumberSInt32Type, &mute)) gMute = (mute != 0);
    UpdateTicks();
    UpdateGain();
    return 0;
}

static OSStatus FS_CreateDevice(AudioServerPlugInDriverRef d, CFDictionaryRef desc, const AudioServerPlugInClientInfo* ci, AudioObjectID* out)
{ (void)d; (void)desc; (void)ci; (void)out; return kAudioHardwareUnsupportedOperationError; }

static OSStatus FS_DestroyDevice(AudioServerPlugInDriverRef d, AudioObjectID id)
{ (void)d; (void)id; return kAudioHardwareUnsupportedOperationError; }

static OSStatus FS_AddDeviceClient(AudioServerPlugInDriverRef d, AudioObjectID id, const AudioServerPlugInClientInfo* ci)
{ (void)d; (void)id; (void)ci; return 0; }

static OSStatus FS_RemoveDeviceClient(AudioServerPlugInDriverRef d, AudioObjectID id, const AudioServerPlugInClientInfo* ci)
{ (void)d; (void)id; (void)ci; return 0; }

static OSStatus FS_PerformConfigChange(AudioServerPlugInDriverRef d, AudioObjectID id, UInt64 action, void* info)
{
    (void)info;
    if (d != gDriverRef || id != kObj_Device) return kAudioHardwareBadObjectError;
    Float64 rate = (Float64)action;
    if (!IsSupportedRate(rate)) return kAudioHardwareBadPropertySizeError;
    pthread_mutex_lock(&gStateMutex);
    pthread_mutex_lock(&gIOMutex);
    gSampleRate = rate;
    UpdateTicks();
    gAnchorHost = mach_absolute_time();
    gAnchorSample = 0;
    ++gSeed;
    pthread_mutex_unlock(&gIOMutex);
    pthread_mutex_unlock(&gStateMutex);
    StoreNumber(CFSTR("rate"), kCFNumberFloat64Type, &rate);
    return 0;
}

static OSStatus FS_AbortConfigChange(AudioServerPlugInDriverRef d, AudioObjectID id, UInt64 action, void* info)
{ (void)d; (void)id; (void)action; (void)info; return 0; }

#pragma mark Property entry points

static Boolean FS_HasProperty(AudioServerPlugInDriverRef d, AudioObjectID obj, pid_t pid, const AudioObjectPropertyAddress* a)
{
    (void)pid;
    if (d != gDriverRef || !a) return false;
    return PropInfo(obj, a, NULL, NULL);
}

static OSStatus FS_IsPropertySettable(AudioServerPlugInDriverRef d, AudioObjectID obj, pid_t pid, const AudioObjectPropertyAddress* a, Boolean* out)
{
    (void)pid;
    if (d != gDriverRef || !a || !out) return kAudioHardwareIllegalOperationError;
    bool set;
    if (!PropInfo(obj, a, NULL, &set)) return kAudioHardwareUnknownPropertyError;
    *out = set;
    return 0;
}

static OSStatus FS_GetPropertyDataSize(AudioServerPlugInDriverRef d, AudioObjectID obj, pid_t pid, const AudioObjectPropertyAddress* a, UInt32 qs, const void* q, UInt32* out)
{
    (void)pid; (void)qs; (void)q;
    if (d != gDriverRef || !a || !out) return kAudioHardwareIllegalOperationError;
    if (!PropInfo(obj, a, out, NULL)) return kAudioHardwareUnknownPropertyError;
    return 0;
}

#define NEED(sz)  do { if (inSize < (UInt32)(sz)) return kAudioHardwareBadPropertySizeError; } while (0)
#define RET(type, value) do { NEED(sizeof(type)); *(type*)out = (value); *outSize = sizeof(type); return 0; } while (0)

// Copies up to as many IDs as fit.
static OSStatus RetIDs(const AudioObjectID* ids, UInt32 count, UInt32 inSize, UInt32* outSize, void* out)
{
    UInt32 n = inSize / sizeof(AudioObjectID);
    if (n > count) n = count;
    memcpy(out, ids, n * sizeof(AudioObjectID));
    *outSize = n * sizeof(AudioObjectID);
    return 0;
}

static OSStatus FS_GetPropertyData(AudioServerPlugInDriverRef d, AudioObjectID obj, pid_t pid, const AudioObjectPropertyAddress* a,
                                   UInt32 qs, const void* q, UInt32 inSize, UInt32* outSize, void* out)
{
    (void)pid;
    if (d != gDriverRef || !a || !outSize || !out) return kAudioHardwareIllegalOperationError;
    if (!PropInfo(obj, a, NULL, NULL)) return kAudioHardwareUnknownPropertyError;

    switch (obj) {
    case kObj_PlugIn:
        switch (a->mSelector) {
        case kAudioObjectPropertyBaseClass: RET(AudioClassID, kAudioObjectClassID);
        case kAudioObjectPropertyClass: RET(AudioClassID, kAudioPlugInClassID);
        case kAudioObjectPropertyOwner: RET(AudioObjectID, kAudioObjectUnknown);
        case kAudioObjectPropertyManufacturer: RET(CFStringRef, CFSTR(kManufacturer));
        case kAudioObjectPropertyOwnedObjects:
        case kAudioPlugInPropertyDeviceList: {
            AudioObjectID ids[] = { kObj_Device };
            return RetIDs(ids, 1, inSize, outSize, out);
        }
        case kAudioPlugInPropertyTranslateUIDToDevice: {
            NEED(sizeof(AudioObjectID));
            if (qs != sizeof(CFStringRef) || !q) return kAudioHardwareBadPropertySizeError;
            CFStringRef uid = *(CFStringRef*)q;
            *(AudioObjectID*)out = (uid && CFEqual(uid, CFSTR(kFS_DeviceUID))) ? kObj_Device : kAudioObjectUnknown;
            *outSize = sizeof(AudioObjectID);
            return 0;
        }
        case kAudioPlugInPropertyResourceBundle: RET(CFStringRef, CFSTR(""));
        }
        break;

    case kObj_Device:
        switch (a->mSelector) {
        case kAudioObjectPropertyBaseClass: RET(AudioClassID, kAudioObjectClassID);
        case kAudioObjectPropertyClass: RET(AudioClassID, kAudioDeviceClassID);
        case kAudioObjectPropertyOwner: RET(AudioObjectID, kObj_PlugIn);
        case kAudioObjectPropertyName: RET(CFStringRef, CFSTR(kFS_DeviceName));
        case kAudioObjectPropertyManufacturer: RET(CFStringRef, CFSTR(kManufacturer));
        case kAudioDevicePropertyDeviceUID: RET(CFStringRef, CFSTR(kFS_DeviceUID));
        case kAudioDevicePropertyModelUID: RET(CFStringRef, CFSTR(kFS_ModelUID));
        case kAudioDevicePropertyTransportType: RET(UInt32, kAudioDeviceTransportTypeVirtual);
        case kAudioDevicePropertyRelatedDevices: {
            AudioObjectID ids[] = { kObj_Device };
            return RetIDs(ids, 1, inSize, outSize, out);
        }
        case kAudioDevicePropertyClockDomain: RET(UInt32, 0);
        case kAudioDevicePropertyDeviceIsAlive: RET(UInt32, 1);
        case kAudioDevicePropertyDeviceIsRunning: RET(UInt32, atomic_load(&gRunning) ? 1 : 0);
        case kAudioDevicePropertyDeviceCanBeDefaultDevice:
        case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice:
            RET(UInt32, a->mScope == kAudioObjectPropertyScopeInput ? 0 : 1);
        case kAudioDevicePropertyLatency: {
            pthread_mutex_lock(&gStateMutex);
            UInt32 l = (a->mScope == kAudioObjectPropertyScopeInput) ? 0 : (UInt32)gLatency;
            pthread_mutex_unlock(&gStateMutex);
            RET(UInt32, l);
        }
        case kAudioDevicePropertySafetyOffset: RET(UInt32, 0);
        case kAudioDevicePropertyIsHidden: RET(UInt32, 0);
        case kAudioDevicePropertyZeroTimeStampPeriod: RET(UInt32, kZeroTimeStampPeriod);
        case kAudioObjectPropertyOwnedObjects: {
            if (a->mScope == kAudioObjectPropertyScopeInput) { *outSize = 0; return 0; }
            AudioObjectID ids[] = { kObj_Stream, kObj_Volume, kObj_Mute };
            return RetIDs(ids, 3, inSize, outSize, out);
        }
        case kAudioDevicePropertyStreams: {
            if (a->mScope == kAudioObjectPropertyScopeInput) { *outSize = 0; return 0; }
            AudioObjectID ids[] = { kObj_Stream };
            return RetIDs(ids, 1, inSize, outSize, out);
        }
        case kAudioObjectPropertyControlList: {
            AudioObjectID ids[] = { kObj_Volume, kObj_Mute };
            return RetIDs(ids, 2, inSize, outSize, out);
        }
        case kAudioDevicePropertyNominalSampleRate: {
            pthread_mutex_lock(&gStateMutex);
            Float64 r = gSampleRate;
            pthread_mutex_unlock(&gStateMutex);
            RET(Float64, r);
        }
        case kAudioDevicePropertyAvailableNominalSampleRates: {
            UInt32 n = inSize / sizeof(AudioValueRange);
            if (n > kNumRates) n = kNumRates;
            for (UInt32 i = 0; i < n; i++) {
                ((AudioValueRange*)out)[i].mMinimum = kRates[i];
                ((AudioValueRange*)out)[i].mMaximum = kRates[i];
            }
            *outSize = n * sizeof(AudioValueRange);
            return 0;
        }
        case kAudioDevicePropertyPreferredChannelsForStereo: {
            NEED(2 * sizeof(UInt32));
            ((UInt32*)out)[0] = 1; ((UInt32*)out)[1] = 2;
            *outSize = 2 * sizeof(UInt32);
            return 0;
        }
        case kAudioDevicePropertyPreferredChannelLayout: {
            UInt32 sz = (UInt32)(offsetof(AudioChannelLayout, mChannelDescriptions) + kFS_Channels * sizeof(AudioChannelDescription));
            NEED(sz);
            AudioChannelLayout* l = (AudioChannelLayout*)out;
            memset(l, 0, sz);
            l->mChannelLayoutTag = kAudioChannelLayoutTag_UseChannelDescriptions;
            l->mNumberChannelDescriptions = kFS_Channels;
            l->mChannelDescriptions[0].mChannelLabel = kAudioChannelLabel_Left;
            l->mChannelDescriptions[1].mChannelLabel = kAudioChannelLabel_Right;
            *outSize = sz;
            return 0;
        }
        case kAudioObjectPropertyCustomPropertyInfoList: {
            const AudioServerPlugInCustomPropertyInfo infos[3] = {
                { kFS_PropRead,    kAudioServerPlugInCustomPropertyDataTypeCFPropertyList, kAudioServerPlugInCustomPropertyDataTypeCFPropertyList },
                { kFS_PropRate,    kAudioServerPlugInCustomPropertyDataTypeCFPropertyList, kAudioServerPlugInCustomPropertyDataTypeNone },
                { kFS_PropLatency, kAudioServerPlugInCustomPropertyDataTypeCFPropertyList, kAudioServerPlugInCustomPropertyDataTypeNone }
            };
            UInt32 n = inSize / sizeof(infos[0]);
            if (n > 3) n = 3;
            memcpy(out, infos, n * sizeof(infos[0]));
            *outSize = n * sizeof(infos[0]);
            return 0;
        }
        case kFS_PropRead: {
            NEED(sizeof(CFPropertyListRef));
            bool have = false; UInt64 want = 0;
            if (qs == sizeof(CFPropertyListRef) && q) {
                CFPropertyListRef pl = *(CFPropertyListRef*)q;
                SInt64 v;
                if (pl && CFGetTypeID(pl) == CFNumberGetTypeID() && CFNumberGetValue((CFNumberRef)pl, kCFNumberSInt64Type, &v)) {
                    have = true; want = (UInt64)v;
                }
            }
            CFDataRef data = FifoCopy(have, want);
            if (!data) return kAudioHardwareUnspecifiedError;
            *(CFPropertyListRef*)out = data;
            *outSize = sizeof(CFPropertyListRef);
            return 0;
        }
        case kFS_PropRate: {
            NEED(sizeof(CFPropertyListRef));
            pthread_mutex_lock(&gIOMutex);
            Float64 r = gRateRatio;
            pthread_mutex_unlock(&gIOMutex);
            *(CFPropertyListRef*)out = CFNumberCreate(NULL, kCFNumberFloat64Type, &r);
            *outSize = sizeof(CFPropertyListRef);
            return 0;
        }
        case kFS_PropLatency: {
            NEED(sizeof(CFPropertyListRef));
            pthread_mutex_lock(&gStateMutex);
            SInt32 l = gLatency;
            pthread_mutex_unlock(&gStateMutex);
            *(CFPropertyListRef*)out = CFNumberCreate(NULL, kCFNumberSInt32Type, &l);
            *outSize = sizeof(CFPropertyListRef);
            return 0;
        }
        }
        break;

    case kObj_Stream:
        switch (a->mSelector) {
        case kAudioObjectPropertyBaseClass: RET(AudioClassID, kAudioObjectClassID);
        case kAudioObjectPropertyClass: RET(AudioClassID, kAudioStreamClassID);
        case kAudioObjectPropertyOwner: RET(AudioObjectID, kObj_Device);
        case kAudioObjectPropertyOwnedObjects: *outSize = 0; return 0;
        case kAudioStreamPropertyIsActive: RET(UInt32, 1);
        case kAudioStreamPropertyDirection: RET(UInt32, 0);
        case kAudioStreamPropertyTerminalType: RET(UInt32, kAudioStreamTerminalTypeLine);
        case kAudioStreamPropertyStartingChannel: RET(UInt32, 1);
        case kAudioStreamPropertyLatency: RET(UInt32, 0);
        case kAudioStreamPropertyVirtualFormat:
        case kAudioStreamPropertyPhysicalFormat: {
            pthread_mutex_lock(&gStateMutex);
            Float64 r = gSampleRate;
            pthread_mutex_unlock(&gStateMutex);
            RET(AudioStreamBasicDescription, MakeFormat(r));
        }
        case kAudioStreamPropertyAvailableVirtualFormats:
        case kAudioStreamPropertyAvailablePhysicalFormats: {
            UInt32 n = inSize / sizeof(AudioStreamRangedDescription);
            if (n > kNumRates) n = kNumRates;
            for (UInt32 i = 0; i < n; i++) {
                AudioStreamRangedDescription* r = &((AudioStreamRangedDescription*)out)[i];
                r->mFormat = MakeFormat(kRates[i]);
                r->mSampleRateRange.mMinimum = kRates[i];
                r->mSampleRateRange.mMaximum = kRates[i];
            }
            *outSize = n * sizeof(AudioStreamRangedDescription);
            return 0;
        }
        }
        break;

    case kObj_Volume:
        switch (a->mSelector) {
        case kAudioObjectPropertyBaseClass: RET(AudioClassID, kAudioLevelControlClassID);
        case kAudioObjectPropertyClass: RET(AudioClassID, kAudioVolumeControlClassID);
        case kAudioObjectPropertyOwner: RET(AudioObjectID, kObj_Device);
        case kAudioObjectPropertyOwnedObjects: *outSize = 0; return 0;
        case kAudioControlPropertyScope: RET(AudioObjectPropertyScope, kAudioObjectPropertyScopeOutput);
        case kAudioControlPropertyElement: RET(AudioObjectPropertyElement, kAudioObjectPropertyElementMain);
        case kAudioLevelControlPropertyScalarValue: {
            pthread_mutex_lock(&gStateMutex);
            Float32 v = gVolume;
            pthread_mutex_unlock(&gStateMutex);
            RET(Float32, v);
        }
        case kAudioLevelControlPropertyDecibelValue: {
            pthread_mutex_lock(&gStateMutex);
            Float32 v = ScalarToDB(gVolume);
            pthread_mutex_unlock(&gStateMutex);
            RET(Float32, v);
        }
        case kAudioLevelControlPropertyDecibelRange: {
            NEED(sizeof(AudioValueRange));
            ((AudioValueRange*)out)->mMinimum = kMinDB;
            ((AudioValueRange*)out)->mMaximum = 0.0;
            *outSize = sizeof(AudioValueRange);
            return 0;
        }
        case kAudioLevelControlPropertyConvertScalarToDecibels: {
            NEED(sizeof(Float32));
            Float32 s = *(Float32*)out;
            s = s < 0.0f ? 0.0f : (s > 1.0f ? 1.0f : s);
            *(Float32*)out = ScalarToDB(s);
            *outSize = sizeof(Float32);
            return 0;
        }
        case kAudioLevelControlPropertyConvertDecibelsToScalar: {
            NEED(sizeof(Float32));
            *(Float32*)out = DBToScalar(*(Float32*)out);
            *outSize = sizeof(Float32);
            return 0;
        }
        }
        break;

    case kObj_Mute:
        switch (a->mSelector) {
        case kAudioObjectPropertyBaseClass: RET(AudioClassID, kAudioBooleanControlClassID);
        case kAudioObjectPropertyClass: RET(AudioClassID, kAudioMuteControlClassID);
        case kAudioObjectPropertyOwner: RET(AudioObjectID, kObj_Device);
        case kAudioObjectPropertyOwnedObjects: *outSize = 0; return 0;
        case kAudioControlPropertyScope: RET(AudioObjectPropertyScope, kAudioObjectPropertyScopeOutput);
        case kAudioControlPropertyElement: RET(AudioObjectPropertyElement, kAudioObjectPropertyElementMain);
        case kAudioBooleanControlPropertyValue: {
            pthread_mutex_lock(&gStateMutex);
            UInt32 v = gMute ? 1 : 0;
            pthread_mutex_unlock(&gStateMutex);
            RET(UInt32, v);
        }
        }
        break;
    }
    return kAudioHardwareUnknownPropertyError;
}

static OSStatus FS_SetPropertyData(AudioServerPlugInDriverRef d, AudioObjectID obj, pid_t pid, const AudioObjectPropertyAddress* a,
                                   UInt32 qs, const void* q, UInt32 inSize, const void* in)
{
    (void)pid; (void)qs; (void)q;
    if (d != gDriverRef || !a || !in) return kAudioHardwareIllegalOperationError;
    bool settable;
    if (!PropInfo(obj, a, NULL, &settable)) return kAudioHardwareUnknownPropertyError;
    if (!settable) return kAudioHardwareUnsupportedOperationError;

    if (obj == kObj_Device) {
        if (a->mSelector == kAudioDevicePropertyNominalSampleRate) {
            if (inSize != sizeof(Float64)) return kAudioHardwareBadPropertySizeError;
            Float64 rate = *(const Float64*)in;
            if (!IsSupportedRate(rate)) return kAudioHardwareIllegalOperationError;
            pthread_mutex_lock(&gStateMutex);
            bool differs = (rate != gSampleRate);
            pthread_mutex_unlock(&gStateMutex);
            if (differs) RequestRateChange(rate);
            return 0;
        }
        if (inSize != sizeof(CFPropertyListRef)) return kAudioHardwareBadPropertySizeError;
        CFPropertyListRef pl = *(CFPropertyListRef*)in;
        if (!pl || CFGetTypeID(pl) != CFNumberGetTypeID()) return kAudioHardwareIllegalOperationError;
        if (a->mSelector == kFS_PropRate) {
            Float64 r;
            if (!CFNumberGetValue((CFNumberRef)pl, kCFNumberFloat64Type, &r) || !(r > 0)) return kAudioHardwareIllegalOperationError;
            if (r < 0.99) r = 0.99;
            if (r > 1.01) r = 1.01;
            pthread_mutex_lock(&gIOMutex);
            ReAnchor();
            gRateRatio = r;
            UpdateTicks();
            pthread_mutex_unlock(&gIOMutex);
            return 0;
        }
        if (a->mSelector == kFS_PropLatency) {
            SInt32 l;
            if (!CFNumberGetValue((CFNumberRef)pl, kCFNumberSInt32Type, &l)) return kAudioHardwareIllegalOperationError;
            if (l < 0) l = 0;
            if (l > 192000) l = 192000;
            pthread_mutex_lock(&gStateMutex);
            bool changed = (l != gLatency);
            gLatency = l;
            pthread_mutex_unlock(&gStateMutex);
            if (changed) {
                AudioObjectPropertyAddress ch = { kAudioDevicePropertyLatency, kAudioObjectPropertyScopeOutput, kAudioObjectPropertyElementMain };
                NotifyChanged(kObj_Device, 1, &ch);
            }
            return 0;
        }
    } else if (obj == kObj_Stream) {
        if (a->mSelector == kAudioStreamPropertyIsActive) return 0;
        if (inSize != sizeof(AudioStreamBasicDescription)) return kAudioHardwareBadPropertySizeError;
        const AudioStreamBasicDescription* f = (const AudioStreamBasicDescription*)in;
        if (f->mFormatID != kAudioFormatLinearPCM || f->mChannelsPerFrame != kFS_Channels ||
            f->mBitsPerChannel != 32 || !(f->mFormatFlags & kAudioFormatFlagIsFloat) || !IsSupportedRate(f->mSampleRate))
            return kAudioDeviceUnsupportedFormatError;
        pthread_mutex_lock(&gStateMutex);
        bool differs = (f->mSampleRate != gSampleRate);
        pthread_mutex_unlock(&gStateMutex);
        if (differs) RequestRateChange(f->mSampleRate);
        return 0;
    } else if (obj == kObj_Volume) {
        if (inSize != sizeof(Float32)) return kAudioHardwareBadPropertySizeError;
        Float32 v = *(const Float32*)in;
        if (a->mSelector == kAudioLevelControlPropertyDecibelValue) v = DBToScalar(v);
        if (!(v >= 0.0f)) v = 0.0f;
        if (v > 1.0f) v = 1.0f;
        pthread_mutex_lock(&gStateMutex);
        bool changed = (v != gVolume);
        gVolume = v;
        UpdateGain();
        pthread_mutex_unlock(&gStateMutex);
        if (changed) {
            StoreNumber(CFSTR("volume"), kCFNumberFloat32Type, &v);
            AudioObjectPropertyAddress ch[2] = {
                { kAudioLevelControlPropertyScalarValue, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain },
                { kAudioLevelControlPropertyDecibelValue, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain }
            };
            NotifyChanged(kObj_Volume, 2, ch);
        }
        return 0;
    } else if (obj == kObj_Mute) {
        if (inSize != sizeof(UInt32)) return kAudioHardwareBadPropertySizeError;
        bool m = (*(const UInt32*)in != 0);
        pthread_mutex_lock(&gStateMutex);
        bool changed = (m != gMute);
        gMute = m;
        UpdateGain();
        pthread_mutex_unlock(&gStateMutex);
        if (changed) {
            SInt32 sv = m ? 1 : 0;
            StoreNumber(CFSTR("mute"), kCFNumberSInt32Type, &sv);
            AudioObjectPropertyAddress ch = { kAudioBooleanControlPropertyValue, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
            NotifyChanged(kObj_Mute, 1, &ch);
        }
        return 0;
    }
    return kAudioHardwareUnknownPropertyError;
}

#pragma mark IO

static OSStatus FS_StartIO(AudioServerPlugInDriverRef d, AudioObjectID dev, UInt32 client)
{
    (void)client;
    if (d != gDriverRef || dev != kObj_Device) return kAudioHardwareBadObjectError;
    pthread_mutex_lock(&gIOMutex);
    if (gIOCount == 0) {
        gAnchorHost = mach_absolute_time();
        gAnchorSample = 0;
        ++gSeed;
        atomic_store(&gRunning, 1);
    }
    ++gIOCount;
    pthread_mutex_unlock(&gIOMutex);
    return 0;
}

static OSStatus FS_StopIO(AudioServerPlugInDriverRef d, AudioObjectID dev, UInt32 client)
{
    (void)client;
    if (d != gDriverRef || dev != kObj_Device) return kAudioHardwareBadObjectError;
    pthread_mutex_lock(&gIOMutex);
    if (gIOCount > 0 && --gIOCount == 0) atomic_store(&gRunning, 0);
    pthread_mutex_unlock(&gIOMutex);
    return 0;
}

static OSStatus FS_GetZeroTimeStamp(AudioServerPlugInDriverRef d, AudioObjectID dev, UInt32 client,
                                    Float64* outSample, UInt64* outHost, UInt64* outSeed)
{
    (void)client;
    if (d != gDriverRef || dev != kObj_Device) return kAudioHardwareBadObjectError;
    pthread_mutex_lock(&gIOMutex);
    UInt64 now = mach_absolute_time();
    Float64 cur = gAnchorSample + (now > gAnchorHost ? (Float64)(now - gAnchorHost) / gTicksPerFrame : 0.0);
    Float64 st = floor(cur / kZeroTimeStampPeriod) * kZeroTimeStampPeriod;
    SInt64 off = (SInt64)llround((st - gAnchorSample) * gTicksPerFrame);
    *outSample = st;
    *outHost = (UInt64)((SInt64)gAnchorHost + off);
    *outSeed = gSeed;
    pthread_mutex_unlock(&gIOMutex);
    return 0;
}

static OSStatus FS_WillDoIOOperation(AudioServerPlugInDriverRef d, AudioObjectID dev, UInt32 client, UInt32 op,
                                     Boolean* outWillDo, Boolean* outInPlace)
{
    (void)client;
    if (d != gDriverRef || dev != kObj_Device) return kAudioHardwareBadObjectError;
    if (outWillDo) *outWillDo = (op == kAudioServerPlugInIOOperationWriteMix);
    if (outInPlace) *outInPlace = true;
    return 0;
}

static OSStatus FS_BeginIOOperation(AudioServerPlugInDriverRef d, AudioObjectID dev, UInt32 client, UInt32 op,
                                    UInt32 frames, const AudioServerPlugInIOCycleInfo* info)
{ (void)d; (void)dev; (void)client; (void)op; (void)frames; (void)info; return 0; }

static OSStatus FS_DoIOOperation(AudioServerPlugInDriverRef d, AudioObjectID dev, AudioObjectID stream, UInt32 client, UInt32 op,
                                 UInt32 frames, const AudioServerPlugInIOCycleInfo* info, void* mainBuf, void* secBuf)
{
    (void)client; (void)info; (void)secBuf;
    if (d != gDriverRef || dev != kObj_Device) return kAudioHardwareBadObjectError;
    if (op == kAudioServerPlugInIOOperationWriteMix && stream == kObj_Stream && mainBuf)
        FifoWrite((const Float32*)mainBuf, frames);
    return 0;
}

static OSStatus FS_EndIOOperation(AudioServerPlugInDriverRef d, AudioObjectID dev, UInt32 client, UInt32 op,
                                  UInt32 frames, const AudioServerPlugInIOCycleInfo* info)
{ (void)d; (void)dev; (void)client; (void)op; (void)frames; (void)info; return 0; }

#pragma mark Factory

static AudioServerPlugInDriverInterface gInterface = {
    NULL,
    FS_QueryInterface,
    FS_AddRef,
    FS_Release,
    FS_Initialize,
    FS_CreateDevice,
    FS_DestroyDevice,
    FS_AddDeviceClient,
    FS_RemoveDeviceClient,
    FS_PerformConfigChange,
    FS_AbortConfigChange,
    FS_HasProperty,
    FS_IsPropertySettable,
    FS_GetPropertyDataSize,
    FS_GetPropertyData,
    FS_SetPropertyData,
    FS_StartIO,
    FS_StopIO,
    FS_GetZeroTimeStamp,
    FS_WillDoIOOperation,
    FS_BeginIOOperation,
    FS_DoIOOperation,
    FS_EndIOOperation
};

__attribute__((visibility("default")))
void* ForceStereo_Create(CFAllocatorRef allocator, CFUUIDRef requestedTypeUUID)
{
    (void)allocator;
    if (CFEqual(requestedTypeUUID, kAudioServerPlugInTypeUUID)) return gDriverRef;
    return NULL;
}
