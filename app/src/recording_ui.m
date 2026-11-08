#import <Cocoa/Cocoa.h>
#include <libavutil/time.h>
#include "recording_ui.h"

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
@property(nonatomic, strong) NSWindow *window;
@property(nonatomic, strong) NSTitlebarAccessoryViewController *accessory;
@property(nonatomic, strong) NSButton *recordButton;
@property(nonatomic, strong) NSButton *stopButton;
@property(nonatomic, strong) NSTextField *status;
@property(nonatomic, strong) NSMenuItem *menuItem;
@property(nonatomic, strong) NSMenuItem *startItem;
@property(nonatomic, strong) NSMenuItem *stopItem;
@property(nonatomic, strong) NSTimer *timer;
@property(nonatomic, strong) NSString *launchDirectory;
@property(nonatomic, strong) NSString *directory;
@property(nonatomic, strong) NSOpenPanel *folderPanel;
- (void)refresh;
- (void)start:(id)sender;
- (void)stop:(id)sender;
- (void)chooseFolder:(id)sender;
- (void)useLaunchFolder:(id)sender;
- (void)dispose;
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
    NSString *detail = [NSString stringWithUTF8String:message] ?: @"";
    NSString *fullDetail = detail;
    if (ready && !active && [detail hasPrefix:@"Waiting for the video"]) {
        detail = @"Ready to record";
    }
    if ([detail hasPrefix:@"Saved:"] || [detail hasPrefix:@"Recording:"]) {
        detail = [[detail hasPrefix:@"Saved:"] ? @"Saved: " : @"Recording: "
            stringByAppendingString:detail.lastPathComponent];
    }
    self.status.stringValue = detail;
    self.status.toolTip = [fullDetail stringByAppendingFormat:@"\nFolder: %@", self.directory];
    self.status.textColor = active ? NSColor.systemRedColor : NSColor.secondaryLabelColor;
    self.recordButton.toolTip = [@"Save a new MP4 in " stringByAppendingString:self.directory];
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
            [[NSUserDefaults standardUserDefaults] setObject:strongSelf.directory
                forKey:@"InvalidScreenCopyRecordingDirectory"];
        }
        strongSelf.folderPanel = nil;
        [strongSelf refresh];
    }];
}
- (void)useLaunchFolder:(id)sender {
    (void)sender;
    self.directory = self.launchDirectory;
    [[NSUserDefaults standardUserDefaults] removeObjectForKey:@"InvalidScreenCopyRecordingDirectory"];
    [self refresh];
}
- (void)dispose {
    [self.timer invalidate];
    self.timer = nil;
    [self.folderPanel cancel:nil];
    [NSApp.mainMenu removeItem:self.menuItem];
    NSUInteger index = [self.window.titlebarAccessoryViewControllers indexOfObject:self.accessory];
    if (index != NSNotFound) {
        [self.window removeTitlebarAccessoryViewControllerAtIndex:index];
    }
}
@end

void *
sc_recording_ui_create(SDL_Window *window, struct sc_recording *recording,
                        struct sc_controller *controller) {
    @autoreleasepool {
        NSWindow *nativeWindow = (__bridge NSWindow *) SDL_GetPointerProperty(
            SDL_GetWindowProperties(window), SDL_PROP_WINDOW_COCOA_WINDOW_POINTER, NULL);
        if (!nativeWindow) return NULL;
        InvalidRecordingUI *ui = [[InvalidRecordingUI alloc] init];
        ui.recording = recording;
        ui.controller = controller;
        ui.window = nativeWindow;
        nativeWindow.titleVisibility = NSWindowTitleHidden;
        ui.launchDirectory = NSFileManager.defaultManager.currentDirectoryPath;
        // Finder launches bundles with cwd '/'; use the app's containing folder.
        if ([ui.launchDirectory isEqualToString:@"/"]
                && [NSBundle.mainBundle.bundlePath hasSuffix:@".app"]) {
            ui.launchDirectory = NSBundle.mainBundle.bundlePath.stringByDeletingLastPathComponent;
        }
        ui.directory = [[NSUserDefaults standardUserDefaults] stringForKey:@"InvalidScreenCopyRecordingDirectory"]
            ?: ui.launchDirectory;
        ui.accessory = [[NSTitlebarAccessoryViewController alloc] init];
        ui.accessory.layoutAttribute = NSLayoutAttributeBottom;
        CGFloat rowWidth = MAX(380, nativeWindow.contentView.bounds.size.width);
        NSView *view = [[InvalidRecordingRow alloc] initWithFrame:NSMakeRect(0, 0, rowWidth, 66)];
        [view.heightAnchor constraintEqualToConstant:66].active = YES;
        ui.accessory.fullScreenMinHeight = 66;
        ui.recordButton = [NSButton buttonWithTitle:@"● Record" target:ui action:@selector(start:)];
        ui.recordButton.frame = NSMakeRect(12, 4, 90, 28);
        ui.recordButton.accessibilityLabel = @"Record";
        ui.stopButton = [NSButton buttonWithTitle:@"■ Stop Recording" target:ui action:@selector(stop:)];
        ui.stopButton.frame = NSMakeRect(108, 4, 142, 28);
        ui.stopButton.accessibilityLabel = @"Stop Recording";
        ui.status = [NSTextField labelWithString:@"Waiting for the video stream…"];
        ui.status.frame = NSMakeRect(260, 8, rowWidth - 272, 20);
        ui.status.autoresizingMask = NSViewWidthSizable;
        ui.status.lineBreakMode = NSLineBreakByTruncatingMiddle;
        ui.status.font = [NSFont systemFontOfSize:11];
        [view addSubview:ui.recordButton];
        [view addSubview:ui.stopButton];
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
        NSMenuItem *reset = [menu addItemWithTitle:@"Use Launch Folder" action:@selector(useLaunchFolder:) keyEquivalent:@""];
        reset.target = ui;
        ui.menuItem = [[NSMenuItem alloc] initWithTitle:@"Recording" action:nil keyEquivalent:@""];
        ui.menuItem.submenu = menu;
        [NSApp.mainMenu addItem:ui.menuItem];
        __weak InvalidRecordingUI *weakUI = ui;
        ui.timer = [NSTimer timerWithTimeInterval:0.2 repeats:YES block:^(NSTimer *timer) {
            (void)timer;
            [weakUI refresh];
        }];
        [NSRunLoop.mainRunLoop addTimer:ui.timer forMode:NSRunLoopCommonModes];
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
