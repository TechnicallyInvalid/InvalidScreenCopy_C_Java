#import <Foundation/Foundation.h>

@interface ISCDevice : NSObject
@property(nonatomic, copy) NSString *serial;
@property(nonatomic, copy) NSString *name;
@property(nonatomic, copy) NSString *state;
@property(nonatomic, copy) NSString *connection;
@property(nonatomic, readonly) BOOL ready;
@property(nonatomic, readonly) NSString *status;
@end

// nil means the adb response was invalid; an empty array means no devices.
NSArray<ISCDevice *> *ISCParseDevices(NSString *output);
NSString *ISCFindADB(NSDictionary<NSString *, NSString *> *environment);
