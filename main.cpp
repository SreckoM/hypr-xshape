// hypr-xshape: honour X11 SHAPE bounding regions for XWayland windows.
//
// Hyprland ignores the X11 shape extension, so the part of a shaped window
// outside its shape is drawn as whatever the buffer holds there (black, or the
// app's background colour). GNOME clips it away. Modo and Houdini rely on this
// for their pie menus.
//
// We read each X11 window's bounding shape over our own xcb connection and put
// it into CWLSurface::m_visibleRegion (buffer coordinates), which the surface
// renderer already clips to. All X requests are asynchronous: blocking on
// Xwayland from inside the compositor could deadlock both.

#define WLR_USE_UNSTABLE

#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/desktop/view/WLSurface.hpp>
#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/xwayland/XSurface.hpp>

// Hyprland 0.54+ replaced the string hook system with Event::bus() and moved
// the window list out of CCompositor. Pick whichever API the headers provide.
#if __has_include(<hyprland/src/event/EventBus.hpp>)
#define XSHAPE_EVENT_BUS 1
#include <hyprland/src/desktop/state/WindowState.hpp>
#include <hyprland/src/event/EventBus.hpp>
#endif

#include <xcb/shape.h>
#include <xcb/xcb.h>
#include <xcb/xcbext.h>

#include <cstdlib>
#include <vector>

static HANDLE                 PHANDLE   = nullptr;
#ifdef XSHAPE_EVENT_BUS
static CHyprSignalListener    g_openHook;
#else
static SP<HOOK_CALLBACK_FN>   g_openHook;
#endif
static xcb_connection_t*      g_conn    = nullptr;
static wl_event_source*       g_source  = nullptr;
static uint8_t                g_shapeEv = 0;

struct SPending {
    xcb_window_t                         id;
    xcb_shape_query_extents_cookie_t     ext;
    xcb_shape_get_rectangles_cookie_t    rects;
    xcb_shape_query_extents_reply_t*     extReply   = nullptr;
    xcb_shape_get_rectangles_reply_t*    rectsReply = nullptr;
    bool                                 extDone = false, rectsDone = false;
};
static std::vector<SPending> g_pending;

static const std::vector<PHLWINDOW>& allWindows() {
#ifdef XSHAPE_EVENT_BUS
    return Desktop::windowState()->windows();
#else
    return g_pCompositor->m_windows;
#endif
}

static PHLWINDOW findWindow(xcb_window_t id) {
    for (auto& w : allWindows()) {
        if (!w->m_isX11)
            continue;
        auto xs = w->m_xwaylandSurface.lock();
        if (xs && xs->m_xID == id)
            return w;
    }
    return nullptr;
}

static void requestShape(xcb_window_t id) {
    if (!g_conn)
        return;
    xcb_shape_select_input(g_conn, id, 1);
    g_pending.push_back({.id = id, .ext = xcb_shape_query_extents(g_conn, id), .rects = xcb_shape_get_rectangles(g_conn, id, XCB_SHAPE_SK_BOUNDING)});
    xcb_flush(g_conn);
}

static void applyShape(const SPending& p) {
    auto w = findWindow(p.id);
    if (!w)
        return;
    auto surf = w->wlSurface();
    if (!surf)
        return;

    CRegion region;
    if (p.extReply && p.extReply->bounding_shaped && p.rectsReply) {
        const auto* r = xcb_shape_get_rectangles_rectangles(p.rectsReply);
        const int   n = xcb_shape_get_rectangles_rectangles_length(p.rectsReply);
        for (int i = 0; i < n; ++i)
            region.add(CBox{(double)r[i].x, (double)r[i].y, (double)r[i].width, (double)r[i].height});
        // Shaped to nothing: an empty region would mean "no clipping", so use a box outside the buffer.
        if (region.empty())
            region.add(CBox{-2, -2, 1, 1});
    }

    if (surf->m_visibleRegion.empty() && region.empty())
        return;
    surf->m_visibleRegion = region;
    g_pHyprRenderer->damageWindow(w, true);
}

static void disconnect() {
    if (g_source)
        wl_event_source_remove(g_source);
    g_source = nullptr;
    for (auto& p : g_pending) {
        free(p.extReply);
        free(p.rectsReply);
    }
    g_pending.clear();
    if (g_conn)
        xcb_disconnect(g_conn);
    g_conn = nullptr;
}

static void pollReplies() {
    for (auto it = g_pending.begin(); it != g_pending.end();) {
        void*                 reply = nullptr;
        xcb_generic_error_t*  err   = nullptr;
        if (!it->extDone && xcb_poll_for_reply(g_conn, it->ext.sequence, &reply, &err)) {
            it->extDone  = true;
            it->extReply = (xcb_shape_query_extents_reply_t*)reply;
            free(err);
        }
        reply = nullptr;
        err   = nullptr;
        if (!it->rectsDone && xcb_poll_for_reply(g_conn, it->rects.sequence, &reply, &err)) {
            it->rectsDone  = true;
            it->rectsReply = (xcb_shape_get_rectangles_reply_t*)reply;
            free(err);
        }
        if (it->extDone && it->rectsDone) {
            applyShape(*it);
            free(it->extReply);
            free(it->rectsReply);
            it = g_pending.erase(it);
        } else
            ++it;
    }
}

static int onXEvent(int fd, uint32_t mask, void*) {
    if (mask & (WL_EVENT_HANGUP | WL_EVENT_ERROR)) {
        disconnect();
        return 0;
    }
    while (auto ev = xcb_poll_for_event(g_conn)) {
        const uint8_t type = ev->response_type & 0x7f;
        if (type == XCB_MAP_NOTIFY)
            requestShape(((xcb_map_notify_event_t*)ev)->window);
        else if (type == g_shapeEv + XCB_SHAPE_NOTIFY) {
            auto* s = (xcb_shape_notify_event_t*)ev;
            if (s->shape_kind == XCB_SHAPE_SK_BOUNDING)
                requestShape(s->affected_window);
        }
        free(ev);
    }
    if (xcb_connection_has_error(g_conn)) {
        disconnect();
        return 0;
    }
    pollReplies();
    xcb_flush(g_conn);
    return 0;
}

// Called on the first X11 window, when Xwayland is known to be up.
static void connect() {
    const char* display = getenv("DISPLAY");
    if (!display)
        return;
    g_conn = xcb_connect(display, nullptr);
    if (xcb_connection_has_error(g_conn)) {
        xcb_disconnect(g_conn);
        g_conn = nullptr;
        return;
    }
    const auto* ext = xcb_get_extension_data(g_conn, &xcb_shape_id);
    if (!ext || !ext->present) {
        xcb_disconnect(g_conn);
        g_conn = nullptr;
        return;
    }
    g_shapeEv = ext->first_event;

    xcb_screen_t* screen = xcb_setup_roots_iterator(xcb_get_setup(g_conn)).data;
    uint32_t      evmask = XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY;
    xcb_change_window_attributes(g_conn, screen->root, XCB_CW_EVENT_MASK, &evmask);

    g_source = wl_event_loop_add_fd(g_pCompositor->m_wlEventLoop, xcb_get_file_descriptor(g_conn), WL_EVENT_READABLE, onXEvent, nullptr);

    for (auto& w : allWindows()) {
        auto xs = w->m_isX11 ? w->m_xwaylandSurface.lock() : nullptr;
        if (xs && xs->m_xID)
            requestShape(xs->m_xID);
    }
    xcb_flush(g_conn);
}

static void onWindowOpen(PHLWINDOW w) {
    auto xs = w && w->m_isX11 ? w->m_xwaylandSurface.lock() : nullptr;
    if (!xs)
        return;
    if (!g_conn)
        connect();
    else
        requestShape(xs->m_xID);
}

APICALL EXPORT std::string PLUGIN_API_VERSION() {
    return HYPRLAND_API_VERSION;
}

APICALL EXPORT PLUGIN_DESCRIPTION_INFO PLUGIN_INIT(HANDLE handle) {
    PHANDLE = handle;

    if (__hyprland_api_get_hash() != std::string(__hyprland_api_get_client_hash())) {
        HyprlandAPI::addNotification(PHANDLE, "[hypr-xshape] Built for a different Hyprland version, not loading", CHyprColor{1.0, 0.2, 0.2, 1.0}, 5000);
        throw std::runtime_error("[hypr-xshape] version mismatch");
    }

#ifdef XSHAPE_EVENT_BUS
    g_openHook = Event::bus()->m_events.window.open.listen([](PHLWINDOW w) { onWindowOpen(w); });
#else
    g_openHook = HyprlandAPI::registerCallbackDynamic(PHANDLE, "openWindow", [](void*, SCallbackInfo&, std::any data) { onWindowOpen(std::any_cast<PHLWINDOW>(data)); });
#endif

    return {"hypr-xshape", "Clip XWayland windows to their X11 SHAPE bounding region", "sreckom", "0.1"};
}

APICALL EXPORT void PLUGIN_EXIT() {
    g_openHook.reset();
    disconnect();
    for (auto& w : allWindows()) {
        if (w->m_isX11 && w->wlSurface())
            w->wlSurface()->m_visibleRegion.clear();
    }
}
