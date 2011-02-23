/*
 * Copyright (C) 2007-2009 Collabora Ltd.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public
 * License along with this program; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA  02110-1301  USA
 *
 * Authors: Xavier Claessens <xclaesse@gmail.com>
 */

#include <config.h>

#include <stdlib.h>
#include <errno.h>
#include <string.h>

#include <glib.h>
#include <glib/gstdio.h>
#include <glib/gi18n.h>
#include <gtk/gtk.h>
#include <gdk/gdkx.h>
#include <unique/unique.h>

#ifdef HAVE_LIBCHAMPLAIN
#include <clutter-gtk/clutter-gtk.h>
#endif

#include <libnotify/notify.h>

#include <telepathy-glib/account-manager.h>
#include <telepathy-glib/dbus.h>
#include <telepathy-glib/debug-sender.h>
#include <telepathy-glib/util.h>
#include <telepathy-glib/connection-manager.h>
#include <telepathy-glib/interfaces.h>

#if HAVE_CALL
 #include <telepathy-yell/telepathy-yell.h>
#endif

#include <telepathy-logger/log-manager.h>

#include <libempathy/empathy-idle.h>
#include <libempathy/empathy-utils.h>
#include <libempathy/empathy-chatroom-manager.h>
#include <libempathy/empathy-account-settings.h>
#include <libempathy/empathy-connectivity.h>
#include <libempathy/empathy-connection-managers.h>
#include <libempathy/empathy-dispatcher.h>
#include <libempathy/empathy-ft-factory.h>
#include <libempathy/empathy-gsettings.h>
#include <libempathy/empathy-tp-chat.h>

#include <libempathy-gtk/empathy-ui-utils.h>
#include <libempathy-gtk/empathy-location-manager.h>
#include <libempathy-gtk/empathy-theme-manager.h>

#include "empathy-main-window.h"
#include "empathy-accounts-common.h"
#include "empathy-accounts-dialog.h"
#include "empathy-chat-manager.h"
#include "empathy-status-icon.h"
#include "empathy-ft-manager.h"

#include "extensions/extensions.h"

#define DEBUG_FLAG EMPATHY_DEBUG_OTHER
#include <libempathy/empathy-debug.h>

static gboolean start_hidden = FALSE;
static gboolean no_connect = FALSE;

static void account_manager_ready_cb (GObject *source_object,
    GAsyncResult *result,
    gpointer user_data);

static void
use_conn_notify_cb (GSettings *gsettings,
    const gchar *key,
    gpointer     user_data)
{
  EmpathyConnectivity *connectivity = user_data;

  empathy_connectivity_set_use_conn (connectivity,
      g_settings_get_boolean (gsettings, key));
}

static void
migrate_config_to_xdg_dir (void)
{
  gchar *xdg_dir, *old_dir, *xdg_filename, *old_filename;
  int i;
  GFile *xdg_file, *old_file;
  static const gchar* filenames[] = {
    "geometry.ini",
    "irc-networks.xml",
    "chatrooms.xml",
    "contact-groups.xml",
    "status-presets.xml",
    "accels.txt",
    NULL
  };

  xdg_dir = g_build_filename (g_get_user_config_dir (), PACKAGE_NAME, NULL);
  if (g_file_test (xdg_dir, G_FILE_TEST_EXISTS | G_FILE_TEST_IS_DIR))
    {
      /* xdg config dir already exists */
      g_free (xdg_dir);
      return;
    }

  old_dir = g_build_filename (g_get_home_dir (), ".gnome2",
      PACKAGE_NAME, NULL);
  if (!g_file_test (old_dir, G_FILE_TEST_EXISTS | G_FILE_TEST_IS_DIR))
    {
      /* old config dir didn't exist */
      g_free (xdg_dir);
      g_free (old_dir);
      return;
    }

  if (g_mkdir_with_parents (xdg_dir, (S_IRUSR | S_IWUSR | S_IXUSR)) == -1)
    {
      DEBUG ("Failed to create configuration directory; aborting migration");
      g_free (xdg_dir);
      g_free (old_dir);
      return;
    }

  for (i = 0; filenames[i]; i++)
    {
      old_filename = g_build_filename (old_dir, filenames[i], NULL);
      if (!g_file_test (old_filename, G_FILE_TEST_EXISTS))
        {
          g_free (old_filename);
          continue;
        }
      xdg_filename = g_build_filename (xdg_dir, filenames[i], NULL);
      old_file = g_file_new_for_path (old_filename);
      xdg_file = g_file_new_for_path (xdg_filename);

      if (!g_file_move (old_file, xdg_file, G_FILE_COPY_NONE,
          NULL, NULL, NULL, NULL))
        DEBUG ("Failed to migrate %s", filenames[i]);

      g_free (old_filename);
      g_free (xdg_filename);
      g_object_unref (old_file);
      g_object_unref (xdg_file);
    }

  g_free (xdg_dir);
  g_free (old_dir);
}

static void
show_accounts_ui (GdkScreen *screen,
    gboolean if_needed)
{
  empathy_accounts_dialog_show_application (screen,
      NULL, if_needed, start_hidden);
}

static UniqueResponse
unique_app_message_cb (UniqueApp *unique_app,
    gint command,
    UniqueMessageData *data,
    guint timestamp,
    gpointer user_data)
{
  GtkWindow *window = user_data;
  TpAccountManager *account_manager;

  DEBUG ("Other instance launched, presenting the main window. "
      "Command=%d, timestamp %u", command, timestamp);

  /* XXX: the standalone app somewhat breaks this case, since
   * communicating it would be a pain */

  /* We're requested to show stuff again, disable the start hidden global
   * in case the accounts wizard wants to pop up.
   */
  start_hidden = FALSE;

  gtk_window_set_screen (GTK_WINDOW (window),
      unique_message_data_get_screen (data));
  gtk_window_set_startup_id (GTK_WINDOW (window),
      unique_message_data_get_startup_id (data));
  gtk_window_present_with_time (GTK_WINDOW (window), timestamp);
  gtk_window_set_skip_taskbar_hint (window, FALSE);

  account_manager = tp_account_manager_dup ();
  tp_account_manager_prepare_async (account_manager, NULL,
      account_manager_ready_cb, NULL);
  g_object_unref (account_manager);

  return UNIQUE_RESPONSE_OK;
}

static gboolean show_version_cb (const char *option_name,
    const char *value,
    gpointer data,
    GError **error) G_GNUC_NORETURN;

static gboolean
show_version_cb (const char *option_name,
    const char *value,
    gpointer data,
    GError **error)
{
  g_print ("%s\n", PACKAGE_STRING);

  exit (EXIT_SUCCESS);
}

static void
new_incoming_transfer_cb (EmpathyFTFactory *factory,
    EmpathyFTHandler *handler,
    GError *error,
    gpointer user_data)
{
  if (error)
    empathy_ft_manager_display_error (handler, error);
  else
    empathy_receive_file_with_file_chooser (handler);
}

static void
new_ft_handler_cb (EmpathyFTFactory *factory,
    EmpathyFTHandler *handler,
    GError *error,
    gpointer user_data)
{
  if (error)
    empathy_ft_manager_display_error (handler, error);
  else
    empathy_ft_manager_add_handler (handler);

  g_object_unref (handler);
}

static void
account_manager_ready_cb (GObject *source_object,
    GAsyncResult *result,
    gpointer user_data)
{
  TpAccountManager *manager = TP_ACCOUNT_MANAGER (source_object);
  GError *error = NULL;
  EmpathyIdle *idle;
  EmpathyConnectivity *connectivity;
  TpConnectionPresenceType presence;
  GSettings *gsettings = g_settings_new (EMPATHY_PREFS_SCHEMA);

  if (!tp_account_manager_prepare_finish (manager, result, &error))
    {
      DEBUG ("Failed to prepare account manager: %s", error->message);
      g_error_free (error);
      return;
    }

  /* Autoconnect */
  idle = empathy_idle_dup_singleton ();
  connectivity = empathy_connectivity_dup_singleton ();

  presence = tp_account_manager_get_most_available_presence (manager, NULL,
      NULL);

  if (g_settings_get_boolean (gsettings, EMPATHY_PREFS_AUTOCONNECT) &&
      !no_connect &&
      tp_connection_presence_type_cmp_availability
          (presence, TP_CONNECTION_PRESENCE_TYPE_OFFLINE)
            <= 0)
      /* if current state is Offline, then put it online */
      empathy_idle_set_state (idle, TP_CONNECTION_PRESENCE_TYPE_AVAILABLE);

  /* Pop up the accounts dialog if we don't have any account */
  if (!empathy_accounts_has_accounts (manager))
    show_accounts_ui (gdk_screen_get_default (), TRUE);

  g_object_unref (idle);
  g_object_unref (connectivity);
  g_object_unref (gsettings);
}

static void
account_status_changed_cb (TpAccount *account,
    guint old_status,
    guint new_status,
    guint reason,
    gchar *dbus_error_name,
    GHashTable *details,
    EmpathyChatroom *room)
{
  if (new_status != TP_CONNECTION_STATUS_CONNECTED)
    return;

  empathy_dispatcher_join_muc (account,
      empathy_chatroom_get_room (room), TP_USER_ACTION_TIME_NOT_USER_ACTION);
}

static void
account_manager_chatroom_ready_cb (GObject *source_object,
    GAsyncResult *result,
    gpointer user_data)
{
  TpAccountManager *account_manager = TP_ACCOUNT_MANAGER (source_object);
  EmpathyChatroomManager *chatroom_manager = user_data;
  GList *accounts, *l;
  GError *error = NULL;

  if (!tp_account_manager_prepare_finish (account_manager, result, &error))
    {
      DEBUG ("Failed to prepare account manager: %s", error->message);
      g_error_free (error);
      return;
    }

  accounts = tp_account_manager_get_valid_accounts (account_manager);

  for (l = accounts; l != NULL; l = g_list_next (l))
    {
      TpAccount *account = TP_ACCOUNT (l->data);
      TpConnection *conn;
      GList *chatrooms, *p;

      conn = tp_account_get_connection (account);

      chatrooms = empathy_chatroom_manager_get_chatrooms (
          chatroom_manager, account);

      for (p = chatrooms; p != NULL; p = p->next)
        {
          EmpathyChatroom *room = EMPATHY_CHATROOM (p->data);

          if (!empathy_chatroom_get_auto_connect (room))
            continue;

          if (conn == NULL)
            {
              g_signal_connect (G_OBJECT (account), "status-changed",
                  G_CALLBACK (account_status_changed_cb), room);
            }
          else
            {
              empathy_dispatcher_join_muc (account,
                  empathy_chatroom_get_room (room),
                  TP_USER_ACTION_TIME_NOT_USER_ACTION);
            }
        }

      g_list_free (chatrooms);
    }

  g_list_free (accounts);
}

static void
chatroom_manager_ready_cb (EmpathyChatroomManager *chatroom_manager,
    GParamSpec *pspec,
    gpointer user_data)
{
  TpAccountManager *account_manager = user_data;

  tp_account_manager_prepare_async (account_manager, NULL,
      account_manager_chatroom_ready_cb, chatroom_manager);
}

static void
empathy_idle_set_auto_away_cb (GSettings *gsettings,
				const gchar *key,
				gpointer user_data)
{
	EmpathyIdle *idle = user_data;

	empathy_idle_set_auto_away (idle,
      g_settings_get_boolean (gsettings, key));
}

int
main (int argc, char *argv[])
{
#ifdef HAVE_GEOCLUE
  EmpathyLocationManager *location_manager = NULL;
#endif
  EmpathyStatusIcon *icon;
  EmpathyDispatcher *dispatcher;
  TpAccountManager *account_manager;
  TplLogManager *log_manager;
  EmpathyChatroomManager *chatroom_manager;
  EmpathyFTFactory  *ft_factory;
  GtkWidget *window;
  EmpathyIdle *idle;
  EmpathyConnectivity *connectivity;
  EmpathyChatManager *chat_manager;
  GError *error = NULL;
  UniqueApp *unique_app;
  gboolean chatroom_manager_ready;
  gboolean autoaway = TRUE;
#ifdef ENABLE_DEBUG
  TpDebugSender *debug_sender;
#endif
  GSettings *gsettings;
  EmpathyThemeManager *theme_mgr;

  GOptionContext *optcontext;
  GOptionEntry options[] = {
      { "no-connect", 'n',
        0, G_OPTION_ARG_NONE, &no_connect,
        N_("Don't connect on startup"),
        NULL },
      { "start-hidden", 'h',
        0, G_OPTION_ARG_NONE, &start_hidden,
        N_("Don't display the contact list or any other dialogs on startup"),
        NULL },
      { "version", 'v',
        G_OPTION_FLAG_NO_ARG, G_OPTION_ARG_CALLBACK, show_version_cb,
        NULL, NULL },
      { NULL }
  };

  /* Init */
  g_thread_init (NULL);

#ifdef HAVE_LIBCHAMPLAIN
  gtk_clutter_init (&argc, &argv);
#endif

  tpy_cli_init ();

  empathy_init ();

  optcontext = g_option_context_new (N_("- Empathy IM Client"));
  g_option_context_add_group (optcontext, gtk_get_option_group (TRUE));
  g_option_context_add_main_entries (optcontext, options, GETTEXT_PACKAGE);

  if (!g_option_context_parse (optcontext, &argc, &argv, &error)) {
    g_print ("%s\nRun '%s --help' to see a full list of available command line options.\n",
        error->message, argv[0]);
    g_warning ("Error in empathy init: %s", error->message);
    return EXIT_FAILURE;
  }

  g_option_context_free (optcontext);

  empathy_gtk_init ();
  g_set_application_name (_(PACKAGE_NAME));

  gtk_window_set_default_icon_name ("empathy");
  textdomain (GETTEXT_PACKAGE);

#ifdef ENABLE_DEBUG
  /* Set up debug sender */
  debug_sender = tp_debug_sender_dup ();
  g_log_set_default_handler (tp_debug_sender_log_handler, G_LOG_DOMAIN);
#endif

  unique_app = unique_app_new ("org.gnome."PACKAGE_NAME, NULL);

  if (unique_app_is_running (unique_app))
    {
      if (unique_app_send_message (unique_app, UNIQUE_ACTIVATE, NULL) ==
          UNIQUE_RESPONSE_OK)
        {
          g_object_unref (unique_app);
          return EXIT_SUCCESS;
        }
    }

  notify_init (_(PACKAGE_NAME));

  /* Setting up Idle */
  idle = empathy_idle_dup_singleton ();

  gsettings = g_settings_new (EMPATHY_PREFS_SCHEMA);
  autoaway = g_settings_get_boolean (gsettings, EMPATHY_PREFS_AUTOAWAY);

  g_signal_connect (gsettings,
      "changed::" EMPATHY_PREFS_AUTOAWAY,
      G_CALLBACK (empathy_idle_set_auto_away_cb), idle);

  empathy_idle_set_auto_away (idle, autoaway);

  /* Setting up Connectivity */
  connectivity = empathy_connectivity_dup_singleton ();
  use_conn_notify_cb (gsettings, EMPATHY_PREFS_USE_CONN,
      connectivity);
  g_signal_connect (gsettings,
      "changed::" EMPATHY_PREFS_USE_CONN,
      G_CALLBACK (use_conn_notify_cb), connectivity);

  /* account management */
  account_manager = tp_account_manager_dup ();
  tp_account_manager_prepare_async (account_manager, NULL,
      account_manager_ready_cb, NULL);

  /* The EmpathyDispatcher doesn't dispatch anything any more but we have to
   * keep it around as we still use it to request channels */
  dispatcher = empathy_dispatcher_dup_singleton ();

  migrate_config_to_xdg_dir ();

  /* Setting up UI */
  window = empathy_main_window_dup ();
  gtk_widget_show (window);
  icon = empathy_status_icon_new (GTK_WINDOW (window), start_hidden);

  /* Chat manager */
  chat_manager = empathy_chat_manager_dup_singleton ();

  g_signal_connect (unique_app, "message-received",
      G_CALLBACK (unique_app_message_cb), window);

  /* Logging */
  log_manager = tpl_log_manager_dup_singleton ();

  chatroom_manager = empathy_chatroom_manager_dup_singleton (NULL);

  g_object_get (chatroom_manager, "ready", &chatroom_manager_ready, NULL);
  if (!chatroom_manager_ready)
    {
      g_signal_connect (G_OBJECT (chatroom_manager), "notify::ready",
          G_CALLBACK (chatroom_manager_ready_cb), account_manager);
    }
  else
    {
      chatroom_manager_ready_cb (chatroom_manager, NULL, account_manager);
    }

  /* Create the FT factory */
  ft_factory = empathy_ft_factory_dup_singleton ();
  g_signal_connect (ft_factory, "new-ft-handler",
      G_CALLBACK (new_ft_handler_cb), NULL);
  g_signal_connect (ft_factory, "new-incoming-transfer",
      G_CALLBACK (new_incoming_transfer_cb), NULL);

  if (!empathy_ft_factory_register (ft_factory, &error))
    {
      g_warning ("Failed to register FileTransfer handler: %s", error->message);
      g_error_free (error);
    }

  /* Location mananger */
#ifdef HAVE_GEOCLUE
  location_manager = empathy_location_manager_dup_singleton ();
#endif

  /* Keep the theme manager alive as it does some caching */
  theme_mgr = empathy_theme_manager_dup_singleton ();

  gtk_main ();

  empathy_idle_set_state (idle, TP_CONNECTION_PRESENCE_TYPE_OFFLINE);

#ifdef ENABLE_DEBUG
  g_object_unref (debug_sender);
#endif

  g_object_unref (chat_manager);
  g_object_unref (idle);
  g_object_unref (connectivity);
  g_object_unref (icon);
  g_object_unref (account_manager);
  g_object_unref (log_manager);
  g_object_unref (dispatcher);
  g_object_unref (chatroom_manager);
#ifdef HAVE_GEOCLUE
  g_object_unref (location_manager);
#endif
  g_object_unref (ft_factory);
  g_object_unref (unique_app);
  g_object_unref (gsettings);
  g_object_unref (theme_mgr);
  gtk_widget_destroy (window);

  notify_uninit ();
  xmlCleanupParser ();

  return EXIT_SUCCESS;
}
