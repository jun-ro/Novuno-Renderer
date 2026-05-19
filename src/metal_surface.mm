#ifdef __APPLE__
#import <Cocoa/Cocoa.h>
#import <QuartzCore/CAMetalLayer.h>

// Returns a CAMetalLayer* attached to the NSWindow's contentView.
// Called from main.cpp via: extern void* create_metal_layer(void* ns_window);
extern "C" void* create_metal_layer(void* ns_window_ptr) {
    NSWindow* nswin = (__bridge NSWindow*)ns_window_ptr;
    NSView*   view  = [nswin contentView];
    [view setWantsLayer:YES];
    CAMetalLayer* layer = [CAMetalLayer layer];
    [view setLayer:layer];
    return (__bridge void*)layer;
}
#endif
