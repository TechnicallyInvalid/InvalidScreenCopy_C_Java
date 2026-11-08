#import <Cocoa/Cocoa.h>
#include <unistd.h>
#include <fcntl.h>
#import "device_picker.h"

static NSString *ISCMirrorPath(void) {
    return [NSBundle.mainBundle.bundlePath stringByAppendingPathComponent:
        @"Contents/Helpers/InvalidScreenCopyDisplay.app/Contents/MacOS/InvalidScreenCopy"];
}

static NSMutableDictionary *ISCEnvironment(void) {
    NSMutableDictionary *environment = NSProcessInfo.processInfo.environment.mutableCopy;
    environment[@"SCRCPY_SERVER_PATH"] = [NSBundle.mainBundle.resourcePath stringByAppendingPathComponent:@"scrcpy-server"];
    environment[@"SCRCPY_ICON_DIR"] = NSBundle.mainBundle.resourcePath;
    NSString *adb = ISCFindADB(environment);
    if (adb) environment[@"ADB"] = adb;
    return environment;
}

@interface ISCLauncher : NSObject <NSApplicationDelegate, NSWindowDelegate,
    NSTableViewDataSource, NSTableViewDelegate>
@property(nonatomic, strong) NSWindow *window;
@property(nonatomic, strong) NSTableView *table;
@property(nonatomic, strong) NSTextField *status;
@property(nonatomic, strong) NSButton *openButton;
@property(nonatomic, strong) NSButton *closeButton;
@property(nonatomic, strong) NSButton *refreshButton;
@property(nonatomic, strong) NSArray<ISCDevice *> *devices;
@property(nonatomic, strong) NSTimer *timer;
@property(nonatomic, strong) NSTask *scanTask;
@property(nonatomic, strong) NSTask *mirrorTask;
@property(nonatomic, strong) NSPipe *controlPipe;
@property(nonatomic, strong) NSString *mirrorSerial;
@property(nonatomic, strong) NSString *mirrorName;
@property(nonatomic, strong) NSString *launchDirectory;
@property(nonatomic, strong) NSString *scanError;
@property(nonatomic, strong) NSString *logPath;
@property(nonatomic, strong) NSFileHandle *logHandle;
@property(nonatomic, assign) BOOL closingMirror;
@property(nonatomic, assign) BOOL quitting;
- (void)showDevices:(id)sender;
- (void)refresh:(id)sender;
- (void)openDisplay:(id)sender;
- (void)closeDisplay:(id)sender;
- (void)updateControls;
@end

@implementation ISCLauncher
- (void)applicationDidFinishLaunching:(NSNotification *)notification {
    (void)notification;
    self.devices = @[];
    NSApp.applicationIconImage = [[NSImage alloc] initWithContentsOfFile:
        [NSBundle.mainBundle.resourcePath stringByAppendingPathComponent:@"scrcpy.png"]];
    self.launchDirectory = NSFileManager.defaultManager.currentDirectoryPath;
    if ([self.launchDirectory isEqualToString:@"/"])
        self.launchDirectory = NSBundle.mainBundle.bundlePath.stringByDeletingLastPathComponent;
    [self createMenu];
    [self createWindow];
    [self showDevices:nil];
    __weak ISCLauncher *weakSelf = self;
    self.timer = [NSTimer scheduledTimerWithTimeInterval:3 repeats:YES block:^(NSTimer *timer) {
        (void)timer;
        [weakSelf refresh:nil];
    }];
}

- (void)createMenu {
    NSMenu *bar = [NSMenu new];
    NSMenuItem *application = [NSMenuItem new];
    NSMenu *menu = [[NSMenu alloc] initWithTitle:@"InvalidScreenCopy"];
    [menu addItemWithTitle:@"About InvalidScreenCopy" action:@selector(orderFrontStandardAboutPanel:) keyEquivalent:@""];
    [menu addItem:NSMenuItem.separatorItem];
    NSMenuItem *devices = [menu addItemWithTitle:@"Connected Devices…" action:@selector(showDevices:) keyEquivalent:@"0"];
    devices.target = self;
    [menu addItem:NSMenuItem.separatorItem];
    [menu addItemWithTitle:@"Hide InvalidScreenCopy" action:@selector(hide:) keyEquivalent:@"h"];
    [menu addItemWithTitle:@"Quit InvalidScreenCopy" action:@selector(terminate:) keyEquivalent:@"q"];
    application.submenu = menu;
    [bar addItem:application];
    NSMenuItem *windowItem = [NSMenuItem new];
    NSMenu *windows = [[NSMenu alloc] initWithTitle:@"Window"];
    [windows addItemWithTitle:@"Minimize" action:@selector(performMiniaturize:) keyEquivalent:@"m"];
    [windows addItemWithTitle:@"Close Window" action:@selector(performClose:) keyEquivalent:@"w"];
    windowItem.submenu = windows;
    [bar addItem:windowItem];
    NSApp.mainMenu = bar;
    NSApp.windowsMenu = windows;
}

- (void)createWindow {
    self.window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 700, 430)
        styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                  NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable
        backing:NSBackingStoreBuffered defer:NO];
    self.window.title = @"InvalidScreenCopy — Connected Devices";
    self.window.identifier = @"InvalidScreenCopyDevicePicker";
    self.window.contentMinSize = NSMakeSize(620, 370);
    self.window.releasedWhenClosed = NO;
    self.window.delegate = self;
    [self.window center];
    NSView *content = self.window.contentView;
    NSTextField *heading = [NSTextField labelWithString:@"Choose your Android device"];
    heading.font = [NSFont systemFontOfSize:22 weight:NSFontWeightSemibold];
    NSTextField *intro = [NSTextField wrappingLabelWithString:
        @"Connect a phone by USB or Android wireless debugging, then open its display."];
    intro.textColor = NSColor.secondaryLabelColor;
    NSScrollView *scroll = [NSScrollView new];
    scroll.hasVerticalScroller = YES;
    scroll.borderType = NSBezelBorder;
    self.table = [NSTableView new];
    self.table.rowHeight = 50;
    self.table.allowsEmptySelection = YES;
    self.table.allowsMultipleSelection = NO;
    self.table.usesAlternatingRowBackgroundColors = YES;
    self.table.columnAutoresizingStyle = NSTableViewLastColumnOnlyAutoresizingStyle;
    self.table.dataSource = self;
    self.table.delegate = self;
    self.table.target = self;
    self.table.doubleAction = @selector(openDisplay:);
    self.table.accessibilityLabel = @"Connected Android devices";
    for (NSString *key in @[@"Device", @"Connection", @"Status"]) {
        NSTableColumn *column = [[NSTableColumn alloc] initWithIdentifier:key];
        column.title = key;
        column.width = [key isEqualToString:@"Device"] ? 285 : [key isEqualToString:@"Connection"] ? 100 : 220;
        column.minWidth = [key isEqualToString:@"Device"] ? 220 : 90;
        [self.table addTableColumn:column];
    }
    scroll.documentView = self.table;
    self.status = [NSTextField wrappingLabelWithString:@"Looking for connected devices…"];
    self.status.textColor = NSColor.secondaryLabelColor;
    self.status.accessibilityLabel = @"Connection status";
    self.refreshButton = [NSButton buttonWithTitle:@"Refresh" target:self action:@selector(refresh:)];
    self.closeButton = [NSButton buttonWithTitle:@"Close Display" target:self action:@selector(closeDisplay:)];
    self.openButton = [NSButton buttonWithTitle:@"Open Display" target:self action:@selector(openDisplay:)];
    self.openButton.keyEquivalent = @"\r";
    for (NSView *view in @[heading, intro, scroll, self.status, self.refreshButton, self.closeButton, self.openButton]) {
        view.translatesAutoresizingMaskIntoConstraints = NO;
        [content addSubview:view];
    }
    [NSLayoutConstraint activateConstraints:@[
        [heading.leadingAnchor constraintEqualToAnchor:content.leadingAnchor constant:24],
        [heading.topAnchor constraintEqualToAnchor:content.topAnchor constant:24],
        [intro.leadingAnchor constraintEqualToAnchor:heading.leadingAnchor],
        [intro.trailingAnchor constraintEqualToAnchor:content.trailingAnchor constant:-24],
        [intro.topAnchor constraintEqualToAnchor:heading.bottomAnchor constant:8],
        [scroll.leadingAnchor constraintEqualToAnchor:heading.leadingAnchor],
        [scroll.trailingAnchor constraintEqualToAnchor:intro.trailingAnchor],
        [scroll.topAnchor constraintEqualToAnchor:intro.bottomAnchor constant:18],
        [scroll.bottomAnchor constraintEqualToAnchor:self.status.topAnchor constant:-12],
        [self.status.leadingAnchor constraintEqualToAnchor:heading.leadingAnchor],
        [self.status.trailingAnchor constraintEqualToAnchor:intro.trailingAnchor],
        [self.status.heightAnchor constraintEqualToConstant:42],
        [self.status.bottomAnchor constraintEqualToAnchor:self.openButton.topAnchor constant:-12],
        [self.openButton.trailingAnchor constraintEqualToAnchor:intro.trailingAnchor],
        [self.openButton.bottomAnchor constraintEqualToAnchor:content.bottomAnchor constant:-20],
        [self.openButton.widthAnchor constraintGreaterThanOrEqualToConstant:125],
        [self.closeButton.trailingAnchor constraintEqualToAnchor:self.openButton.leadingAnchor constant:-10],
        [self.closeButton.centerYAnchor constraintEqualToAnchor:self.openButton.centerYAnchor],
        [self.refreshButton.leadingAnchor constraintEqualToAnchor:heading.leadingAnchor],
        [self.refreshButton.centerYAnchor constraintEqualToAnchor:self.openButton.centerYAnchor],
    ]];
}

- (ISCDevice *)selectedDevice {
    NSInteger row = self.table.selectedRow;
    return row >= 0 && row < (NSInteger)self.devices.count ? self.devices[row] : nil;
}

- (void)showDevices:(id)sender {
    (void)sender;
    [self.window deminiaturize:nil];
    [self.window makeKeyAndOrderFront:nil];
    if (@available(macOS 14.0, *)) [NSApp activate];
    else [NSApp activateIgnoringOtherApps:YES];
    [self refresh:nil];
}

- (BOOL)applicationShouldHandleReopen:(NSApplication *)app hasVisibleWindows:(BOOL)visible {
    (void)app; (void)visible;
    [self showDevices:nil];
    return YES;
}

- (void)applicationDidBecomeActive:(NSNotification *)notification {
    (void)notification;
    // The display activates this existing process directly. Restore a closed
    // or minimized picker without opening the bundle again through LaunchServices.
    [self.window deminiaturize:nil];
    [self.window makeKeyAndOrderFront:nil];
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)app {
    (void)app;
    return NO;
}

- (void)updateControls {
    ISCDevice *selected = [self selectedDevice];
    BOOL active = self.mirrorTask != nil;
    self.openButton.title = active ? @"Show Display" : @"Open Display";
    self.openButton.enabled = !self.closingMirror && (active || selected.ready);
    self.closeButton.enabled = active && !self.closingMirror;
    self.refreshButton.enabled = self.scanTask == nil;
    if (self.closingMirror) self.status.stringValue = @"Closing the display and finishing any recording…";
    else if (active) self.status.stringValue = [NSString stringWithFormat:
        @"%@ is open. Close its display before switching devices.", self.mirrorName];
    else if (self.scanError) self.status.stringValue = self.scanError;
    else if (!self.devices.count) self.status.stringValue =
        @"No Android devices found. Connect a phone, enable USB debugging and unlock it to allow this Mac.";
    else if ([selected.state isEqualToString:@"unauthorized"] || [selected.state isEqualToString:@"authorizing"])
        self.status.stringValue = @"Unlock the phone and accept its USB debugging prompt. The list refreshes automatically.";
    else if (selected && !selected.ready) self.status.stringValue = @"This device is unavailable. Reconnect or unlock it, then try again.";
    else self.status.stringValue = @"Select a ready device and click Open Display. The list refreshes automatically.";
}

- (void)refresh:(id)sender {
    (void)sender;
    if (self.scanTask || self.quitting) return;
    NSString *adb = ISCFindADB(NSProcessInfo.processInfo.environment);
    if (!adb) {
        self.devices = @[];
        self.scanError = @"Android platform-tools (adb) was not found. Install it in the Android SDK or Homebrew, then reopen this app.";
        [self.table reloadData];
        [self updateControls];
        return;
    }
    NSTask *task = [NSTask new];
    task.executableURL = [NSURL fileURLWithPath:adb];
    task.arguments = @[@"devices", @"-l"];
    task.standardInput = NSFileHandle.fileHandleWithNullDevice;
    NSPipe *pipe = NSPipe.pipe;
    task.standardOutput = pipe;
    task.standardError = pipe;
    NSError *error = nil;
    if (![task launchAndReturnError:&error]) {
        self.devices = @[];
        [self.table reloadData];
        self.scanError = [@"Could not list devices: " stringByAppendingString:error.localizedDescription];
        [self updateControls];
        return;
    }
    self.scanTask = task;
    self.refreshButton.enabled = NO;
    __weak ISCLauncher *weakSelf = self;
    __block BOOL timedOut = NO;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 8 * NSEC_PER_SEC), dispatch_get_main_queue(), ^{
        if (task.running) { timedOut = YES; [task terminate]; }
    });
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
        @autoreleasepool {
            NSData *data = [pipe.fileHandleForReading readDataToEndOfFile];
            [task waitUntilExit];
            NSString *output = [[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding] ?: @"";
            dispatch_async(dispatch_get_main_queue(), ^{
                ISCLauncher *owner = weakSelf;
                if (!owner || owner.quitting) return;
                NSString *selection = [owner selectedDevice].serial;
                NSArray *devices = task.terminationStatus == 0 && !timedOut ? ISCParseDevices(output) : nil;
                owner.scanTask = nil;
                owner.devices = devices ?: @[];
                owner.scanError = devices ? nil : timedOut ? @"Device discovery timed out. Reconnect the phone, then click Refresh."
                    : @"Could not communicate with Android Debug Bridge. Reconnect the phone, then click Refresh.";
                [owner.table reloadData];
                NSInteger selected = -1;
                for (NSUInteger i = 0; i < owner.devices.count; ++i) {
                    if ([owner.devices[i].serial isEqualToString:selection]) { selected = i; break; }
                }
                if (selected < 0) {
                    for (NSUInteger i = 0; i < owner.devices.count; ++i) {
                        if (owner.devices[i].ready) { selected = i; break; }
                    }
                }
                if (selected < 0 && owner.devices.count) selected = 0;
                if (selected >= 0) [owner.table selectRowIndexes:[NSIndexSet indexSetWithIndex:selected] byExtendingSelection:NO];
                [owner updateControls];
            });
        }
    });
}

- (NSInteger)numberOfRowsInTableView:(NSTableView *)tableView {
    (void)tableView;
    return self.devices.count;
}

- (NSView *)tableView:(NSTableView *)table viewForTableColumn:(NSTableColumn *)column row:(NSInteger)row {
    (void)table;
    ISCDevice *device = self.devices[row];
    NSTextField *text = [NSTextField labelWithString:@""];
    text.lineBreakMode = NSLineBreakByTruncatingMiddle;
    if ([column.identifier isEqualToString:@"Device"]) {
        text.stringValue = [NSString stringWithFormat:@"%@\n%@", device.name, device.serial];
        text.maximumNumberOfLines = 2;
    } else if ([column.identifier isEqualToString:@"Connection"]) text.stringValue = device.connection;
    else text.stringValue = [device.serial isEqualToString:self.mirrorSerial] ? @"Display open" : device.status;
    text.toolTip = device.serial;
    text.textColor = device.ready ? NSColor.labelColor : NSColor.secondaryLabelColor;
    NSTableCellView *cell = [NSTableCellView new];
    cell.textField = text;
    text.translatesAutoresizingMaskIntoConstraints = NO;
    [cell addSubview:text];
    [NSLayoutConstraint activateConstraints:@[
        [text.leadingAnchor constraintEqualToAnchor:cell.leadingAnchor constant:8],
        [text.trailingAnchor constraintEqualToAnchor:cell.trailingAnchor constant:-8],
        [text.centerYAnchor constraintEqualToAnchor:cell.centerYAnchor],
    ]];
    return cell;
}

- (void)tableViewSelectionDidChange:(NSNotification *)notification {
    (void)notification;
    [self updateControls];
}

- (void)openDisplay:(id)sender {
    (void)sender;
    if (self.closingMirror) return;
    if (self.mirrorTask) {
        NSRunningApplication *app = [NSRunningApplication runningApplicationWithProcessIdentifier:self.mirrorTask.processIdentifier];
        if (@available(macOS 14.0, *)) {
            [NSApp yieldActivationToApplication:app];
            [app activateFromApplication:NSRunningApplication.currentApplication options:NSApplicationActivateAllWindows];
        } else [app activateWithOptions:NSApplicationActivateAllWindows];
        return;
    }
    ISCDevice *device = [self selectedDevice];
    if (!device.ready) return;
    NSTask *task = [NSTask new];
    task.executableURL = [NSURL fileURLWithPath:ISCMirrorPath()];
    // Device identifiers remain individual arguments, never shell commands.
    task.arguments = @[@"--serial", device.serial, @"--stay-awake",
        @"--window-title", [@"InvalidScreenCopy — " stringByAppendingString:device.name]];
    NSMutableDictionary *environment = ISCEnvironment();
    environment[@"ISC_LAUNCHER_PIPE"] = @"1";
    environment[@"ISC_LAUNCHER_PID"] = [NSString stringWithFormat:@"%d", getpid()];
    environment[@"ISC_RECORDING_LAUNCH_DIRECTORY"] = self.launchDirectory;
    task.environment = environment;
    // Resolving an inherited cwd inside a protected/unavailable folder can
    // block before AppKit can show a window. The destination is passed above;
    // the mirror needs no filesystem traversal merely to discover that path.
    task.currentDirectoryURL = [NSURL fileURLWithPath:@"/" isDirectory:YES];
    self.controlPipe = NSPipe.pipe;
    fcntl(self.controlPipe.fileHandleForWriting.fileDescriptor, F_SETNOSIGPIPE, 1);
    task.standardInput = self.controlPipe;
    NSString *template = [NSTemporaryDirectory() stringByAppendingPathComponent:@"InvalidScreenCopy-display-XXXXXX"];
    char *path = strdup(template.fileSystemRepresentation);
    int fd = path ? mkstemp(path) : -1;
    if (fd < 0) {
        free(path);
        self.controlPipe = nil;
        self.scanError = @"Could not create a temporary display log. Check free disk space and try again.";
        [self updateControls];
        return;
    }
    self.logPath = [NSFileManager.defaultManager stringWithFileSystemRepresentation:path length:strlen(path)];
    free(path);
    self.logHandle = [[NSFileHandle alloc] initWithFileDescriptor:fd closeOnDealloc:YES];
    task.standardOutput = self.logHandle;
    task.standardError = self.logHandle;
    __weak ISCLauncher *weakSelf = self;
    task.terminationHandler = ^(NSTask *ended) {
        dispatch_async(dispatch_get_main_queue(), ^{ [weakSelf mirrorEnded:ended]; });
    };
    NSError *error = nil;
    if (![task launchAndReturnError:&error]) {
        self.controlPipe = nil;
        [self cleanLog];
        [self showError:[@"Could not open the display. " stringByAppendingString:error.localizedDescription]];
        return;
    }
    self.mirrorTask = task;
    self.mirrorSerial = device.serial;
    self.mirrorName = device.name;
    self.scanError = nil;
    [self.table reloadData];
    [self updateControls];
    // The new process needs a moment to register its AppKit window. Transfer
    // activation cooperatively instead of relying on a background launch.
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC), dispatch_get_main_queue(), ^{
        if (weakSelf.mirrorTask == task && !weakSelf.closingMirror) [weakSelf openDisplay:nil];
    });
}

- (void)closeDisplay:(id)sender {
    (void)sender;
    if (!self.mirrorTask || self.closingMirror) return;
    self.closingMirror = YES;
    // Request SDL's normal quit path over a private pipe. AppKit-launched
    // processes may inherit ignored SIGINT, so a signal is not a reliable close.
    NSError *error = nil;
    [self.controlPipe.fileHandleForWriting writeData:[@"q" dataUsingEncoding:NSUTF8StringEncoding] error:&error];
    [self updateControls];
}

- (NSString *)logTail {
    NSFileHandle *file = [NSFileHandle fileHandleForReadingAtPath:self.logPath];
    unsigned long long end = [file seekToEndOfFile];
    [file seekToFileOffset:end > 6000 ? end - 6000 : 0];
    NSData *data = [file readDataToEndOfFile];
    [file closeFile];
    return [[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding] ?: @"No diagnostic details available.";
}

- (void)cleanLog {
    [self.logHandle closeFile];
    self.logHandle = nil;
    if (self.logPath) [NSFileManager.defaultManager removeItemAtPath:self.logPath error:nil];
    self.logPath = nil;
}

- (void)showError:(NSString *)detail {
    [self showDevices:nil];
    NSAlert *alert = [NSAlert new];
    alert.messageText = @"Display unavailable";
    alert.informativeText = detail;
    [alert beginSheetModalForWindow:self.window completionHandler:nil];
}

- (void)mirrorEnded:(NSTask *)task {
    if (task != self.mirrorTask) return;
    BOOL expected = self.closingMirror;
    NSString *details = task.terminationStatus != 0 && !expected ? [self logTail] : nil;
    [self cleanLog];
    [self.controlPipe.fileHandleForWriting closeFile];
    self.controlPipe = nil;
    self.mirrorTask = nil;
    self.mirrorSerial = nil;
    self.mirrorName = nil;
    self.closingMirror = NO;
    [self.table reloadData];
    [self updateControls];
    if (self.quitting) [NSApp replyToApplicationShouldTerminate:YES];
    else if (details) [self showError:details];
    else [self showDevices:nil];
}

- (NSApplicationTerminateReply)applicationShouldTerminate:(NSApplication *)sender {
    (void)sender;
    self.quitting = YES;
    [self.timer invalidate];
    if (self.scanTask.running) [self.scanTask terminate];
    if (self.mirrorTask) {
        [self closeDisplay:nil];
        return NSTerminateLater;
    }
    return NSTerminateNow;
}
@end

int main(int argc, char **argv) {
    @autoreleasepool {
        // Explicit command-line arguments retain the existing CLI behavior.
        if (argc > 1 && strncmp(argv[1], "-psn_", 5)) {
            NSDictionary *environment = ISCEnvironment();
            for (NSString *key in @[@"ADB", @"SCRCPY_SERVER_PATH", @"SCRCPY_ICON_DIR"]) {
                NSString *value = environment[key];
                if (value) setenv(key.UTF8String, value.UTF8String, 1);
            }
            execv(ISCMirrorPath().fileSystemRepresentation, argv);
            perror("Could not run InvalidScreenCopy");
            return 1;
        }
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
        __attribute__((objc_precise_lifetime)) ISCLauncher *delegate = [ISCLauncher new];
        NSApp.delegate = delegate;
        [NSApp run];
    }
    return 0;
}
