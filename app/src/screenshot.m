#import "screenshot.h"
#include "capture_file.h"
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
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
    // A process exit during writing must leave only a hidden temporary file,
    // never an incomplete PNG with its final timestamped name.
    char *temporary;
    if (asprintf(&temporary, "%s/.InvalidScreenCopy-screenshot-XXXXXX",
                 directory.fileSystemRepresentation) < 0) {
        if (error) *error = [NSError errorWithDomain:NSPOSIXErrorDomain code:ENOMEM userInfo:nil];
        return NO;
    }
    int fd = mkstemp(temporary); // private mode 0600, in the destination filesystem
    int code = errno;
    BOOL ok = NO;
    char *path = NULL;
    if (fd >= 0) {
        if (fcntl(fd, F_SETFD, FD_CLOEXEC) < 0) {
            code = errno;
            close(fd);
        } else {
            FILE *file = fdopen(fd, "wb");
            if (!file) {
                code = errno;
                close(fd);
            } else {
                errno = 0;
                ok = fwrite(png.bytes, 1, png.length, file) == png.length;
                code = errno;
                if (fclose(file)) { ok = NO; code = errno; }
                if (ok) {
                    ok = sc_capture_publish_file(temporary, directory.fileSystemRepresentation,
                                                  unix_ms, "png", &path);
                    if (!ok) code = errno;
                }
            }
        }
        // A successfully published path owns the complete contents. A failed
        // publication leaves existing files untouched, even on unsupported FSes.
        unlink(temporary);
    }
    free(temporary);
    if (ok) {
        if (savedPath) *savedPath = [NSFileManager.defaultManager
            stringWithFileSystemRepresentation:path length:strlen(path)];
    } else {
        if (error) *error = [NSError errorWithDomain:NSPOSIXErrorDomain
            code:code ?: EIO userInfo:nil];
    }
    free(path);
    return ok;
}
