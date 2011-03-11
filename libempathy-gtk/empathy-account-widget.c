/*
 * Copyright (C) 2006-2007 Imendio AB
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
 *          Martyn Russell <martyn@imendio.com>
 *          Cosimo Cecchi <cosimo.cecchi@collabora.co.uk>
 *          Jonathan Tellier <jonathan.tellier@gmail.com>
 *          Danielle Madeley <danielle.madeley@collabora.co.uk>
 */

#include <config.h>

#include <string.h>

#include <gtk/gtk.h>
#include <glib/gi18n-lib.h>

#ifdef HAVE_MEEGO
#include <mx-gtk/mx-gtk.h>
#endif /* HAVE_MEEGO */


#include <libempathy/empathy-utils.h>

#include <telepathy-glib/account.h>
#include <telepathy-glib/account-manager.h>
#include <telepathy-glib/connection-manager.h>
#include <telepathy-glib/util.h>
#include <dbus/dbus-protocol.h>

#include "empathy-account-widget.h"
#include "empathy-account-widget-private.h"
#include "empathy-account-widget-sip.h"
#include "empathy-account-widget-irc.h"
#include "empathy-account-widget-skype.h"
#include "empathy-ui-utils.h"

#define DEBUG_FLAG EMPATHY_DEBUG_ACCOUNT
#include <libempathy/empathy-debug.h>

G_DEFINE_TYPE (EmpathyAccountWidget, empathy_account_widget, G_TYPE_OBJECT)

enum {
  PROP_PROTOCOL = 1,
  PROP_SETTINGS,
  PROP_SIMPLE,
  PROP_CREATING_ACCOUNT,
  PROP_OTHER_ACCOUNTS_EXIST,
};

enum {
  HANDLE_APPLY,
  ACCOUNT_CREATED,
  CANCELLED,
  LAST_SIGNAL
};

static void account_widget_apply_and_log_in (EmpathyAccountWidget *);

static guint signals[LAST_SIGNAL] = { 0 };

#define GET_PRIV(obj) EMPATHY_GET_PRIV (obj, EmpathyAccountWidget)
#define CHANGED_TIMEOUT 300

static void
account_widget_set_control_buttons_sensitivity (EmpathyAccountWidget *self,
    gboolean sensitive)
{
  EmpathyAccountWidgetPriv *priv = GET_PRIV (self);

  if (!priv->simple)
    {
      /* we hit this case because of the 'other-accounts-exist' property handler
       * being called during init (before constructed()) */
      if (priv->apply_button == NULL || priv->cancel_button == NULL)
        return;

      gtk_widget_set_sensitive (priv->apply_button, sensitive);
      gtk_widget_set_sensitive (priv->cancel_button,
          (sensitive || priv->creating_account) && priv->other_accounts_exist);
    }
}

static void
account_widget_handle_control_buttons_sensitivity (EmpathyAccountWidget *self)
{
  EmpathyAccountWidgetPriv *priv = GET_PRIV (self);
  gboolean is_valid;

  is_valid = empathy_account_settings_is_valid (priv->settings);

  if (!priv->simple)
      account_widget_set_control_buttons_sensitivity (self, is_valid);

  g_signal_emit (self, signals[HANDLE_APPLY], 0, is_valid);
}

static void
account_widget_entry_changed_common (EmpathyAccountWidget *self,
    GtkEntry *entry, gboolean focus)
{
  const gchar *str;
  const gchar *param_name;
  EmpathyAccountWidgetPriv *priv = GET_PRIV (self);

  str = gtk_entry_get_text (entry);
  param_name = g_object_get_data (G_OBJECT (entry), "param_name");

  if (EMP_STR_EMPTY (str))
    {
      const gchar *value = NULL;

      empathy_account_settings_unset (priv->settings, param_name);

      if (focus)
        {
          value = empathy_account_settings_get_string (priv->settings,
              param_name);
          DEBUG ("Unset %s and restore to %s", param_name, value);
          gtk_entry_set_text (entry, value ? value : "");
        }
    }
  else
    {
      DEBUG ("Setting %s to %s", param_name,
          tp_strdiff (param_name, "password") ? str : "***");
      empathy_account_settings_set_string (priv->settings, param_name, str);
    }
}

static void
account_widget_entry_changed_cb (GtkEditable *entry,
    EmpathyAccountWidget *self)
{
  EmpathyAccountWidgetPriv *priv = GET_PRIV (self);

  if (priv->automatic_change)
    return;

  account_widget_entry_changed_common (self, GTK_ENTRY (entry), FALSE);
  empathy_account_widget_changed (self);
}

static void
account_widget_int_changed_cb (GtkWidget *widget,
    EmpathyAccountWidget *self)
{
  const gchar *param_name;
  gint value;
  const gchar *signature;
  EmpathyAccountWidgetPriv *priv = GET_PRIV (self);

  value = gtk_spin_button_get_value_as_int (GTK_SPIN_BUTTON (widget));
  param_name = g_object_get_data (G_OBJECT (widget), "param_name");

  signature = empathy_account_settings_get_dbus_signature (priv->settings,
    param_name);
  g_return_if_fail (signature != NULL);

  DEBUG ("Setting %s to %d", param_name, value);

  switch ((int)*signature)
    {
    case DBUS_TYPE_INT16:
    case DBUS_TYPE_INT32:
      empathy_account_settings_set_int32 (priv->settings, param_name, value);
      break;
    case DBUS_TYPE_INT64:
      empathy_account_settings_set_int64 (priv->settings, param_name, value);
      break;
    case DBUS_TYPE_UINT16:
    case DBUS_TYPE_UINT32:
      empathy_account_settings_set_uint32 (priv->settings, param_name, value);
      break;
    case DBUS_TYPE_UINT64:
      empathy_account_settings_set_uint64 (priv->settings, param_name, value);
      break;
    default:
      g_return_if_reached ();
    }

  empathy_account_widget_changed (self);
}

static void
account_widget_checkbutton_toggled_cb (GtkWidget *widget,
    EmpathyAccountWidget *self)
{
  gboolean     value;
  gboolean     default_value;
  const gchar *param_name;
  EmpathyAccountWidgetPriv *priv = GET_PRIV (self);

  value = gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON (widget));
  param_name = g_object_get_data (G_OBJECT (widget), "param_name");

  /* FIXME: This is ugly! checkbox don't have a "not-set" value so we
   * always unset the param and set the value if different from the
   * default value. */
  empathy_account_settings_unset (priv->settings, param_name);
  default_value = empathy_account_settings_get_boolean (priv->settings,
      param_name);

  if (default_value == value)
    {
      DEBUG ("Unset %s and restore to %d", param_name, default_value);
    }
  else
    {
      DEBUG ("Setting %s to %d", param_name, value);
      empathy_account_settings_set_boolean (priv->settings, param_name, value);
    }

  empathy_account_widget_changed (self);
}

static void
account_widget_jabber_ssl_toggled_cb (GtkWidget *checkbutton_ssl,
    EmpathyAccountWidget *self)
{
  EmpathyAccountWidgetPriv *priv = GET_PRIV (self);
  gboolean   value;
  gint32       port = 0;

  value = gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON (checkbutton_ssl));
  port = empathy_account_settings_get_uint32 (priv->settings, "port");

  if (value)
    {
      if (port == 5222 || port == 0)
        port = 5223;
    }
  else
    {
      if (port == 5223 || port == 0)
        port = 5222;
    }

  gtk_spin_button_set_value (GTK_SPIN_BUTTON (priv->spinbutton_port), port);

  priv->contains_pending_changes = TRUE;
}

static void
account_widget_combobox_changed_cb (GtkWidget *widget,
    EmpathyAccountWidget *self)
{
  GtkTreeIter iter;
  GtkTreeModel *model;
  const gchar *value;
  const GValue *v;
  const gchar *default_value = NULL;
  const gchar *param_name;
  EmpathyAccountWidgetPriv *priv = GET_PRIV (self);

  if (!gtk_combo_box_get_active_iter (GTK_COMBO_BOX (widget), &iter))
    return;

  model = gtk_combo_box_get_model (GTK_COMBO_BOX (widget));
  /* the param value is stored in the first column */
  gtk_tree_model_get (model, &iter, 0, &value, -1);

  param_name = g_object_get_data (G_OBJECT (widget), "param_name");

  v = empathy_account_settings_get_default (priv->settings, param_name);
  if (v != NULL)
    default_value = g_value_get_string (v);

  if (!tp_strdiff (value, default_value))
    {
      DEBUG ("Unset %s and restore to %s", param_name, default_value);
      empathy_account_settings_unset (priv->settings, param_name);
    }
  else
    {
      DEBUG ("Setting %s to %s", param_name, value);
      empathy_account_settings_set_string (priv->settings, param_name, value);
    }

  empathy_account_widget_changed (self);
}

static void
clear_icon_released_cb (GtkEntry *entry,
    GtkEntryIconPosition icon_pos,
    GdkEvent *event,
    EmpathyAccountWidget *self)
{
  EmpathyAccountWidgetPriv *priv = GET_PRIV (self);
  const gchar *param_name;

  param_name = g_object_get_data (G_OBJECT (entry), "param_name");

  DEBUG ("Unset %s", param_name);
  empathy_account_settings_unset (priv->settings, param_name);
  gtk_entry_set_text (entry, "");

  empathy_account_widget_changed (self);
}

static void
password_entry_changed_cb (GtkEditable *entry,
    EmpathyAccountWidget *self)
{
  const gchar *str;

  str = gtk_entry_get_text (GTK_ENTRY (entry));

  gtk_entry_set_icon_sensitive (GTK_ENTRY (entry),
      GTK_ENTRY_ICON_SECONDARY, !EMP_STR_EMPTY (str));
}

static void
password_entry_activated_cb (GtkEntry *entry,
    EmpathyAccountWidget *self)
{
    EmpathyAccountWidgetPriv *priv = GET_PRIV (self);

    if (gtk_widget_get_sensitive (priv->apply_button))
        account_widget_apply_and_log_in (self);
}

void
empathy_account_widget_setup_widget (EmpathyAccountWidget *self,
    GtkWidget *widget,
    const gchar *param_name)
{
  EmpathyAccountWidgetPriv *priv = GET_PRIV (self);

  g_object_set_data_full (G_OBJECT (widget), "param_name",
      g_strdup (param_name), g_free);

  if (GTK_IS_SPIN_BUTTON (widget))
    {
      gint value = 0;
      const gchar *signature;

      signature = empathy_account_settings_get_dbus_signature (priv->settings,
          param_name);
      g_return_if_fail (signature != NULL);

      switch ((int)*signature)
        {
          case DBUS_TYPE_INT16:
          case DBUS_TYPE_INT32:
            value = empathy_account_settings_get_int32 (priv->settings,
              param_name);
            break;
          case DBUS_TYPE_INT64:
            value = empathy_account_settings_get_int64 (priv->settings,
              param_name);
            break;
          case DBUS_TYPE_UINT16:
          case DBUS_TYPE_UINT32:
            value = empathy_account_settings_get_uint32 (priv->settings,
              param_name);
            break;
          case DBUS_TYPE_UINT64:
            value = empathy_account_settings_get_uint64 (priv->settings,
                param_name);
            break;
          default:
            g_return_if_reached ();
        }

      gtk_spin_button_set_value (GTK_SPIN_BUTTON (widget), value);

      g_signal_connect (widget, "value-changed",
          G_CALLBACK (account_widget_int_changed_cb),
          self);
    }
  else if (GTK_IS_ENTRY (widget))
    {
      const gchar *str = NULL;

      str = empathy_account_settings_get_string (priv->settings, param_name);
      gtk_entry_set_text (GTK_ENTRY (widget), str ? str : "");

      if (!tp_strdiff (param_name, "account"))
        priv->param_account_widget = widget;
      else if (!tp_strdiff (param_name, "password"))
        priv->param_password_widget = widget;

      if (strstr (param_name, "password"))
        {
          gtk_entry_set_visibility (GTK_ENTRY (widget), FALSE);

          /* Add 'clear' icon */
          gtk_entry_set_icon_from_stock (GTK_ENTRY (widget),
              GTK_ENTRY_ICON_SECONDARY, GTK_STOCK_CLEAR);

          gtk_entry_set_icon_sensitive (GTK_ENTRY (widget),
              GTK_ENTRY_ICON_SECONDARY, !EMP_STR_EMPTY (str));

          g_signal_connect (widget, "icon-release",
              G_CALLBACK (clear_icon_released_cb), self);
          g_signal_connect (widget, "changed",
              G_CALLBACK (password_entry_changed_cb), self);
          g_signal_connect (widget, "activate",
              G_CALLBACK (password_entry_activated_cb), self);
        }

      g_signal_connect (widget, "changed",
          G_CALLBACK (account_widget_entry_changed_cb), self);
    }
  else if (GTK_IS_TOGGLE_BUTTON (widget))
    {
      gboolean value = FALSE;

      value = empathy_account_settings_get_boolean (priv->settings,
          param_name);
      gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (widget), value);

      g_signal_connect (widget, "toggled",
          G_CALLBACK (account_widget_checkbutton_toggled_cb),
          self);
    }
  else if (GTK_IS_COMBO_BOX (widget))
    {
      /* The combo box's model has to contain the param value in its first
       * column (as a string) */
      const gchar *str;
      GtkTreeModel *model;
      GtkTreeIter iter;
      gboolean valid;

      str = empathy_account_settings_get_string (priv->settings, param_name);
      model = gtk_combo_box_get_model (GTK_COMBO_BOX (widget));

      valid = gtk_tree_model_get_iter_first (model, &iter);
      while (valid)
        {
          gchar *name;

          gtk_tree_model_get (model, &iter, 0, &name, -1);
          if (!tp_strdiff (name, str))
            {
              gtk_combo_box_set_active_iter (GTK_COMBO_BOX (widget), &iter);
              valid = FALSE;
            }
          else
            {
              valid = gtk_tree_model_iter_next (model, &iter);
            }

          g_free (name);
        }

      g_signal_connect (widget, "changed",
          G_CALLBACK (account_widget_combobox_changed_cb),
          self);
    }
  else
    {
      DEBUG ("Unknown type of widget for param %s", param_name);
    }
}

static GHashTable *
build_translated_params (void)
{
  GHashTable *hash;

  hash = g_hash_table_new (g_str_hash, g_str_equal);
  g_hash_table_insert (hash, "account", _("Account"));
  g_hash_table_insert (hash, "password", _("Password"));
  g_hash_table_insert (hash, "server", _("Server"));
  g_hash_table_insert (hash, "port", _("Port"));

  return hash;
}

static gchar *
account_widget_generic_format_param_name (const gchar *param_name)
{
  gchar *str;
  gchar *p;
  static GHashTable *translated_params = NULL;

  if (G_UNLIKELY (translated_params == NULL))
    translated_params = build_translated_params ();

  /* Translate most common parameters */
  str = g_hash_table_lookup (translated_params, param_name);
  if (str != NULL)
    return g_strdup (str);

  str = g_strdup (param_name);

  if (str && g_ascii_isalpha (str[0]))
    str[0] = g_ascii_toupper (str[0]);

  while ((p = strchr (str, '-')) != NULL)
    {
      if (p[1] != '\0' && g_ascii_isalpha (p[1]))
        {
          p[0] = ' ';
          p[1] = g_ascii_toupper (p[1]);
        }

      p++;
    }

  return str;
}

static void
accounts_widget_generic_setup (EmpathyAccountWidget *self,
    GtkWidget *table_common_settings,
    GtkWidget *table_advanced_settings)
{
  TpConnectionManagerParam *params, *param;
  EmpathyAccountWidgetPriv *priv = GET_PRIV (self);

  params = empathy_account_settings_get_tp_params (priv->settings);

  for (param = params; param != NULL && param->name != NULL; param++)
    {
      GtkWidget       *table_settings;
      guint            n_rows = 0;
      GtkWidget       *widget = NULL;
      gchar           *param_name_formatted;

      if (param->flags & TP_CONN_MGR_PARAM_FLAG_REQUIRED)
        table_settings = table_common_settings;
      else if (priv->simple)
        return;
      else
        table_settings = table_advanced_settings;

      param_name_formatted = account_widget_generic_format_param_name
        (param->name);
      g_object_get (table_settings, "n-rows", &n_rows, NULL);
      gtk_table_resize (GTK_TABLE (table_settings), ++n_rows, 2);

      if (param->dbus_signature[0] == 's')
        {
          gchar *str;

          str = g_strdup_printf (_("%s:"), param_name_formatted);
          widget = gtk_label_new (str);
          gtk_misc_set_alignment (GTK_MISC (widget), 0, 0.5);
          g_free (str);

          gtk_table_attach (GTK_TABLE (table_settings),
              widget,
              0, 1,
              n_rows - 1, n_rows,
              GTK_FILL, 0,
              0, 0);
          gtk_widget_show (widget);

          widget = gtk_entry_new ();
          if (strcmp (param->name, "account") == 0)
            {
              g_signal_connect (widget, "realize",
                  G_CALLBACK (gtk_widget_grab_focus),
                  NULL);
            }
          gtk_table_attach (GTK_TABLE (table_settings),
              widget,
              1, 2,
              n_rows - 1, n_rows,
              GTK_FILL | GTK_EXPAND, 0,
              0, 0);
          gtk_widget_show (widget);
        }
      /* int types: ynqiuxt. double type is 'd' */
      else if (param->dbus_signature[0] == 'y' ||
          param->dbus_signature[0] == 'n' ||
          param->dbus_signature[0] == 'q' ||
          param->dbus_signature[0] == 'i' ||
          param->dbus_signature[0] == 'u' ||
          param->dbus_signature[0] == 'x' ||
          param->dbus_signature[0] == 't' ||
          param->dbus_signature[0] == 'd')
        {
          gchar   *str = NULL;
          gdouble  minint = 0;
          gdouble  maxint = 0;
          gdouble  step = 1;

          switch (param->dbus_signature[0])
            {
            case 'y': minint = G_MININT8;  maxint = G_MAXINT8;   break;
            case 'n': minint = G_MININT16; maxint = G_MAXINT16;  break;
            case 'q': minint = 0;          maxint = G_MAXUINT16; break;
            case 'i': minint = G_MININT32; maxint = G_MAXINT32;  break;
            case 'u': minint = 0;          maxint = G_MAXUINT32; break;
            case 'x': minint = G_MININT64; maxint = G_MAXINT64;  break;
            case 't': minint = 0;          maxint = G_MAXUINT64; break;
            case 'd': minint = G_MININT32; maxint = G_MAXINT32;
              step = 0.1; break;
            default: g_assert_not_reached ();
            }

          str = g_strdup_printf (_("%s:"), param_name_formatted);
          widget = gtk_label_new (str);
          gtk_misc_set_alignment (GTK_MISC (widget), 0, 0.5);
          g_free (str);

          gtk_table_attach (GTK_TABLE (table_settings),
              widget,
              0, 1,
              n_rows - 1, n_rows,
              GTK_FILL, 0,
              0, 0);
          gtk_widget_show (widget);

          widget = gtk_spin_button_new_with_range (minint, maxint, step);
          gtk_table_attach (GTK_TABLE (table_settings),
              widget,
              1, 2,
              n_rows - 1, n_rows,
              GTK_FILL | GTK_EXPAND, 0,
              0, 0);
          gtk_widget_show (widget);
        }
      else if (param->dbus_signature[0] == 'b')
        {
          widget = gtk_check_button_new_with_label (param_name_formatted);
          gtk_table_attach (GTK_TABLE (table_settings),
              widget,
              0, 2,
              n_rows - 1, n_rows,
              GTK_FILL | GTK_EXPAND, 0,
              0, 0);
          gtk_widget_show (widget);
        }
      else
        {
          DEBUG ("Unknown signature for param %s: %s",
              param_name_formatted, param->dbus_signature);
        }

      if (widget)
        empathy_account_widget_setup_widget (self, widget, param->name);

      g_free (param_name_formatted);
    }
}

static void
account_widget_handle_params_valist (EmpathyAccountWidget *self,
    const gchar *first_widget,
    va_list args)
{
  GObject *object;
  const gchar *name;

  for (name = first_widget; name; name = va_arg (args, const gchar *))
    {
      const gchar *param_name;

      param_name = va_arg (args, const gchar *);
      object = gtk_builder_get_object (self->ui_details->gui, name);

      if (!object)
        {
          g_warning ("Builder is missing object '%s'.", name);
          continue;
        }

      empathy_account_widget_setup_widget (self, GTK_WIDGET (object),
          param_name);
    }
}

static void
account_widget_cancel_clicked_cb (GtkWidget *button,
    EmpathyAccountWidget *self)
{
  g_signal_emit (self, signals[CANCELLED], 0);
}

static void
account_widget_account_enabled_cb (GObject *source_object,
    GAsyncResult *res,
    gpointer user_data)
{
  GError *error = NULL;
  TpAccount *account = TP_ACCOUNT (source_object);
  EmpathyAccountWidget *widget = EMPATHY_ACCOUNT_WIDGET (user_data);
  EmpathyAccountWidgetPriv *priv = GET_PRIV (widget);

  tp_account_set_enabled_finish (account, res, &error);

  if (error != NULL)
    {
      DEBUG ("Could not enable the account: %s", error->message);
      g_error_free (error);
    }
  else
    {
      empathy_connect_new_account (account, priv->account_manager);
    }

  /* unref widget - part of the workaround */
  g_object_unref (widget);
}

static void
account_widget_applied_cb (GObject *source_object,
    GAsyncResult *res,
    gpointer user_data)
{
  GError *error = NULL;
  TpAccount *account;
  EmpathyAccountSettings *settings = EMPATHY_ACCOUNT_SETTINGS (source_object);
  EmpathyAccountWidget *widget = EMPATHY_ACCOUNT_WIDGET (user_data);
  EmpathyAccountWidgetPriv *priv = GET_PRIV (widget);
  gboolean reconnect_required;

  empathy_account_settings_apply_finish (settings, res, &reconnect_required,
      &error);

  if (error != NULL)
    {
      DEBUG ("Could not apply changes to account: %s", error->message);
      g_error_free (error);
      return;
    }

  account = empathy_account_settings_get_account (priv->settings);

  if (account != NULL)
    {
      if (priv->creating_account)
        {
          /* By default, when an account is created, we enable it. */

          /* workaround to keep widget alive during async call */
          g_object_ref (widget);

          tp_account_set_enabled_async (account, TRUE,
              account_widget_account_enabled_cb, widget);
          g_signal_emit (widget, signals[ACCOUNT_CREATED], 0, account);
        }
      else if (priv->enabled_checkbox != NULL)
        {
          gboolean enabled_checked;

          enabled_checked =
#ifdef HAVE_MEEGO
            mx_gtk_light_switch_get_active (
                MX_GTK_LIGHT_SWITCH (priv->enabled_checkbox));
#else
            gtk_toggle_button_get_active (
                GTK_TOGGLE_BUTTON (priv->enabled_checkbox));
#endif /* HAVE_MEEGO */

          /* If the account was offline, we always want to try reconnecting,
           * to give it a chance to connect if the previous params were wrong.
           * tp_account_reconnect_async() won't do anything if the requested
           * presence is offline anyway. */
          if (tp_account_get_connection_status (account, NULL) ==
              TP_CONNECTION_STATUS_DISCONNECTED)
            reconnect_required = TRUE;

          if (reconnect_required && tp_account_is_enabled (account)
              && enabled_checked)
            {
              /* After having applied changes to a user account, we
               * reconnect it if needed. This is done so the new
               * information entered by the user is validated on the server. */
              tp_account_reconnect_async (account, NULL, NULL);
            }
        }
    }

  if (!priv->destroyed)
    account_widget_set_control_buttons_sensitivity (widget, FALSE);

  priv->contains_pending_changes = FALSE;

  /* unref the widget - part of the workaround */
  g_object_unref (widget);
}

static void
account_widget_apply_and_log_in (EmpathyAccountWidget *self)
{
  EmpathyAccountWidgetPriv *priv = GET_PRIV (self);
  gboolean display_name_overridden;

  if (priv->radiobutton_reuse != NULL)
    {
      gboolean reuse = gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON (
            priv->radiobutton_reuse));

      DEBUG ("Set register param: %d", !reuse);
      empathy_account_settings_set_boolean (priv->settings, "register", !reuse);
    }

  g_object_get (priv->settings,
      "display-name-overridden", &display_name_overridden, NULL);

  if (priv->creating_account || !display_name_overridden)
    {
      gchar *display_name;

      /* set default display name for new accounts or update if user didn't
       * manually override it. */
      display_name = empathy_account_widget_get_default_display_name (self);

      empathy_account_settings_set_display_name_async (priv->settings,
          display_name, NULL, NULL);

      g_free (display_name);
    }

  /* workaround to keep widget alive during async call */
  g_object_ref (self);
  empathy_account_settings_apply_async (priv->settings,
      account_widget_applied_cb, self);
}

static void
account_widget_apply_clicked_cb (GtkWidget *button,
    EmpathyAccountWidget *self)
{
    account_widget_apply_and_log_in (self);
}

static void
account_widget_setup_generic (EmpathyAccountWidget *self)
{
  GtkWidget *table_common_settings;
  GtkWidget *table_advanced_settings;

  table_common_settings = GTK_WIDGET (gtk_builder_get_object
      (self->ui_details->gui, "table_common_settings"));
  table_advanced_settings = GTK_WIDGET (gtk_builder_get_object
      (self->ui_details->gui, "table_advanced_settings"));

  accounts_widget_generic_setup (self, table_common_settings,
      table_advanced_settings);

  g_object_unref (self->ui_details->gui);
}

static void
account_widget_settings_ready_cb (EmpathyAccountSettings *settings,
    GParamSpec *pspec,
    gpointer user_data)
{
  EmpathyAccountWidget *self = user_data;
  EmpathyAccountWidgetPriv *priv = GET_PRIV (self);

  if (empathy_account_settings_is_ready (priv->settings))
    account_widget_setup_generic (self);
}

static void
account_widget_build_generic (EmpathyAccountWidget *self,
    const char *filename)
{
  EmpathyAccountWidgetPriv *priv = GET_PRIV (self);
  GtkWidget *expander_advanced;

  self->ui_details->gui = empathy_builder_get_file (filename,
      "table_common_settings", &priv->table_common_settings,
      "vbox_generic_settings", &self->ui_details->widget,
      "expander_advanced_settings", &expander_advanced,
      NULL);

  if (priv->simple)
    gtk_widget_hide (expander_advanced);

  g_object_ref (self->ui_details->gui);

  if (empathy_account_settings_is_ready (priv->settings))
    account_widget_setup_generic (self);
  else
    g_signal_connect (priv->settings, "notify::ready",
        G_CALLBACK (account_widget_settings_ready_cb), self);
}

static void
account_widget_build_salut (EmpathyAccountWidget *self,
    const char *filename)
{
  EmpathyAccountWidgetPriv *priv = GET_PRIV (self);
  GtkWidget *expander_advanced;

  self->ui_details->gui = empathy_builder_get_file (filename,
      "table_common_settings", &priv->table_common_settings,
      "vbox_salut_settings", &self->ui_details->widget,
      "expander_advanced_settings", &expander_advanced,
      NULL);

  empathy_account_widget_handle_params (self,
      "entry_published", "published-name",
      "entry_nickname", "nickname",
      "entry_first_name", "first-name",
      "entry_last_name", "last-name",
      "entry_email", "email",
      "entry_jid", "jid",
      NULL);

  if (priv->simple)
    gtk_widget_hide (expander_advanced);

  self->ui_details->default_focus = g_strdup ("entry_first_name");
}

static void
account_widget_build_irc (EmpathyAccountWidget *self,
  const char *filename)
{
  EmpathyAccountWidgetPriv *priv = GET_PRIV (self);

  if (priv->simple)
    {
      priv->irc_network_chooser = empathy_account_widget_irc_build_simple (self,
          filename);
    }
  else
    {
      priv->irc_network_chooser = empathy_account_widget_irc_build (self,
          filename, &priv->table_common_settings);
    }
}

static void
account_widget_build_sip (EmpathyAccountWidget *self,
  const char *filename)
{
  EmpathyAccountWidgetPriv *priv = GET_PRIV (self);
  empathy_account_widget_sip_build (self, filename,
    &priv->table_common_settings);

  if (priv->simple)
    {
      priv->remember_password_widget = GTK_WIDGET (gtk_builder_get_object (
              self->ui_details->gui, "remember_password_simple"));
    }
  else
    {
      priv->remember_password_widget = GTK_WIDGET (gtk_builder_get_object (
              self->ui_details->gui, "remember_password"));
    }
}

static void
account_widget_build_msn (EmpathyAccountWidget *self,
    const char *filename)
{
  EmpathyAccountWidgetPriv *priv = GET_PRIV (self);

  if (priv->simple)
    {
      self->ui_details->gui = empathy_builder_get_file (filename,
          "vbox_msn_simple", &self->ui_details->widget,
          NULL);

      empathy_account_widget_handle_params (self,
          "entry_id_simple", "account",
          "entry_password_simple", "password",
          NULL);

      self->ui_details->default_focus = g_strdup ("entry_id_simple");

      priv->remember_password_widget = GTK_WIDGET (gtk_builder_get_object (
              self->ui_details->gui, "remember_password_simple"));
    }
  else
    {
      self->ui_details->gui = empathy_builder_get_file (filename,
          "table_common_msn_settings", &priv->table_common_settings,
          "vbox_msn_settings", &self->ui_details->widget,
          NULL);

      empathy_account_widget_handle_params (self,
          "entry_id", "account",
          "entry_password", "password",
          "entry_server", "server",
          "spinbutton_port", "port",
          NULL);

      self->ui_details->default_focus = g_strdup ("entry_id");

      priv->remember_password_widget = GTK_WIDGET (gtk_builder_get_object (
              self->ui_details->gui, "remember_password"));
    }
}

static gboolean
account_widget_is_gtalk (EmpathyAccountWidget *self)
{
  EmpathyAccountWidgetPriv *priv = GET_PRIV (self);

  return !tp_strdiff (empathy_account_settings_get_icon_name (priv->settings),
      "im-google-talk");
}

static gboolean
account_widget_is_facebook (EmpathyAccountWidget *self)
{
  EmpathyAccountWidgetPriv *priv = GET_PRIV (self);

  return !tp_strdiff (empathy_account_settings_get_icon_name (priv->settings),
      "im-facebook");
}

#define FACEBOOK_SUFFIX "@chat.facebook.com"

static void
facebook_id_widget_changed_cb (GtkWidget *entry,
    EmpathyAccountWidget *self)
{
  EmpathyAccountWidgetPriv *priv = GET_PRIV (self);
  const gchar *account;

  account_widget_entry_changed_common (self, GTK_ENTRY (entry), FALSE);

  account = empathy_account_settings_get_string (priv->settings, "account");
  if (!EMP_STR_EMPTY (account) &&
      !g_str_has_suffix (account, FACEBOOK_SUFFIX))
    {
      gchar *tmp;

      tmp = g_strdup_printf ("%s%s", account, FACEBOOK_SUFFIX);

      DEBUG ("Change account from '%s' to '%s'", account, tmp);

      empathy_account_settings_set_string (priv->settings, "account", tmp);
      g_free (tmp);
    }

  empathy_account_widget_changed (self);
}

static gchar *
remove_facebook_suffix (const gchar *str)
{
  if (!g_str_has_suffix (str, FACEBOOK_SUFFIX))
    return g_strdup (str);

  return g_strndup (str, strlen (str) - strlen (FACEBOOK_SUFFIX));
}

static void
setup_facebook_id_widget (EmpathyAccountWidget *self,
    GtkWidget *widget)
{
  EmpathyAccountWidgetPriv *priv = GET_PRIV (self);
  const gchar *str = NULL;

  g_object_set_data_full (G_OBJECT (widget), "param_name",
      g_strdup ("account"), g_free);

  str = empathy_account_settings_get_string (priv->settings, "account");
  if (str != NULL)
    {
      gchar *tmp;

      tmp = remove_facebook_suffix (str);
      gtk_entry_set_text (GTK_ENTRY (widget), tmp);
      g_free (tmp);
    }

  priv->param_account_widget = widget;

  g_signal_connect (widget, "changed",
      G_CALLBACK (facebook_id_widget_changed_cb), self);
}

static void
account_widget_build_jabber (EmpathyAccountWidget *self,
    const char *filename)
{
  EmpathyAccountWidgetPriv *priv = GET_PRIV (self);
  GtkWidget *spinbutton_port;
  GtkWidget *checkbutton_ssl;
  GtkWidget *label_id, *label_password;
  GtkWidget *label_id_create, *label_password_create;
  GtkWidget *label_example_gtalk, *label_example_jabber, *label_example_fb;
  gboolean is_gtalk, is_facebook;
  GtkWidget *expander_advanced;
  GtkWidget *entry_id;

  is_gtalk = account_widget_is_gtalk (self);
  is_facebook = account_widget_is_facebook (self);

  if (priv->simple && !is_gtalk && !is_facebook)
    {
      /* Simple widget for XMPP */
      self->ui_details->gui = empathy_builder_get_file (filename,
          "vbox_jabber_simple", &self->ui_details->widget,
          "label_id_simple", &label_id,
          "label_id_create", &label_id_create,
          "label_password_simple", &label_password,
          "label_password_create", &label_password_create,
          NULL);

      if (empathy_account_settings_get_boolean (priv->settings, "register"))
        {
          gtk_widget_hide (label_id);
          gtk_widget_hide (label_password);
          gtk_widget_show (label_id_create);
          gtk_widget_show (label_password_create);
        }

      empathy_account_widget_handle_params (self,
          "entry_id_simple", "account",
          "entry_password_simple", "password",
          NULL);

      self->ui_details->default_focus = g_strdup ("entry_id_simple");

      priv->remember_password_widget = GTK_WIDGET (gtk_builder_get_object (
              self->ui_details->gui, "remember_password_simple"));
    }
  else if (priv->simple && is_gtalk)
    {
      /* Simple widget for Google Talk */
      self->ui_details->gui = empathy_builder_get_file (filename,
          "vbox_gtalk_simple", &self->ui_details->widget,
          NULL);

      empathy_account_widget_handle_params (self,
          "entry_id_g_simple", "account",
          "entry_password_g_simple", "password",
          NULL);

      self->ui_details->default_focus = g_strdup ("entry_id_g_simple");

      priv->remember_password_widget = GTK_WIDGET (gtk_builder_get_object (
              self->ui_details->gui, "remember_password_g_simple"));
    }
  else if (priv->simple && is_facebook)
    {
      /* Simple widget for Facebook */
      self->ui_details->gui = empathy_builder_get_file (filename,
          "vbox_fb_simple", &self->ui_details->widget,
          "entry_id_fb_simple", &entry_id,
          NULL);

      empathy_account_widget_handle_params (self,
          "entry_password_fb_simple", "password",
          NULL);

      setup_facebook_id_widget (self, entry_id);

      self->ui_details->default_focus = g_strdup ("entry_id_fb_simple");

      priv->remember_password_widget = GTK_WIDGET (gtk_builder_get_object (
              self->ui_details->gui, "remember_password_fb_simple"));
    }
  else
    {
      /* Full widget for XMPP, Google Talk and Facebook*/
      self->ui_details->gui = empathy_builder_get_file (filename,
          "table_common_settings", &priv->table_common_settings,
          "vbox_jabber_settings", &self->ui_details->widget,
          "spinbutton_port", &spinbutton_port,
          "checkbutton_ssl", &checkbutton_ssl,
          "label_username_example", &label_example_jabber,
          "label_username_g_example", &label_example_gtalk,
          "label_username_f_example", &label_example_fb,
          "expander_advanced", &expander_advanced,
          "entry_id", &entry_id,
          "label_id", &label_id,
          NULL);

      empathy_account_widget_handle_params (self,
          "entry_password", "password",
          "entry_resource", "resource",
          "entry_server", "server",
          "spinbutton_port", "port",
          "spinbutton_priority", "priority",
          "checkbutton_ssl", "old-ssl",
          "checkbutton_ignore_ssl_errors", "ignore-ssl-errors",
          "checkbutton_encryption", "require-encryption",
          NULL);

      if (is_facebook)
        {
          gtk_label_set_label (GTK_LABEL (label_id), _("Username:"));

          /* Facebook special case the entry ID widget to hide the
           * "@chat.facebook.com" part */
          setup_facebook_id_widget (self, entry_id);
        }
      else
        {
          empathy_account_widget_setup_widget (self, entry_id, "account");
        }

      self->ui_details->default_focus = g_strdup ("entry_id");
      priv->spinbutton_port = spinbutton_port;

      priv->remember_password_widget = GTK_WIDGET (gtk_builder_get_object (
              self->ui_details->gui, "remember_password"));

      g_signal_connect (checkbutton_ssl, "toggled",
          G_CALLBACK (account_widget_jabber_ssl_toggled_cb),
          self);

      if (is_gtalk)
        {
          gtk_widget_hide (label_example_jabber);
          gtk_widget_show (label_example_gtalk);
        }
      else if (is_facebook)
        {
          gtk_widget_hide (label_example_jabber);
          gtk_widget_show (label_example_fb);
          gtk_widget_hide (expander_advanced);
        }
    }
}

static void
account_widget_build_icq (EmpathyAccountWidget *self,
    const char *filename)
{
  EmpathyAccountWidgetPriv *priv = GET_PRIV (self);
  GtkWidget *spinbutton_port;

  if (priv->simple)
    {
      self->ui_details->gui = empathy_builder_get_file (filename,
          "vbox_icq_simple", &self->ui_details->widget,
          NULL);

      empathy_account_widget_handle_params (self,
          "entry_uin_simple", "account",
          "entry_password_simple", "password",
          NULL);

      self->ui_details->default_focus = g_strdup ("entry_uin_simple");

      priv->remember_password_widget = GTK_WIDGET (gtk_builder_get_object (
              self->ui_details->gui, "remember_password_simple"));
    }
  else
    {
      self->ui_details->gui = empathy_builder_get_file (filename,
          "table_common_settings", &priv->table_common_settings,
          "vbox_icq_settings", &self->ui_details->widget,
          "spinbutton_port", &spinbutton_port,
          NULL);

      empathy_account_widget_handle_params (self,
          "entry_uin", "account",
          "entry_password", "password",
          "entry_server", "server",
          "spinbutton_port", "port",
          "entry_charset", "charset",
          NULL);

      self->ui_details->default_focus = g_strdup ("entry_uin");

      priv->remember_password_widget = GTK_WIDGET (gtk_builder_get_object (
              self->ui_details->gui, "remember_password"));
    }
}

static void
account_widget_build_aim (EmpathyAccountWidget *self,
    const char *filename)
{
  EmpathyAccountWidgetPriv *priv = GET_PRIV (self);
  GtkWidget *spinbutton_port;

  if (priv->simple)
    {
      self->ui_details->gui = empathy_builder_get_file (filename,
          "vbox_aim_simple", &self->ui_details->widget,
          NULL);

      empathy_account_widget_handle_params (self,
          "entry_screenname_simple", "account",
          "entry_password_simple", "password",
          NULL);

      self->ui_details->default_focus = g_strdup ("entry_screenname_simple");

      priv->remember_password_widget = GTK_WIDGET (gtk_builder_get_object (
              self->ui_details->gui, "remember_password_simple"));
    }
  else
    {
      self->ui_details->gui = empathy_builder_get_file (filename,
          "table_common_settings", &priv->table_common_settings,
          "vbox_aim_settings", &self->ui_details->widget,
          "spinbutton_port", &spinbutton_port,
          NULL);

      empathy_account_widget_handle_params (self,
          "entry_screenname", "account",
          "entry_password", "password",
          "entry_server", "server",
          "spinbutton_port", "port",
          NULL);

      self->ui_details->default_focus = g_strdup ("entry_screenname");

      priv->remember_password_widget = GTK_WIDGET (gtk_builder_get_object (
              self->ui_details->gui, "remember_password"));
    }
}

static void
account_widget_build_yahoo (EmpathyAccountWidget *self,
    const char *filename)
{
  EmpathyAccountWidgetPriv *priv = GET_PRIV (self);

  if (priv->simple)
    {
      self->ui_details->gui = empathy_builder_get_file (filename,
          "vbox_yahoo_simple", &self->ui_details->widget,
          NULL);

      empathy_account_widget_handle_params (self,
          "entry_id_simple", "account",
          "entry_password_simple", "password",
          NULL);

      self->ui_details->default_focus = g_strdup ("entry_id_simple");

      priv->remember_password_widget = GTK_WIDGET (gtk_builder_get_object (
              self->ui_details->gui, "remember_password_simple"));
    }
  else
    {
      self->ui_details->gui = empathy_builder_get_file (filename,
          "table_common_settings", &priv->table_common_settings,
          "vbox_yahoo_settings", &self->ui_details->widget,
          NULL);

      empathy_account_widget_handle_params (self,
          "entry_id", "account",
          "entry_password", "password",
          "entry_locale", "room-list-locale",
          "entry_charset", "charset",
          "spinbutton_port", "port",
          "checkbutton_ignore_invites", "ignore-invites",
          NULL);

      self->ui_details->default_focus = g_strdup ("entry_id");

      priv->remember_password_widget = GTK_WIDGET (gtk_builder_get_object (
              self->ui_details->gui, "remember_password"));
    }
}

static void
account_widget_build_groupwise (EmpathyAccountWidget *self,
    const char *filename)
{
  EmpathyAccountWidgetPriv *priv = GET_PRIV (self);

  if (priv->simple)
    {
      self->ui_details->gui = empathy_builder_get_file (filename,
          "vbox_groupwise_simple", &self->ui_details->widget,
          NULL);

      empathy_account_widget_handle_params (self,
          "entry_id_simple", "account",
          "entry_password_simple", "password",
          NULL);

      self->ui_details->default_focus = g_strdup ("entry_id_simple");

      priv->remember_password_widget = GTK_WIDGET (gtk_builder_get_object (
              self->ui_details->gui, "remember_password_simple"));
    }
  else
    {
      self->ui_details->gui = empathy_builder_get_file (filename,
          "table_common_groupwise_settings", &priv->table_common_settings,
          "vbox_groupwise_settings", &self->ui_details->widget,
          NULL);

      empathy_account_widget_handle_params (self,
          "entry_id", "account",
          "entry_password", "password",
          "entry_server", "server",
          "spinbutton_port", "port",
          NULL);

      self->ui_details->default_focus = g_strdup ("entry_id");

      priv->remember_password_widget = GTK_WIDGET (gtk_builder_get_object (
              self->ui_details->gui, "remember_password"));
    }
}

gboolean
account_widget_skype_show_eula (GtkWindow *parent)
{
  GtkWidget *dialog, *textview, *vbox, *scrolledwindow;
  GtkTextBuffer *buffer;
  gchar *filename = empathy_file_lookup ("skype-eula.txt", "data");
  gchar *eula;
  gint result;
  gsize len;
  GError *error = NULL;

  g_file_get_contents (filename, &eula, &len, &error);
  g_free (filename);

  if (error != NULL)
    {
      g_warning ("Could not open Skype EULA: %s", error->message);
      g_error_free (error);
      return FALSE;
    }

  dialog = gtk_dialog_new_with_buttons (_("End User License Agreement"),
      parent, GTK_DIALOG_MODAL,
      _("Decline"), GTK_RESPONSE_CANCEL,
      _("Accept"), GTK_RESPONSE_ACCEPT,
      NULL);

  buffer = gtk_text_buffer_new (NULL);
  gtk_text_buffer_set_text (buffer, eula, len);
  g_free (eula);
  textview = gtk_text_view_new_with_buffer (buffer);
  gtk_text_view_set_editable (GTK_TEXT_VIEW (textview), FALSE);
  gtk_text_view_set_wrap_mode (GTK_TEXT_VIEW (textview), GTK_WRAP_WORD_CHAR);

  vbox = gtk_dialog_get_content_area (GTK_DIALOG (dialog));

  scrolledwindow = gtk_scrolled_window_new (NULL, NULL);
  gtk_scrolled_window_add_with_viewport (
      GTK_SCROLLED_WINDOW (scrolledwindow), textview);
  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scrolledwindow),
      GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);

  gtk_box_pack_start (GTK_BOX (vbox), scrolledwindow, TRUE, TRUE, 0);
  gtk_window_set_default_size (GTK_WINDOW (dialog), 400, 250);
  gtk_widget_show_all (dialog);

  result = gtk_dialog_run (GTK_DIALOG (dialog));

  gtk_widget_destroy (dialog);

  return (result == GTK_RESPONSE_ACCEPT);
}

static void
account_widget_build_skype (EmpathyAccountWidget *self,
    const char *filename)
{
  empathy_account_widget_build_skype (self, filename);
}


static void
account_widget_destroy_cb (GtkWidget *widget,
    EmpathyAccountWidget *self)
{
  EmpathyAccountWidgetPriv *priv = GET_PRIV (self);
  /* set the destroyed flag - workaround */
  priv->destroyed = TRUE;

  g_object_unref (self);
}

static void
empathy_account_widget_enabled_cb (TpAccount *account,
      GParamSpec *spec,
      gpointer user_data)
{
  EmpathyAccountWidget *widget = EMPATHY_ACCOUNT_WIDGET (user_data);
  EmpathyAccountWidgetPriv *priv = GET_PRIV (widget);
  gboolean enabled = tp_account_is_enabled (account);

  if (priv->enabled_checkbox != NULL)
    {
#ifdef HAVE_MEEGO
      mx_gtk_light_switch_set_active (
          MX_GTK_LIGHT_SWITCH (priv->enabled_checkbox),
          enabled);
#else
      gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (priv->enabled_checkbox),
          enabled);
#endif /* HAVE_MEEGO */
    }
}

static void
#ifdef HAVE_MEEGO
account_widget_switch_flipped_cb (MxGtkLightSwitch *sw,
    gboolean state,
    gpointer user_data)
#else
account_widget_enabled_toggled_cb (GtkToggleButton *toggle_button,
    gpointer user_data)
#endif /* HAVE_MEEGO */
{
  EmpathyAccountWidgetPriv *priv = GET_PRIV (user_data);
  TpAccount *account;
#ifndef HAVE_MEEGO
  gboolean state;

  state = gtk_toggle_button_get_active (toggle_button);
#endif /* HAVE_MEEGO */

  account = empathy_account_settings_get_account (priv->settings);

  /* Enable the account according to the value of the "Enabled" checkbox */
  /* workaround to keep widget alive during async call */
  g_object_ref (user_data);
  tp_account_set_enabled_async (account, state,
      account_widget_account_enabled_cb, user_data);
}

void
empathy_account_widget_set_other_accounts_exist (EmpathyAccountWidget *self,
    gboolean others_exist)
{
  EmpathyAccountWidgetPriv *priv = GET_PRIV (self);

  priv->other_accounts_exist = others_exist;

  if (priv->creating_account)
    account_widget_handle_control_buttons_sensitivity (self);
}

static void
do_set_property (GObject *object,
    guint prop_id,
    const GValue *value,
    GParamSpec *pspec)
{
  EmpathyAccountWidgetPriv *priv = GET_PRIV (object);

  switch (prop_id)
    {
    case PROP_SETTINGS:
      priv->settings = g_value_dup_object (value);
      break;
    case PROP_SIMPLE:
      priv->simple = g_value_get_boolean (value);
      break;
    case PROP_CREATING_ACCOUNT:
      priv->creating_account = g_value_get_boolean (value);
      break;
    case PROP_OTHER_ACCOUNTS_EXIST:
      empathy_account_widget_set_other_accounts_exist (
          EMPATHY_ACCOUNT_WIDGET (object), g_value_get_boolean (value));
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
do_get_property (GObject *object,
    guint prop_id,
    GValue *value,
    GParamSpec *pspec)
{
  EmpathyAccountWidgetPriv *priv = GET_PRIV (object);

  switch (prop_id)
    {
    case PROP_PROTOCOL:
      g_value_set_string (value,
        empathy_account_settings_get_protocol (priv->settings));
      break;
    case PROP_SETTINGS:
      g_value_set_object (value, priv->settings);
      break;
    case PROP_SIMPLE:
      g_value_set_boolean (value, priv->simple);
      break;
    case PROP_CREATING_ACCOUNT:
      g_value_set_boolean (value, priv->creating_account);
      break;
    case PROP_OTHER_ACCOUNTS_EXIST:
      g_value_set_boolean (value, priv->other_accounts_exist);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
set_apply_button (EmpathyAccountWidget *self)
{
  EmpathyAccountWidgetPriv *priv = GET_PRIV (self);
  GtkWidget *image;

  /* We can't use the stock button as its accelerator ('A') clashes with the
   * Add button. */
  gtk_button_set_use_stock (GTK_BUTTON (priv->apply_button), FALSE);

  gtk_button_set_label (GTK_BUTTON (priv->apply_button), _("A_pply"));
  gtk_button_set_use_underline (GTK_BUTTON (priv->apply_button), TRUE);

  image = gtk_image_new_from_stock (GTK_STOCK_APPLY, GTK_ICON_SIZE_BUTTON);
  gtk_button_set_image (GTK_BUTTON (priv->apply_button), image);
}

static void
presence_changed_cb (TpAccountManager *manager,
    TpConnectionPresenceType state,
    const gchar *status,
    const gchar *message,
    EmpathyAccountWidget *self)
{
  EmpathyAccountWidgetPriv *priv = GET_PRIV (self);

  if (priv->destroyed)
    return;

  if (priv->apply_button == NULL)
    /* This button doesn't exist in 'simple' mode */
    return;

  if (state > TP_CONNECTION_PRESENCE_TYPE_OFFLINE &&
      priv->creating_account)
    {
      /* We are online and creating a new account, display a Login button */
      GtkWidget *image;

      gtk_button_set_use_stock (GTK_BUTTON (priv->apply_button), FALSE);
      gtk_button_set_label (GTK_BUTTON (priv->apply_button), _("L_og in"));

      image = gtk_image_new_from_stock (GTK_STOCK_CONNECT,
          GTK_ICON_SIZE_BUTTON);
      gtk_button_set_image (GTK_BUTTON (priv->apply_button), image);
    }
  else
    {
      /* We are offline or modifying an existing account, display
       * a Save button */
      set_apply_button (self);
    }
}

static void
account_manager_ready_cb (GObject *source_object,
    GAsyncResult *result,
    gpointer user_data)
{
  EmpathyAccountWidget *self = EMPATHY_ACCOUNT_WIDGET (user_data);
  TpAccountManager *account_manager = TP_ACCOUNT_MANAGER (source_object);
  GError *error = NULL;
  TpConnectionPresenceType state;

  if (!tp_account_manager_prepare_finish (account_manager, result, &error))
    {
      DEBUG ("Failed to prepare account manager: %s", error->message);
      g_error_free (error);
      goto out;
    }

  state = tp_account_manager_get_most_available_presence (account_manager, NULL,
      NULL);

  /* simulate a presence change so the apply button will be changed
   * if needed */
  presence_changed_cb (account_manager, state, NULL, NULL, self);

out:
  g_object_unref (self);
}

#define WIDGET(cm, proto) \
  { #cm, #proto, "empathy-account-widget-"#proto".ui", \
    account_widget_build_##proto }

static void
add_enable_checkbox (EmpathyAccountWidget *self,
    TpAccount *account)
{
  EmpathyAccountWidgetPriv *priv = GET_PRIV (self);
#ifdef HAVE_MEEGO
  GtkWidget *w;
#else
  GtkWidget *vbox = self->ui_details->widget;
#endif /* HAVE_MEEGO */
  guint nb_rows, nb_columns;
  gboolean is_enabled;

  /* handle the "Enabled" checkbox. We only add it when modifying an account */
  if (priv->creating_account || priv->table_common_settings == NULL)
    return;

  is_enabled = tp_account_is_enabled (account);

#ifdef HAVE_MEEGO
  w = gtk_label_new (_("Account:"));
  gtk_misc_set_alignment (GTK_MISC (w), 0, 0.5);

  priv->enabled_checkbox = mx_gtk_light_switch_new ();

  mx_gtk_light_switch_set_active (
      MX_GTK_LIGHT_SWITCH (priv->enabled_checkbox), is_enabled);

  gtk_widget_show (w);
#else
  priv->enabled_checkbox =
      gtk_check_button_new_with_mnemonic (_("_Enabled"));

  gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (priv->enabled_checkbox),
      is_enabled);
#endif /* HAVE_MEEGO */

  g_object_get (priv->table_common_settings, "n-rows", &nb_rows,
      "n-columns", &nb_columns, NULL);

  gtk_table_resize (GTK_TABLE (priv->table_common_settings), ++nb_rows,
      nb_columns);

#ifdef HAVE_MEEGO
  gtk_table_attach (GTK_TABLE (priv->table_common_settings),
      w,
      0, 1, nb_rows - 1, nb_rows,
      GTK_FILL, 0, 0, 0);
  gtk_table_attach (GTK_TABLE (priv->table_common_settings),
      priv->enabled_checkbox,
      1, nb_columns, nb_rows - 1, nb_rows,
      GTK_EXPAND | GTK_FILL, 0, 0, 0);
#else
  gtk_box_pack_start (GTK_BOX (vbox), priv->enabled_checkbox, FALSE, FALSE, 0);
  gtk_box_reorder_child (GTK_BOX (vbox), priv->enabled_checkbox, 0);
#endif /* HAVE_MEEGO */

  gtk_widget_show (priv->enabled_checkbox);

#ifdef HAVE_MEEGO
  g_signal_connect (G_OBJECT (priv->enabled_checkbox), "switch-flipped",
      G_CALLBACK (account_widget_switch_flipped_cb), self);
#else
  g_signal_connect (G_OBJECT (priv->enabled_checkbox), "toggled",
      G_CALLBACK (account_widget_enabled_toggled_cb), self);
#endif /* HAVE_MEEGO */
}

#ifndef HAVE_MEEGO
/* Meego doesn't support registration */
static void
add_register_buttons (EmpathyAccountWidget *self,
    TpAccount *account)
{
  EmpathyAccountWidgetPriv *priv = GET_PRIV (self);
  const TpConnectionManagerProtocol *protocol;
  GtkWidget *radiobutton_register;
  GtkWidget *vbox = self->ui_details->widget;

  if (!priv->creating_account)
    return;

  protocol = empathy_account_settings_get_tp_protocol (priv->settings);
  if (protocol == NULL)
    return;

  if (!tp_connection_manager_protocol_can_register (protocol))
    return;

  if (account_widget_is_gtalk (self) || account_widget_is_facebook (self))
    return;

  if (priv->simple)
    return;

  priv->radiobutton_reuse = gtk_radio_button_new_with_label (NULL,
      _("This account already exists on the server"));
  radiobutton_register = gtk_radio_button_new_with_label (
      gtk_radio_button_get_group (GTK_RADIO_BUTTON (priv->radiobutton_reuse)),
      _("Create a new account on the server"));

  gtk_box_pack_start (GTK_BOX (vbox), priv->radiobutton_reuse, FALSE, FALSE, 0);
  gtk_box_pack_start (GTK_BOX (vbox), radiobutton_register, FALSE, FALSE, 0);
  gtk_box_reorder_child (GTK_BOX (vbox), priv->radiobutton_reuse, 0);
  gtk_box_reorder_child (GTK_BOX (vbox), radiobutton_register, 1);
  gtk_widget_show (priv->radiobutton_reuse);
  gtk_widget_show (radiobutton_register);
}
#endif /* HAVE_MEEGO */

static void
remember_password_toggled_cb (GtkToggleButton *button,
    EmpathyAccountWidget *self)
{
  EmpathyAccountWidgetPriv *priv = GET_PRIV (self);

  if (gtk_toggle_button_get_active (button))
    {
      gtk_widget_set_sensitive (priv->param_password_widget, TRUE);
    }
  else
    {
      gtk_widget_set_sensitive (priv->param_password_widget, FALSE);
      gtk_entry_set_text (GTK_ENTRY (priv->param_password_widget), "");
      empathy_account_settings_unset (priv->settings, "password");
    }
}

static void
account_settings_password_retrieved_cb (GObject *object,
    gpointer user_data)
{
  EmpathyAccountWidget *self = user_data;
  EmpathyAccountWidgetPriv *priv = GET_PRIV (self);
  const gchar *password = empathy_account_settings_get_string (
      priv->settings, "password");

  if (password != NULL)
    {
      /* We have to do this so that when we call gtk_entry_set_text,
       * the ::changed callback doesn't think the user made the
       * change. */
      priv->automatic_change = TRUE;
      gtk_entry_set_text (GTK_ENTRY (priv->param_password_widget), password);
      priv->automatic_change = FALSE;
    }

  gtk_toggle_button_set_active (
      GTK_TOGGLE_BUTTON (priv->remember_password_widget),
      !EMP_STR_EMPTY (password));
}

static void
do_constructed (GObject *obj)
{
  EmpathyAccountWidget *self = EMPATHY_ACCOUNT_WIDGET (obj);
  EmpathyAccountWidgetPriv *priv = GET_PRIV (self);
  TpAccount *account;
  const gchar *protocol, *cm_name;
  const gchar *display_name, *default_display_name;
  guint i = 0;
  struct {
    const gchar *cm_name;
    const gchar *protocol;
    const char *file;
    void (*func)(EmpathyAccountWidget *self, const gchar *filename);
  } widgets [] = {
    { "salut", "local-xmpp", "empathy-account-widget-local-xmpp.ui",
        account_widget_build_salut },
    WIDGET (gabble, jabber),
    WIDGET (butterfly, msn),
    WIDGET (haze, icq),
    WIDGET (haze, aim),
    WIDGET (haze, yahoo),
    WIDGET (haze, groupwise),
    WIDGET (idle, irc),
    WIDGET (sofiasip, sip),
    WIDGET (psyke, skype),
  };

  cm_name = empathy_account_settings_get_cm (priv->settings);
  protocol = empathy_account_settings_get_protocol (priv->settings);

  for (i = 0 ; i < G_N_ELEMENTS (widgets); i++)
    {
      if (!tp_strdiff (widgets[i].cm_name, cm_name) &&
          !tp_strdiff (widgets[i].protocol, protocol))
        {
          gchar *filename;

          filename = empathy_file_lookup (widgets[i].file,
              "libempathy-gtk");
          widgets[i].func (self, filename);
          g_free (filename);

          break;
        }
    }

  if (i == G_N_ELEMENTS (widgets))
    {
      gchar *filename = empathy_file_lookup (
          "empathy-account-widget-generic.ui", "libempathy-gtk");
      account_widget_build_generic (self, filename);
      g_free (filename);
    }

  /* handle default focus */
  if (self->ui_details->default_focus != NULL)
    {
      GObject *default_focus_entry;

      default_focus_entry = gtk_builder_get_object
        (self->ui_details->gui, self->ui_details->default_focus);
      g_signal_connect (default_focus_entry, "realize",
          G_CALLBACK (gtk_widget_grab_focus),
          NULL);
    }

  /* remember password */
  if (priv->param_password_widget != NULL
      && priv->remember_password_widget != NULL
      && empathy_account_settings_supports_sasl (priv->settings))
    {
      if (priv->simple)
        {
          gtk_toggle_button_set_active (
              GTK_TOGGLE_BUTTON (priv->remember_password_widget), TRUE);
        }
      else
        {
          gtk_toggle_button_set_active (
              GTK_TOGGLE_BUTTON (priv->remember_password_widget),
              !EMP_STR_EMPTY (empathy_account_settings_get_string (
                      priv->settings, "password")));

          /* The password might not have been retrieved from the
           * keyring yet. We should update the remember password
           * toggle button and the password entry when/if it is. */
          g_signal_connect (priv->settings, "password-retrieved",
              G_CALLBACK (account_settings_password_retrieved_cb), self);
        }

      g_signal_connect (priv->remember_password_widget, "toggled",
          G_CALLBACK (remember_password_toggled_cb), self);

      remember_password_toggled_cb (
          GTK_TOGGLE_BUTTON (priv->remember_password_widget), self);
    }
  else if (priv->remember_password_widget != NULL
      && !empathy_account_settings_supports_sasl (priv->settings))
    {
      gtk_widget_set_visible (priv->remember_password_widget, FALSE);
    }

  /* dup and init the account-manager */
  priv->account_manager = tp_account_manager_dup ();

  g_object_ref (self);
  tp_account_manager_prepare_async (priv->account_manager, NULL,
      account_manager_ready_cb, self);

  /* handle apply and cancel button */
  if (!priv->simple)
    {
      GtkWidget *hbox = gtk_hbox_new (TRUE, 3);
      GtkWidget *image;

      /*  We can't use the stock button as its accelerator ('C') clashes with
       *  the Close button. */
      priv->cancel_button = gtk_button_new ();
      gtk_button_set_label (GTK_BUTTON (priv->cancel_button), _("Ca_ncel"));
      gtk_button_set_use_underline (GTK_BUTTON (priv->cancel_button), TRUE);

      image = gtk_image_new_from_stock (GTK_STOCK_CANCEL, GTK_ICON_SIZE_BUTTON);
      gtk_button_set_image (GTK_BUTTON (priv->cancel_button), image);

      priv->apply_button = gtk_button_new ();
      set_apply_button (self);

      /* We'll change this button to a "Log in" one if we are creating a new
       * account and are connected. */
      tp_g_signal_connect_object (priv->account_manager,
          "most-available-presence-changed",
          G_CALLBACK (presence_changed_cb), obj, 0);

      gtk_box_pack_end (GTK_BOX (hbox), priv->apply_button, TRUE,
          TRUE, 3);
      gtk_box_pack_end (GTK_BOX (hbox), priv->cancel_button, TRUE,
          TRUE, 3);

      gtk_box_pack_end (GTK_BOX (self->ui_details->widget), hbox, FALSE,
          FALSE, 3);

      g_signal_connect (priv->cancel_button, "clicked",
          G_CALLBACK (account_widget_cancel_clicked_cb),
          self);
      g_signal_connect (priv->apply_button, "clicked",
          G_CALLBACK (account_widget_apply_clicked_cb),
          self);
      gtk_widget_show_all (hbox);

      if (priv->creating_account)
        /* When creating an account, the user might have nothing to enter.
         * That means that no control interaction might occur,
         * so we update the control button sensitivity manually.
         */
        account_widget_handle_control_buttons_sensitivity (self);
      else
        account_widget_set_control_buttons_sensitivity (self, FALSE);
    }

  account = empathy_account_settings_get_account (priv->settings);

  if (account != NULL)
    {
      g_signal_connect (account, "notify::enabled",
          G_CALLBACK (empathy_account_widget_enabled_cb), self);
    }

#ifndef HAVE_MEEGO
  add_register_buttons (self, account);
#endif /* HAVE_MEEGO */
  add_enable_checkbox (self, account);

  /* hook up to widget destruction to unref ourselves */
  g_signal_connect (self->ui_details->widget, "destroy",
      G_CALLBACK (account_widget_destroy_cb), self);

  empathy_builder_unref_and_keep_widget (self->ui_details->gui,
      self->ui_details->widget);
  self->ui_details->gui = NULL;

  display_name = empathy_account_settings_get_display_name (priv->settings);
  default_display_name = empathy_account_widget_get_default_display_name (self);

  if (tp_strdiff (display_name, default_display_name) &&
      !priv->creating_account)
    {
      /* The display name of the account is not the one that we'd assign by
       * default; assume that the user changed it manually */
      g_object_set (priv->settings, "display-name-overridden", TRUE, NULL);
    }
}

static void
do_dispose (GObject *obj)
{
  EmpathyAccountWidget *self = EMPATHY_ACCOUNT_WIDGET (obj);
  EmpathyAccountWidgetPriv *priv = GET_PRIV (self);

  if (priv->dispose_run)
    return;

  priv->dispose_run = TRUE;

  if (priv->settings != NULL)
    {
      TpAccount *account;
      account = empathy_account_settings_get_account (priv->settings);

      if (account != NULL)
        {
          g_signal_handlers_disconnect_by_func (account,
              empathy_account_widget_enabled_cb, self);
        }

      g_object_unref (priv->settings);
      priv->settings = NULL;
    }

  if (priv->account_manager != NULL)
    {
      g_object_unref (priv->account_manager);
      priv->account_manager = NULL;
    }

  if (G_OBJECT_CLASS (empathy_account_widget_parent_class)->dispose != NULL)
    G_OBJECT_CLASS (empathy_account_widget_parent_class)->dispose (obj);
}

static void
do_finalize (GObject *obj)
{
  EmpathyAccountWidget *self = EMPATHY_ACCOUNT_WIDGET (obj);

  g_free (self->ui_details->default_focus);
  g_slice_free (EmpathyAccountWidgetUIDetails, self->ui_details);

  if (G_OBJECT_CLASS (empathy_account_widget_parent_class)->finalize != NULL)
    G_OBJECT_CLASS (empathy_account_widget_parent_class)->finalize (obj);
}

static void
empathy_account_widget_class_init (EmpathyAccountWidgetClass *klass)
{
  GObjectClass *oclass = G_OBJECT_CLASS (klass);
  GParamSpec *param_spec;

  oclass->get_property = do_get_property;
  oclass->set_property = do_set_property;
  oclass->constructed = do_constructed;
  oclass->dispose = do_dispose;
  oclass->finalize = do_finalize;

  param_spec = g_param_spec_string ("protocol",
      "protocol", "The protocol of the account",
      NULL,
      G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
  g_object_class_install_property (oclass, PROP_PROTOCOL, param_spec);

  param_spec = g_param_spec_object ("settings",
      "settings", "The settings of the account",
      EMPATHY_TYPE_ACCOUNT_SETTINGS,
      G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | G_PARAM_CONSTRUCT_ONLY);
  g_object_class_install_property (oclass, PROP_SETTINGS, param_spec);

  param_spec = g_param_spec_boolean ("simple",
      "simple", "Whether the account widget is a simple or an advanced one",
      FALSE,
      G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | G_PARAM_CONSTRUCT_ONLY);
  g_object_class_install_property (oclass, PROP_SIMPLE, param_spec);

  param_spec = g_param_spec_boolean ("creating-account",
      "creating-account",
      "TRUE if we're creating an account, FALSE if we're modifying it",
      FALSE,
      G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | G_PARAM_CONSTRUCT_ONLY);
  g_object_class_install_property (oclass, PROP_CREATING_ACCOUNT, param_spec);

  param_spec = g_param_spec_boolean ("other-accounts-exist",
      "other-accounts-exist",
      "TRUE if there are any other accounts (even if this isn't yet saved)",
      FALSE,
      G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | G_PARAM_CONSTRUCT_ONLY);
  g_object_class_install_property (oclass, PROP_OTHER_ACCOUNTS_EXIST,
                  param_spec);

  signals[HANDLE_APPLY] =
    g_signal_new ("handle-apply", G_TYPE_FROM_CLASS (klass),
        G_SIGNAL_RUN_LAST, 0, NULL, NULL,
        g_cclosure_marshal_VOID__BOOLEAN,
        G_TYPE_NONE,
        1, G_TYPE_BOOLEAN);

  /* This signal is emitted when an account has been created and enabled. */
  signals[ACCOUNT_CREATED] =
      g_signal_new ("account-created", G_TYPE_FROM_CLASS (klass),
          G_SIGNAL_RUN_LAST, 0, NULL, NULL,
          g_cclosure_marshal_VOID__OBJECT,
          G_TYPE_NONE,
          1, G_TYPE_OBJECT);

  signals[CANCELLED] =
      g_signal_new ("cancelled", G_TYPE_FROM_CLASS (klass),
          G_SIGNAL_RUN_LAST, 0, NULL, NULL,
          g_cclosure_marshal_VOID__VOID,
          G_TYPE_NONE,
          0);

  g_type_class_add_private (klass, sizeof (EmpathyAccountWidgetPriv));
}

static void
empathy_account_widget_init (EmpathyAccountWidget *self)
{
  EmpathyAccountWidgetPriv *priv =
    G_TYPE_INSTANCE_GET_PRIVATE ((self), EMPATHY_TYPE_ACCOUNT_WIDGET,
        EmpathyAccountWidgetPriv);

  self->priv = priv;
  priv->dispose_run = FALSE;

  self->ui_details = g_slice_new0 (EmpathyAccountWidgetUIDetails);
}

/* public methods */

void
empathy_account_widget_discard_pending_changes
    (EmpathyAccountWidget *widget)
{
  EmpathyAccountWidgetPriv *priv = GET_PRIV (widget);

  empathy_account_settings_discard_changes (priv->settings);
  priv->contains_pending_changes = FALSE;
}

gboolean
empathy_account_widget_contains_pending_changes (EmpathyAccountWidget *widget)
{
  EmpathyAccountWidgetPriv *priv = GET_PRIV (widget);

  return priv->contains_pending_changes;
}

void
empathy_account_widget_handle_params (EmpathyAccountWidget *self,
    const gchar *first_widget,
    ...)
{
  va_list args;

  va_start (args, first_widget);
  account_widget_handle_params_valist (self, first_widget, args);
  va_end (args);
}

GtkWidget *
empathy_account_widget_get_widget (EmpathyAccountWidget *widget)
{
  return widget->ui_details->widget;
}

EmpathyAccountWidget *
empathy_account_widget_new_for_protocol (EmpathyAccountSettings *settings,
    gboolean simple)
{
  EmpathyAccountWidget *self;

  g_return_val_if_fail (EMPATHY_IS_ACCOUNT_SETTINGS (settings), NULL);

  self = g_object_new
    (EMPATHY_TYPE_ACCOUNT_WIDGET,
        "settings", settings, "simple", simple,
        "creating-account",
        empathy_account_settings_get_account (settings) == NULL,
        NULL);

  return self;
}

gchar *
empathy_account_widget_get_default_display_name (EmpathyAccountWidget *self)
{
  EmpathyAccountWidgetPriv *priv = GET_PRIV (self);
  const gchar *login_id;
  const gchar *protocol, *p;
  gchar *default_display_name;

  login_id = empathy_account_settings_get_string (priv->settings, "account");
  protocol = empathy_account_settings_get_protocol (priv->settings);

  if (login_id != NULL)
    {
      /* TODO: this should be done in empathy-account-widget-irc */
      if (!tp_strdiff (protocol, "irc"))
        {
          EmpathyIrcNetwork *network;

          network = empathy_irc_network_chooser_get_network (
              priv->irc_network_chooser);
          g_assert (network != NULL);

          /* To translators: The first parameter is the login id and the
           * second one is the network. The resulting string will be something
           * like: "MyUserName on freenode".
           * You should reverse the order of these arguments if the
           * server should come before the login id in your locale.*/
          default_display_name = g_strdup_printf (_("%1$s on %2$s"),
              login_id, empathy_irc_network_get_name (network));
        }
      else if (account_widget_is_facebook (self))
        {
          gchar *tmp;

          tmp = remove_facebook_suffix (login_id);
          default_display_name = g_strdup_printf ("Facebook (%s)", tmp);
          g_free (tmp);
        }
      else
        {
          default_display_name = g_strdup (login_id);
        }

      return default_display_name;
    }

  if ((p = empathy_protocol_name_to_display_name (protocol)) != NULL)
    protocol = p;

  if (protocol != NULL)
    {
      /* To translators: The parameter is the protocol name. The resulting
       * string will be something like: "Jabber Account" */
      default_display_name = g_strdup_printf (_("%s Account"), protocol);
    }
  else
    {
      default_display_name = g_strdup (_("New account"));
    }

  return default_display_name;
}

/* Used by subclass to indicate that widget contains pending changes */
void
empathy_account_widget_changed (EmpathyAccountWidget *self)
{
  EmpathyAccountWidgetPriv *priv = GET_PRIV (self);

  account_widget_handle_control_buttons_sensitivity (self);
  priv->contains_pending_changes = TRUE;
}

void
empathy_account_widget_set_account_param (EmpathyAccountWidget *self,
    const gchar *account)
{
  EmpathyAccountWidgetPriv *priv = GET_PRIV (self);

  if (priv->param_account_widget == NULL)
    return;

  gtk_entry_set_text (GTK_ENTRY (priv->param_account_widget), account);
}

void
empathy_account_widget_set_password_param (EmpathyAccountWidget *self,
    const gchar *account)
{
  EmpathyAccountWidgetPriv *priv = GET_PRIV (self);

  if (priv->param_password_widget == NULL)
    return;

  gtk_entry_set_text (GTK_ENTRY (priv->param_password_widget), account);
}
