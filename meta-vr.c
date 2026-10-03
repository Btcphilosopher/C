/*
 * spatial_desktop.c
 *
 * Native C foundation for a Meta Quest Spatial Desktop.
 *
 * Architecture:
 *   - Spatial 3D windows
 *   - Multiple workspaces
 *   - Window grabbing / movement
 *   - Window resizing
 *   - Window focus
 *   - Controller / hand abstraction
 *   - Spatial notifications
 *   - Desktop scene management
 *   - Application abstraction
 *
 * Intended next layer:
 *   OpenXR + Vulkan + Android NDK
 *
 * Compile desktop prototype:
 *   cc -std=c11 -Wall -Wextra -O2 spatial_desktop.c -lm -o spatial_desktop
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <math.h>
#include <time.h>

/* ============================================================
 * CONSTANTS
 * ============================================================ */

#define SD_MAX_WINDOWS        64
#define SD_MAX_WORKSPACES     8
#define SD_MAX_APPS           32
#define SD_MAX_NOTIFICATIONS  32
#define SD_MAX_INPUTS         16

#define SD_PI 3.14159265358979323846f

/* ============================================================
 * BASIC MATH
 * ============================================================ */

typedef struct {
    float x;
    float y;
    float z;
} SD_Vec3;

typedef struct {
    float x;
    float y;
    float z;
    float w;
} SD_Quat;

typedef struct {
    float m[16];
} SD_Mat4;

static SD_Vec3 sd_vec3(float x, float y, float z)
{
    SD_Vec3 v = {x, y, z};
    return v;
}

static SD_Vec3 sd_vec3_add(SD_Vec3 a, SD_Vec3 b)
{
    return sd_vec3(a.x + b.x, a.y + b.y, a.z + b.z);
}

static SD_Vec3 sd_vec3_sub(SD_Vec3 a, SD_Vec3 b)
{
    return sd_vec3(a.x - b.x, a.y - b.y, a.z - b.z);
}

static SD_Vec3 sd_vec3_mul(SD_Vec3 a, float s)
{
    return sd_vec3(a.x * s, a.y * s, a.z * s);
}

static float sd_vec3_dot(SD_Vec3 a, SD_Vec3 b)
{
    return a.x*b.x + a.y*b.y + a.z*b.z;
}

static float sd_vec3_length(SD_Vec3 a)
{
    return sqrtf(sd_vec3_dot(a, a));
}

static SD_Vec3 sd_vec3_normalize(SD_Vec3 a)
{
    float l = sd_vec3_length(a);

    if (l < 0.000001f)
        return sd_vec3(0, 0, 0);

    return sd_vec3_mul(a, 1.0f / l);
}

static SD_Vec3 sd_vec3_lerp(SD_Vec3 a, SD_Vec3 b, float t)
{
    return sd_vec3(
        a.x + (b.x - a.x) * t,
        a.y + (b.y - a.y) * t,
        a.z + (b.z - a.z) * t
    );
}

/* ============================================================
 * QUATERNIONS
 * ============================================================ */

static SD_Quat sd_quat(float x, float y, float z, float w)
{
    SD_Quat q = {x, y, z, w};
    return q;
}

static SD_Quat sd_quat_identity(void)
{
    return sd_quat(0, 0, 0, 1);
}

static SD_Quat sd_quat_multiply(SD_Quat a, SD_Quat b)
{
    SD_Quat q;

    q.x = a.w*b.x + a.x*b.w + a.y*b.z - a.z*b.y;
    q.y = a.w*b.y - a.x*b.z + a.y*b.w + a.z*b.x;
    q.z = a.w*b.z + a.x*b.y - a.y*b.x + a.z*b.w;
    q.w = a.w*b.w - a.x*b.x - a.y*b.y - a.z*b.z;

    return q;
}

/* ============================================================
 * TRANSFORM
 * ============================================================ */

typedef struct {
    SD_Vec3 position;
    SD_Quat rotation;
    SD_Vec3 scale;
} SD_Transform;

static SD_Transform sd_transform_identity(void)
{
    SD_Transform t;

    t.position = sd_vec3(0, 0, 0);
    t.rotation = sd_quat_identity();
    t.scale = sd_vec3(1, 1, 1);

    return t;
}

/* ============================================================
 * RAY
 * ============================================================ */

typedef struct {
    SD_Vec3 origin;
    SD_Vec3 direction;
} SD_Ray;

/* ============================================================
 * INPUT
 * ============================================================ */

typedef enum {
    SD_INPUT_NONE = 0,
    SD_INPUT_LEFT_CONTROLLER,
    SD_INPUT_RIGHT_CONTROLLER,
    SD_INPUT_LEFT_HAND,
    SD_INPUT_RIGHT_HAND
} SD_InputSource;

typedef enum {
    SD_BUTTON_NONE = 0,
    SD_BUTTON_TRIGGER,
    SD_BUTTON_GRIP,
    SD_BUTTON_PRIMARY,
    SD_BUTTON_SECONDARY,
    SD_BUTTON_MENU
} SD_Button;

typedef struct {
    SD_InputSource source;

    SD_Vec3 position;
    SD_Quat rotation;

    SD_Ray pointer_ray;

    bool connected;

    bool trigger;
    bool grip;
    bool primary;
    bool secondary;
    bool menu;

    float trigger_value;
    float grip_value;

} SD_InputState;

/* ============================================================
 * WINDOW
 * ============================================================ */

typedef enum {
    SD_WINDOW_NORMAL = 0,
    SD_WINDOW_MINIMIZED,
    SD_WINDOW_MAXIMIZED,
    SD_WINDOW_HIDDEN
} SD_WindowState;

typedef struct {
    uint32_t id;

    char title[128];

    SD_Transform transform;

    float width;
    float height;

    float min_width;
    float min_height;

    float max_width;
    float max_height;

    SD_WindowState state;

    bool visible;
    bool focused;
    bool grabbed;
    bool resizing;

    int workspace;

    uint64_t last_interaction;

    void (*draw)(void *userdata);
    void (*update)(float dt, void *userdata);

    void *userdata;

} SD_Window;

/* ============================================================
 * APPLICATION
 * ============================================================ */

typedef struct {
    uint32_t id;

    char name[128];

    bool running;
    bool visible;

    void (*start)(void *);
    void (*update)(float, void *);
    void (*draw)(void *);
    void (*shutdown)(void *);

    void *userdata;

} SD_Application;

/* ============================================================
 * WORKSPACE
 * ============================================================ */

typedef struct {

    uint32_t id;

    char name[64];

    SD_Transform transform;

    uint32_t windows[SD_MAX_WINDOWS];

    uint32_t window_count;

    bool active;

} SD_Workspace;

/* ============================================================
 * NOTIFICATIONS
 * ============================================================ */

typedef struct {

    uint32_t id;

    char title[128];
    char message[512];

    float lifetime;
    float remaining;

    bool active;

} SD_Notification;

/* ============================================================
 * RENDERER ABSTRACTION
 * ============================================================ */

typedef struct {

    bool initialized;

    uint32_t width;
    uint32_t height;

    void (*begin_frame)(void *);
    void (*end_frame)(void *);

    void (*draw_panel)(
        void *,
        SD_Transform *,
        float,
        float
    );

    void (*draw_text)(
        void *,
        const char *,
        SD_Vec3,
        float
    );

    void *userdata;

} SD_Renderer;

/*
 * This deliberately doesn't contain Vulkan calls yet.
 *
 * The OpenXR/Vulkan implementation can populate these callbacks.
 */

/* ============================================================
 * ENGINE
 * ============================================================ */

typedef struct {

    bool running;

    float time;
    float delta_time;

    SD_Renderer renderer;

    SD_InputState inputs[SD_MAX_INPUTS];
    uint32_t input_count;

    SD_Window windows[SD_MAX_WINDOWS];
    uint32_t window_count;

    SD_Application applications[SD_MAX_APPS];
    uint32_t application_count;

    SD_Workspace workspaces[SD_MAX_WORKSPACES];
    uint32_t workspace_count;

    SD_Notification notifications[SD_MAX_NOTIFICATIONS];
    uint32_t notification_count;

    uint32_t focused_window;

    uint32_t grabbed_window;

    int active_workspace;

} SD_Engine;

/* ============================================================
 * GLOBAL ENGINE
 * ============================================================ */

static SD_Engine g_engine;

/* ============================================================
 * TIME
 * ============================================================ */

static uint64_t sd_now_ms(void)
{
    return (uint64_t)(clock() * 1000 / CLOCKS_PER_SEC);
}

/* ============================================================
 * ENGINE INITIALIZATION
 * ============================================================ */

static void sd_engine_init(SD_Engine *engine)
{
    memset(engine, 0, sizeof(*engine));

    engine->running = true;
    engine->focused_window = UINT32_MAX;
    engine->grabbed_window = UINT32_MAX;
    engine->active_workspace = 0;

    printf("Spatial Desktop engine initialized\n");
}

/* ============================================================
 * WORKSPACES
 * ============================================================ */

static int sd_workspace_create(
    SD_Engine *engine,
    const char *name
)
{
    if (engine->workspace_count >= SD_MAX_WORKSPACES)
        return -1;

    SD_Workspace *ws =
        &engine->workspaces[engine->workspace_count];

    memset(ws, 0, sizeof(*ws));

    ws->id = engine->workspace_count;

    strncpy(
        ws->name,
        name,
        sizeof(ws->name) - 1
    );

    ws->transform = sd_transform_identity();

    ws->active =
        engine->workspace_count == 0;

    engine->workspace_count++;

    return (int)ws->id;
}

static void sd_workspace_switch(
    SD_Engine *engine,
    int workspace
)
{
    if (workspace < 0 ||
        workspace >= (int)engine->workspace_count)
        return;

    for (uint32_t i = 0;
         i < engine->workspace_count;
         ++i)
    {
        engine->workspaces[i].active =
            ((int)i == workspace);
    }

    engine->active_workspace = workspace;

    printf(
        "Switched to workspace: %s\n",
        engine->workspaces[workspace].name
    );
}

/* ============================================================
 * WINDOW CREATION
 * ============================================================ */

static uint32_t sd_window_create(
    SD_Engine *engine,
    const char *title,
    float width,
    float height,
    SD_Vec3 position
)
{
    if (engine->window_count >= SD_MAX_WINDOWS)
        return UINT32_MAX;

    SD_Window *window =
        &engine->windows[engine->window_count];

    memset(window, 0, sizeof(*window));

    window->id = engine->window_count;

    strncpy(
        window->title,
        title,
        sizeof(window->title) - 1
    );

    window->transform =
        sd_transform_identity();

    window->transform.position = position;

    window->width = width;
    window->height = height;

    window->min_width = 0.2f;
    window->min_height = 0.1f;

    window->max_width = 5.0f;
    window->max_height = 5.0f;

    window->state = SD_WINDOW_NORMAL;

    window->visible = true;

    window->workspace =
        engine->active_workspace;

    engine->window_count++;

    SD_Workspace *ws =
        &engine->workspaces[engine->active_workspace];

    if (ws->window_count < SD_MAX_WINDOWS)
    {
        ws->windows[ws->window_count++] =
            window->id;
    }

    printf(
        "Created spatial window: %s\n",
        window->title
    );

    return window->id;
}

/* ============================================================
 * WINDOW LOOKUP
 * ============================================================ */

static SD_Window *sd_window_get(
    SD_Engine *engine,
    uint32_t id
)
{
    if (id >= engine->window_count)
        return NULL;

    return &engine->windows[id];
}

/* ============================================================
 * WINDOW FOCUS
 * ============================================================ */

static void sd_window_focus(
    SD_Engine *engine,
    uint32_t id
)
{
    SD_Window *window =
        sd_window_get(engine, id);

    if (!window)
        return;

    if (!window->visible)
        return;

    for (uint32_t i = 0;
         i < engine->window_count;
         ++i)
    {
        engine->windows[i].focused = false;
    }

    window->focused = true;

    engine->focused_window = id;

    window->last_interaction =
        sd_now_ms();

    printf(
        "Focused window: %s\n",
        window->title
    );
}

/* ============================================================
 * WINDOW MOVE
 * ============================================================ */

static void sd_window_move(
    SD_Window *window,
    SD_Vec3 position
)
{
    if (!window)
        return;

    window->transform.position =
        position;
}

/* ============================================================
 * WINDOW RESIZE
 * ============================================================ */

static void sd_window_resize(
    SD_Window *window,
    float width,
    float height
)
{
    if (!window)
        return;

    if (width < window->min_width)
        width = window->min_width;

    if (height < window->min_height)
        height = window->min_height;

    if (width > window->max_width)
        width = window->max_width;

    if (height > window->max_height)
        height = window->max_height;

    window->width = width;
    window->height = height;
}

/* ============================================================
 * MINIMIZE
 * ============================================================ */

static void sd_window_minimize(
    SD_Window *window
)
{
    if (!window)
        return;

    window->state =
        SD_WINDOW_MINIMIZED;

    window->visible = false;
}

/* ============================================================
 * RESTORE
 * ============================================================ */

static void sd_window_restore(
    SD_Window *window
)
{
    if (!window)
        return;

    window->state =
        SD_WINDOW_NORMAL;

    window->visible = true;
}

/* ============================================================
 * MAXIMIZE
 * ============================================================ */

static void sd_window_maximize(
    SD_Window *window
)
{
    if (!window)
        return;

    window->state =
        SD_WINDOW_MAXIMIZED;

    window->transform.position =
        sd_vec3(0, 0, -2.0f);

    window->width = 4.0f;
    window->height = 2.3f;
}

/* ============================================================
 * RAY / WINDOW INTERSECTION
 *
 * Windows are treated as rectangular planes.
 * ============================================================ */

static bool sd_ray_window_intersection(
    SD_Ray ray,
    SD_Window *window,
    float *distance
)
{
    /*
     * Simplified window plane:
     *
     * local plane lies at z = 0
     * normal = (0,0,1)
     *
     * Full production implementation should transform
     * the ray into window-local coordinates using the
     * inverse window transform.
     */

    float dz = ray.direction.z;

    if (fabsf(dz) < 0.00001f)
        return false;

    float t =
        -ray.origin.z / dz;

    if (t < 0)
        return false;

    SD_Vec3 hit =
        sd_vec3_add(
            ray.origin,
            sd_vec3_mul(ray.direction, t)
        );

    float half_w =
        window->width * 0.5f;

    float half_h =
        window->height * 0.5f;

    if (hit.x < -half_w ||
        hit.x > half_w)
        return false;

    if (hit.y < -half_h ||
        hit.y > half_h)
        return false;

    if (distance)
        *distance = t;

    return true;
}

/* ============================================================
 * FIND WINDOW UNDER POINTER
 * ============================================================ */

static int sd_window_pick(
    SD_Engine *engine,
    SD_Ray ray
)
{
    float closest = 999999.0f;

    int selected = -1;

    for (uint32_t i = 0;
         i < engine->window_count;
         ++i)
    {
        SD_Window *window =
            &engine->windows[i];

        if (!window->visible)
            continue;

        if (window->workspace !=
            engine->active_workspace)
            continue;

        float distance;

        if (sd_ray_window_intersection(
                ray,
                window,
                &distance))
        {
            if (distance < closest)
            {
                closest = distance;
                selected = (int)i;
            }
        }
    }

    return selected;
}

/* ============================================================
 * GRABBING
 * ============================================================ */

static void sd_begin_grab(
    SD_Engine *engine,
    uint32_t window_id
)
{
    SD_Window *window =
        sd_window_get(engine, window_id);

    if (!window)
        return;

    sd_window_focus(engine, window_id);

    window->grabbed = true;

    engine->grabbed_window =
        window_id;

    printf(
        "Grabbed: %s\n",
        window->title
    );
}

static void sd_end_grab(
    SD_Engine *engine
)
{
    if (engine->grabbed_window ==
        UINT32_MAX)
        return;

    SD_Window *window =
        sd_window_get(
            engine,
            engine->grabbed_window
        );

    if (window)
        window->grabbed = false;

    engine->grabbed_window =
        UINT32_MAX;
}

/* ============================================================
 * INPUT UPDATE
 * ============================================================ */

static void sd_input_update(
    SD_Engine *engine
)
{
    for (uint32_t i = 0;
         i < engine->input_count;
         ++i)
    {
        SD_InputState *input =
            &engine->inputs[i];

        if (!input->connected)
            continue;

        /*
         * Production version:
         *
         * OpenXR action states would be read here.
         *
         * Example actions:
         *
         * /user/hand/left/input/trigger/value
         * /user/hand/right/input/trigger/value
         * /user/hand/left/input/grip/value
         * /user/hand/right/input/grip/value
         */
    }
}

/* ============================================================
 * SPATIAL WINDOW UPDATE
 * ============================================================ */

static void sd_update_windows(
    SD_Engine *engine,
    float dt
)
{
    for (uint32_t i = 0;
         i < engine->window_count;
         ++i)
    {
        SD_Window *window =
            &engine->windows[i];

        if (!window->visible)
            continue;

        if (window->workspace !=
            engine->active_workspace)
            continue;

        if (window->update)
            window->update(
                dt,
                window->userdata
            );
    }
}

/* ============================================================
 * NOTIFICATIONS
 * ============================================================ */

static uint32_t sd_notification_create(
    SD_Engine *engine,
    const char *title,
    const char *message,
    float lifetime
)
{
    if (engine->notification_count >=
        SD_MAX_NOTIFICATIONS)
        return UINT32_MAX;

    SD_Notification *n =
        &engine->notifications[
            engine->notification_count
        ];

    memset(n, 0, sizeof(*n));

    n->id =
        engine->notification_count;

    strncpy(
        n->title,
        title,
        sizeof(n->title) - 1
    );

    strncpy(
        n->message,
        message,
        sizeof(n->message) - 1
    );

    n->lifetime = lifetime;
    n->remaining = lifetime;
    n->active = true;

    engine->notification_count++;

    return n->id;
}

static void sd_update_notifications(
    SD_Engine *engine,
    float dt
)
{
    for (uint32_t i = 0;
         i < engine->notification_count;
         ++i)
    {
        SD_Notification *n =
            &engine->notifications[i];

        if (!n->active)
            continue;

        n->remaining -= dt;

        if (n->remaining <= 0)
        {
            n->remaining = 0;
            n->active = false;
        }
    }
}

/* ============================================================
 * DESKTOP SYSTEM BAR
 * ============================================================ */

typedef struct {

    bool visible;

    float width;
    float height;

    SD_Transform transform;

} SD_SystemBar;

static SD_SystemBar g_system_bar;

static void sd_system_bar_init(void)
{
    g_system_bar.visible = true;

    g_system_bar.width = 3.0f;
    g_system_bar.height = 0.12f;

    g_system_bar.transform =
        sd_transform_identity();

    /*
     * Place bar slightly below the user's
     * forward-facing field of view.
     */

    g_system_bar.transform.position =
        sd_vec3(
            0,
            -1.0f,
            -2.0f
        );
}

/* ============================================================
 * APPLICATION LAUNCHER
 * ============================================================ */

static int sd_application_register(
    SD_Engine *engine,
    const char *name,
    void (*start)(void *),
    void (*update)(float, void *),
    void (*draw)(void *),
    void (*shutdown)(void *),
    void *userdata
)
{
    if (engine->application_count >=
        SD_MAX_APPS)
        return -1;

    SD_Application *app =
        &engine->applications[
            engine->application_count
        ];

    memset(app, 0, sizeof(*app));

    app->id =
        engine->application_count;

    strncpy(
        app->name,
        name,
        sizeof(app->name) - 1
    );

    app->start = start;
    app->update = update;
    app->draw = draw;
    app->shutdown = shutdown;
    app->userdata = userdata;

    engine->application_count++;

    return (int)app->id;
}

static void sd_application_launch(
    SD_Engine *engine,
    uint32_t application_id
)
{
    if (application_id >=
        engine->application_count)
        return;

    SD_Application *app =
        &engine->applications[
            application_id
        ];

    if (app->running)
        return;

    app->running = true;
    app->visible = true;

    if (app->start)
        app->start(app->userdata);

    printf(
        "Application launched: %s\n",
        app->name
    );
}

/* ============================================================
 * WINDOW LAYOUT
 * ============================================================ */

static void sd_layout_arc(
    SD_Engine *engine
)
{
    /*
     * Arrange windows around the user
     * like a curved spatial monitor wall.
     */

    int visible_count = 0;

    for (uint32_t i = 0;
         i < engine->window_count;
         ++i)
    {
        SD_Window *w =
            &engine->windows[i];

        if (!w->visible)
            continue;

        if (w->workspace !=
            engine->active_workspace)
            continue;

        visible_count++;
    }

    if (visible_count == 0)
        return;

    float radius = 2.2f;

    int index = 0;

    for (uint32_t i = 0;
         i < engine->window_count;
         ++i)
    {
        SD_Window *w =
            &engine->windows[i];

        if (!w->visible)
            continue;

        if (w->workspace !=
            engine->active_workspace)
            continue;

        float t;

        if (visible_count == 1)
            t = 0.0f;
        else
            t =
                (float)index /
                (float)(visible_count - 1);

        float angle =
            (-45.0f + 90.0f * t)
            * SD_PI / 180.0f;

        w->transform.position =
            sd_vec3(
                sinf(angle) * radius,
                1.4f,
                -cosf(angle) * radius
            );

        index++;
    }
}

/* ============================================================
 * DESKTOP RENDER
 * ============================================================ */

static void sd_render_desktop(
    SD_Engine *engine
)
{
    SD_Renderer *renderer =
        &engine->renderer;

    if (!renderer->initialized)
        return;

    if (renderer->begin_frame)
        renderer->begin_frame(
            renderer->userdata
        );

    /*
     * Windows
     */

    for (uint32_t i = 0;
         i < engine->window_count;
         ++i)
    {
        SD_Window *window =
            &engine->windows[i];

        if (!window->visible)
            continue;

        if (window->workspace !=
            engine->active_workspace)
            continue;

        if (renderer->draw_panel)
        {
            renderer->draw_panel(
                renderer->userdata,
                &window->transform,
                window->width,
                window->height
            );
        }

        if (window->draw)
        {
            window->draw(
                window->userdata
            );
        }
    }

    /*
     * System bar
     */

    if (g_system_bar.visible &&
        renderer->draw_panel)
    {
        renderer->draw_panel(
            renderer->userdata,
            &g_system_bar.transform,
            g_system_bar.width,
            g_system_bar.height
        );
    }

    /*
     * Notifications
     */

    float notification_y = 1.7f;

    for (uint32_t i = 0;
         i < engine->notification_count;
         ++i)
    {
        SD_Notification *n =
            &engine->notifications[i];

        if (!n->active)
            continue;

        if (renderer->draw_text)
        {
            SD_Vec3 position =
                sd_vec3(
                    0,
                    notification_y,
                    -2.0f
                );

            renderer->draw_text(
                renderer->userdata,
                n->title,
                position,
                0.04f
            );

            notification_y -= 0.15f;
        }
    }

    if (renderer->end_frame)
        renderer->end_frame(
            renderer->userdata
        );
}

/* ============================================================
 * ENGINE UPDATE
 * ============================================================ */

static void sd_engine_update(
    SD_Engine *engine,
    float dt
)
{
    engine->delta_time = dt;
    engine->time += dt;

    sd_input_update(engine);

    sd_update_windows(
        engine,
        dt
    );

    sd_update_notifications(
        engine,
        dt
    );

    for (uint32_t i = 0;
         i < engine->application_count;
         ++i)
    {
        SD_Application *app =
            &engine->applications[i];

        if (!app->running)
            continue;

        if (app->update)
            app->update(
                dt,
                app->userdata
            );
    }
}

/* ============================================================
 * TEST APPLICATIONS
 * ============================================================ */

static void terminal_draw(void *userdata)
{
    (void)userdata;

    printf(
        "[Terminal] drawing spatial terminal\n"
    );
}

static void browser_draw(void *userdata)
{
    (void)userdata;

    printf(
        "[Browser] drawing spatial browser\n"
    );
}

static void files_draw(void *userdata)
{
    (void)userdata;

    printf(
        "[Files] drawing spatial filesystem\n"
    );
}

/* ============================================================
 * DESKTOP SETUP
 * ============================================================ */

static void sd_create_default_desktop(
    SD_Engine *engine
)
{
    /*
     * Create default workspaces.
     */

    sd_workspace_create(
        engine,
        "Home"
    );

    sd_workspace_create(
        engine,
        "Work"
    );

    sd_workspace_create(
        engine,
        "Media"
    );

    /*
     * Applications.
     */

    int terminal =
        sd_application_register(
            engine,
            "Terminal",
            NULL,
            NULL,
            terminal_draw,
            NULL,
            NULL
        );

    int browser =
        sd_application_register(
            engine,
            "Browser",
            NULL,
            NULL,
            browser_draw,
            NULL,
            NULL
        );

    int files =
        sd_application_register(
            engine,
            "Files",
            NULL,
            NULL,
            files_draw,
            NULL,
            NULL
        );

    /*
     * Spatial windows.
     */

    uint32_t terminal_window =
        sd_window_create(
            engine,
            "Terminal",
            1.4f,
            0.85f,
            sd_vec3(
                -0.9f,
                1.4f,
                -2.2f
            )
        );

    uint32_t browser_window =
        sd_window_create(
            engine,
            "Browser",
            1.8f,
            1.05f,
            sd_vec3(
                0,
                1.5f,
                -2.5f
            )
        );

    uint32_t files_window =
        sd_window_create(
            engine,
            "Files",
            1.3f,
            0.9f,
            sd_vec3(
                0.9f,
                1.35f,
                -2.2f
            )
        );

    (void)terminal_window;
    (void)browser_window;
    (void)files_window;

    /*
     * Launch applications.
     */

    if (terminal >= 0)
        sd_application_launch(
            engine,
            (uint32_t)terminal
        );

    if (browser >= 0)
        sd_application_launch(
            engine,
            (uint32_t)browser
        );

    if (files >= 0)
        sd_application_launch(
            engine,
            (uint32_t)files
        );

    /*
     * Spatial notification.
     */

    sd_notification_create(
        engine,
        "Spatial Desktop",
        "Welcome to your spatial workspace.",
        5.0f
    );

    sd_system_bar_init();

    sd_layout_arc(engine);
}

/* ============================================================
 * CONSOLE DEBUG INTERFACE
 * ============================================================ */

static void sd_debug_print(
    SD_Engine *engine
)
{
    printf("\n");
    printf("====================================\n");
    printf("       SPATIAL DESKTOP STATUS       \n");
    printf("====================================\n");

    printf(
        "Time: %.2f\n",
        engine->time
    );

    printf(
        "Workspace: %d / %u\n",
        engine->active_workspace,
        engine->workspace_count
    );

    printf(
        "Windows: %u\n",
        engine->window_count
    );

    printf(
        "Applications: %u\n",
        engine->application_count
    );

    printf(
        "Notifications: %u\n",
        engine->notification_count
    );

    printf("\nWindows:\n");

    for (uint32_t i = 0;
         i < engine->window_count;
         ++i)
    {
        SD_Window *w =
            &engine->windows[i];

        printf(
            "  [%u] %-20s "
            "position=(%.2f %.2f %.2f) "
            "size=(%.2f %.2f) "
            "visible=%d focused=%d\n",
            w->id,
            w->title,
            w->transform.position.x,
            w->transform.position.y,
            w->transform.position.z,
            w->width,
            w->height,
            w->visible,
            w->focused
        );
    }

    printf("====================================\n");
}

/* ============================================================
 * DEMONSTRATION
 * ============================================================ */

static void sd_demo_interaction(
    SD_Engine *engine
)
{
    /*
     * Simulate selecting the browser.
     */

    if (engine->window_count < 2)
        return;

    sd_window_focus(
        engine,
        1
    );

    /*
     * Simulate resizing it.
     */

    SD_Window *browser =
        sd_window_get(
            engine,
            1
        );

    if (browser)
    {
        sd_window_resize(
            browser,
            2.2f,
            1.25f
        );
    }

    /*
     * Simulate grabbing and moving.
     */

    sd_begin_grab(
        engine,
        1
    );

    if (browser)
    {
        sd_window_move(
            browser,
            sd_vec3(
                0.15f,
                1.45f,
                -2.3f
            )
        );
    }

    sd_end_grab(engine);
}

/* ============================================================
 * SHUTDOWN
 * ============================================================ */

static void sd_engine_shutdown(
    SD_Engine *engine
)
{
    for (uint32_t i = 0;
         i < engine->application_count;
         ++i)
    {
        SD_Application *app =
            &engine->applications[i];

        if (app->running &&
            app->shutdown)
        {
            app->shutdown(
                app->userdata
            );
        }
    }

    engine->running = false;

    printf(
        "Spatial Desktop shutdown\n"
    );
}

/* ============================================================
 * MAIN
 * ============================================================ */

int main(void)
{
    SD_Engine *engine =
        &g_engine;

    sd_engine_init(engine);

    sd_create_default_desktop(
        engine
    );

    sd_debug_print(engine);

    /*
     * Demonstrate interaction.
     */

    sd_demo_interaction(engine);

    sd_debug_print(engine);

    /*
     * Main engine loop.
     *
     * The real Quest version replaces this
     * with an OpenXR frame loop.
     */

    const float dt =
        1.0f / 72.0f;

    for (int frame = 0;
         frame < 300;
         ++frame)
    {
        sd_engine_update(
            engine,
            dt
        );

        sd_render_desktop(
            engine
        );
    }

    sd_engine_shutdown(
        engine
    );

    return 0;
}



```c
/*
 * vr_engineering_lab.c
 *
 * Native C foundation for a VR Engineering Laboratory.
 *
 * Concept:
 *   A room-scale engineering laboratory where the user can:
 *
 *   - inspect machines at 1:1 scale
 *   - grab components
 *   - move components
 *   - rotate components
 *   - disassemble assemblies
 *   - assemble components
 *   - operate mechanisms
 *   - inspect dimensions
 *   - measure distances
 *   - inspect forces / torque
 *   - manipulate engineering tools
 *   - simulate machines
 *
 * Target architecture:
 *
 *   Meta Quest
 *   OpenXR
 *   Vulkan
 *   Android NDK
 *
 * This file contains the core simulation/application layer.
 * OpenXR and Vulkan can be attached through the renderer/input
 * interfaces.
 *
 * Compile desktop simulation:
 *
 *   cc -std=c11 -Wall -Wextra -O2 vr_engineering_lab.c \
 *      -lm -o vr_engineering_lab
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <math.h>
#include <time.h>

/* ============================================================
 * CONSTANTS
 * ============================================================ */

#define LAB_MAX_OBJECTS       512
#define LAB_MAX_MACHINES      64
#define LAB_MAX_TOOLS         64
#define LAB_MAX_MEASUREMENTS  128
#define LAB_MAX_JOINTS        256
#define LAB_MAX_INPUTS        8
#define LAB_MAX_SCENES        32

#define LAB_GRAVITY 9.80665f
#define LAB_PI 3.14159265358979323846f

/* ============================================================
 * VECTOR
 * ============================================================ */

typedef struct
{
    float x;
    float y;
    float z;

} Vec3;

static Vec3 vec3(
    float x,
    float y,
    float z)
{
    Vec3 v = {x, y, z};
    return v;
}

static Vec3 vec3_add(
    Vec3 a,
    Vec3 b)
{
    return vec3(
        a.x + b.x,
        a.y + b.y,
        a.z + b.z
    );
}

static Vec3 vec3_sub(
    Vec3 a,
    Vec3 b)
{
    return vec3(
        a.x - b.x,
        a.y - b.y,
        a.z - b.z
    );
}

static Vec3 vec3_mul(
    Vec3 a,
    float s)
{
    return vec3(
        a.x * s,
        a.y * s,
        a.z * s
    );
}

static float vec3_dot(
    Vec3 a,
    Vec3 b)
{
    return
        a.x * b.x +
        a.y * b.y +
        a.z * b.z;
}

static Vec3 vec3_cross(
    Vec3 a,
    Vec3 b)
{
    return vec3(
        a.y*b.z - a.z*b.y,
        a.z*b.x - a.x*b.z,
        a.x*b.y - a.y*b.x
    );
}

static float vec3_length(Vec3 a)
{
    return sqrtf(
        vec3_dot(a, a)
    );
}

static Vec3 vec3_normalize(Vec3 a)
{
    float length =
        vec3_length(a);

    if (length < 0.000001f)
        return vec3(0, 0, 0);

    return vec3_mul(
        a,
        1.0f / length
    );
}

/* ============================================================
 * QUATERNION
 * ============================================================ */

typedef struct
{
    float x;
    float y;
    float z;
    float w;

} Quat;

static Quat quat_identity(void)
{
    Quat q = {
        0,
        0,
        0,
        1
    };

    return q;
}

static Quat quat_multiply(
    Quat a,
    Quat b)
{
    Quat q;

    q.x =
        a.w*b.x +
        a.x*b.w +
        a.y*b.z -
        a.z*b.y;

    q.y =
        a.w*b.y -
        a.x*b.z +
        a.y*b.w +
        a.z*b.x;

    q.z =
        a.w*b.z +
        a.x*b.y -
        a.y*b.x +
        a.z*b.w;

    q.w =
        a.w*b.w -
        a.x*b.x -
        a.y*b.y -
        a.z*b.z;

    return q;
}

/* ============================================================
 * TRANSFORM
 * ============================================================ */

typedef struct
{
    Vec3 position;
    Quat rotation;
    Vec3 scale;

} Transform;

static Transform transform_identity(void)
{
    Transform t;

    t.position = vec3(
        0,
        0,
        0
    );

    t.rotation =
        quat_identity();

    t.scale = vec3(
        1,
        1,
        1
    );

    return t;
}

/* ============================================================
 * OBJECT TYPES
 * ============================================================ */

typedef enum
{
    OBJECT_NONE = 0,

    OBJECT_MACHINE,

    OBJECT_COMPONENT,

    OBJECT_TOOL,

    OBJECT_FASTENER,

    OBJECT_GEAR,

    OBJECT_SHAFT,

    OBJECT_BEARING,

    OBJECT_MOTOR,

    OBJECT_PISTON,

    OBJECT_CYLINDER,

    OBJECT_PANEL,

    OBJECT_SENSOR,

    OBJECT_WORKBENCH

} ObjectType;

/* ============================================================
 * PHYSICS BODY
 * ============================================================ */

typedef struct
{
    bool dynamic;
    bool gravity;

    float mass;

    Vec3 velocity;
    Vec3 angular_velocity;

    Vec3 force;
    Vec3 torque;

    float linear_damping;
    float angular_damping;

} PhysicsBody;

/* ============================================================
 * ENGINEERING OBJECT
 * ============================================================ */

typedef struct
{
    uint32_t id;

    char name[128];

    ObjectType type;

    Transform transform;

    PhysicsBody physics;

    Vec3 dimensions;

    float temperature;

    float stress;

    float strain;

    float pressure;

    float rotational_speed;

    float torque;

    bool visible;

    bool selectable;

    bool grabbed;

    bool installed;

    bool damaged;

    int parent_object;

} EngineeringObject;

/* ============================================================
 * JOINT
 * ============================================================ */

typedef enum
{
    JOINT_FIXED = 0,
    JOINT_REVOLUTE,
    JOINT_PRISMATIC,
    JOINT_BALL,
    JOINT_SPRING

} JointType;

typedef struct
{
    uint32_t id;

    uint32_t object_a;
    uint32_t object_b;

    JointType type;

    Vec3 axis;

    float position;
    float velocity;

    float minimum;
    float maximum;

    float stiffness;
    float damping;

    bool locked;

} EngineeringJoint;

/* ============================================================
 * MACHINE
 * ============================================================ */

typedef struct
{
    uint32_t id;

    char name[128];

    uint32_t object_ids[LAB_MAX_OBJECTS];

    uint32_t object_count;

    uint32_t joint_ids[LAB_MAX_JOINTS];

    uint32_t joint_count;

    bool running;

    float operating_speed;

    float efficiency;

    float power;

} EngineeringMachine;

/* ============================================================
 * TOOLS
 * ============================================================ */

typedef enum
{
    TOOL_NONE = 0,

    TOOL_MEASURING_TAPE,

    TOOL_CALIPER,

    TOOL_RULER,

    TOOL_TORQUE_WRENCH,

    TOOL_SCREWDRIVER,

    TOOL_SPANNER,

    TOOL_MALLET,

    TOOL_MULTIMETER,

    TOOL_THERMOMETER

} ToolType;

typedef struct
{
    uint32_t id;

    char name[64];

    ToolType type;

    float measurement;

    float accuracy;

    bool active;

} EngineeringTool;

/* ============================================================
 * MEASUREMENT
 * ============================================================ */

typedef enum
{
    MEASURE_DISTANCE = 0,
    MEASURE_ANGLE,
    MEASURE_FORCE,
    MEASURE_TORQUE,
    MEASURE_TEMPERATURE,
    MEASURE_PRESSURE,
    MEASURE_SPEED

} MeasurementType;

typedef struct
{
    uint32_t id;

    MeasurementType type;

    uint32_t object_a;
    uint32_t object_b;

    float value;

    char units[16];

} Measurement;

/* ============================================================
 * INPUT
 * ============================================================ */

typedef enum
{
    INPUT_LEFT = 0,
    INPUT_RIGHT

} InputHand;

typedef struct
{
    bool connected;

    InputHand hand;

    Vec3 position;

    Quat rotation;

    bool trigger;
    bool grip;

    float trigger_value;
    float grip_value;

    bool primary;
    bool secondary;

} VRInput;

/* ============================================================
 * RAY
 * ============================================================ */

typedef struct
{
    Vec3 origin;
    Vec3 direction;

} Ray;

/* ============================================================
 * RENDERER
 * ============================================================ */

typedef struct
{
    bool initialized;

    void (*begin_frame)(void *);
    void (*end_frame)(void *);

    void (*draw_object)(
        void *,
        EngineeringObject *
    );

    void (*draw_line)(
        void *,
        Vec3,
        Vec3
    );

    void (*draw_text)(
        void *,
        const char *,
        Vec3,
        float
    );

    void *userdata;

} LabRenderer;

/* ============================================================
 * LAB
 * ============================================================ */

typedef struct
{
    bool running;

    float simulation_time;

    EngineeringObject objects[
        LAB_MAX_OBJECTS
    ];

    uint32_t object_count;

    EngineeringJoint joints[
        LAB_MAX_JOINTS
    ];

    uint32_t joint_count;

    EngineeringMachine machines[
        LAB_MAX_MACHINES
    ];

    uint32_t machine_count;

    EngineeringTool tools[
        LAB_MAX_TOOLS
    ];

    uint32_t tool_count;

    Measurement measurements[
        LAB_MAX_MEASUREMENTS
    ];

    uint32_t measurement_count;

    VRInput inputs[
        LAB_MAX_INPUTS
    ];

    uint32_t input_count;

    LabRenderer renderer;

} EngineeringLab;

/* ============================================================
 * OBJECT CREATION
 * ============================================================ */

static uint32_t lab_create_object(
    EngineeringLab *lab,
    const char *name,
    ObjectType type,
    Vec3 position,
    Vec3 dimensions,
    float mass)
{
    if (lab->object_count >=
        LAB_MAX_OBJECTS)
        return UINT32_MAX;

    EngineeringObject *object =
        &lab->objects[
            lab->object_count
        ];

    memset(
        object,
        0,
        sizeof(*object)
    );

    object->id =
        lab->object_count;

    strncpy(
        object->name,
        name,
        sizeof(object->name) - 1
    );

    object->type = type;

    object->transform =
        transform_identity();

    object->transform.position =
        position;

    object->dimensions =
        dimensions;

    object->physics.mass =
        mass;

    object->physics.dynamic =
        mass > 0;

    object->physics.gravity = true;

    object->physics.linear_damping =
        0.05f;

    object->physics.angular_damping =
        0.05f;

    object->temperature =
        20.0f;

    object->visible = true;
    object->selectable = true;

    object->parent_object = -1;

    lab->object_count++;

    return object->id;
}

/* ============================================================
 * OBJECT LOOKUP
 * ============================================================ */

static EngineeringObject *lab_get_object(
    EngineeringLab *lab,
    uint32_t id)
{
    if (id >= lab->object_count)
        return NULL;

    return &lab->objects[id];
}

/* ============================================================
 * APPLY FORCE
 * ============================================================ */

static void lab_apply_force(
    EngineeringObject *object,
    Vec3 force)
{
    if (!object)
        return;

    if (!object->physics.dynamic)
        return;

    object->physics.force =
        vec3_add(
            object->physics.force,
            force
        );
}

/* ============================================================
 * APPLY TORQUE
 * ============================================================ */

static void lab_apply_torque(
    EngineeringObject *object,
    Vec3 torque)
{
    if (!object)
        return;

    if (!object->physics.dynamic)
        return;

    object->physics.torque =
        vec3_add(
            object->physics.torque,
            torque
        );
}

/* ============================================================
 * PHYSICS
 * ============================================================ */

static void lab_integrate_object(
    EngineeringObject *object,
    float dt)
{
    if (!object)
        return;

    if (!object->physics.dynamic)
        return;

    float mass =
        object->physics.mass;

    if (mass <= 0.00001f)
        return;

    Vec3 acceleration =
        vec3_mul(
            object->physics.force,
            1.0f / mass
        );

    if (object->physics.gravity)
    {
        acceleration.y -=
            LAB_GRAVITY;
    }

    object->physics.velocity =
        vec3_add(
            object->physics.velocity,
            vec3_mul(
                acceleration,
                dt
            )
        );

    object->physics.velocity =
        vec3_mul(
            object->physics.velocity,
            1.0f -
            object->physics.linear_damping *
            dt
        );

    object->transform.position =
        vec3_add(
            object->transform.position,
            vec3_mul(
                object->physics.velocity,
                dt
            )
        );

    object->physics.force =
        vec3(0, 0, 0);

    object->physics.torque =
        vec3(0, 0, 0);
}

/* ============================================================
 * GROUND COLLISION
 * ============================================================ */

static void lab_ground_collision(
    EngineeringObject *object)
{
    if (!object)
        return;

    float half_height =
        object->dimensions.y *
        0.5f;

    float ground =
        half_height;

    if (object->transform.position.y <
        ground)
    {
        object->transform.position.y =
            ground;

        if (object->physics.velocity.y < 0)
        {
            object->physics.velocity.y *=
                -0.25f;
        }
    }
}

/* ============================================================
 * JOINT CREATION
 * ============================================================ */

static uint32_t lab_create_joint(
    EngineeringLab *lab,
    uint32_t a,
    uint32_t b,
    JointType type,
    Vec3 axis)
{
    if (lab->joint_count >=
        LAB_MAX_JOINTS)
        return UINT32_MAX;

    EngineeringJoint *joint =
        &lab->joints[
            lab->joint_count
        ];

    memset(
        joint,
        0,
        sizeof(*joint)
    );

    joint->id =
        lab->joint_count;

    joint->object_a = a;
    joint->object_b = b;

    joint->type = type;

    joint->axis =
        vec3_normalize(axis);

    joint->minimum = -1000.0f;
    joint->maximum = 1000.0f;

    joint->stiffness = 100.0f;
    joint->damping = 5.0f;

    lab->joint_count++;

    return joint->id;
}

/* ============================================================
 * REVOLUTE JOINT
 * ============================================================ */

static void lab_update_revolute_joint(
    EngineeringLab *lab,
    EngineeringJoint *joint,
    float dt)
{
    if (!joint->locked)
    {
        joint->position +=
            joint->velocity *
            dt;

        if (joint->position <
            joint->minimum)
        {
            joint->position =
                joint->minimum;

            joint->velocity *=
                -0.25f;
        }

        if (joint->position >
            joint->maximum)
        {
            joint->position =
                joint->maximum;

            joint->velocity *=
                -0.25f;
        }
    }

    EngineeringObject *a =
        lab_get_object(
            lab,
            joint->object_a
        );

    EngineeringObject *b =
        lab_get_object(
            lab,
            joint->object_b
        );

    if (!a || !b)
        return;

    /*
     * In a full rigid-body engine this would
     * use a constraint solver.
     *
     * Here we maintain a simplified mechanism
     * relationship.
     */

    (void)dt;
}

/* ============================================================
 * MACHINE CREATION
 * ============================================================ */

static uint32_t lab_create_machine(
    EngineeringLab *lab,
    const char *name)
{
    if (lab->machine_count >=
        LAB_MAX_MACHINES)
        return UINT32_MAX;

    EngineeringMachine *machine =
        &lab->machines[
            lab->machine_count
        ];

    memset(
        machine,
        0,
        sizeof(*machine)
    );

    machine->id =
        lab->machine_count;

    strncpy(
        machine->name,
        name,
        sizeof(machine->name) - 1
    );

    machine->efficiency = 1.0f;

    lab->machine_count++;

    return machine->id;
}

/* ============================================================
 * ADD OBJECT TO MACHINE
 * ============================================================ */

static void lab_machine_add_object(
    EngineeringLab *lab,
    uint32_t machine_id,
    uint32_t object_id)
{
    if (machine_id >=
        lab->machine_count)
        return;

    EngineeringMachine *machine =
        &lab->machines[machine_id];

    if (machine->object_count >=
        LAB_MAX_OBJECTS)
        return;

    machine->object_ids[
        machine->object_count++
    ] = object_id;

    EngineeringObject *object =
        lab_get_object(
            lab,
            object_id
        );

    if (object)
        object->installed = true;
}

/* ============================================================
 * MEASUREMENT
 * ============================================================ */

static uint32_t lab_measure_distance(
    EngineeringLab *lab,
    uint32_t a,
    uint32_t b)
{
    if (lab->measurement_count >=
        LAB_MAX_MEASUREMENTS)
        return UINT32_MAX;

    EngineeringObject *object_a =
        lab_get_object(lab, a);

    EngineeringObject *object_b =
        lab_get_object(lab, b);

    if (!object_a || !object_b)
        return UINT32_MAX;

    Measurement *measurement =
        &lab->measurements[
            lab->measurement_count
        ];

    memset(
        measurement,
        0,
        sizeof(*measurement)
    );

    measurement->id =
        lab->measurement_count;

    measurement->type =
        MEASURE_DISTANCE;

    measurement->object_a = a;
    measurement->object_b = b;

    measurement->value =
        vec3_length(
            vec3_sub(
                object_a->transform.position,
                object_b->transform.position
            )
        );

    strncpy(
        measurement->units,
        "m",
        sizeof(measurement->units) - 1
    );

    lab->measurement_count++;

    printf(
        "Measurement: %.4f m\n",
        measurement->value
    );

    return measurement->id;
}

/* ============================================================
 * RAYCAST
 * ============================================================ */

static bool lab_ray_box_intersection(
    Ray ray,
    EngineeringObject *object,
    float *distance)
{
    /*
     * Simplified axis-aligned bounding box.
     */

    Vec3 center =
        object->transform.position;

    Vec3 half =
        vec3_mul(
            object->dimensions,
            0.5f
        );

    float min_x =
        center.x - half.x;

    float max_x =
        center.x + half.x;

    float min_y =
        center.y - half.y;

    float max_y =
        center.y + half.y;

    float min_z =
        center.z - half.z;

    float max_z =
        center.z + half.z;

    float tmin =
        -INFINITY;

    float tmax =
        INFINITY;

    if (fabsf(ray.direction.x) < 0.00001f)
    {
        if (ray.origin.x < min_x ||
            ray.origin.x > max_x)
            return false;
    }
    else
    {
        float tx1 =
            (min_x - ray.origin.x) /
            ray.direction.x;

        float tx2 =
            (max_x - ray.origin.x) /
            ray.direction.x;

        if (tx1 > tx2)
        {
            float temp = tx1;
            tx1 = tx2;
            tx2 = temp;
        }

        if (tx1 > tmin)
            tmin = tx1;

        if (tx2 < tmax)
            tmax = tx2;
    }

    if (fabsf(ray.direction.y) < 0.00001f)
    {
        if (ray.origin.y < min_y ||
            ray.origin.y > max_y)
            return false;
    }
    else
    {
        float ty1 =
            (min_y - ray.origin.y) /
            ray.direction.y;

        float ty2 =
            (max_y - ray.origin.y) /
            ray.direction.y;

        if (ty1 > ty2)
        {
            float temp = ty1;
            ty1 = ty2;
            ty2 = temp;
        }

        if (ty1 > tmin)
            tmin = ty1;

        if (ty2 < tmax)
            tmax = ty2;
    }

    if (fabsf(ray.direction.z) < 0.00001f)
    {
        if (ray.origin.z < min_z ||
            ray.origin.z > max_z)
            return false;
    }
    else
    {
        float tz1 =
            (min_z - ray.origin.z) /
            ray.direction.z;

        float tz2 =
            (max_z - ray.origin.z) /
            ray.direction.z;

        if (tz1 > tz2)
        {
            float temp = tz1;
            tz1 = tz2;
            tz2 = temp;
        }

        if (tz1 > tmin)
            tmin = tz1;

        if (tz2 < tmax)
            tmax = tz2;
    }

    if (tmax < tmin)
        return false;

    if (tmax < 0)
        return false;

    if (distance)
        *distance =
            tmin >= 0 ?
            tmin :
            tmax;

    return true;
}

/* ============================================================
 * PICK OBJECT
 * ============================================================ */

static int lab_pick_object(
    EngineeringLab *lab,
    Ray ray)
{
    float closest =
        INFINITY;

    int selected = -1;

    for (uint32_t i = 0;
         i < lab->object_count;
         ++i)
    {
        EngineeringObject *object =
            &lab->objects[i];

        if (!object->visible ||
            !object->selectable)
            continue;

        float distance;

        if (lab_ray_box_intersection(
                ray,
                object,
                &distance))
        {
            if (distance < closest)
            {
                closest = distance;
                selected = (int)i;
            }
        }
    }

    return selected;
}

/* ============================================================
 * GRAB OBJECT
 * ============================================================ */

static void lab_grab_object(
    EngineeringLab *lab,
    uint32_t object_id)
{
    EngineeringObject *object =
        lab_get_object(
            lab,
            object_id
        );

    if (!object)
        return;

    object->grabbed = true;

    /*
     * Disable gravity while held.
     */

    object->physics.gravity =
        false;

    object->physics.velocity =
        vec3(0, 0, 0);

    printf(
        "Grabbed engineering object: %s\n",
        object->name
    );
}

/* ============================================================
 * RELEASE OBJECT
 * ============================================================ */

static void lab_release_object(
    EngineeringLab *lab,
    uint32_t object_id,
    Vec3 release_velocity)
{
    EngineeringObject *object =
        lab_get_object(
            lab,
            object_id
        );

    if (!object)
        return;

    object->grabbed = false;

    object->physics.gravity =
        true;

    object->physics.velocity =
        release_velocity;
}

/* ============================================================
 * TOOL CREATION
 * ============================================================ */

static uint32_t lab_create_tool(
    EngineeringLab *lab,
    const char *name,
    ToolType type,
    float accuracy)
{
    if (lab->tool_count >=
        LAB_MAX_TOOLS)
        return UINT32_MAX;

    EngineeringTool *tool =
        &lab->tools[
            lab->tool_count
        ];

    memset(
        tool,
        0,
        sizeof(*tool)
    );

    tool->id =
        lab->tool_count;

    strncpy(
        tool->name,
        name,
        sizeof(tool->name) - 1
    );

    tool->type = type;
    tool->accuracy = accuracy;

    lab->tool_count++;

    return tool->id;
}

/* ============================================================
 * CALIPER
 * ============================================================ */

static float lab_caliper_measure(
    EngineeringLab *lab,
    uint32_t tool_id,
    uint32_t object_id)
{
    if (tool_id >= lab->tool_count)
        return 0;

    EngineeringTool *tool =
        &lab->tools[tool_id];

    EngineeringObject *object =
        lab_get_object(
            lab,
            object_id
        );

    if (!object)
        return 0;

    if (tool->type !=
        TOOL_CALIPER)
        return 0;

    float measurement =
        object->dimensions.x;

    /*
     * Instrument noise model.
     */

    float noise =
        ((float)rand() /
         (float)RAND_MAX - 0.5f)
         * tool->accuracy;

    measurement += noise;

    tool->measurement =
        measurement;

    return measurement;
}

/* ============================================================
 * TORQUE WRENCH
 * ============================================================ */

static void lab_apply_torque_wrench(
    EngineeringLab *lab,
    uint32_t object_id,
    float torque)
{
    EngineeringObject *object =
        lab_get_object(
            lab,
            object_id
        );

    if (!object)
        return;

    object->torque =
        torque;

    printf(
        "Torque applied to %s: %.2f Nm\n",
        object->name,
        torque
    );
}

/* ============================================================
 * MACHINE SIMULATION
 * ============================================================ */

static void lab_update_machine(
    EngineeringLab *lab,
    EngineeringMachine *machine,
    float dt)
{
    if (!machine->running)
        return;

    machine->power =
        machine->operating_speed *
        0.01f;

    /*
     * Update objects belonging
     * to machine.
     */

    for (uint32_t i = 0;
         i < machine->object_count;
         ++i)
    {
        EngineeringObject *object =
            lab_get_object(
                lab,
                machine->object_ids[i]
            );

        if (!object)
            continue;

        if (object->type ==
            OBJECT_MOTOR)
        {
            object->rotational_speed =
                machine->operating_speed;
        }

        if (object->type ==
            OBJECT_SHAFT)
        {
            object->rotational_speed =
                machine->operating_speed;
        }

        /*
         * Simplified heat generation.
         */

        object->temperature +=
            machine->power *
            0.001f *
            dt;

        object->temperature -=
            0.02f *
            (object->temperature - 20.0f) *
            dt;
    }
}

/* ============================================================
 * LAB UPDATE
 * ============================================================ */

static void lab_update(
    EngineeringLab *lab,
    float dt)
{
    lab->simulation_time += dt;

    /*
     * Physics.
     */

    for (uint32_t i = 0;
         i < lab->object_count;
         ++i)
    {
        EngineeringObject *object =
            &lab->objects[i];

        if (object->grabbed)
            continue;

        lab_integrate_object(
            object,
            dt
        );

        lab_ground_collision(
            object
        );
    }

    /*
     * Joints.
     */

    for (uint32_t i = 0;
         i < lab->joint_count;
         ++i)
    {
        EngineeringJoint *joint =
            &lab->joints[i];

        if (joint->type ==
            JOINT_REVOLUTE)
        {
            lab_update_revolute_joint(
                lab,
                joint,
                dt
            );
        }
    }

    /*
     * Machines.
     */

    for (uint32_t i = 0;
         i < lab->machine_count;
         ++i)
    {
        lab_update_machine(
            lab,
            &lab->machines[i],
            dt
        );
    }
}

/* ============================================================
 * RENDER
 * ============================================================ */

static void lab_render(
    EngineeringLab *lab)
{
    if (!lab->renderer.initialized)
        return;

    if (lab->renderer.begin_frame)
        lab->renderer.begin_frame(
            lab->renderer.userdata
        );

    for (uint32_t i = 0;
         i < lab->object_count;
         ++i)
    {
        EngineeringObject *object =
            &lab->objects[i];

        if (!object->visible)
            continue;

        if (lab->renderer.draw_object)
        {
            lab->renderer.draw_object(
                lab->renderer.userdata,
                object
            );
        }
    }

    if (lab->renderer.end_frame)
        lab->renderer.end_frame(
            lab->renderer.userdata
        );
}

/* ============================================================
 * BUILD DEMONSTRATION ENGINE
 * ============================================================ */

static void lab_build_engine(
    EngineeringLab *lab)
{
    /*
     * Machine frame.
     */

    uint32_t frame =
        lab_create_object(
            lab,
            "Engine Frame",
            OBJECT_MACHINE,
            vec3(0, 1.0f, -2.0f),
            vec3(1.5f, 1.0f, 0.8f),
            120.0f
        );

    /*
     * Motor.
     */

    uint32_t motor =
        lab_create_object(
            lab,
            "Electric Motor",
            OBJECT_MOTOR,
            vec3(0, 1.7f, -2.0f),
            vec3(0.45f, 0.45f, 0.7f),
            18.0f
        );

    /*
     * Shaft.
     */

    uint32_t shaft =
        lab_create_object(
            lab,
            "Drive Shaft",
            OBJECT_SHAFT,
            vec3(0, 1.7f, -1.4f),
            vec3(0.15f, 0.15f, 1.2f),
            5.0f
        );

    /*
     * Bearing.
     */

    uint32_t bearing =
        lab_create_object(
            lab,
            "Bearing",
            OBJECT_BEARING,
            vec3(0, 1.7f, -1.0f),
            vec3(0.30f, 0.30f, 0.20f),
            1.2f
        );

    /*
     * Gear.
     */

    uint32_t gear =
        lab_create_object(
            lab,
            "Drive Gear",
            OBJECT_GEAR,
            vec3(0, 1.7f, -0.5f),
            vec3(0.55f, 0.55f, 0.15f),
            3.0f
        );

    /*
     * Piston.
     */

    uint32_t piston =
        lab_create_object(
            lab,
            "Piston",
            OBJECT_PISTON,
            vec3(0, 1.7f, 0.2f),
            vec3(0.25f, 0.7f, 0.25f),
            4.0f
        );

    /*
     * Machine.
     */

    uint32_t machine =
        lab_create_machine(
            lab,
            "Educational Electric Drive"
        );

    lab_machine_add_object(
        lab,
        machine,
        frame
    );

    lab_machine_add_object(
        lab,
        machine,
        motor
    );

    lab_machine_add_object(
        lab,
        machine,
        shaft
    );

    lab_machine_add_object(
        lab,
        machine,
        bearing
    );

    lab_machine_add_object(
        lab,
        machine,
        gear
    );

    lab_machine_add_object(
        lab,
        machine,
        piston
    );

    /*
     * Mechanical joints.
     */

    lab_create_joint(
        lab,
        motor,
        shaft,
        JOINT_REVOLUTE,
        vec3(0, 0, 1)
    );

    lab_create_joint(
        lab,
        shaft,
        gear,
        JOINT_FIXED,
        vec3(0, 0, 1)
    );

    lab_create_joint(
        lab,
        gear,
        piston,
        JOINT_PRISMATIC,
        vec3(0, 1, 0)
    );

    /*
     * Start machine.
     */

    lab->machines[machine].running =
        true;

    lab->machines[machine].operating_speed =
        1200.0f;
}

/* ============================================================
 * BUILD LAB
 * ============================================================ */

static void lab_build_room(
    EngineeringLab *lab)
{
    /*
     * Main workbench.
     */

    lab_create_object(
        lab,
        "Engineering Workbench",
        OBJECT_WORKBENCH,
        vec3(0, 0.5f, -3.0f),
        vec3(3.0f, 1.0f, 1.2f),
        200.0f
    );

    /*
     * Add measuring instruments.
     */

    lab_create_tool(
        lab,
        "Digital Caliper",
        TOOL_CALIPER,
        0.0001f
    );

    lab_create_tool(
        lab,
        "Torque Wrench",
        TOOL_TORQUE_WRENCH,
        0.1f
    );

    lab_create_tool(
        lab,
        "Digital Thermometer",
        TOOL_THERMOMETER,
        0.1f
    );
}

/* ============================================================
 * STATUS
 * ============================================================ */

static void lab_print_status(
    EngineeringLab *lab)
{
    printf("\n");
    printf(
        "============================================\n"
    );

    printf(
        "           VR ENGINEERING LAB\n"
    );

    printf(
        "============================================\n"
    );

    printf(
        "Simulation time: %.2f s\n",
        lab->simulation_time
    );

    printf(
        "Objects: %u\n",
        lab->object_count
    );

    printf(
        "Machines: %u\n",
        lab->machine_count
    );

    printf(
        "Joints: %u\n",
        lab->joint_count
    );

    printf(
        "Tools: %u\n",
        lab->tool_count
    );

    printf("\n");

    for (uint32_t i = 0;
         i < lab->object_count;
         ++i)
    {
        EngineeringObject *object =
            &lab->objects[i];

        printf(
            "%3u  %-24s "
            "pos=(%6.2f %6.2f %6.2f) "
            "temp=%6.2f C "
            "speed=%7.1f RPM\n",
            object->id,
            object->name,
            object->transform.position.x,
            object->transform.position.y,
            object->transform.position.z,
            object->temperature,
            object->rotational_speed
        );
    }

    printf(
        "============================================\n"
    );
}

/* ============================================================
 * DEMONSTRATE ENGINEERING INTERACTION
 * ============================================================ */

static void lab_demo(
    EngineeringLab *lab)
{
    /*
     * Measure motor / shaft distance.
     */

    lab_measure_distance(
        lab,
        1,
        2
    );

    /*
     * Use caliper on gear.
     */

    float diameter =
        lab_caliper_measure(
            lab,
            0,
            4
        );

    printf(
        "Caliper measurement: %.4f m\n",
        diameter
    );

    /*
     * Apply torque.
     */

    lab_apply_torque_wrench(
        lab,
        2,
        25.0f
    );

    /*
     * Grab piston.
     */

    lab_grab_object(
        lab,
        5
    );

    /*
     * Move it by hand.
     */

    EngineeringObject *piston =
        lab_get_object(
            lab,
            5
        );

    if (piston)
    {
        piston->transform.position =
            vec3(
                0,
                2.0f,
                0.2f
            );
    }

    /*
     * Release with small velocity.
     */

    lab_release_object(
        lab,
        5,
        vec3(
            0,
            0,
            0
        )
    );
}

/* ============================================================
 * MAIN
 * ============================================================ */

int main(void)
{
    srand(
        (unsigned int)time(NULL)
    );

    EngineeringLab lab;

    memset(
        &lab,
        0,
        sizeof(lab)
    );

    lab.running = true;

    printf(
        "Starting VR Engineering Laboratory...\n"
    );

    /*
     * Build physical laboratory.
     */

    lab_build_room(
        &lab
    );

    /*
     * Build demonstration machine.
     */

    lab_build_engine(
        &lab
    );

    /*
     * Demonstrate engineering
     * interaction.
     */

    lab_demo(
        &lab
    );

    /*
     * Simulation.
     *
     * A real Quest implementation would
     * synchronize this with the OpenXR frame
     * loop and use a fixed physics timestep.
     */

    const float dt =
        1.0f / 120.0f;

    for (int frame = 0;
         frame < 1200;
         ++frame)
    {
        lab_update(
            &lab,
            dt
        );

        /*
         * VR renderer.
         */

        lab_render(
            &lab
        );
    }

    lab_print_status(
        &lab
    );

    printf(
        "\nVR Engineering Lab shutdown.\n"
    );

    return 0;
}
```

### The architecture this establishes

```text
                 META QUEST
                     │
                  OpenXR
                     │
          ┌──────────┴──────────┐
          │                     │
       HEAD POSE          HAND / CONTROLLER
          │                     │
          └──────────┬──────────┘
                     │
              VR INTERACTION
                     │
       ┌─────────────┼─────────────┐
       │             │             │
     Grab          Measure       Operate
       │             │             │
       └─────────────┼─────────────┘
                     │
             ENGINEERING LAB
                     │
        ┌────────────┼────────────┐
        │            │            │
     Objects       Joints       Tools
        │            │            │
        └────────────┼────────────┘
                     │
              PHYSICS ENGINE
                     │
             MACHINE SIMULATION
                     │
              Vulkan Renderer
                     │
                  QUEST
```

The particularly useful direction from here would be to make this **a genuinely interactive engineering simulator**: real OpenXR hand tracking → grab a component → physically remove it from an assembly → inspect it → measure it with a virtual caliper → reinstall it → start the machine and observe the mechanism operating in real time.




```c
/*
 * virtual_workshop.c
 *
 * Native C foundation for a room-scale VR workshop.
 *
 * Features:
 *   - Persistent workshop environment
 *   - Workbenches
 *   - Tools
 *   - Parts and materials
 *   - Grabbing and manipulation
 *   - Tool activation
 *   - Cutting
 *   - Drilling
 *   - Fastening
 *   - Welding
 *   - Soldering
 *   - 3D-printing jobs
 *   - Electronics
 *   - Measurements
 *   - Inventory
 *   - Fabrication jobs
 *   - Basic physics
 *
 * Intended target:
 *   Meta Quest / OpenXR / Vulkan / Android NDK
 *
 * Compile prototype:
 *
 *   cc -std=c11 -Wall -Wextra -O2 virtual_workshop.c \
 *      -lm -o virtual_workshop
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <math.h>
#include <time.h>

/* ============================================================
 * CONSTANTS
 * ============================================================ */

#define WORKSHOP_MAX_OBJECTS       1024
#define WORKSHOP_MAX_TOOLS         128
#define WORKSHOP_MAX_MATERIALS     256
#define WORKSHOP_MAX_JOBS          128
#define WORKSHOP_MAX_INVENTORY     512
#define WORKSHOP_MAX_CONNECTIONS   512
#define WORKSHOP_MAX_INPUTS        8

#define WORKSHOP_GRAVITY 9.80665f

/* ============================================================
 * VECTOR
 * ============================================================ */

typedef struct
{
    float x;
    float y;
    float z;

} Vec3;

static Vec3 v3(float x, float y, float z)
{
    Vec3 v = { x, y, z };
    return v;
}

static Vec3 v3_add(Vec3 a, Vec3 b)
{
    return v3(
        a.x + b.x,
        a.y + b.y,
        a.z + b.z
    );
}

static Vec3 v3_sub(Vec3 a, Vec3 b)
{
    return v3(
        a.x - b.x,
        a.y - b.y,
        a.z - b.z
    );
}

static Vec3 v3_mul(Vec3 a, float s)
{
    return v3(
        a.x * s,
        a.y * s,
        a.z * s
    );
}

static float v3_dot(Vec3 a, Vec3 b)
{
    return
        a.x*b.x +
        a.y*b.y +
        a.z*b.z;
}

static float v3_length(Vec3 a)
{
    return sqrtf(v3_dot(a, a));
}

static Vec3 v3_normalize(Vec3 a)
{
    float l = v3_length(a);

    if (l < 0.000001f)
        return v3(0, 0, 0);

    return v3_mul(a, 1.0f / l);
}

/* ============================================================
 * QUATERNION
 * ============================================================ */

typedef struct
{
    float x;
    float y;
    float z;
    float w;

} Quat;

static Quat quat_identity(void)
{
    Quat q = {0, 0, 0, 1};
    return q;
}

/* ============================================================
 * TRANSFORM
 * ============================================================ */

typedef struct
{
    Vec3 position;
    Quat rotation;
    Vec3 scale;

} Transform;

static Transform transform_identity(void)
{
    Transform t;

    t.position = v3(0, 0, 0);
    t.rotation = quat_identity();
    t.scale = v3(1, 1, 1);

    return t;
}

/* ============================================================
 * OBJECT TYPES
 * ============================================================ */

typedef enum
{
    OBJECT_NONE = 0,

    OBJECT_WORKBENCH,
    OBJECT_PART,
    OBJECT_MATERIAL,

    OBJECT_WOOD,
    OBJECT_METAL,
    OBJECT_PLASTIC,

    OBJECT_ELECTRONIC_COMPONENT,
    OBJECT_CIRCUIT_BOARD,
    OBJECT_WIRE,

    OBJECT_SCREW,
    OBJECT_BOLT,
    OBJECT_NUT,

    OBJECT_3D_PRINT,

    OBJECT_MACHINE

} WorkshopObjectType;

/* ============================================================
 * PHYSICS
 * ============================================================ */

typedef struct
{
    bool dynamic;
    bool gravity;

    float mass;

    Vec3 velocity;
    Vec3 force;

    float damping;

} Physics;

/* ============================================================
 * WORKSHOP OBJECT
 * ============================================================ */

typedef struct
{
    uint32_t id;

    char name[128];

    WorkshopObjectType type;

    Transform transform;

    Vec3 dimensions;

    Physics physics;

    float temperature;

    float hardness;

    float strength;

    float electrical_resistance;

    float electrical_voltage;

    float electrical_current;

    bool visible;
    bool selectable;
    bool grabbed;

    bool damaged;
    bool fabricated;

    int parent;

} WorkshopObject;

/* ============================================================
 * TOOL TYPES
 * ============================================================ */

typedef enum
{
    TOOL_NONE = 0,

    TOOL_HAMMER,
    TOOL_SCREWDRIVER,
    TOOL_SPANNER,
    TOOL_PLIERS,

    TOOL_DRILL,
    TOOL_SAW,
    TOOL_GRINDER,

    TOOL_SOLDERING_IRON,
    TOOL_WELDER,

    TOOL_MULTIMETER,
    TOOL_CALIPER,

    TOOL_3D_PRINTER,

    TOOL_LASER_CUTTER,
    TOOL_CNC_MACHINE

} ToolType;

/* ============================================================
 * TOOL
 * ============================================================ */

typedef struct
{
    uint32_t id;

    char name[128];

    ToolType type;

    Transform transform;

    float power;

    float speed;

    float temperature;

    float torque;

    float accuracy;

    bool active;
    bool grabbed;

} WorkshopTool;

/* ============================================================
 * MATERIAL
 * ============================================================ */

typedef enum
{
    MATERIAL_NONE = 0,

    MATERIAL_STEEL,
    MATERIAL_ALUMINIUM,
    MATERIAL_COPPER,
    MATERIAL_BRASS,

    MATERIAL_PINE,
    MATERIAL_OAK,

    MATERIAL_ABS,
    MATERIAL_PLA,

    MATERIAL_SILICONE

} MaterialType;

typedef struct
{
    uint32_t id;

    char name[64];

    MaterialType type;

    float density;

    float hardness;

    float melting_point;

    float electrical_conductivity;

} WorkshopMaterial;

/* ============================================================
 * FABRICATION JOB
 * ============================================================ */

typedef enum
{
    JOB_NONE = 0,

    JOB_CUT,
    JOB_DRILL,
    JOB_GRIND,
    JOB_SAND,
    JOB_WELD,
    JOB_SOLDER,
    JOB_PRINT_3D,
    JOB_CNC

} JobType;

typedef enum
{
    JOB_QUEUED = 0,
    JOB_RUNNING,
    JOB_COMPLETE,
    JOB_FAILED

} JobState;

typedef struct
{
    uint32_t id;

    char name[128];

    JobType type;

    JobState state;

    uint32_t object_id;
    uint32_t tool_id;

    float progress;
    float duration;

    float energy_used;

} FabricationJob;

/* ============================================================
 * ELECTRICAL CONNECTION
 * ============================================================ */

typedef struct
{
    uint32_t id;

    uint32_t object_a;
    uint32_t object_b;

    float resistance;

    bool conductive;

} ElectricalConnection;

/* ============================================================
 * VR INPUT
 * ============================================================ */

typedef struct
{
    bool connected;

    Vec3 position;
    Quat rotation;

    bool trigger;
    bool grip;

    float trigger_value;
    float grip_value;

    bool primary;
    bool secondary;

} VRInput;

/* ============================================================
 * RAY
 * ============================================================ */

typedef struct
{
    Vec3 origin;
    Vec3 direction;

} Ray;

/* ============================================================
 * RENDERER
 * ============================================================ */

typedef struct
{
    bool initialized;

    void (*begin_frame)(void *);
    void (*end_frame)(void *);

    void (*draw_object)(
        void *,
        WorkshopObject *
    );

    void (*draw_tool)(
        void *,
        WorkshopTool *
    );

    void (*draw_line)(
        void *,
        Vec3,
        Vec3
    );

    void (*draw_text)(
        void *,
        const char *,
        Vec3,
        float
    );

    void *userdata;

} WorkshopRenderer;

/* ============================================================
 * WORKSHOP
 * ============================================================ */

typedef struct
{
    bool running;

    float time;

    WorkshopObject objects[
        WORKSHOP_MAX_OBJECTS
    ];

    uint32_t object_count;

    WorkshopTool tools[
        WORKSHOP_MAX_TOOLS
    ];

    uint32_t tool_count;

    WorkshopMaterial materials[
        WORKSHOP_MAX_MATERIALS
    ];

    uint32_t material_count;

    FabricationJob jobs[
        WORKSHOP_MAX_JOBS
    ];

    uint32_t job_count;

    ElectricalConnection connections[
        WORKSHOP_MAX_CONNECTIONS
    ];

    uint32_t connection_count;

    VRInput inputs[
        WORKSHOP_MAX_INPUTS
    ];

    uint32_t input_count;

    WorkshopRenderer renderer;

} VirtualWorkshop;

/* ============================================================
 * OBJECT CREATION
 * ============================================================ */

static uint32_t workshop_create_object(
    VirtualWorkshop *w,
    const char *name,
    WorkshopObjectType type,
    Vec3 position,
    Vec3 dimensions,
    float mass)
{
    if (w->object_count >=
        WORKSHOP_MAX_OBJECTS)
        return UINT32_MAX;

    WorkshopObject *object =
        &w->objects[w->object_count];

    memset(
        object,
        0,
        sizeof(*object)
    );

    object->id =
        w->object_count;

    strncpy(
        object->name,
        name,
        sizeof(object->name) - 1
    );

    object->type = type;

    object->transform =
        transform_identity();

    object->transform.position =
        position;

    object->dimensions =
        dimensions;

    object->physics.mass =
        mass;

    object->physics.dynamic =
        mass > 0;

    object->physics.gravity = true;

    object->physics.damping =
        0.05f;

    object->visible = true;
    object->selectable = true;

    object->parent = -1;

    w->object_count++;

    return object->id;
}

/* ============================================================
 * OBJECT LOOKUP
 * ============================================================ */

static WorkshopObject *workshop_object(
    VirtualWorkshop *w,
    uint32_t id)
{
    if (id >= w->object_count)
        return NULL;

    return &w->objects[id];
}

/* ============================================================
 * TOOL CREATION
 * ============================================================ */

static uint32_t workshop_create_tool(
    VirtualWorkshop *w,
    const char *name,
    ToolType type,
    float power)
{
    if (w->tool_count >=
        WORKSHOP_MAX_TOOLS)
        return UINT32_MAX;

    WorkshopTool *tool =
        &w->tools[w->tool_count];

    memset(
        tool,
        0,
        sizeof(*tool)
    );

    tool->id =
        w->tool_count;

    strncpy(
        tool->name,
        name,
        sizeof(tool->name) - 1
    );

    tool->type = type;
    tool->power = power;

    tool->accuracy = 0.001f;

    w->tool_count++;

    return tool->id;
}

/* ============================================================
 * MATERIAL CREATION
 * ============================================================ */

static uint32_t workshop_create_material(
    VirtualWorkshop *w,
    const char *name,
    MaterialType type,
    float density,
    float hardness,
    float melting_point)
{
    if (w->material_count >=
        WORKSHOP_MAX_MATERIALS)
        return UINT32_MAX;

    WorkshopMaterial *m =
        &w->materials[
            w->material_count
        ];

    memset(
        m,
        0,
        sizeof(*m)
    );

    m->id =
        w->material_count;

    strncpy(
        m->name,
        name,
        sizeof(m->name) - 1
    );

    m->type = type;
    m->density = density;
    m->hardness = hardness;
    m->melting_point =
        melting_point;

    w->material_count++;

    return m->id;
}

/* ============================================================
 * GRAB
 * ============================================================ */

static void workshop_grab(
    VirtualWorkshop *w,
    uint32_t object_id)
{
    WorkshopObject *object =
        workshop_object(
            w,
            object_id
        );

    if (!object)
        return;

    if (!object->selectable)
        return;

    object->grabbed = true;

    object->physics.gravity =
        false;

    object->physics.velocity =
        v3(0, 0, 0);

    printf(
        "Grabbed: %s\n",
        object->name
    );
}

/* ============================================================
 * RELEASE
 * ============================================================ */

static void workshop_release(
    VirtualWorkshop *w,
    uint32_t object_id,
    Vec3 velocity)
{
    WorkshopObject *object =
        workshop_object(
            w,
            object_id
        );

    if (!object)
        return;

    object->grabbed = false;

    object->physics.gravity =
        true;

    object->physics.velocity =
        velocity;
}

/* ============================================================
 * RAY / BOX
 * ============================================================ */

static bool workshop_ray_box(
    Ray ray,
    WorkshopObject *object,
    float *distance)
{
    Vec3 center =
        object->transform.position;

    Vec3 half =
        v3_mul(
            object->dimensions,
            0.5f
        );

    float min_x =
        center.x - half.x;

    float max_x =
        center.x + half.x;

    float min_y =
        center.y - half.y;

    float max_y =
        center.y + half.y;

    float min_z =
        center.z - half.z;

    float max_z =
        center.z + half.z;

    float tmin = -INFINITY;
    float tmax = INFINITY;

    float origin[3] = {
        ray.origin.x,
        ray.origin.y,
        ray.origin.z
    };

    float direction[3] = {
        ray.direction.x,
        ray.direction.y,
        ray.direction.z
    };

    float minimum[3] = {
        min_x,
        min_y,
        min_z
    };

    float maximum[3] = {
        max_x,
        max_y,
        max_z
    };

    for (int axis = 0;
         axis < 3;
         ++axis)
    {
        if (fabsf(direction[axis]) < 0.00001f)
        {
            if (origin[axis] < minimum[axis] ||
                origin[axis] > maximum[axis])
                return false;
        }
        else
        {
            float t1 =
                (minimum[axis] -
                 origin[axis]) /
                direction[axis];

            float t2 =
                (maximum[axis] -
                 origin[axis]) /
                direction[axis];

            if (t1 > t2)
            {
                float temp = t1;
                t1 = t2;
                t2 = temp;
            }

            if (t1 > tmin)
                tmin = t1;

            if (t2 < tmax)
                tmax = t2;
        }
    }

    if (tmax < tmin)
        return false;

    if (tmax < 0)
        return false;

    if (distance)
        *distance =
            tmin >= 0 ?
            tmin :
            tmax;

    return true;
}

/* ============================================================
 * PICK
 * ============================================================ */

static int workshop_pick(
    VirtualWorkshop *w,
    Ray ray)
{
    float closest = INFINITY;

    int selected = -1;

    for (uint32_t i = 0;
         i < w->object_count;
         ++i)
    {
        WorkshopObject *object =
            &w->objects[i];

        if (!object->visible ||
            !object->selectable)
            continue;

        float distance;

        if (workshop_ray_box(
                ray,
                object,
                &distance))
        {
            if (distance < closest)
            {
                closest = distance;
                selected = (int)i;
            }
        }
    }

    return selected;
}

/* ============================================================
 * DRILLING
 * ============================================================ */

static bool workshop_drill(
    VirtualWorkshop *w,
    uint32_t tool_id,
    uint32_t object_id,
    float depth)
{
    if (tool_id >= w->tool_count)
        return false;

    WorkshopTool *tool =
        &w->tools[tool_id];

    WorkshopObject *object =
        workshop_object(
            w,
            object_id
        );

    if (!object)
        return false;

    if (tool->type != TOOL_DRILL)
        return false;

    if (depth <= 0)
        return false;

    /*
     * Simplified drilling model.
     */

    float hardness =
        object->hardness;

    float work =
        hardness *
        depth *
        100.0f;

    tool->temperature +=
        work * 0.0001f;

    object->temperature +=
        work * 0.00005f;

    object->damaged = true;

    printf(
        "Drilled %.3f m into %s\n",
        depth,
        object->name
    );

    return true;
}

/* ============================================================
 * CUTTING
 * ============================================================ */

static bool workshop_cut(
    VirtualWorkshop *w,
    uint32_t tool_id,
    uint32_t object_id,
    float depth)
{
    if (tool_id >= w->tool_count)
        return false;

    WorkshopTool *tool =
        &w->tools[tool_id];

    WorkshopObject *object =
        workshop_object(
            w,
            object_id
        );

    if (!object)
        return false;

    if (tool->type != TOOL_SAW &&
        tool->type != TOOL_GRINDER)
        return false;

    if (depth <= 0)
        return false;

    object->damaged = true;

    /*
     * Reduce the object's dimension.
     */

    if (object->dimensions.x > depth)
    {
        object->dimensions.x -=
            depth;
    }

    tool->temperature +=
        depth * 20.0f;

    printf(
        "Cut %s by %.3f m\n",
        object->name,
        depth
    );

    return true;
}

/* ============================================================
 * SOLDERING
 * ============================================================ */

static bool workshop_solder(
    VirtualWorkshop *w,
    uint32_t tool_id,
    uint32_t object_a,
    uint32_t object_b)
{
    if (tool_id >= w->tool_count)
        return false;

    WorkshopTool *tool =
        &w->tools[tool_id];

    if (tool->type !=
        TOOL_SOLDERING_IRON)
        return false;

    WorkshopObject *a =
        workshop_object(w, object_a);

    WorkshopObject *b =
        workshop_object(w, object_b);

    if (!a || !b)
        return false;

    /*
     * A real version would require
     * spatial contact and thermal modelling.
     */

    a->fabricated = true;
    b->fabricated = true;

    tool->temperature =
        350.0f;

    printf(
        "Soldered %s to %s\n",
        a->name,
        b->name
    );

    return true;
}

/* ============================================================
 * WELDING
 * ============================================================ */

static bool workshop_weld(
    VirtualWorkshop *w,
    uint32_t tool_id,
    uint32_t object_a,
    uint32_t object_b)
{
    if (tool_id >= w->tool_count)
        return false;

    WorkshopTool *tool =
        &w->tools[tool_id];

    if (tool->type != TOOL_WELDER)
        return false;

    WorkshopObject *a =
        workshop_object(w, object_a);

    WorkshopObject *b =
        workshop_object(w, object_b);

    if (!a || !b)
        return false;

    a->fabricated = true;
    b->fabricated = true;

    a->temperature +=
        500.0f;

    b->temperature +=
        500.0f;

    printf(
        "Welded %s to %s\n",
        a->name,
        b->name
    );

    return true;
}

/* ============================================================
 * ELECTRICAL CONNECTION
 * ============================================================ */

static uint32_t workshop_connect(
    VirtualWorkshop *w,
    uint32_t a,
    uint32_t b,
    float resistance)
{
    if (w->connection_count >=
        WORKSHOP_MAX_CONNECTIONS)
        return UINT32_MAX;

    ElectricalConnection *c =
        &w->connections[
            w->connection_count
        ];

    memset(
        c,
        0,
        sizeof(*c)
    );

    c->id =
        w->connection_count;

    c->object_a = a;
    c->object_b = b;

    c->resistance =
        resistance;

    c->conductive = true;

    w->connection_count++;

    return c->id;
}

/* ============================================================
 * MULTIMETER
 * ============================================================ */

static float workshop_measure_voltage(
    VirtualWorkshop *w,
    uint32_t object_id)
{
    WorkshopObject *object =
        workshop_object(
            w,
            object_id
        );

    if (!object)
        return 0;

    return object->electrical_voltage;
}

static float workshop_measure_current(
    VirtualWorkshop *w,
    uint32_t object_id)
{
    WorkshopObject *object =
        workshop_object(
            w,
            object_id
        );

    if (!object)
        return 0;

    return object->electrical_current;
}

/* ============================================================
 * 3D PRINTING
 * ============================================================ */

static uint32_t workshop_start_print(
    VirtualWorkshop *w,
    const char *name,
    Vec3 dimensions,
    float duration)
{
    uint32_t object_id =
        workshop_create_object(
            w,
            name,
            OBJECT_3D_PRINT,
            v3(0, 1.0f, -2.0f),
            dimensions,
            0.5f
        );

    if (object_id ==
        UINT32_MAX)
        return UINT32_MAX;

    if (w->job_count >=
        WORKSHOP_MAX_JOBS)
        return UINT32_MAX;

    FabricationJob *job =
        &w->jobs[w->job_count];

    memset(
        job,
        0,
        sizeof(*job)
    );

    job->id =
        w->job_count;

    snprintf(
        job->name,
        sizeof(job->name),
        "3D Print: %s",
        name
    );

    job->type =
        JOB_PRINT_3D;

    job->state =
        JOB_RUNNING;

    job->object_id =
        object_id;

    job->duration =
        duration;

    job->progress = 0;

    w->job_count++;

    return job->id;
}

/* ============================================================
 * FABRICATION JOB UPDATE
 * ============================================================ */

static void workshop_update_jobs(
    VirtualWorkshop *w,
    float dt)
{
    for (uint32_t i = 0;
         i < w->job_count;
         ++i)
    {
        FabricationJob *job =
            &w->jobs[i];

        if (job->state !=
            JOB_RUNNING)
            continue;

        if (job->duration <= 0)
        {
            job->state =
                JOB_COMPLETE;

            continue;
        }

        job->progress +=
            dt / job->duration;

        job->energy_used +=
            dt * 0.1f;

        if (job->progress >= 1.0f)
        {
            job->progress = 1.0f;

            job->state =
                JOB_COMPLETE;

            WorkshopObject *object =
                workshop_object(
                    w,
                    job->object_id
                );

            if (object)
            {
                object->fabricated =
                    true;
            }

            printf(
                "Fabrication complete: %s\n",
                job->name
            );
        }
    }
}

/* ============================================================
 * PHYSICS
 * ============================================================ */

static void workshop_physics(
    VirtualWorkshop *w,
    float dt)
{
    for (uint32_t i = 0;
         i < w->object_count;
         ++i)
    {
        WorkshopObject *object =
            &w->objects[i];

        if (!object->physics.dynamic)
            continue;

        if (object->grabbed)
            continue;

        float mass =
            object->physics.mass;

        if (mass <= 0.0001f)
            continue;

        Vec3 acceleration =
            v3_mul(
                object->physics.force,
                1.0f / mass
            );

        if (object->physics.gravity)
        {
            acceleration.y -=
                WORKSHOP_GRAVITY;
        }

        object->physics.velocity =
            v3_add(
                object->physics.velocity,
                v3_mul(
                    acceleration,
                    dt
                )
            );

        object->physics.velocity =
            v3_mul(
                object->physics.velocity,
                1.0f -
                object->physics.damping *
                dt
            );

        object->transform.position =
            v3_add(
                object->transform.position,
                v3_mul(
                    object->physics.velocity,
                    dt
                )
            );

        /*
         * Workshop floor.
         */

        float floor =
            object->dimensions.y *
            0.5f;

        if (object->transform.position.y <
            floor)
        {
            object->transform.position.y =
                floor;

            if (object->physics.velocity.y < 0)
            {
                object->physics.velocity.y *=
                    -0.2f;
            }
        }

        object->physics.force =
            v3(0, 0, 0);
    }
}

/* ============================================================
 * THERMAL SIMULATION
 * ============================================================ */

static void workshop_thermal(
    VirtualWorkshop *w,
    float dt)
{
    for (uint32_t i = 0;
         i < w->object_count;
         ++i)
    {
        WorkshopObject *object =
            &w->objects[i];

        /*
         * Cool objects toward room temperature.
         */

        float room =
            20.0f;

        float cooling =
            0.08f *
            (object->temperature - room);

        object->temperature -=
            cooling * dt;
    }

    for (uint32_t i = 0;
         i < w->tool_count;
         ++i)
    {
        WorkshopTool *tool =
            &w->tools[i];

        float cooling =
            0.12f *
            (tool->temperature - 20.0f);

        tool->temperature -=
            cooling * dt;
    }
}

/* ============================================================
 * WORKSHOP UPDATE
 * ============================================================ */

static void workshop_update(
    VirtualWorkshop *w,
    float dt)
{
    w->time += dt;

    workshop_physics(
        w,
        dt
    );

    workshop_thermal(
        w,
        dt
    );

    workshop_update_jobs(
        w,
        dt
    );
}

/* ============================================================
 * RENDER
 * ============================================================ */

static void workshop_render(
    VirtualWorkshop *w)
{
    if (!w->renderer.initialized)
        return;

    if (w->renderer.begin_frame)
        w->renderer.begin_frame(
            w->renderer.userdata
        );

    for (uint32_t i = 0;
         i < w->object_count;
         ++i)
    {
        WorkshopObject *object =
            &w->objects[i];

        if (object->visible &&
            w->renderer.draw_object)
        {
            w->renderer.draw_object(
                w->renderer.userdata,
                object
            );
        }
    }

    for (uint32_t i = 0;
         i < w->tool_count;
         ++i)
    {
        WorkshopTool *tool =
            &w->tools[i];

        if (w->renderer.draw_tool)
        {
            w->renderer.draw_tool(
                w->renderer.userdata,
                tool
            );
        }
    }

    if (w->renderer.end_frame)
        w->renderer.end_frame(
            w->renderer.userdata
        );
}

/* ============================================================
 * BUILD WORKSHOP
 * ============================================================ */

static void workshop_build(
    VirtualWorkshop *w)
{
    /*
     * Main workbench.
     */

    workshop_create_object(
        w,
        "Main Workbench",
        OBJECT_WORKBENCH,
        v3(0, 0.5f, -2.5f),
        v3(3.0f, 1.0f, 1.2f),
        200.0f
    );

    /*
     * Secondary bench.
     */

    workshop_create_object(
        w,
        "Electronics Bench",
        OBJECT_WORKBENCH,
        v3(3.0f, 0.5f, -1.0f),
        v3(2.0f, 1.0f, 0.9f),
        150.0f
    );

    /*
     * Material stock.
     */

    workshop_create_object(
        w,
        "Steel Bar",
        OBJECT_METAL,
        v3(-2.0f, 0.15f, -2.0f),
        v3(1.5f, 0.1f, 0.1f),
        1.8f
    );

    workshop_create_object(
        w,
        "Aluminium Block",
        OBJECT_METAL,
        v3(-2.0f, 0.25f, -2.0f),
        v3(0.4f, 0.3f, 0.3f),
        0.8f
    );

    workshop_create_object(
        w,
        "Pine Block",
        OBJECT_WOOD,
        v3(-2.0f, 0.35f, -1.5f),
        v3(0.6f, 0.3f, 0.3f),
        0.4f
    );

    /*
     * Electronics.
     */

    uint32_t pcb =
        workshop_create_object(
            w,
            "Arduino-style Control Board",
            OBJECT_CIRCUIT_BOARD,
            v3(3.0f, 1.05f, -1.0f),
            v3(0.15f, 0.02f, 0.1f),
            0.05f
        );

    WorkshopObject *board =
        workshop_object(w, pcb);

    if (board)
    {
        board->electrical_voltage =
            5.0f;

        board->electrical_resistance =
            1000.0f;
    }

    workshop_create_object(
        w,
        "Copper Wire",
        OBJECT_WIRE,
        v3(3.4f, 1.05f, -1.0f),
        v3(0.5f, 0.01f, 0.01f),
        0.01f
    );

    /*
     * Tools.
     */

    workshop_create_tool(
        w,
        "Cordless Drill",
        TOOL_DRILL,
        750.0f
    );

    workshop_create_tool(
        w,
        "Workshop Saw",
        TOOL_SAW,
        1000.0f
    );

    workshop_create_tool(
        w,
        "Soldering Iron",
        TOOL_SOLDERING_IRON,
        60.0f
    );

    workshop_create_tool(
        w,
        "Arc Welder",
        TOOL_WELDER,
        3000.0f
    );

    workshop_create_tool(
        w,
        "Digital Multimeter",
        TOOL_MULTIMETER,
        0
    );

    workshop_create_tool(
        w,
        "Digital Caliper",
        TOOL_CALIPER,
        0
    );

    workshop_create_tool(
        w,
        "3D Printer",
        TOOL_3D_PRINTER,
        500.0f
    );

    /*
     * Materials.
     */

    workshop_create_material(
        w,
        "Steel",
        MATERIAL_STEEL,
        7850.0f,
        6.0f,
        1510.0f
    );

    workshop_create_material(
        w,
        "Aluminium",
        MATERIAL_ALUMINIUM,
        2700.0f,
        2.75f,
        660.0f
    );

    workshop_create_material(
        w,
        "Copper",
        MATERIAL_COPPER,
        8960.0f,
        3.0f,
        1085.0f
    );

    workshop_create_material(
        w,
        "PLA",
        MATERIAL_PLA,
        1240.0f,
        2.0f,
        180.0f
    );
}

/* ============================================================
 * DEMONSTRATION
 * ============================================================ */

static void workshop_demo(
    VirtualWorkshop *w)
{
    /*
     * Drill the aluminium block.
     */

    workshop_drill(
        w,
        0,
        4,
        0.025f
    );

    /*
     * Cut steel.
     */

    workshop_cut(
        w,
        1,
        3,
        0.10f
    );

    /*
     * Connect electronics.
     */

    workshop_connect(
        w,
        6,
        7,
        0.05f
    );

    /*
     * Start a 3D print.
     */

    workshop_start_print(
        w,
        "Custom Gear",
        v3(
            0.08f,
            0.08f,
            0.02f
        ),
        30.0f
    );

    /*
     * Solder electronics.
     */

    workshop_solder(
        w,
        2,
        6,
        7
    );
}

/* ============================================================
 * STATUS
 * ============================================================ */

static void workshop_status(
    VirtualWorkshop *w)
{
    printf("\n");
    printf(
        "================================================\n"
    );

    printf(
        "              VIRTUAL WORKSHOP\n"
    );

    printf(
        "================================================\n"
    );

    printf(
        "Simulation time: %.2f s\n",
        w->time
    );

    printf(
        "Objects: %u\n",
        w->object_count
    );

    printf(
        "Tools: %u\n",
        w->tool_count
    );

    printf(
        "Materials: %u\n",
        w->material_count
    );

    printf(
        "Fabrication jobs: %u\n",
        w->job_count
    );

    printf(
        "Electrical connections: %u\n",
        w->connection_count
    );

    printf("\nOBJECTS\n");

    for (uint32_t i = 0;
         i < w->object_count;
         ++i)
    {
        WorkshopObject *o =
            &w->objects[i];

        printf(
            "[%03u] %-28s "
            "pos=(%.2f %.2f %.2f) "
            "temp=%.1fC "
            "fabricated=%d\n",
            o->id,
            o->name,
            o->transform.position.x,
            o->transform.position.y,
            o->transform.position.z,
            o->temperature,
            o->fabricated
        );
    }

    printf("\nTOOLS\n");

    for (uint32_t i = 0;
         i < w->tool_count;
         ++i)
    {
        WorkshopTool *tool =
            &w->tools[i];

        printf(
            "[%03u] %-28s "
            "temperature=%.1fC "
            "active=%d\n",
            tool->id,
            tool->name,
            tool->temperature,
            tool->active
        );
    }

    printf("\nJOBS\n");

    for (uint32_t i = 0;
         i < w->job_count;
         ++i)
    {
        FabricationJob *job =
            &w->jobs[i];

        printf(
            "[%03u] %-28s "
            "progress=%6.1f%% "
            "state=%d\n",
            job->id,
            job->name,
            job->progress * 100.0f,
            job->state
        );
    }

    printf(
        "================================================\n"
    );
}

/* ============================================================
 * MAIN
 * ============================================================ */

int main(void)
{
    srand(
        (unsigned int)time(NULL)
    );

    VirtualWorkshop workshop;

    memset(
        &workshop,
        0,
        sizeof(workshop)
    );

    workshop.running = true;

    printf(
        "Starting Virtual Workshop...\n"
    );

    /*
     * Construct workshop.
     */

    workshop_build(
        &workshop
    );

    /*
     * Demonstrate fabrication.
     */

    workshop_demo(
        &workshop
    );

    /*
     * Simulate approximately
     * one minute.
     */

    const float dt =
        1.0f / 120.0f;

    for (int frame = 0;
         frame < 7200;
         ++frame)
    {
        workshop_update(
            &workshop,
            dt
        );

        workshop_render(
            &workshop
        );
    }

    workshop_status(
        &workshop
    );

    printf(
        "\nVirtual Workshop shutdown.\n"
    );

    return 0;
}
```




/*
    spatial_audio.c
    ------------------------------------------------------------
    #4 — Spatial Audio & Head Tracking
    Meta Quest / OpenXR-oriented C foundation

    Features:
      - 3D listener/head tracking
      - Spatial sound sources
      - Distance attenuation
      - Stereo spatialisation
      - Azimuth/elevation calculation
      - Doppler effect
      - Distance filtering
      - Occlusion
      - Reverb zones
      - Audio source velocity
      - Head-relative audio
      - HRTF-ready architecture
      - Simple audio mixer

    This is the platform-independent audio core.
    A production Quest implementation can connect the output
    to Android/AAudio, OpenSL ES, Oboe, or another audio backend.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>

#define MAX_AUDIO_SOURCES 256
#define MAX_REVERB_ZONES  32
#define AUDIO_SAMPLE_RATE 48000
#define AUDIO_CHANNELS 2
#define PI 3.14159265358979323846f

/* ============================================================
   VECTOR MATH
   ============================================================ */

typedef struct {
    float x;
    float y;
    float z;
} Vec3;

static Vec3 vec3(float x, float y, float z)
{
    Vec3 v = {x, y, z};
    return v;
}

static Vec3 vec_add(Vec3 a, Vec3 b)
{
    return vec3(
        a.x + b.x,
        a.y + b.y,
        a.z + b.z
    );
}

static Vec3 vec_sub(Vec3 a, Vec3 b)
{
    return vec3(
        a.x - b.x,
        a.y - b.y,
        a.z - b.z
    );
}

static Vec3 vec_scale(Vec3 v, float s)
{
    return vec3(
        v.x * s,
        v.y * s,
        v.z * s
    );
}

static float vec_dot(Vec3 a, Vec3 b)
{
    return a.x*b.x + a.y*b.y + a.z*b.z;
}

static float vec_length(Vec3 v)
{
    return sqrtf(vec_dot(v, v));
}

static Vec3 vec_normalize(Vec3 v)
{
    float l = vec_length(v);

    if (l < 0.000001f)
        return vec3(0, 0, 0);

    return vec_scale(v, 1.0f / l);
}

static float clampf(float v, float min, float max)
{
    if (v < min) return min;
    if (v > max) return max;
    return v;
}

static float lerpf(float a, float b, float t)
{
    return a + (b - a) * t;
}

/* ============================================================
   QUATERNIONS
   ============================================================ */

typedef struct {
    float x;
    float y;
    float z;
    float w;
} Quat;

static Quat quat_identity(void)
{
    Quat q = {0, 0, 0, 1};
    return q;
}

static Quat quat_normalize(Quat q)
{
    float l = sqrtf(
        q.x*q.x +
        q.y*q.y +
        q.z*q.z +
        q.w*q.w
    );

    if (l < 0.000001f)
        return quat_identity();

    q.x /= l;
    q.y /= l;
    q.z /= l;
    q.w /= l;

    return q;
}

static Quat quat_multiply(Quat a, Quat b)
{
    Quat q;

    q.w = a.w*b.w - a.x*b.x - a.y*b.y - a.z*b.z;
    q.x = a.w*b.x + a.x*b.w + a.y*b.z - a.z*b.y;
    q.y = a.w*b.y - a.x*b.z + a.y*b.w + a.z*b.x;
    q.z = a.w*b.z + a.x*b.y - a.y*b.x + a.z*b.w;

    return q;
}

static Vec3 quat_rotate(Quat q, Vec3 v)
{
    Vec3 qv = vec3(q.x, q.y, q.z);

    Vec3 t = vec_scale(
        vec3(
            qv.y*v.z - qv.z*v.y,
            qv.z*v.x - qv.x*v.z,
            qv.x*v.y - qv.y*v.x
        ),
        2.0f
    );

    Vec3 result = vec_add(
        v,
        vec_add(
            vec_scale(t, q.w),
            vec3(
                qv.y*t.z - qv.z*t.y,
                qv.z*t.x - qv.x*t.z,
                qv.x*t.y - qv.y*t.x
            )
        )
    );

    return result;
}

/* ============================================================
   LISTENER / HEAD TRACKING
   ============================================================ */

typedef struct {

    Vec3 position;

    Vec3 velocity;

    Quat orientation;

    Vec3 forward;
    Vec3 up;
    Vec3 right;

} AudioListener;

static void listener_update_basis(AudioListener *listener)
{
    listener->forward =
        quat_rotate(
            listener->orientation,
            vec3(0, 0, -1)
        );

    listener->up =
        quat_rotate(
            listener->orientation,
            vec3(0, 1, 0)
        );

    listener->right =
        quat_rotate(
            listener->orientation,
            vec3(1, 0, 0)
        );
}

static void listener_init(AudioListener *listener)
{
    memset(listener, 0, sizeof(*listener));

    listener->orientation = quat_identity();

    listener_update_basis(listener);
}

/*
    In a real Meta Quest application this function would receive
    the headset pose from OpenXR.

    position:
        headset position in metres

    orientation:
        headset orientation quaternion
*/

static void listener_set_pose(
    AudioListener *listener,
    Vec3 position,
    Quat orientation,
    float dt
)
{
    if (dt > 0.000001f) {

        listener->velocity =
            vec_scale(
                vec_sub(
                    position,
                    listener->position
                ),
                1.0f / dt
            );
    }

    listener->position = position;

    listener->orientation =
        quat_normalize(orientation);

    listener_update_basis(listener);
}

/* ============================================================
   AUDIO SOURCE
   ============================================================ */

typedef struct {

    int id;

    bool active;

    Vec3 position;
    Vec3 velocity;

    float volume;

    float pitch;

    float min_distance;
    float max_distance;

    bool looping;

    bool spatialized;

    bool head_locked;

    bool occluded;

    float occlusion;

    float lowpass;

    float reverb_send;

    float doppler_factor;

    const float *samples;

    size_t sample_count;

    size_t cursor;

} AudioSource;

/* ============================================================
   REVERB ZONE
   ============================================================ */

typedef struct {

    int id;

    Vec3 position;

    float radius;

    float wetness;

    float room_size;

    float damping;

} ReverbZone;

/* ============================================================
   SPATIAL AUDIO RESULT
   ============================================================ */

typedef struct {

    float distance;

    float azimuth;

    float elevation;

    float attenuation;

    float left_gain;

    float right_gain;

    float doppler;

    float occlusion;

    float reverb;

    float lowpass;

} SpatialResult;

/* ============================================================
   AUDIO ENGINE
   ============================================================ */

typedef struct {

    AudioListener listener;

    AudioSource sources[MAX_AUDIO_SOURCES];

    ReverbZone zones[MAX_REVERB_ZONES];

    int source_count;
    int zone_count;

    float master_volume;

    float speed_of_sound;

} SpatialAudioEngine;

/* ============================================================
   ENGINE INITIALISATION
   ============================================================ */

static void audio_engine_init(SpatialAudioEngine *engine)
{
    memset(engine, 0, sizeof(*engine));

    listener_init(&engine->listener);

    engine->master_volume = 1.0f;

    /*
        Approximate speed of sound in air at room temperature.
    */
    engine->speed_of_sound = 343.0f;
}

/* ============================================================
   AUDIO SOURCE CREATION
   ============================================================ */

static AudioSource *audio_source_create(
    SpatialAudioEngine *engine
)
{
    if (engine->source_count >= MAX_AUDIO_SOURCES)
        return NULL;

    int index = engine->source_count++;

    AudioSource *source =
        &engine->sources[index];

    memset(source, 0, sizeof(*source));

    source->id = index;

    source->active = true;

    source->volume = 1.0f;

    source->pitch = 1.0f;

    source->min_distance = 1.0f;

    source->max_distance = 30.0f;

    source->spatialized = true;

    source->doppler_factor = 1.0f;

    source->lowpass = 1.0f;

    return source;
}

/* ============================================================
   REVERB ZONE CREATION
   ============================================================ */

static ReverbZone *reverb_zone_create(
    SpatialAudioEngine *engine,
    Vec3 position,
    float radius
)
{
    if (engine->zone_count >= MAX_REVERB_ZONES)
        return NULL;

    ReverbZone *zone =
        &engine->zones[engine->zone_count++];

    memset(zone, 0, sizeof(*zone));

    zone->id = engine->zone_count - 1;

    zone->position = position;

    zone->radius = radius;

    zone->wetness = 0.25f;

    zone->room_size = 0.7f;

    zone->damping = 0.4f;

    return zone;
}

/* ============================================================
   DISTANCE ATTENUATION
   ============================================================ */

static float calculate_distance_gain(
    float distance,
    float min_distance,
    float max_distance
)
{
    if (distance <= min_distance)
        return 1.0f;

    if (distance >= max_distance)
        return 0.0f;

    /*
        Inverse-distance style attenuation.
    */

    float gain =
        min_distance /
        distance;

    /*
        Smoothly clamp the tail.
    */

    float range =
        (distance - min_distance) /
        (max_distance - min_distance);

    float fade =
        1.0f - range;

    fade = clampf(fade, 0.0f, 1.0f);

    return clampf(
        gain * fade,
        0.0f,
        1.0f
    );
}

/* ============================================================
   AZIMUTH / ELEVATION
   ============================================================ */

static void calculate_direction_angles(
    AudioListener *listener,
    Vec3 source_position,
    float *azimuth,
    float *elevation
)
{
    Vec3 direction =
        vec_normalize(
            vec_sub(
                source_position,
                listener->position
            )
        );

    float front =
        vec_dot(
            listener->forward,
            direction
        );

    float side =
        vec_dot(
            listener->right,
            direction
        );

    float up =
        vec_dot(
            listener->up,
            direction
        );

    *azimuth =
        atan2f(side, front);

    float horizontal =
        sqrtf(
            front*front +
            side*side
        );

    *elevation =
        atan2f(
            up,
            horizontal
        );
}

/* ============================================================
   STEREO PAN
   ============================================================ */

static void calculate_stereo_pan(
    float azimuth,
    float *left,
    float *right
)
{
    /*
        Equal-power style panning.

        -PI/2 = hard left
         0    = centre
        +PI/2 = hard right
    */

    float pan =
        clampf(
            sinf(azimuth),
            -1.0f,
            1.0f
        );

    float angle =
        (pan + 1.0f) *
        0.25f *
        PI;

    *left =
        cosf(angle);

    *right =
        sinf(angle);
}

/* ============================================================
   DOPPLER
   ============================================================ */

static float calculate_doppler(
    AudioListener *listener,
    AudioSource *source,
    float speed_of_sound
)
{
    Vec3 source_to_listener =
        vec_normalize(
            vec_sub(
                listener->position,
                source->position
            )
        );

    float source_velocity =
        vec_dot(
            source->velocity,
            source_to_listener
        );

    float listener_velocity =
        vec_dot(
            listener->velocity,
            source_to_listener
        );

    float numerator =
        speed_of_sound -
        listener_velocity;

    float denominator =
        speed_of_sound -
        source_velocity;

    if (fabsf(denominator) < 0.001f)
        denominator = 0.001f;

    float ratio =
        numerator /
        denominator;

    return clampf(
        ratio,
        0.5f,
        2.0f
    );
}

/* ============================================================
   OCCLUSION
   ============================================================ */

static float calculate_occlusion(
    AudioSource *source
)
{
    if (!source->occluded)
        return 0.0f;

    return clampf(
        source->occlusion,
        0.0f,
        1.0f
    );
}

/* ============================================================
   REVERB
   ============================================================ */

static float calculate_reverb(
    SpatialAudioEngine *engine,
    Vec3 position
)
{
    float result = 0.0f;

    for (int i = 0;
         i < engine->zone_count;
         ++i)
    {
        ReverbZone *zone =
            &engine->zones[i];

        float distance =
            vec_length(
                vec_sub(
                    position,
                    zone->position
                )
            );

        if (distance < zone->radius) {

            float influence =
                1.0f -
                distance / zone->radius;

            result =
                fmaxf(
                    result,
                    influence *
                    zone->wetness
                );
        }
    }

    return clampf(
        result,
        0.0f,
        1.0f
    );
}

/* ============================================================
   SPATIALISE SOURCE
   ============================================================ */

static SpatialResult spatialize_source(
    SpatialAudioEngine *engine,
    AudioSource *source
)
{
    SpatialResult result;

    memset(
        &result,
        0,
        sizeof(result)
    );

    Vec3 source_position =
        source->position;

    if (source->head_locked) {

        /*
            Head-locked sources don't move relative
            to the listener.
        */

        source_position =
            vec_add(
                engine->listener.position,
                vec3(0, 0, -1)
            );
    }

    Vec3 delta =
        vec_sub(
            source_position,
            engine->listener.position
        );

    result.distance =
        vec_length(delta);

    calculate_direction_angles(
        &engine->listener,
        source_position,
        &result.azimuth,
        &result.elevation
    );

    result.attenuation =
        calculate_distance_gain(
            result.distance,
            source->min_distance,
            source->max_distance
        );

    calculate_stereo_pan(
        result.azimuth,
        &result.left_gain,
        &result.right_gain
    );

    result.doppler =
        calculate_doppler(
            &engine->listener,
            source,
            engine->speed_of_sound
        );

    result.occlusion =
        calculate_occlusion(source);

    result.reverb =
        calculate_reverb(
            engine,
            source_position
        );

    /*
        Occlusion reduces volume.
    */

    float occlusion_gain =
        1.0f -
        result.occlusion * 0.75f;

    result.attenuation *=
        occlusion_gain;

    /*
        Occlusion also creates a low-pass
        effect.
    */

    result.lowpass =
        1.0f -
        result.occlusion * 0.85f;

    return result;
}

/* ============================================================
   SOURCE SAMPLE
   ============================================================ */

static float source_next_sample(
    AudioSource *source
)
{
    if (!source->samples ||
        source->sample_count == 0)
    {
        return 0.0f;
    }

    if (source->cursor >=
        source->sample_count)
    {
        if (source->looping)
            source->cursor = 0;
        else {
            source->active = false;
            return 0.0f;
        }
    }

    return source->samples[
        source->cursor++
    ];
}

/* ============================================================
   SIMPLE LOW PASS
   ============================================================ */

typedef struct {

    float previous;
    float coefficient;

} LowPassFilter;

static void lowpass_init(
    LowPassFilter *filter,
    float coefficient
)
{
    filter->previous = 0.0f;

    filter->coefficient =
        clampf(
            coefficient,
            0.0f,
            1.0f
        );
}

static float lowpass_process(
    LowPassFilter *filter,
    float input
)
{
    filter->previous =
        lerpf(
            filter->previous,
            input,
            filter->coefficient
        );

    return filter->previous;
}

/* ============================================================
   SIMPLE REVERB
   ============================================================ */

#define REVERB_BUFFER_SIZE 48000

typedef struct {

    float buffer[REVERB_BUFFER_SIZE];

    int index;

    float feedback;

    float wet;

} SimpleReverb;

static void reverb_init(
    SimpleReverb *reverb
)
{
    memset(
        reverb,
        0,
        sizeof(*reverb)
    );

    reverb->feedback = 0.65f;

    reverb->wet = 0.25f;
}

static float reverb_process(
    SimpleReverb *reverb,
    float input
)
{
    float delayed =
        reverb->buffer[
            reverb->index
        ];

    float output =
        input +
        delayed * reverb->wet;

    reverb->buffer[
        reverb->index
    ] =
        input +
        delayed *
        reverb->feedback;

    reverb->index++;

    if (reverb->index >=
        REVERB_BUFFER_SIZE)
    {
        reverb->index = 0;
    }

    return output;
}

/* ============================================================
   AUDIO MIXER
   ============================================================ */

typedef struct {

    float left;
    float right;

} StereoSample;

static StereoSample mix_sources(
    SpatialAudioEngine *engine
)
{
    StereoSample output;

    output.left = 0.0f;
    output.right = 0.0f;

    for (int i = 0;
         i < engine->source_count;
         ++i)
    {
        AudioSource *source =
            &engine->sources[i];

        if (!source->active)
            continue;

        SpatialResult spatial =
            spatialize_source(
                engine,
                source
            );

        float sample =
            source_next_sample(
                source
            );

        sample *=
            source->volume;

        sample *=
            spatial.attenuation;

        /*
            Doppler modifies pitch in a real
            resampler. Here it is calculated
            and exposed for the audio backend.
        */

        float doppler =
            spatial.doppler *
            source->doppler_factor;

        (void)doppler;

        /*
            Stereo spatialisation.
        */

        output.left +=
            sample *
            spatial.left_gain;

        output.right +=
            sample *
            spatial.right_gain;

        /*
            Reverb contribution.
        */

        float reverb_amount =
            spatial.reverb *
            source->reverb_send;

        output.left +=
            sample *
            reverb_amount *
            0.15f;

        output.right +=
            sample *
            reverb_amount *
            0.15f;
    }

    output.left *=
        engine->master_volume;

    output.right *=
        engine->master_volume;

    output.left =
        clampf(
            output.left,
            -1.0f,
            1.0f
        );

    output.right =
        clampf(
            output.right,
            -1.0f,
            1.0f
        );

    return output;
}

/* ============================================================
   ROOM / ACOUSTIC MATERIALS
   ============================================================ */

typedef enum {

    MATERIAL_CONCRETE,
    MATERIAL_METAL,
    MATERIAL_GLASS,
    MATERIAL_WOOD,
    MATERIAL_CARPET,
    MATERIAL_FABRIC

} AcousticMaterial;

typedef struct {

    float absorption;

    float reflection;

    float diffusion;

} AcousticProperties;

static AcousticProperties
get_acoustic_properties(
    AcousticMaterial material
)
{
    switch (material) {

        case MATERIAL_CONCRETE:
            return (AcousticProperties){
                0.08f,
                0.92f,
                0.35f
            };

        case MATERIAL_METAL:
            return (AcousticProperties){
                0.03f,
                0.97f,
                0.20f
            };

        case MATERIAL_GLASS:
            return (AcousticProperties){
                0.04f,
                0.96f,
                0.10f
            };

        case MATERIAL_WOOD:
            return (AcousticProperties){
                0.30f,
                0.70f,
                0.50f
            };

        case MATERIAL_CARPET:
            return (AcousticProperties){
                0.75f,
                0.25f,
                0.70f
            };

        case MATERIAL_FABRIC:
            return (AcousticProperties){
                0.80f,
                0.20f,
                0.80f
            };
    }

    return (AcousticProperties){
        0.5f,
        0.5f,
        0.5f
    };
}

/* ============================================================
   AUDIO EVENTS
   ============================================================ */

typedef enum {

    AUDIO_EVENT_FOOTSTEP,
    AUDIO_EVENT_BUTTON,
    AUDIO_EVENT_IMPACT,
    AUDIO_EVENT_DOOR,
    AUDIO_EVENT_MACHINE,
    AUDIO_EVENT_VOICE,
    AUDIO_EVENT_AMBIENCE,
    AUDIO_EVENT_UI

} AudioEventType;

typedef struct {

    AudioEventType type;

    Vec3 position;

    float intensity;

    float pitch;

} AudioEvent;

static void audio_event_play(
    SpatialAudioEngine *engine,
    AudioEvent event
)
{
    /*
        In a full engine this would pull an appropriate
        sample from an audio asset manager and create or
        recycle an AudioSource.
    */

    AudioSource *source =
        audio_source_create(engine);

    if (!source)
        return;

    source->position =
        event.position;

    source->volume =
        clampf(
            event.intensity,
            0.0f,
            1.0f
        );

    source->pitch =
        event.pitch;

    source->looping =
        false;

    source->spatialized =
        true;
}

/* ============================================================
   HRTF INTERFACE
   ============================================================ */

typedef struct {

    bool enabled;

    int sample_rate;

    int impulse_response_length;

} HRTFProcessor;

static void hrtf_init(
    HRTFProcessor *hrtf
)
{
    hrtf->enabled = true;

    hrtf->sample_rate =
        AUDIO_SAMPLE_RATE;

    hrtf->impulse_response_length =
        256;
}

/*
    HRTF-ready processing interface.

    Actual Quest-quality binaural rendering should replace this
    with measured HRTF impulse responses and convolution.
*/

static StereoSample hrtf_process(
    HRTFProcessor *hrtf,
    float mono,
    float azimuth,
    float elevation
)
{
    StereoSample result;

    if (!hrtf->enabled) {

        result.left = mono;
        result.right = mono;

        return result;
    }

    float left;
    float right;

    calculate_stereo_pan(
        azimuth,
        &left,
        &right
    );

    /*
        Simple elevation attenuation.

        A real HRTF system would use different
        impulse responses for elevation.
    */

    float elevation_factor =
        1.0f -
        fabsf(
            elevation / (PI * 0.5f)
        ) * 0.15f;

    result.left =
        mono *
        left *
        elevation_factor;

    result.right =
        mono *
        right *
        elevation_factor;

    return result;
}

/* ============================================================
   DEBUG INFORMATION
   ============================================================ */

static void print_spatial_info(
    SpatialAudioEngine *engine,
    AudioSource *source
)
{
    SpatialResult r =
        spatialize_source(
            engine,
            source
        );

    printf(
        "SOURCE %d\n"
        "  Distance:   %.2f m\n"
        "  Azimuth:    %.2f deg\n"
        "  Elevation:  %.2f deg\n"
        "  Attenuation %.3f\n"
        "  Left gain:  %.3f\n"
        "  Right gain: %.3f\n"
        "  Doppler:    %.3f\n"
        "  Occlusion:  %.3f\n"
        "  Reverb:     %.3f\n"
        "  Lowpass:    %.3f\n\n",
        source->id,
        r.distance,
        r.azimuth * 180.0f / PI,
        r.elevation * 180.0f / PI,
        r.attenuation,
        r.left_gain,
        r.right_gain,
        r.doppler,
        r.occlusion,
        r.reverb,
        r.lowpass
    );
}

/* ============================================================
   META QUEST HEAD-TRACKING MOCK
   ============================================================ */

/*
    Replace this with OpenXR calls.

    Typical production flow:

        xrWaitFrame()
        xrBeginFrame()
        xrLocateViews()
        xrLocateSpace()

    The resulting headset pose is passed into
    listener_set_pose().
*/

static void mock_update_headset(
    SpatialAudioEngine *engine,
    float time
)
{
    Vec3 position =
        vec3(
            sinf(time) * 0.05f,
            1.6f,
            cosf(time) * 0.05f
        );

    /*
        Small demonstration head rotation.
    */

    float yaw =
        sinf(time * 0.7f) *
        0.15f;

    Quat orientation;

    orientation.x = 0.0f;
    orientation.y = sinf(yaw * 0.5f);
    orientation.z = 0.0f;
    orientation.w = cosf(yaw * 0.5f);

    listener_set_pose(
        &engine->listener,
        position,
        orientation,
        1.0f / 90.0f
    );
}

/* ============================================================
   DEMO SCENE
   ============================================================ */

static void create_demo_scene(
    SpatialAudioEngine *engine
)
{
    /*
        Central machine.
    */

    AudioSource *machine =
        audio_source_create(engine);

    machine->position =
        vec3(0, 1.2f, -3.0f);

    machine->volume =
        0.5f;

    machine->min_distance =
        1.0f;

    machine->max_distance =
        15.0f;

    machine->looping =
        true;

    machine->reverb_send =
        0.7f;

    /*
        Left speaker.
    */

    AudioSource *left =
        audio_source_create(engine);

    left->position =
        vec3(-4, 1.5f, -2);

    left->volume =
        0.8f;

    left->reverb_send =
        0.2f;

    /*
        Right speaker.
    */

    AudioSource *right =
        audio_source_create(engine);

    right->position =
        vec3(4, 1.5f, -2);

    right->volume =
        0.8f;

    /*
        Head-locked UI sound.
    */

    AudioSource *ui =
        audio_source_create(engine);

    ui->head_locked =
        true;

    ui->spatialized =
        false;

    ui->volume =
        0.25f;

    /*
        Large room reverb.
    */

    ReverbZone *room =
        reverb_zone_create(
            engine,
            vec3(0, 1.5f, -3),
            10.0f
        );

    room->wetness =
        0.35f;

    room->room_size =
        0.9f;

    /*
        Hallway reverb.
    */

    ReverbZone *hall =
        reverb_zone_create(
            engine,
            vec3(0, 1.5f, 8),
            6.0f
        );

    hall->wetness =
        0.65f;

    hall->room_size =
        1.0f;
}

/* ============================================================
   MAIN
   ============================================================ */

int main(void)
{
    SpatialAudioEngine engine;

    audio_engine_init(
        &engine
    );

    HRTFProcessor hrtf;

    hrtf_init(
        &hrtf
    );

    create_demo_scene(
        &engine
    );

    printf(
        "\n"
        "=============================================\n"
        " META QUEST SPATIAL AUDIO ENGINE\n"
        "=============================================\n\n"
    );

    printf(
        "Sample rate: %d Hz\n"
        "Sources:     %d\n"
        "Reverb zones:%d\n"
        "HRTF:        %s\n\n",
        AUDIO_SAMPLE_RATE,
        engine.source_count,
        engine.zone_count,
        hrtf.enabled ? "enabled" : "disabled"
    );

    /*
        Demonstrate head tracking.
    */

    for (int frame = 0;
         frame < 10;
         ++frame)
    {
        float time =
            frame / 90.0f;

        mock_update_headset(
            &engine,
            time
        );

        printf(
            "FRAME %d\n",
            frame
        );

        printf(
            "Head position: "
            "%.3f %.3f %.3f\n",
            engine.listener.position.x,
            engine.listener.position.y,
            engine.listener.position.z
        );

        if (engine.source_count > 0)
        {
            print_spatial_info(
                &engine,
                &engine.sources[0]
            );
        }

        /*
            Generate one stereo sample.
        */

        StereoSample output =
            mix_sources(
                &engine
            );

        printf(
            "Mixed audio: L=%+.4f R=%+.4f\n\n",
            output.left,
            output.right
        );
    }

    /*
        Demonstrate HRTF processing.
    */

    StereoSample binaural =
        hrtf_process(
            &hrtf,
            0.5f,
            PI * 0.5f,
            0.0f
        );

    printf(
        "HRTF test:\n"
        "  Left:  %.4f\n"
        "  Right: %.4f\n",
        binaural.left,
        binaural.right
    );

    printf(
        "\nSpatial audio engine shutdown.\n"
    );

    return 0;
}




/*
    vr_voice_enhancement.c
    ------------------------------------------------------------
    #5 — Real-Time Voice Enhancement
    Meta Quest / VR spatial communications C foundation

    Pipeline:

        Microphone
            |
            v
        DC Removal
            |
            v
        Noise Gate
            |
            v
        Voice Activity Detection
            |
            v
        Noise Reduction
            |
            v
        High-Pass Filter
            |
            v
        Voice EQ
            |
            v
        Compressor
            |
            v
        Automatic Gain Control
            |
            v
        De-Reverberation
            |
            v
        Limiter
            |
            v
        Spatial Voice Output

    C11 / C17
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>

#define SAMPLE_RATE        48000
#define CHANNELS           1
#define FRAME_SIZE         480
#define MAX_VOICE_USERS    32

#define PI 3.14159265358979323846f

/* ============================================================
   UTILITIES
   ============================================================ */

static float clampf(
    float x,
    float min_value,
    float max_value
)
{
    if (x < min_value)
        return min_value;

    if (x > max_value)
        return max_value;

    return x;
}

static float db_to_linear(float db)
{
    return powf(10.0f, db / 20.0f);
}

static float linear_to_db(float x)
{
    if (x <= 0.000001f)
        return -120.0f;

    return 20.0f * log10f(x);
}

/* ============================================================
   AUDIO FRAME
   ============================================================ */

typedef struct {

    float samples[FRAME_SIZE];

    int count;

} AudioFrame;

/* ============================================================
   BIQUAD FILTER
   ============================================================ */

typedef struct {

    float b0;
    float b1;
    float b2;

    float a1;
    float a2;

    float x1;
    float x2;

    float y1;
    float y2;

} Biquad;

/* ============================================================
   BIQUAD PROCESSOR
   ============================================================ */

static float biquad_process(
    Biquad *filter,
    float input
)
{
    float output =
        filter->b0 * input +
        filter->b1 * filter->x1 +
        filter->b2 * filter->x2 -
        filter->a1 * filter->y1 -
        filter->a2 * filter->y2;

    filter->x2 = filter->x1;
    filter->x1 = input;

    filter->y2 = filter->y1;
    filter->y1 = output;

    return output;
}

/* ============================================================
   LOW-PASS FILTER
   ============================================================ */

static void biquad_lowpass(
    Biquad *filter,
    float frequency,
    float q
)
{
    float omega =
        2.0f * PI *
        frequency /
        SAMPLE_RATE;

    float alpha =
        sinf(omega) /
        (2.0f * q);

    float cosw =
        cosf(omega);

    float b0 =
        (1.0f - cosw) / 2.0f;

    float b1 =
        1.0f - cosw;

    float b2 =
        (1.0f - cosw) / 2.0f;

    float a0 =
        1.0f + alpha;

    float a1 =
        -2.0f * cosw;

    float a2 =
        1.0f - alpha;

    filter->b0 = b0 / a0;
    filter->b1 = b1 / a0;
    filter->b2 = b2 / a0;

    filter->a1 = a1 / a0;
    filter->a2 = a2 / a0;

    filter->x1 = 0;
    filter->x2 = 0;
    filter->y1 = 0;
    filter->y2 = 0;
}

/* ============================================================
   HIGH-PASS FILTER
   ============================================================ */

static void biquad_highpass(
    Biquad *filter,
    float frequency,
    float q
)
{
    float omega =
        2.0f * PI *
        frequency /
        SAMPLE_RATE;

    float alpha =
        sinf(omega) /
        (2.0f * q);

    float cosw =
        cosf(omega);

    float b0 =
        (1.0f + cosw) / 2.0f;

    float b1 =
        -(1.0f + cosw);

    float b2 =
        (1.0f + cosw) / 2.0f;

    float a0 =
        1.0f + alpha;

    float a1 =
        -2.0f * cosw;

    float a2 =
        1.0f - alpha;

    filter->b0 = b0 / a0;
    filter->b1 = b1 / a0;
    filter->b2 = b2 / a0;

    filter->a1 = a1 / a0;
    filter->a2 = a2 / a0;

    filter->x1 = 0;
    filter->x2 = 0;
    filter->y1 = 0;
    filter->y2 = 0;
}

/* ============================================================
   PEAK EQ FILTER
   ============================================================ */

static void biquad_peaking(
    Biquad *filter,
    float frequency,
    float gain_db,
    float q
)
{
    float omega =
        2.0f * PI *
        frequency /
        SAMPLE_RATE;

    float alpha =
        sinf(omega) /
        (2.0f * q);

    float A =
        powf(
            10.0f,
            gain_db / 40.0f
        );

    float cosw =
        cosf(omega);

    float b0 =
        1.0f +
        alpha * A;

    float b1 =
        -2.0f * cosw;

    float b2 =
        1.0f -
        alpha * A;

    float a0 =
        1.0f +
        alpha / A;

    float a1 =
        -2.0f * cosw;

    float a2 =
        1.0f -
        alpha / A;

    filter->b0 = b0 / a0;
    filter->b1 = b1 / a0;
    filter->b2 = b2 / a0;

    filter->a1 = a1 / a0;
    filter->a2 = a2 / a0;

    filter->x1 = 0;
    filter->x2 = 0;
    filter->y1 = 0;
    filter->y2 = 0;
}

/* ============================================================
   NOISE PROFILE
   ============================================================ */

typedef struct {

    float energy;

    float floor;

    float adaptation;

} NoiseProfile;

static void noise_profile_init(
    NoiseProfile *profile
)
{
    profile->energy = 0.0f;

    profile->floor =
        0.003f;

    profile->adaptation =
        0.002f;
}

/* ============================================================
   NOISE ESTIMATION
   ============================================================ */

static float estimate_noise(
    NoiseProfile *profile,
    const float *samples,
    int count,
    bool voice_active
)
{
    float energy = 0.0f;

    for (int i = 0;
         i < count;
         ++i)
    {
        energy +=
            samples[i] *
            samples[i];
    }

    energy /=
        (float)count;

    energy =
        sqrtf(energy);

    /*
        Only update the noise estimate when
        speech is probably absent.
    */

    if (!voice_active) {

        profile->energy =
            profile->energy *
            (1.0f -
             profile->adaptation)
            +
            energy *
            profile->adaptation;
    }

    if (profile->energy <
        profile->floor)
    {
        profile->energy =
            profile->floor;
    }

    return profile->energy;
}

/* ============================================================
   VOICE ACTIVITY DETECTOR
   ============================================================ */

typedef struct {

    float threshold;

    float attack;

    float release;

    float probability;

    bool active;

} VoiceActivityDetector;

static void vad_init(
    VoiceActivityDetector *vad
)
{
    vad->threshold =
        0.012f;

    vad->attack =
        0.20f;

    vad->release =
        0.05f;

    vad->probability =
        0.0f;

    vad->active =
        false;
}

static bool vad_process(
    VoiceActivityDetector *vad,
    const float *samples,
    int count
)
{
    float energy = 0.0f;

    for (int i = 0;
         i < count;
         ++i)
    {
        energy +=
            samples[i] *
            samples[i];
    }

    energy =
        sqrtf(
            energy /
            (float)count
        );

    float target =
        energy >
        vad->threshold
        ? 1.0f
        : 0.0f;

    float smoothing =
        target >
        vad->probability
        ? vad->attack
        : vad->release;

    vad->probability =
        vad->probability *
        (1.0f - smoothing)
        +
        target *
        smoothing;

    vad->active =
        vad->probability >
        0.45f;

    return vad->active;
}

/* ============================================================
   NOISE REDUCTION
   ============================================================ */

typedef struct {

    NoiseProfile profile;

    float strength;

} NoiseReducer;

static void noise_reducer_init(
    NoiseReducer *nr
)
{
    noise_profile_init(
        &nr->profile
    );

    nr->strength =
        0.75f;
}

static void noise_reducer_process(
    NoiseReducer *nr,
    float *samples,
    int count,
    bool voice_active
)
{
    float noise =
        estimate_noise(
            &nr->profile,
            samples,
            count,
            voice_active
        );

    float reduction =
        clampf(
            noise *
            nr->strength *
            8.0f,
            0.0f,
            0.85f
        );

    for (int i = 0;
         i < count;
         ++i)
    {
        float x =
            samples[i];

        /*
            Soft spectral-style suppression
            approximation.

            Production implementation can replace
            this with STFT/Wiener filtering.
        */

        float magnitude =
            fabsf(x);

        float threshold =
            noise * 1.5f;

        if (magnitude <
            threshold)
        {
            samples[i] *=
                (1.0f - reduction);
        }
        else {

            float excess =
                magnitude -
                threshold;

            float gain =
                1.0f -
                reduction *
                expf(
                    -excess * 20.0f
                );

            samples[i] *=
                gain;
        }
    }
}

/* ============================================================
   NOISE GATE
   ============================================================ */

typedef struct {

    float threshold;

    float attack;

    float release;

    float gain;

} NoiseGate;

static void noise_gate_init(
    NoiseGate *gate
)
{
    gate->threshold =
        db_to_linear(-48.0f);

    gate->attack =
        0.05f;

    gate->release =
        0.005f;

    gate->gain =
        1.0f;
}

static float noise_gate_process(
    NoiseGate *gate,
    float sample
)
{
    float magnitude =
        fabsf(sample);

    float target =
        magnitude >
        gate->threshold
        ? 1.0f
        : 0.0f;

    float smoothing =
        target >
        gate->gain
        ? gate->attack
        : gate->release;

    gate->gain =
        gate->gain *
        (1.0f - smoothing)
        +
        target *
        smoothing;

    return sample *
           gate->gain;
}

/* ============================================================
   COMPRESSOR
   ============================================================ */

typedef struct {

    float threshold_db;

    float ratio;

    float attack;

    float release;

    float envelope;

    float makeup_gain;

} Compressor;

static void compressor_init(
    Compressor *compressor
)
{
    compressor->threshold_db =
        -18.0f;

    compressor->ratio =
        3.0f;

    compressor->attack =
        0.01f;

    compressor->release =
        0.10f;

    compressor->envelope =
        0.0f;

    compressor->makeup_gain =
        db_to_linear(3.0f);
}

static float compressor_process(
    Compressor *compressor,
    float input
)
{
    float magnitude =
        fabsf(input);

    float coefficient =
        magnitude >
        compressor->envelope
        ? compressor->attack
        : compressor->release;

    compressor->envelope =
        compressor->envelope *
        (1.0f - coefficient)
        +
        magnitude *
        coefficient;

    float level_db =
        linear_to_db(
            compressor->envelope
        );

    float gain_db =
        0.0f;

    if (level_db >
        compressor->threshold_db)
    {
        float excess =
            level_db -
            compressor->threshold_db;

        float compressed =
            excess /
            compressor->ratio;

        gain_db =
            compressed -
            excess;
    }

    float gain =
        db_to_linear(
            gain_db
        );

    return input *
           gain *
           compressor->makeup_gain;
}

/* ============================================================
   AUTOMATIC GAIN CONTROL
   ============================================================ */

typedef struct {

    float target_level;

    float current_gain;

    float attack;

    float release;

    float maximum_gain;

} AGC;

static void agc_init(
    AGC *agc
)
{
    agc->target_level =
        db_to_linear(-12.0f);

    agc->current_gain =
        1.0f;

    agc->attack =
        0.005f;

    agc->release =
        0.0005f;

    agc->maximum_gain =
        db_to_linear(12.0f);
}

static float agc_process(
    AGC *agc,
    float input
)
{
    float magnitude =
        fabsf(input);

    if (magnitude >
        0.000001f)
    {
        float desired =
            agc->target_level /
            magnitude;

        desired =
            clampf(
                desired,
                0.1f,
                agc->maximum_gain
            );

        float coefficient =
            desired <
            agc->current_gain
            ? agc->attack
            : agc->release;

        agc->current_gain =
            agc->current_gain *
            (1.0f - coefficient)
            +
            desired *
            coefficient;
    }

    return input *
           agc->current_gain;
}

/* ============================================================
   DE-ESSER
   ============================================================ */

typedef struct {

    Biquad high_band;

    float threshold;

    float reduction;

} DeEsser;

static void deesser_init(
    DeEsser *deesser
)
{
    biquad_highpass(
        &deesser->high_band,
        4500.0f,
        0.707f
    );

    deesser->threshold =
        0.25f;

    deesser->reduction =
        0.35f;
}

static float deesser_process(
    DeEsser *deesser,
    float input
)
{
    float high =
        biquad_process(
            &deesser->high_band,
            input
        );

    float magnitude =
        fabsf(high);

    if (magnitude >
        deesser->threshold)
    {
        float excess =
            magnitude -
            deesser->threshold;

        float gain =
            1.0f -
            clampf(
                excess *
                deesser->reduction,
                0.0f,
                0.7f
            );

        return input * gain;
    }

    return input;
}

/* ============================================================
   LIMITER
   ============================================================ */

typedef struct {

    float ceiling;

} Limiter;

static void limiter_init(
    Limiter *limiter
)
{
    limiter->ceiling =
        db_to_linear(-1.0f);
}

static float limiter_process(
    Limiter *limiter,
    float input
)
{
    if (input >
        limiter->ceiling)
    {
        return limiter->ceiling;
    }

    if (input <
        -limiter->ceiling)
    {
        return -limiter->ceiling;
    }

    return input;
}

/* ============================================================
   VOICE PROCESSOR
   ============================================================ */

typedef struct {

    VoiceActivityDetector vad;

    NoiseReducer noise_reducer;

    NoiseGate gate;

    Compressor compressor;

    AGC agc;

    DeEsser deesser;

    Limiter limiter;

    Biquad highpass;

    Biquad voice_eq;

    float output_gain;

    bool enabled;

} VoiceProcessor;

/* ============================================================
   VOICE PROCESSOR INITIALISATION
   ============================================================ */

static void voice_processor_init(
    VoiceProcessor *processor
)
{
    memset(
        processor,
        0,
        sizeof(*processor)
    );

    vad_init(
        &processor->vad
    );

    noise_reducer_init(
        &processor->noise_reducer
    );

    noise_gate_init(
        &processor->gate
    );

    compressor_init(
        &processor->compressor
    );

    agc_init(
        &processor->agc
    );

    deesser_init(
        &processor->deesser
    );

    limiter_init(
        &processor->limiter
    );

    /*
        Remove low-frequency rumble.

        Useful for:
        - controller handling
        - footsteps
        - air conditioning
        - desk vibration
        - headset movement
    */

    biquad_highpass(
        &processor->highpass,
        80.0f,
        0.707f
    );

    /*
        Voice presence EQ.

        Boosting the speech region makes speech
        intelligible without simply increasing
        overall volume.
    */

    biquad_peaking(
        &processor->voice_eq,
        2500.0f,
        3.0f,
        0.8f
    );

    processor->output_gain =
        1.0f;

    processor->enabled =
        true;
}

/* ============================================================
   VOICE PROCESSING PIPELINE
   ============================================================ */

static void voice_process_frame(
    VoiceProcessor *processor,
    AudioFrame *frame
)
{
    if (!processor->enabled)
        return;

    /*
        1. Detect voice.
    */

    bool voice_active =
        vad_process(
            &processor->vad,
            frame->samples,
            frame->count
        );

    /*
        2. Remove stationary noise.
    */

    noise_reducer_process(
        &processor->noise_reducer,
        frame->samples,
        frame->count,
        voice_active
    );

    /*
        3. Process individual samples.
    */

    for (int i = 0;
         i < frame->count;
         ++i)
    {
        float sample =
            frame->samples[i];

        /*
            High-pass.
        */

        sample =
            biquad_process(
                &processor->highpass,
                sample
            );

        /*
            Voice clarity EQ.
        */

        sample =
            biquad_process(
                &processor->voice_eq,
                sample
            );

        /*
            Gate.
        */

        sample =
            noise_gate_process(
                &processor->gate,
                sample
            );

        /*
            Compression.
        */

        sample =
            compressor_process(
                &processor->compressor,
                sample
            );

        /*
            Automatic microphone levelling.
        */

        sample =
            agc_process(
                &processor->agc,
                sample
            );

        /*
            De-essing.
        */

        sample =
            deesser_process(
                &processor->deesser,
                sample
            );

        /*
            Final limiter.
        */

        sample =
            limiter_process(
                &processor->limiter,
                sample
            );

        sample *=
            processor->output_gain;

        frame->samples[i] =
            sample;
    }
}

/* ============================================================
   VOICE QUALITY METRICS
   ============================================================ */

typedef struct {

    float rms;

    float peak;

    float db;

    float noise_floor;

    float speech_probability;

    bool voice_active;

} VoiceMetrics;

static VoiceMetrics
calculate_voice_metrics(
    VoiceProcessor *processor,
    const AudioFrame *frame
)
{
    VoiceMetrics metrics;

    memset(
        &metrics,
        0,
        sizeof(metrics)
    );

    float energy = 0.0f;

    for (int i = 0;
         i < frame->count;
         ++i)
    {
        float magnitude =
            fabsf(
                frame->samples[i]
            );

        if (magnitude >
            metrics.peak)
        {
            metrics.peak =
                magnitude;
        }

        energy +=
            frame->samples[i] *
            frame->samples[i];
    }

    metrics.rms =
        sqrtf(
            energy /
            (float)frame->count
        );

    metrics.db =
        linear_to_db(
            metrics.rms
        );

    metrics.noise_floor =
        processor->
            noise_reducer.
            profile.energy;

    metrics.speech_probability =
        processor->
            vad.probability;

    metrics.voice_active =
        processor->
            vad.active;

    return metrics;
}

/* ============================================================
   NETWORK VOICE PACKET
   ============================================================ */

typedef struct {

    uint32_t sequence;

    uint32_t timestamp;

    uint16_t sample_count;

    uint8_t voice_activity;

    float gain;

    float samples[FRAME_SIZE];

} VoicePacket;

/*
    A real multiplayer implementation would normally encode
    this with Opus rather than sending raw PCM.
*/

static VoicePacket encode_voice_packet(
    const AudioFrame *frame,
    uint32_t sequence,
    uint32_t timestamp,
    bool voice_active
)
{
    VoicePacket packet;

    memset(
        &packet,
        0,
        sizeof(packet)
    );

    packet.sequence =
        sequence;

    packet.timestamp =
        timestamp;

    packet.sample_count =
        (uint16_t)frame->count;

    packet.voice_activity =
        voice_active ? 1 : 0;

    memcpy(
        packet.samples,
        frame->samples,
        sizeof(float) *
        frame->count
    );

    return packet;
}

/* ============================================================
   REMOTE VOICE USER
   ============================================================ */

typedef struct {

    bool active;

    uint32_t id;

    float volume;

    float spatial_volume;

    float network_jitter;

    float packet_loss;

    float interpolation;

    float distance;

} RemoteVoiceUser;

/* ============================================================
   VOICE CHAT ENGINE
   ============================================================ */

typedef struct {

    VoiceProcessor microphone;

    RemoteVoiceUser users[
        MAX_VOICE_USERS
    ];

    int user_count;

    float master_volume;

    bool push_to_talk;

    bool microphone_muted;

} VoiceChatEngine;

/* ============================================================
   VOICE CHAT INITIALISATION
   ============================================================ */

static void voice_chat_init(
    VoiceChatEngine *chat
)
{
    memset(
        chat,
        0,
        sizeof(*chat)
    );

    voice_processor_init(
        &chat->microphone
    );

    chat->master_volume =
        1.0f;

    chat->push_to_talk =
        false;

    chat->microphone_muted =
        false;
}

/* ============================================================
   REMOTE VOICE USER
   ============================================================ */

static RemoteVoiceUser *
voice_user_create(
    VoiceChatEngine *chat,
    uint32_t id
)
{
    if (chat->user_count >=
        MAX_VOICE_USERS)
    {
        return NULL;
    }

    RemoteVoiceUser *user =
        &chat->users[
            chat->user_count++
        ];

    memset(
        user,
        0,
        sizeof(*user)
    );

    user->active =
        true;

    user->id =
        id;

    user->volume =
        1.0f;

    user->spatial_volume =
        1.0f;

    user->interpolation =
        0.5f;

    return user;
}

/* ============================================================
   DISTANCE VOICE ATTENUATION
   ============================================================ */

static float voice_distance_gain(
    float distance
)
{
    const float near_distance =
        0.5f;

    const float far_distance =
        30.0f;

    if (distance <=
        near_distance)
    {
        return 1.0f;
    }

    if (distance >=
        far_distance)
    {
        return 0.0f;
    }

    float gain =
        near_distance /
        distance;

    return clampf(
        gain,
        0.0f,
        1.0f
    );
}

/* ============================================================
   SPATIAL VOICE
   ============================================================ */

typedef struct {

    float left;

    float right;

} VoiceStereo;

/*
    Simple spatial voice mixer.

    Production version can feed this into the same
    HRTF engine used by #4.
*/

static VoiceStereo spatial_voice_mix(
    float mono_voice,
    float azimuth,
    float distance,
    float user_volume
)
{
    VoiceStereo output;

    float pan =
        sinf(azimuth);

    pan =
        clampf(
            pan,
            -1.0f,
            1.0f
        );

    float angle =
        (pan + 1.0f) *
        0.25f *
        PI;

    float left =
        cosf(angle);

    float right =
        sinf(angle);

    float distance_gain =
        voice_distance_gain(
            distance
        );

    float gain =
        user_volume *
        distance_gain;

    output.left =
        mono_voice *
        left *
        gain;

    output.right =
        mono_voice *
        right *
        gain;

    return output;
}

/* ============================================================
   MICROPHONE FRAME PROCESSING
   ============================================================ */

static bool process_microphone(
    VoiceChatEngine *chat,
    AudioFrame *frame
)
{
    if (chat->microphone_muted)
    {
        memset(
            frame->samples,
            0,
            sizeof(frame->samples)
        );

        return false;
    }

    if (chat->push_to_talk)
    {
        /*
            Push-to-talk button state would be supplied
            by the Quest controller layer.
        */
    }

    voice_process_frame(
        &chat->microphone,
        frame
    );

    return chat->
        microphone.
        vad.
        active;
}

/* ============================================================
   DEBUG
   ============================================================ */

static void print_voice_metrics(
    VoiceProcessor *processor,
    const AudioFrame *frame
)
{
    VoiceMetrics metrics =
        calculate_voice_metrics(
            processor,
            frame
        );

    printf(
        "VOICE METRICS\n"
        "  RMS:              %.5f\n"
        "  Level:            %.2f dB\n"
        "  Peak:             %.5f\n"
        "  Noise floor:      %.5f\n"
        "  Voice probability %.3f\n"
        "  Voice active:     %s\n\n",
        metrics.rms,
        metrics.db,
        metrics.peak,
        metrics.noise_floor,
        metrics.speech_probability,
        metrics.voice_active
            ? "YES"
            : "NO"
    );
}

/* ============================================================
   DEMONSTRATION INPUT
   ============================================================ */

static void generate_test_voice(
    AudioFrame *frame,
    float time
)
{
    for (int i = 0;
         i < frame->count;
         ++i)
    {
        float t =
            time +
            (float)i /
            SAMPLE_RATE;

        /*
            Simulated voice fundamental
            plus harmonics.
        */

        float voice =
            sinf(
                2.0f *
                PI *
                140.0f *
                t
            ) * 0.15f;

        voice +=
            sinf(
                2.0f *
                PI *
                280.0f *
                t
            ) * 0.06f;

        voice +=
            sinf(
                2.0f *
                PI *
                700.0f *
                t
            ) * 0.025f;

        /*
            Simulated background noise.
        */

        float noise =
            sinf(
                2.0f *
                PI *
                60.0f *
                t
            ) * 0.008f;

        frame->samples[i] =
            voice +
            noise;
    }
}

/* ============================================================
   MAIN
   ============================================================ */

int main(void)
{
    VoiceChatEngine chat;

    voice_chat_init(
        &chat
    );

    /*
        Create remote users.
    */

    voice_user_create(
        &chat,
        1001
    );

    voice_user_create(
        &chat,
        1002
    );

    voice_user_create(
        &chat,
        1003
    );

    printf(
        "\n"
        "=============================================\n"
        " META QUEST REAL-TIME VOICE ENGINE\n"
        "=============================================\n\n"
    );

    printf(
        "Sample rate:     %d Hz\n"
        "Frame size:      %d samples\n"
        "Remote users:    %d\n"
        "Noise reduction: enabled\n"
        "Voice detection: enabled\n"
        "Compression:     enabled\n"
        "AGC:             enabled\n"
        "De-esser:        enabled\n"
        "Limiter:         enabled\n\n",
        SAMPLE_RATE,
        FRAME_SIZE,
        chat.user_count
    );

    /*
        Process simulated microphone frames.
    */

    for (int frame_number = 0;
         frame_number < 10;
         ++frame_number)
    {
        AudioFrame frame;

        memset(
            &frame,
            0,
            sizeof(frame)
        );

        frame.count =
            FRAME_SIZE;

        generate_test_voice(
            &frame,
            (float)frame_number *
            FRAME_SIZE /
            SAMPLE_RATE
        );

        bool transmitting =
            process_microphone(
                &chat,
                &frame
            );

        printf(
            "FRAME %02d | transmitting=%s\n",
            frame_number,
            transmitting
                ? "YES"
                : "NO"
        );

        print_voice_metrics(
            &chat.microphone,
            &frame
        );

        /*
            Example voice packet.
        */

        VoicePacket packet =
            encode_voice_packet(
                &frame,
                (uint32_t)frame_number,
                (uint32_t)(
                    frame_number *
                    FRAME_SIZE
                ),
                transmitting
            );

        printf(
            "Packet: sequence=%u "
            "samples=%u "
            "voice=%u\n\n",
            packet.sequence,
            packet.sample_count,
            packet.voice_activity
        );
    }

    /*
        Example remote spatial voice.
    */

    printf(
        "SPATIAL VOICE TEST\n"
    );

    float angles[] = {
        -PI,
        -PI * 0.5f,
        0.0f,
        PI * 0.5f,
        PI
    };

    for (int i = 0;
         i < 5;
         ++i)
    {
        VoiceStereo stereo =
            spatial_voice_mix(
                0.5f,
                angles[i],
                3.0f,
                1.0f
            );

        printf(
            "Angle %+.2f rad -> "
            "L=%+.3f R=%+.3f\n",
            angles[i],
            stereo.left,
            stereo.right
        );
    }

    printf(
        "\nVoice engine shutdown.\n"
    );

    return 0;
}


/*
 * wind_noise_cancellation.c
 *
 * Meta Quest VR - Wind Noise Cancellation Engine
 *
 * C11 / C17
 *
 * Designed as a platform-independent DSP core.
 *
 * Intended pipeline:
 *
 *   Microphone Array
 *          |
 *          v
 *   DC / Rumble Removal
 *          |
 *          v
 *   Wind Detection
 *          |
 *          v
 *   Wind Profile Estimation
 *          |
 *          v
 *   Adaptive High-Pass
 *          |
 *          v
 *   Low-Frequency Suppression
 *          |
 *          v
 *   Voice Preservation
 *          |
 *          v
 *   Noise Gate
 *          |
 *          v
 *   Limiter
 *          |
 *          v
 *   Clean Voice
 *
 * Production integration:
 *   OpenXR + Android NDK + Quest microphone/audio backend
 *
 * This file intentionally keeps the hardware interface abstract.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <stdbool.h>

/* ============================================================
   CONFIGURATION
   ============================================================ */

#define WNC_SAMPLE_RATE       48000
#define WNC_FRAME_SIZE        480
#define WNC_MAX_MICS          4

#define WNC_PI                3.14159265358979323846f

#define WNC_MIN_HP_HZ         70.0f
#define WNC_MAX_HP_HZ         900.0f

#define WNC_WIND_ATTACK       0.08f
#define WNC_WIND_RELEASE      0.01f

#define WNC_VOICE_THRESHOLD   0.012f

#define WNC_MAX_GAIN          2.0f
#define WNC_MIN_GAIN          0.08f

/* ============================================================
   UTILITY FUNCTIONS
   ============================================================ */

static float clampf(float x, float lo, float hi)
{
    if (x < lo) return lo;
    if (x > hi) return hi;
    return x;
}

static float lerpf(float a, float b, float t)
{
    return a + (b - a) * t;
}

static float db_to_linear(float db)
{
    return powf(10.0f, db / 20.0f);
}

static float linear_to_db(float x)
{
    if (x <= 0.0000001f)
        return -140.0f;

    return 20.0f * log10f(x);
}

/* ============================================================
   AUDIO BUFFER
   ============================================================ */

typedef struct
{
    float samples[WNC_FRAME_SIZE];
    int count;
} AudioFrame;

/* ============================================================
   MICROPHONE ARRAY
   ============================================================ */

typedef struct
{
    float samples[WNC_MAX_MICS][WNC_FRAME_SIZE];

    int microphone_count;
    int sample_count;

} MicrophoneArrayFrame;

/* ============================================================
   BIQUAD FILTER
   ============================================================ */

typedef struct
{
    float b0;
    float b1;
    float b2;

    float a1;
    float a2;

    float x1;
    float x2;

    float y1;
    float y2;

} Biquad;

static void biquad_reset(Biquad *f)
{
    memset(f, 0, sizeof(*f));
}

/* ------------------------------------------------------------
   Low-pass
   ------------------------------------------------------------ */

static void biquad_lowpass(
    Biquad *f,
    float sample_rate,
    float frequency,
    float q)
{
    float w0 = 2.0f * WNC_PI * frequency / sample_rate;

    float c = cosf(w0);
    float s = sinf(w0);

    float alpha = s / (2.0f * q);

    float b0 = (1.0f - c) * 0.5f;
    float b1 = 1.0f - c;
    float b2 = (1.0f - c) * 0.5f;

    float a0 = 1.0f + alpha;
    float a1 = -2.0f * c;
    float a2 = 1.0f - alpha;

    f->b0 = b0 / a0;
    f->b1 = b1 / a0;
    f->b2 = b2 / a0;

    f->a1 = a1 / a0;
    f->a2 = a2 / a0;
}

/* ------------------------------------------------------------
   High-pass
   ------------------------------------------------------------ */

static void biquad_highpass(
    Biquad *f,
    float sample_rate,
    float frequency,
    float q)
{
    float w0 = 2.0f * WNC_PI * frequency / sample_rate;

    float c = cosf(w0);
    float s = sinf(w0);

    float alpha = s / (2.0f * q);

    float b0 = (1.0f + c) * 0.5f;
    float b1 = -(1.0f + c);
    float b2 = (1.0f + c) * 0.5f;

    float a0 = 1.0f + alpha;
    float a1 = -2.0f * c;
    float a2 = 1.0f - alpha;

    f->b0 = b0 / a0;
    f->b1 = b1 / a0;
    f->b2 = b2 / a0;

    f->a1 = a1 / a0;
    f->a2 = a2 / a0;
}

/* ------------------------------------------------------------
   Band-pass
   ------------------------------------------------------------ */

static void biquad_bandpass(
    Biquad *f,
    float sample_rate,
    float frequency,
    float q)
{
    float w0 = 2.0f * WNC_PI * frequency / sample_rate;

    float c = cosf(w0);
    float s = sinf(w0);

    float alpha = s / (2.0f * q);

    float b0 = alpha;
    float b1 = 0.0f;
    float b2 = -alpha;

    float a0 = 1.0f + alpha;
    float a1 = -2.0f * c;
    float a2 = 1.0f - alpha;

    f->b0 = b0 / a0;
    f->b1 = b1 / a0;
    f->b2 = b2 / a0;

    f->a1 = a1 / a0;
    f->a2 = a2 / a0;
}

/* ------------------------------------------------------------
   Process one sample
   ------------------------------------------------------------ */

static float biquad_process(
    Biquad *f,
    float x)
{
    float y =
        f->b0 * x +
        f->b1 * f->x1 +
        f->b2 * f->x2 -
        f->a1 * f->y1 -
        f->a2 * f->y2;

    f->x2 = f->x1;
    f->x1 = x;

    f->y2 = f->y1;
    f->y1 = y;

    return y;
}

/* ============================================================
   ENERGY ESTIMATION
   ============================================================ */

static float frame_rms(
    const float *samples,
    int count)
{
    double energy = 0.0;

    for (int i = 0; i < count; ++i)
        energy += samples[i] * samples[i];

    return sqrtf((float)(energy / (double)count));
}

static float frame_peak(
    const float *samples,
    int count)
{
    float peak = 0.0f;

    for (int i = 0; i < count; ++i)
    {
        float a = fabsf(samples[i]);

        if (a > peak)
            peak = a;
    }

    return peak;
}

/* ============================================================
   WIND PROFILE
   ============================================================ */

typedef struct
{
    float low_frequency_energy;
    float mid_frequency_energy;
    float high_frequency_energy;

    float low_ratio;

    float turbulence;
    float coherence;

    float wind_strength;

    bool wind_detected;

} WindProfile;

/* ============================================================
   WIND DETECTOR
   ============================================================ */

typedef struct
{
    Biquad lowpass;
    Biquad bandpass;
    Biquad highpass;

    float previous_energy;

    float wind_strength;

    float attack;
    float release;

} WindDetector;

/* ------------------------------------------------------------
   Initialise detector
   ------------------------------------------------------------ */

static void wind_detector_init(
    WindDetector *d)
{
    memset(d, 0, sizeof(*d));

    biquad_lowpass(
        &d->lowpass,
        WNC_SAMPLE_RATE,
        180.0f,
        0.707f);

    biquad_bandpass(
        &d->bandpass,
        WNC_SAMPLE_RATE,
        800.0f,
        0.7f);

    biquad_highpass(
        &d->highpass,
        WNC_SAMPLE_RATE,
        2500.0f,
        0.707f);

    d->attack = WNC_WIND_ATTACK;
    d->release = WNC_WIND_RELEASE;
}

/* ------------------------------------------------------------
   Analyse wind
   ------------------------------------------------------------ */

static WindProfile wind_detector_process(
    WindDetector *d,
    const float *samples,
    int count)
{
    WindProfile p;

    memset(&p, 0, sizeof(p));

    float low_energy = 0.0f;
    float mid_energy = 0.0f;
    float high_energy = 0.0f;

    float instantaneous_energy = 0.0f;

    for (int i = 0; i < count; ++i)
    {
        float x = samples[i];

        float low = biquad_process(
            &d->lowpass,
            x);

        float mid = biquad_process(
            &d->bandpass,
            x);

        float high = biquad_process(
            &d->highpass,
            x);

        low_energy += low * low;
        mid_energy += mid * mid;
        high_energy += high * high;

        instantaneous_energy += x * x;
    }

    low_energy /= count;
    mid_energy /= count;
    high_energy /= count;

    float total =
        low_energy +
        mid_energy +
        high_energy +
        0.00000001f;

    p.low_frequency_energy = sqrtf(low_energy);
    p.mid_frequency_energy = sqrtf(mid_energy);
    p.high_frequency_energy = sqrtf(high_energy);

    p.low_ratio =
        low_energy / total;

    /*
     * Wind often creates disproportionate low-frequency,
     * turbulent energy.
     */

    float turbulence =
        fabsf(
            sqrtf(instantaneous_energy / count) -
            d->previous_energy);

    d->previous_energy =
        sqrtf(instantaneous_energy / count);

    p.turbulence =
        clampf(turbulence * 8.0f, 0.0f, 1.0f);

    float raw_wind =
        p.low_ratio * 0.70f +
        p.turbulence * 0.30f;

    raw_wind =
        clampf(raw_wind, 0.0f, 1.0f);

    /*
     * Hysteresis / smoothing.
     */

    if (raw_wind > d->wind_strength)
    {
        d->wind_strength =
            lerpf(
                d->wind_strength,
                raw_wind,
                d->attack);
    }
    else
    {
        d->wind_strength =
            lerpf(
                d->wind_strength,
                raw_wind,
                d->release);
    }

    p.wind_strength =
        d->wind_strength;

    p.wind_detected =
        d->wind_strength > 0.25f;

    return p;
}

/* ============================================================
   MULTI-MIC COHERENCE
   ============================================================ */

static float microphone_difference(
    const float *a,
    const float *b,
    int count)
{
    float energy = 0.0f;

    for (int i = 0; i < count; ++i)
    {
        float d = a[i] - b[i];

        energy += d * d;
    }

    return sqrtf(energy / count);
}

static float microphone_coherence(
    const float *a,
    const float *b,
    int count)
{
    float aa = 0.0f;
    float bb = 0.0f;
    float ab = 0.0f;

    for (int i = 0; i < count; ++i)
    {
        aa += a[i] * a[i];
        bb += b[i] * b[i];
        ab += a[i] * b[i];
    }

    float denominator =
        sqrtf(aa * bb) + 0.00000001f;

    float c = ab / denominator;

    return clampf(fabsf(c), 0.0f, 1.0f);
}

/* ============================================================
   WIND MICROPHONE ARRAY ANALYSIS
   ============================================================ */

typedef struct
{
    float wind_strength;
    float coherence;
    float microphone_difference;

    bool wind_detected;

} ArrayWindEstimate;

static ArrayWindEstimate analyse_microphone_array(
    MicrophoneArrayFrame *array)
{
    ArrayWindEstimate e;

    memset(&e, 0, sizeof(e));

    if (array->microphone_count < 2)
        return e;

    float differences = 0.0f;
    float coherences = 0.0f;

    int pairs = 0;

    for (int a = 0;
         a < array->microphone_count;
         ++a)
    {
        for (int b = a + 1;
             b < array->microphone_count;
             ++b)
        {
            differences +=
                microphone_difference(
                    array->samples[a],
                    array->samples[b],
                    array->sample_count);

            coherences +=
                microphone_coherence(
                    array->samples[a],
                    array->samples[b],
                    array->sample_count);

            pairs++;
        }
    }

    if (pairs > 0)
    {
        differences /= pairs;
        coherences /= pairs;
    }

    e.microphone_difference =
        differences;

    e.coherence =
        coherences;

    /*
     * Strong microphone disagreement combined
     * with turbulence is characteristic of
     * wind striking the headset microphone ports.
     */

    e.wind_strength =
        clampf(
            differences * 3.0f +
            (1.0f - coherences) * 0.4f,
            0.0f,
            1.0f);

    e.wind_detected =
        e.wind_strength > 0.35f;

    return e;
}

/* ============================================================
   ADAPTIVE HIGH-PASS FILTER
   ============================================================ */

typedef struct
{
    Biquad filter;

    float current_frequency;

    float min_frequency;
    float max_frequency;

    float smoothing;

} AdaptiveHighPass;

static void adaptive_hp_init(
    AdaptiveHighPass *hp)
{
    memset(hp, 0, sizeof(*hp));

    hp->min_frequency =
        WNC_MIN_HP_HZ;

    hp->max_frequency =
        WNC_MAX_HP_HZ;

    hp->current_frequency =
        hp->min_frequency;

    hp->smoothing = 0.15f;

    biquad_highpass(
        &hp->filter,
        WNC_SAMPLE_RATE,
        hp->current_frequency,
        0.707f);
}

static void adaptive_hp_update(
    AdaptiveHighPass *hp,
    float wind_strength)
{
    float target =
        lerpf(
            hp->min_frequency,
            hp->max_frequency,
            wind_strength);

    hp->current_frequency =
        lerpf(
            hp->current_frequency,
            target,
            hp->smoothing);

    biquad_highpass(
        &hp->filter,
        WNC_SAMPLE_RATE,
        hp->current_frequency,
        0.707f);
}

static void adaptive_hp_process(
    AdaptiveHighPass *hp,
    float *samples,
    int count)
{
    for (int i = 0; i < count; ++i)
    {
        samples[i] =
            biquad_process(
                &hp->filter,
                samples[i]);
    }
}

/* ============================================================
   LOW FREQUENCY WIND SUPPRESSOR
   ============================================================ */

typedef struct
{
    Biquad lowpass;

    float suppression;

} WindSuppressor;

static void wind_suppressor_init(
    WindSuppressor *s)
{
    memset(s, 0, sizeof(*s));

    biquad_lowpass(
        &s->lowpass,
        WNC_SAMPLE_RATE,
        220.0f,
        0.707f);
}

static void wind_suppressor_process(
    WindSuppressor *s,
    float *samples,
    int count,
    float wind_strength)
{
    s->suppression =
        clampf(
            wind_strength,
            0.0f,
            1.0f);

    /*
     * Extract the low-frequency wind component
     * and attenuate it adaptively.
     */

    for (int i = 0; i < count; ++i)
    {
        float low =
            biquad_process(
                &s->lowpass,
                samples[i]);

        float attenuation =
            1.0f -
            0.90f * s->suppression;

        samples[i] -=
            low * (1.0f - attenuation);
    }
}

/* ============================================================
   VOICE ACTIVITY DETECTOR
   ============================================================ */

typedef struct
{
    float threshold;

    float envelope;

    bool voice_active;

} VoiceDetector;

static void voice_detector_init(
    VoiceDetector *v)
{
    memset(v, 0, sizeof(*v));

    v->threshold =
        WNC_VOICE_THRESHOLD;
}

static bool voice_detector_process(
    VoiceDetector *v,
    const float *samples,
    int count)
{
    float rms =
        frame_rms(samples, count);

    v->envelope =
        lerpf(
            v->envelope,
            rms,
            0.20f);

    v->voice_active =
        v->envelope >
        v->threshold;

    return v->voice_active;
}

/* ============================================================
   VOICE PRESERVATION
   ============================================================ */

typedef struct
{
    float voice_gain;
    float target_gain;

} VoicePreserver;

static void voice_preserver_init(
    VoicePreserver *v)
{
    memset(v, 0, sizeof(*v));

    v->voice_gain = 1.0f;
    v->target_gain = 1.0f;
}

static void voice_preserver_update(
    VoicePreserver *v,
    bool voice_active,
    float wind_strength)
{
    if (voice_active)
    {
        /*
         * Preserve intelligibility while
         * wind suppression increases.
         */

        v->target_gain =
            1.0f +
            wind_strength * 0.25f;
    }
    else
    {
        v->target_gain =
            0.85f;
    }

    v->voice_gain =
        lerpf(
            v->voice_gain,
            v->target_gain,
            0.10f);
}

/* ============================================================
   ADAPTIVE NOISE GATE
   ============================================================ */

typedef struct
{
    float open_level;
    float close_level;

    float gain;

} NoiseGate;

static void noise_gate_init(
    NoiseGate *g)
{
    memset(g, 0, sizeof(*g));

    g->open_level = 0.010f;
    g->close_level = 0.004f;

    g->gain = 1.0f;
}

static void noise_gate_process(
    NoiseGate *g,
    float *samples,
    int count)
{
    float rms =
        frame_rms(samples, count);

    if (rms > g->open_level)
    {
        g->gain =
            lerpf(
                g->gain,
                1.0f,
                0.20f);
    }
    else if (rms < g->close_level)
    {
        g->gain =
            lerpf(
                g->gain,
                0.15f,
                0.08f);
    }

    for (int i = 0; i < count; ++i)
        samples[i] *= g->gain;
}

/* ============================================================
   LIMITER
   ============================================================ */

typedef struct
{
    float threshold;
    float release;

    float gain;

} Limiter;

static void limiter_init(
    Limiter *l)
{
    memset(l, 0, sizeof(*l));

    l->threshold = 0.92f;
    l->release = 0.05f;

    l->gain = 1.0f;
}

static void limiter_process(
    Limiter *l,
    float *samples,
    int count)
{
    float peak =
        frame_peak(samples, count);

    float target_gain = 1.0f;

    if (peak > l->threshold)
    {
        target_gain =
            l->threshold /
            (peak + 0.0000001f);
    }

    if (target_gain < l->gain)
    {
        l->gain =
            target_gain;
    }
    else
    {
        l->gain =
            lerpf(
                l->gain,
                target_gain,
                l->release);
    }

    for (int i = 0; i < count; ++i)
    {
        samples[i] *= l->gain;

        samples[i] =
            clampf(
                samples[i],
                -1.0f,
                1.0f);
    }
}

/* ============================================================
   WIND NOISE CANCELLER
   ============================================================ */

typedef struct
{
    WindDetector detector;

    AdaptiveHighPass adaptive_hp;

    WindSuppressor suppressor;

    VoiceDetector voice_detector;

    VoicePreserver voice_preserver;

    NoiseGate gate;

    Limiter limiter;

    float wind_strength;

    float microphone_coherence;

    float current_cutoff;

    bool wind_active;
    bool voice_active;

} WindNoiseCanceller;

/* ------------------------------------------------------------
   Initialise
   ------------------------------------------------------------ */

static void wind_noise_canceller_init(
    WindNoiseCanceller *wnc)
{
    memset(wnc, 0, sizeof(*wnc));

    wind_detector_init(
        &wnc->detector);

    adaptive_hp_init(
        &wnc->adaptive_hp);

    wind_suppressor_init(
        &wnc->suppressor);

    voice_detector_init(
        &wnc->voice_detector);

    voice_preserver_init(
        &wnc->voice_preserver);

    noise_gate_init(
        &wnc->gate);

    limiter_init(
        &wnc->limiter);
}

/* ============================================================
   PROCESS ONE MONO FRAME
   ============================================================ */

static void wind_noise_process(
    WindNoiseCanceller *wnc,
    float *samples,
    int count)
{
    WindProfile profile =
        wind_detector_process(
            &wnc->detector,
            samples,
            count);

    wnc->wind_strength =
        profile.wind_strength;

    wnc->wind_active =
        profile.wind_detected;

    /*
     * Detect speech before aggressive
     * wind suppression.
     */

    wnc->voice_active =
        voice_detector_process(
            &wnc->voice_detector,
            samples,
            count);

    /*
     * Adaptive high-pass.
     */

    adaptive_hp_update(
        &wnc->adaptive_hp,
        wnc->wind_strength);

    wnc->current_cutoff =
        wnc->adaptive_hp.current_frequency;

    adaptive_hp_process(
        &wnc->adaptive_hp,
        samples,
        count);

    /*
     * Remove residual low-frequency
     * turbulent energy.
     */

    wind_suppressor_process(
        &wnc->suppressor,
        samples,
        count,
        wnc->wind_strength);

    /*
     * Preserve speech level.
     */

    voice_preserver_update(
        &wnc->voice_preserver,
        wnc->voice_active,
        wnc->wind_strength);

    for (int i = 0; i < count; ++i)
    {
        samples[i] *=
            wnc->voice_preserver.voice_gain;
    }

    /*
     * Gate residual microphone noise.
     */

    noise_gate_process(
        &wnc->gate,
        samples,
        count);

    /*
     * Final safety limiter.
     */

    limiter_process(
        &wnc->limiter,
        samples,
        count);
}

/* ============================================================
   MULTI-MIC BEAMFORMER
   ============================================================ */

typedef struct
{
    int microphone_count;

    float microphone_delay[WNC_MAX_MICS];

    float microphone_gain[WNC_MAX_MICS];

} SimpleBeamformer;

static void beamformer_init(
    SimpleBeamformer *b,
    int microphone_count)
{
    memset(b, 0, sizeof(*b));

    b->microphone_count =
        microphone_count;

    for (int i = 0;
         i < microphone_count;
         ++i)
    {
        b->microphone_gain[i] =
            1.0f /
            (float)microphone_count;
    }
}

/*
 * Simple delay-and-sum microphone
 * combination.
 *
 * A production implementation can replace
 * this with calibrated GCC-PHAT beamforming.
 */

static void beamformer_process(
    SimpleBeamformer *b,
    MicrophoneArrayFrame *input,
    AudioFrame *output)
{
    memset(
        output->samples,
        0,
        sizeof(output->samples));

    output->count =
        input->sample_count;

    for (int mic = 0;
         mic < input->microphone_count;
         ++mic)
    {
        int delay =
            (int)b->microphone_delay[mic];

        float gain =
            b->microphone_gain[mic];

        for (int i = 0;
             i < input->sample_count;
             ++i)
        {
            int source =
                i - delay;

            if (source >= 0 &&
                source < input->sample_count)
            {
                output->samples[i] +=
                    input->samples[mic][source]
                    * gain;
            }
        }
    }
}

/* ============================================================
   ARRAY-AWARE WIND PROCESSOR
   ============================================================ */

typedef struct
{
    WindNoiseCanceller canceller;

    SimpleBeamformer beamformer;

    float array_wind_strength;

    float array_coherence;

} ArrayWindProcessor;

static void array_processor_init(
    ArrayWindProcessor *p,
    int microphone_count)
{
    memset(p, 0, sizeof(*p));

    wind_noise_canceller_init(
        &p->canceller);

    beamformer_init(
        &p->beamformer,
        microphone_count);
}

/* ------------------------------------------------------------
   Process microphone array
   ------------------------------------------------------------ */

static void array_processor_process(
    ArrayWindProcessor *p,
    MicrophoneArrayFrame *input,
    AudioFrame *output)
{
    ArrayWindEstimate estimate =
        analyse_microphone_array(
            input);

    p->array_wind_strength =
        estimate.wind_strength;

    p->array_coherence =
        estimate.coherence;

    /*
     * Combine the microphone array.
     */

    beamformer_process(
        &p->beamformer,
        input,
        output);

    /*
     * Process combined signal.
     */

    wind_noise_process(
        &p->canceller,
        output->samples,
        output->count);

    /*
     * Incorporate array wind information.
     */

    if (estimate.wind_detected)
    {
        float additional =
            estimate.wind_strength * 0.15f;

        for (int i = 0;
             i < output->count;
             ++i)
        {
            output->samples[i] *=
                1.0f - additional;
        }
    }
}

/* ============================================================
   SYNTHETIC TEST SIGNAL
   ============================================================ */

static float random_float(void)
{
    return
        (float)rand() /
        (float)RAND_MAX;
}

/* ------------------------------------------------------------
   Generate synthetic speech-like signal
   ------------------------------------------------------------ */

static void generate_voice(
    float *buffer,
    int count,
    float *phase)
{
    for (int i = 0; i < count; ++i)
    {
        float t =
            (float)i /
            WNC_SAMPLE_RATE;

        float f =
            130.0f +
            25.0f *
            sinf(
                2.0f *
                WNC_PI *
                2.0f *
                t);

        *phase +=
            2.0f *
            WNC_PI *
            f /
            WNC_SAMPLE_RATE;

        if (*phase >
            2.0f * WNC_PI)
        {
            *phase -=
                2.0f * WNC_PI;
        }

        /*
         * Harmonic structure approximating
         * voiced speech.
         */

        float x =
            0.18f *
            sinf(*phase);

        x +=
            0.08f *
            sinf(*phase * 2.0f);

        x +=
            0.04f *
            sinf(*phase * 3.0f);

        /*
         * Add very small excitation noise.
         */

        x +=
            (random_float() - 0.5f)
            * 0.01f;

        buffer[i] = x;
    }
}

/* ============================================================
   SYNTHETIC WIND
   ============================================================ */

typedef struct
{
    float state;

} SyntheticWind;

static void synthetic_wind_init(
    SyntheticWind *w)
{
    w->state = 0.0f;
}

static float synthetic_wind_sample(
    SyntheticWind *w)
{
    float random_component =
        random_float() * 2.0f - 1.0f;

    /*
     * Slowly varying turbulent pressure.
     */

    w->state =
        0.985f * w->state +
        0.015f * random_component;

    float turbulence =
        random_component * 0.15f +
        w->state * 0.85f;

    /*
     * Low-frequency rumble.
     */

    static float phase = 0.0f;

    phase +=
        2.0f *
        WNC_PI *
        80.0f /
        WNC_SAMPLE_RATE;

    if (phase >
        2.0f * WNC_PI)
    {
        phase -=
            2.0f * WNC_PI;
    }

    float rumble =
        sinf(phase) * 0.25f;

    return
        turbulence * 0.35f +
        rumble;
}

/* ============================================================
   TEST MICROPHONE ARRAY
   ============================================================ */

static void generate_test_array(
    MicrophoneArrayFrame *array,
    float *voice_phase,
    SyntheticWind *wind)
{
    array->microphone_count = 2;
    array->sample_count =
        WNC_FRAME_SIZE;

    float voice[WNC_FRAME_SIZE];

    generate_voice(
        voice,
        WNC_FRAME_SIZE,
        voice_phase);

    for (int i = 0;
         i < WNC_FRAME_SIZE;
         ++i)
    {
        float wind_sample =
            synthetic_wind_sample(
                wind);

        /*
         * Each microphone receives a
         * slightly different wind field.
         */

        float wind_a =
            wind_sample *
            (0.90f +
             random_float() * 0.20f);

        float wind_b =
            wind_sample *
            (0.65f +
             random_float() * 0.70f);

        /*
         * Mic A.
         */

        array->samples[0][i] =
            voice[i] +
            wind_a;

        /*
         * Mic B.
         */

        array->samples[1][i] =
            voice[i] +
            wind_b;
    }
}

/* ============================================================
   PERFORMANCE METRICS
   ============================================================ */

typedef struct
{
    float input_rms;
    float output_rms;

    float input_peak;
    float output_peak;

    float wind_strength;

    float cutoff_frequency;

    bool wind_detected;
    bool voice_detected;

} WindMetrics;

static WindMetrics calculate_metrics(
    const float *input,
    const float *output,
    int count,
    WindNoiseCanceller *processor)
{
    WindMetrics m;

    memset(&m, 0, sizeof(m));

    m.input_rms =
        frame_rms(input, count);

    m.output_rms =
        frame_rms(output, count);

    m.input_peak =
        frame_peak(input, count);

    m.output_peak =
        frame_peak(output, count);

    m.wind_strength =
        processor->wind_strength;

    m.cutoff_frequency =
        processor->current_cutoff;

    m.wind_detected =
        processor->wind_active;

    m.voice_detected =
        processor->voice_active;

    return m;
}

/* ============================================================
   STATUS DISPLAY
   ============================================================ */

static void print_metrics(
    WindMetrics *m)
{
    printf(
        "Input RMS:       %8.4f\n",
        m->input_rms);

    printf(
        "Output RMS:      %8.4f\n",
        m->output_rms);

    printf(
        "Input Peak:      %8.4f\n",
        m->input_peak);

    printf(
        "Output Peak:     %8.4f\n",
        m->output_peak);

    printf(
        "Wind Strength:   %8.3f\n",
        m->wind_strength);

    printf(
        "Adaptive HP:     %8.1f Hz\n",
        m->cutoff_frequency);

    printf(
        "Wind Detected:   %s\n",
        m->wind_detected ?
        "YES" : "NO");

    printf(
        "Voice Detected:  %s\n",
        m->voice_detected ?
        "YES" : "NO");
}

/* ============================================================
   MAIN DEMONSTRATION
   ============================================================ */

int main(void)
{
    printf("\n");
    printf("============================================\n");
    printf(" META QUEST WIND-NOISE CANCELLATION ENGINE\n");
    printf("============================================\n");
    printf("\n");

    printf(
        "Sample rate: %d Hz\n",
        WNC_SAMPLE_RATE);

    printf(
        "Frame size:   %d samples\n",
        WNC_FRAME_SIZE);

    printf(
        "Frame time:   %.2f ms\n",
        1000.0 *
        WNC_FRAME_SIZE /
        WNC_SAMPLE_RATE);

    printf("\n");

    ArrayWindProcessor processor;

    array_processor_init(
        &processor,
        2);

    SyntheticWind wind;

    synthetic_wind_init(
        &wind);

    float voice_phase = 0.0f;

    for (int frame = 0;
         frame < 100;
         ++frame)
    {
        MicrophoneArrayFrame input;

        generate_test_array(
            &input,
            &voice_phase,
            &wind);

        float original[
            WNC_FRAME_SIZE];

        memcpy(
            original,
            input.samples[0],
            sizeof(original));

        AudioFrame output;

        array_processor_process(
            &processor,
            &input,
            &output);

        WindMetrics metrics =
            calculate_metrics(
                original,
                output.samples,
                output.count,
                &processor.canceller);

        printf(
            "Frame %03d | "
            "Wind %.2f | "
            "HP %.0f Hz | "
            "Voice %s\n",
            frame,
            metrics.wind_strength,
            metrics.cutoff_frequency,
            metrics.voice_detected ?
            "YES" : "NO");
    }

    printf("\n");
    printf("Final processor state:\n");

    WindMetrics final_metrics;

    /*
     * Generate one final test frame.
     */

    MicrophoneArrayFrame final_input;

    generate_test_array(
        &final_input,
        &voice_phase,
        &wind);

    float final_original[
        WNC_FRAME_SIZE];

    memcpy(
        final_original,
        final_input.samples[0],
        sizeof(final_original));

    AudioFrame final_output;

    array_processor_process(
        &processor,
        &final_input,
        &final_output);

    final_metrics =
        calculate_metrics(
            final_original,
            final_output.samples,
            final_output.count,
            &processor.canceller);

    print_metrics(
        &final_metrics);

    printf("\n");
    printf("Wind cancellation complete.\n");

    return 0;
}





/*
 * automatic_eq_optimisation.c
 *
 * Meta Quest VR
 * Automatic Real-Time EQ Optimisation
 *
 * C11 / C17
 *
 * Features:
 *   - Real-time spectral analysis
 *   - Automatic tonal-balance estimation
 *   - Parametric EQ
 *   - Adaptive low/mid/high correction
 *   - Voice-preservation mode
 *   - Music/game mode detection
 *   - Loudness normalisation
 *   - Dynamic EQ
 *   - Treble protection
 *   - Bass management
 *   - Smooth coefficient transitions
 *
 * Audio architecture:
 *
 *   Input
 *     |
 *     v
 *   Analysis
 *     |
 *     +----> Bass analysis
 *     |
 *     +----> Mid analysis
 *     |
 *     +----> Treble analysis
 *     |
 *     +----> Voice analysis
 *     |
 *     v
 *   Tonal target
 *     |
 *     v
 *   Automatic EQ controller
 *     |
 *     v
 *   Parametric EQ
 *     |
 *     v
 *   Loudness control
 *     |
 *     v
 *   Limiter
 *     |
 *     v
 *   Quest headphones
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <math.h>

#define EQ_SAMPLE_RATE       48000
#define EQ_FRAME_SIZE        480

#define EQ_PI                3.14159265358979323846f

#define EQ_MAX_BANDS         8

#define EQ_MIN_GAIN_DB       -12.0f
#define EQ_MAX_GAIN_DB        12.0f

#define EQ_MAX_Q              5.0f

/* ============================================================
   UTILITIES
   ============================================================ */

static float clampf(
    float x,
    float lo,
    float hi)
{
    if (x < lo)
        return lo;

    if (x > hi)
        return hi;

    return x;
}

static float lerpf(
    float a,
    float b,
    float t)
{
    return a + (b - a) * t;
}

static float db_to_linear(
    float db)
{
    return powf(
        10.0f,
        db / 20.0f);
}

static float linear_to_db(
    float x)
{
    if (x < 0.0000001f)
        return -140.0f;

    return 20.0f * log10f(x);
}

/* ============================================================
   AUDIO STATISTICS
   ============================================================ */

static float rms(
    const float *x,
    int count)
{
    double sum = 0.0;

    for (int i = 0;
         i < count;
         ++i)
    {
        sum +=
            x[i] * x[i];
    }

    return sqrtf(
        (float)(sum / count));
}

static float peak(
    const float *x,
    int count)
{
    float p = 0.0f;

    for (int i = 0;
         i < count;
         ++i)
    {
        float a =
            fabsf(x[i]);

        if (a > p)
            p = a;
    }

    return p;
}

/* ============================================================
   BIQUAD
   ============================================================ */

typedef struct
{
    float b0;
    float b1;
    float b2;

    float a1;
    float a2;

    float x1;
    float x2;

    float y1;
    float y2;

} Biquad;

static void biquad_reset(
    Biquad *b)
{
    b->x1 = 0.0f;
    b->x2 = 0.0f;

    b->y1 = 0.0f;
    b->y2 = 0.0f;
}

/* ------------------------------------------------------------
   Peaking EQ
   ------------------------------------------------------------ */

static void biquad_peaking(
    Biquad *b,
    float sample_rate,
    float frequency,
    float gain_db,
    float q)
{
    float A =
        powf(
            10.0f,
            gain_db / 40.0f);

    float w0 =
        2.0f *
        EQ_PI *
        frequency /
        sample_rate;

    float alpha =
        sinf(w0) /
        (2.0f * q);

    float c =
        cosf(w0);

    float b0 =
        1.0f +
        alpha * A;

    float b1 =
        -2.0f * c;

    float b2 =
        1.0f -
        alpha * A;

    float a0 =
        1.0f +
        alpha / A;

    float a1 =
        -2.0f * c;

    float a2 =
        1.0f -
        alpha / A;

    b->b0 = b0 / a0;
    b->b1 = b1 / a0;
    b->b2 = b2 / a0;

    b->a1 = a1 / a0;
    b->a2 = a2 / a0;
}

/* ------------------------------------------------------------
   Low shelf
   ------------------------------------------------------------ */

static void biquad_lowshelf(
    Biquad *b,
    float sample_rate,
    float frequency,
    float gain_db)
{
    float A =
        powf(
            10.0f,
            gain_db / 40.0f);

    float w0 =
        2.0f *
        EQ_PI *
        frequency /
        sample_rate;

    float c =
        cosf(w0);

    float s =
        sinf(w0);

    float alpha =
        s / 2.0f *
        sqrtf(
            (A + 1.0f / A));

    float beta =
        2.0f *
        sqrtf(A) *
        alpha;

    float b0 =
        A *
        ((A + 1.0f)
         - (A - 1.0f) * c
         + beta);

    float b1 =
        2.0f * A *
        ((A - 1.0f)
         - (A + 1.0f) * c);

    float b2 =
        A *
        ((A + 1.0f)
         - (A - 1.0f) * c
         - beta);

    float a0 =
        (A + 1.0f)
        + (A - 1.0f) * c
        + beta;

    float a1 =
        -2.0f *
        ((A - 1.0f)
         + (A + 1.0f) * c);

    float a2 =
        (A + 1.0f)
        + (A - 1.0f) * c
        - beta;

    b->b0 = b0 / a0;
    b->b1 = b1 / a0;
    b->b2 = b2 / a0;

    b->a1 = a1 / a0;
    b->a2 = a2 / a0;
}

/* ------------------------------------------------------------
   High shelf
   ------------------------------------------------------------ */

static void biquad_highshelf(
    Biquad *b,
    float sample_rate,
    float frequency,
    float gain_db)
{
    float A =
        powf(
            10.0f,
            gain_db / 40.0f);

    float w0 =
        2.0f *
        EQ_PI *
        frequency /
        sample_rate;

    float c =
        cosf(w0);

    float s =
        sinf(w0);

    float alpha =
        s / 2.0f *
        sqrtf(
            (A + 1.0f / A));

    float beta =
        2.0f *
        sqrtf(A) *
        alpha;

    float b0 =
        A *
        ((A + 1.0f)
         + (A - 1.0f) * c
         + beta);

    float b1 =
        -2.0f * A *
        ((A - 1.0f)
         + (A + 1.0f) * c);

    float b2 =
        A *
        ((A + 1.0f)
         + (A - 1.0f) * c
         - beta);

    float a0 =
        (A + 1.0f)
        - (A - 1.0f) * c
        + beta;

    float a1 =
        2.0f *
        ((A - 1.0f)
         - (A + 1.0f) * c);

    float a2 =
        (A + 1.0f)
        - (A - 1.0f) * c
        - beta;

    b->b0 = b0 / a0;
    b->b1 = b1 / a0;
    b->b2 = b2 / a0;

    b->a1 = a1 / a0;
    b->a2 = a2 / a0;
}

/* ------------------------------------------------------------
   Process
   ------------------------------------------------------------ */

static float biquad_process(
    Biquad *b,
    float x)
{
    float y =
        b->b0 * x +
        b->b1 * b->x1 +
        b->b2 * b->x2 -
        b->a1 * b->y1 -
        b->a2 * b->y2;

    b->x2 = b->x1;
    b->x1 = x;

    b->y2 = b->y1;
    b->y1 = y;

    return y;
}

/* ============================================================
   EQ BAND
   ============================================================ */

typedef enum
{
    EQ_BAND_PEAKING,
    EQ_BAND_LOW_SHELF,
    EQ_BAND_HIGH_SHELF

} EQBandType;

typedef struct
{
    EQBandType type;

    float frequency;

    float target_gain_db;
    float current_gain_db;

    float q;

    Biquad filter;

} EQBand;

/* ============================================================
   SPECTRAL ANALYSIS
   ============================================================ */

typedef struct
{
    Biquad low;
    Biquad low_mid;
    Biquad mid;
    Biquad high_mid;
    Biquad high;

} SpectralAnalyzer;

typedef struct
{
    float bass;
    float low_mid;
    float mid;
    float high_mid;
    float treble;

    float total;

    float bass_db;
    float mid_db;
    float treble_db;

} Spectrum;

/* ------------------------------------------------------------
   Initialise analysis filters
   ------------------------------------------------------------ */

static void spectrum_init(
    SpectralAnalyzer *a)
{
    memset(a, 0, sizeof(*a));

    /*
     * These filters overlap deliberately.
     */

    biquad_peaking(
        &a->low,
        EQ_SAMPLE_RATE,
        100.0f,
        0.0f,
        0.7f);

    biquad_peaking(
        &a->low_mid,
        EQ_SAMPLE_RATE,
        400.0f,
        0.0f,
        0.7f);

    biquad_peaking(
        &a->mid,
        EQ_SAMPLE_RATE,
        1000.0f,
        0.0f,
        0.7f);

    biquad_peaking(
        &a->high_mid,
        EQ_SAMPLE_RATE,
        3000.0f,
        0.0f,
        0.7f);

    biquad_peaking(
        &a->high,
        EQ_SAMPLE_RATE,
        7000.0f,
        0.0f,
        0.7f);
}

/* ------------------------------------------------------------
   Analyse spectrum
   ------------------------------------------------------------ */

static Spectrum spectrum_analyse(
    SpectralAnalyzer *a,
    const float *samples,
    int count)
{
    Spectrum s;

    memset(&s, 0, sizeof(s));

    for (int i = 0;
         i < count;
         ++i)
    {
        float x =
            samples[i];

        float low =
            biquad_process(
                &a->low,
                x);

        float low_mid =
            biquad_process(
                &a->low_mid,
                x);

        float mid =
            biquad_process(
                &a->mid,
                x);

        float high_mid =
            biquad_process(
                &a->high_mid,
                x);

        float high =
            biquad_process(
                &a->high,
                x);

        s.bass +=
            low * low;

        s.low_mid +=
            low_mid * low_mid;

        s.mid +=
            mid * mid;

        s.high_mid +=
            high_mid * high_mid;

        s.treble +=
            high * high;
    }

    s.bass =
        sqrtf(s.bass / count);

    s.low_mid =
        sqrtf(s.low_mid / count);

    s.mid =
        sqrtf(s.mid / count);

    s.high_mid =
        sqrtf(s.high_mid / count);

    s.treble =
        sqrtf(s.treble / count);

    s.total =
        s.bass +
        s.low_mid +
        s.mid +
        s.high_mid +
        s.treble;

    s.bass_db =
        linear_to_db(s.bass);

    s.mid_db =
        linear_to_db(
            s.mid);

    s.treble_db =
        linear_to_db(
            s.treble);

    return s;
}

/* ============================================================
   AUDIO CONTENT CLASSIFICATION
   ============================================================ */

typedef enum
{
    AUDIO_UNKNOWN,
    AUDIO_VOICE,
    AUDIO_MUSIC,
    AUDIO_GAME

} AudioType;

typedef struct
{
    float voice_score;
    float music_score;
    float game_score;

    AudioType type;

} ContentClassifier;

/* ------------------------------------------------------------
   Voice estimator
   ------------------------------------------------------------ */

static float estimate_voice(
    const Spectrum *s)
{
    /*
     * Voice tends to have significant
     * mid-band energy while extreme
     * bass/treble are less dominant.
     */

    float score =
        s->mid /
        (s->bass +
         s->treble +
         0.00001f);

    return clampf(
        score * 0.7f,
        0.0f,
        1.0f);
}

/* ------------------------------------------------------------
   Music estimator
   ------------------------------------------------------------ */

static float estimate_music(
    const Spectrum *s)
{
    float spread =
        s->bass +
        s->mid +
        s->treble;

    if (spread < 0.00001f)
        return 0.0f;

    float balance =
        1.0f -
        fabsf(
            s->bass -
            s->treble) /
        (spread + 0.00001f);

    return clampf(
        balance,
        0.0f,
        1.0f);
}

/* ------------------------------------------------------------
   Game estimator
   ------------------------------------------------------------ */

static float estimate_game(
    const Spectrum *s)
{
    /*
     * Games frequently contain strong
     * low-frequency effects and upper
     * midrange/transient information.
     */

    float score =
        (s->bass +
         s->high_mid * 1.2f)
        /
        (s->mid +
         0.00001f);

    return clampf(
        score * 0.35f,
        0.0f,
        1.0f);
}

/* ------------------------------------------------------------
   Classify
   ------------------------------------------------------------ */

static AudioType classify_audio(
    ContentClassifier *c,
    const Spectrum *s)
{
    c->voice_score =
        estimate_voice(s);

    c->music_score =
        estimate_music(s);

    c->game_score =
        estimate_game(s);

    if (c->voice_score >
        c->music_score &&
        c->voice_score >
        c->game_score)
    {
        c->type =
            AUDIO_VOICE;
    }
    else if (c->music_score >
             c->game_score)
    {
        c->type =
            AUDIO_MUSIC;
    }
    else
    {
        c->type =
            AUDIO_GAME;
    }

    return c->type;
}

/* ============================================================
   EQ TARGET
   ============================================================ */

typedef struct
{
    float bass_target;
    float low_mid_target;
    float mid_target;
    float high_mid_target;
    float treble_target;

} EQTarget;

/* ------------------------------------------------------------
   Build target
   ------------------------------------------------------------ */

static EQTarget calculate_eq_target(
    const Spectrum *s,
    AudioType type)
{
    EQTarget target;

    memset(
        &target,
        0,
        sizeof(target));

    /*
     * Target is deliberately modest.
     *
     * The goal is correction rather than
     * extreme tonal coloration.
     */

    float bass_error =
        -18.0f -
        s->bass_db;

    float mid_error =
        -20.0f -
        s->mid_db;

    float treble_error =
        -24.0f -
        s->treble_db;

    target.bass_target =
        clampf(
            bass_error * 0.30f,
            -6.0f,
            6.0f);

    target.mid_target =
        clampf(
            mid_error * 0.25f,
            -4.0f,
            4.0f);

    target.treble_target =
        clampf(
            treble_error * 0.25f,
            -5.0f,
            5.0f);

    target.low_mid_target =
        -target.mid_target * 0.35f;

    target.high_mid_target =
        -target.treble_target * 0.20f;

    /*
     * Voice:
     * emphasise intelligibility.
     */

    if (type == AUDIO_VOICE)
    {
        target.bass_target -=
            1.5f;

        target.low_mid_target -=
            0.5f;

        target.high_mid_target +=
            1.5f;

        target.treble_target +=
            0.5f;
    }

    /*
     * Music:
     * allow somewhat greater low-end
     * and high-end preservation.
     */

    if (type == AUDIO_MUSIC)
    {
        target.bass_target +=
            1.0f;

        target.treble_target +=
            0.5f;
    }

    /*
     * Games:
     * preserve positional/transient detail.
     */

    if (type == AUDIO_GAME)
    {
        target.mid_target +=
            0.5f;

        target.high_mid_target +=
            1.0f;
    }

    target.bass_target =
        clampf(
            target.bass_target,
            EQ_MIN_GAIN_DB,
            EQ_MAX_GAIN_DB);

    target.low_mid_target =
        clampf(
            target.low_mid_target,
            EQ_MIN_GAIN_DB,
            EQ_MAX_GAIN_DB);

    target.mid_target =
        clampf(
            target.mid_target,
            EQ_MIN_GAIN_DB,
            EQ_MAX_GAIN_DB);

    target.high_mid_target =
        clampf(
            target.high_mid_target,
            EQ_MIN_GAIN_DB,
            EQ_MAX_GAIN_DB);

    target.treble_target =
        clampf(
            target.treble_target,
            EQ_MIN_GAIN_DB,
            EQ_MAX_GAIN_DB);

    return target;
}

/* ============================================================
   AUTOMATIC EQ ENGINE
   ============================================================ */

typedef struct
{
    EQBand bands[EQ_MAX_BANDS];

    int band_count;

    SpectralAnalyzer analyzer;

    ContentClassifier classifier;

    Spectrum spectrum;

    EQTarget target;

    AudioType content_type;

    float master_gain;

} AutomaticEQ;

/* ------------------------------------------------------------
   Initialise
   ------------------------------------------------------------ */

static void automatic_eq_init(
    AutomaticEQ *eq)
{
    memset(eq, 0, sizeof(*eq));

    spectrum_init(
        &eq->analyzer);

    eq->band_count = 5;

    /*
     * Bass
     */

    eq->bands[0].type =
        EQ_BAND_LOW_SHELF;

    eq->bands[0].frequency =
        100.0f;

    eq->bands[0].q =
        0.707f;

    /*
     * Low-mid
     */

    eq->bands[1].type =
        EQ_BAND_PEAKING;

    eq->bands[1].frequency =
        400.0f;

    eq->bands[1].q =
        0.9f;

    /*
     * Mid
     */

    eq->bands[2].type =
        EQ_BAND_PEAKING;

    eq->bands[2].frequency =
        1000.0f;

    eq->bands[2].q =
        0.8f;

    /*
     * High-mid
     */

    eq->bands[3].type =
        EQ_BAND_PEAKING;

    eq->bands[3].frequency =
        3000.0f;

    eq->bands[3].q =
        0.8f;

    /*
     * Treble
     */

    eq->bands[4].type =
        EQ_BAND_HIGH_SHELF;

    eq->bands[4].frequency =
        7000.0f;

    eq->bands[4].q =
        0.707f;

    for (int i = 0;
         i < eq->band_count;
         ++i)
    {
        eq->bands[i].current_gain_db =
            0.0f;

        eq->bands[i].target_gain_db =
            0.0f;
    }

    eq->master_gain = 1.0f;
}

/* ============================================================
   UPDATE EQ COEFFICIENTS
   ============================================================ */

static void automatic_eq_update_filters(
    AutomaticEQ *eq)
{
    for (int i = 0;
         i < eq->band_count;
         ++i)
    {
        EQBand *band =
            &eq->bands[i];

        /*
         * Smooth parameter changes.
         */

        band->current_gain_db =
            lerpf(
                band->current_gain_db,
                band->target_gain_db,
                0.08f);

        if (band->type ==
            EQ_BAND_LOW_SHELF)
        {
            biquad_lowshelf(
                &band->filter,
                EQ_SAMPLE_RATE,
                band->frequency,
                band->current_gain_db);
        }
        else if (band->type ==
                 EQ_BAND_HIGH_SHELF)
        {
            biquad_highshelf(
                &band->filter,
                EQ_SAMPLE_RATE,
                band->frequency,
                band->current_gain_db);
        }
        else
        {
            biquad_peaking(
                &band->filter,
                EQ_SAMPLE_RATE,
                band->frequency,
                band->current_gain_db,
                band->q);
        }
    }
}

/* ============================================================
   MAP TARGET TO BANDS
   ============================================================ */

static void automatic_eq_set_target(
    AutomaticEQ *eq,
    const EQTarget *target)
{
    eq->bands[0].target_gain_db =
        target->bass_target;

    eq->bands[1].target_gain_db =
        target->low_mid_target;

    eq->bands[2].target_gain_db =
        target->mid_target;

    eq->bands[3].target_gain_db =
        target->high_mid_target;

    eq->bands[4].target_gain_db =
        target->treble_target;
}

/* ============================================================
   ANALYSE + UPDATE
   ============================================================ */

static void automatic_eq_analyse(
    AutomaticEQ *eq,
    const float *samples,
    int count)
{
    eq->spectrum =
        spectrum_analyse(
            &eq->analyzer,
            samples,
            count);

    eq->content_type =
        classify_audio(
            &eq->classifier,
            &eq->spectrum);

    eq->target =
        calculate_eq_target(
            &eq->spectrum,
            eq->content_type);

    automatic_eq_set_target(
        eq,
        &eq->target);

    automatic_eq_update_filters(
        eq);
}

/* ============================================================
   PROCESS AUDIO
   ============================================================ */

static void automatic_eq_process(
    AutomaticEQ *eq,
    float *samples,
    int count)
{
    for (int i = 0;
         i < count;
         ++i)
    {
        float x =
            samples[i];

        for (int b = 0;
             b < eq->band_count;
             ++b)
        {
            x =
                biquad_process(
                    &eq->bands[b].filter,
                    x);
        }

        samples[i] =
            x *
            eq->master_gain;
    }
}

/* ============================================================
   LOUDNESS NORMALISATION
   ============================================================ */

typedef struct
{
    float target_rms;

    float gain;

} LoudnessController;

static void loudness_init(
    LoudnessController *l)
{
    memset(l, 0, sizeof(*l));

    l->target_rms =
        0.18f;

    l->gain =
        1.0f;
}

static void loudness_process(
    LoudnessController *l,
    float *samples,
    int count)
{
    float level =
        rms(
            samples,
            count);

    if (level >
        0.00001f)
    {
        float target =
            l->target_rms /
            level;

        target =
            clampf(
                target,
                0.50f,
                1.80f);

        l->gain =
            lerpf(
                l->gain,
                target,
                0.04f);
    }

    for (int i = 0;
         i < count;
         ++i)
    {
        samples[i] *=
            l->gain;
    }
}

/* ============================================================
   OUTPUT LIMITER
   ============================================================ */

typedef struct
{
    float threshold;
    float gain;

} OutputLimiter;

static void output_limiter_init(
    OutputLimiter *l)
{
    l->threshold =
        0.92f;

    l->gain =
        1.0f;
}

static void output_limiter_process(
    OutputLimiter *l,
    float *samples,
    int count)
{
    float p =
        peak(
            samples,
            count);

    float target =
        1.0f;

    if (p >
        l->threshold)
    {
        target =
            l->threshold /
            (p + 0.000001f);
    }

    l->gain =
        lerpf(
            l->gain,
            target,
            0.30f);

    for (int i = 0;
         i < count;
         ++i)
    {
        samples[i] *=
            l->gain;

        samples[i] =
            clampf(
                samples[i],
                -1.0f,
                1.0f);
    }
}

/* ============================================================
   COMPLETE VR AUDIO ENGINE
   ============================================================ */

typedef struct
{
    AutomaticEQ eq;

    LoudnessController loudness;

    OutputLimiter limiter;

    float input_level;
    float output_level;

} VRAudioEQEngine;

/* ------------------------------------------------------------
   Initialise
   ------------------------------------------------------------ */

static void vr_eq_init(
    VRAudioEQEngine *engine)
{
    memset(
        engine,
        0,
        sizeof(*engine));

    automatic_eq_init(
        &engine->eq);

    loudness_init(
        &engine->loudness);

    output_limiter_init(
        &engine->limiter);
}

/* ------------------------------------------------------------
   Process
   ------------------------------------------------------------ */

static void vr_eq_process(
    VRAudioEQEngine *engine,
    float *samples,
    int count)
{
    engine->input_level =
        rms(
            samples,
            count);

    /*
     * Analyse BEFORE changing the signal.
     */

    automatic_eq_analyse(
        &engine->eq,
        samples,
        count);

    /*
     * Apply adaptive EQ.
     */

    automatic_eq_process(
        &engine->eq,
        samples,
        count);

    /*
     * Normalise perceived level.
     */

    loudness_process(
        &engine->loudness,
        samples,
        count);

    /*
     * Prevent clipping.
     */

    output_limiter_process(
        &engine->limiter,
        samples,
        count);

    engine->output_level =
        rms(
            samples,
            count);
}

/* ============================================================
   TEST SIGNAL GENERATION
   ============================================================ */

static float random_float(void)
{
    return
        (float)rand() /
        (float)RAND_MAX;
}

static void generate_test_audio(
    float *buffer,
    int count,
    float *phase)
{
    for (int i = 0;
         i < count;
         ++i)
    {
        float t =
            (float)i /
            EQ_SAMPLE_RATE;

        /*
         * Bass.
         */

        float bass =
            0.20f *
            sinf(
                2.0f *
                EQ_PI *
                80.0f *
                t);

        /*
         * Voice-like midrange.
         */

        float voice =
            0.12f *
            sinf(
                2.0f *
                EQ_PI *
                440.0f *
                t);

        /*
         * High-mid detail.
         */

        float presence =
            0.06f *
            sinf(
                2.0f *
                EQ_PI *
                3000.0f *
                t);

        /*
         * Treble.
         */

        float treble =
            0.035f *
            sinf(
                2.0f *
                EQ_PI *
                8000.0f *
                t);

        /*
         * Small broadband component.
         */

        float noise =
            (random_float() - 0.5f)
            * 0.015f;

        buffer[i] =
            bass +
            voice +
            presence +
            treble +
            noise;

        *phase +=
            2.0f *
            EQ_PI /
            EQ_SAMPLE_RATE;
    }
}

/* ============================================================
   STATUS
   ============================================================ */

static const char *audio_type_name(
    AudioType type)
{
    switch (type)
    {
        case AUDIO_VOICE:
            return "VOICE";

        case AUDIO_MUSIC:
            return "MUSIC";

        case AUDIO_GAME:
            return "GAME";

        default:
            return "UNKNOWN";
    }
}

static void print_eq_status(
    VRAudioEQEngine *engine)
{
    AutomaticEQ *eq =
        &engine->eq;

    printf(
        "\n--------------------------------------------\n");

    printf(
        "Automatic EQ Status\n");

    printf(
        "--------------------------------------------\n");

    printf(
        "Content:       %s\n",
        audio_type_name(
            eq->content_type));

    printf(
        "Bass:          %7.2f dB\n",
        eq->spectrum.bass_db);

    printf(
        "Mid:           %7.2f dB\n",
        eq->spectrum.mid_db);

    printf(
        "Treble:        %7.2f dB\n",
        eq->spectrum.treble_db);

    printf(
        "\nEQ Targets:\n");

    printf(
        "Bass:          %7.2f dB\n",
        eq->bands[0].current_gain_db);

    printf(
        "Low-Mid:       %7.2f dB\n",
        eq->bands[1].current_gain_db);

    printf(
        "Mid:           %7.2f dB\n",
        eq->bands[2].current_gain_db);

    printf(
        "High-Mid:      %7.2f dB\n",
        eq->bands[3].current_gain_db);

    printf(
        "Treble:        %7.2f dB\n",
        eq->bands[4].current_gain_db);

    printf(
        "\nInput RMS:      %.4f\n",
        engine->input_level);

    printf(
        "Output RMS:     %.4f\n",
        engine->output_level);

    printf(
        "Loudness Gain:  %.3f\n",
        engine->loudness.gain);

    printf(
        "Limiter Gain:   %.3f\n",
        engine->limiter.gain);
}

/* ============================================================
   MAIN
   ============================================================ */

int main(void)
{
    printf("\n");
    printf("============================================\n");
    printf(" META QUEST AUTOMATIC EQ ENGINE\n");
    printf("============================================\n");

    printf(
        "Sample rate: %d Hz\n",
        EQ_SAMPLE_RATE);

    printf(
        "Frame size:  %d samples\n",
        EQ_FRAME_SIZE);

    printf(
        "Latency frame: %.2f ms\n",
        1000.0 *
        EQ_FRAME_SIZE /
        EQ_SAMPLE_RATE);

    printf("\n");

    VRAudioEQEngine engine;

    vr_eq_init(
        &engine);

    float buffer[
        EQ_FRAME_SIZE];

    float phase =
        0.0f;

    /*
     * Run 100 real-time frames.
     */

    for (int frame = 0;
         frame < 100;
         ++frame)
    {
        generate_test_audio(
            buffer,
            EQ_FRAME_SIZE,
            &phase);

        vr_eq_process(
            &engine,
            buffer,
            EQ_FRAME_SIZE);

        if ((frame % 10) == 0)
        {
            printf(
                "Frame %03d | "
                "Type %-7s | "
                "Bass %+5.1f | "
                "Mid %+5.1f | "
                "Treble %+5.1f\n",

                frame,

                audio_type_name(
                    engine.eq.content_type),

                engine.eq
                    .bands[0]
                    .current_gain_db,

                engine.eq
                    .bands[2]
                    .current_gain_db,

                engine.eq
                    .bands[4]
                    .current_gain_db);
        }
    }

    print_eq_status(
        &engine);

    printf("\n");
    printf(
        "Automatic EQ optimisation complete.\n");

    return 0;
}


/*
 * industrial_digital_twin.c
 *
 * Meta Quest VR - Industrial Digital Twin
 *
 * C11 / C17
 *
 * Core features:
 *
 *   - Industrial facility model
 *   - Machines / robots / conveyors
 *   - Live telemetry
 *   - Temperature monitoring
 *   - Vibration monitoring
 *   - Energy consumption
 *   - Production throughput
 *   - Equipment health
 *   - Alarm system
 *   - Spatial inspection
 *   - Factory navigation
 *   - Data overlays
 *   - Predictive-maintenance foundation
 *
 * The renderer / OpenXR layer is intentionally abstracted.
 *
 * Architecture:
 *
 *   Physical Factory
 *          |
 *          v
 *   IoT / PLC / SCADA
 *          |
 *          v
 *   Telemetry Gateway
 *          |
 *          v
 *   Digital Twin Engine
 *          |
 *          +------> Health Model
 *          |
 *          +------> Energy Model
 *          |
 *          +------> Production Model
 *          |
 *          +------> Alarm System
 *          |
 *          v
 *       Meta Quest
 *          |
 *          v
 *    Immersive Factory
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <math.h>

#define DT_MAX_ASSETS       256
#define DT_MAX_SENSORS      1024
#define DT_MAX_ALARMS       256
#define DT_MAX_CONNECTIONS  512

#define DT_PI               3.14159265358979323846f

/* ============================================================
   UTILITIES
   ============================================================ */

static float clampf(
    float x,
    float min,
    float max)
{
    if (x < min)
        return min;

    if (x > max)
        return max;

    return x;
}

static float lerpf(
    float a,
    float b,
    float t)
{
    return a + (b - a) * t;
}

static float distance3(
    float ax,
    float ay,
    float az,
    float bx,
    float by,
    float bz)
{
    float dx = ax - bx;
    float dy = ay - by;
    float dz = az - bz;

    return sqrtf(
        dx * dx +
        dy * dy +
        dz * dz);
}

/* ============================================================
   VECTOR
   ============================================================ */

typedef struct
{
    float x;
    float y;
    float z;

} Vec3;

/* ============================================================
   ASSET TYPES
   ============================================================ */

typedef enum
{
    ASSET_FACTORY,
    ASSET_BUILDING,
    ASSET_MACHINE,
    ASSET_ROBOT,
    ASSET_CONVEYOR,
    ASSET_CRANE,
    ASSET_PUMP,
    ASSET_MOTOR,
    ASSET_TURBINE,
    ASSET_BOILER,
    ASSET_TANK,
    ASSET_PIPELINE,
    ASSET_GENERATOR,
    ASSET_TRANSFORMER,
    ASSET_WAREHOUSE,
    ASSET_VEHICLE

} AssetType;

/* ============================================================
   ASSET STATE
   ============================================================ */

typedef enum
{
    ASSET_OFFLINE,
    ASSET_IDLE,
    ASSET_RUNNING,
    ASSET_WARNING,
    ASSET_CRITICAL,
    ASSET_MAINTENANCE

} AssetState;

/* ============================================================
   SENSOR TYPES
   ============================================================ */

typedef enum
{
    SENSOR_TEMPERATURE,
    SENSOR_PRESSURE,
    SENSOR_VIBRATION,
    SENSOR_CURRENT,
    SENSOR_VOLTAGE,
    SENSOR_POWER,
    SENSOR_FLOW,
    SENSOR_SPEED,
    SENSOR_RPM,
    SENSOR_HUMIDITY,
    SENSOR_POSITION,
    SENSOR_THROUGHPUT

} SensorType;

/* ============================================================
   SENSOR
   ============================================================ */

typedef struct
{
    uint32_t id;

    uint32_t asset_id;

    SensorType type;

    float value;

    float previous_value;

    float minimum;
    float maximum;

    float warning_level;
    float critical_level;

    float filtered_value;

    uint64_t timestamp;

    bool online;

} Sensor;

/* ============================================================
   ASSET
   ============================================================ */

typedef struct
{
    uint32_t id;

    char name[64];

    AssetType type;

    AssetState state;

    Vec3 position;

    Vec3 rotation;

    Vec3 scale;

    float health;

    float efficiency;

    float energy_kw;

    float production_rate;

    float temperature;

    float vibration;

    float utilization;

    uint32_t sensor_ids[16];

    int sensor_count;

} IndustrialAsset;

/* ============================================================
   ALARMS
   ============================================================ */

typedef enum
{
    ALARM_INFO,
    ALARM_WARNING,
    ALARM_CRITICAL

} AlarmSeverity;

typedef struct
{
    uint32_t id;

    uint32_t asset_id;

    uint32_t sensor_id;

    AlarmSeverity severity;

    char message[128];

    float value;

    uint64_t timestamp;

    bool active;

} Alarm;

/* ============================================================
   PRODUCTION LINE
   ============================================================ */

typedef struct
{
    uint32_t id;

    char name[64];

    uint32_t asset_ids[64];

    int asset_count;

    float target_rate;

    float actual_rate;

    float efficiency;

    uint64_t units_produced;

} ProductionLine;

/* ============================================================
   ENERGY SYSTEM
   ============================================================ */

typedef struct
{
    float generation_mw;

    float consumption_mw;

    float renewable_mw;

    float storage_mw;

    float grid_mw;

    float factory_power_factor;

} EnergySystem;

/* ============================================================
   DIGITAL TWIN
   ============================================================ */

typedef struct
{
    IndustrialAsset assets[
        DT_MAX_ASSETS];

    int asset_count;

    Sensor sensors[
        DT_MAX_SENSORS];

    int sensor_count;

    Alarm alarms[
        DT_MAX_ALARMS];

    int alarm_count;

    ProductionLine lines[32];

    int line_count;

    EnergySystem energy;

    uint64_t simulation_time;

} DigitalTwin;

/* ============================================================
   ASSET CREATION
   ============================================================ */

static IndustrialAsset *digital_twin_add_asset(
    DigitalTwin *twin,
    const char *name,
    AssetType type,
    Vec3 position)
{
    if (twin->asset_count >=
        DT_MAX_ASSETS)
    {
        return NULL;
    }

    IndustrialAsset *asset =
        &twin->assets[
            twin->asset_count];

    memset(
        asset,
        0,
        sizeof(*asset));

    asset->id =
        (uint32_t)
        twin->asset_count + 1;

    strncpy(
        asset->name,
        name,
        sizeof(asset->name) - 1);

    asset->type =
        type;

    asset->state =
        ASSET_IDLE;

    asset->position =
        position;

    asset->rotation =
        (Vec3){0.0f, 0.0f, 0.0f};

    asset->scale =
        (Vec3){1.0f, 1.0f, 1.0f};

    asset->health =
        1.0f;

    asset->efficiency =
        1.0f;

    asset->utilization =
        0.0f;

    twin->asset_count++;

    return asset;
}

/* ============================================================
   SENSOR CREATION
   ============================================================ */

static Sensor *digital_twin_add_sensor(
    DigitalTwin *twin,
    uint32_t asset_id,
    SensorType type,
    float minimum,
    float maximum,
    float warning,
    float critical)
{
    if (twin->sensor_count >=
        DT_MAX_SENSORS)
    {
        return NULL;
    }

    Sensor *sensor =
        &twin->sensors[
            twin->sensor_count];

    memset(
        sensor,
        0,
        sizeof(*sensor));

    sensor->id =
        (uint32_t)
        twin->sensor_count + 1;

    sensor->asset_id =
        asset_id;

    sensor->type =
        type;

    sensor->minimum =
        minimum;

    sensor->maximum =
        maximum;

    sensor->warning_level =
        warning;

    sensor->critical_level =
        critical;

    sensor->online =
        true;

    twin->sensor_count++;

    /*
     * Attach sensor to asset.
     */

    for (int i = 0;
         i < twin->asset_count;
         ++i)
    {
        if (twin->assets[i].id ==
            asset_id)
        {
            IndustrialAsset *asset =
                &twin->assets[i];

            if (asset->sensor_count < 16)
            {
                asset->sensor_ids[
                    asset->sensor_count++] =
                    sensor->id;
            }

            break;
        }
    }

    return sensor;
}

/* ============================================================
   SENSOR LOOKUP
   ============================================================ */

static Sensor *find_sensor(
    DigitalTwin *twin,
    uint32_t id)
{
    for (int i = 0;
         i < twin->sensor_count;
         ++i)
    {
        if (twin->sensors[i].id == id)
            return &twin->sensors[i];
    }

    return NULL;
}

static IndustrialAsset *find_asset(
    DigitalTwin *twin,
    uint32_t id)
{
    for (int i = 0;
         i < twin->asset_count;
         ++i)
    {
        if (twin->assets[i].id == id)
            return &twin->assets[i];
    }

    return NULL;
}

/* ============================================================
   SENSOR UPDATE
   ============================================================ */

static void sensor_update(
    DigitalTwin *twin,
    uint32_t sensor_id,
    float value,
    uint64_t timestamp)
{
    Sensor *sensor =
        find_sensor(
            twin,
            sensor_id);

    if (!sensor)
        return;

    sensor->previous_value =
        sensor->value;

    sensor->value =
        value;

    /*
     * Exponential filtering.
     */

    sensor->filtered_value =
        lerpf(
            sensor->filtered_value,
            value,
            0.15f);

    sensor->timestamp =
        timestamp;

    sensor->online =
        true;
}

/* ============================================================
   ALARM SYSTEM
   ============================================================ */

static void create_alarm(
    DigitalTwin *twin,
    uint32_t asset_id,
    uint32_t sensor_id,
    AlarmSeverity severity,
    const char *message,
    float value)
{
    /*
     * Avoid unlimited duplicate alarms.
     */

    for (int i = 0;
         i < twin->alarm_count;
         ++i)
    {
        Alarm *alarm =
            &twin->alarms[i];

        if (alarm->active &&
            alarm->sensor_id ==
                sensor_id &&
            alarm->severity ==
                severity)
        {
            alarm->value =
                value;

            alarm->timestamp =
                twin->simulation_time;

            return;
        }
    }

    if (twin->alarm_count >=
        DT_MAX_ALARMS)
    {
        return;
    }

    Alarm *alarm =
        &twin->alarms[
            twin->alarm_count++];

    memset(
        alarm,
        0,
        sizeof(*alarm));

    alarm->id =
        (uint32_t)
        twin->alarm_count;

    alarm->asset_id =
        asset_id;

    alarm->sensor_id =
        sensor_id;

    alarm->severity =
        severity;

    alarm->value =
        value;

    alarm->timestamp =
        twin->simulation_time;

    alarm->active =
        true;

    strncpy(
        alarm->message,
        message,
        sizeof(alarm->message) - 1);
}

/* ============================================================
   SENSOR EVALUATION
   ============================================================ */

static void evaluate_sensor(
    DigitalTwin *twin,
    Sensor *sensor)
{
    IndustrialAsset *asset =
        find_asset(
            twin,
            sensor->asset_id);

    if (!asset)
        return;

    float value =
        sensor->filtered_value;

    /*
     * Critical threshold.
     */

    if (value >=
        sensor->critical_level)
    {
        asset->state =
            ASSET_CRITICAL;

        create_alarm(
            twin,
            asset->id,
            sensor->id,
            ALARM_CRITICAL,
            "Critical sensor threshold",
            value);

        return;
    }

    /*
     * Warning threshold.
     */

    if (value >=
        sensor->warning_level)
    {
        asset->state =
            ASSET_WARNING;

        create_alarm(
            twin,
            asset->id,
            sensor->id,
            ALARM_WARNING,
            "Sensor warning threshold",
            value);

        return;
    }
}

/* ============================================================
   EQUIPMENT HEALTH
   ============================================================ */

static void calculate_asset_health(
    DigitalTwin *twin,
    IndustrialAsset *asset)
{
    float temperature_penalty =
        0.0f;

    float vibration_penalty =
        0.0f;

    /*
     * Search attached sensors.
     */

    for (int i = 0;
         i < asset->sensor_count;
         ++i)
    {
        Sensor *sensor =
            find_sensor(
                twin,
                asset->sensor_ids[i]);

        if (!sensor)
            continue;

        if (sensor->type ==
            SENSOR_TEMPERATURE)
        {
            if (sensor->value >
                sensor->warning_level)
            {
                temperature_penalty =
                    clampf(
                        (sensor->value -
                         sensor->warning_level) /
                        (sensor->critical_level -
                         sensor->warning_level +
                         0.001f),
                        0.0f,
                        1.0f);
            }
        }

        if (sensor->type ==
            SENSOR_VIBRATION)
        {
            if (sensor->value >
                sensor->warning_level)
            {
                vibration_penalty =
                    clampf(
                        (sensor->value -
                         sensor->warning_level) /
                        (sensor->critical_level -
                         sensor->warning_level +
                         0.001f),
                        0.0f,
                        1.0f);
            }
        }
    }

    float penalty =
        temperature_penalty *
        0.45f +
        vibration_penalty *
        0.55f;

    asset->health =
        clampf(
            1.0f - penalty,
            0.0f,
            1.0f);

    asset->efficiency =
        clampf(
            asset->health *
            (1.0f -
             0.15f *
             (1.0f -
              asset->utilization)),
            0.0f,
            1.0f);
}

/* ============================================================
   PREDICTIVE MAINTENANCE
   ============================================================ */

typedef struct
{
    float temperature_trend;
    float vibration_trend;
    float health_trend;

    float failure_risk;

} MaintenancePrediction;

static MaintenancePrediction predict_failure(
    DigitalTwin *twin,
    IndustrialAsset *asset)
{
    MaintenancePrediction p;

    memset(
        &p,
        0,
        sizeof(p));

    float temp_trend = 0.0f;
    float vibration_trend = 0.0f;

    for (int i = 0;
         i < asset->sensor_count;
         ++i)
    {
        Sensor *sensor =
            find_sensor(
                twin,
                asset->sensor_ids[i]);

        if (!sensor)
            continue;

        float delta =
            sensor->value -
            sensor->previous_value;

        if (sensor->type ==
            SENSOR_TEMPERATURE)
        {
            temp_trend +=
                delta;
        }

        if (sensor->type ==
            SENSOR_VIBRATION)
        {
            vibration_trend +=
                delta;
        }
    }

    p.temperature_trend =
        temp_trend;

    p.vibration_trend =
        vibration_trend;

    /*
     * Simplified risk model.
     *
     * Production version would use
     * historical time-series data,
     * survival analysis or ML.
     */

    float risk =
        (1.0f -
         asset->health)
        * 0.60f;

    risk +=
        fabsf(
            vibration_trend)
        * 0.20f;

    risk +=
        fabsf(
            temp_trend)
        * 0.20f;

    p.failure_risk =
        clampf(
            risk,
            0.0f,
            1.0f);

    return p;
}

/* ============================================================
   ENERGY MODEL
   ============================================================ */

static void update_energy(
    DigitalTwin *twin)
{
    float consumption =
        0.0f;

    for (int i = 0;
         i < twin->asset_count;
         ++i)
    {
        IndustrialAsset *asset =
            &twin->assets[i];

        consumption +=
            asset->energy_kw;
    }

    twin->energy.consumption_mw =
        consumption / 1000.0f;

    twin->energy.factory_power_factor =
        0.94f;

    twin->energy.grid_mw =
        twin->energy.consumption_mw -
        twin->energy.generation_mw -
        twin->energy.storage_mw;

    if (twin->energy.grid_mw < 0.0f)
        twin->energy.grid_mw = 0.0f;
}

/* ============================================================
   PRODUCTION
   ============================================================ */

static void update_production(
    DigitalTwin *twin,
    float delta_seconds)
{
    for (int i = 0;
         i < twin->line_count;
         ++i)
    {
        ProductionLine *line =
            &twin->lines[i];

        float rate =
            0.0f;

        for (int j = 0;
             j < line->asset_count;
             ++j)
        {
            IndustrialAsset *asset =
                find_asset(
                    twin,
                    line->asset_ids[j]);

            if (!asset)
                continue;

            rate +=
                asset->production_rate;
        }

        if (line->asset_count > 0)
        {
            line->actual_rate =
                rate /
                line->asset_count;
        }

        line->efficiency =
            clampf(
                line->actual_rate /
                (line->target_rate +
                 0.0001f),
                0.0f,
                1.5f);

        line->units_produced +=
            (uint64_t)(
                line->actual_rate *
                delta_seconds);
    }
}

/* ============================================================
   DIGITAL TWIN UPDATE
   ============================================================ */

static void digital_twin_update(
    DigitalTwin *twin,
    float delta_seconds)
{
    twin->simulation_time +=
        (uint64_t)
        (delta_seconds * 1000.0f);

    /*
     * Evaluate sensors.
     */

    for (int i = 0;
         i < twin->sensor_count;
         ++i)
    {
        evaluate_sensor(
            twin,
            &twin->sensors[i]);
    }

    /*
     * Calculate equipment health.
     */

    for (int i = 0;
         i < twin->asset_count;
         ++i)
    {
        calculate_asset_health(
            twin,
            &twin->assets[i]);
    }

    update_energy(
        twin);

    update_production(
        twin,
        delta_seconds);
}

/* ============================================================
   VR CAMERA
   ============================================================ */

typedef struct
{
    Vec3 position;

    float yaw;
    float pitch;

    float movement_speed;

} VRCamera;

static void vr_camera_init(
    VRCamera *camera)
{
    memset(
        camera,
        0,
        sizeof(*camera));

    camera->position =
        (Vec3){
            0.0f,
            1.7f,
            5.0f
        };

    camera->movement_speed =
        2.0f;
}

/* ============================================================
   SPATIAL INSPECTION
   ============================================================ */

static IndustrialAsset *find_nearest_asset(
    DigitalTwin *twin,
    Vec3 position,
    float max_distance)
{
    IndustrialAsset *nearest =
        NULL;

    float best =
        max_distance;

    for (int i = 0;
         i < twin->asset_count;
         ++i)
    {
        IndustrialAsset *asset =
            &twin->assets[i];

        float d =
            distance3(
                position.x,
                position.y,
                position.z,
                asset->position.x,
                asset->position.y,
                asset->position.z);

        if (d < best)
        {
            best =
                d;

            nearest =
                asset;
        }
    }

    return nearest;
}

/* ============================================================
   VR INSPECTION PANEL
   ============================================================ */

static void display_asset_panel(
    DigitalTwin *twin,
    IndustrialAsset *asset)
{
    printf("\n");
    printf(
        "============================================\n");

    printf(
        " ASSET INSPECTION\n");

    printf(
        "============================================\n");

    printf(
        "Name:          %s\n",
        asset->name);

    printf(
        "ID:            %u\n",
        asset->id);

    printf(
        "Health:        %.1f %%\n",
        asset->health * 100.0f);

    printf(
        "Efficiency:    %.1f %%\n",
        asset->efficiency * 100.0f);

    printf(
        "Temperature:   %.1f C\n",
        asset->temperature);

    printf(
        "Vibration:     %.3f\n",
        asset->vibration);

    printf(
        "Energy:        %.1f kW\n",
        asset->energy_kw);

    printf(
        "Production:    %.1f units/min\n",
        asset->production_rate);

    printf(
        "Utilisation:   %.1f %%\n",
        asset->utilization * 100.0f);

    MaintenancePrediction prediction =
        predict_failure(
            twin,
            asset);

    printf(
        "Failure Risk:  %.1f %%\n",
        prediction.failure_risk *
        100.0f);

    printf(
        "============================================\n");
}

/* ============================================================
   FACILITY STATISTICS
   ============================================================ */

static void display_factory_dashboard(
    DigitalTwin *twin)
{
    int running = 0;
    int warnings = 0;
    int critical = 0;

    float health = 0.0f;

    for (int i = 0;
         i < twin->asset_count;
         ++i)
    {
        IndustrialAsset *a =
            &twin->assets[i];

        health +=
            a->health;

        if (a->state ==
            ASSET_RUNNING)
            running++;

        if (a->state ==
            ASSET_WARNING)
            warnings++;

        if (a->state ==
            ASSET_CRITICAL)
            critical++;
    }

    if (twin->asset_count > 0)
    {
        health /=
            twin->asset_count;
    }

    printf("\n");
    printf(
        "============================================\n");

    printf(
        " INDUSTRIAL DIGITAL TWIN\n");

    printf(
        "============================================\n");

    printf(
        "Assets:         %d\n",
        twin->asset_count);

    printf(
        "Running:        %d\n",
        running);

    printf(
        "Warnings:       %d\n",
        warnings);

    printf(
        "Critical:       %d\n",
        critical);

    printf(
        "Mean Health:    %.1f %%\n",
        health * 100.0f);

    printf(
        "Power:          %.2f MW\n",
        twin->energy.consumption_mw);

    printf(
        "Grid:           %.2f MW\n",
        twin->energy.grid_mw);

    printf(
        "Power Factor:   %.3f\n",
        twin->energy.factory_power_factor);

    printf(
        "Active Alarms:  %d\n",
        twin->alarm_count);

    printf(
        "============================================\n");
}

/* ============================================================
   DEMO FACTORY
   ============================================================ */

static void create_demo_factory(
    DigitalTwin *twin)
{
    memset(
        twin,
        0,
        sizeof(*twin));

    /*
     * Factory building.
     */

    IndustrialAsset *factory =
        digital_twin_add_asset(
            twin,
            "Main Factory",
            ASSET_FACTORY,
            (Vec3){
                0.0f,
                0.0f,
                0.0f
            });

    factory->scale =
        (Vec3){
            40.0f,
            8.0f,
            30.0f
        };

    /*
     * CNC machine.
     */

    IndustrialAsset *cnc =
        digital_twin_add_asset(
            twin,
            "CNC Cell 01",
            ASSET_MACHINE,
            (Vec3){
                -8.0f,
                1.0f,
                -5.0f
            });

    cnc->state =
        ASSET_RUNNING;

    cnc->energy_kw =
        75.0f;

    cnc->production_rate =
        18.0f;

    cnc->utilization =
        0.82f;

    digital_twin_add_sensor(
        twin,
        cnc->id,
        SENSOR_TEMPERATURE,
        0.0f,
        120.0f,
        75.0f,
        100.0f);

    digital_twin_add_sensor(
        twin,
        cnc->id,
        SENSOR_VIBRATION,
        0.0f,
        10.0f,
        4.0f,
        7.0f);

    digital_twin_add_sensor(
        twin,
        cnc->id,
        SENSOR_POWER,
        0.0f,
        150.0f,
        110.0f,
        140.0f);

    /*
     * Robot.
     */

    IndustrialAsset *robot =
        digital_twin_add_asset(
            twin,
            "Robotic Arm 01",
            ASSET_ROBOT,
            (Vec3){
                2.0f,
                1.0f,
                -4.0f
            });

    robot->state =
        ASSET_RUNNING;

    robot->energy_kw =
        22.0f;

    robot->production_rate =
        31.0f;

    robot->utilization =
        0.91f;

    digital_twin_add_sensor(
        twin,
        robot->id,
        SENSOR_TEMPERATURE,
        0.0f,
        100.0f,
        65.0f,
        85.0f);

    digital_twin_add_sensor(
        twin,
        robot->id,
        SENSOR_VIBRATION,
        0.0f,
        10.0f,
        3.0f,
        6.0f);

    /*
     * Conveyor.
     */

    IndustrialAsset *conveyor =
        digital_twin_add_asset(
            twin,
            "Conveyor Line A",
            ASSET_CONVEYOR,
            (Vec3){
                8.0f,
                1.0f,
                -4.0f
            });

    conveyor->state =
        ASSET_RUNNING;

    conveyor->energy_kw =
        15.0f;

    conveyor->production_rate =
        31.0f;

    conveyor->utilization =
        0.88f;

    /*
     * Pump.
     */

    IndustrialAsset *pump =
        digital_twin_add_asset(
            twin,
            "Cooling Pump 01",
            ASSET_PUMP,
            (Vec3){
                -5.0f,
                1.0f,
                7.0f
            });

    pump->state =
        ASSET_RUNNING;

    pump->energy_kw =
        11.0f;

    digital_twin_add_sensor(
        twin,
        pump->id,
        SENSOR_TEMPERATURE,
        0.0f,
        120.0f,
        70.0f,
        95.0f);

    digital_twin_add_sensor(
        twin,
        pump->id,
        SENSOR_VIBRATION,
        0.0f,
        10.0f,
        3.0f,
        6.0f);

    /*
     * Generator.
     */

    IndustrialAsset *generator =
        digital_twin_add_asset(
            twin,
            "Backup Generator",
            ASSET_GENERATOR,
            (Vec3){
                12.0f,
                2.0f,
                8.0f
            });

    generator->state =
        ASSET_IDLE;

    generator->energy_kw =
        0.0f;

    /*
     * Warehouse.
     */

    IndustrialAsset *warehouse =
        digital_twin_add_asset(
            twin,
            "Automated Warehouse",
            ASSET_WAREHOUSE,
            (Vec3){
                0.0f,
                0.0f,
                18.0f
            });

    warehouse->state =
        ASSET_RUNNING;

    warehouse->energy_kw =
        120.0f;

    warehouse->utilization =
        0.73f;

    /*
     * Production line.
     */

    ProductionLine *line =
        &twin->lines[
            twin->line_count++];

    memset(
        line,
        0,
        sizeof(*line));

    line->id = 1;

    strcpy(
        line->name,
        "Assembly Line A");

    line->target_rate =
        30.0f;

    line->asset_ids[
        line->asset_count++] =
        cnc->id;

    line->asset_ids[
        line->asset_count++] =
        robot->id;

    line->asset_ids[
        line->asset_count++] =
        conveyor->id;

    /*
     * Energy infrastructure.
     */

    twin->energy.generation_mw =
        0.50f;

    twin->energy.renewable_mw =
        0.18f;

    twin->energy.storage_mw =
        0.05f;
}

/* ============================================================
   SIMULATED TELEMETRY
   ============================================================ */

static float random_unit(void)
{
    return
        (float)rand() /
        (float)RAND_MAX;
}

static void generate_demo_telemetry(
    DigitalTwin *twin)
{
    for (int i = 0;
         i < twin->sensor_count;
         ++i)
    {
        Sensor *sensor =
            &twin->sensors[i];

        IndustrialAsset *asset =
            find_asset(
                twin,
                sensor->asset_id);

        if (!asset)
            continue;

        float value =
            0.0f;

        switch (sensor->type)
        {
            case SENSOR_TEMPERATURE:

                value =
                    45.0f +
                    asset->utilization *
                    25.0f +
                    (random_unit() - 0.5f)
                    * 4.0f;

                break;

            case SENSOR_VIBRATION:

                value =
                    0.8f +
                    asset->utilization *
                    2.0f +
                    (random_unit() - 0.5f)
                    * 0.5f;

                break;

            case SENSOR_POWER:

                value =
                    asset->energy_kw *
                    (0.75f +
                     random_unit() *
                     0.20f);

                break;

            default:

                value =
                    sensor->minimum +
                    random_unit() *
                    (sensor->maximum -
                     sensor->minimum);

                break;
        }

        sensor_update(
            twin,
            sensor->id,
            value,
            twin->simulation_time);
    }
}

/* ============================================================
   MAIN
   ============================================================ */

int main(void)
{
    printf("\n");
    printf(
        "============================================\n");
    printf(
        " META QUEST INDUSTRIAL DIGITAL TWIN\n");
    printf(
        "============================================\n\n");

    DigitalTwin twin;

    create_demo_factory(
        &twin);

    VRCamera camera;

    vr_camera_init(
        &camera);

    /*
     * Simulate approximately 10 seconds
     * of factory operation.
     */

    for (int frame = 0;
         frame < 300;
         ++frame)
    {
        generate_demo_telemetry(
            &twin);

        digital_twin_update(
            &twin,
            1.0f / 30.0f);

        if ((frame % 60) == 0)
        {
            display_factory_dashboard(
                &twin);
        }
    }

    /*
     * Place the VR user near the CNC.
     */

    camera.position =
        (Vec3){
            -6.0f,
            1.7f,
            -4.0f
        };

    IndustrialAsset *nearest =
        find_nearest_asset(
            &twin,
            camera.position,
            5.0f);

    if (nearest)
    {
        display_asset_panel(
            &twin,
            nearest);
    }

    printf("\n");
    printf(
        "Digital twin simulation complete.\n");

    return 0;
}


/*
 * vr_robotics_teleoperation.c
 *
 * Meta Quest VR
 * #9 - Robotics Control & Teleoperation
 *
 * C11 / C17
 *
 * Features:
 *   - VR head tracking
 *   - Hand/controller tracking
 *   - Robot joint model
 *   - Forward kinematics
 *   - Inverse kinematics
 *   - Cartesian end-effector control
 *   - Workspace limits
 *   - Velocity limiting
 *   - Acceleration limiting
 *   - Collision-distance monitoring
 *   - Emergency stop
 *   - Telemetry
 *   - Virtual robot simulation
 *
 * Intended architecture:
 *
 *       Meta Quest
 *            |
 *       OpenXR tracking
 *            |
 *       Hand/controller pose
 *            |
 *            v
 *     Teleoperation Layer
 *            |
 *       Safety Manager
 *            |
 *       Inverse Kinematics
 *            |
 *       Joint Controller
 *            |
 *       Robot Interface
 *            |
 *       Physical Robot
 *
 * IMPORTANT:
 * This is a control/simulation foundation.
 * A real industrial robot requires a certified
 * safety controller, hardware interlocks and
 * manufacturer-specific robot APIs.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <math.h>

#define ROBOT_MAX_JOINTS       7
#define ROBOT_MAX_LINKS        7
#define ROBOT_MAX_OBSTACLES    64

#define ROBOT_PI               3.14159265358979323846f

#define CONTROL_RATE_HZ        250.0f
#define CONTROL_DT             (1.0f / CONTROL_RATE_HZ)

#define MAX_LINEAR_SPEED       1.5f
#define MAX_ANGULAR_SPEED      2.5f

#define POSITION_TOLERANCE     0.002f

/* ============================================================
   VECTOR
   ============================================================ */

typedef struct
{
    float x;
    float y;
    float z;

} Vec3;

static Vec3 vec3(
    float x,
    float y,
    float z)
{
    Vec3 v;

    v.x = x;
    v.y = y;
    v.z = z;

    return v;
}

static Vec3 vec_add(
    Vec3 a,
    Vec3 b)
{
    return vec3(
        a.x + b.x,
        a.y + b.y,
        a.z + b.z);
}

static Vec3 vec_sub(
    Vec3 a,
    Vec3 b)
{
    return vec3(
        a.x - b.x,
        a.y - b.y,
        a.z - b.z);
}

static Vec3 vec_scale(
    Vec3 a,
    float s)
{
    return vec3(
        a.x * s,
        a.y * s,
        a.z * s);
}

static float vec_dot(
    Vec3 a,
    Vec3 b)
{
    return
        a.x * b.x +
        a.y * b.y +
        a.z * b.z;
}

static float vec_length(
    Vec3 a)
{
    return sqrtf(
        vec_dot(a, a));
}

static Vec3 vec_normalize(
    Vec3 a)
{
    float l =
        vec_length(a);

    if (l < 0.000001f)
        return vec3(0, 0, 0);

    return vec_scale(
        a,
        1.0f / l);
}

/* ============================================================
   QUATERNION
   ============================================================ */

typedef struct
{
    float w;
    float x;
    float y;
    float z;

} Quat;

static Quat quat_identity(void)
{
    return (Quat){
        1.0f,
        0.0f,
        0.0f,
        0.0f
    };
}

static Quat quat_normalize(
    Quat q)
{
    float l =
        sqrtf(
            q.w*q.w +
            q.x*q.x +
            q.y*q.y +
            q.z*q.z);

    if (l < 0.000001f)
        return quat_identity();

    q.w /= l;
    q.x /= l;
    q.y /= l;
    q.z /= l;

    return q;
}

/* ============================================================
   POSE
   ============================================================ */

typedef struct
{
    Vec3 position;
    Quat orientation;

} Pose;

/* ============================================================
   VR INPUT
   ============================================================ */

typedef struct
{
    Pose head;

    Pose left_hand;
    Pose right_hand;

    bool left_grip;
    bool right_grip;

    bool left_trigger;
    bool right_trigger;

    bool emergency_button;

} VRInput;

/* ============================================================
   ROBOT JOINT
   ============================================================ */

typedef struct
{
    char name[32];

    float angle;

    float target_angle;

    float velocity;

    float target_velocity;

    float acceleration;

    float min_angle;
    float max_angle;

    float max_velocity;
    float max_acceleration;

} RobotJoint;

/* ============================================================
   ROBOT LINK
   ============================================================ */

typedef struct
{
    float length;

    float radius;

    Vec3 position;

} RobotLink;

/* ============================================================
   END EFFECTOR
   ============================================================ */

typedef struct
{
    Pose pose;

    float grip;

    bool holding_object;

} EndEffector;

/* ============================================================
   OBSTACLE
   ============================================================ */

typedef struct
{
    Vec3 center;

    Vec3 half_extents;

    float safety_margin;

} Obstacle;

/* ============================================================
   ROBOT
   ============================================================ */

typedef struct
{
    RobotJoint joints[
        ROBOT_MAX_JOINTS];

    RobotLink links[
        ROBOT_MAX_LINKS];

    int joint_count;

    EndEffector end_effector;

    Obstacle obstacles[
        ROBOT_MAX_OBSTACLES];

    int obstacle_count;

    bool enabled;

    bool emergency_stop;

} Robot;

/* ============================================================
   MATRIX
   ============================================================ */

typedef struct
{
    float m[4][4];

} Mat4;

static Mat4 mat4_identity(void)
{
    Mat4 m;

    memset(
        &m,
        0,
        sizeof(m));

    for (int i = 0;
         i < 4;
         ++i)
    {
        m.m[i][i] = 1.0f;
    }

    return m;
}

static Mat4 mat4_multiply(
    Mat4 a,
    Mat4 b)
{
    Mat4 r;

    memset(
        &r,
        0,
        sizeof(r));

    for (int row = 0;
         row < 4;
         ++row)
    {
        for (int col = 0;
             col < 4;
             ++col)
        {
            for (int k = 0;
                 k < 4;
                 ++k)
            {
                r.m[row][col] +=
                    a.m[row][k] *
                    b.m[k][col];
            }
        }
    }

    return r;
}

static Mat4 mat4_translate(
    Vec3 p)
{
    Mat4 m =
        mat4_identity();

    m.m[0][3] =
        p.x;

    m.m[1][3] =
        p.y;

    m.m[2][3] =
        p.z;

    return m;
}

/* ============================================================
   ROTATION MATRICES
   ============================================================ */

static Mat4 mat4_rotate_x(
    float a)
{
    Mat4 m =
        mat4_identity();

    float c =
        cosf(a);

    float s =
        sinf(a);

    m.m[1][1] = c;
    m.m[1][2] = -s;
    m.m[2][1] = s;
    m.m[2][2] = c;

    return m;
}

static Mat4 mat4_rotate_y(
    float a)
{
    Mat4 m =
        mat4_identity();

    float c =
        cosf(a);

    float s =
        sinf(a);

    m.m[0][0] = c;
    m.m[0][2] = s;
    m.m[2][0] = -s;
    m.m[2][2] = c;

    return m;
}

static Mat4 mat4_rotate_z(
    float a)
{
    Mat4 m =
        mat4_identity();

    float c =
        cosf(a);

    float s =
        sinf(a);

    m.m[0][0] = c;
    m.m[0][1] = -s;
    m.m[1][0] = s;
    m.m[1][1] = c;

    return m;
}

/* ============================================================
   FORWARD KINEMATICS
   ============================================================ */

static Pose robot_forward_kinematics(
    Robot *robot)
{
    Mat4 transform =
        mat4_identity();

    for (int i = 0;
         i < robot->joint_count;
         ++i)
    {
        RobotJoint *joint =
            &robot->joints[i];

        RobotLink *link =
            &robot->links[i];

        /*
         * Alternating axes creates
         * a simple 6/7-axis manipulator.
         */

        if ((i % 3) == 0)
        {
            transform =
                mat4_multiply(
                    transform,
                    mat4_rotate_z(
                        joint->angle));
        }
        else if ((i % 3) == 1)
        {
            transform =
                mat4_multiply(
                    transform,
                    mat4_rotate_y(
                        joint->angle));
        }
        else
        {
            transform =
                mat4_multiply(
                    transform,
                    mat4_rotate_x(
                        joint->angle));
        }

        transform =
            mat4_multiply(
                transform,
                mat4_translate(
                    vec3(
                        link->length,
                        0.0f,
                        0.0f)));
    }

    Pose result;

    result.position =
        vec3(
            transform.m[0][3],
            transform.m[1][3],
            transform.m[2][3]);

    /*
     * Orientation is represented here
     * as identity for the compact example.
     *
     * A production implementation would
     * extract the quaternion from the
     * final rotation matrix.
     */

    result.orientation =
        quat_identity();

    return result;
}

/* ============================================================
   POSITION ERROR
   ============================================================ */

static Vec3 pose_position_error(
    Pose current,
    Pose target)
{
    return vec_sub(
        target.position,
        current.position);
}

/* ============================================================
   JACOBIAN NUMERICAL APPROXIMATION
   ============================================================ */

static Vec3 numerical_joint_gradient(
    Robot *robot,
    int joint_index,
    Vec3 target)
{
    const float epsilon =
        0.0005f;

    float original =
        robot->joints[
            joint_index].angle;

    Pose base =
        robot_forward_kinematics(
            robot);

    Vec3 error_base =
        pose_position_error(
            base,
            (Pose){
                target,
                quat_identity()
            });

    robot->joints[
        joint_index].angle =
        original + epsilon;

    Pose moved =
        robot_forward_kinematics(
            robot);

    Vec3 error_moved =
        pose_position_error(
            moved,
            (Pose){
                target,
                quat_identity()
            });

    robot->joints[
        joint_index].angle =
        original;

    Vec3 derivative =
        vec_scale(
            vec_sub(
                error_moved,
                error_base),
            1.0f / epsilon);

    return derivative;
}

/* ============================================================
   INVERSE KINEMATICS
   ============================================================ */

static bool robot_inverse_kinematics(
    Robot *robot,
    Pose target)
{
    const int iterations =
        80;

    const float learning_rate =
        0.45f;

    for (int iteration = 0;
         iteration < iterations;
         ++iteration)
    {
        Pose current =
            robot_forward_kinematics(
                robot);

        Vec3 error =
            pose_position_error(
                current,
                target);

        float error_length =
            vec_length(error);

        if (error_length <
            POSITION_TOLERANCE)
        {
            return true;
        }

        /*
         * Gradient-descent style IK.
         */

        for (int j = 0;
             j < robot->joint_count;
             ++j)
        {
            Vec3 gradient =
                numerical_joint_gradient(
                    robot,
                    j,
                    target.position);

            float influence =
                vec_dot(
                    gradient,
                    error);

            float delta =
                learning_rate *
                influence *
                0.01f;

            delta =
                clampf(
                    delta,
                    -0.08f,
                    0.08f);

            robot->joints[j].angle +=
                delta;

            robot->joints[j].angle =
                clampf(
                    robot->joints[j].angle,
                    robot->joints[j].min_angle,
                    robot->joints[j].max_angle);
        }
    }

    return false;
}

/* ============================================================
   WORKSPACE SAFETY
   ============================================================ */

static bool point_inside_obstacle(
    Vec3 p,
    Obstacle *o)
{
    float dx =
        fabsf(
            p.x -
            o->center.x);

    float dy =
        fabsf(
            p.y -
            o->center.y);

    float dz =
        fabsf(
            p.z -
            o->center.z);

    return
        dx <=
            o->half_extents.x +
            o->safety_margin &&

        dy <=
            o->half_extents.y +
            o->safety_margin &&

        dz <=
            o->half_extents.z +
            o->safety_margin;
}

/* ============================================================
   COLLISION CHECK
   ============================================================ */

static bool robot_collision_check(
    Robot *robot)
{
    Pose end =
        robot_forward_kinematics(
            robot);

    for (int i = 0;
         i < robot->obstacle_count;
         ++i)
    {
        if (point_inside_obstacle(
                end.position,
                &robot->obstacles[i]))
        {
            return true;
        }
    }

    return false;
}

/* ============================================================
   VELOCITY LIMITER
   ============================================================ */

static float approach(
    float current,
    float target,
    float maximum_delta)
{
    float delta =
        target -
        current;

    if (delta >
        maximum_delta)
    {
        delta =
            maximum_delta;
    }

    if (delta <
        -maximum_delta)
    {
        delta =
            -maximum_delta;
    }

    return current +
           delta;
}

static void robot_update_joints(
    Robot *robot,
    float dt)
{
    if (!robot->enabled ||
        robot->emergency_stop)
    {
        for (int i = 0;
             i < robot->joint_count;
             ++i)
        {
            robot->joints[i]
                .target_velocity = 0.0f;
        }
    }

    for (int i = 0;
         i < robot->joint_count;
         ++i)
    {
        RobotJoint *joint =
            &robot->joints[i];

        float desired_velocity =
            joint->target_velocity;

        desired_velocity =
            clampf(
                desired_velocity,
                -joint->max_velocity,
                joint->max_velocity);

        /*
         * Acceleration limiting.
         */

        float maximum_delta =
            joint->max_acceleration *
            dt;

        joint->velocity =
            approach(
                joint->velocity,
                desired_velocity,
                maximum_delta);

        joint->angle +=
            joint->velocity *
            dt;

        joint->angle =
            clampf(
                joint->angle,
                joint->min_angle,
                joint->max_angle);
    }
}

/* ============================================================
   TELEOPERATION SAFETY
   ============================================================ */

typedef struct
{
    bool tracking_valid;

    bool emergency_stop;

    bool workspace_safe;

    bool velocity_safe;

    bool command_allowed;

} SafetyState;

static SafetyState evaluate_safety(
    Robot *robot,
    VRInput *input)
{
    SafetyState s;

    memset(
        &s,
        0,
        sizeof(s));

    s.tracking_valid =
        true;

    s.emergency_stop =
        robot->emergency_stop ||
        input->emergency_button;

    s.workspace_safe =
        !robot_collision_check(
            robot);

    s.velocity_safe =
        true;

    s.command_allowed =
        s.tracking_valid &&
        !s.emergency_stop &&
        s.workspace_safe &&
        s.velocity_safe;

    return s;
}

/* ============================================================
   VR → ROBOT MAPPING
   ============================================================ */

typedef struct
{
    Pose origin;

    float position_scale;

    bool clutch;

} TeleoperationMapping;

static void teleoperation_init(
    TeleoperationMapping *mapping)
{
    memset(
        mapping,
        0,
        sizeof(*mapping));

    mapping->origin =
        (Pose){
            vec3(
                0.0f,
                0.0f,
                0.0f),
            quat_identity()
        };

    mapping->position_scale =
        1.0f;
}

static Pose map_hand_to_robot(
    TeleoperationMapping *mapping,
    Pose hand)
{
    Pose result;

    result.position =
        vec_add(
            mapping->origin.position,
            vec_scale(
                hand.position,
                mapping->position_scale));

    result.orientation =
        hand.orientation;

    return result;
}

/* ============================================================
   COMMAND GENERATION
   ============================================================ */

static void generate_robot_command(
    Robot *robot,
    TeleoperationMapping *mapping,
    VRInput *input,
    SafetyState *safety)
{
    if (!safety->command_allowed)
        return;

    if (!input->right_grip)
        return;

    Pose target =
        map_hand_to_robot(
            mapping,
            input->right_hand);

    /*
     * Solve target pose.
     */

    bool solved =
        robot_inverse_kinematics(
            robot,
            target);

    if (!solved)
        return;

    /*
     * Convert desired joint positions
     * into velocity commands.
     */

    for (int i = 0;
         i < robot->joint_count;
         ++i)
    {
        RobotJoint *joint =
            &robot->joints[i];

        float error =
            joint->angle -
            joint->target_angle;

        joint->target_velocity =
            -error * 5.0f;

        joint->target_velocity =
            clampf(
                joint->target_velocity,
                -joint->max_velocity,
                joint->max_velocity);
    }
}

/* ============================================================
   ROBOT CREATION
   ============================================================ */

static void robot_init(
    Robot *robot)
{
    memset(
        robot,
        0,
        sizeof(*robot));

    robot->joint_count =
        6;

    const char *names[] =
    {
        "Base",
        "Shoulder",
        "Elbow",
        "Wrist 1",
        "Wrist 2",
        "Wrist 3"
    };

    float lengths[] =
    {
        0.35f,
        0.45f,
        0.40f,
        0.20f,
        0.15f,
        0.10f
    };

    for (int i = 0;
         i < robot->joint_count;
         ++i)
    {
        RobotJoint *joint =
            &robot->joints[i];

        RobotLink *link =
            &robot->links[i];

        strncpy(
            joint->name,
            names[i],
            sizeof(joint->name) - 1);

        joint->angle =
            0.0f;

        joint->target_angle =
            0.0f;

        joint->velocity =
            0.0f;

        joint->min_angle =
            -ROBOT_PI;

        joint->max_angle =
            ROBOT_PI;

        joint->max_velocity =
            2.0f;

        joint->max_acceleration =
            4.0f;

        link->length =
            lengths[i];

        link->radius =
            0.05f;
    }

    robot->enabled =
        true;

    robot->emergency_stop =
        false;

    /*
     * Example safety obstacle.
     */

    robot->obstacles[0] =
        (Obstacle){
            vec3(
                0.5f,
                0.0f,
                0.2f),

            vec3(
                0.15f,
                0.30f,
                0.15f),

            0.05f
        };

    robot->obstacle_count =
        1;
}

/* ============================================================
   ROBOT STATUS
   ============================================================ */

static void print_robot_status(
    Robot *robot,
    SafetyState *safety)
{
    Pose pose =
        robot_forward_kinematics(
            robot);

    printf("\n");
    printf(
        "============================================\n");

    printf(
        " VR ROBOT STATUS\n");

    printf(
        "============================================\n");

    printf(
        "Enabled:       %s\n",
        robot->enabled ?
            "YES" : "NO");

    printf(
        "E-Stop:        %s\n",
        robot->emergency_stop ?
            "ACTIVE" : "CLEAR");

    printf(
        "Tracking:      %s\n",
        safety->tracking_valid ?
            "VALID" : "INVALID");

    printf(
        "Workspace:     %s\n",
        safety->workspace_safe ?
            "SAFE" : "BLOCKED");

    printf(
        "Command:       %s\n",
        safety->command_allowed ?
            "ALLOWED" : "STOPPED");

    printf(
        "\nEnd Effector\n");

    printf(
        "X:             %.3f m\n",
        pose.position.x);

    printf(
        "Y:             %.3f m\n",
        pose.position.y);

    printf(
        "Z:             %.3f m\n",
        pose.position.z);

    printf(
        "\nJoint Positions\n");

    for (int i = 0;
         i < robot->joint_count;
         ++i)
    {
        printf(
            "J%d %-12s %+.3f rad\n",
            i + 1,
            robot->joints[i].name,
            robot->joints[i].angle);
    }

    printf(
        "============================================\n");
}

/* ============================================================
   SIMULATED QUEST INPUT
   ============================================================ */

static VRInput generate_vr_input(
    float time)
{
    VRInput input;

    memset(
        &input,
        0,
        sizeof(input));

    input.head.position =
        vec3(
            0.0f,
            1.7f,
            1.0f);

    input.head.orientation =
        quat_identity();

    /*
     * Simulated right-hand motion.
     */

    input.right_hand.position =
        vec3(
            0.55f +
                0.12f *
                sinf(time),

            0.20f +
                0.10f *
                cosf(time * 0.7f),

            0.20f +
                0.12f *
                sinf(time * 0.5f));

    input.right_hand.orientation =
        quat_identity();

    input.right_grip =
        true;

    input.right_trigger =
        true;

    input.emergency_button =
        false;

    return input;
}

/* ============================================================
   TELEOPERATION ENGINE
   ============================================================ */

typedef struct
{
    Robot robot;

    TeleoperationMapping mapping;

    VRInput input;

    SafetyState safety;

    float time;

} VRTeleoperation;

/* ------------------------------------------------------------
   Initialise
   ------------------------------------------------------------ */

static void teleoperation_engine_init(
    VRTeleoperation *engine)
{
    memset(
        engine,
        0,
        sizeof(*engine));

    robot_init(
        &engine->robot);

    teleoperation_init(
        &engine->mapping);

    engine->time =
        0.0f;
}

/* ------------------------------------------------------------
   Update
   ------------------------------------------------------------ */

static void teleoperation_engine_update(
    VRTeleoperation *engine,
    float dt)
{
    engine->time +=
        dt;

    engine->input =
        generate_vr_input(
            engine->time);

    engine->safety =
        evaluate_safety(
            &engine->robot,
            &engine->input);

    /*
     * Emergency stop has absolute
     * priority.
     */

    if (engine->input.emergency_button)
    {
        engine->robot.emergency_stop =
            true;
    }

    if (!engine->robot.emergency_stop)
    {
        generate_robot_command(
            &engine->robot,
            &engine->mapping,
            &engine->input,
            &engine->safety);
    }

    robot_update_joints(
        &engine->robot,
        dt);
}

/* ============================================================
   MAIN
   ============================================================ */

int main(void)
{
    printf("\n");
    printf(
        "============================================\n");
    printf(
        " META QUEST VR ROBOTICS TELEOPERATION\n");
    printf(
        "============================================\n");

    printf(
        "Control frequency: %.0f Hz\n",
        CONTROL_RATE_HZ);

    printf(
        "Control period:    %.3f ms\n",
        CONTROL_DT * 1000.0f);

    VRTeleoperation engine;

    teleoperation_engine_init(
        &engine);

    /*
     * Simulate five seconds.
     */

    const int frames =
        (int)(
            5.0f *
            CONTROL_RATE_HZ);

    for (int i = 0;
         i < frames;
         ++i)
    {
        teleoperation_engine_update(
            &engine,
            CONTROL_DT);

        if ((i % 250) == 0)
        {
            print_robot_status(
                &engine.robot,
                &engine.safety);
        }
    }

    printf("\n");
    printf(
        "Teleoperation simulation complete.\n");

    return 0;
}





/*
 * vr_spatial_os.c
 *
 * #10 - VR Operating System / Spatial OS
 *
 * C11 / C17
 *
 * Concept:
 *   A native C spatial-computing shell for Meta Quest.
 *
 * Features:
 *   - 3D desktop
 *   - Spatial windows
 *   - 3D filesystem
 *   - Folders and files
 *   - Persistent spatial objects
 *   - Applications
 *   - Notifications
 *   - Hand/controller interaction
 *   - Window manipulation
 *   - Spatial taskbar
 *   - Virtual terminal
 *   - Desktop/workspace switching
 *   - Head/hand tracking abstraction
 *   - Application lifecycle
 *
 * Production architecture:
 *
 *        Meta Quest
 *             |
 *          OpenXR
 *             |
 *      Spatial Input Layer
 *             |
 *       Spatial Shell
 *        /    |     \
 *       /     |      \
 *   Files   Apps   Notifications
 *       \     |      /
 *        \    |     /
 *         Spatial Scene
 *              |
 *        Vulkan/OpenGL ES
 *
 * This file is the platform-independent C
 * foundation. Real Quest deployment would
 * connect the renderer/input/storage layers
 * to Android NDK + OpenXR.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <math.h>

#define VR_MAX_WINDOWS          64
#define VR_MAX_FILES            512
#define VR_MAX_APPS             64
#define VR_MAX_NOTIFICATIONS    128
#define VR_MAX_WORKSPACES       16
#define VR_MAX_NAME             96
#define VR_MAX_PATH             256

#define VR_PI 3.14159265358979323846f

/* ============================================================
   VECTOR
   ============================================================ */

typedef struct
{
    float x;
    float y;
    float z;

} Vec3;

static Vec3 vec3(
    float x,
    float y,
    float z)
{
    return (Vec3){x, y, z};
}

static Vec3 vec_add(
    Vec3 a,
    Vec3 b)
{
    return vec3(
        a.x + b.x,
        a.y + b.y,
        a.z + b.z);
}

static Vec3 vec_sub(
    Vec3 a,
    Vec3 b)
{
    return vec3(
        a.x - b.x,
        a.y - b.y,
        a.z - b.z);
}

static Vec3 vec_scale(
    Vec3 a,
    float s)
{
    return vec3(
        a.x * s,
        a.y * s,
        a.z * s);
}

static float vec_length(
    Vec3 a)
{
    return sqrtf(
        a.x*a.x +
        a.y*a.y +
        a.z*a.z);
}

/* ============================================================
   QUATERNION
   ============================================================ */

typedef struct
{
    float w;
    float x;
    float y;
    float z;

} Quat;

static Quat quat_identity(void)
{
    return (Quat){
        1.0f,
        0.0f,
        0.0f,
        0.0f
    };
}

/* ============================================================
   POSE
   ============================================================ */

typedef struct
{
    Vec3 position;
    Quat orientation;

} Pose;

/* ============================================================
   SPATIAL INPUT
   ============================================================ */

typedef struct
{
    Pose head;

    Pose left_hand;
    Pose right_hand;

    bool left_pinched;
    bool right_pinched;

    bool left_trigger;
    bool right_trigger;

    bool menu_pressed;

} SpatialInput;

/* ============================================================
   OBJECT TYPES
   ============================================================ */

typedef enum
{
    OBJECT_EMPTY = 0,
    OBJECT_WINDOW,
    OBJECT_FILE,
    OBJECT_FOLDER,
    OBJECT_APP,
    OBJECT_NOTIFICATION,
    OBJECT_WIDGET,
    OBJECT_TERMINAL

} SpatialObjectType;

/* ============================================================
   FILE SYSTEM
   ============================================================ */

typedef enum
{
    FILE_UNKNOWN = 0,
    FILE_TEXT,
    FILE_CODE,
    FILE_IMAGE,
    FILE_VIDEO,
    FILE_AUDIO,
    FILE_DOCUMENT,
    FILE_FOLDER

} FileType;

typedef struct
{
    uint32_t id;

    char name[
        VR_MAX_NAME];

    char path[
        VR_MAX_PATH];

    FileType type;

    uint64_t size;

    uint64_t created;

    uint64_t modified;

    uint32_t parent_id;

    bool is_directory;

    bool selected;

} SpatialFile;

/* ============================================================
   WINDOW
   ============================================================ */

typedef struct
{
    uint32_t id;

    char title[
        VR_MAX_NAME];

    Pose pose;

    float width;

    float height;

    float depth;

    float opacity;

    bool visible;

    bool focused;

    bool minimized;

    bool maximized;

    bool movable;

    bool resizable;

    uint32_t workspace;

    uint32_t application_id;

} SpatialWindow;

/* ============================================================
   APPLICATION
   ============================================================ */

typedef enum
{
    APP_SYSTEM = 0,
    APP_FILE_MANAGER,
    APP_TERMINAL,
    APP_BROWSER,
    APP_EDITOR,
    APP_MEDIA,
    APP_SETTINGS,
    APP_MONITOR,
    APP_3D_VIEWER

} ApplicationType;

typedef struct
{
    uint32_t id;

    char name[
        VR_MAX_NAME];

    ApplicationType type;

    bool running;

    bool minimized;

    bool background;

    uint32_t primary_window;

} SpatialApplication;

/* ============================================================
   NOTIFICATION
   ============================================================ */

typedef enum
{
    NOTIFICATION_INFO = 0,
    NOTIFICATION_SUCCESS,
    NOTIFICATION_WARNING,
    NOTIFICATION_ERROR

} NotificationType;

typedef struct
{
    uint32_t id;

    char title[
        VR_MAX_NAME];

    char message[
        256];

    NotificationType type;

    float lifetime;

    float age;

    bool visible;

} SpatialNotification;

/* ============================================================
   WORKSPACE
   ============================================================ */

typedef struct
{
    uint32_t id;

    char name[
        VR_MAX_NAME];

    Vec3 origin;

    bool active;

} SpatialWorkspace;

/* ============================================================
   SPATIAL SHELL
   ============================================================ */

typedef struct
{
    SpatialObjectType type;

    uint32_t id;

    Pose pose;

    bool selected;

} SpatialObject;

/* ============================================================
   TERMINAL
   ============================================================ */

#define TERMINAL_HISTORY 64

typedef struct
{
    char history[
        TERMINAL_HISTORY][256];

    int count;

    char command[
        256];

} SpatialTerminal;

/* ============================================================
   SPATIAL OS
   ============================================================ */

typedef struct
{
    SpatialFile files[
        VR_MAX_FILES];

    int file_count;

    SpatialWindow windows[
        VR_MAX_WINDOWS];

    int window_count;

    SpatialApplication apps[
        VR_MAX_APPS];

    int app_count;

    SpatialNotification notifications[
        VR_MAX_NOTIFICATIONS];

    int notification_count;

    SpatialWorkspace workspaces[
        VR_MAX_WORKSPACES];

    int workspace_count;

    SpatialTerminal terminal;

    SpatialInput input;

    uint32_t active_workspace;

    uint32_t focused_window;

    uint32_t next_file_id;

    uint32_t next_window_id;

    uint32_t next_app_id;

    uint32_t next_notification_id;

    float time;

} SpatialOS;

/* ============================================================
   FILE SYSTEM
   ============================================================ */

static uint32_t fs_create_file(
    SpatialOS *os,
    const char *name,
    const char *path,
    FileType type,
    uint64_t size)
{
    if (os->file_count >=
        VR_MAX_FILES)
    {
        return 0;
    }

    SpatialFile *file =
        &os->files[
            os->file_count++];

    memset(
        file,
        0,
        sizeof(*file));

    file->id =
        ++os->next_file_id;

    strncpy(
        file->name,
        name,
        VR_MAX_NAME - 1);

    strncpy(
        file->path,
        path,
        VR_MAX_PATH - 1);

    file->type =
        type;

    file->size =
        size;

    file->created =
        0;

    file->modified =
        0;

    file->is_directory =
        type == FILE_FOLDER;

    return file->id;
}

static SpatialFile *fs_find(
    SpatialOS *os,
    uint32_t id)
{
    for (int i = 0;
         i < os->file_count;
         ++i)
    {
        if (os->files[i].id == id)
            return &os->files[i];
    }

    return NULL;
}

/* ============================================================
   APPLICATION MANAGER
   ============================================================ */

static uint32_t app_launch(
    SpatialOS *os,
    const char *name,
    ApplicationType type)
{
    if (os->app_count >=
        VR_MAX_APPS)
    {
        return 0;
    }

    SpatialApplication *app =
        &os->apps[
            os->app_count++];

    memset(
        app,
        0,
        sizeof(*app));

    app->id =
        ++os->next_app_id;

    strncpy(
        app->name,
        name,
        VR_MAX_NAME - 1);

    app->type =
        type;

    app->running =
        true;

    app->background =
        false;

    return app->id;
}

/* ============================================================
   WINDOW MANAGER
   ============================================================ */

static uint32_t window_create(
    SpatialOS *os,
    const char *title,
    Pose pose,
    float width,
    float height,
    uint32_t app_id)
{
    if (os->window_count >=
        VR_MAX_WINDOWS)
    {
        return 0;
    }

    SpatialWindow *window =
        &os->windows[
            os->window_count++];

    memset(
        window,
        0,
        sizeof(*window));

    window->id =
        ++os->next_window_id;

    strncpy(
        window->title,
        title,
        VR_MAX_NAME - 1);

    window->pose =
        pose;

    window->width =
        width;

    window->height =
        height;

    window->depth =
        0.02f;

    window->opacity =
        1.0f;

    window->visible =
        true;

    window->focused =
        false;

    window->movable =
        true;

    window->resizable =
        true;

    window->workspace =
        os->active_workspace;

    window->application_id =
        app_id;

    return window->id;
}

/* ============================================================
   WINDOW LOOKUP
   ============================================================ */

static SpatialWindow *window_find(
    SpatialOS *os,
    uint32_t id)
{
    for (int i = 0;
         i < os->window_count;
         ++i)
    {
        if (os->windows[i].id ==
            id)
        {
            return
                &os->windows[i];
        }
    }

    return NULL;
}

/* ============================================================
   WINDOW FOCUS
   ============================================================ */

static void window_focus(
    SpatialOS *os,
    uint32_t id)
{
    for (int i = 0;
         i < os->window_count;
         ++i)
    {
        os->windows[i].focused =
            false;
    }

    SpatialWindow *window =
        window_find(
            os,
            id);

    if (window == NULL)
        return;

    window->focused =
        true;

    os->focused_window =
        id;
}

/* ============================================================
   WINDOW MOVE
   ============================================================ */

static void window_move(
    SpatialOS *os,
    uint32_t id,
    Vec3 position)
{
    SpatialWindow *window =
        window_find(
            os,
            id);

    if (window == NULL)
        return;

    if (!window->movable)
        return;

    window->pose.position =
        position;
}

/* ============================================================
   WINDOW RESIZE
   ============================================================ */

static void window_resize(
    SpatialOS *os,
    uint32_t id,
    float width,
    float height)
{
    SpatialWindow *window =
        window_find(
            os,
            id);

    if (window == NULL)
        return;

    if (!window->resizable)
        return;

    window->width =
        fmaxf(
            0.25f,
            width);

    window->height =
        fmaxf(
            0.15f,
            height);
}

/* ============================================================
   NOTIFICATIONS
   ============================================================ */

static void notification_push(
    SpatialOS *os,
    const char *title,
    const char *message,
    NotificationType type)
{
    if (os->notification_count >=
        VR_MAX_NOTIFICATIONS)
    {
        return;
    }

    SpatialNotification *n =
        &os->notifications[
            os->notification_count++];

    memset(
        n,
        0,
        sizeof(*n));

    n->id =
        ++os->next_notification_id;

    strncpy(
        n->title,
        title,
        VR_MAX_NAME - 1);

    strncpy(
        n->message,
        message,
        sizeof(n->message) - 1);

    n->type =
        type;

    n->lifetime =
        5.0f;

    n->age =
        0.0f;

    n->visible =
        true;
}

/* ============================================================
   WORKSPACE
   ============================================================ */

static uint32_t workspace_create(
    SpatialOS *os,
    const char *name,
    Vec3 origin)
{
    if (os->workspace_count >=
        VR_MAX_WORKSPACES)
    {
        return 0;
    }

    SpatialWorkspace *workspace =
        &os->workspaces[
            os->workspace_count++];

    memset(
        workspace,
        0,
        sizeof(*workspace));

    workspace->id =
        os->workspace_count;

    strncpy(
        workspace->name,
        name,
        VR_MAX_NAME - 1);

    workspace->origin =
        origin;

    return workspace->id;
}

/* ============================================================
   SWITCH WORKSPACE
   ============================================================ */

static void workspace_switch(
    SpatialOS *os,
    uint32_t workspace)
{
    if (workspace >=
        (uint32_t)os->workspace_count)
    {
        return;
    }

    os->active_workspace =
        workspace;

    for (int i = 0;
         i < os->window_count;
         ++i)
    {
        os->windows[i].visible =
            os->windows[i].workspace ==
            workspace;
    }

    char message[128];

    snprintf(
        message,
        sizeof(message),
        "Switched to workspace %u",
        workspace);

    notification_push(
        os,
        "Workspace",
        message,
        NOTIFICATION_INFO);
}

/* ============================================================
   SPATIAL DISTANCE
   ============================================================ */

static float distance_to_head(
    SpatialInput *input,
    Pose pose)
{
    return vec_length(
        vec_sub(
            pose.position,
            input->head.position));
}

/* ============================================================
   WINDOW HIT TEST
   ============================================================ */

static bool window_hit_test(
    SpatialOS *os,
    SpatialWindow *window)
{
    float distance =
        distance_to_head(
            &os->input,
            window->pose);

    /*
     * Simplified spatial interaction
     * volume around the user's hands.
     */

    Vec3 hand =
        os->input.right_hand.position;

    float hand_distance =
        vec_length(
            vec_sub(
                hand,
                window->pose.position));

    return
        distance < 8.0f &&
        hand_distance < 0.75f;
}

/* ============================================================
   PINCH INTERACTION
   ============================================================ */

static void process_spatial_input(
    SpatialOS *os)
{
    if (!os->input.right_pinched)
        return;

    /*
     * Find the nearest window
     * under the hand.
     */

    float best_distance =
        9999.0f;

    uint32_t best_window =
        0;

    for (int i = 0;
         i < os->window_count;
         ++i)
    {
        SpatialWindow *window =
            &os->windows[i];

        if (!window->visible)
            continue;

        if (!window_hit_test(
                os,
                window))
        {
            continue;
        }

        float d =
            vec_length(
                vec_sub(
                    os->input.right_hand.position,
                    window->pose.position));

        if (d < best_distance)
        {
            best_distance =
                d;

            best_window =
                window->id;
        }
    }

    if (best_window)
    {
        window_focus(
            os,
            best_window);
    }
}

/* ============================================================
   TERMINAL
   ============================================================ */

static void terminal_print(
    SpatialOS *os,
    const char *text)
{
    if (os->terminal.count <
        TERMINAL_HISTORY)
    {
        strncpy(
            os->terminal.history[
                os->terminal.count++],
            text,
            255);
    }
}

static void terminal_command(
    SpatialOS *os,
    const char *command)
{
    if (strcmp(
            command,
            "help") == 0)
    {
        terminal_print(
            os,
            "Commands: help, apps, files, windows, workspace");
    }

    else if (strcmp(
                 command,
                 "apps") == 0)
    {
        for (int i = 0;
             i < os->app_count;
             ++i)
        {
            char line[256];

            snprintf(
                line,
                sizeof(line),
                "APP: %s [%s]",
                os->apps[i].name,
                os->apps[i].running ?
                    "RUNNING" :
                    "STOPPED");

            terminal_print(
                os,
                line);
        }
    }

    else if (strcmp(
                 command,
                 "files") == 0)
    {
        for (int i = 0;
             i < os->file_count;
             ++i)
        {
            char line[256];

            snprintf(
                line,
                sizeof(line),
                "FILE: %s",
                os->files[i].path);

            terminal_print(
                os,
                line);
        }
    }

    else if (strcmp(
                 command,
                 "windows") == 0)
    {
        for (int i = 0;
             i < os->window_count;
             ++i)
        {
            char line[256];

            snprintf(
                line,
                sizeof(line),
                "WINDOW: %s",
                os->windows[i].title);

            terminal_print(
                os,
                line);
        }
    }

    else if (strcmp(
                 command,
                 "workspace") == 0)
    {
        char line[256];

        snprintf(
            line,
            sizeof(line),
            "Workspace: %u",
            os->active_workspace);

        terminal_print(
            os,
            line);
    }

    else
    {
        terminal_print(
            os,
            "Unknown command");
    }
}

/* ============================================================
   FILE MANAGER
   ============================================================ */

static void launch_file_manager(
    SpatialOS *os)
{
    uint32_t app =
        app_launch(
            os,
            "Files",
            APP_FILE_MANAGER);

    Pose pose =
    {
        vec3(
            -0.8f,
            1.6f,
            -1.5f),

        quat_identity()
    };

    window_create(
        os,
        "Files",
        pose,
        1.2f,
        0.8f,
        app);
}

/* ============================================================
   TERMINAL APP
   ============================================================ */

static void launch_terminal(
    SpatialOS *os)
{
    uint32_t app =
        app_launch(
            os,
            "Terminal",
            APP_TERMINAL);

    Pose pose =
    {
        vec3(
            0.8f,
            1.4f,
            -1.5f),

        quat_identity()
    };

    window_create(
        os,
        "Spatial Terminal",
        pose,
        1.3f,
        0.7f,
        app);
}

/* ============================================================
   MONITOR APP
   ============================================================ */

static void launch_monitor(
    SpatialOS *os)
{
    uint32_t app =
        app_launch(
            os,
            "System Monitor",
            APP_MONITOR);

    Pose pose =
    {
        vec3(
            0.0f,
            2.2f,
            -2.0f),

        quat_identity()
    };

    window_create(
        os,
        "System Monitor",
        pose,
        1.0f,
        0.6f,
        app);
}

/* ============================================================
   SPATIAL DESKTOP
   ============================================================ */

static void create_desktop(
    SpatialOS *os)
{
    uint32_t app =
        app_launch(
            os,
            "Spatial Desktop",
            APP_SYSTEM);

    Pose pose =
    {
        vec3(
            0.0f,
            1.6f,
            -2.0f),

        quat_identity()
    };

    window_create(
        os,
        "Desktop",
        pose,
        1.6f,
        0.9f,
        app);
}

/* ============================================================
   INITIALISE OS
   ============================================================ */

static void spatial_os_init(
    SpatialOS *os)
{
    memset(
        os,
        0,
        sizeof(*os));

    os->next_file_id =
        100;

    os->next_window_id =
        1000;

    os->next_app_id =
        2000;

    os->next_notification_id =
        3000;

    /*
     * Workspaces.
     */

    workspace_create(
        os,
        "Main",
        vec3(0, 0, 0));

    workspace_create(
        os,
        "Work",
        vec3(4, 0, 0));

    workspace_create(
        os,
        "Media",
        vec3(-4, 0, 0));

    workspace_create(
        os,
        "Development",
        vec3(0, 0, 4));

    os->active_workspace =
        0;

    /*
     * Virtual filesystem.
     */

    fs_create_file(
        os,
        "Documents",
        "/Documents",
        FILE_FOLDER,
        0);

    fs_create_file(
        os,
        "Projects",
        "/Projects",
        FILE_FOLDER,
        0);

    fs_create_file(
        os,
        "Pictures",
        "/Pictures",
        FILE_FOLDER,
        0);

    fs_create_file(
        os,
        "music.aac",
        "/Music/music.aac",
        FILE_AUDIO,
        8400000);

    fs_create_file(
        os,
        "main.c",
        "/Projects/main.c",
        FILE_CODE,
        18240);

    fs_create_file(
        os,
        "README.txt",
        "/Projects/README.txt",
        FILE_TEXT,
        4820);

    fs_create_file(
        os,
        "scene.glb",
        "/Projects/scene.glb",
        FILE_3D,
        24000000);

    /*
     * Applications.
     */

    create_desktop(
        os);

    launch_file_manager(
        os);

    launch_terminal(
        os);

    launch_monitor(
        os);

    notification_push(
        os,
        "Spatial OS",
        "System initialised",
        NOTIFICATION_SUCCESS);
}

/* ============================================================
   NOTIFICATION UPDATE
   ============================================================ */

static void update_notifications(
    SpatialOS *os,
    float dt)
{
    for (int i = 0;
         i < os->notification_count;
         ++i)
    {
        SpatialNotification *n =
            &os->notifications[i];

        if (!n->visible)
            continue;

        n->age +=
            dt;

        if (n->age >=
            n->lifetime)
        {
            n->visible =
                false;
        }
    }
}

/* ============================================================
   WINDOW DEPTH SORT
   ============================================================ */

static void update_window_state(
    SpatialOS *os)
{
    for (int i = 0;
         i < os->window_count;
         ++i)
    {
        SpatialWindow *window =
            &os->windows[i];

        if (!window->visible)
            continue;

        /*
         * Automatically hide windows
         * that belong to another workspace.
         */

        window->visible =
            window->workspace ==
            os->active_workspace;
    }
}

/* ============================================================
   SYSTEM TELEMETRY
   ============================================================ */

typedef struct
{
    float frame_time_ms;

    float cpu_usage;

    float gpu_usage;

    float memory_mb;

    float battery;

    float temperature;

} SystemTelemetry;

static SystemTelemetry read_system_telemetry(
    float time)
{
    SystemTelemetry t;

    t.frame_time_ms =
        8.3f +
        1.2f *
        sinf(time);

    t.cpu_usage =
        22.0f +
        8.0f *
        sinf(time * 0.5f);

    t.gpu_usage =
        48.0f +
        12.0f *
        sinf(time * 0.7f);

    t.memory_mb =
        2400.0f +
        80.0f *
        sinf(time * 0.2f);

    t.battery =
        fmaxf(
            0.0f,
            87.0f -
            time * 0.05f);

    t.temperature =
        31.0f +
        2.0f *
        sinf(time);

    return t;
}

/* ============================================================
   SYSTEM MONITOR
   ============================================================ */

static void print_system_monitor(
    SystemTelemetry *t)
{
    printf(
        "\n--- SPATIAL SYSTEM MONITOR ---\n");

    printf(
        "Frame time: %.2f ms\n",
        t->frame_time_ms);

    printf(
        "CPU:        %.1f %%\n",
        t->cpu_usage);

    printf(
        "GPU:        %.1f %%\n",
        t->gpu_usage);

    printf(
        "Memory:     %.0f MB\n",
        t->memory_mb);

    printf(
        "Battery:    %.1f %%\n",
        t->battery);

    printf(
        "Temperature %.1f C\n",
        t->temperature);
}

/* ============================================================
   DESKTOP STATUS
   ============================================================ */

static void print_desktop(
    SpatialOS *os)
{
    printf(
        "\n============================================\n");

    printf(
        "             SPATIAL OS\n");

    printf(
        "============================================\n");

    printf(
        "Workspace: %s\n",
        os->workspaces[
            os->active_workspace].name);

    printf(
        "Files:     %d\n",
        os->file_count);

    printf(
        "Windows:   %d\n",
        os->window_count);

    printf(
        "Apps:      %d\n",
        os->app_count);

    printf(
        "\nWindows:\n");

    for (int i = 0;
         i < os->window_count;
         ++i)
    {
        SpatialWindow *w =
            &os->windows[i];

        if (!w->visible)
            continue;

        printf(
            " [%s] %s\n",
            w->focused ?
                "FOCUS" :
                "     ",
            w->title);

        printf(
            "       position %.2f %.2f %.2f\n",
            w->pose.position.x,
            w->pose.position.y,
            w->pose.position.z);
    }

    printf(
        "============================================\n");
}

/* ============================================================
   SIMULATED HEADSET INPUT
   ============================================================ */

static SpatialInput simulate_input(
    float time)
{
    SpatialInput input;

    memset(
        &input,
        0,
        sizeof(input));

    input.head.position =
        vec3(
            0.0f,
            1.65f,
            0.0f);

    input.head.orientation =
        quat_identity();

    input.right_hand.position =
        vec3(
            0.25f +
                0.15f *
                sinf(time),

            1.25f,

            -0.9f);

    input.right_hand.orientation =
        quat_identity();

    input.left_hand.position =
        vec3(
            -0.25f,
            1.25f,
            -0.9f);

    input.left_hand.orientation =
        quat_identity();

    input.right_pinched =
        false;

    input.left_pinched =
        false;

    return input;
}

/* ============================================================
   OS UPDATE
   ============================================================ */

static void spatial_os_update(
    SpatialOS *os,
    float dt)
{
    os->time +=
        dt;

    os->input =
        simulate_input(
            os->time);

    process_spatial_input(
        os);

    update_notifications(
        os,
        dt);

    update_window_state(
        os);
}

/* ============================================================
   SPATIAL TASKBAR
   ============================================================ */

static void print_taskbar(
    SpatialOS *os)
{
    printf(
        "\n[ SPATIAL TASKBAR ] ");

    printf(
        "Workspace: %s | ",
        os->workspaces[
            os->active_workspace].name);

    printf(
        "Apps: %d | ",
        os->app_count);

    printf(
        "Windows: %d\n",
        os->window_count);
}

/* ============================================================
   TERMINAL OUTPUT
   ============================================================ */

static void print_terminal(
    SpatialOS *os)
{
    printf(
        "\n--- SPATIAL TERMINAL ---\n");

    for (int i = 0;
         i < os->terminal.count;
         ++i)
    {
        printf(
            "%s\n",
            os->terminal.history[i]);
    }
}

/* ============================================================
   DEMO
   ============================================================ */

static void run_demo(
    SpatialOS *os)
{
    printf(
        "\nBooting Spatial OS...\n");

    printf(
        "Loading spatial filesystem...\n");

    printf(
        "Loading applications...\n");

    printf(
        "Loading spatial shell...\n");

    printf(
        "Connecting XR input...\n");

    printf(
        "Spatial environment ready.\n");

    print_desktop(
        os);

    print_taskbar(
        os);

    /*
     * Demonstrate terminal.
     */

    terminal_command(
        os,
        "help");

    terminal_command(
        os,
        "apps");

    terminal_command(
        os,
        "files");

    terminal_command(
        os,
        "workspace");

    print_terminal(
        os);

    /*
     * Demonstrate workspace switching.
     */

    workspace_switch(
        os,
        1);

    print_desktop(
        os);

    /*
     * System telemetry.
     */

    SystemTelemetry telemetry =
        read_system_telemetry(
            os->time);

    print_system_monitor(
        &telemetry);
}

/* ============================================================
   MAIN
   ============================================================ */

int main(void)
{
    printf(
        "\n");
    printf(
        "============================================\n");
    printf(
        "        VR SPATIAL OPERATING SYSTEM\n");
    printf(
        "============================================\n");
    printf(
        "Native C spatial computing shell\n");
    printf(
        "Target concept: Meta Quest\n");
    printf(
        "============================================\n");

    SpatialOS os;

    spatial_os_init(
        &os);

    /*
     * Run the spatial desktop.
     */

    for (int frame = 0;
         frame < 300;
         ++frame)
    {
        spatial_os_update(
            &os,
            1.0f / 90.0f);
    }

    run_demo(
        &os);

    printf(
        "\nSpatial OS shutting down.\n");

    return 0;
}





