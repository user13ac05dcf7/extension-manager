/*
 * exm-repository-dialog.c
 *
 * Copyright 2026 Jake
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

#include "exm-repository-dialog.h"

#include "exm-config.h"
#include "web/exm-repository.h"

#include <glib/gi18n.h>

struct _ExmRepositoryDialog
{
    AdwPreferencesDialog parent_instance;

    ExmManager *manager;

    AdwPreferencesGroup *group;
    AdwEntryRow *url_row;
    AdwEntryRow *key_row;
    GtkLabel *error_label;
    GtkButton *restore_button;
    GtkButton *default_button;
    GtkButton *apply_button;
};

G_DEFINE_FINAL_TYPE (ExmRepositoryDialog, exm_repository_dialog, ADW_TYPE_PREFERENCES_DIALOG)

enum {
    PROP_0,
    PROP_MANAGER,
    N_PROPS
};

static GParamSpec *properties [N_PROPS];

ExmRepositoryDialog *
exm_repository_dialog_new (ExmManager *manager)
{
    return g_object_new (EXM_TYPE_REPOSITORY_DIALOG,
                         "manager", manager,
                         NULL);
}

static void
show_error (ExmRepositoryDialog *self,
            const gchar         *message)
{
    gtk_label_set_label (self->error_label, message);
    gtk_widget_set_visible (GTK_WIDGET (self->error_label), message != NULL);
}

static void
set_busy (ExmRepositoryDialog *self,
          gboolean             busy)
{
    gtk_widget_set_sensitive (GTK_WIDGET (self->url_row), !busy);
    gtk_widget_set_sensitive (GTK_WIDGET (self->key_row), !busy);
    gtk_widget_set_sensitive (GTK_WIDGET (self->restore_button), !busy);
    gtk_widget_set_sensitive (GTK_WIDGET (self->default_button), !busy);
    gtk_widget_set_sensitive (GTK_WIDGET (self->apply_button), !busy);
}

static void
load_repository (ExmRepositoryDialog *self)
{
    g_autofree gchar *url = NULL;
    g_autofree gchar *key = NULL;
    ExmRepositoryOrigin origin;

    origin = exm_manager_get_repository (self->manager, &url, &key);

    gtk_editable_set_text (GTK_EDITABLE (self->url_row), url);
    gtk_editable_set_text (GTK_EDITABLE (self->key_row), key);
    show_error (self, NULL);

    if (origin == EXM_REPOSITORY_ORIGIN_SHELL)
    {
        adw_preferences_group_set_description (self->group,
                                               _("GNOME Shell installs and updates extensions from this repository instead of extensions.gnome.org. Its index must be signed with the key below."));
        return;
    }

    adw_preferences_group_set_description (self->group,
                                           _("Extension Manager shows extensions from this repository instead of extensions.gnome.org. Its index must be signed with the key below."));
    gtk_widget_set_visible (GTK_WIDGET (self->restore_button), TRUE);

    if (origin == EXM_REPOSITORY_ORIGIN_ENVIRONMENT)
    {
        set_busy (self, TRUE);
        show_error (self, _("The repository is set by EXM_EXTENSION_REPOSITORY"));
    }
}

static void
on_repository_set (ExmManager          *manager,
                   GAsyncResult        *result,
                   ExmRepositoryDialog *self)
{
    g_autoptr (GError) error = NULL;
    gboolean success = exm_manager_set_repository_finish (manager, result, &error);

    // Closed while the Shell was answering
    if (self->manager == NULL)
    {
        g_object_unref (self);
        return;
    }

    set_busy (self, FALSE);

    if (!success)
    {
        show_error (self, error->message);
        g_object_unref (self);
        return;
    }

    adw_dialog_close (ADW_DIALOG (self));
    g_object_unref (self);
}

static void
set_repository (ExmRepositoryDialog *self,
                const gchar         *url,
                const gchar         *key)
{
    show_error (self, NULL);
    set_busy (self, TRUE);

    exm_manager_set_repository_async (self->manager, url, key, NULL,
                                      (GAsyncReadyCallback) on_repository_set,
                                      g_object_ref (self));
}

static void
on_apply (ExmRepositoryDialog *self)
{
    g_autofree gchar *url = g_strstrip (g_strdup (gtk_editable_get_text (GTK_EDITABLE (self->url_row))));
    g_autofree gchar *key = g_strstrip (g_strdup (gtk_editable_get_text (GTK_EDITABLE (self->key_row))));
    g_autoptr (GError) error = NULL;

    if (*url == '\0')
    {
        show_error (self, _("Enter the index URL, or use extensions.gnome.org"));
        return;
    }

    if (!exm_repository_check_url (url, &error) ||
        !exm_repository_check_key (key, &error))
    {
        show_error (self, error->message);
        return;
    }

    set_repository (self, url, key);
}

static void
on_use_default (ExmRepositoryDialog *self)
{
    set_repository (self, "", "");
}

static void
on_restore (ExmRepositoryDialog *self)
{
    exm_manager_reset_repository (self->manager);
    adw_dialog_close (ADW_DIALOG (self));
}

static void
on_changed (ExmRepositoryDialog *self)
{
    show_error (self, NULL);
}

static void
exm_repository_dialog_get_property (GObject    *object,
                                    guint       prop_id,
                                    GValue     *value,
                                    GParamSpec *pspec)
{
    ExmRepositoryDialog *self = EXM_REPOSITORY_DIALOG (object);

    switch (prop_id)
    {
    case PROP_MANAGER:
        g_value_set_object (value, self->manager);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
exm_repository_dialog_set_property (GObject      *object,
                                    guint         prop_id,
                                    const GValue *value,
                                    GParamSpec   *pspec)
{
    ExmRepositoryDialog *self = EXM_REPOSITORY_DIALOG (object);

    switch (prop_id)
    {
    case PROP_MANAGER:
        self->manager = g_value_dup_object (value);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
exm_repository_dialog_constructed (GObject *object)
{
    ExmRepositoryDialog *self = EXM_REPOSITORY_DIALOG (object);

    G_OBJECT_CLASS (exm_repository_dialog_parent_class)->constructed (object);

    load_repository (self);
}

static void
exm_repository_dialog_dispose (GObject *object)
{
    ExmRepositoryDialog *self = EXM_REPOSITORY_DIALOG (object);

    g_clear_object (&self->manager);

    G_OBJECT_CLASS (exm_repository_dialog_parent_class)->dispose (object);
}

static void
exm_repository_dialog_class_init (ExmRepositoryDialogClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);
    GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

    object_class->get_property = exm_repository_dialog_get_property;
    object_class->set_property = exm_repository_dialog_set_property;
    object_class->constructed = exm_repository_dialog_constructed;
    object_class->dispose = exm_repository_dialog_dispose;

    properties [PROP_MANAGER] =
        g_param_spec_object ("manager",
                             "Manager",
                             "Manager",
                             EXM_TYPE_MANAGER,
                             G_PARAM_READWRITE|G_PARAM_CONSTRUCT_ONLY);

    g_object_class_install_properties (object_class, N_PROPS, properties);

    gtk_widget_class_set_template_from_resource (widget_class, g_strdup_printf ("%s/exm-repository-dialog.ui", RESOURCE_PATH));

    gtk_widget_class_bind_template_child (widget_class, ExmRepositoryDialog, group);
    gtk_widget_class_bind_template_child (widget_class, ExmRepositoryDialog, url_row);
    gtk_widget_class_bind_template_child (widget_class, ExmRepositoryDialog, key_row);
    gtk_widget_class_bind_template_child (widget_class, ExmRepositoryDialog, error_label);
    gtk_widget_class_bind_template_child (widget_class, ExmRepositoryDialog, restore_button);
    gtk_widget_class_bind_template_child (widget_class, ExmRepositoryDialog, default_button);
    gtk_widget_class_bind_template_child (widget_class, ExmRepositoryDialog, apply_button);

    gtk_widget_class_bind_template_callback (widget_class, on_apply);
    gtk_widget_class_bind_template_callback (widget_class, on_use_default);
    gtk_widget_class_bind_template_callback (widget_class, on_restore);
    gtk_widget_class_bind_template_callback (widget_class, on_changed);
}

static void
exm_repository_dialog_init (ExmRepositoryDialog *self)
{
    gtk_widget_init_template (GTK_WIDGET (self));
}
