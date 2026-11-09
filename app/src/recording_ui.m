#import <Cocoa/Cocoa.h>
#include <libavutil/time.h>
#include "recording_ui.h"
#include "screen.h"
#import "screenshot.h"

static NSUserDefaults *ISCRecordingDefaults(void) {
    static NSUserDefaults *defaults;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        defaults = [[NSUserDefaults alloc] initWithSuiteName:@"com.technicallyinvalid.InvalidScreenCopy"];
    });
    return defaults;
}

// Keep clicks in the recording row out of the window's title-bar drag handling.
@interface InvalidRecordingRow : NSView
@end

@implementation InvalidRecordingRow
- (BOOL)mouseDownCanMoveWindow {
    return NO;
}
@end

@interface InvalidRecordingUI : NSObject
@property(nonatomic, assign) struct sc_recording *recording;
@property(nonatomic, assign) struct sc_controller *controller;
@property(nonatomic, assign) struct sc_screen *screen;
@property(nonatomic, strong) NSWindow *window;
@property(nonatomic, strong) NSTitlebarAccessoryViewController *accessory;
@property(nonatomic, strong) NSButton *recordButton;
@property(nonatomic, strong) NSButton *stopButton;
@property(nonatomic, strong) NSButton *openFolderButton;
@property(nonatomic, strong) NSButton *screenshotCopyButton;
@property(nonatomic, strong) NSButton *saveScreenshotButton;
@property(nonatomic, strong) NSTextField *status;
@property(nonatomic, strong) NSMenuItem *menuItem;
@property(nonatomic, strong) NSMenuItem *startItem;
@property(nonatomic, strong) NSMenuItem *stopItem;
@property(nonatomic, strong) NSMenuItem *devicesItem;
@property(nonatomic, strong) NSString *screenshotStatus;
@property(nonatomic, assign) NSTimeInterval screenshotStatusUntil;
@property(nonatomic, strong) NSTimer *timer;
@property(nonatomic, strong) NSString *launchDirectory;
@property(nonatomic, strong) NSString *directory;
@property(nonatomic, strong) NSOpenPanel *folderPanel;
@property(nonatomic, strong) NSFileHandle *launcherInput;
- (void)refresh;
- (void)start:(id)sender;
- (void)stop:(id)sender;
- (void)chooseFolder:(id)sender;
- (void)openFolder:(id)sender;
- (void)showDevices:(id)sender;
- (void)useLaunchFolder:(id)sender;
- (void)dispose;
- (void)copyScreenshot:(id)sender;
- (void)saveScreenshot:(id)sender;
@end

@implementation InvalidRecordingUI
- (void)refresh {
    bool ready, active;
    char message[1024];
    sc_recording_poll(self.recording, &ready, &active, message, sizeof(message));
    self.recordButton.enabled = ready && !active;
    self.stopButton.enabled = active;
    self.startItem.enabled = ready && !active;
    self.stopItem.enabled = active;
    BOOL canCapture = sc_screen_can_capture(self.screen) && !self.window.attachedSheet;
    self.screenshotCopyButton.enabled = canCapture;
    self.saveScreenshotButton.enabled = canCapture;
    NSString *detail = [NSString stringWithUTF8String:message] ?: @"";
    NSString *fullDetail = detail;
    if (ready && !active && [detail hasPrefix:@"Waiting for the video"]) {
        detail = @"Ready to record";
    }
    if ([detail hasPrefix:@"Saved:"] || [detail hasPrefix:@"Recording:"]) {
        detail = [[detail hasPrefix:@"Saved:"] ? @"Saved: " : @"Recording: "
            stringByAppendingString:detail.lastPathComponent];
    }
    if (NSDate.timeIntervalSinceReferenceDate < self.screenshotStatusUntil) {
        detail = active ? [@"Recording · " stringByAppendingString:self.screenshotStatus]
                        : self.screenshotStatus;
        fullDetail = [detail stringByAppendingFormat:@"\n%@", fullDetail];
    }
    self.status.stringValue = detail;
    self.status.toolTip = [fullDetail stringByAppendingFormat:@"\nFolder: %@", self.directory];
    self.status.textColor = active ? NSColor.systemRedColor : NSColor.secondaryLabelColor;
    self.recordButton.toolTip = [@"Save a new MP4 in " stringByAppendingString:self.directory];
    self.openFolderButton.toolTip = [@"Open in Finder: " stringByAppendingString:self.directory];
    self.saveScreenshotButton.toolTip = [@"Save a PNG in " stringByAppendingString:self.directory];
}
- (void)screenshotMessage:(NSString *)message {
    self.screenshotStatus = message;
    self.screenshotStatusUntil = NSDate.timeIntervalSinceReferenceDate + 5;
    [self refresh];
}
- (void)screenshotError:(NSError *)error {
    NSAlert *alert = [NSAlert new];
    alert.messageText = @"Could not capture screenshot";
    alert.informativeText = error.localizedDescription ?: @"Please try again when the phone image is visible.";
    [alert beginSheetModalForWindow:self.window completionHandler:nil];
    [self refresh];
}
- (NSData *)screenshotPNG {
    SDL_Surface *surface = sc_screen_capture(self.screen);
    if (!surface) {
        [self screenshotError:[NSError errorWithDomain:@"InvalidScreenCopy.Screenshot" code:1
            userInfo:@{NSLocalizedDescriptionKey: [NSString stringWithUTF8String:SDL_GetError()]}]];
        return nil;
    }
    NSError *error = nil;
    NSData *png = ISCScreenshotPNG(surface, &error);
    SDL_DestroySurface(surface);
    if (!png) [self screenshotError:error];
    return png;
}
- (void)copyScreenshot:(id)sender {
    (void)sender;
    if (self.window.attachedSheet) return;
    NSData *png = [self screenshotPNG];
    if (!png) return;
    NSError *error = nil;
    if (ISCCopyScreenshot(png, NSPasteboard.generalPasteboard, &error))
        [self screenshotMessage:@"Screenshot copied"];
    else [self screenshotError:error];
}
- (void)saveScreenshot:(id)sender {
    (void)sender;
    if (self.window.attachedSheet) return;
    int64_t unix_ms = av_gettime() / 1000;
    NSData *png = [self screenshotPNG];
    if (!png) return;
    NSError *error = nil;
    NSString *path = nil;
    if (ISCSaveScreenshot(png, self.directory, unix_ms, &path, &error))
        [self screenshotMessage:[@"Saved screenshot: " stringByAppendingString:path.lastPathComponent]];
    else [self screenshotError:error];
}
- (void)start:(id)sender {
    (void)sender;
    if (sc_recording_start(self.recording, self.directory.fileSystemRepresentation,
                           av_gettime() / 1000)) {
        if (self.controller) {
            // Wake an idle phone without toggling an already active display.
            struct sc_control_msg wake = {.type = SC_CONTROL_MSG_TYPE_INJECT_KEYCODE};
            wake.inject_keycode.keycode = AKEYCODE_WAKEUP;
            wake.inject_keycode.action = AKEY_EVENT_ACTION_DOWN;
            sc_controller_push_msg(self.controller, &wake);
            wake.inject_keycode.action = AKEY_EVENT_ACTION_UP;
            sc_controller_push_msg(self.controller, &wake);
            struct sc_control_msg message = {.type = SC_CONTROL_MSG_TYPE_REQUEST_KEYFRAME};
            sc_controller_push_msg(self.controller, &message);
        }
    }
    [self refresh];
}
- (void)stop:(id)sender {
    (void)sender;
    self.status.stringValue = @"Finishing recording…";
    sc_recording_stop(self.recording);
    [self refresh];
}
- (void)chooseFolder:(id)sender {
    (void)sender;
    if (self.folderPanel) {
        return;
    }
    NSOpenPanel *panel = [NSOpenPanel openPanel];
    self.folderPanel = panel;
    panel.title = @"Choose Recording Folder";
    panel.prompt = @"Use Folder";
    panel.canChooseDirectories = YES;
    panel.canChooseFiles = NO;
    panel.canCreateDirectories = YES;
    panel.allowsMultipleSelection = NO;
    panel.directoryURL = [NSURL fileURLWithPath:self.directory isDirectory:YES];
    __weak InvalidRecordingUI *weakSelf = self;
    [panel beginSheetModalForWindow:self.window completionHandler:^(NSModalResponse response) {
        InvalidRecordingUI *strongSelf = weakSelf;
        if (!strongSelf) return;
        if (response == NSModalResponseOK && panel.URL) {
            strongSelf.directory = panel.URL.path;
            [ISCRecordingDefaults() setObject:strongSelf.directory
                forKey:@"InvalidScreenCopyRecordingDirectory"];
        }
        strongSelf.folderPanel = nil;
        [strongSelf refresh];
    }];
    [self refresh];
}
- (void)useLaunchFolder:(id)sender {
    (void)sender;
    self.directory = self.launchDirectory;
    [ISCRecordingDefaults() removeObjectForKey:@"InvalidScreenCopyRecordingDirectory"];
    [self refresh];
}
- (void)openFolder:(id)sender {
    (void)sender;
    BOOL isDirectory = NO;
    BOOL exists = [NSFileManager.defaultManager fileExistsAtPath:self.directory
                                                   isDirectory:&isDirectory];
    NSURL *url = [NSURL fileURLWithPath:self.directory isDirectory:YES];
    if (exists && isDirectory && [NSWorkspace.sharedWorkspace openURL:url]) {
        return;
    }
    NSAlert *alert = [[NSAlert alloc] init];
    alert.messageText = @"Could not open recording folder";
    alert.informativeText = [NSString stringWithFormat:
        @"Check that this folder still exists and is accessible:\n%@", self.directory];
    [alert beginSheetModalForWindow:self.window completionHandler:nil];
}
- (void)showDevices:(id)sender {
    (void)sender;
    pid_t pid = [NSProcessInfo.processInfo.environment[@"ISC_LAUNCHER_PID"] intValue];
    if (pid <= 0) return;
    NSRunningApplication *launcher = [NSRunningApplication runningApplicationWithProcessIdentifier:pid];
    if (!launcher || launcher.terminated) return;
    // Reopening the bundle synchronously can block the SDL event loop inside
    // LaunchServices. Activate our already-running parent by process ID instead.
    if (@available(macOS 14.0, *)) {
        [NSApp yieldActivationToApplication:launcher];
        [launcher activateFromApplication:NSRunningApplication.currentApplication options:NSApplicationActivateAllWindows];
    } else [launcher activateWithOptions:NSApplicationActivateAllWindows];
}
- (void)dispose {
    [self.timer invalidate];
    self.timer = nil;
    [self.folderPanel cancel:nil];
    self.launcherInput.readabilityHandler = nil;
    [NSApp.mainMenu removeItem:self.menuItem];
    [self.devicesItem.menu removeItem:self.devicesItem];
    NSUInteger index = [self.window.titlebarAccessoryViewControllers indexOfObject:self.accessory];
    if (index != NSNotFound) {
        [self.window removeTitlebarAccessoryViewControllerAtIndex:index];
    }
}
@end

void *
sc_recording_ui_create(struct sc_screen *screen, struct sc_recording *recording,
                        struct sc_controller *controller) {
    @autoreleasepool {
        NSWindow *nativeWindow = (__bridge NSWindow *) SDL_GetPointerProperty(
            SDL_GetWindowProperties(screen->window), SDL_PROP_WINDOW_COCOA_WINDOW_POINTER, NULL);
        if (!nativeWindow) return NULL;
        InvalidRecordingUI *ui = [[InvalidRecordingUI alloc] init];
        ui.recording = recording;
        ui.controller = controller;
        ui.screen = screen;
        ui.window = nativeWindow;
        nativeWindow.titleVisibility = NSWindowTitleHidden;
        NSString *launchDirectory = NSProcessInfo.processInfo.environment[@"ISC_RECORDING_LAUNCH_DIRECTORY"];
        ui.launchDirectory = launchDirectory.isAbsolutePath ? launchDirectory
            : NSFileManager.defaultManager.currentDirectoryPath;
        // Finder launches bundles with cwd '/'; use the app's containing folder.
        if ([ui.launchDirectory isEqualToString:@"/"]
                && [NSBundle.mainBundle.bundlePath hasSuffix:@".app"]) {
            ui.launchDirectory = NSBundle.mainBundle.bundlePath.stringByDeletingLastPathComponent;
        }
        ui.directory = [ISCRecordingDefaults() stringForKey:@"InvalidScreenCopyRecordingDirectory"]
            ?: ui.launchDirectory;
        ui.accessory = [[NSTitlebarAccessoryViewController alloc] init];
        ui.accessory.layoutAttribute = NSLayoutAttributeBottom;
        // Two button rows keep all controls usable in a narrow phone window.
        NSSize minimumSize = nativeWindow.minSize;
        minimumSize.width = MAX(minimumSize.width, 502);
        nativeWindow.minSize = minimumSize;
        NSRect windowFrame = nativeWindow.frame;
        if (windowFrame.size.width < minimumSize.width) {
            windowFrame.size.width = minimumSize.width;
            [nativeWindow setFrame:windowFrame display:YES];
        }
        CGFloat rowWidth = nativeWindow.contentView.bounds.size.width;
        NSView *view = [[InvalidRecordingRow alloc] initWithFrame:NSMakeRect(0, 0, rowWidth, 98)];
        [view.heightAnchor constraintEqualToConstant:98].active = YES;
        ui.accessory.fullScreenMinHeight = 98;
        ui.recordButton = [NSButton buttonWithTitle:@"● Record" target:ui action:@selector(start:)];
        ui.recordButton.frame = NSMakeRect(12, 36, 90, 28);
        ui.recordButton.accessibilityLabel = @"Record";
        ui.stopButton = [NSButton buttonWithTitle:@"■ Stop Recording" target:ui action:@selector(stop:)];
        ui.stopButton.frame = NSMakeRect(108, 36, 142, 28);
        ui.stopButton.accessibilityLabel = @"Stop Recording";
        NSButton *chooseFolder = [NSButton buttonWithTitle:@"Choose Folder" target:ui action:@selector(chooseFolder:)];
        chooseFolder.frame = NSMakeRect(258, 36, 116, 28);
        chooseFolder.toolTip = @"Choose where videos and screenshots are saved";
        ui.openFolderButton = [NSButton buttonWithTitle:@"Open Folder" target:ui action:@selector(openFolder:)];
        ui.openFolderButton.frame = NSMakeRect(382, 36, 108, 28);
        ui.screenshotCopyButton = [NSButton buttonWithTitle:@"Copy Screenshot" target:ui action:@selector(copyScreenshot:)];
        ui.screenshotCopyButton.frame = NSMakeRect(12, 4, 156, 28);
        ui.screenshotCopyButton.toolTip = @"Copy the displayed phone image to the clipboard";
        ui.saveScreenshotButton = [NSButton buttonWithTitle:@"Save Screenshot" target:ui action:@selector(saveScreenshot:)];
        ui.saveScreenshotButton.frame = NSMakeRect(176, 4, 156, 28);
        ui.status = [NSTextField labelWithString:@"Waiting for the video stream…"];
        // Symmetric insets center status text while leaving native controls clear.
        ui.status.frame = NSMakeRect(80, 70, rowWidth - 160, 20);
        ui.status.autoresizingMask = NSViewWidthSizable;
        ui.status.alignment = NSTextAlignmentCenter;
        ui.status.lineBreakMode = NSLineBreakByTruncatingMiddle;
        ui.status.font = [NSFont systemFontOfSize:11];
        [view addSubview:ui.recordButton];
        [view addSubview:ui.stopButton];
        [view addSubview:chooseFolder];
        [view addSubview:ui.openFolderButton];
        [view addSubview:ui.screenshotCopyButton];
        [view addSubview:ui.saveScreenshotButton];
        [view addSubview:ui.status];
        ui.accessory.view = view;
        [nativeWindow addTitlebarAccessoryViewController:ui.accessory];

        NSMenu *menu = [[NSMenu alloc] initWithTitle:@"Recording"];
        menu.autoenablesItems = NO;
        ui.startItem = [menu addItemWithTitle:@"Start Recording" action:@selector(start:) keyEquivalent:@"r"];
        ui.startItem.target = ui;
        ui.stopItem = [menu addItemWithTitle:@"Stop Recording" action:@selector(stop:) keyEquivalent:@"r"];
        ui.stopItem.keyEquivalentModifierMask = NSEventModifierFlagCommand | NSEventModifierFlagShift;
        ui.stopItem.target = ui;
        [menu addItem:NSMenuItem.separatorItem];
        NSMenuItem *choose = [menu addItemWithTitle:@"Choose Recording Folder…" action:@selector(chooseFolder:) keyEquivalent:@""];
        choose.target = ui;
        NSMenuItem *open = [menu addItemWithTitle:@"Open Current Folder" action:@selector(openFolder:) keyEquivalent:@""];
        open.target = ui;
        NSMenuItem *reset = [menu addItemWithTitle:@"Use Launch Folder" action:@selector(useLaunchFolder:) keyEquivalent:@""];
        reset.target = ui;
        ui.menuItem = [[NSMenuItem alloc] initWithTitle:@"Recording" action:nil keyEquivalent:@""];
        ui.menuItem.submenu = menu;
        [NSApp.mainMenu addItem:ui.menuItem];
        if ([NSProcessInfo.processInfo.environment[@"ISC_LAUNCHER_PID"] intValue] > 0) {
            NSMenu *applicationMenu = NSApp.mainMenu.itemArray.firstObject.submenu;
            ui.devicesItem = [[NSMenuItem alloc] initWithTitle:@"Connected Devices…" action:@selector(showDevices:) keyEquivalent:@"0"];
            ui.devicesItem.target = ui;
            [applicationMenu insertItem:ui.devicesItem atIndex:MIN(2, applicationMenu.numberOfItems)];
        }
        __weak InvalidRecordingUI *weakUI = ui;
        ui.timer = [NSTimer timerWithTimeInterval:0.2 repeats:YES block:^(NSTimer *timer) {
            (void)timer;
            [weakUI refresh];
        }];
        [NSRunLoop.mainRunLoop addTimer:ui.timer forMode:NSRunLoopCommonModes];
        if ([NSProcessInfo.processInfo.environment[@"ISC_LAUNCHER_PIPE"] isEqualToString:@"1"]) {
            ui.launcherInput = NSFileHandle.fileHandleWithStandardInput;
            ui.launcherInput.readabilityHandler = ^(NSFileHandle *handle) {
                NSData *data = handle.availableData;
                // EOF also closes the mirror gracefully if its launcher exits.
                if (!data.length || memchr(data.bytes, 'q', data.length)) {
                    handle.readabilityHandler = nil;
                    SDL_Event event;
                    SDL_zero(event);
                    event.type = SDL_EVENT_QUIT;
                    SDL_PushEvent(&event);
                }
            };
        }
        [ui refresh];
        return (__bridge_retained void *)ui;
    }
}

void
sc_recording_ui_destroy(void *pointer) {
    @autoreleasepool {
        InvalidRecordingUI *ui = (__bridge_transfer InvalidRecordingUI *)pointer;
        [ui dispose];
    }
}
