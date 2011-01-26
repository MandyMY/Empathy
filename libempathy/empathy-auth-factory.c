/*
 * empathy-auth-factory.c - Source for EmpathyAuthFactory
 * Copyright (C) 2010 Collabora Ltd.
 * @author Cosimo Cecchi <cosimo.cecchi@collabora.co.uk>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 */

#include "empathy-auth-factory.h"

#include <telepathy-glib/interfaces.h>
#include <telepathy-glib/simple-handler.h>
#include <telepathy-glib/util.h>

#define DEBUG_FLAG EMPATHY_DEBUG_TLS
#include "empathy-debug.h"
#include "empathy-keyring.h"
#include "empathy-server-sasl-handler.h"
#include "empathy-server-tls-handler.h"
#include "empathy-utils.h"

#include "extensions/extensions.h"

G_DEFINE_TYPE (EmpathyAuthFactory, empathy_auth_factory, TP_TYPE_BASE_CLIENT);

typedef struct {
  /* Keep a ref here so the auth client doesn't have to mess with
   * refs. It will be cleared when the channel (and so the handler)
   * gets invalidated. */
  EmpathyServerSASLHandler *sasl_handler;

  gboolean dispose_run;
} EmpathyAuthFactoryPriv;

enum {
  NEW_SERVER_TLS_HANDLER,
  NEW_SERVER_SASL_HANDLER,
  LAST_SIGNAL,
};

static guint signals[LAST_SIGNAL] = { 0, };

#define GET_PRIV(obj) EMPATHY_GET_PRIV (obj, EmpathyAuthFactory)

static EmpathyAuthFactory *auth_factory_singleton = NULL;

typedef struct {
  TpHandleChannelsContext *context;
  EmpathyAuthFactory *self;
} HandlerContextData;

static void
handler_context_data_free (HandlerContextData *data)
{
  tp_clear_object (&data->self);
  tp_clear_object (&data->context);

  g_slice_free (HandlerContextData, data);
}

static HandlerContextData *
handler_context_data_new (EmpathyAuthFactory *self,
    TpHandleChannelsContext *context)
{
  HandlerContextData *data;

  data = g_slice_new0 (HandlerContextData);
  data->self = g_object_ref (self);

  if (context != NULL)
    data->context = g_object_ref (context);

  return data;
}

static void
server_tls_handler_ready_cb (GObject *source,
    GAsyncResult *res,
    gpointer user_data)
{
  EmpathyServerTLSHandler *handler;
  GError *error = NULL;
  EmpathyAuthFactoryPriv *priv;
  HandlerContextData *data = user_data;

  priv = GET_PRIV (data->self);
  handler = empathy_server_tls_handler_new_finish (res, &error);

  if (error != NULL)
    {
      DEBUG ("Failed to create a server TLS handler; error %s",
          error->message);
      tp_handle_channels_context_fail (data->context, error);

      g_error_free (error);
    }
  else
    {
      tp_handle_channels_context_accept (data->context);
      g_signal_emit (data->self, signals[NEW_SERVER_TLS_HANDLER], 0,
          handler);

      g_object_unref (handler);
    }

  handler_context_data_free (data);
}

static void
sasl_handler_invalidated_cb (EmpathyServerSASLHandler *handler,
    gpointer user_data)
{
  EmpathyAuthFactory *self = user_data;
  EmpathyAuthFactoryPriv *priv = GET_PRIV (self);

  DEBUG ("SASL handler is invalidated, unref it");

  tp_clear_object (&priv->sasl_handler);
}

static void
server_sasl_handler_ready_cb (GObject *source,
    GAsyncResult *res,
    gpointer user_data)
{
  EmpathyAuthFactoryPriv *priv;
  GError *error = NULL;
  HandlerContextData *data = user_data;

  priv = GET_PRIV (data->self);
  priv->sasl_handler = empathy_server_sasl_handler_new_finish (res, &error);

  if (error != NULL)
    {
      DEBUG ("Failed to create a server SASL handler; error %s",
          error->message);

      if (data->context != NULL)
        tp_handle_channels_context_fail (data->context, error);

      g_error_free (error);
    }
  else
    {
      if (data->context != NULL)
        tp_handle_channels_context_accept (data->context);

      g_signal_connect (priv->sasl_handler, "invalidated",
          G_CALLBACK (sasl_handler_invalidated_cb), data->self);

      g_signal_emit (data->self, signals[NEW_SERVER_SASL_HANDLER], 0,
          priv->sasl_handler);
    }

  handler_context_data_free (data);
}

static gboolean
common_checks (EmpathyAuthFactory *self,
    GList *channels,
    gboolean must_be_sasl,
    GError **error)
{
  EmpathyAuthFactoryPriv *priv = GET_PRIV (self);
  TpChannel *channel;
  GHashTable *props;
  const gchar * const *available_mechanisms;
  const GError *dbus_error;

  /* there can't be more than one ServerTLSConnection or
   * ServerAuthentication channels at the same time, for the same
   * connection/account.
   */
  if (g_list_length (channels) != 1)
    {
      g_set_error_literal (error, TP_ERROR, TP_ERROR_INVALID_ARGUMENT,
          "Can't handle more than one ServerTLSConnection or ServerAuthentication "
          "channel for the same connection.");

      return FALSE;
    }

  channel = channels->data;

  if (tp_channel_get_channel_type_id (channel) !=
      TP_IFACE_QUARK_CHANNEL_TYPE_SERVER_AUTHENTICATION)
    {
      if (must_be_sasl
          || tp_channel_get_channel_type_id (channel) !=
          EMP_IFACE_QUARK_CHANNEL_TYPE_SERVER_TLS_CONNECTION)
        {
          g_set_error (error, TP_ERROR, TP_ERROR_INVALID_ARGUMENT,
              "Can only handle ServerTLSConnection or ServerAuthentication channels, "
              "this was a %s channel", tp_channel_get_channel_type (channel));

          return FALSE;
        }
    }

  if (tp_channel_get_channel_type_id (channel) ==
      TP_IFACE_QUARK_CHANNEL_TYPE_SERVER_AUTHENTICATION
      && priv->sasl_handler != NULL)
    {
      g_set_error_literal (error, TP_ERROR, TP_ERROR_INVALID_ARGUMENT,
          "Can't handle more than one ServerAuthentication channel at one time");

      return FALSE;
    }

  props = tp_channel_borrow_immutable_properties (channel);
  available_mechanisms = tp_asv_get_boxed (props,
      TP_PROP_CHANNEL_INTERFACE_SASL_AUTHENTICATION_AVAILABLE_MECHANISMS,
      G_TYPE_STRV);

  if (tp_channel_get_channel_type_id (channel) ==
      TP_IFACE_QUARK_CHANNEL_TYPE_SERVER_AUTHENTICATION
      && !tp_strv_contains (available_mechanisms, "X-TELEPATHY-PASSWORD"))
    {
      g_set_error_literal (error, TP_ERROR, TP_ERROR_INVALID_ARGUMENT,
          "Only the X-TELEPATHY-PASSWORD SASL mechanism is supported");

      return FALSE;
    }

  dbus_error = tp_proxy_get_invalidated (channel);

  if (dbus_error != NULL)
    {
      *error = g_error_copy (dbus_error);
      return FALSE;
    }

  return TRUE;
}

static void
handle_channels (TpBaseClient *handler,
    TpAccount *account,
    TpConnection *connection,
    GList *channels,
    GList *requests_satisfied,
    gint64 user_action_time,
    TpHandleChannelsContext *context)
{
  TpChannel *channel;
  GError *error = NULL;
  EmpathyAuthFactory *self = EMPATHY_AUTH_FACTORY (handler);
  HandlerContextData *data;

  DEBUG ("Handle TLS or SASL carrier channels.");

  if (!common_checks (self, channels, FALSE, &error))
    {
      DEBUG ("Failed checks: %s", error->message);
      tp_handle_channels_context_fail (context, error);
      g_clear_error (&error);
      return;
    }

  /* The common checks above have checked this is fine. */
  channel = channels->data;

  data = handler_context_data_new (self, context);
  tp_handle_channels_context_delay (context);

  /* create a handler */
  if (tp_channel_get_channel_type_id (channel) ==
      EMP_IFACE_QUARK_CHANNEL_TYPE_SERVER_TLS_CONNECTION)
    {
      empathy_server_tls_handler_new_async (channel, server_tls_handler_ready_cb,
          data);
    }
  else if (tp_channel_get_channel_type_id (channel) ==
      TP_IFACE_QUARK_CHANNEL_TYPE_SERVER_AUTHENTICATION)
    {
      empathy_server_sasl_handler_new_async (account, channel,
          server_sasl_handler_ready_cb, data);
    }
}

typedef struct
{
  EmpathyAuthFactory *self;
  TpObserveChannelsContext *context;
  TpChannelDispatchOperation *dispatch_operation;
  TpAccount *account;
  TpChannel *channel;
} ObserveChannelsData;

static void
observe_channels_data_free (ObserveChannelsData *data)
{
  g_object_unref (data->context);
  g_object_unref (data->account);
  g_object_unref (data->channel);
  g_object_unref (data->dispatch_operation);
  g_slice_free (ObserveChannelsData, data);
}

static void
claim_cb (GObject *source,
    GAsyncResult *result,
    gpointer user_data)
{
  ObserveChannelsData *data = user_data;
  GError *error = NULL;

  if (!tp_channel_dispatch_operation_claim_finish (
          TP_CHANNEL_DISPATCH_OPERATION (source), result, &error))
    {
      DEBUG ("Failed to call Claim: %s", error->message);
      g_clear_error (&error);
    }
  else
    {
      HandlerContextData *h_data;

      DEBUG ("Claim called successfully");

      h_data = handler_context_data_new (data->self, NULL);

      empathy_server_sasl_handler_new_async (TP_ACCOUNT (data->account),
          data->channel, server_sasl_handler_ready_cb, h_data);
    }

  observe_channels_data_free (data);
}

static void
get_password_cb (GObject *source,
    GAsyncResult *result,
    gpointer user_data)
{
  ObserveChannelsData *data = user_data;

  if (empathy_keyring_get_password_finish (TP_ACCOUNT (source), result, NULL) == NULL)
    {
      /* We don't actually mind if this fails, just let the approver
       * go ahead and take the channel. */

      DEBUG ("We don't have a password for account %s, letting the event "
          "manager approver take it", tp_proxy_get_object_path (source));

      tp_observe_channels_context_accept (data->context);
      observe_channels_data_free (data);
    }
  else
    {
      DEBUG ("We have a password for account %s, calling Claim",
          tp_proxy_get_object_path (source));

      tp_channel_dispatch_operation_claim_async (data->dispatch_operation,
          claim_cb, data);

      tp_observe_channels_context_accept (data->context);
    }
}

static void
observe_channels (TpBaseClient *client,
    TpAccount *account,
    TpConnection *connection,
    GList *channels,
    TpChannelDispatchOperation *dispatch_operation,
    GList *requests,
    TpObserveChannelsContext *context)
{
  EmpathyAuthFactory *self = EMPATHY_AUTH_FACTORY (client);
  TpChannel *channel;
  GError *error = NULL;
  ObserveChannelsData *data;

  DEBUG ("New auth channel to observe");

  if (!common_checks (self, channels, TRUE, &error))
    {
      DEBUG ("Failed checks: %s", error->message);
      tp_observe_channels_context_fail (context, error);
      g_clear_error (&error);
      return;
    }

  /* We're now sure this is a server auth channel using the SASL auth
   * type and X-TELEPATHY-PASSWORD is available. Great. */

  channel = channels->data;

  data = g_slice_new0 (ObserveChannelsData);
  data->self = self;
  data->context = g_object_ref (context);
  data->dispatch_operation = g_object_ref (dispatch_operation);
  data->account = g_object_ref (account);
  data->channel = g_object_ref (channel);

  empathy_keyring_get_password_async (account, get_password_cb, data);

  tp_observe_channels_context_delay (context);
}

static GObject *
empathy_auth_factory_constructor (GType type,
    guint n_params,
    GObjectConstructParam *params)
{
  GObject *retval;

  if (auth_factory_singleton != NULL)
    {
      retval = g_object_ref (auth_factory_singleton);
    }
  else
    {
      retval = G_OBJECT_CLASS (empathy_auth_factory_parent_class)->constructor
        (type, n_params, params);

      auth_factory_singleton = EMPATHY_AUTH_FACTORY (retval);
      g_object_add_weak_pointer (retval, (gpointer *) &auth_factory_singleton);
    }

  return retval;
}

static void
empathy_auth_factory_init (EmpathyAuthFactory *self)
{
  self->priv = G_TYPE_INSTANCE_GET_PRIVATE (self,
      EMPATHY_TYPE_AUTH_FACTORY, EmpathyAuthFactoryPriv);
}

static void
empathy_auth_factory_constructed (GObject *obj)
{
  EmpathyAuthFactory *self = EMPATHY_AUTH_FACTORY (obj);
  TpBaseClient *client = TP_BASE_CLIENT (self);
  GError *error = NULL;

  /* chain up to TpBaseClient first */
  G_OBJECT_CLASS (empathy_auth_factory_parent_class)->constructed (obj);

  if (error != NULL)
    {
      g_critical ("Failed to get TpDBusDaemon: %s", error->message);
      g_error_free (error);
      return;
    }

  tp_base_client_set_handler_bypass_approval (client, FALSE);

  tp_base_client_take_handler_filter (client, tp_asv_new (
          /* ChannelType */
          TP_PROP_CHANNEL_CHANNEL_TYPE, G_TYPE_STRING,
          EMP_IFACE_CHANNEL_TYPE_SERVER_TLS_CONNECTION,
          /* AuthenticationMethod */
          TP_PROP_CHANNEL_TARGET_HANDLE_TYPE, G_TYPE_UINT,
          TP_HANDLE_TYPE_NONE, NULL));

  tp_base_client_take_handler_filter (client, tp_asv_new (
          /* ChannelType */
          TP_PROP_CHANNEL_CHANNEL_TYPE, G_TYPE_STRING,
          TP_IFACE_CHANNEL_TYPE_SERVER_AUTHENTICATION,
          /* AuthenticationMethod */
          TP_PROP_CHANNEL_TYPE_SERVER_AUTHENTICATION_AUTHENTICATION_METHOD,
          G_TYPE_STRING, TP_IFACE_CHANNEL_INTERFACE_SASL_AUTHENTICATION,
          NULL));

  tp_base_client_take_observer_filter (client, tp_asv_new (
          /* ChannelType */
          TP_PROP_CHANNEL_CHANNEL_TYPE, G_TYPE_STRING,
          TP_IFACE_CHANNEL_TYPE_SERVER_AUTHENTICATION,
          /* AuthenticationMethod */
          TP_PROP_CHANNEL_TYPE_SERVER_AUTHENTICATION_AUTHENTICATION_METHOD,
          G_TYPE_STRING, TP_IFACE_CHANNEL_INTERFACE_SASL_AUTHENTICATION,
          NULL));
}

static void
empathy_auth_factory_dispose (GObject *object)
{
  EmpathyAuthFactoryPriv *priv = GET_PRIV (object);

  if (priv->dispose_run)
    return;

  priv->dispose_run = TRUE;

  tp_clear_object (&priv->sasl_handler);

  G_OBJECT_CLASS (empathy_auth_factory_parent_class)->dispose (object);
}

static void
empathy_auth_factory_class_init (EmpathyAuthFactoryClass *klass)
{
  GObjectClass *oclass = G_OBJECT_CLASS (klass);
  TpBaseClientClass *base_client_cls = TP_BASE_CLIENT_CLASS (klass);

  oclass->constructor = empathy_auth_factory_constructor;
  oclass->constructed = empathy_auth_factory_constructed;
  oclass->dispose = empathy_auth_factory_dispose;

  base_client_cls->handle_channels = handle_channels;
  base_client_cls->observe_channels = observe_channels;

  g_type_class_add_private (klass, sizeof (EmpathyAuthFactoryPriv));

  signals[NEW_SERVER_TLS_HANDLER] =
    g_signal_new ("new-server-tls-handler",
      G_TYPE_FROM_CLASS (klass),
      G_SIGNAL_RUN_LAST, 0,
      NULL, NULL,
      g_cclosure_marshal_VOID__OBJECT,
      G_TYPE_NONE,
      1, EMPATHY_TYPE_SERVER_TLS_HANDLER);

  signals[NEW_SERVER_SASL_HANDLER] =
    g_signal_new ("new-server-sasl-handler",
      G_TYPE_FROM_CLASS (klass),
      G_SIGNAL_RUN_LAST, 0,
      NULL, NULL,
      g_cclosure_marshal_VOID__OBJECT,
      G_TYPE_NONE,
      1, EMPATHY_TYPE_SERVER_SASL_HANDLER);
}

EmpathyAuthFactory *
empathy_auth_factory_dup_singleton (void)
{
  EmpathyAuthFactory *out = NULL;
  TpDBusDaemon *bus;

  bus = tp_dbus_daemon_dup (NULL);
  out = g_object_new (EMPATHY_TYPE_AUTH_FACTORY,
      "dbus-daemon", bus,
      "name", "Empathy.Auth",
      NULL);
  g_object_unref (bus);

  return out;
}

gboolean
empathy_auth_factory_register (EmpathyAuthFactory *self,
    GError **error)
{
  return tp_base_client_register (TP_BASE_CLIENT (self), error);
}
