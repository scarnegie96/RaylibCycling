// macOS: CoreRay runs as a plain executable (not a .app bundle), so the Dock would show a
// generic icon and raylib/GLFW's SetWindowIcon does nothing on macOS. Set the Dock icon
// from the app's PNG at start-up instead.
#import <Cocoa/Cocoa.h>

extern "C" bool coreray_set_dock_icon(const char* pngPath) {
    @autoreleasepool {
        NSString* path = [NSString stringWithUTF8String:pngPath];
        NSImage* image = [[NSImage alloc] initWithContentsOfFile:path];
        if (!image) return false;
        [NSApp setApplicationIconImage:image];
        return true;
    }
}
