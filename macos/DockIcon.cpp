// macOS: CoreRay runs as a plain executable (not a .app bundle), so the Dock would show a
// generic icon and raylib/GLFW's SetWindowIcon does nothing on macOS. Set the Dock icon from
// the app's PNG at start-up. Plain C++ through the Objective-C runtime (no Objective-C
// compiler needed), equivalent to:
//   [NSApp setApplicationIconImage:[[NSImage alloc] initWithContentsOfFile:@(path)]];
#include <objc/message.h>
#include <objc/runtime.h>

extern "C" bool coreray_set_dock_icon(const char* pngPath) {
    Class nsString = objc_getClass("NSString"), nsImage = objc_getClass("NSImage"), nsApp = objc_getClass("NSApplication");
    if (!nsString || !nsImage || !nsApp || !pngPath) return false;
    id path = reinterpret_cast<id (*)(Class, SEL, const char*)>(objc_msgSend)(nsString, sel_registerName("stringWithUTF8String:"), pngPath);
    if (!path) return false;
    id image = reinterpret_cast<id (*)(Class, SEL)>(objc_msgSend)(nsImage, sel_registerName("alloc"));
    image = reinterpret_cast<id (*)(id, SEL, id)>(objc_msgSend)(image, sel_registerName("initWithContentsOfFile:"), path);
    if (!image) return false;
    id app = reinterpret_cast<id (*)(Class, SEL)>(objc_msgSend)(nsApp, sel_registerName("sharedApplication"));
    if (!app) return false;
    reinterpret_cast<void (*)(id, SEL, id)>(objc_msgSend)(app, sel_registerName("setApplicationIconImage:"), image);
    return true;
}
