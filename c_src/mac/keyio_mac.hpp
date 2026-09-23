#include <IOKit/hid/IOHIDLib.h>
#include <IOKit/hidsystem/IOHIDShared.h>
#include <unistd.h>
#include <errno.h>
#include <thread>
#include <map>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <mach/mach_error.h>
#include <AvailabilityMacros.h>

/* The name was changed from "Master" to "Main" in Apple SDK 12.0 (Monterey) */
/* MAC_OS_X_VERSION_12_0 does not exist :( */
#if MAC_OS_X_VERSION_MAX_ALLOWED < 120000
    #define kIOMainPortDefault kIOMasterPortDefault
#endif

int init_sink(void);
int exit_sink(void);

/*
 * Key event information that's shared between C++ and Haskell.
 *
 * type: represents key up or key down
 * page: represents IOKit usage page
 * usage: represents IOKit usage
 */
struct KeyEvent {
    uint64_t type;
    uint32_t page;
    uint32_t usage;
};

/*
 * These are needed to receive unaltered key events from the OS.
 */
static std::thread thread;
static CFRunLoopRef listener_loop;
static std::map<io_service_t,IOHIDDeviceRef> source_device;
static int fd[2];

struct DeviceSelector {
    char *product;
    bool use_registry_id;
    uint64_t registry_id;
};

static DeviceSelector selector = {nullptr, false, 0};

void print_iokit_error(const char *fname, int freturn = 0) {
    std::cerr << fname << " error";
    if(freturn) {
        //std::cerr << " " << std::hex << freturn;
        std::cerr << ": ";
        std::cerr << mach_error_string(freturn);
    }
    std::cerr << std::endl;
}

/*
 * We'll register this callback to run whenever an IOHIDDevice
 * (representing a keyboard) sends input from the user.
 *
 * It passes the relevant information into a pipe that will be read
 * from with wait_key.
 */
void input_callback(void *context, IOReturn result, void *sender, IOHIDValueRef value) {
    struct KeyEvent e;
    IOHIDElementRef element = IOHIDValueGetElement(value);
    e.type = IOHIDValueGetIntegerValue(value);
    e.page = IOHIDElementGetUsagePage(element);
    e.usage = IOHIDElementGetUsage(element);
    write(fd[1], &e, sizeof(struct KeyEvent));
}

bool matches_selector(io_service_t device, CFStringRef product, DeviceSelector *selector) {
    if (selector->use_registry_id) {
        uint64_t registry_id;
        kern_return_t kr = IORegistryEntryGetRegistryEntryID(device, &registry_id);
        if (kr != KERN_SUCCESS) {
            print_iokit_error("IORegistryEntryGetRegistryEntryID", kr);
            return false;
        }
        return registry_id == selector->registry_id;
    }

    if (!selector->product) {
        return true;
    }

    CFStringRef requested = CFStringCreateWithCString(
        kCFAllocatorDefault, selector->product, CFStringGetSystemEncoding());
    if (!requested) {
        print_iokit_error("CFStringCreateWithCString");
        return false;
    }
    bool matches = CFStringCompare(product, requested, 0) == kCFCompareEqualTo;
    CFRelease(requested);
    return matches;
}

void open_matching_devices(DeviceSelector *selector, io_iterator_t iter) {
    kern_return_t kr;
    CFStringRef cfkarabiner = CFStringCreateWithCString(kCFAllocatorDefault, "Karabiner ", CFStringGetSystemEncoding());
    if(cfkarabiner == NULL) {
        print_iokit_error("CFStringCreateWithCString");
        return;
    }
    for(mach_port_t curr = IOIteratorNext(iter); curr; curr = IOIteratorNext(iter)) {
        CFStringRef cfcurr = (CFStringRef)IORegistryEntryCreateCFProperty(curr, CFSTR(kIOHIDProductKey), kCFAllocatorDefault, kIOHIDOptionsTypeNone);
        if(cfcurr == NULL) {
            print_iokit_error("IORegistryEntryCreateCFProperty");
            cfcurr = CFStringCreateWithCString (NULL, "Unknown External Keyboard", kCFStringEncodingUTF8);
        }

        // any device named "Karabiner ..." should be ignored
        bool match = !CFStringHasPrefix(cfcurr, cfkarabiner)
            && matches_selector(curr, cfcurr, selector);
        CFRelease(cfcurr);
        if(!match) {
            IOObjectRelease(curr);
            continue;
        }
        IOHIDDeviceRef dev = IOHIDDeviceCreate(kCFAllocatorDefault, curr);
        IOHIDDeviceRegisterInputValueCallback(dev, input_callback, NULL);
        kr = IOHIDDeviceOpen(dev, kIOHIDOptionsTypeSeizeDevice);
        if(kr != kIOReturnSuccess) {
            print_iokit_error("IOHIDDeviceOpen", kr);
            CFRelease(dev);
            IOObjectRelease(curr);
            continue;
        }
        source_device[curr] = dev;
        IOHIDDeviceScheduleWithRunLoop(dev, listener_loop, kCFRunLoopDefaultMode);
        IOObjectRelease(curr);
    }
    CFRelease(cfkarabiner);
}

/*
 * We'll register this callback to run whenever an IOHIDDevice
 * (representing a keyboard) is connected to the OS
 *
 */
void matched_callback(void *context, io_iterator_t iter) {
    open_matching_devices((DeviceSelector *)context, iter);
}

/*
 * We'll register this callback to run whenever an IOHIDDevice
 * (representing a keyboard) is disconnected from the OS
 *
 */
void terminated_callback(void *context, io_iterator_t iter) {
    for(mach_port_t curr = IOIteratorNext(iter); curr; curr = IOIteratorNext(iter)) {
        auto it = source_device.find(curr);
        if (it != source_device.end()) {
            IOHIDDeviceClose(it->second, kIOHIDOptionsTypeSeizeDevice);
            CFRelease(it->second);
            source_device.erase(it);
        }
        IOObjectRelease(curr);
    }
}

/*
 * Reads a new key event from the pipe, blocking until a new event is
 * ready.
 */
extern "C" int wait_key(struct KeyEvent *e) {
    return read(fd[0], e, sizeof(struct KeyEvent)) == sizeof(struct KeyEvent);
}

/*
 * For each keyboard, registers an asynchronous callback to run when
 * new input from the user is available from that keyboard. Then
 * sleeps indefinitely, ready to received asynchronous callbacks.
 */
void monitor_kb(DeviceSelector *selector) {
    kern_return_t kr;
    CFMutableDictionaryRef matching_dictionary = IOServiceMatching(kIOHIDDeviceKey);
    if(!matching_dictionary) {
        print_iokit_error("IOServiceMatching");
        return;
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
    listener_loop = CFRunLoopGetCurrent();
    IONotificationPortRef notification_port = IONotificationPortCreate(kIOMainPortDefault);
    if (!notification_port) {
        print_iokit_error("IONotificationPortCreate");
        CFRelease(matching_dictionary);
        return;
    }
    CFRunLoopSourceRef notification_source = IONotificationPortGetRunLoopSource(notification_port);
    CFRunLoopAddSource(listener_loop, notification_source, kCFRunLoopDefaultMode);

    io_iterator_t iter = IO_OBJECT_NULL;
    CFRetain(matching_dictionary);
    kr = IOServiceAddMatchingNotification(notification_port,
                                          kIOMatchedNotification,
                                          matching_dictionary,
                                          matched_callback,
                                          selector,
                                          &iter);
    if(kr != KERN_SUCCESS) {
        print_iokit_error("IOServiceAddMatchingNotification", kr);
        IONotificationPortDestroy(notification_port);
        CFRelease(matching_dictionary);
        return;
    }
    matched_callback(selector, iter);

    CFRetain(matching_dictionary);
    kr = IOServiceAddMatchingNotification(notification_port,
                                          kIOTerminatedNotification,
                                          matching_dictionary,
                                          terminated_callback,
                                          NULL,
                                          &iter);
    if(kr != KERN_SUCCESS) {
        print_iokit_error("IOServiceAddMatchingNotification", kr);
        IONotificationPortDestroy(notification_port);
        CFRelease(matching_dictionary);
        return;
    }
    terminated_callback(NULL, iter);
    CFRelease(matching_dictionary);
    CFRunLoopRun();
    for(std::pair<const io_service_t,IOHIDDeviceRef> p: source_device) {
        kr = IOHIDDeviceClose(p.second,kIOHIDOptionsTypeSeizeDevice);
        if(kr != KERN_SUCCESS) {
            print_iokit_error("IOHIDDeviceClose", kr);
        }
        CFRelease(p.second);
    }
    source_device.clear();
    IONotificationPortDestroy(notification_port);
}

/*
 * Opens and seizes input from each keyboard device whose product name
 * matches the parameter (if NULL is received, then it opens all
 * keyboard devices). Spawns a thread to receive asynchronous input
 * and opens a pipe for this thread to send key event data to the main
 * thread.
 *
 * Loads a karabiner kernel extension that will send key events
 * back to the OS.
 */
extern "C" int grab_kb(char *product, uint64_t registry_id, uint8_t use_registry_id) {
    // Source
    if (pipe(fd) == -1) {
        std::cerr << "pipe error: " << errno << std::endl;
        return errno;
    }
    if(product && !use_registry_id) {
        selector.product = strdup(product);
    }
    selector.use_registry_id = use_registry_id;
    selector.registry_id = registry_id;
    thread = std::thread{monitor_kb, &selector};
    // Sink
    return init_sink();
}

/*
 * Releases the resources needed to receive key events from and send
 * key events to the OS.
 */
extern "C" int release_kb() {
    int retval = 0;
    kern_return_t kr;
    // Source
    if(thread.joinable() && listener_loop) {
        CFRunLoopStop(listener_loop);
        thread.join();
    } else {
        std::cerr << "No thread was running!" << std::endl;
    }
    if(selector.product) {
        free(selector.product);
        selector.product = nullptr;
    }
    if (close(fd[0]) == -1) {
        std::cerr << "close error: " << errno << std::endl;
        retval = 1;
    }
    if (close(fd[1]) == -1) {
        std::cerr << "close error: " << errno << std::endl;
        retval = 1;
    }
    // Sink
    if(exit_sink()) retval = 1;
    return retval;
}
