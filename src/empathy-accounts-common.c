/*
 * Copyright (C) 2005-2007 Imendio AB
 * Copyright (C) 2007-2010 Collabora Ltd.
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
 * Authors: Martyn Russell <martyn@imendio.com>
 *          Xavier Claessens <xclaesse@gmail.com>
 *          Cosimo Cecchi <cosimo.cecchi@collabora.co.uk>
 *          Jonathan Tellier <jonathan.tellier@gmail.com>
 *          Travis Reitter <travis.reitter@collabora.co.uk>
 */

#include <config.h>

#include <string.h>
#include <stdlib.h>

#include <gtk/gtk.h>
#include <glib/gi18n-lib.h>
#include <unique/unique.h>

#include <telepathy-glib/account-manager.h>
#include <telepathy-glib/util.h>

#include <libempathy/empathy-utils.h>
#include <libempathy/empathy-connection-managers.h>
#include <libempathy-gtk/empathy-ui-utils.h>

#include "empathy-accounts-common.h"
#include "empathy-accounts-dialog.h"
#include "empathy-account-assistant.h"
#include "empathy-import-mc4-accounts.h"
#include "empathy-auto-salut-account-helper.h"

#define DEBUG_FLAG EMPATHY_DEBUG_ACCOUNT
#include <libempathy/empathy-debug.h>

gboolean
empathy_accounts_has_non_salut_accounts (TpAccountManager *manager)
{
  gboolean ret = FALSE;
  GList *accounts, *l;

  accounts = tp_account_manager_get_valid_accounts (manager);

  for (l = accounts ; l != NULL; l = g_list_next (l))
    {
      if (tp_strdiff (tp_account_get_protocol (l->data), "local-xmpp"))
        {
          ret = TRUE;
          break;
        }
    }

  g_list_free (accounts);

  return ret;
}

gboolean
empathy_accounts_has_accounts (TpAccountManager *manager)
{
  GList *accounts;
  gboolean has_accounts;

  accounts = tp_account_manager_get_valid_accounts (manager);
  has_accounts = (accounts != NULL);
  g_list_free (accounts);

  return has_accounts;
}

void
empathy_accounts_import (TpAccountManager *account_mgr,
    EmpathyConnectionManagers *cm_mgr)
{
  g_return_if_fail (tp_account_manager_is_prepared (account_mgr,
      TP_ACCOUNT_MANAGER_FEATURE_CORE));
  g_return_if_fail (empathy_connection_managers_is_ready (cm_mgr));

  if (!empathy_import_mc4_has_imported ())
    empathy_import_mc4_accounts (cm_mgr);
}

static void
do_show_accounts_ui (TpAccountManager *manager,
    TpAccount *account,
    GCallback window_destroyed_cb)
{
  GtkWidget *accounts_window;

  accounts_window = empathy_accounts_dialog_show (NULL, account);

  if (window_destroyed_cb)
    g_signal_connect (accounts_window, "destroy", window_destroyed_cb, NULL);

  gtk_window_present (GTK_WINDOW (accounts_window));
}

static GtkWidget *
show_account_assistant (EmpathyConnectionManagers *connection_mgrs,
    GCallback assistant_destroy_cb)
{
  GtkWidget *assistant;

  assistant = empathy_account_assistant_show (NULL, connection_mgrs);
  if (assistant_destroy_cb)
    g_signal_connect (assistant, "destroy", assistant_destroy_cb, NULL);

  return assistant;
}

static void
connection_managers_prepare_for_accounts (GObject *source,
    GAsyncResult *result,
    gpointer user_data)
{
  EmpathyConnectionManagers *cm_mgr = EMPATHY_CONNECTION_MANAGERS (source);
  GCallback assistant_destroy_cb = G_CALLBACK (user_data);

  if (!empathy_connection_managers_prepare_finish (cm_mgr, result, NULL))
    goto out;

  show_account_assistant (cm_mgr, assistant_destroy_cb);
  DEBUG ("would show the account assistant");

out:
  g_object_unref (cm_mgr);
}

void
empathy_accounts_show_accounts_ui (TpAccountManager *manager,
    TpAccount *account,
    GCallback window_destroyed_cb)
{
  g_return_if_fail (TP_IS_ACCOUNT_MANAGER (manager));
  g_return_if_fail (!account || TP_IS_ACCOUNT (account));

  if (empathy_accounts_has_non_salut_accounts (manager))
    {
      do_show_accounts_ui (manager, account, window_destroyed_cb);
    }
  else
    {
      EmpathyConnectionManagers *cm_mgr;

      cm_mgr = empathy_connection_managers_dup_singleton ();

      empathy_connection_managers_prepare_async (cm_mgr,
          connection_managers_prepare_for_accounts, window_destroyed_cb);
    }
}
