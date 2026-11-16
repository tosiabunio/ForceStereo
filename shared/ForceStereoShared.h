// Shared between the HAL driver and the forwarding helper.
#ifndef FORCESTEREO_SHARED_H
#define FORCESTEREO_SHARED_H

#include <CoreAudio/CoreAudio.h>

#define kFS_DeviceUID   "ForceStereo_UID"
#define kFS_ModelUID    "ForceStereo_ModelUID"
#define kFS_DeviceName  "ForceStereo"
#define kFS_Channels    2

// Custom device properties (global scope, main element). All are CFPropertyList typed.
enum {
    kFS_PropRead    = 'fsrd', // get: CFData (FSReadHeader + interleaved Float32). Qualifier: CFNumber read index.
    kFS_PropRate    = 'fsrr', // get/set: CFNumber Float64, clock rate ratio (1.0 = nominal)
    kFS_PropLatency = 'fslt'  // get/set: CFNumber SInt32, frames of latency reported to clients
};

#define kFS_ReadMagic     0x46535231u /* 'FSR1' */
#define kFS_MaxReadFrames 8192

typedef struct {
    UInt32  magic;
    UInt32  frames;       // frames following this header
    UInt64  startIndex;   // stream index of the first frame returned
    UInt64  writeIndex;   // driver's write index at the time of the call
    Float64 sampleRate;   // nominal rate of the virtual device
    UInt32  running;      // non-zero while any client runs IO on the virtual device
    UInt32  reserved;
} FSReadHeader;

#endif
