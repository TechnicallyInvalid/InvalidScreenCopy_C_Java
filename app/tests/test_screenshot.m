#import "screenshot.h"
#include "capture_file.h"
#include <assert.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

int main(void) {
    @autoreleasepool {
        // Non-square image and padded rows catch dimensions, channel ordering,
        // accidental vertical flips, and incorrect stride handling.
        uint8_t pixels[40] = {
            255,0,0,255, 0,255,0,255, 0,0,255,255, 0,0,0,0, 0,0,0,0,
            0,255,255,255, 255,0,255,255, 255,255,0,255, 0,0,0,0, 0,0,0,0,
        };
        SDL_Surface *surface = SDL_CreateSurfaceFrom(3, 2, SDL_PIXELFORMAT_RGBA32, pixels, 20);
        assert(surface);
        NSError *error = nil;
        NSData *png = ISCScreenshotPNG(surface, &error);
        SDL_DestroySurface(surface);
        assert(png && !error);
        NSBitmapImageRep *image = [[NSBitmapImageRep alloc] initWithData:png];
        assert(image.pixelsWide == 3 && image.pixelsHigh == 2);
        for (int y = 0; y < 2; ++y) for (int x = 0; x < 3; ++x) {
            NSUInteger actual[4];
            [image getPixel:actual atX:x y:y];
            for (int c = 0; c < 4; ++c) assert(actual[c] == pixels[y * 20 + x * 4 + c]);
        }
        assert(!ISCScreenshotPNG(NULL, &error) && error);

        NSString *directory = [NSTemporaryDirectory() stringByAppendingPathComponent:NSUUID.UUID.UUIDString];
        assert([NSFileManager.defaultManager createDirectoryAtPath:directory
            withIntermediateDirectories:NO attributes:nil error:nil]);
        NSString *first, *second, *third;
        assert(ISCSaveScreenshot(png, directory, 1794100000123, &first, nil));
        assert(ISCSaveScreenshot(png, directory, 1794100000123, &second, nil));
        assert([first hasSuffix:@"-123.png"] && [second hasSuffix:@"-123-001.png"]);
        NSString *link = [[first stringByDeletingPathExtension] stringByAppendingString:@"-002.png"];
        assert([NSFileManager.defaultManager createSymbolicLinkAtPath:link withDestinationPath:first error:nil]);
        assert(ISCSaveScreenshot(png, directory, 1794100000123, &third, nil));
        assert([third hasSuffix:@"-123-003.png"]);
        assert([[NSData dataWithContentsOfFile:first] isEqualToData:png]);
        assert([[NSData dataWithContentsOfFile:second] isEqualToData:png]);
        assert([[NSData dataWithContentsOfFile:third] isEqualToData:png]);
        struct stat info;
        assert(!stat(first.fileSystemRepresentation, &info));
        assert((info.st_mode & 0777) == 0600);

        // A partly written staging file has no final PNG name. Publication
        // happens only once its complete contents have been closed successfully.
        NSString *pending = [directory stringByAppendingPathComponent:@".pending-screenshot"];
        NSString *fourth = [[first stringByDeletingPathExtension] stringByAppendingString:@"-004.png"];
        int fd = open(pending.fileSystemRepresentation, O_WRONLY | O_CREAT | O_EXCL, 0600);
        assert(fd >= 0);
        FILE *staged = fdopen(fd, "wb");
        assert(staged);
        NSUInteger half = png.length / 2;
        assert(fwrite(png.bytes, 1, half, staged) == half);
        assert(!fflush(staged));
        assert(![NSFileManager.defaultManager fileExistsAtPath:fourth]);
        assert(fwrite((const uint8_t *)png.bytes + half, 1, png.length - half, staged) == png.length - half);
        assert(!fclose(staged));
        char *published = NULL;
        assert(sc_capture_publish_file(pending.fileSystemRepresentation, directory.fileSystemRepresentation,
                                        1794100000123, "png", &published));
        assert([fourth isEqualToString:[NSString stringWithUTF8String:published]]);
        assert([[NSData dataWithContentsOfFile:fourth] isEqualToData:png]);
        assert(!stat(published, &info) && (info.st_mode & 0777) == 0600);
        // Exclusive rename consumes the temporary name; hard-link publication
        // leaves it for the caller to remove. Both expose only complete data.
        if ([NSFileManager.defaultManager fileExistsAtPath:pending])
            assert(!unlink(pending.fileSystemRepresentation));
        assert(!stat(published, &info) && info.st_nlink == 1);
        free(published);

        // Publication failure must neither replace a previous file nor leave a
        // final name or hidden temporary file behind.
        NSArray *beforeFailure = [NSFileManager.defaultManager contentsOfDirectoryAtPath:directory error:nil];
        assert(!ISCSaveScreenshot(png, directory, -1, nil, &error) && error.code == EINVAL);
        NSArray *afterFailure = [NSFileManager.defaultManager contentsOfDirectoryAtPath:directory error:nil];
        assert([[NSSet setWithArray:beforeFailure] isEqualToSet:[NSSet setWithArray:afterFailure]]);
        for (NSString *name in afterFailure) assert(![name hasPrefix:@".InvalidScreenCopy-screenshot-"]);
        assert([[NSData dataWithContentsOfFile:first] isEqualToData:png]);
        assert(!ISCSaveScreenshot(png, [directory stringByAppendingPathComponent:@"missing"], 0, nil, &error));
        assert(error.code == ENOENT);
        assert([NSFileManager.defaultManager removeItemAtPath:directory error:nil]);
        NSLog(@"PASS: PNG dimensions/colors/stride; atomic publication; complete payloads; collisions, permissions, failure cleanup");
    }
    return 0;
}
