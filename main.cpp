/* 
   ASUS DH Remote daemon
   2009-10, Tino Wagner <ich@tinowagner.com> 
 
   Useful Links:
		<http://www.cocoadev.com/index.pl?UsingTheAppleRemoteControl>
		<http://www.brandon-holland.com/irkeyboardemu.html>
 */
#include <iostream>
#include <time.h>
#include <sys/sysctl.h>

#include <CoreFoundation/CoreFoundation.h>
#include <ApplicationServices/ApplicationServices.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/IOCFPlugIn.h>
#include <IOKit/IOMessage.h>
#include <IOKit/hid/IOHIDLib.h>
#include <IOKit/hid/IOHIDKeys.h>
#include <IOKit/hid/IOHIDUsageTables.h>
#include <IOKit/hidsystem/IOHIDLib.h>
#include <IOKit/hidsystem/IOHIDShared.h>
#include <IOKit/hidsystem/IOHIDParameter.h>

// AsusRemote version
#define VERSION_STRING "v0.2"
#define AUTHOR_STRING "2009-10, (c) Tino Wagner <ich@tinowagner.com>"

#pragma mark ASUS Key Code Definitions

// Usage keys to identify the receiver.
const int PRIMARY_USAGE_KEY = 0;
const int PRIMARY_USAGE_PAGE_KEY = 0xff00;

/*
 * ASUS DH Remote
 * Key Codes
 */
#define KEY_CODE_RELEASE 0x00
#define KEY_CODE_POWER 0x01
#define KEY_CODE_QUICK_POWER 0x02
#define KEY_CODE_NOISE_OFF 0x03
#define KEY_CODE_WIFI 0x04
#define KEY_CODE_AP_LAUNCH 0x05
#define KEY_CODE_MAXIMIZE 0x06
#define KEY_CODE_PLUS 0x07
#define KEY_CODE_REV 0x08
#define KEY_CODE_PLAY_PAUSE 0x09
#define KEY_CODE_FWD 0x0a
#define KEY_CODE_MINUS 0x0b
#define TOTAL_KEY_CODES KEY_CODE_MINUS

/*
 * Emulate Apple Remote
 */
typedef enum {
	UpKey = 0,
	DownKey,
	MenuKey,
	PlayKey,
	RightKey,
	LeftKey,
	
	PressedUp, /* 6 */
	PressedDown,
	PressedMenu, /* Implies menu-held; no ReleasedMenu needed */
	PressedPlay, /* Not implemented! */
	PressedRight, /*  10 - fast forward */
	PressedLeft, /* 11 - rewind */
	
	ReleasedUp, /* 12 */
	ReleasedDown,
	ReleasedMenu, /* No-op; included for numerical consistency */
	ReleasedPlay,
	ReleasedRight,  /*  16 - fast forward */
	ReleasedLeft /* 17 - rewind */
} IRKeyboardKey;



#pragma mark Globals

// HID globals
static IONotificationPortRef notifyPort = 0;
static io_iterator_t addedIter = 0;

// Globals for evaluating the key presses
static UInt8 lastKeyCode = 0;
static bool lastPressIsLong = false;
static CFAbsoluteTime lastTime = 0;
// Depending on the delay between pressing and releasing a key, the daemon will
// do different operations. `keyRecognitionDelay' is the minimum time between
// pressing a key and deciding on the action.
const double keyRecogitionDelay = 0.25; 

// Started with the Plex option?
bool usePlex = false;


#pragma mark Declarations (Structures)

typedef struct HIDData
{
    io_object_t	notification;
    IOHIDDeviceInterface122 **hidDeviceInterface;
    IOHIDQueueInterface **hidQueueInterface;
    CFDictionaryRef hidElementDictionary;
    CFRunLoopSourceRef eventSource;
    UInt8  buffer[256];
} HIDData;

typedef HIDData * 		HIDDataRef;

typedef struct HIDElement {
    SInt32		currentValue;
    SInt32		usagePage;
    SInt32		usage;
    IOHIDElementType	type;
    IOHIDElementCookie	cookie;
    HIDDataRef          owner;
}HIDElement;

typedef HIDElement *HIDElementRef;

#pragma mark Declarations (Functions)
int Initialize();
void HIDDeviceAdded(void*, io_iterator_t);
void DeviceNotification(void*, io_service_t, natural_t, void *);
bool FindHIDElements(HIDDataRef);
void InterruptReportCallbackFunction(void*, IOReturn, void*, void*, uint32_t);

void RemoteKeyPressedCallback(CFRunLoopTimerRef timer, void *info);
void HandleKey(UInt8 code);
void HandleKeyPress(UInt8 code);
void HandleKeyRelease(UInt8 code);



#pragma mark main

/*
 * Program entry point.
 */
int main (const int argc, const char *argv[]) {
	printf("AsusRemote %s\n%s\n\n", VERSION_STRING, AUTHOR_STRING);
    
    if (argc > 1) {
        if (strcmp(argv[1], "-plex") == 0) {
            printf("* Emulating key presses for Plex *\n");
            usePlex = true;
        }
    }
	
    // Initialize HID.
	Initialize();
	
	// Main loop.
	CFRunLoopRun();
}



#pragma mark Key code processing

/*
 * Process HID Events of the remote.
 */
void InterruptReportCallbackFunction(void *target, IOReturn result,
									 void *refcon, void *sender,
									 uint32_t bufferSize)
{
    HIDDataRef hidDataRef = (HIDDataRef)refcon;
    
    if (!hidDataRef)
        return;
	
	/*
	 // Dump raw HID data
	 for (int index = 0; index < bufferSize; index++) {
	 printf("%2.2x ", hidDataRef->buffer[index]);
	 }
	 printf("\n");
	 */
	
	
	UInt8 remote_key_code = hidDataRef->buffer[1];
	
	// Call release function if code is 0x00.
	if (remote_key_code == 0x00) {
		CFGregorianUnits diff = CFAbsoluteTimeGetDifferenceAsGregorianUnits(
											CFAbsoluteTimeGetCurrent(), lastTime,
											0, kCFGregorianUnitsSeconds);
		double delta = diff.seconds;
		//printf("delta is %f s\n", delta);
		
		if (delta < 0.9*keyRecogitionDelay) {
			lastPressIsLong = false;
		} else {
			HandleKeyRelease(lastKeyCode);
		}
		
		return;
	}
	
	lastKeyCode = remote_key_code;
	lastPressIsLong = true;
	lastTime = CFAbsoluteTimeGetCurrent();
	
	// Create timer to find out if it was a short key press or a longer action.
	CFRunLoopTimerRef timer = CFRunLoopTimerCreate(0,
												   CFAbsoluteTimeGetCurrent()+keyRecogitionDelay,
												   0, 0, 0, RemoteKeyPressedCallback,
												   0);
	CFRunLoopAddTimer(CFRunLoopGetCurrent(), timer, kCFRunLoopDefaultMode);
	CFRelease(timer);
}

/*
 * Function to be called when the timer is fired.
 */
void RemoteKeyPressedCallback(CFRunLoopTimerRef timer, void *info) {
	if (lastPressIsLong) {
		// Key is pressed even after the timer was called.
		HandleKeyPress(lastKeyCode);
	}
	else {
		// Press and release within a short amount of time.
		HandleKey(lastKeyCode);
	}
}

/*
 * Returns a C-string representation of the key name corresponding to the key
 * code `code'.
 */
const char* GetKeyName(UInt8 code) {
	if (code < 1 || code > TOTAL_KEY_CODES) 
		return 0;
	
	static const char *key_names[] = 
	{0, "Power", "Quick Power", "Noise Off", "Wifi",
		"AP Launch", "Maximize", "Plus", "Reverse",
		"Play/Pause", "Forward", "Minus"};
	
	return key_names[code];
}

/*
 * Issue an Apple Remote command using IRKeyboardEmu's sysctl.
 */
void IssueAppleRemoteCommand(IRKeyboardKey key) {
	int ret = sysctlbyname("kern.sendIR", NULL, NULL, &key, sizeof(key));
	if (ret == -1) {
		printf("ERROR: Unable issuing sysctl command, errno = %i!\n", errno);
	}
}

/*
 * Handle short key presses.
 */
void HandleKey(UInt8 code) {
	printf("Key:          %s\n", GetKeyName(code));
	
    if (usePlex) {
        switch (code) {
            case KEY_CODE_PLUS:
                CGPostKeyboardEvent(0, (CGKeyCode)126, true);
                CGPostKeyboardEvent(0, (CGKeyCode)126, false);
                break;
            case KEY_CODE_MINUS:
                CGPostKeyboardEvent(0, (CGKeyCode)125, true);
                CGPostKeyboardEvent(0, (CGKeyCode)125, false);
                break;
            case KEY_CODE_REV:
                CGPostKeyboardEvent(0, (CGKeyCode)123, true);
                CGPostKeyboardEvent(0, (CGKeyCode)123, false);
                break;
            case KEY_CODE_FWD:
                CGPostKeyboardEvent(0, (CGKeyCode)124, true);
                CGPostKeyboardEvent(0, (CGKeyCode)124, false);
                break;
            case KEY_CODE_PLAY_PAUSE:
                CGPostKeyboardEvent(0, (CGKeyCode)36, true);
                CGPostKeyboardEvent(0, (CGKeyCode)36, false);
                break;
            case KEY_CODE_MAXIMIZE:
                CGPostKeyboardEvent(0, (CGKeyCode)53, true);
                CGPostKeyboardEvent(0, (CGKeyCode)53, false);
                break;
            case KEY_CODE_AP_LAUNCH:
                break;
            default:
                break;
        }
        return;
    }
    
	switch (code) {
		case KEY_CODE_PLUS:
			IssueAppleRemoteCommand(UpKey);
			break;
		case KEY_CODE_MINUS:
			IssueAppleRemoteCommand(DownKey);
			break;
		case KEY_CODE_REV:
			IssueAppleRemoteCommand(LeftKey);
			break;
		case KEY_CODE_FWD:
			IssueAppleRemoteCommand(RightKey);
			break;
		case KEY_CODE_PLAY_PAUSE:
			IssueAppleRemoteCommand(PlayKey);
			break;
		case KEY_CODE_AP_LAUNCH:
			IssueAppleRemoteCommand(MenuKey);
			break;
		default:
			break;
	}
}

/*
 * Handle initial key press on longer key presses.
 */
void HandleKeyPress(UInt8 code) {
	printf("Key pressed:  %s\n", GetKeyName(code));
    
    if (usePlex) {
        switch (code) {
            case KEY_CODE_PLUS:
                CGPostKeyboardEvent(0, (CGKeyCode)126, true);
                break;
            case KEY_CODE_MINUS:
                CGPostKeyboardEvent(0, (CGKeyCode)125, true);
                break;
            case KEY_CODE_REV:
                CGPostKeyboardEvent(0, (CGKeyCode)123, true);
                break;
            case KEY_CODE_FWD:
                CGPostKeyboardEvent(0, (CGKeyCode)124, true);
                break;
            case KEY_CODE_PLAY_PAUSE:
                CGPostKeyboardEvent(0, (CGKeyCode)36, true);
                break;
            case KEY_CODE_MAXIMIZE:
                CGPostKeyboardEvent(0, (CGKeyCode)53, true);
                break;
            case KEY_CODE_AP_LAUNCH:
                break;
            default:
                break;
        }
        return;
    }
	
	switch (code) {
		case KEY_CODE_PLUS:
			IssueAppleRemoteCommand(PressedUp);
			break;
		case KEY_CODE_MINUS:
			IssueAppleRemoteCommand(PressedDown);
			break;
		case KEY_CODE_REV:
			IssueAppleRemoteCommand(PressedLeft);
			break;
		case KEY_CODE_FWD:
			IssueAppleRemoteCommand(PressedRight);
			break;
		case KEY_CODE_PLAY_PAUSE:
			IssueAppleRemoteCommand(PlayKey);
			break;
		case KEY_CODE_AP_LAUNCH:
			IssueAppleRemoteCommand(PressedMenu);
			break;
		default:
			break;
	}
}

/*
 * Handle final key release on longer key presses.
 */
void HandleKeyRelease(UInt8 code) {
	printf("Key released: %s\n", GetKeyName(code));
    
    if (usePlex) {
        switch (code) {
            case KEY_CODE_PLUS:
                CGPostKeyboardEvent(0, (CGKeyCode)126, false);
                break;
            case KEY_CODE_MINUS:
                CGPostKeyboardEvent(0, (CGKeyCode)125, false);
                break;
            case KEY_CODE_REV:
                CGPostKeyboardEvent(0, (CGKeyCode)123, false);
                break;
            case KEY_CODE_FWD:
                CGPostKeyboardEvent(0, (CGKeyCode)124, false);
                break;
            case KEY_CODE_PLAY_PAUSE:
                CGPostKeyboardEvent(0, (CGKeyCode)36, false);
                break;
            case KEY_CODE_MAXIMIZE:
                CGPostKeyboardEvent(0, (CGKeyCode)53, false);
                break;
            case KEY_CODE_AP_LAUNCH:
                break;
            default:
                break;
        }
        return;
    }
	
	switch (code) {
		case KEY_CODE_PLUS:
			IssueAppleRemoteCommand(ReleasedUp);
			break;
		case KEY_CODE_MINUS:
			IssueAppleRemoteCommand(ReleasedDown);
			break;
		case KEY_CODE_REV:
			IssueAppleRemoteCommand(ReleasedLeft);
			break;
		case KEY_CODE_FWD:
			IssueAppleRemoteCommand(ReleasedRight);
			break;
		case KEY_CODE_PLAY_PAUSE:
			IssueAppleRemoteCommand(ReleasedPlay);
			break;
		/*case KEY_CODE_AP_LAUNCH:
			IssueAppleRemoteCommand(ReleasedMenu);
			break;*/
		default:
			break;
	}
}



#pragma mark HID

/*
 * Initializes HID Notifications for the ASUS Remote.
 */
int Initialize() {
	// Create Mach master port
	mach_port_t	masterPort;
	kern_return_t kr = IOMasterPort(bootstrap_port, &masterPort);
	if (kr || !masterPort)
		return 101;
	
	// Create notification port and add it to the run loop.
	notifyPort = IONotificationPortCreate(masterPort);
	CFRunLoopAddSource(CFRunLoopGetCurrent(),
					   IONotificationPortGetRunLoopSource(notifyPort),
					   kCFRunLoopDefaultMode);
	
	// Create IOKit notifications.
	CFMutableDictionaryRef matchingDict = IOServiceMatching(kIOHIDDeviceKey);
	
	if (!matchingDict)
		return 102;
	
	CFNumberRef	refUsageKey = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &PRIMARY_USAGE_KEY);
	CFNumberRef refUsagePageKey = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &PRIMARY_USAGE_PAGE_KEY);
	CFDictionarySetValue(matchingDict, CFSTR(kIOHIDPrimaryUsageKey), refUsageKey);
	CFDictionarySetValue(matchingDict, CFSTR(kIOHIDPrimaryUsagePageKey), refUsagePageKey);
	CFRelease(refUsageKey);
	CFRelease(refUsagePageKey);
	
	// Add the notification to be called, when a matching device is added.
	kr = IOServiceAddMatchingNotification(notifyPort,
										  kIOFirstMatchNotification,
										  matchingDict,
										  HIDDeviceAdded, 
										  0,
										  &addedIter);
	if (kr != kIOReturnSuccess)
        return 103;
	
    HIDDeviceAdded(0, addedIter);
	
	return 0;
}

/*
 * Callback for IOServiceAddMatchingNotification
 */
void HIDDeviceAdded(void *refCon, io_iterator_t iterator) {
	io_object_t hidDevice = 0;
	IOCFPlugInInterface **plugInInterface = 0;
	IOHIDDeviceInterface122 **hidDeviceInterface = 0;
	HRESULT result = S_FALSE;
	HIDDataRef hidDataRef = 0;
	IOReturn kr;
	SInt32 score;
	bool pass;
	
	// Iterate through all matching devices.
	while (hidDevice = IOIteratorNext(iterator)) {
		kr = IOCreatePlugInInterfaceForService(hidDevice, kIOHIDDeviceUserClientTypeID, 
											   kIOCFPlugInInterfaceID, &plugInInterface, &score);
		if (kr != kIOReturnSuccess) {
			IOObjectRelease(hidDevice);
			continue;
		}
		
		result = (*plugInInterface)->QueryInterface(plugInInterface, CFUUIDGetUUIDBytes(kIOHIDDeviceInterfaceID122), 
													(LPVOID*)&hidDeviceInterface);
		
		if ((result == S_OK) && hidDeviceInterface) {
			hidDataRef = (HIDDataRef)malloc(sizeof(HIDData));
			bzero(hidDataRef, sizeof(HIDData));
			hidDataRef->hidDeviceInterface = hidDeviceInterface;
			
			/* Open the device. */
            result = (*(hidDataRef->hidDeviceInterface))->open (hidDataRef->hidDeviceInterface, 0);
			
            /* Find the HID elements for this device */
            pass = FindHIDElements(hidDataRef);
			
            /* Create an asynchronous event source for this device. */
            result = (*(hidDataRef->hidDeviceInterface))->createAsyncEventSource(hidDataRef->hidDeviceInterface, &hidDataRef->eventSource);
			
            /* Set the handler to call when the device sends a report. */
            result = (*(hidDataRef->hidDeviceInterface))->setInterruptReportHandlerCallback(hidDataRef->hidDeviceInterface, hidDataRef->buffer, sizeof(hidDataRef->buffer), &InterruptReportCallbackFunction, NULL, hidDataRef);
			
            /* Add the asynchronous event source to the run loop. */
            CFRunLoopAddSource(CFRunLoopGetCurrent(), hidDataRef->eventSource, kCFRunLoopDefaultMode);
			
			/* Register an interest in finding out anything that happens with this device (disconnection, for example) */
            IOServiceAddInterestNotification(	
											 notifyPort,				// notifyPort
											 hidDevice,					// service
											 kIOGeneralInterest,		// interestType
											 DeviceNotification,		// callback
											 hidDataRef,				// refCon
											 &(hidDataRef->notification)// notification
											 );
		}
		
        // Clean up
        (*plugInInterface)->Release(plugInInterface);
		IOObjectRelease(hidDevice);
	}
}

//---------------------------------------------------------------------------
// DeviceNotification
//
// This routine will get called whenever any kIOGeneralInterest notification 
// happens. 
//---------------------------------------------------------------------------
void DeviceNotification(void *			refCon,
                        io_service_t 	service,
                        natural_t		messageType,
                        void *			messageArgument )
{
    kern_return_t	kr;
    HIDDataRef		hidDataRef = (HIDDataRef) refCon;
	
    /* Check to see if a device went away and clean up. */
    if ( (hidDataRef != NULL) &&
		(messageType == kIOMessageServiceIsTerminated) )
    {
        if (hidDataRef->hidQueueInterface != NULL)
        {
            kr = (*(hidDataRef->hidQueueInterface))->stop((hidDataRef->hidQueueInterface));
            kr = (*(hidDataRef->hidQueueInterface))->dispose((hidDataRef->hidQueueInterface));
            kr = (*(hidDataRef->hidQueueInterface))->Release (hidDataRef->hidQueueInterface);
            hidDataRef->hidQueueInterface = NULL;
        }
		
        if (hidDataRef->hidDeviceInterface != NULL)
        {
            kr = (*(hidDataRef->hidDeviceInterface))->close (hidDataRef->hidDeviceInterface);
            kr = (*(hidDataRef->hidDeviceInterface))->Release (hidDataRef->hidDeviceInterface);
            hidDataRef->hidDeviceInterface = NULL;
        }
        
        if (hidDataRef->notification)
        {
            kr = IOObjectRelease(hidDataRef->notification);
            hidDataRef->notification = 0;
        }
		
    }
}

bool FindHIDElements(HIDDataRef hidDataRef)
{
    CFArrayRef              elementArray	= NULL;
    CFMutableDictionaryRef  hidElements     = NULL;
    CFMutableDataRef        newData         = NULL;
    CFNumberRef             number			= NULL;
    CFDictionaryRef         element			= NULL;
    HIDElement              newElement;
    IOReturn                ret				= kIOReturnError;
    unsigned                i;
	
    if (!hidDataRef)
        return false;
	
    /* Create a mutable dictionary to hold HID elements. */
    hidElements = CFDictionaryCreateMutable(
											kCFAllocatorDefault, 
											0, 
											&kCFTypeDictionaryKeyCallBacks, 
											&kCFTypeDictionaryValueCallBacks);                                    
    if ( !hidElements )
        return false;
	
    // Let's find the elements
    ret = (*hidDataRef->hidDeviceInterface)->copyMatchingElements(	
																  hidDataRef->hidDeviceInterface, 
																  NULL, 
																  &elementArray);
	
	
    if ( (ret != kIOReturnSuccess) || !elementArray)
        goto FIND_ELEMENT_CLEANUP;
	
    //CFShow(elementArray);
	
    /* Iterate through the elements and read their values. */
    for (i=0; i<CFArrayGetCount(elementArray); i++)
    {
        element = (CFDictionaryRef) CFArrayGetValueAtIndex(elementArray, i);
        if ( !element )
            continue;
		
        bzero(&newElement, sizeof(HIDElement));
        
        newElement.owner = hidDataRef;
        
		/* Read the element's usage page (top level category describing the type of
		 element---kHIDPage_GenericDesktop, for example) */
        number = (CFNumberRef)CFDictionaryGetValue(element, CFSTR(kIOHIDElementUsagePageKey));
        if ( !number ) continue;
        CFNumberGetValue(number, kCFNumberSInt32Type, &newElement.usagePage );
		
		/* Read the element's usage (second level category describing the type of
		 element---kHIDUsage_GD_Keyboard, for example) */
        number = (CFNumberRef)CFDictionaryGetValue(element, CFSTR(kIOHIDElementUsageKey));
        if ( !number ) continue;
        CFNumberGetValue(number, kCFNumberSInt32Type, &newElement.usage );
        
		/* Read the cookie (unique identifier) for the element */
        number = (CFNumberRef)CFDictionaryGetValue(element, CFSTR(kIOHIDElementCookieKey));
        if ( !number ) continue;
        CFNumberGetValue(number, kCFNumberIntType, &(newElement.cookie) );
        
		/* Determine what type of element this is---button, Axis, etc. */
        number = (CFNumberRef)CFDictionaryGetValue(element, CFSTR(kIOHIDElementTypeKey));
        if ( !number ) continue;
        CFNumberGetValue(number, kCFNumberIntType, &(newElement.type) );
		
		/* Pay attention to X/Y coordinates of a pointing device and
		 the first mouse button.  For other elements, go on to the
		 next element. */
        if ( newElement.usagePage == kHIDPage_GenericDesktop )
        {
            switch ( newElement.usage )
            {
                case kHIDUsage_GD_X:
                case kHIDUsage_GD_Y:
                    break;
                default:
                    continue;
            }
        }
        else if ( newElement.usagePage == kHIDPage_Button )
        {
			
            switch ( newElement.usage )
            {
                case kHIDUsage_Button_1:
                    break;
                default:
                    continue;
            }
        }
        else
            continue;
		
		/* Add this element to the hidElements dictionary. */
        newData = CFDataCreateMutable(kCFAllocatorDefault, sizeof(HIDElement));
        if ( !newData ) continue;
        bcopy(&newElement, CFDataGetMutableBytePtr(newData), sizeof(HIDElement));
		
        number = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &newElement.cookie);        
        if ( !number )  continue;
        CFDictionarySetValue(hidElements, number, newData);
        CFRelease(number);
        CFRelease(newData);
    }
    
FIND_ELEMENT_CLEANUP:
    if ( elementArray ) CFRelease(elementArray);
    
    if (CFDictionaryGetCount(hidElements) == 0)
    {
        CFRelease(hidElements);
        hidElements = NULL;
    }
    else 
    {
        hidDataRef->hidElementDictionary = hidElements;
    }
    
    return hidDataRef->hidElementDictionary;
}
