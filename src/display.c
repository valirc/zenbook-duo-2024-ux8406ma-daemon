/*
 * display.c — backend dispatcher.
 *
 * Backends register themselves at load time via constructor
 * functions (see display_backend.h). At display_init() the
 * dispatcher chooses one based on:
 *   1. cfg->pantalla_backend (if not "auto"; future config key),
 *   2. an environment-driven probe, or
 *   3. a hard fallback to whichever backend probes first.
 *
 * After display_init() the public display_* helpers route every call
 * to the active backend. Backends are free to leave hooks NULL; the
 * dispatcher returns -ENOSYS in that case so the caller can degrade
 * gracefully instead of crashing.
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "display.h"
#include "display_backend.h"

#define MAX_BACKENDS 8

static const struct display_backend *registry[MAX_BACKENDS];
static size_t registry_count = 0;

static const struct display_backend *active = NULL;

void display_backend_register(const struct display_backend *backend)
{
    if (!backend || !backend->name)
    {
        fprintf(stderr, "display: ignoring backend without a name\n");
        return;
    }
    for (size_t i = 0; i < registry_count; ++i)
    {
        if (!strcmp(registry[i]->name, backend->name))
        {
            fprintf(stderr, "display: backend '%s' already registered, ignoring\n",
                    backend->name);
            return;
        }
    }
    if (registry_count >= MAX_BACKENDS)
    {
        fprintf(stderr, "display: backend registry full (max %d)\n", MAX_BACKENDS);
        return;
    }
    registry[registry_count++] = backend;
}

static const struct display_backend *find_backend(const char *name)
{
    if (!name) return NULL;
    for (size_t i = 0; i < registry_count; ++i)
    {
        if (!strcmp(registry[i]->name, name))
            return registry[i];
    }
    return NULL;
}

static const struct display_backend *autodetect_backend(void)
{
    const char *session = getenv("XDG_SESSION_TYPE");
    const char *desktop = getenv("XDG_CURRENT_DESKTOP");

    /* Wayland + GNOME → gdctl */
    if (session && !strcmp(session, "wayland") &&
        desktop && strstr(desktop, "GNOME"))
    {
        const struct display_backend *gd = find_backend("gdctl");
        if (gd && (!gd->probe || gd->probe()))
        {
            return gd;
        }
    }

    /* X11 → xrandr */
    if (session && !strcmp(session, "x11"))
    {
        const struct display_backend *xr = find_backend("xrandr");
        if (xr && (!xr->probe || xr->probe()))
        {
            return xr;
        }
    }

    /* Fallback: first backend whose probe accepts */
    for (size_t i = 0; i < registry_count; ++i)
    {
        if (!registry[i]->probe || registry[i]->probe())
        {
            fprintf(stderr,
                    "display: autodetect fell back to '%s' "
                    "(XDG_SESSION_TYPE='%s', XDG_CURRENT_DESKTOP='%s')\n",
                    registry[i]->name,
                    session ? session : "(unset)",
                    desktop ? desktop : "(unset)");
            return registry[i];
        }
    }
    return NULL;
}

int display_init(void)
{
    if (active)
    {
        return 0; /* idempotent */
    }

    /* TODO: honour cfg->pantalla_backend when the field is added. */
    active = autodetect_backend();
    if (!active)
    {
        fprintf(stderr,
                "display: no backend available (registered=%zu)\n",
                registry_count);
        return -1;
    }
    fprintf(stderr, "display: using backend '%s'\n", active->name);
    return 0;
}

const char *display_active_backend(void)
{
    return active ? active->name : NULL;
}

#define REQUIRE_ACTIVE_OR_ERR()                                       \
    do {                                                              \
        if (!active) {                                                \
            fprintf(stderr,                                           \
                    "display: %s called before display_init()\n",    \
                    __func__);                                        \
            errno = EINVAL;                                           \
            return -1;                                                \
        }                                                             \
    } while (0)

int display_set_output(const char *output, display_output_state state,
                       const char *mode, const char *rate)
{
    REQUIRE_ACTIVE_OR_ERR();
    if (!active->set_output)
    {
        errno = ENOSYS;
        return -1;
    }
    return active->set_output(output, state, mode, rate);
}

int display_is_output_on(const char *output)
{
    if (!active)
    {
        fprintf(stderr,
                "display: %s called before display_init()\n", __func__);
        return 0;
    }
    if (!active->is_output_on)
    {
        return 0;
    }
    return active->is_output_on(output);
}

int display_set_rotation(const char *output, display_rotation r)
{
    REQUIRE_ACTIVE_OR_ERR();
    if (!active->set_rotation)
    {
        errno = ENOSYS;
        return -1;
    }
    return active->set_rotation(output, r);
}

int display_set_wallpapers(const char *bg1, const char *bg2)
{
    REQUIRE_ACTIVE_OR_ERR();
    if (!active->set_wallpapers)
    {
        errno = ENOSYS;
        return -1;
    }
    return active->set_wallpapers(bg1, bg2);
}

int display_set_primary(const char *output)
{
    REQUIRE_ACTIVE_OR_ERR();
    if (!active->set_primary)
    {
        errno = ENOSYS;
        return -1;
    }
    return active->set_primary(output);
}

const char *display_get_primary(void)
{
    if (!active || !active->get_primary)
        return "eDP-1";
    return active->get_primary();
}

int display_is_output_connected(const char *output)
{
    if (!active || !active->is_output_connected)
        return 0;
    return active->is_output_connected(output);
}
