/*
 * exm-manager.c
 *
 * Copyright 2022-2025 Matthew Jakeman <mjakeman26@outlook.co.nz>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "exm-manager.h"

#include "exm-config.h"
#include "exm-package.h"
#include "shell-dbus-interface.h"

#include "../exm-types.h"
#include "../exm-enums.h"
#include "../web/exm-repository.h"

#include <glib/gi18n.h>

struct _ExmManager
{
    GObject parent_instance;

    ShellExtensions *proxy;
    GListModel *ext_model;
    GSettings *settings;

    const gchar *shell_version;
    gboolean extensions_enabled;
    gboolean is_supported;

    guint update_callback_id;
};

G_DEFINE_FINAL_TYPE (ExmManager, exm_manager, G_TYPE_OBJECT)

enum {
    PROP_0,
    PROP_EXTENSIONS,
    PROP_EXTENSIONS_ENABLED,
    PROP_SHELL_VERSION,
    PROP_IS_SUPPORTED,
    N_PROPS
};

static GParamSpec *properties [N_PROPS];

enum {
    SIGNAL_0,
    SIGNAL_UPDATES_AVAILABLE,
    SIGNAL_ERROR_OCCURRED,
    SIGNAL_INSTALL_STATUS,
    SIGNAL_LOGOUT_REQUIRED,
    N_SIGNALS
};

static guint signals [N_SIGNALS];

ExmManager *
exm_manager_new (void)
{
    return g_object_new (EXM_TYPE_MANAGER, NULL);
}

static void
exm_manager_get_property (GObject    *object,
                          guint       prop_id,
                          GValue     *value,
                          GParamSpec *pspec)
{
    ExmManager *self = EXM_MANAGER (object);

    switch (prop_id)
    {
    case PROP_EXTENSIONS:
        g_value_set_object (value, self->ext_model);
        break;
    case PROP_SHELL_VERSION:
        g_value_set_string (value, self->shell_version);
        break;
    case PROP_EXTENSIONS_ENABLED:
        g_value_set_boolean (value, self->extensions_enabled);
        break;
    case PROP_IS_SUPPORTED:
        g_value_set_boolean (value, self->is_supported);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
exm_manager_set_property (GObject      *object,
                          guint         prop_id,
                          const GValue *value,
                          GParamSpec   *pspec)
{
    ExmManager *self = EXM_MANAGER (object);

    switch (prop_id)
    {
    case PROP_SHELL_VERSION:
        self->shell_version = g_value_dup_string (value);
        break;
    case PROP_EXTENSIONS_ENABLED:
        self->extensions_enabled = g_value_get_boolean (value);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

typedef struct
{
    ExmExtension *extension;
    ExmManager *manager;
} ExmCallbackData;

static ExmCallbackData *
create_callback_data (ExmManager *manager,
                      ExmExtension *extension)
{
    ExmCallbackData *data;
    data = g_slice_new0 (ExmCallbackData);

    data->manager = g_object_ref (manager);
    data->extension = g_object_ref (extension);

	return data;
}

static void
free_callback_data (ExmCallbackData *data)
{
	if (!data)
		return;

    g_clear_object (&data->manager);
    g_clear_object (&data->extension);

    g_slice_free (ExmCallbackData, data);
}

#define notify_error(self_, f_, ...); \
    {\
        char *error_text;\
        error_text = g_strdup_printf (f_, __VA_ARGS__);\
        g_critical ("%s", error_text);\
        g_signal_emit (G_OBJECT (self_), signals [SIGNAL_ERROR_OCCURRED], 0, error_text);\
        g_free (error_text);\
    }\

static void
enable_extension_done (ShellExtensions *proxy,
                       GAsyncResult    *res,
                       ExmCallbackData *data)
{
    GError *error = NULL;
    gboolean success;
    shell_extensions_call_enable_extension_finish (proxy, &success, res, &error);

    if (!success)
    {
        gchar *uuid;
        g_object_get (data->extension, "uuid", &uuid, NULL);
        if (error)
        {
            notify_error (data->manager, "Could not enable extension '%s': %s\n", uuid, error->message);
        }
        else
        {
            notify_error (data->manager, "Could not enable extension '%s': unknown failure", uuid);
        }
    }

    free_callback_data (data);
}

void
exm_manager_enable_extension (ExmManager   *self,
                              ExmExtension *extension)
{
    gchar *uuid;
    g_object_get (extension, "uuid", &uuid, NULL);

    shell_extensions_call_enable_extension (self->proxy,
                                            uuid,
                                            NULL,
                                            (GAsyncReadyCallback) enable_extension_done,
                                            create_callback_data (self, extension));
}

static void
disable_extension_done (ShellExtensions *proxy,
                        GAsyncResult    *res,
                        ExmCallbackData *data)
{
    GError *error = NULL;
    gboolean success;
    shell_extensions_call_disable_extension_finish (proxy, &success, res, &error);

    if (!success)
    {
        gchar *uuid;
        g_object_get (data->extension, "uuid", &uuid, NULL);
        if (error)
        {
            notify_error (data->manager, "Could not disable extension '%s': %s\n", uuid, error->message);
        }
        else
        {
            notify_error (data->manager, "Could not disable extension '%s': unknown failure", uuid);
        }
    }

    free_callback_data (data);
}

void
exm_manager_disable_extension (ExmManager   *self,
                               ExmExtension *extension)
{
    gchar *uuid;
    g_object_get (extension, "uuid", &uuid, NULL);

    shell_extensions_call_disable_extension (self->proxy,
                                             uuid,
                                             NULL,
                                             (GAsyncReadyCallback) disable_extension_done,
                                             create_callback_data (self, extension));
}

static void
remove_extension_done (ShellExtensions *proxy,
                       GAsyncResult    *res,
                       ExmCallbackData *data)
{
    GError *error = NULL;
    gboolean success;
    shell_extensions_call_uninstall_extension_finish (proxy, &success, res, &error);

    if (!success)
    {
        gchar *uuid;
        g_object_get (data->extension, "uuid", &uuid, NULL);
        if (error)
        {
            notify_error (data->manager, "Could not remove extension '%s': %s\n", uuid, error->message);
        }
        else
        {
            notify_error (data->manager, "Could not remove extension '%s': unknown failure", uuid);
        }
    }

    free_callback_data (data);
}

void
exm_manager_remove_extension (ExmManager   *self,
                              ExmExtension *extension)
{
    gchar *uuid;
    g_object_get (extension, "uuid", &uuid, NULL);

    shell_extensions_call_uninstall_extension (self->proxy,
                                               uuid,
                                               NULL,
                                               (GAsyncReadyCallback) remove_extension_done,
                                               create_callback_data (self, extension));
}

static void
open_prefs_done (ShellExtensions *proxy,
                 GAsyncResult    *res,
                 ExmCallbackData *data)
{
    GError *error = NULL;
    shell_extensions_call_launch_extension_prefs_finish (proxy, res, &error);

    // TODO: Don't enable until we can export the window handle over dbus
    /*if (error)
    {
        gchar *uuid;
        g_object_get (data->extension, "uuid", &uuid, NULL);
        notify_error (data->manager, "Could not open extension '%s' preferences: %s\n", uuid, error->message);
    }*/

    free_callback_data (data);
}

void
exm_manager_open_prefs (ExmManager   *self,
                        ExmExtension *extension)
{
    gchar *uuid;
    g_object_get (extension, "uuid", &uuid, NULL);

    shell_extensions_call_launch_extension_prefs (self->proxy,
                                                  uuid,
                                                  NULL,
                                                  (GAsyncReadyCallback) open_prefs_done,
                                                  create_callback_data (self, extension));
}

static gpointer
list_model_get_by_uuid (GListModel  *model,
                        const gchar *uuid)
{
    int n_items = g_list_model_get_n_items (model);
    for (int i = 0; i < n_items; i++)
    {
        ExmExtension *ext = g_list_model_get_item (model, i);

        gchar *cmp_uuid;
        g_object_get (ext, "uuid", &cmp_uuid, NULL);

        if (strcmp (uuid, cmp_uuid) == 0)
            return ext;
    }

    return NULL;
}

static gboolean
list_model_contains (GListModel  *model,
                     const gchar *uuid)
{
    return list_model_get_by_uuid (model, uuid) != NULL;
}

ExmExtension *
exm_manager_get_by_uuid (ExmManager  *self,
                         const gchar *uuid)
{
    ExmExtension *result = NULL;

    if ((result = list_model_get_by_uuid (self->ext_model, uuid)) != NULL)
        return result;

    return NULL;
}

gboolean
exm_manager_is_installed_uuid (ExmManager  *self,
                               const gchar *uuid)
{
    if (list_model_contains (self->ext_model, uuid))
        return TRUE;

    return FALSE;
}

static void
on_remote_installed (ShellExtensions *proxy,
                     GAsyncResult    *res,
                     GTask           *task)
{
    g_autofree gchar *out_result = NULL;
    GError *error = NULL;

    if (!shell_extensions_call_install_remote_extension_finish (proxy, &out_result, res, &error))
        g_task_return_error (task, error);
    else
        g_task_return_boolean (task, g_strcmp0 (out_result, "cancelled") != 0);

    g_object_unref (task);
}

static void
install_remote (ExmManager *self,
                GTask      *task)
{
    shell_extensions_call_install_remote_extension (self->proxy,
                                                    g_task_get_task_data (task),
                                                    g_task_get_cancellable (task),
                                                    (GAsyncReadyCallback) on_remote_installed,
                                                    task);
}

static void
on_package_installed (GObject      *source G_GNUC_UNUSED,
                      GAsyncResult *res,
                      GTask        *task)
{
    ExmManager *self = g_task_get_source_object (task);
    const gchar *uuid = g_task_get_task_data (task);
    g_autoptr (GSettings) shell_settings = NULL;
    GError *error = NULL;
    gboolean staged, schema_copy;

    if (!exm_package_install_finish (res, &staged, &error))
    {
        notify_error (self, "Could not install extension '%s': %s\n", uuid, error->message);
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }

    // An update replaces the installed version at login and stays enabled
    if (!staged)
    {
        shell_settings = exm_package_get_shell_settings (&schema_copy, &error);

        if (shell_settings == NULL ||
            !exm_package_enable (shell_settings, schema_copy, uuid, &error))
        {
            notify_error (self, "Extension '%s' is installed, but could not be enabled: %s\n",
                          uuid, error->message);
            g_clear_error (&error);
        }
    }

    g_signal_emit (self, signals [SIGNAL_LOGOUT_REQUIRED], 0, uuid);

    g_task_return_boolean (task, TRUE);
    g_object_unref (task);
}

static void
on_package_found (ExmRepository *repository,
                  GAsyncResult  *res,
                  GTask         *task)
{
    ExmManager *self = g_task_get_source_object (task);
    const gchar *uuid = g_task_get_task_data (task);
    g_autoptr (JsonNode) entry = NULL;
    g_autofree gchar *index_url = NULL;
    g_autofree gchar *url = NULL;
    g_autofree gchar *sha256 = NULL;
    g_autofree gchar *shell_dir = NULL;
    JsonObject *extension;
    const gchar *tier;
    GError *error = NULL;
    gint64 size;

    entry = exm_repository_lookup_finish (repository, res, &index_url, &error);
    if (entry == NULL)
        goto failed;

    extension = json_node_get_object (entry);
    tier = json_object_get_string_member_with_default (extension, "tier", NULL);

    // Mirrored releases are the ones on extensions.gnome.org, which the Shell
    // installs, loads at once and keeps updated
    if (g_strcmp0 (tier, "ego-mirror") == 0)
    {
        install_remote (self, task);
        return;
    }

    if (!exm_repository_get_download (extension, index_url, self->shell_version,
                                      &url, &sha256, &size, &error))
        goto failed;

    shell_dir = exm_package_get_shell_dir ();
    exm_package_install_async (uuid, url, sha256, size, self->shell_version, shell_dir,
                               g_task_get_cancellable (task),
                               (GAsyncReadyCallback) on_package_installed, task);
    return;

failed:
    notify_error (self, "Could not install extension '%s': %s\n", uuid, error->message);
    g_task_return_error (task, error);
    g_object_unref (task);
}

/**
 * exm_manager_install_async:
 * @uuid: the extension to install
 *
 * GNOME Shell installs from its own repository, or from
 * extensions.gnome.org. With a repository of Extension Manager's own,
 * releases it mirrors from extensions.gnome.org are still installed by the
 * Shell; the others are installed by Extension Manager and start after the
 * next login, which "logout-required" announces.
 */
void
exm_manager_install_async (ExmManager          *self,
                           const gchar         *uuid,
                           GCancellable        *cancellable,
                           GAsyncReadyCallback  callback,
                           gpointer             user_data)
{
    GTask *task = g_task_new (self, cancellable, callback, user_data);
    ExmRepository *repository = exm_repository_get_default ();

    g_task_set_task_data (task, g_strdup (uuid), g_free);

    if (exm_manager_get_repository (self, NULL, NULL) != EXM_REPOSITORY_ORIGIN_SHELL &&
        exm_repository_is_enabled (repository))
    {
        exm_repository_lookup_async (repository, uuid, cancellable,
                                     (GAsyncReadyCallback) on_package_found, task);
        return;
    }

    install_remote (self, task);
}

/**
 * exm_manager_install_finish:
 *
 * Returns: %TRUE if installed, %FALSE if it failed or the user cancelled
 */
gboolean
exm_manager_install_finish (ExmManager    *self,
                            GAsyncResult  *result,
                            GError       **error)
{
    g_return_val_if_fail (g_task_is_valid (result, self), FALSE);

    return g_task_propagate_boolean (G_TASK (result), error);
}

static int
list_model_get_number_of_updates (GListModel *model)
{
    int num_updates = 0;

    int n_items = g_list_model_get_n_items (model);
    for (int i = 0; i < n_items; i++)
    {
        ExmExtension *ext = g_list_model_get_item (model, i);

        gboolean has_update;
        g_object_get (ext, "has-update", &has_update, NULL);

        if (has_update)
            num_updates++;
    }

    return num_updates;
}

static guint
notify_extension_updates (ExmManager *self)
{
    // Checks for updates in the background and if found then
    // emits the 'updates-available' signal with the number of updates
    int n_updates = 0;

    n_updates += list_model_get_number_of_updates (self->ext_model);

    g_info ("There are %d new updates available.", n_updates);

    if (n_updates > 0)
        g_signal_emit (G_OBJECT (self), signals [SIGNAL_UPDATES_AVAILABLE], 0, n_updates);

    self->update_callback_id = 0;

    return G_SOURCE_REMOVE;
}

static void
queue_notify_extension_updates (ExmManager *self)
{
    // See: notify_extension_updates

    if (self->update_callback_id != 0)
        return;

    self->update_callback_id = g_timeout_add (0, G_SOURCE_FUNC (notify_extension_updates), self);
}

static void
check_for_updates_done (ShellExtensions *proxy,
                        GAsyncResult    *res,
                        ExmManager      *manager)
{
    GError *error = NULL;
    shell_extensions_call_check_for_updates_finish (proxy, res, &error);

    if (error)
    {
        notify_error (manager, "Could not check for updates: %s\n", error->message);
    }

    // Notify the user if updates are detected
    queue_notify_extension_updates (manager);
}

void
exm_manager_check_for_updates (ExmManager *self)
{
    shell_extensions_call_check_for_updates (self->proxy,
                                             NULL,
                                             (GAsyncReadyCallback) check_for_updates_done,
                                             self);
}

static void
exm_manager_class_init (ExmManagerClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);

    object_class->get_property = exm_manager_get_property;
    object_class->set_property = exm_manager_set_property;

    properties [PROP_EXTENSIONS]
        = g_param_spec_object ("extensions",
                               "Extensions List Model",
                               "Extensions List Model",
                               G_TYPE_LIST_MODEL,
                               G_PARAM_READABLE);

    properties [PROP_SHELL_VERSION]
        = g_param_spec_string ("shell-version",
                               "Shell Version",
                               "Shell Version",
                               NULL,
                               G_PARAM_READWRITE);

    properties [PROP_EXTENSIONS_ENABLED]
        = g_param_spec_boolean ("extensions-enabled",
                                "Extensions Enabled",
                                "Extensions Enabled",
                                FALSE,
                                G_PARAM_READWRITE);

    properties [PROP_IS_SUPPORTED]
        = g_param_spec_boolean ("is-supported",
                                "Is Supported",
                                "Whether GNOME Shell is running and owns the extensions D-Bus name",
                                FALSE,
                                G_PARAM_READABLE);

    g_object_class_install_properties (object_class, N_PROPS, properties);

    signals [SIGNAL_UPDATES_AVAILABLE]
        = g_signal_new ("updates-available",
                        G_TYPE_FROM_CLASS (object_class),
                        G_SIGNAL_RUN_LAST|G_SIGNAL_NO_RECURSE|G_SIGNAL_NO_HOOKS,
                        0, NULL, NULL, NULL,
                        G_TYPE_NONE, 1,
                        G_TYPE_INT);

    signals [SIGNAL_ERROR_OCCURRED]
        = g_signal_new ("error-occurred",
                        G_TYPE_FROM_CLASS (object_class),
                        G_SIGNAL_RUN_LAST|G_SIGNAL_NO_RECURSE|G_SIGNAL_NO_HOOKS,
                        0, NULL, NULL, NULL,
                        G_TYPE_NONE, 1,
                        G_TYPE_STRING);

    signals [SIGNAL_INSTALL_STATUS]
        = g_signal_new ("install-status",
                        G_TYPE_FROM_CLASS (object_class),
                        G_SIGNAL_RUN_LAST|G_SIGNAL_NO_RECURSE|G_SIGNAL_NO_HOOKS,
                        0, NULL, NULL, NULL,
                        G_TYPE_NONE, 1,
                        EXM_TYPE_INSTALL_BUTTON_STATE);

    /* An extension was installed or updated that GNOME Shell only starts
     * after the user logs out and back in */
    signals [SIGNAL_LOGOUT_REQUIRED]
        = g_signal_new ("logout-required",
                        G_TYPE_FROM_CLASS (object_class),
                        G_SIGNAL_RUN_LAST|G_SIGNAL_NO_RECURSE|G_SIGNAL_NO_HOOKS,
                        0, NULL, NULL, NULL,
                        G_TYPE_NONE, 1,
                        G_TYPE_STRING);
}

static void
parse_single_extension (ExmExtension **extension,
                        const gchar   *extension_uuid,
                        GVariantIter  *variant_iter,
                        gboolean      *is_uninstall_operation)
{
    gchar *prop_name;
    GVariant *prop_value;

    // Well-Defined Properties
    gchar *uuid = NULL;
    gchar *name = NULL;
    gchar *description = NULL;
    ExmExtensionState state = EXM_EXTENSION_STATE_INITIALIZED;
    gboolean enabled = FALSE;
    gchar *path = NULL;
    gchar *url = NULL;
    gchar *version = NULL;
    gchar *version_name = NULL;
    gchar *error = NULL;
    gboolean has_prefs = FALSE;
    gboolean has_update = FALSE;
    gboolean can_change = TRUE;
    gboolean is_user = FALSE;
    ExmExtensionType type = EXM_EXTENSION_TYPE_SYSTEM;
    GPtrArray *session_modes = g_ptr_array_new_with_free_func (g_free);
    GHashTable *donations = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, (GDestroyNotify) g_ptr_array_unref);

    if (extension && *extension)
    {
        const gchar *uuid_cmp;
        g_object_get (*extension, "uuid", &uuid_cmp, NULL);
        g_assert (strcmp (extension_uuid, uuid_cmp) == 0);
    }
    else
    {
        *extension = exm_extension_new (extension_uuid);
    }

    g_debug ("Found extension '%s' with properties:\n", uuid);

    while (g_variant_iter_loop (variant_iter, "{sv}", &prop_name, &prop_value))
    {
        g_debug (" - Property: %s=%s\n", prop_name, g_variant_print(prop_value, 0));

        // Compare with DBus property names
        if (g_strcmp0 (prop_name, "uuid") == 0)
        {
            g_variant_get (prop_value, "s", &uuid);

            // Assert that this is the same as the extension uuid
            g_assert (g_strcmp0 (extension_uuid, uuid) == 0);
        }
        else if (g_strcmp0 (prop_name, "name") == 0)
        {
            g_variant_get (prop_value, "s", &name);
        }
        else if (g_strcmp0 (prop_name, "description") == 0)
        {
            g_variant_get (prop_value, "s", &description);
        }
        else if (g_strcmp0 (prop_name, "state") == 0)
        {
            gdouble val;
            g_variant_get (prop_value, "d", &val);
            state = (ExmExtensionState)val;
        }
        else if (g_strcmp0 (prop_name, "enabled") == 0)
        {
            g_variant_get (prop_value, "b", &enabled);
        }
        else if (g_strcmp0 (prop_name, "path") == 0)
        {
            g_variant_get (prop_value, "s", &path);
        }
        else if (g_strcmp0 (prop_name, "url") == 0)
        {
            g_variant_get (prop_value, "s", &url);
        }
        else if (g_strcmp0 (prop_name, "version") == 0)
        {
            gdouble val;
            g_variant_get (prop_value, "d", &val);
            version = g_strdup_printf ("%d", (gint)val);
        }
        else if (g_strcmp0 (prop_name, "version-name") == 0)
        {
            g_variant_get (prop_value, "s", &version_name);
        }
        else if (g_strcmp0 (prop_name, "error") == 0)
        {
            g_variant_get (prop_value, "s", &error);
        }
        else if (g_strcmp0 (prop_name, "hasPrefs") == 0)
        {
            g_variant_get (prop_value, "b", &has_prefs);
        }
        else if (g_strcmp0 (prop_name, "hasUpdate") == 0)
        {
            g_variant_get (prop_value, "b", &has_update);
        }
        else if (g_strcmp0 (prop_name, "canChange") == 0)
        {
            g_variant_get (prop_value, "b", &can_change);
        }
        else if (g_strcmp0 (prop_name, "type") == 0)
        {
            gdouble val;
            g_variant_get (prop_value, "d", &val);
            type = (ExmExtensionType)val;
        }
        else if (g_strcmp0 (prop_name, "sessionModes") == 0)
        {
            GVariantIter iter;
            GVariant *variant;
            gchar *mode;
            g_variant_iter_init (&iter, prop_value);
            while (g_variant_iter_loop (&iter, "v", &variant))
            {
                mode = g_variant_dup_string (variant, NULL);
                g_ptr_array_add (session_modes, g_strdup (mode));
            }
        }
        else if (g_strcmp0 (prop_name, "donations") == 0)
        {
            GVariantIter iter;
            GVariant *donation_entry;
            gchar *key;
            g_variant_iter_init (&iter, prop_value);

            while (g_variant_iter_loop (&iter, "{sv}", &key, &donation_entry))
            {
                GPtrArray *usernames = g_ptr_array_new_with_free_func (g_free);

                if (g_variant_is_of_type (donation_entry, G_VARIANT_TYPE_STRING))
                {
                    g_ptr_array_add (usernames, g_variant_dup_string (donation_entry, NULL));
                }
                else if (g_variant_is_container (donation_entry))
                {
                    for (gsize j = 0; j < g_variant_n_children (donation_entry); j++)
                    {
                        GVariant *child_v = g_variant_get_child_value (donation_entry, j);
                        GVariant *child_s = g_variant_get_variant (child_v);
                        g_ptr_array_add (usernames, g_variant_dup_string (child_s, NULL));
                        g_variant_unref (child_s);
                        g_variant_unref (child_v);
                    }
                }

                g_hash_table_insert (donations, g_strdup (key), usernames);
            }
        }
    }

    is_user = (type == EXM_EXTENSION_TYPE_PER_USER);
    *is_uninstall_operation = (state == EXM_EXTENSION_STATE_UNINSTALLED);

    g_object_set (*extension,
                  "name", name,
                  "description", description,
                  "state", state,
                  "enabled", enabled,
                  "path", path,
                  "url", url,
                  "version", version,
                  "version-name", version_name,
                  "error", error,
                  "has-prefs", has_prefs,
                  "has-update", has_update,
                  "can-change", can_change,
                  "is-user", is_user,
                  "session-modes", session_modes,
                  "donations", donations,
                  NULL);

    g_free (uuid);
    g_free (name);
    g_free (description);
    g_free (path);
    g_free (url);
    g_free (version);
    g_free (version_name);
    g_free (error);
}

static void
parse_extension_list (GVariant    *exlist,
                      GListModel **ext_model)
{
    GListStore *ext_store;

    /* format: a{sa{sv}}
     * array of interfaces, where each interface is an array of properties
     * each interface corresponds to one extension
     * this is terrible >:(
     * see also: https://stackoverflow.com/questions/54131543/how-can-i-get-the-g-dbus-connection-signal-subscribe-function-to-tell-me-about-p
     */
    GVariantIter *iter, *iter2;
    gchar *exname;

    ext_store = g_list_store_new (EXM_TYPE_EXTENSION);

    g_variant_get (exlist, "a{sa{sv}}", &iter);
    while (g_variant_iter_loop (iter, "{sa{sv}}", &exname, &iter2)) {
        gboolean is_uninstall_operation;
        ExmExtension *extension = NULL;

        parse_single_extension (&extension, exname, iter2, &is_uninstall_operation);

        if (is_uninstall_operation)
        {
            g_error ("Invalid extension: '%s' is being uninstalled.", exname);
            continue;
        }

        g_list_store_append (ext_store, extension);
    }
    g_variant_iter_free (iter);

    *ext_model = G_LIST_MODEL (ext_store);
}

static void
update_extension_list (ExmManager *self)
{
    GError *error = NULL;

    GVariant* exlist;
    shell_extensions_call_list_extensions_sync (self->proxy, &exlist, NULL, &error);

    if (error != NULL)
    {
        notify_error (self, "Could not list extensions: %s\n", error->message);
        return;
    }

    // Unref object if exists
    g_clear_object (&self->ext_model);

    parse_extension_list (exlist, &self->ext_model);

    g_object_notify_by_pspec (G_OBJECT (self), properties [PROP_EXTENSIONS]);

    queue_notify_extension_updates (self);
}

static void
on_state_changed (ShellExtensions *object G_GNUC_UNUSED,
                  const gchar     *arg_uuid,
                  GVariant        *arg_state,
                  ExmManager      *self)
{
    ExmExtension *extension;
    gboolean is_new;
    gboolean is_uninstall_operation;
    GListStore *list_store;

    g_debug ("State Changed for extension '%s'\n", arg_uuid);

    // Parse the new extension state and update only that element in
    // the list model. This will automatically update any bound
    // listboxes or listviews.

    // This is NULL if it does not exist
    extension = exm_manager_get_by_uuid (self, arg_uuid);
    is_new = (extension == NULL);

    GVariantIter *iter;
    g_variant_get (arg_state, "a{sv}", &iter);
    parse_single_extension (&extension, arg_uuid, iter, &is_uninstall_operation);
    g_variant_iter_free (iter);

    list_store = G_LIST_STORE (self->ext_model);

    if (is_new)
    {
        g_list_store_insert_sorted (list_store, extension, (GCompareDataFunc)compare_extension, NULL);
        return;
    }

    guint position;

    if (is_uninstall_operation)
    {
        if (g_list_store_find_with_equal_func (list_store, extension, (GEqualFunc)is_extension_equal, &position))
            g_list_store_remove (list_store, position);

        return;
    }

    // Emit items-changed signal to re-sort extension list
    if (g_list_store_find_with_equal_func (list_store, extension, (GEqualFunc)is_extension_equal, &position))
        g_list_model_items_changed (G_LIST_MODEL (list_store), position, 0, 0);

    // If the extension that has changed has an update, then
    // one or more extensions have updates available. Lazily
    // check the exact number and emit the 'updates-available'
    // signal.
    gboolean has_update;
    g_object_get (extension, "has-update", &has_update, NULL);

    if (has_update)
        queue_notify_extension_updates (self);
}

/*
 * GNOME Shell installs and updates from the catalogue in its
 * extension-repository settings, exposes them as the read-only
 * ExtensionRepository and ExtensionRepositoryKey properties and changes them
 * with SetExtensionRepository. Browse the same catalogue, so what is shown is
 * what the Shell installs. A Shell without these properties installs from
 * extensions.gnome.org; the repository then comes from the application's
 * own settings (or EXM_EXTENSION_REPOSITORY and EXM_EXTENSION_REPOSITORY_KEY,
 * for testing), and Extension Manager installs what the Shell cannot.
 */
#define REPOSITORY_PROPERTY "ExtensionRepository"
#define REPOSITORY_KEY_PROPERTY "ExtensionRepositoryKey"

static gchar *
get_string_property (ExmManager  *self,
                     const gchar *name)
{
    g_autoptr (GVariant) value = NULL;

    if (self->proxy == NULL)
        return NULL;

    value = g_dbus_proxy_get_cached_property (G_DBUS_PROXY (self->proxy), name);

    if (value == NULL || !g_variant_is_of_type (value, G_VARIANT_TYPE_STRING))
        return NULL;

    return g_variant_dup_string (value, NULL);
}

/**
 * exm_manager_get_repository:
 * @url: (out) (optional): the repository in use, empty for
 *   extensions.gnome.org
 * @public_key: (out) (optional): the key that signs its index
 *
 * Returns: where the repository comes from
 */
ExmRepositoryOrigin
exm_manager_get_repository (ExmManager  *self,
                            gchar      **url,
                            gchar      **public_key)
{
    g_autofree gchar *shell_url = get_string_property (self, REPOSITORY_PROPERTY);
    g_autofree gchar *shell_key = get_string_property (self, REPOSITORY_KEY_PROPERTY);
    g_autofree gchar *chosen_url = NULL;
    g_autofree gchar *chosen_key = NULL;
    ExmRepositoryOrigin origin;

    origin = exm_repository_choose_source (shell_url, shell_key, self->settings,
                                           &chosen_url, &chosen_key);

    if (url)
        *url = g_steal_pointer (&chosen_url);
    if (public_key)
        *public_key = g_steal_pointer (&chosen_key);

    return origin;
}

static void
update_repository (ExmManager *self)
{
    g_autofree gchar *url = NULL;
    g_autofree gchar *key = NULL;

    exm_manager_get_repository (self, &url, &key);
    exm_repository_set_source (exm_repository_get_default (), url, key);
}

static void
on_settings_changed (ExmManager  *self,
                     const gchar *key)
{
    if (g_str_has_prefix (key, "extension-repository"))
        update_repository (self);
}

static void
on_repository_set (GDBusConnection *connection,
                   GAsyncResult    *res,
                   GTask           *task)
{
    g_autoptr (GVariant) reply = NULL;
    GError *error = NULL;

    reply = g_dbus_connection_call_finish (connection, res, &error);

    if (reply != NULL)
    {
        g_task_return_boolean (task, TRUE);
    }
    else if (g_error_matches (error, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_METHOD))
    {
        g_clear_error (&error);
        g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                                 _("This version of GNOME Shell cannot use an extension repository"));
    }
    else
    {
        // The Shell explains what it refused, e.g. a URL that is not HTTPS
        g_dbus_error_strip_remote_error (error);
        g_task_return_error (task, error);
    }

    g_object_unref (task);
}

/**
 * exm_manager_set_repository_async:
 * @url: the index URL, or "" for extensions.gnome.org
 * @public_key: the key that signs the index, or ""
 *
 * Sets the repository the Shell installs and updates from when it has one,
 * and the application's own otherwise. Both are checked and changed
 * together.
 */
void
exm_manager_set_repository_async (ExmManager          *self,
                                  const gchar         *url,
                                  const gchar         *public_key,
                                  GCancellable        *cancellable,
                                  GAsyncReadyCallback  callback,
                                  gpointer             user_data)
{
    GTask *task = g_task_new (self, cancellable, callback, user_data);
    GDBusProxy *proxy = G_DBUS_PROXY (self->proxy);
    GError *error = NULL;

    if (exm_manager_get_repository (self, NULL, NULL) != EXM_REPOSITORY_ORIGIN_SHELL)
    {
        if (exm_repository_store_source (self->settings, url, public_key, &error))
            g_task_return_boolean (task, TRUE);
        else
            g_task_return_error (task, error);

        g_object_unref (task);
        return;
    }

    g_dbus_connection_call (g_dbus_proxy_get_connection (proxy),
                            g_dbus_proxy_get_name (proxy),
                            g_dbus_proxy_get_object_path (proxy),
                            g_dbus_proxy_get_interface_name (proxy),
                            "SetExtensionRepository",
                            g_variant_new ("(ss)", url ? url : "", public_key ? public_key : ""),
                            G_VARIANT_TYPE_UNIT,
                            G_DBUS_CALL_FLAGS_NONE,
                            -1,
                            cancellable,
                            (GAsyncReadyCallback) on_repository_set,
                            task);
}

gboolean
exm_manager_set_repository_finish (ExmManager    *self,
                                   GAsyncResult  *result,
                                   GError       **error)
{
    g_return_val_if_fail (g_task_is_valid (result, self), FALSE);

    return g_task_propagate_boolean (G_TASK (result), error);
}

/**
 * exm_manager_reset_repository:
 *
 * Goes back to the repository the application comes with. Only for its own
 * settings; a Shell with a repository keeps its own.
 */
void
exm_manager_reset_repository (ExmManager *self)
{
    g_autoptr (GSettings) batch = NULL;

    g_return_if_fail (exm_manager_get_repository (self, NULL, NULL) != EXM_REPOSITORY_ORIGIN_SHELL);

    batch = g_settings_new (APP_ID);
    g_settings_delay (batch);
    g_settings_reset (batch, "extension-repository");
    g_settings_reset (batch, "extension-repository-key");
    g_settings_apply (batch);
}

static void
update_is_supported (ExmManager *self)
{
    gchar *owner;
    gboolean supported;

    owner = g_dbus_proxy_get_name_owner (G_DBUS_PROXY (self->proxy));
    supported = (owner != NULL);
    g_free (owner);

    update_repository (self);

    if (supported == self->is_supported)
        return;

    self->is_supported = supported;
    g_object_notify_by_pspec (G_OBJECT (self), properties [PROP_IS_SUPPORTED]);
}

static void
exm_manager_init (ExmManager *self)
{
    GError *error = NULL;

    self->settings = g_settings_new (APP_ID);
    g_signal_connect_swapped (self->settings, "changed",
                              G_CALLBACK (on_settings_changed), self);

    self->proxy = shell_extensions_proxy_new_for_bus_sync (G_BUS_TYPE_SESSION,
                                                           G_DBUS_PROXY_FLAGS_NONE,
                                                           "org.gnome.Shell.Extensions",
                                                           "/org/gnome/Shell/Extensions",
                                                           NULL, &error);

    if (error != NULL)
    {
        notify_error (self, "Could not create proxy: %s\n", error->message);
        update_repository (self);
        return;
    }

    self->update_callback_id = 0;

    g_signal_connect_swapped (self->proxy, "notify::g-name-owner",
                              G_CALLBACK (update_is_supported), self);
    g_signal_connect_swapped (self->proxy, "g-properties-changed",
                              G_CALLBACK (update_repository), self);
    update_is_supported (self);

    g_object_bind_property (self->proxy, "shell-version",
                            self, "shell-version",
                            G_BINDING_SYNC_CREATE);

    g_object_bind_property (self->proxy, "user-extensions-enabled",
                            self, "extensions-enabled",
                            G_BINDING_BIDIRECTIONAL|G_BINDING_SYNC_CREATE);

    if (self->is_supported)
        update_extension_list (self);

    g_signal_connect (self->proxy,
                      "extension-state-changed",
                      G_CALLBACK (on_state_changed),
                      self);
}
