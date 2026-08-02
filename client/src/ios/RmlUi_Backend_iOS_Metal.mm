#import "RmlUi_Backend_iOS_Metal.h"
#import "../metal/RmlUi_Renderer_Metal.h"
#import "RmlUi_Platform_iOS.h"

#import <Metal/Metal.h>
#import <MetalKit/MetalKit.h>

struct BackendData {
    SystemInterface_iOS system_interface;
    RenderInterface_Metal* render_interface = nullptr;
};

static BackendData* g_data = nullptr;

bool Backend::Initialize(id<MTLDevice> device, MTKView* view)
{
    if (g_data || !device || !view)
        return false;
    g_data = new BackendData();
    g_data->render_interface = new RenderInterface_Metal(device, view);
    if (!*g_data->render_interface) {
        delete g_data->render_interface;
        delete g_data;
        g_data = nullptr;
        return false;
    }
    return true;
}

void Backend::Shutdown()
{
    if (!g_data)
        return;
    delete g_data->render_interface;
    delete g_data;
    g_data = nullptr;
}

Rml::SystemInterface* Backend::GetSystemInterface()
{
    RMLUI_ASSERT(g_data);
    return &g_data->system_interface;
}

Rml::RenderInterface* Backend::GetRenderInterface()
{
    RMLUI_ASSERT(g_data);
    return g_data->render_interface;
}

void Backend::SetViewport(int width, int height)
{
    RMLUI_ASSERT(g_data);
    g_data->render_interface->SetViewport(width, height);
}

void Backend::SetViewportTopOffset(int top)
{
    RMLUI_ASSERT(g_data);
    g_data->render_interface->SetViewportTopOffset(top);
}

void Backend::BeginFrame(id<MTLCommandBuffer> command_buffer,
                          MTLRenderPassDescriptor* pass_descriptor)
{
    RMLUI_ASSERT(g_data);
    g_data->render_interface->BeginFrame(command_buffer, pass_descriptor);
}

void Backend::EndFrame()
{
    RMLUI_ASSERT(g_data);
    g_data->render_interface->EndFrame();
}
