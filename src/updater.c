/*
Copyright (c) 2021 Raspberry Pi (Trading) Ltd.
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:
    * Redistributions of source code must retain the above copyright
      notice, this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright
      notice, this list of conditions and the following disclaimer in the
      documentation and/or other materials provided with the distribution.
    * Neither the name of the copyright holder nor the
      names of its contributors may be used to endorse or promote products
      derived from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY
DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/


#include <string.h>
#include <math.h>
#include <ctype.h>
#include <stdlib.h>
#include <locale.h>

#include <glib.h>
#include <glib/gi18n.h>
#include <glib/gstdio.h>
#include <gtk/gtk.h>
#include <gdk/gdkx.h>

#define I_KNOW_THE_PACKAGEKIT_GLIB2_API_IS_SUBJECT_TO_CHANGE
#include <packagekit-glib2/packagekit.h>

#include <X11/Xlib.h>

#include <libintl.h>

#define MSG_PULSE  -1
#define MSG_PROMPT -2
#define MSG_REBOOT -3

/* Controls */

static GtkWidget *msg_dlg, *msg_msg, *msg_pb, *msg_btn, *msg_btn2;

gboolean success = TRUE;
gboolean wayland;
int calls;

/*----------------------------------------------------------------------------*/
/* Prototypes                                                                 */
/*----------------------------------------------------------------------------*/

static gboolean net_available (void);
static gboolean clock_synced (void);
static void resync (void);
static void message (char *msg, int type);
static gboolean quit (GtkButton *button, gpointer data);
static gboolean reboot (GtkButton *button, gpointer data);
static PkResults *error_handler (PkTask *task, PkClient *client, GAsyncResult *res, char *desc);
static void progress (PkProgress *progress, PkProgressType type, gpointer data);
static gboolean refresh_cache (gpointer data);
static void compare_versions (PkClient *client, GAsyncResult *res, gpointer data);
static void start_install (PkClient *client, GAsyncResult *res, gpointer data);
static void install_done (PkTask *task, GAsyncResult *res, gpointer data);

/*----------------------------------------------------------------------------*/
/* Helper functions for system status                                         */
/*----------------------------------------------------------------------------*/

static gboolean net_available (void)
{
    if (system ("hostname -I | grep -q \\\\.") == 0) return TRUE;
    else return FALSE;
}

static gboolean clock_synced (void)
{
    if (system ("test -e /usr/sbin/ntpd") == 0)
    {
        if (system ("ntpq -p | grep -q ^\\*") == 0) return TRUE;
    }
    else
    {
        if (system ("timedatectl status | grep -q \"synchronized: yes\"") == 0) return TRUE;
    }
    return FALSE;
}

static void resync (void)
{
    if (system ("test -e /usr/sbin/ntpd") == 0)
    {
        system ("sudo /etc/init.d/ntp stop; sudo ntpd -gq; sudo /etc/init.d/ntp start");
    }
    else
    {
        system ("sudo systemctl -q stop systemd-timesyncd 2> /dev/null; sudo systemctl -q start systemd-timesyncd 2> /dev/null");
    }
}

/*----------------------------------------------------------------------------*/
/* Progress / error box                                                       */
/*----------------------------------------------------------------------------*/

static void message (char *msg, int type)
{
    if (!msg_dlg)
    {
        GtkBuilder *builder;

        builder = gtk_builder_new_from_file (PACKAGE_DATA_DIR "/gui-updater.ui");

        msg_dlg = (GtkWidget *) gtk_builder_get_object (builder, "modal");
        msg_msg = (GtkWidget *) gtk_builder_get_object (builder, "modal_msg");
        msg_pb = (GtkWidget *) gtk_builder_get_object (builder, "modal_pb");
        msg_btn = (GtkWidget *) gtk_builder_get_object (builder, "modal_ok");
        msg_btn2 = (GtkWidget *) gtk_builder_get_object (builder, "modal_cancel");

        g_object_unref (builder);
    }

    gtk_label_set_text (GTK_LABEL (msg_msg), msg);

    gtk_widget_hide (msg_pb);
    gtk_widget_hide (msg_btn);
    gtk_widget_hide (msg_btn2);

    switch (type)
    {
        case MSG_PROMPT :   g_signal_connect (msg_btn, "clicked", G_CALLBACK (quit), NULL);
                            gtk_widget_show (msg_btn);
                            break;

        case MSG_REBOOT :   gtk_button_set_label (GTK_BUTTON (msg_btn), _("Reboot"));
                            gtk_button_set_label (GTK_BUTTON (msg_btn2), _("Later"));
                            g_signal_connect (msg_btn, "clicked", G_CALLBACK (reboot), NULL);
                            g_signal_connect (msg_btn2, "clicked", G_CALLBACK (quit), NULL);
                            gtk_widget_show (msg_btn);
                            gtk_widget_show (msg_btn2);
                            break;

        case MSG_PULSE :    gtk_widget_show (msg_pb);
                            gtk_progress_bar_pulse (GTK_PROGRESS_BAR (msg_pb));
                            break;

        default :           gtk_widget_show (msg_pb);
                            gtk_progress_bar_set_fraction (GTK_PROGRESS_BAR (msg_pb), (type / 100.0));
                            break;
    }

    gboolean vis = gtk_widget_is_visible (msg_dlg);
    gtk_widget_show (msg_dlg);

    // force a redraw if the window was not already displayed - forces a resize to correct size
    if (wayland && !vis)
    {
        gtk_widget_hide (msg_dlg);
        gtk_widget_unrealize (msg_dlg);
        gtk_widget_show (msg_dlg);
    }
}

static gboolean quit (GtkButton *button, gpointer data)
{
    if (success)
    {
        if (wayland)
            system ("wfpanelctl updater check");
        else
            system ("lxpanelctl-pi command updater check");
    }

    if (msg_dlg)
    {
        gtk_widget_destroy (GTK_WIDGET (msg_dlg));
        msg_dlg = NULL;
    }

    gtk_main_quit ();
    return FALSE;
}

static gboolean reboot (GtkButton *button, gpointer data)
{
    if (msg_dlg)
    {
        gtk_widget_destroy (GTK_WIDGET (msg_dlg));
        msg_dlg = NULL;
    }

    system ("sync;reboot");
    return FALSE;
}


/*----------------------------------------------------------------------------*/
/* Helper functions for async operations                                      */
/*----------------------------------------------------------------------------*/

static PkResults *error_handler (PkTask *task, PkClient *client, GAsyncResult *res, char *desc)
{
    PkResults *results;
    PkError *pkerror;
    GError *error = NULL;
    gchar *buf;

    if (task) results = pk_task_generic_finish (task, res, &error);
    else results = pk_client_generic_finish (client, res, &error);
    if (error != NULL)
    {
        success = FALSE;
        buf = g_strdup_printf (_("Error %s - %s"), desc, error->message);
        message (buf, MSG_PROMPT);
        g_free (buf);
        g_error_free (error);
        return NULL;
    }

    pkerror = pk_results_get_error_code (results);
    if (pkerror != NULL)
    {
        success = FALSE;
        buf = g_strdup_printf (_("Error %s - %s"), desc, pk_error_get_details (pkerror));
        message (buf, MSG_PROMPT);
        g_free (buf);
        g_object_unref (pkerror);
        return NULL;
    }

    return results;
}

static void progress (PkProgress *progress, PkProgressType type, gpointer data)
{
    int role = pk_progress_get_role (progress);
    int status = pk_progress_get_status (progress);
    int percent = pk_progress_get_percentage (progress);

    if (msg_dlg)
    {
        if ((type == PK_PROGRESS_TYPE_PERCENTAGE || type == PK_PROGRESS_TYPE_ITEM_PROGRESS
            || type == PK_PROGRESS_TYPE_PACKAGE || type == PK_PROGRESS_TYPE_PACKAGE_ID
            || type == PK_PROGRESS_TYPE_DOWNLOAD_SIZE_REMAINING || type == PK_PROGRESS_TYPE_SPEED)
            && percent >= 0 && percent <= 100)
        {
            switch (role)
            {
                case PK_ROLE_ENUM_GET_UPDATES :         if (status == PK_STATUS_ENUM_LOADING_CACHE)
                                                            message (_("Comparing versions - please wait..."), percent);
                                                        else
                                                            gtk_progress_bar_pulse (GTK_PROGRESS_BAR (msg_pb));
                                                        break;

                case PK_ROLE_ENUM_UPDATE_PACKAGES :     if (status == PK_STATUS_ENUM_DOWNLOAD)
                                                            message (_("Downloading updates - please wait..."), percent);
                                                        else if (status == PK_STATUS_ENUM_INSTALL || status == PK_STATUS_ENUM_RUNNING)
                                                            message (_("Installing updates - please wait..."), percent);
                                                        else
                                                            gtk_progress_bar_pulse (GTK_PROGRESS_BAR (msg_pb));
                                                        break;

                default :                           gtk_progress_bar_pulse (GTK_PROGRESS_BAR (msg_pb));
                                                    break;
            }
        }
        else gtk_progress_bar_pulse (GTK_PROGRESS_BAR (msg_pb));
    }
}


/*----------------------------------------------------------------------------*/
/* Handlers for PackageKit asynchronous install sequence                      */
/*----------------------------------------------------------------------------*/

static gboolean ntp_check (gpointer data)
{
    if (clock_synced ())
    {
        g_idle_add (refresh_cache, NULL);
        return FALSE;
    }

    if (calls++ > 120)
    {
        message (_("Could not sync time - exiting"), MSG_PROMPT);
        return FALSE;
    }

    return TRUE;
}

static gboolean refresh_cache (gpointer data)
{
    PkTask *task;

    message (_("Updating package data - please wait..."), MSG_PULSE);

    task = pk_task_new ();

    pk_client_refresh_cache_async (PK_CLIENT (task), TRUE, NULL, (PkProgressCallback) progress, NULL, (GAsyncReadyCallback) compare_versions, NULL);
    return FALSE;
}

static void compare_versions (PkClient *client, GAsyncResult *res, gpointer data)
{
    if (!error_handler (NULL, client, res, _("updating cache"))) return;

    message (_("Comparing versions - please wait..."), MSG_PULSE);

    pk_client_get_updates_async (client, PK_FILTER_ENUM_NONE, NULL, (PkProgressCallback) progress, NULL, (GAsyncReadyCallback) start_install, data);
}

static gboolean filter_fn (PkPackage *package, gpointer user_data)
{
    PkInfoEnum info = pk_package_get_info (package);
	switch (info)
    {
        case PK_INFO_ENUM_LOW:
        case PK_INFO_ENUM_NORMAL:
        case PK_INFO_ENUM_IMPORTANT:
        case PK_INFO_ENUM_SECURITY:
        case PK_INFO_ENUM_BUGFIX:
        case PK_INFO_ENUM_ENHANCEMENT:
        case PK_INFO_ENUM_BLOCKED:      return TRUE;
                                        break;

        default:                        return FALSE;
                                        break;
    }
}

static gboolean filter_fn_x86 (PkPackage *package, gpointer user_data)
{
    if (strstr (pk_package_get_arch (package), "amd64")) return FALSE;
    return filter_fn (package, NULL);
}

static void start_install (PkClient *client, GAsyncResult *res, gpointer data)
{
    PkPackageSack *sack = NULL, *fsack;
    gchar **ids;

    PkResults *results = error_handler (NULL, client, res, _("comparing versions"));
    if (!results) return;

    sack = pk_results_get_package_sack (results);
    if (system ("raspi-config nonint is_pi"))
        fsack = pk_package_sack_filter (sack, filter_fn_x86, data);
    else
        fsack = pk_package_sack_filter (sack, filter_fn, data);

    if (pk_package_sack_get_size (fsack) > 0)
    {
        message (_("Downloading updates - please wait..."), MSG_PULSE);

        ids = pk_package_sack_get_ids (fsack);
        pk_task_update_packages_async (PK_TASK (client), ids, NULL, (PkProgressCallback) progress, NULL, (GAsyncReadyCallback) install_done, NULL);
        g_strfreev (ids);
    }
    else
    {
        gtk_window_set_title (GTK_WINDOW (msg_dlg), _("Install complete"));
        message (_("System is up to date"), MSG_PROMPT);
    }

    if (sack) g_object_unref (sack);
    g_object_unref (fsack);
}

static void install_done (PkTask *task, GAsyncResult *res, gpointer data)
{
    if (!error_handler (task, NULL, res, _("installing packages"))) return;
    gtk_window_set_title (GTK_WINDOW (msg_dlg), _("Install complete"));

    if (access ("/run/reboot-required", F_OK))
        message (_("System is up to date"), MSG_PROMPT);
    else
        message (_("System is up to date.\nA reboot is required to complete the install. Reboot now or later?"), MSG_REBOOT);
}


/*----------------------------------------------------------------------------*/
/* Main function                                                              */
/*----------------------------------------------------------------------------*/

int main (int argc, char *argv[])
{
    setlocale (LC_ALL, "");
    bindtextdomain (GETTEXT_PACKAGE, PACKAGE_LOCALE_DIR);
    bind_textdomain_codeset (GETTEXT_PACKAGE, "UTF-8");
    textdomain (GETTEXT_PACKAGE);

    // GTK setup
    g_set_prgname ("wf-panel-pi");
    gtk_init (&argc, &argv);
    gtk_icon_theme_prepend_search_path (gtk_icon_theme_get_default(), PACKAGE_DATA_DIR);

    if (getenv ("WAYLAND_DISPLAY")) wayland = TRUE;
    else wayland = FALSE;

    // check the network is connected and the clock is synced
    calls = 0;
    if (!net_available ())
    {
        message (_("No network connection - exiting"), MSG_PROMPT);
    }
    else if (!clock_synced ())
    {
        message (_("Synchronising clock - please wait..."), MSG_PULSE);
        resync ();
        g_timeout_add_seconds (1, ntp_check, NULL);
    }
    else g_idle_add (refresh_cache, NULL);

    gtk_main ();

    return 0;
}


/* End of file                                                                */
/*----------------------------------------------------------------------------*/
