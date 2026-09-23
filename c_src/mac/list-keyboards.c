#include <IOKit/hid/IOHIDLib.h>
#include <IOKit/hidsystem/IOHIDShared.h>
#include <inttypes.h>
#include <stdio.h>

/* The name was changed from "Master" to "Main" in Apple SDK 12.0 (Monterey) */
/* MAC_OS_X_VERSION_12_0 does not exist :( */
#if MAC_OS_X_VERSION_MAX_ALLOWED < 120000
    #define kIOMainPortDefault kIOMasterPortDefault
#endif

static void print_json_string(CFStringRef string) {
    char value[1024];
    if (!string || !CFStringGetCString(string, value, sizeof(value), kCFStringEncodingUTF8)) {
        fputs("null", stdout);
        return;
    }

    putchar('"');
    for (const unsigned char *p = (const unsigned char *)value; *p; ++p) {
        switch (*p) {
        case '"': fputs("\\\"", stdout); break;
        case '\\': fputs("\\\\", stdout); break;
        case '\n': fputs("\\n", stdout); break;
        case '\r': fputs("\\r", stdout); break;
        case '\t': fputs("\\t", stdout); break;
        default:
            if (*p < 0x20) {
                printf("\\u%04x", *p);
            } else {
                putchar(*p);
            }
        }
    }
    putchar('"');
}

int main() {
    CFMutableDictionaryRef matching_dictionary = IOServiceMatching(kIOHIDDeviceKey);
    if(!matching_dictionary) {
        fprintf(stderr,"IOServiceMatching failed");
        return 1;
    }
    UInt32 value;
    CFNumberRef cfValue;
    value = kHIDPage_GenericDesktop;
    cfValue = CFNumberCreate( kCFAllocatorDefault, kCFNumberSInt32Type, &value );
    CFDictionarySetValue(matching_dictionary, CFSTR(kIOHIDDeviceUsagePageKey), cfValue);
    CFRelease(cfValue);
    value = kHIDUsage_GD_Keyboard;
    cfValue = CFNumberCreate( kCFAllocatorDefault, kCFNumberSInt32Type, &value );
    CFDictionarySetValue(matching_dictionary,CFSTR(kIOHIDDeviceUsageKey),cfValue);
    CFRelease(cfValue);
    io_iterator_t iter = IO_OBJECT_NULL;
    kern_return_t r = IOServiceGetMatchingServices(kIOMainPortDefault,
                                                   matching_dictionary,
                                                   &iter);
    if(r != KERN_SUCCESS) {
        fprintf(stderr,"IOServiceGetMatchingServices failed");
        return r;
    }
    for(mach_port_t curr = IOIteratorNext(iter); curr; curr = IOIteratorNext(iter)) {
        uint64_t registry_id;
        kern_return_t registry_result = IORegistryEntryGetRegistryEntryID(curr, &registry_id);
        CFStringRef product = (CFStringRef)IORegistryEntryCreateCFProperty(
            curr, CFSTR(kIOHIDProductKey), kCFAllocatorDefault, kIOHIDOptionsTypeNone);
        if (registry_result == KERN_SUCCESS) {
            printf("{\"registry_id\":%" PRIu64 ",\"product\":", registry_id);
            print_json_string(product);
            puts("}");
        }
        if (product) {
            CFRelease(product);
        }
        IOObjectRelease(curr);
    }
    IOObjectRelease(iter);
    return 0;
}
