#import "screenshot.h"
#include <assert.h>
#include <sys/stat.h>

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
        struct stat info;
        assert(!stat(first.fileSystemRepresentation, &info));
        assert((info.st_mode & 0777) == 0600);
        assert(!ISCSaveScreenshot(png, [directory stringByAppendingPathComponent:@"missing"], 0, nil, &error));
        assert(error.code == ENOENT);
        assert([NSFileManager.defaultManager removeItemAtPath:directory error:nil]);
        NSLog(@"PASS: PNG dimensions/colors/stride; missing frame/folder; exclusive names, symlink collisions, permissions");
    }
    return 0;
}
