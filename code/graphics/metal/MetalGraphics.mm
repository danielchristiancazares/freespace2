// Metal renderer for FreeSpace 2 - Quick and dirty triangle demo

#include "MetalGraphics.h"
#include "graphics/2d.h"
#include "graphics/grstub.h"
#include "osapi/osapi.h"
#include "io/timer.h"
#include "cmdline/cmdline.h"

#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#import <SDL.h>
#import <SDL_metal.h>
#import <SDL_syswm.h>
#import <Cocoa/Cocoa.h>

namespace graphics {
namespace metal {

// Metal state
static id<MTLDevice> device = nil;
static id<MTLCommandQueue> commandQueue = nil;
static CAMetalLayer* metalLayer = nil;
static id<MTLRenderPipelineState> pipelineState = nil;
static id<MTLBuffer> vertexBuffer = nil;
static std::unique_ptr<os::GraphicsOperations> graphicsOps;
static std::unique_ptr<os::Viewport> viewport;
static SDL_MetalView metalView = nullptr;

// Vertex data for a triangle
struct Vertex {
    float position[2];
    float color[4];
};

static const Vertex triangleVertices[] = {
    // positions         // colors (RGBA)
    {{  0.0f,  0.5f }, { 1.0f, 0.0f, 0.0f, 1.0f }},  // top - red
    {{ -0.5f, -0.5f }, { 0.0f, 1.0f, 0.0f, 1.0f }},  // bottom left - green
    {{  0.5f, -0.5f }, { 0.0f, 0.0f, 1.0f, 1.0f }},  // bottom right - blue
};

// Metal shader source
static const char* shaderSource = R"(
#include <metal_stdlib>
using namespace metal;

struct VertexIn {
    float2 position [[attribute(0)]];
    float4 color [[attribute(1)]];
};

struct VertexOut {
    float4 position [[position]];
    float4 color;
};

vertex VertexOut vertexShader(VertexIn in [[stage_in]]) {
    VertexOut out;
    out.position = float4(in.position, 0.0, 1.0);
    out.color = in.color;
    return out;
}

fragment float4 fragmentShader(VertexOut in [[stage_in]]) {
    return in.color;
}
)";

static void metal_flip()
{
    @autoreleasepool {
        if (!metalLayer || !device || !commandQueue || !pipelineState) {
            return;
        }

        id<CAMetalDrawable> drawable = [metalLayer nextDrawable];
        if (!drawable) {
            return;
        }

        MTLRenderPassDescriptor* passDescriptor = [MTLRenderPassDescriptor renderPassDescriptor];
        passDescriptor.colorAttachments[0].texture = drawable.texture;
        passDescriptor.colorAttachments[0].loadAction = MTLLoadActionClear;
        passDescriptor.colorAttachments[0].storeAction = MTLStoreActionStore;
        passDescriptor.colorAttachments[0].clearColor = MTLClearColorMake(0.1, 0.1, 0.2, 1.0);

        id<MTLCommandBuffer> commandBuffer = [commandQueue commandBuffer];
        id<MTLRenderCommandEncoder> encoder = [commandBuffer renderCommandEncoderWithDescriptor:passDescriptor];

        [encoder setRenderPipelineState:pipelineState];
        [encoder setVertexBuffer:vertexBuffer offset:0 atIndex:0];
        [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
        [encoder endEncoding];

        [commandBuffer presentDrawable:drawable];
        [commandBuffer commit];
    }
}

static void metal_cleanup(int /*minimize*/)
{
    cleanup();
}

static void metal_clear()
{
    // Clear is handled in flip for this simple demo
}

static void metal_set_clear_color(int r, int g, int b)
{
    // Could store these for use in flip, but keeping it simple
}

static int metal_zbuffer_get()
{
    return 0;
}

static int metal_zbuffer_set(int /*mode*/)
{
    return 0;
}

static void metal_setup_frame()
{
    // Nothing needed for this simple demo
}

void initialize_function_pointers()
{
    // Start with stubs
    gr_stub_init_function_pointers();
}

static void init_metal_function_pointers()
{
    // Override the key functions
    gr_screen.gf_flip = metal_flip;
    gr_screen.gf_clear = metal_clear;
    gr_screen.gf_set_clear_color = metal_set_clear_color;
    gr_screen.gf_zbuffer_get = metal_zbuffer_get;
    gr_screen.gf_zbuffer_set = metal_zbuffer_set;
    gr_screen.gf_setup_frame = metal_setup_frame;
}

bool initialize(std::unique_ptr<os::GraphicsOperations>&& ops)
{
    @autoreleasepool {
        graphicsOps = std::move(ops);

        mprintf(("Initializing Metal renderer...\n"));

        // Create the Metal device
        device = MTLCreateSystemDefaultDevice();
        if (!device) {
            mprintf(("Metal: Failed to create device\n"));
            return false;
        }
        mprintf(("Metal: Using device: %s\n", [[device name] UTF8String]));

        // Create viewport/window
        os::ViewPortProperties props;
        props.title = "FreeSpace 2 Open - Metal";
        props.width = gr_screen.max_w;
        props.height = gr_screen.max_h;
        props.display = 0;

        if (Cmdline_fullscreen_window) {
            props.flags.set(os::ViewPortFlags::Borderless);
        } else if (Cmdline_window) {
            // windowed mode
        } else {
            props.flags.set(os::ViewPortFlags::Fullscreen);
        }

        viewport = graphicsOps->createViewport(props);
        if (!viewport) {
            mprintf(("Metal: Failed to create viewport\n"));
            return false;
        }

        SDL_Window* sdlWindow = viewport->toSDLWindow();
        if (!sdlWindow) {
            mprintf(("Metal: Failed to get SDL window\n"));
            return false;
        }

        // Create SDL Metal view - this properly sets up the CAMetalLayer
        metalView = SDL_Metal_CreateView(sdlWindow);
        if (!metalView) {
            mprintf(("Metal: Failed to create Metal view: %s\n", SDL_GetError()));
            return false;
        }

        // Get the CAMetalLayer from SDL
        metalLayer = (__bridge CAMetalLayer*)SDL_Metal_GetLayer(metalView);
        if (!metalLayer) {
            mprintf(("Metal: Failed to get Metal layer\n"));
            return false;
        }

        // Configure the layer
        metalLayer.device = device;
        metalLayer.pixelFormat = MTLPixelFormatBGRA8Unorm;
        metalLayer.framebufferOnly = YES;

        int w, h;
        SDL_Metal_GetDrawableSize(sdlWindow, &w, &h);
        metalLayer.drawableSize = CGSizeMake(w, h);
        mprintf(("Metal: Drawable size: %d x %d\n", w, h));

        // Create command queue
        commandQueue = [device newCommandQueue];
        if (!commandQueue) {
            mprintf(("Metal: Failed to create command queue\n"));
            return false;
        }

        // Create shaders
        NSError* error = nil;
        id<MTLLibrary> library = [device newLibraryWithSource:[NSString stringWithUTF8String:shaderSource]
                                                      options:nil
                                                        error:&error];
        if (!library) {
            mprintf(("Metal: Failed to compile shaders: %s\n", [[error localizedDescription] UTF8String]));
            return false;
        }

        id<MTLFunction> vertexFunction = [library newFunctionWithName:@"vertexShader"];
        id<MTLFunction> fragmentFunction = [library newFunctionWithName:@"fragmentShader"];

        // Create vertex descriptor
        MTLVertexDescriptor* vertexDescriptor = [[MTLVertexDescriptor alloc] init];
        vertexDescriptor.attributes[0].format = MTLVertexFormatFloat2;
        vertexDescriptor.attributes[0].offset = offsetof(Vertex, position);
        vertexDescriptor.attributes[0].bufferIndex = 0;
        vertexDescriptor.attributes[1].format = MTLVertexFormatFloat4;
        vertexDescriptor.attributes[1].offset = offsetof(Vertex, color);
        vertexDescriptor.attributes[1].bufferIndex = 0;
        vertexDescriptor.layouts[0].stride = sizeof(Vertex);
        vertexDescriptor.layouts[0].stepRate = 1;
        vertexDescriptor.layouts[0].stepFunction = MTLVertexStepFunctionPerVertex;

        // Create pipeline state
        MTLRenderPipelineDescriptor* pipelineDescriptor = [[MTLRenderPipelineDescriptor alloc] init];
        pipelineDescriptor.vertexFunction = vertexFunction;
        pipelineDescriptor.fragmentFunction = fragmentFunction;
        pipelineDescriptor.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
        pipelineDescriptor.vertexDescriptor = vertexDescriptor;

        pipelineState = [device newRenderPipelineStateWithDescriptor:pipelineDescriptor error:&error];
        if (!pipelineState) {
            mprintf(("Metal: Failed to create pipeline state: %s\n", [[error localizedDescription] UTF8String]));
            return false;
        }

        // Create vertex buffer
        vertexBuffer = [device newBufferWithBytes:triangleVertices
                                           length:sizeof(triangleVertices)
                                          options:MTLResourceStorageModeShared];
        if (!vertexBuffer) {
            mprintf(("Metal: Failed to create vertex buffer\n"));
            return false;
        }

        // Initialize function pointers
        init_metal_function_pointers();

        // Set renderer info
        gr_screen.mode = GR_METAL;

        mprintf(("Metal: Initialization complete\n"));

        // Register and set the viewport as main
        auto port = os::addViewport(std::move(viewport));
        os::setMainViewPort(port);

        return true;
    }
}

void cleanup()
{
    @autoreleasepool {
        mprintf(("Metal: Cleaning up...\n"));

        vertexBuffer = nil;
        pipelineState = nil;
        commandQueue = nil;
        metalLayer = nil;
        device = nil;

        if (metalView) {
            SDL_Metal_DestroyView(metalView);
            metalView = nullptr;
        }

        viewport.reset();
        graphicsOps.reset();

        mprintf(("Metal: Cleanup complete\n"));
    }
}

} // namespace metal
} // namespace graphics
