#import <Cocoa/Cocoa.h>
#include <SDL3/SDL.h>
#include <stdint.h>

// The surface remains owned by the caller; encoding copies its pixels.
NSData *ISCScreenshotPNG(SDL_Surface *surface, NSError **error);
BOOL ISCCopyScreenshot(NSData *png, NSPasteboard *pasteboard, NSError **error);
BOOL ISCSaveScreenshot(NSData *png, NSString *directory, int64_t unix_ms,
                       NSString **savedPath, NSError **error);
