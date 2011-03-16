/*
 * Copyright (C) 2003-2007 Imendio AB
 * Copyright (C) 2007-2011 Collabora Ltd.
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
 * Authors: Richard Hult <richard@imendio.com>
 *          Martyn Russell <martyn@imendio.com>
 *          Xavier Claessens <xclaesse@gmail.com>
 *
 * Some snippets are taken from GnuTLS 2.8.6, which is distributed under the
 * same GNU Lesser General Public License 2.1 (or later) version. See
 * empathy_get_x509_certified_hostname ().
 */

#include "config.h"

#include <string.h>
#include <time.h>
#include <sys/types.h>

#include <glib/gi18n-lib.h>

#include <libxml/uri.h>

#include <folks/folks.h>
#include <folks/folks-telepathy.h>

#include <telepathy-glib/account-manager.h>
#include <telepathy-glib/connection.h>
#include <telepathy-glib/channel.h>
#include <telepathy-glib/dbus.h>
#include <telepathy-glib/util.h>

#include "empathy-utils.h"
#include "empathy-contact-manager.h"
#include "empathy-individual-manager.h"
#include "empathy-dispatcher.h"
#include "empathy-idle.h"
#include "empathy-tp-contact-factory.h"

#include <extensions/extensions.h>

#define DEBUG_FLAG EMPATHY_DEBUG_OTHER
#include "empathy-debug.h"

/* Translation between presence types and string */
static struct {
	const gchar *name;
	TpConnectionPresenceType type;
} presence_types[] = {
	{ "available", TP_CONNECTION_PRESENCE_TYPE_AVAILABLE },
	{ "busy",      TP_CONNECTION_PRESENCE_TYPE_BUSY },
	{ "away",      TP_CONNECTION_PRESENCE_TYPE_AWAY },
	{ "ext_away",  TP_CONNECTION_PRESENCE_TYPE_EXTENDED_AWAY },
	{ "hidden",    TP_CONNECTION_PRESENCE_TYPE_HIDDEN },
	{ "offline",   TP_CONNECTION_PRESENCE_TYPE_OFFLINE },
	{ "unset",     TP_CONNECTION_PRESENCE_TYPE_UNSET },
	{ "unknown",   TP_CONNECTION_PRESENCE_TYPE_UNKNOWN },
	{ "error",     TP_CONNECTION_PRESENCE_TYPE_ERROR },
	/* alternative names */
	{ "dnd",      TP_CONNECTION_PRESENCE_TYPE_BUSY },
	{ "brb",      TP_CONNECTION_PRESENCE_TYPE_AWAY },
	{ "xa",       TP_CONNECTION_PRESENCE_TYPE_EXTENDED_AWAY },
	{ NULL, },
};



void
empathy_init (void)
{
	static gboolean initialized = FALSE;

	if (initialized)
		return;

	g_type_init ();

	/* Setup gettext */
	bindtextdomain (GETTEXT_PACKAGE, LOCALEDIR);
	bind_textdomain_codeset (GETTEXT_PACKAGE, "UTF-8");

	/* Setup debug output for empathy and telepathy-glib */
	if (g_getenv ("EMPATHY_TIMING") != NULL) {
		g_log_set_default_handler (tp_debug_timestamped_log_handler, NULL);
	}
	empathy_debug_set_flags (g_getenv ("EMPATHY_DEBUG"));
	tp_debug_divert_messages (g_getenv ("EMPATHY_LOGFILE"));

	emp_cli_init ();

	initialized = TRUE;
}

gchar *
empathy_substring (const gchar *str,
		  gint         start,
		  gint         end)
{
	return g_strndup (str + start, end - start);
}

gint
empathy_strcasecmp (const gchar *s1,
		   const gchar *s2)
{
	return empathy_strncasecmp (s1, s2, -1);
}

gint
empathy_strncasecmp (const gchar *s1,
		    const gchar *s2,
		    gsize        n)
{
	gchar *u1, *u2;
	gint   ret_val;

	u1 = g_utf8_casefold (s1, n);
	u2 = g_utf8_casefold (s2, n);

	ret_val = g_utf8_collate (u1, u2);
	g_free (u1);
	g_free (u2);

	return ret_val;
}

gboolean
empathy_xml_validate (xmlDoc      *doc,
		     const gchar *dtd_filename)
{
	gchar        *path;
	xmlChar      *escaped;
	xmlValidCtxt  cvp;
	xmlDtd       *dtd;
	gboolean      ret;

	path = g_build_filename (g_getenv ("EMPATHY_SRCDIR"), "libempathy",
				 dtd_filename, NULL);
	if (!g_file_test (path, G_FILE_TEST_EXISTS)) {
		g_free (path);
		path = g_build_filename (DATADIR, "empathy", dtd_filename, NULL);
	}
	DEBUG ("Loading dtd file %s", path);

	/* The list of valid chars is taken from libxml. */
	escaped = xmlURIEscapeStr ((const xmlChar *) path,
		(const xmlChar *)":@&=+$,/?;");
	g_free (path);

	memset (&cvp, 0, sizeof (cvp));
	dtd = xmlParseDTD (NULL, escaped);
	ret = xmlValidateDtd (&cvp, doc, dtd);

	xmlFree (escaped);
	xmlFreeDtd (dtd);

	return ret;
}

xmlNodePtr
empathy_xml_node_get_child (xmlNodePtr   node,
			   const gchar *child_name)
{
	xmlNodePtr l;

        g_return_val_if_fail (node != NULL, NULL);
        g_return_val_if_fail (child_name != NULL, NULL);

	for (l = node->children; l; l = l->next) {
		if (l->name && strcmp ((const gchar *) l->name, child_name) == 0) {
			return l;
		}
	}

	return NULL;
}

xmlChar *
empathy_xml_node_get_child_content (xmlNodePtr   node,
				   const gchar *child_name)
{
	xmlNodePtr l;

        g_return_val_if_fail (node != NULL, NULL);
        g_return_val_if_fail (child_name != NULL, NULL);

	l = empathy_xml_node_get_child (node, child_name);
	if (l) {
		return xmlNodeGetContent (l);
	}

	return NULL;
}

xmlNodePtr
empathy_xml_node_find_child_prop_value (xmlNodePtr   node,
				       const gchar *prop_name,
				       const gchar *prop_value)
{
	xmlNodePtr l;
	xmlNodePtr found = NULL;

        g_return_val_if_fail (node != NULL, NULL);
        g_return_val_if_fail (prop_name != NULL, NULL);
        g_return_val_if_fail (prop_value != NULL, NULL);

	for (l = node->children; l && !found; l = l->next) {
		xmlChar *prop;

		if (!xmlHasProp (l, (const xmlChar *) prop_name)) {
			continue;
		}

		prop = xmlGetProp (l, (const xmlChar *) prop_name);
		if (prop && strcmp ((const gchar *) prop, prop_value) == 0) {
			found = l;
		}

		xmlFree (prop);
	}

	return found;
}

const gchar *
empathy_presence_get_default_message (TpConnectionPresenceType presence)
{
	switch (presence) {
	case TP_CONNECTION_PRESENCE_TYPE_AVAILABLE:
		return _("Available");
	case TP_CONNECTION_PRESENCE_TYPE_BUSY:
		return _("Busy");
	case TP_CONNECTION_PRESENCE_TYPE_AWAY:
	case TP_CONNECTION_PRESENCE_TYPE_EXTENDED_AWAY:
		return _("Away");
	case TP_CONNECTION_PRESENCE_TYPE_HIDDEN:
		return _("Invisible");
	case TP_CONNECTION_PRESENCE_TYPE_OFFLINE:
		return _("Offline");
	case TP_CONNECTION_PRESENCE_TYPE_UNKNOWN:
		return _("Unknown");
	case TP_CONNECTION_PRESENCE_TYPE_UNSET:
	case TP_CONNECTION_PRESENCE_TYPE_ERROR:
	default:
		return NULL;
	}

	return NULL;
}

const gchar *
empathy_presence_to_str (TpConnectionPresenceType presence)
{
	int i;

	for (i = 0 ; presence_types[i].name != NULL; i++)
		if (presence == presence_types[i].type)
			return presence_types[i].name;

	return NULL;
}

TpConnectionPresenceType
empathy_presence_from_str (const gchar *str)
{
	int i;

	for (i = 0 ; presence_types[i].name != NULL; i++)
		if (!tp_strdiff (str, presence_types[i].name))
			return presence_types[i].type;

	return TP_CONNECTION_PRESENCE_TYPE_UNSET;
}

static const gchar *
empathy_status_reason_get_default_message (TpConnectionStatusReason reason)
{
	switch (reason) {
	case TP_CONNECTION_STATUS_REASON_NONE_SPECIFIED:
		return _("No reason specified");
	case TP_CONNECTION_STATUS_REASON_REQUESTED:
		return _("Status is set to offline");
	case TP_CONNECTION_STATUS_REASON_NETWORK_ERROR:
		return _("Network error");
	case TP_CONNECTION_STATUS_REASON_AUTHENTICATION_FAILED:
		return _("Authentication failed");
	case TP_CONNECTION_STATUS_REASON_ENCRYPTION_ERROR:
		return _("Encryption error");
	case TP_CONNECTION_STATUS_REASON_NAME_IN_USE:
		return _("Name in use");
	case TP_CONNECTION_STATUS_REASON_CERT_NOT_PROVIDED:
		return _("Certificate not provided");
	case TP_CONNECTION_STATUS_REASON_CERT_UNTRUSTED:
		return _("Certificate untrusted");
	case TP_CONNECTION_STATUS_REASON_CERT_EXPIRED:
		return _("Certificate expired");
	case TP_CONNECTION_STATUS_REASON_CERT_NOT_ACTIVATED:
		return _("Certificate not activated");
	case TP_CONNECTION_STATUS_REASON_CERT_HOSTNAME_MISMATCH:
		return _("Certificate hostname mismatch");
	case TP_CONNECTION_STATUS_REASON_CERT_FINGERPRINT_MISMATCH:
		return _("Certificate fingerprint mismatch");
	case TP_CONNECTION_STATUS_REASON_CERT_SELF_SIGNED:
		return _("Certificate self-signed");
	case TP_CONNECTION_STATUS_REASON_CERT_OTHER_ERROR:
		return _("Certificate error");
	default:
		return _("Unknown reason");
	}
}

static GHashTable *
create_errors_to_message_hash (void)
{
	GHashTable *errors;

	errors = g_hash_table_new (g_str_hash, g_str_equal);
	g_hash_table_insert (errors, TP_ERROR_STR_NETWORK_ERROR, _("Network error"));
	g_hash_table_insert (errors, TP_ERROR_STR_AUTHENTICATION_FAILED,
		_("Authentication failed"));
	g_hash_table_insert (errors, TP_ERROR_STR_ENCRYPTION_ERROR,
		_("Encryption error"));
	g_hash_table_insert (errors, TP_ERROR_STR_CERT_NOT_PROVIDED,
		_("Certificate not provided"));
	g_hash_table_insert (errors, TP_ERROR_STR_CERT_UNTRUSTED,
		_("Certificate untrusted"));
	g_hash_table_insert (errors, TP_ERROR_STR_CERT_EXPIRED,
		_("Certificate expired"));
	g_hash_table_insert (errors, TP_ERROR_STR_CERT_NOT_ACTIVATED,
		_("Certificate not activated"));
	g_hash_table_insert (errors, TP_ERROR_STR_CERT_HOSTNAME_MISMATCH,
		_("Certificate hostname mismatch"));
	g_hash_table_insert (errors, TP_ERROR_STR_CERT_FINGERPRINT_MISMATCH,
		_("Certificate fingerprint mismatch"));
	g_hash_table_insert (errors, TP_ERROR_STR_CERT_SELF_SIGNED,
		_("Certificate self-signed"));
	g_hash_table_insert (errors, TP_ERROR_STR_CANCELLED,
		_("Status is set to offline"));
	g_hash_table_insert (errors, TP_ERROR_STR_ENCRYPTION_NOT_AVAILABLE,
		_("Encryption is not available"));
	g_hash_table_insert (errors, TP_ERROR_STR_CERT_INVALID,
		_("Certificate is invalid"));
	g_hash_table_insert (errors, TP_ERROR_STR_CONNECTION_REFUSED,
		_("Connection has been refused"));
	g_hash_table_insert (errors, TP_ERROR_STR_CONNECTION_FAILED,
		_("Connection can't be established"));
	g_hash_table_insert (errors, TP_ERROR_STR_CONNECTION_LOST,
		_("Connection has been lost"));
	g_hash_table_insert (errors, TP_ERROR_STR_ALREADY_CONNECTED,
		_("This resource is already connected to the server"));
	g_hash_table_insert (errors, TP_ERROR_STR_CONNECTION_REPLACED,
		_("Connection has been replaced by a new connection using the "
		"same resource"));
	g_hash_table_insert (errors, TP_ERROR_STR_REGISTRATION_EXISTS,
		_("The account already exists on the server"));
	g_hash_table_insert (errors, TP_ERROR_STR_SERVICE_BUSY,
		_("Server is currently too busy to handle the connection"));
	g_hash_table_insert (errors, TP_ERROR_STR_CERT_REVOKED,
		_("Certificate has been revoked"));
	g_hash_table_insert (errors, TP_ERROR_STR_CERT_INSECURE,
		_("Certificate uses an insecure cipher algorithm or is "
		"cryptographically weak"));
	g_hash_table_insert (errors, TP_ERROR_STR_CERT_LIMIT_EXCEEDED,
		_("The length of the server certificate, or the depth of the "
		"server certificate chain, exceed the limits imposed by the "
		"cryptography library"));
	g_hash_table_insert (errors, TP_ERROR_STR_SOFTWARE_UPGRADE_REQUIRED,
		_("Your software is too old"));

	return errors;
}

static const gchar *
empathy_dbus_error_name_get_default_message  (const gchar *error)
{
	static GHashTable *errors_to_message = NULL;

	if (error == NULL)
		return NULL;

	if (G_UNLIKELY (errors_to_message == NULL)) {
		errors_to_message = create_errors_to_message_hash ();
	}

	return g_hash_table_lookup (errors_to_message, error);
}

const gchar *
empathy_account_get_error_message (TpAccount *account,
    gboolean *user_requested)
{
	const gchar *dbus_error;
	const gchar *message;
        const GHashTable *details = NULL;
	TpConnectionStatusReason reason;

	dbus_error = tp_account_get_detailed_error (account, &details);

        if (user_requested != NULL)
          {
            if (tp_asv_get_boolean (details, "user-requested", NULL))
              *user_requested = TRUE;
            else
              *user_requested = FALSE;
          }

	message = empathy_dbus_error_name_get_default_message (dbus_error);
	if (message != NULL)
		return message;

	DEBUG ("Don't understand error '%s'; fallback to the status reason (%u)",
		dbus_error, reason);

	tp_account_get_connection_status (account, &reason);

	return empathy_status_reason_get_default_message (reason);
}

gchar *
empathy_file_lookup (const gchar *filename, const gchar *subdir)
{
	gchar *path;

	if (!subdir) {
		subdir = ".";
	}

	path = g_build_filename (g_getenv ("EMPATHY_SRCDIR"), subdir, filename, NULL);
	if (!g_file_test (path, G_FILE_TEST_EXISTS)) {
		g_free (path);
		path = g_build_filename (DATADIR, "empathy", filename, NULL);
	}

	return path;
}

guint
empathy_proxy_hash (gconstpointer key)
{
	TpProxy      *proxy = TP_PROXY (key);
	TpProxyClass *proxy_class = TP_PROXY_GET_CLASS (key);

	g_return_val_if_fail (TP_IS_PROXY (proxy), 0);
	g_return_val_if_fail (proxy_class->must_have_unique_name, 0);

	return g_str_hash (proxy->object_path) ^ g_str_hash (proxy->bus_name);
}

gboolean
empathy_proxy_equal (gconstpointer a,
		     gconstpointer b)
{
	TpProxy *proxy_a = TP_PROXY (a);
	TpProxy *proxy_b = TP_PROXY (b);
	TpProxyClass *proxy_a_class = TP_PROXY_GET_CLASS (a);
	TpProxyClass *proxy_b_class = TP_PROXY_GET_CLASS (b);

	g_return_val_if_fail (TP_IS_PROXY (proxy_a), FALSE);
	g_return_val_if_fail (TP_IS_PROXY (proxy_b), FALSE);
	g_return_val_if_fail (proxy_a_class->must_have_unique_name, 0);
	g_return_val_if_fail (proxy_b_class->must_have_unique_name, 0);

	return g_str_equal (proxy_a->object_path, proxy_b->object_path) &&
	       g_str_equal (proxy_a->bus_name, proxy_b->bus_name);
}

gboolean
empathy_check_available_state (void)
{
	TpConnectionPresenceType presence;
	EmpathyIdle *idle;

	idle = empathy_idle_dup_singleton ();
	presence = empathy_idle_get_state (idle);
	g_object_unref (idle);

	if (presence != TP_CONNECTION_PRESENCE_TYPE_AVAILABLE &&
		presence != TP_CONNECTION_PRESENCE_TYPE_UNSET) {
		return FALSE;
	}

	return TRUE;
}

gint
empathy_uint_compare (gconstpointer a,
		      gconstpointer b)
{
	return *(guint *) a - *(guint *) b;
}

gchar *
empathy_protocol_icon_name (const gchar *protocol)
{
  if (!tp_strdiff (protocol, "yahoojp"))
    /* Yahoo Japan uses the same icon as Yahoo */
    protocol = "yahoo";
  else if (!tp_strdiff (protocol, "simple"))
    /* SIMPLE uses the same icon as SIP */
    protocol = "sip";
  else if (!tp_strdiff (protocol, "sms"))
    return g_strdup ("phone");

  return g_strdup_printf ("im-%s", protocol);
}

GType
empathy_type_dbus_ao (void)
{
  static GType t = 0;

  if (G_UNLIKELY (t == 0))
     t = dbus_g_type_get_collection ("GPtrArray", DBUS_TYPE_G_OBJECT_PATH);

  return t;
}

const char *
empathy_protocol_name_to_display_name (const gchar *proto_name)
{
  int i;
  static struct {
    const gchar *proto;
    const gchar *display;
    gboolean translated;
  } names[] = {
    { "jabber", "Jabber", FALSE },
    { "gtalk", "Google Talk", FALSE },
    { "msn", "Windows Live (MSN)", FALSE, },
    { "local-xmpp", N_("People Nearby"), TRUE },
    { "irc", "IRC", FALSE },
    { "icq", "ICQ", FALSE },
    { "aim", "AIM", FALSE },
    { "yahoo", "Yahoo!", FALSE },
    { "yahoojp", N_("Yahoo! Japan"), TRUE },
    { "facebook", N_("Facebook Chat"), TRUE },
    { "groupwise", "GroupWise", FALSE },
    { "sip", "SIP", FALSE },
    { NULL, NULL }
  };

  for (i = 0; names[i].proto != NULL; i++)
    {
      if (!tp_strdiff (proto_name, names[i].proto))
        {
          if (names[i].translated)
            return _(names[i].display);
          else
            return names[i].display;
        }
    }

  return NULL;
}

/* Note: this function depends on the account manager having its core feature
 * prepared. */
TpAccount *
empathy_get_account_for_connection (TpConnection *connection)
{
  TpAccountManager *manager;
  TpAccount *account = NULL;
  GList *accounts, *l;

  manager = tp_account_manager_dup ();

  accounts = tp_account_manager_get_valid_accounts (manager);

  for (l = accounts; l != NULL; l = l->next)
    {
      TpAccount *a = l->data;

      if (tp_account_get_connection (a) == connection)
        {
          account = a;
          break;
        }
    }

  g_list_free (accounts);
  g_object_unref (manager);

  return account;
}

gboolean
empathy_account_manager_get_accounts_connected (gboolean *connecting)
{
  TpAccountManager *manager;
  GList *accounts, *l;
  gboolean out_connecting = FALSE;
  gboolean out_connected = FALSE;

  manager = tp_account_manager_dup ();

  if (G_UNLIKELY (!tp_account_manager_is_prepared (manager,
          TP_ACCOUNT_MANAGER_FEATURE_CORE)))
    g_critical (G_STRLOC ": %s called before AccountManager ready", G_STRFUNC);

  accounts = tp_account_manager_get_valid_accounts (manager);

  for (l = accounts; l != NULL; l = l->next)
    {
      TpConnectionStatus s = tp_account_get_connection_status (
          TP_ACCOUNT (l->data), NULL);

      if (s == TP_CONNECTION_STATUS_CONNECTING)
        out_connecting = TRUE;
      else if (s == TP_CONNECTION_STATUS_CONNECTED)
        out_connected = TRUE;

      if (out_connecting && out_connected)
        break;
    }

  g_list_free (accounts);
  g_object_unref (manager);

  if (connecting != NULL)
    *connecting = out_connecting;

  return out_connected;
}

/* Change the RequestedPresence of a newly created account to ensure that it
 * is actually connected. */
void
empathy_connect_new_account (TpAccount *account,
    TpAccountManager *account_manager)
{
  TpConnectionPresenceType presence;
  gchar *status, *message;

  /* only force presence if presence was offline, unknown or unset */
  presence = tp_account_get_requested_presence (account, NULL, NULL);
  switch (presence)
    {
      case TP_CONNECTION_PRESENCE_TYPE_OFFLINE:
      case TP_CONNECTION_PRESENCE_TYPE_UNKNOWN:
      case TP_CONNECTION_PRESENCE_TYPE_UNSET:
        presence = tp_account_manager_get_most_available_presence (
            account_manager, &status, &message);

        if (presence == TP_CONNECTION_PRESENCE_TYPE_OFFLINE)
          /* Global presence is offline; we force it so user doesn't have to
           * manually change the presence to connect his new account. */
          presence = TP_CONNECTION_PRESENCE_TYPE_AVAILABLE;

        tp_account_request_presence_async (account, presence,
            status, NULL, NULL, NULL);

        g_free (status);
        g_free (message);
        break;

       case TP_CONNECTION_PRESENCE_TYPE_AVAILABLE:
       case TP_CONNECTION_PRESENCE_TYPE_AWAY:
       case TP_CONNECTION_PRESENCE_TYPE_EXTENDED_AWAY:
       case TP_CONNECTION_PRESENCE_TYPE_HIDDEN:
       case TP_CONNECTION_PRESENCE_TYPE_BUSY:
       case TP_CONNECTION_PRESENCE_TYPE_ERROR:
       default:
        /* do nothing if the presence is not offline */
        break;
    }
}

/* Translate Folks' general presence type to the Tp presence type */
TpConnectionPresenceType
empathy_folks_presence_type_to_tp (FolksPresenceType type)
{
  return (TpConnectionPresenceType) type;
}

/* Returns TRUE if the given Individual contains a TpContact */
gboolean
empathy_folks_individual_contains_contact (FolksIndividual *individual)
{
  GList *personas, *l;

  g_return_val_if_fail (FOLKS_IS_INDIVIDUAL (individual), FALSE);

  personas = folks_individual_get_personas (individual);
  for (l = personas; l != NULL; l = l->next)
    {
      if (empathy_folks_persona_is_interesting (FOLKS_PERSONA (l->data)))
        return (tpf_persona_get_contact (TPF_PERSONA (l->data)) != NULL);
    }

  return FALSE;
}

/* TODO: this needs to be eliminated (and replaced in some cases with user
 * prompts) when we break the assumption that FolksIndividuals are 1:1 with
 * TpContacts */

/* Retrieve the EmpathyContact corresponding to the first TpContact contained
 * within the given Individual. Note that this is a temporary convenience. See
 * the TODO above. */
EmpathyContact *
empathy_contact_dup_from_folks_individual (FolksIndividual *individual)
{
  GList *personas, *l;
  EmpathyContact *contact = NULL;

  g_return_val_if_fail (FOLKS_IS_INDIVIDUAL (individual), NULL);

  personas = folks_individual_get_personas (individual);
  for (l = personas; (l != NULL) && (contact == NULL); l = l->next)
    {
      TpfPersona *persona = l->data;

      if (empathy_folks_persona_is_interesting (FOLKS_PERSONA (persona)))
        {
          TpContact *tp_contact;

          tp_contact = tpf_persona_get_contact (persona);
          contact = empathy_contact_dup_from_tp_contact (tp_contact);
          empathy_contact_set_persona (contact, FOLKS_PERSONA (persona));
        }
    }

  return contact;
}

TpChannelGroupChangeReason
tp_channel_group_change_reason_from_folks_groups_change_reason (
    FolksGroupDetailsChangeReason reason)
{
  return (TpChannelGroupChangeReason) reason;
}

gboolean
empathy_folks_persona_is_interesting (FolksPersona *persona)
{
  /* We're not interested in non-Telepathy personas */
  if (!TPF_IS_PERSONA (persona))
    return FALSE;

  /* We're not interested in user personas which haven't been added to the
   * contact list (see bgo#637151). */
  if (folks_persona_get_is_user (persona) &&
      !tpf_persona_get_is_in_contact_list (TPF_PERSONA (persona)))
    {
      return FALSE;
    }

  return TRUE;
}

gchar *
empathy_get_x509_certificate_hostname (gnutls_x509_crt_t cert)
{
  gchar dns_name[256];
  gsize dns_name_size;
  gint idx;
  gint res = 0;

  /* this snippet is taken from GnuTLS.
   * see gnutls/lib/x509/rfc2818_hostname.c
   */
  for (idx = 0; res >= 0; idx++)
    {
      dns_name_size = sizeof (dns_name);
      res = gnutls_x509_crt_get_subject_alt_name (cert, idx,
          dns_name, &dns_name_size, NULL);

      if (res == GNUTLS_SAN_DNSNAME || res == GNUTLS_SAN_IPADDRESS)
        return g_strndup (dns_name, dns_name_size);
    }

  dns_name_size = sizeof (dns_name);
  res = gnutls_x509_crt_get_dn_by_oid (cert, GNUTLS_OID_X520_COMMON_NAME,
      0, 0, dns_name, &dns_name_size);

  if (res >= 0)
    return g_strndup (dns_name, dns_name_size);

  return NULL;
}
