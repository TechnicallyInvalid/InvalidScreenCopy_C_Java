#import "screenshot.h"
#include "capture_file.h"
#include <errno.h>
#include <string.h>
#include <unistd.h>

static void ISCImageError(NSError **error, NSString *message) {
    if (error) *error = [NSError errorWithDomain:@"InvalidScreenCopy.Screenshot" code:1
        userInfo:@{NSLocalizedDescriptionKey: message}];
}

NSData *ISCScreenshotPNG(SDL_Surface *surface, NSError **error) {
    if (!surface) {
        ISCImageError(error, @"No phone image is available yet.");
        return nil;
    }
    SDL_Surface *rgba = SDL_ConvertSurface(surface, SDL_PIXELFORMAT_RGBA32);
    if (!rgba) {
        ISCImageError(error, [NSString stringWithUTF8String:SDL_GetError()]);
        return nil;
    }
    NSBitmapImageRep *bitmap = [[NSBitmapImageRep alloc]
        initWithBitmapDataPlanes:NULL pixelsWide:rgba->w pixelsHigh:rgba->h
        bitsPerSample:8 samplesPerPixel:4 hasAlpha:YES isPlanar:NO
        colorSpaceName:NSDeviceRGBColorSpace bitmapFormat:NSBitmapFormatAlphaNonpremultiplied
        bytesPerRow:rgba->w * 4 bitsPerPixel:32];
    if (bitmap) {
        for (int y = 0; y < rgba->h; ++y) {
            memcpy(bitmap.bitmapData + y * bitmap.bytesPerRow,
                   (const uint8_t *)rgba->pixels + y * rgba->pitch, rgba->w * 4);
        }
    }
    SDL_DestroySurface(rgba);
    NSData *png = [bitmap representationUsingType:NSBitmapImageFileTypePNG properties:@{}];
    if (!png) ISCImageError(error, @"Could not encode the screenshot as PNG.");
    return png;
}

BOOL ISCCopyScreenshot(NSData *png, NSPasteboard *pasteboard, NSError **error) {
    NSBitmapImageRep *bitmap = [[NSBitmapImageRep alloc] initWithData:png];
    NSData *tiff = bitmap.TIFFRepresentation;
    if (!png.length || !bitmap || !tiff) {
        ISCImageError(error, @"Could not prepare the screenshot for the clipboard.");
        return NO;
    }
    NSPasteboardItem *item = [NSPasteboardItem new];
    [item setData:png forType:NSPasteboardTypePNG];
    [item setData:tiff forType:NSPasteboardTypeTIFF];
    [pasteboard clearContents];
    if (![pasteboard writeObjects:@[item]]) {
        ISCImageError(error, @"Could not copy the screenshot to the clipboard.");
        return NO;
    }
    return YES;
}

BOOL ISCSaveScreenshot(NSData *png, NSString *directory, int64_t unix_ms,
                       NSString **savedPath, NSError **error) {
    if (savedPath) *savedPath = nil;
    if (!png.length) {
        ISCImageError(error, @"No screenshot data is available to save.");
        return NO;
    }
    char *path = NULL;
    FILE *file = sc_capture_create_file(directory.fileSystemRepresentation,
                                        unix_ms, "png", &path);
    if (!file) {
        if (error) *error = [NSError errorWithDomain:NSPOSIXErrorDomain code:errno userInfo:nil];
        return NO;
    }
    BOOL ok = fwrite(png.bytes, 1, png.length, file) == png.length;
    int code = errno;
    if (fclose(file)) { ok = NO; code = errno; }
    if (ok) {
        if (savedPath) *savedPath = [NSFileManager.defaultManager
            stringWithFileSystemRepresentation:path length:strlen(path)];
    } else {
        // Remove only our newly reserved, incomplete PNG, never an older file.
        unlink(path);
        if (error) *error = [NSError errorWithDomain:NSPOSIXErrorDomain
            code:code ?: EIO userInfo:nil];
    }
    free(path);
    return ok;
}
