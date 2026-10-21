/*
 * ptzctl — long-lived helper that drives the Osmo gimbal over UVC PTZ.
 * Reads commands from stdin so the Python server doesn't fork per frame.
 *
 *   r                -> prints "range panMin panMax tiltMin tiltMax"
 *   g                -> prints "pos pan tilt"
 *   s <pan> <tilt>   -> absolute move, prints "ok" or "err"
 *   c                -> recenter
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef PTZ_H
#define PTZ_H

// UVC Pan/Tilt Absolute on the DJI Osmo Pocket 3 webcam, via IOKit.
// All functions return 0 on success. Angles are in degrees.

typedef struct {
  double panMin, panMax, tiltMin, tiltMax;
} ptz_range_t;

int ptz_get(double *pan, double *tilt);
int ptz_set(double pan, double tilt);
int ptz_range(ptz_range_t *out);

#endif


#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/IOCFPlugIn.h>
#include <IOKit/usb/IOUSBLib.h>

#define VID 0x2ca3          // DJI
#define PID 0x0023          // Osmo Pocket 3
#define CT_PANTILT_ABS 0x0D // UVC camera terminal control selector
#define WINDEX ((1 << 8) | 0) // camera terminal id 1, VideoControl interface 0

#define GET_CUR 0x81
#define GET_MIN 0x82
#define GET_MAX 0x83
#define SET_CUR 0x01

// Opens the device for one request; cheap enough at UI rates and survives replugging.
static IOUSBDeviceInterface **open_device(void) {
  CFMutableDictionaryRef match = IOServiceMatching("IOUSBHostDevice");
  int vid = VID, pid = PID;
  CFNumberRef v = CFNumberCreate(NULL, kCFNumberIntType, &vid);
  CFNumberRef p = CFNumberCreate(NULL, kCFNumberIntType, &pid);
  CFDictionarySetValue(match, CFSTR("idVendor"), v);
  CFDictionarySetValue(match, CFSTR("idProduct"), p);
  CFRelease(v);
  CFRelease(p);

  io_service_t svc = IOServiceGetMatchingService(kIOMainPortDefault, match);
  if (!svc) return NULL;

  IOCFPlugInInterface **plug = NULL;
  SInt32 score;
  kern_return_t kr = IOCreatePlugInInterfaceForService(svc, kIOUSBDeviceUserClientTypeID,
                                                       kIOCFPlugInInterfaceID, &plug, &score);
  IOObjectRelease(svc);
  if (kr || !plug) return NULL;

  IOUSBDeviceInterface **dev = NULL;
  (*plug)->QueryInterface(plug, CFUUIDGetUUIDBytes(kIOUSBDeviceInterfaceID), (LPVOID *)&dev);
  IODestroyPlugInInterface(plug);
  return dev;
}

static int request(UInt8 type, UInt8 req, int32_t v[2]) {
  IOUSBDeviceInterface **dev = open_device();
  if (!dev) return -1;
  IOUSBDevRequest r = {
    .bmRequestType = type, .bRequest = req,
    .wValue = CT_PANTILT_ABS << 8, .wIndex = WINDEX,
    .wLength = 8, .pData = v,
  };
  IOReturn kr = (*dev)->DeviceRequest(dev, &r);
  (*dev)->Release(dev);
  return kr ? -2 : 0;
}

static int get_pair(UInt8 req, double *pan, double *tilt) {
  int32_t v[2] = {0};
  int rc = request(0xA1, req, v);
  if (rc) return rc;
  *pan = v[0] / 3600.0; // UVC units are arc-seconds
  *tilt = v[1] / 3600.0;
  return 0;
}

int ptz_get(double *pan, double *tilt) {
  return get_pair(GET_CUR, pan, tilt);
}

int ptz_set(double pan, double tilt) {
  int32_t v[2] = { (int32_t)(pan * 3600), (int32_t)(tilt * 3600) };
  return request(0x21, SET_CUR, v);
}

int ptz_range(ptz_range_t *out) {
  int rc = get_pair(GET_MIN, &out->panMin, &out->tiltMin);
  return rc ? rc : get_pair(GET_MAX, &out->panMax, &out->tiltMax);
}


int main(void) {
    setvbuf(stdout, NULL, _IOLBF, 0);

    ptz_range_t range;
    int have_range = (ptz_range(&range) == 0);
    if (have_range)
        printf("range %f %f %f %f\n", range.panMin, range.panMax, range.tiltMin, range.tiltMax);
    else
        printf("range err\n");

    char line[256];
    while (fgets(line, sizeof(line), stdin)) {
        char cmd = line[0];
        if (cmd == 'r') {
            ptz_range_t r;
            if (ptz_range(&r) == 0) {
                printf("range %f %f %f %f\n", r.panMin, r.panMax, r.tiltMin, r.tiltMax);
                have_range = 1;
                range = r;
            } else {
                printf("range err\n");
            }
        } else if (cmd == 'g') {
            double pan, tilt;
            if (ptz_get(&pan, &tilt) == 0)
                printf("pos %f %f\n", pan, tilt);
            else
                printf("pos err\n");
        } else if (cmd == 's') {
            double pan = 0, tilt = 0;
            if (sscanf(line + 1, "%lf %lf", &pan, &tilt) == 2)
                printf("%s\n", ptz_set(pan, tilt) == 0 ? "ok" : "err");
            else
                printf("err\n");
        } else if (cmd == 'c') {
            double pan = have_range ? (range.panMin + range.panMax) / 2.0 : 90.0;
            printf("%s\n", ptz_set(pan, 0.0) == 0 ? "ok" : "err");
        }
    }
    return 0;
}
