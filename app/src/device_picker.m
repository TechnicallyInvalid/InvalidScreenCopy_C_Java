#import "device_picker.h"

@implementation ISCDevice
- (BOOL)ready { return [self.state isEqualToString:@"device"]; }
- (NSString *)status {
    if (self.ready) return @"Ready";
    if ([self.state isEqualToString:@"unauthorized"] || [self.state isEqualToString:@"authorizing"])
        return @"Allow USB debugging";
    if ([self.state isEqualToString:@"offline"]) return @"Offline — reconnect";
    if ([self.state isEqualToString:@"no permissions"]) return @"Access unavailable";
    return self.state.capitalizedString;
}
@end

NSArray<ISCDevice *> *ISCParseDevices(NSString *output) {
    BOOL header = NO;
    NSMutableArray *devices = [NSMutableArray array];
    NSMutableSet *serials = [NSMutableSet set];
    NSSet *states = [NSSet setWithArray:@[@"device", @"unauthorized", @"offline",
        @"bootloader", @"host", @"recovery", @"rescue", @"sideload",
        @"authorizing", @"connecting", @"detached", @"no permissions"]];
    for (NSString *line in [output componentsSeparatedByCharactersInSet:NSCharacterSet.newlineCharacterSet]) {
        if ([line hasPrefix:@"List of devices attached"]) { header = YES; continue; }
        if (!header || [line hasPrefix:@"*"] || [line hasPrefix:@"adb server"]
                || [line hasPrefix:@"adb:"] || [line hasPrefix:@"error:"]) continue;
        NSRange separator = [line rangeOfString:@"\t"];
        NSUInteger detailsStart = separator.location + 1;
        if (separator.location == NSNotFound) {
            // `devices -l` may pad serials with spaces instead of a tab. Scan
            // backwards for the state, preserving spaces inside the serial.
            NSRegularExpression *words = [NSRegularExpression regularExpressionWithPattern:@"\\S+" options:0 error:nil];
            NSArray<NSTextCheckingResult *> *matches = [words matchesInString:line options:0 range:NSMakeRange(0, line.length)];
            for (NSTextCheckingResult *match in matches.reverseObjectEnumerator) {
                NSString *token = [line substringWithRange:match.range];
                if ([states containsObject:token] || [[line substringFromIndex:match.range.location] hasPrefix:@"no permissions"]) {
                    separator = NSMakeRange(match.range.location, 0);
                    detailsStart = match.range.location;
                    break;
                }
            }
        }
        if (separator.location == NSNotFound) continue;
        NSString *serial = [[line substringToIndex:separator.location]
            stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceCharacterSet];
        NSString *details = [[line substringFromIndex:detailsStart]
            stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceCharacterSet];
        NSArray *tokens = [[details componentsSeparatedByCharactersInSet:NSCharacterSet.whitespaceCharacterSet]
            filteredArrayUsingPredicate:[NSPredicate predicateWithFormat:@"length > 0"]];
        NSString *state = [details hasPrefix:@"no permissions"] ? @"no permissions" : tokens.firstObject;
        if (!serial.length || !state || ![states containsObject:state] || [serials containsObject:serial]) continue;
        ISCDevice *device = [ISCDevice new];
        device.serial = serial;
        device.state = state;
        device.name = serial;
        device.connection = [serial hasPrefix:@"emulator-"] ? @"Emulator" : @"Network";
        for (NSString *token in tokens) {
            if ([token hasPrefix:@"model:"] && token.length > 6)
                device.name = [[token substringFromIndex:6] stringByReplacingOccurrencesOfString:@"_" withString:@" "];
            if ([token hasPrefix:@"usb:"]) device.connection = @"USB";
        }
        if ([device.connection isEqualToString:@"Network"] && ![serial containsString:@":"]
                && ![serial containsString:@"._tcp"]) device.connection = @"Android";
        [devices addObject:device];
        [serials addObject:serial];
    }
    return header ? devices : nil;
}

static BOOL ISCIsExecutable(NSString *path) {
    BOOL directory = NO;
    return [NSFileManager.defaultManager fileExistsAtPath:path isDirectory:&directory]
        && !directory && [NSFileManager.defaultManager isExecutableFileAtPath:path];
}

NSString *ISCFindADB(NSDictionary<NSString *, NSString *> *environment) {
    NSMutableArray<NSString *> *candidates = [NSMutableArray array];
    NSString *override = environment[@"ADB"];
    // Match the CLI's explicit ADB override; do not silently select a different
    // executable when a configured absolute path is missing.
    if ([override containsString:@"/"]) {
        NSString *path = override.isAbsolutePath ? override :
            [NSFileManager.defaultManager.currentDirectoryPath stringByAppendingPathComponent:override];
        return ISCIsExecutable(path) ? path : nil;
    }
    if (!override.length) for (NSString *key in @[@"ANDROID_HOME", @"ANDROID_SDK_ROOT"]) {
        NSString *root = environment[key];
        if (root.length) [candidates addObject:[root stringByAppendingPathComponent:@"platform-tools/adb"]];
    }
    NSString *name = override.length ? override : @"adb";
    for (NSString *directory in [environment[@"PATH"] componentsSeparatedByString:@":"]) {
        if (directory.isAbsolutePath) [candidates addObject:[directory stringByAppendingPathComponent:name]];
    }
    if (!override.length) [candidates addObjectsFromArray:@[
        [NSHomeDirectory() stringByAppendingPathComponent:@"Library/Android/sdk/platform-tools/adb"],
        @"/opt/homebrew/bin/adb", @"/usr/local/bin/adb"]];
    for (NSString *path in candidates) {
        if (ISCIsExecutable(path)) return path;
    }
    return nil;
}
