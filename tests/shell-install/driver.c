/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Drives Extension Manager's install path without the UI. Test only. */
#include "local/exm-manager.h"
#include "web/exm-repository.h"
#include <glib.h>

static GMainLoop *loop;
static int status = 1;

static void
on_installed (GObject *source, GAsyncResult *res, gpointer data)
{
    GError *error = NULL;
    if (exm_manager_install_finish (EXM_MANAGER (source), res, &error))
    {
        g_print ("DRIVER: installed %s\n", (const char *) data);
        status = 0;
    }
    else
        g_print ("DRIVER: install failed: %s\n", error->message);
    g_clear_error (&error);
    g_main_loop_quit (loop);
}

static void
start (ExmManager *manager, const char *uuid)
{
    g_autofree gchar *url = NULL, *key = NULL;
    int origin = exm_manager_get_repository (manager, &url, &key);
    g_print ("DRIVER: repository origin %d url %s\n", origin, url);
    exm_manager_install_async (manager, uuid, NULL, on_installed, (gpointer) uuid);
}

static void
on_ready (ExmManager *manager, GParamSpec *pspec, const char *uuid)
{
    gboolean ready;
    g_object_get (manager, "is-ready", &ready, NULL);
    if (ready)
        start (manager, uuid);
}

static void
on_lookup (GObject *source, GAsyncResult *res, gpointer data)
{
    GError *error = NULL;
    g_autofree gchar *index_url = NULL;
    JsonNode *entry = exm_repository_lookup_finish (EXM_REPOSITORY (source), res, &index_url, &error);
    if (entry)
    {
        g_print ("DRIVER: lookup ok tier %s\n", json_object_get_string_member (json_node_get_object (entry), "tier"));
        status = 0;
    }
    else
        g_print ("DRIVER: lookup failed: %s\n", error->message);
    g_main_loop_quit (loop);
}

int
main (int argc, char **argv)
{
    if (argc > 2 && g_str_equal (argv[2], "lookup"))
    {
        ExmRepository *repository = exm_repository_get_default ();
        loop = g_main_loop_new (NULL, FALSE);
        exm_repository_set_source (repository, g_getenv ("EXM_EXTENSION_REPOSITORY"), g_getenv ("EXM_EXTENSION_REPOSITORY_KEY"));
        exm_repository_lookup_async (repository, argv[1], NULL, on_lookup, NULL);
        g_main_loop_run (loop);
        return status;
    }

    ExmManager *manager = exm_manager_new ();
    gboolean ready;
    loop = g_main_loop_new (NULL, FALSE);
    g_object_get (manager, "is-ready", &ready, NULL);
    if (ready)
        start (manager, argv[1]);
    else
        g_signal_connect (manager, "notify::is-ready", G_CALLBACK (on_ready), argv[1]);
    g_timeout_add_seconds (60, (GSourceFunc) g_main_loop_quit, loop);
    g_main_loop_run (loop);
    return status;
}
