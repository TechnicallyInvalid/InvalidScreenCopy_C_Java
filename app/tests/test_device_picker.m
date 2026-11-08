#import <Foundation/Foundation.h>
#include <assert.h>
#import "device_picker.h"

int main(void) {
    @autoreleasepool {
        assert(ISCParseDevices(@"adb: cannot connect to daemon") == nil);
        assert(ISCParseDevices(@"List of devices attached\n\n").count == 0);
        NSArray<ISCDevice *> *devices = ISCParseDevices(
            @"* daemon started successfully\nList of devices attached\r\n"
             "usb-123\tdevice usb:1-2 product:husky model:Pixel_8_Pro device:husky\r\n"
             "emulator-5554          device product:sdk model:Android_SDK device:emulator\n"
             "192.0.2.4:5555\toffline\n"
             "phone with spaces     unauthorized usb:2-3\n"
             "denied\tno permissions (access unavailable)\n"
             "$(touch /tmp/invalid-picker-should-not-execute)\tdevice model:Literal_Serial\n"
             "usb-123\tdevice model:Duplicate\n"
             "invalid garbage\n");
        assert(devices.count == 6);
        assert([devices[0].name isEqualToString:@"Pixel 8 Pro"]);
        assert([devices[0].connection isEqualToString:@"USB"] && devices[0].ready);
        assert([devices[1].connection isEqualToString:@"Emulator"]);
        assert([devices[2].connection isEqualToString:@"Network"] && !devices[2].ready);
        assert([devices[3].serial isEqualToString:@"phone with spaces"] && !devices[3].ready);
        assert([devices[4].status isEqualToString:@"Access unavailable"]);
        assert([devices[5].serial isEqualToString:@"$(touch /tmp/invalid-picker-should-not-execute)"]);
        assert(!ISCFindADB(@{@"ADB": @"/no/such/platform-tools/adb"}));
        assert(!ISCFindADB(@{@"ADB": @"/tmp"}));
        NSString *root = [NSTemporaryDirectory() stringByAppendingPathComponent:NSUUID.UUID.UUIDString];
        NSString *tools = [root stringByAppendingPathComponent:@"platform-tools"];
        assert([NSFileManager.defaultManager createDirectoryAtPath:tools withIntermediateDirectories:YES attributes:nil error:nil]);
        NSString *fakeADB = [tools stringByAppendingPathComponent:@"adb"];
        assert([NSFileManager.defaultManager createFileAtPath:fakeADB contents:[@"#!/bin/sh\nexit 0\n" dataUsingEncoding:NSUTF8StringEncoding]
            attributes:@{NSFilePosixPermissions: @0700}]);
        assert([ISCFindADB(@{@"ANDROID_HOME": root, @"PATH": @""}) isEqualToString:fakeADB]);
        assert(!ISCFindADB(@{@"ADB": @"missing-adb", @"PATH": @"/no/such/path", @"ANDROID_HOME": root}));
        assert([NSFileManager.defaultManager removeItemAtPath:root error:nil]);
        NSLog(@"PASS: empty/error discovery, USB/network/emulator, authorization, literal serials, explicit ADB override");
    }
    return 0;
}
