/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*- */
/*
 * Copyright (C) 2007-2009 Collabora Ltd.
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
 *
 * Authors: Xavier Claessens <xclaesse@gmail.com>
 */

#include <config.h>

#include <string.h>
#include <stdlib.h>

#include <gtk/gtk.h>
#include <glib/gi18n-lib.h>

#ifdef HAVE_LIBCHAMPLAIN
#include <champlain/champlain.h>
#include <champlain-gtk/champlain-gtk.h>
#endif

#include <telepathy-glib/account.h>
#include <telepathy-glib/util.h>
#include <telepathy-glib/interfaces.h>

#include <libempathy/empathy-tp-contact-factory.h>
#include <libempathy/empathy-contact-manager.h>
#include <libempathy/empathy-contact-list.h>
#include <libempathy/empathy-location.h>
#include <libempathy/empathy-time.h>
#include <libempathy/empathy-utils.h>

#include "empathy-contact-widget.h"
#include "empathy-account-chooser.h"
#include "empathy-avatar-chooser.h"
#include "empathy-avatar-image.h"
#include "empathy-groups-widget.h"
#include "empathy-ui-utils.h"
#include "empathy-string-parser.h"
#include "empathy-kludge-label.h"

#define DEBUG_FLAG EMPATHY_DEBUG_CONTACT
#include <libempathy/empathy-debug.h>

/**
 * SECTION:empathy-contact-widget
 * @title:EmpathyContactWidget
 * @short_description: A widget used to display and edit details about a contact
 * @include: libempathy-empathy-contact-widget.h
 *
 * #EmpathyContactWidget is a widget which displays appropriate widgets
 * with details about a contact, also allowing changing these details,
 * if desired.
 */

/**
 * EmpathyContactWidget:
 * @parent: parent object
 *
 * Widget which displays appropriate widgets with details about a contact,
 * also allowing changing these details, if desired.
 */

/* Delay before updating the widget when the id entry changed (seconds) */
#define ID_CHANGED_TIMEOUT 1

typedef struct
{
  EmpathyContactManager *manager;
  EmpathyContact *contact;
  EmpathyContactWidgetFlags flags;
  guint widget_id_timeout;
  gulong fav_sig_id;

  GtkWidget *vbox_contact_widget;

  /* Contact */
  GtkWidget *hbox_contact;
  GtkWidget *widget_avatar;
  GtkWidget *widget_account;
  GtkWidget *image_account;
  GtkWidget *label_account;
  GtkWidget *widget_id;
  GtkWidget *widget_alias;
  GtkWidget *label_alias;
  GtkWidget *hbox_presence;
  GtkWidget *image_state;
  GtkWidget *label_status;
  GtkWidget *table_contact;
  GtkWidget *vbox_avatar;
  GtkWidget *favourite_checkbox;

  /* Location */
  GtkWidget *vbox_location;
  GtkWidget *subvbox_location;
  GtkWidget *table_location;
  GtkWidget *label_location;
#ifdef HAVE_LIBCHAMPLAIN
  GtkWidget *viewport_map;
  GtkWidget *map_view_embed;
  ChamplainView *map_view;
#endif

  /* Groups */
  GtkWidget *groups_widget;

  /* Details */
  GtkWidget *vbox_details;
  GtkWidget *table_details;
  GtkWidget *hbox_details_requested;
  GtkWidget *spinner_details;
  GList *details_to_set;
  GCancellable *details_cancellable;

  /* Client */
  GtkWidget *vbox_client;
  GtkWidget *table_client;
  GtkWidget *hbox_client_requested;
} EmpathyContactWidget;

typedef struct
{
  EmpathyContactWidget *information;
  const gchar *name;
  gboolean found;
  GtkTreeIter found_iter;
} FindName;

enum
{
  COL_NAME,
  COL_ENABLED,
  COL_EDITABLE,
  COL_COUNT
};

static gboolean
field_value_is_empty (TpContactInfoField *field)
{
  if (field->field_value == NULL)
    return TRUE;

  return field->field_value[0] == NULL;
}

static void
contact_widget_save (EmpathyContactWidget *information)
{
  TpConnection *connection;
  GList *l, *next;

  connection = empathy_contact_get_connection (information->contact);

  /* Remove empty fields */
  for (l = information->details_to_set; l != NULL; l = next)
    {
      TpContactInfoField *field = l->data;

      next = l->next;
      if (field_value_is_empty (field))
        {
          DEBUG ("Drop empty field: %s", field->field_name);
          tp_contact_info_field_free (field);
          information->details_to_set =
              g_list_delete_link (information->details_to_set, l);
        }
    }

  if (information->details_to_set != NULL)
    {
      tp_connection_set_contact_info_async (connection,
          information->details_to_set, NULL, NULL);
      tp_contact_info_list_free (information->details_to_set);
      information->details_to_set = NULL;
    }
}

static void
contact_widget_details_setup (EmpathyContactWidget *information)
{
  gtk_widget_hide (information->vbox_details);

  information->spinner_details = gtk_spinner_new ();
  gtk_box_pack_end (GTK_BOX (information->hbox_details_requested),
      information->spinner_details, TRUE, TRUE, 0);
  gtk_widget_show (information->spinner_details);
}

static void
contact_widget_details_changed_cb (GtkEntry *entry,
    TpContactInfoField *field)
{
  const gchar *strv[] = { NULL, NULL };

  strv[0] = gtk_entry_get_text (entry);

  if (field->field_value != NULL)
    g_strfreev (field->field_value);
  field->field_value = g_strdupv ((GStrv) strv);
}

static void contact_widget_details_notify_cb (EmpathyContactWidget *information);

typedef struct
{
  const gchar *field_name;
  const gchar *title;
  gboolean linkify;
} InfoFieldData;

static InfoFieldData info_field_datas[] =
{
  { "fn",    N_("Full name:"),      FALSE },
  { "tel",   N_("Phone number:"),   FALSE },
  { "email", N_("E-mail address:"), TRUE },
  { "url",   N_("Website:"),        TRUE },
  { "bday",  N_("Birthday:"),       FALSE },
  { NULL, NULL }
};

static InfoFieldData *
find_info_field_data (const gchar *field_name)
{
  guint i;

  for (i = 0; info_field_datas[i].field_name != NULL; i++)
    {
      if (!tp_strdiff (info_field_datas[i].field_name, field_name))
        return info_field_datas + i;
    }
  return NULL;
}

static gint
contact_info_field_name_cmp (const gchar *name1,
    const gchar *name2)
{
  guint i;

  if (!tp_strdiff (name1, name2))
    return 0;

  /* We use the order of info_field_datas */
  for (i = 0; info_field_datas[i].field_name != NULL; i++)
    {
      if (!tp_strdiff (info_field_datas[i].field_name, name1))
        return -1;
      if (!tp_strdiff (info_field_datas[i].field_name, name2))
        return +1;
    }

  return g_strcmp0 (name1, name2);
}

static gint
contact_info_field_cmp (TpContactInfoField *field1,
    TpContactInfoField *field2)
{
  return contact_info_field_name_cmp (field1->field_name, field2->field_name);
}

static gint
contact_info_field_spec_cmp (TpContactInfoFieldSpec *spec1,
    TpContactInfoFieldSpec *spec2)
{
  return contact_info_field_name_cmp (spec1->name, spec2->name);
}

static guint
contact_widget_details_update_edit (EmpathyContactWidget *information)
{
  TpContact *contact;
  TpConnection *connection;
  GList *specs, *l;
  guint n_rows = 0;
  GList *info;

  g_assert (information->details_to_set == NULL);

  contact = empathy_contact_get_tp_contact (information->contact);
  connection = tp_contact_get_connection (contact);
  info = tp_contact_get_contact_info (contact);

  specs = tp_connection_get_contact_info_supported_fields (connection);
  specs = g_list_sort (specs, (GCompareFunc) contact_info_field_spec_cmp);
  for (l = specs; l != NULL; l = l->next)
    {
      TpContactInfoFieldSpec *spec = l->data;
      TpContactInfoField *field = NULL;
      InfoFieldData *field_data;
      GList *ll;
      GtkWidget *w;

      field_data = find_info_field_data (spec->name);
      if (field_data == NULL)
        {
          DEBUG ("Unhandled ContactInfo field spec: %s", spec->name);
        }

      /* Search initial value */
      for (ll = info; ll != NULL && field == NULL; ll = ll->next)
        {
          TpContactInfoField *tmp = ll->data;

          if (!tp_strdiff (tmp->field_name, spec->name))
            field = tmp;
        }

      if (field != NULL)
        {
          /* We found the field, make a copy for the details_to_set list */
          field = tp_contact_info_field_copy (field);
          DEBUG ("Field %s is in our vCard", spec->name);
        }
      else
        {
          /* Empathy doesn't support editing this field and it's not in the
           * contact's fields so we can't do much with it. */
          DEBUG ("Field %s is not in our vCard", spec->name);

          if (field_data == NULL)
            continue;

          field = tp_contact_info_field_new (spec->name, spec->parameters,
              NULL);
        }

      information->details_to_set = g_list_prepend (information->details_to_set,
          field);

      /* Empathy doesn't display this field so we can't change it. But we put
       * it in the details_to_set list so it won't be erased when calling
       * SetContactInfo (bgo #630427) */
      if (field_data == NULL)
        continue;

      /* Add Title */
      w = gtk_label_new (_(field_data->title));
      gtk_table_attach (GTK_TABLE (information->table_details),
          w, 0, 1, n_rows, n_rows + 1, GTK_FILL, 0, 0, 0);
      gtk_misc_set_alignment (GTK_MISC (w), 0, 0.5);
      gtk_widget_show (w);

      /* Add Value */
      w = gtk_entry_new ();
      gtk_entry_set_text (GTK_ENTRY (w),
          field->field_value[0] ? field->field_value[0] : "");
      gtk_table_attach_defaults (GTK_TABLE (information->table_details),
          w, 1, 2, n_rows, n_rows + 1);
      gtk_widget_show (w);

      g_signal_connect (w, "changed",
        G_CALLBACK (contact_widget_details_changed_cb), field);

      n_rows++;
    }
  g_list_free (specs);
  g_list_free (info);

  return n_rows;
}

static guint
contact_widget_details_update_show (EmpathyContactWidget *information)
{
  TpContact *contact;
  GList *info, *l;
  guint n_rows = 0;

  contact = empathy_contact_get_tp_contact (information->contact);
  info = tp_contact_get_contact_info (contact);
  info = g_list_sort (info, (GCompareFunc) contact_info_field_cmp);
  for (l = info; l != NULL; l = l->next)
    {
      TpContactInfoField *field = l->data;
      InfoFieldData *field_data;
      const gchar *value;
      GtkWidget *w;

      if (field->field_value == NULL || field->field_value[0] == NULL)
        continue;

      value = field->field_value[0];

      field_data = find_info_field_data (field->field_name);
      if (field_data == NULL)
        {
          DEBUG ("Unhandled ContactInfo field: %s", field->field_name);
          continue;
        }

      /* Add Title */
      w = gtk_label_new (_(field_data->title));
      gtk_table_attach (GTK_TABLE (information->table_details),
          w, 0, 1, n_rows, n_rows + 1, GTK_FILL, 0, 0, 0);
      gtk_misc_set_alignment (GTK_MISC (w), 0, 0.5);
      gtk_widget_show (w);

      /* Add Value */
      w = gtk_label_new (value);
      if (field_data->linkify)
        {
          gchar *markup;

          markup = empathy_add_link_markup (value);
          gtk_label_set_markup (GTK_LABEL (w), markup);
          g_free (markup);
        }

      if ((information->flags & EMPATHY_CONTACT_WIDGET_FOR_TOOLTIP) == 0)
        gtk_label_set_selectable (GTK_LABEL (w), TRUE);

      gtk_table_attach_defaults (GTK_TABLE (information->table_details),
          w, 1, 2, n_rows, n_rows + 1);
      gtk_misc_set_alignment (GTK_MISC (w), 0, 0.5);
      gtk_widget_show (w);

      n_rows++;
    }
  g_list_free (info);

  return n_rows;
}

static void
contact_widget_details_notify_cb (EmpathyContactWidget *information)
{
  guint n_rows;

  gtk_container_foreach (GTK_CONTAINER (information->table_details),
      (GtkCallback) gtk_widget_destroy, NULL);

  if ((information->flags & EMPATHY_CONTACT_WIDGET_EDIT_DETAILS) != 0)
    n_rows = contact_widget_details_update_edit (information);
  else
    n_rows = contact_widget_details_update_show (information);

  if (n_rows > 0)
    {
      gtk_widget_show (information->vbox_details);
      gtk_widget_show (information->table_details);
    }
  else
    {
      gtk_widget_hide (information->vbox_details);
    }

  gtk_widget_hide (information->hbox_details_requested);
  gtk_spinner_stop (GTK_SPINNER (information->spinner_details));
}

static void
contact_widget_details_request_cb (GObject *object,
    GAsyncResult *res,
    gpointer user_data)
{
  TpContact *contact = TP_CONTACT (object);
  EmpathyContactWidget *information = user_data;
  GError *error = NULL;

  if (!tp_contact_request_contact_info_finish (contact, res, &error))
    {
      /* If the request got cancelled it could mean the contact widget is
       * destroyed, so we should not dereference information */
      if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        {
          g_clear_error (&error);
          return;
        }

      gtk_widget_hide (information->vbox_details);
      g_clear_error (&error);
    }
  else
    {
      contact_widget_details_notify_cb (information);
    }

  /* If we are going to edit ContactInfo, we don't want live updates */
  if ((information->flags & EMPATHY_CONTACT_WIDGET_EDIT_DETAILS) == 0)
    {
      g_signal_connect_swapped (contact, "notify::contact-info",
          G_CALLBACK (contact_widget_details_notify_cb), information);
    }

  tp_clear_object (&information->details_cancellable);
}

static void
contact_widget_details_feature_prepared_cb (GObject *object,
    GAsyncResult *res,
    gpointer user_data)
{
  TpConnection *connection = TP_CONNECTION (object);
  EmpathyContactWidget *information = user_data;
  TpContact *contact;
  TpContactInfoFlags flags;

  if (!tp_proxy_prepare_finish (connection, res, NULL))
    {
      gtk_widget_hide (information->vbox_details);
      return;
    }

  /* If we want to edit info, but connection does not support that, stop */
  flags = tp_connection_get_contact_info_flags (connection);
  if ((flags & TP_CONTACT_INFO_FLAG_CAN_SET) == 0 &&
      (information->flags & EMPATHY_CONTACT_WIDGET_EDIT_DETAILS) != 0)
    {
      gtk_widget_hide (information->vbox_details);
      return;
    }

  /* Request the contact's info */
  gtk_widget_show (information->vbox_details);
  gtk_widget_show (information->hbox_details_requested);
  gtk_widget_hide (information->table_details);
  gtk_spinner_start (GTK_SPINNER (information->spinner_details));

  contact = empathy_contact_get_tp_contact (information->contact);
  g_assert (information->details_cancellable == NULL);
  information->details_cancellable = g_cancellable_new ();
  tp_contact_request_contact_info_async (contact,
      information->details_cancellable, contact_widget_details_request_cb,
      information);
}

static void
contact_widget_details_update (EmpathyContactWidget *information)
{
  TpContact *tp_contact = NULL;

  if ((information->flags & EMPATHY_CONTACT_WIDGET_SHOW_DETAILS) == 0 &&
      (information->flags & EMPATHY_CONTACT_WIDGET_EDIT_DETAILS) == 0)
    return;

  gtk_widget_hide (information->vbox_details);

  if (information->contact != NULL)
    tp_contact = empathy_contact_get_tp_contact (information->contact);

  if (tp_contact != NULL)
    {
      GQuark features[] = { TP_CONNECTION_FEATURE_CONTACT_INFO, 0 };
      TpConnection *connection;

      /* First, make sure the CONTACT_INFO feature is ready on the connection */
      connection = tp_contact_get_connection (tp_contact);
      tp_proxy_prepare_async (connection, features,
          contact_widget_details_feature_prepared_cb, information);
    }
}

static void
contact_widget_client_update (EmpathyContactWidget *information)
{
  /* FIXME: Needs new telepathy spec */
}

static void
contact_widget_client_setup (EmpathyContactWidget *information)
{
  /* FIXME: Needs new telepathy spec */
  gtk_widget_hide (information->vbox_client);
}

static void
contact_widget_groups_update (EmpathyContactWidget *information)
{
  if (information->flags & EMPATHY_CONTACT_WIDGET_EDIT_GROUPS &&
      information->contact != NULL)
    {
      FolksPersona *persona =
          empathy_contact_get_persona (information->contact);

      if (FOLKS_IS_GROUP_DETAILS (persona))
        {
          empathy_groups_widget_set_group_details (
              EMPATHY_GROUPS_WIDGET (information->groups_widget),
              FOLKS_GROUP_DETAILS (persona));
          gtk_widget_show (information->groups_widget);

          return;
        }
    }

  /* In case of failure */
  gtk_widget_hide (information->groups_widget);
}

/* Converts the Location's GHashTable's key to a user readable string */
static const gchar *
location_key_to_label (const gchar *key)
{
  if (tp_strdiff (key, EMPATHY_LOCATION_COUNTRY_CODE) == FALSE)
    return _("Country ISO Code:");
  else if (tp_strdiff (key, EMPATHY_LOCATION_COUNTRY) == FALSE)
    return _("Country:");
  else if (tp_strdiff (key, EMPATHY_LOCATION_REGION) == FALSE)
    return _("State:");
  else if (tp_strdiff (key, EMPATHY_LOCATION_LOCALITY) == FALSE)
    return _("City:");
  else if (tp_strdiff (key, EMPATHY_LOCATION_AREA) == FALSE)
    return _("Area:");
  else if (tp_strdiff (key, EMPATHY_LOCATION_POSTAL_CODE) == FALSE)
    return _("Postal Code:");
  else if (tp_strdiff (key, EMPATHY_LOCATION_STREET) == FALSE)
    return _("Street:");
  else if (tp_strdiff (key, EMPATHY_LOCATION_BUILDING) == FALSE)
    return _("Building:");
  else if (tp_strdiff (key, EMPATHY_LOCATION_FLOOR) == FALSE)
    return _("Floor:");
  else if (tp_strdiff (key, EMPATHY_LOCATION_ROOM) == FALSE)
    return _("Room:");
  else if (tp_strdiff (key, EMPATHY_LOCATION_TEXT) == FALSE)
    return _("Text:");
  else if (tp_strdiff (key, EMPATHY_LOCATION_DESCRIPTION) == FALSE)
    return _("Description:");
  else if (tp_strdiff (key, EMPATHY_LOCATION_URI) == FALSE)
    return _("URI:");
  else if (tp_strdiff (key, EMPATHY_LOCATION_ACCURACY_LEVEL) == FALSE)
    return _("Accuracy Level:");
  else if (tp_strdiff (key, EMPATHY_LOCATION_ERROR) == FALSE)
    return _("Error:");
  else if (tp_strdiff (key, EMPATHY_LOCATION_VERTICAL_ERROR_M) == FALSE)
    return _("Vertical Error (meters):");
  else if (tp_strdiff (key, EMPATHY_LOCATION_HORIZONTAL_ERROR_M) == FALSE)
    return _("Horizontal Error (meters):");
  else if (tp_strdiff (key, EMPATHY_LOCATION_SPEED) == FALSE)
    return _("Speed:");
  else if (tp_strdiff (key, EMPATHY_LOCATION_BEARING) == FALSE)
    return _("Bearing:");
  else if (tp_strdiff (key, EMPATHY_LOCATION_CLIMB) == FALSE)
    return _("Climb Speed:");
  else if (tp_strdiff (key, EMPATHY_LOCATION_TIMESTAMP) == FALSE)
    return _("Last Updated on:");
  else if (tp_strdiff (key, EMPATHY_LOCATION_LON) == FALSE)
    return _("Longitude:");
  else if (tp_strdiff (key, EMPATHY_LOCATION_LAT) == FALSE)
    return _("Latitude:");
  else if (tp_strdiff (key, EMPATHY_LOCATION_ALT) == FALSE)
    return _("Altitude:");
  else
  {
    DEBUG ("Unexpected Location key: %s", key);
    return key;
  }
}

static void
contact_widget_location_update (EmpathyContactWidget *information)
{
  GHashTable *location;
  GValue *value;
  gdouble lat = 0.0, lon = 0.0;
  gboolean has_position = TRUE;
  GtkWidget *label;
  guint row = 0;
  static const gchar* ordered_geolocation_keys[] = {
    EMPATHY_LOCATION_TEXT,
    EMPATHY_LOCATION_URI,
    EMPATHY_LOCATION_DESCRIPTION,
    EMPATHY_LOCATION_BUILDING,
    EMPATHY_LOCATION_FLOOR,
    EMPATHY_LOCATION_ROOM,
    EMPATHY_LOCATION_STREET,
    EMPATHY_LOCATION_AREA,
    EMPATHY_LOCATION_LOCALITY,
    EMPATHY_LOCATION_REGION,
    EMPATHY_LOCATION_COUNTRY,
    NULL
  };
  int i;
  const gchar *skey;
  gboolean display_map = FALSE;

  if (!(information->flags & EMPATHY_CONTACT_WIDGET_SHOW_LOCATION))
    {
      gtk_widget_hide (information->vbox_location);
      return;
    }

  location = empathy_contact_get_location (information->contact);
  if (location == NULL || g_hash_table_size (location) == 0)
    {
      gtk_widget_hide (information->vbox_location);
      return;
    }

  value = g_hash_table_lookup (location, EMPATHY_LOCATION_LAT);
  if (value == NULL)
      has_position = FALSE;
  else
      lat = g_value_get_double (value);

  value = g_hash_table_lookup (location, EMPATHY_LOCATION_LON);
  if (value == NULL)
      has_position = FALSE;
  else
      lon = g_value_get_double (value);

  value = g_hash_table_lookup (location, EMPATHY_LOCATION_TIMESTAMP);
  if (value == NULL)
    {
      gchar *loc = g_strdup_printf ("<b>%s</b>", _("Location"));
      gtk_label_set_markup (GTK_LABEL (information->label_location), loc);
      g_free (loc);
    }
  else
    {
      gchar *user_date;
      gchar *text;
      gint64 stamp;
      gchar *tmp;

      stamp = g_value_get_int64 (value);

      user_date = empathy_time_to_string_relative (stamp);

      tmp = g_strdup_printf ("<b>%s</b>", _("Location"));
      /* translators: format is "Location, $date" */
      text = g_strdup_printf (_("%s, %s"), tmp, user_date);
      g_free (tmp);
      gtk_label_set_markup (GTK_LABEL (information->label_location), text);
      g_free (user_date);
      g_free (text);
    }


  /* Prepare the location information table */
  if (information->table_location != NULL)
    {
      gtk_widget_destroy (information->table_location);
    }

  information->table_location = gtk_table_new (1, 2, FALSE);
  gtk_box_pack_start (GTK_BOX (information->subvbox_location),
      information->table_location, FALSE, FALSE, 5);


  for (i = 0; (skey = ordered_geolocation_keys[i]); i++)
    {
      const gchar* user_label;
      GValue *gvalue;
      char *svalue = NULL;

      gvalue = g_hash_table_lookup (location, (gpointer) skey);
      if (gvalue == NULL)
        continue;

      user_label = location_key_to_label (skey);

      label = gtk_label_new (user_label);
      gtk_misc_set_alignment (GTK_MISC (label), 0, 0.5);
      gtk_table_attach (GTK_TABLE (information->table_location),
          label, 0, 1, row, row + 1, GTK_FILL, GTK_FILL, 10, 0);
      gtk_widget_show (label);

      if (G_VALUE_TYPE (gvalue) == G_TYPE_DOUBLE)
        {
          gdouble dvalue;
          dvalue = g_value_get_double (gvalue);
          svalue = g_strdup_printf ("%f", dvalue);
        }
      else if (G_VALUE_TYPE (gvalue) == G_TYPE_STRING)
        {
          svalue = g_value_dup_string (gvalue);
        }
      else if (G_VALUE_TYPE (gvalue) == G_TYPE_INT64)
        {
          gint64 time_;

          time_ = g_value_get_int64 (value);
          svalue = empathy_time_to_string_utc (time_, _("%B %e, %Y at %R UTC"));
        }

      if (svalue != NULL)
        {
          label = gtk_label_new (svalue);
          gtk_table_attach_defaults (GTK_TABLE (information->table_location),
              label, 1, 2, row, row + 1);
          gtk_misc_set_alignment (GTK_MISC (label), 0, 0);
          gtk_widget_show (label);

          if (!(information->flags & EMPATHY_CONTACT_WIDGET_FOR_TOOLTIP))
            gtk_label_set_selectable (GTK_LABEL (label), TRUE);
        }

      g_free (svalue);
      row++;
    }

#ifdef HAVE_LIBCHAMPLAIN
  if (has_position &&
      !(information->flags & EMPATHY_CONTACT_WIDGET_FOR_TOOLTIP))
    {
      /* Cannot be displayed in tooltips until Clutter-Gtk can deal with such
       * windows */
      display_map = TRUE;
    }
#endif

  if (row > 0)
    {
      /* We can display some fields */
      gtk_widget_show (information->table_location);
    }
  else if (!display_map)
    {
      /* Can't display either fields or map */
      gtk_widget_hide (information->vbox_location);
      return;
    }

#ifdef HAVE_LIBCHAMPLAIN
  if (display_map)
    {
      ClutterActor *marker;
      ChamplainLayer *layer;

      information->map_view_embed = gtk_champlain_embed_new ();
      information->map_view = gtk_champlain_embed_get_view (
          GTK_CHAMPLAIN_EMBED (information->map_view_embed));

      gtk_container_add (GTK_CONTAINER (information->viewport_map),
          information->map_view_embed);
      g_object_set (G_OBJECT (information->map_view),
          "show-license", TRUE,
          "scroll-mode", CHAMPLAIN_SCROLL_MODE_KINETIC,
          "zoom-level", 10,
          NULL);

      layer = champlain_layer_new ();
      champlain_view_add_layer (information->map_view, layer);

      marker = champlain_marker_new_with_text (
          empathy_contact_get_alias (information->contact), NULL, NULL, NULL);
      champlain_base_marker_set_position (CHAMPLAIN_BASE_MARKER (marker), lat, lon);
      clutter_container_add (CLUTTER_CONTAINER (layer), marker, NULL);

      champlain_view_center_on (information->map_view, lat, lon);
      gtk_widget_show_all (information->viewport_map);
    }
#endif

    gtk_widget_show (information->vbox_location);
}

static void
save_avatar_menu_activate_cb (GtkWidget *widget,
                              EmpathyContactWidget *information)
{
  GtkWidget *dialog;
  EmpathyAvatar *avatar;
  gchar *ext = NULL, *filename;

  dialog = gtk_file_chooser_dialog_new (_("Save Avatar"),
      NULL,
      GTK_FILE_CHOOSER_ACTION_SAVE,
      GTK_STOCK_CANCEL, GTK_RESPONSE_CANCEL,
      GTK_STOCK_SAVE, GTK_RESPONSE_ACCEPT,
      NULL);

  gtk_file_chooser_set_do_overwrite_confirmation (GTK_FILE_CHOOSER (dialog),
      TRUE);

  /* look for the avatar extension */
  avatar = empathy_contact_get_avatar (information->contact);
  if (avatar->format != NULL)
    {
      gchar **splitted;

      splitted = g_strsplit (avatar->format, "/", 2);
      if (splitted[0] != NULL && splitted[1] != NULL)
          ext = g_strdup (splitted[1]);

      g_strfreev (splitted);
    }
  else
    {
      /* Avatar was loaded from the cache so was converted to PNG */
      ext = g_strdup ("png");
    }

  if (ext != NULL)
    {
      gchar *id;

      id = tp_escape_as_identifier (empathy_contact_get_id (
            information->contact));

      filename = g_strdup_printf ("%s.%s", id, ext);
      gtk_file_chooser_set_current_name (GTK_FILE_CHOOSER (dialog), filename);

      g_free (id);
      g_free (ext);
      g_free (filename);
    }

  if (gtk_dialog_run (GTK_DIALOG (dialog)) == GTK_RESPONSE_ACCEPT)
    {
      GError *error = NULL;

      filename = gtk_file_chooser_get_filename (GTK_FILE_CHOOSER (dialog));

      if (!empathy_avatar_save_to_file (avatar, filename, &error))
        {
          /* Save error */
          GtkWidget *error_dialog;

          error_dialog = gtk_message_dialog_new (NULL, 0,
              GTK_MESSAGE_ERROR, GTK_BUTTONS_CLOSE,
              _("Unable to save avatar"));

          gtk_message_dialog_format_secondary_text (
              GTK_MESSAGE_DIALOG (error_dialog), "%s", error->message);

          g_signal_connect (error_dialog, "response",
              G_CALLBACK (gtk_widget_destroy), NULL);

          gtk_window_present (GTK_WINDOW (error_dialog));

          g_clear_error (&error);
        }

      g_free (filename);
    }

  gtk_widget_destroy (dialog);
}

static void
popup_avatar_menu (EmpathyContactWidget *information,
                   GtkWidget *parent,
                   GdkEventButton *event)
{
  GtkWidget *menu, *item;
  gint button, event_time;

  if (information->contact == NULL ||
      empathy_contact_get_avatar (information->contact) == NULL)
      return;

  menu = gtk_menu_new ();

  /* Add "Save as..." entry */
  item = gtk_image_menu_item_new_from_stock (GTK_STOCK_SAVE_AS, NULL);
  gtk_menu_shell_append (GTK_MENU_SHELL (menu), item);
  gtk_widget_show (item);

  g_signal_connect (item, "activate",
      G_CALLBACK (save_avatar_menu_activate_cb), information);

  if (event)
    {
      button = event->button;
      event_time = event->time;
    }
  else
    {
      button = 0;
      event_time = gtk_get_current_event_time ();
    }

  gtk_menu_popup (GTK_MENU (menu), NULL, NULL, NULL, NULL,
      button, event_time);
  g_object_ref_sink (menu);
  g_object_unref (menu);
}

static gboolean
widget_avatar_popup_menu_cb (GtkWidget *widget,
                             EmpathyContactWidget *information)
{
  popup_avatar_menu (information, widget, NULL);

  return TRUE;
}

static gboolean
widget_avatar_button_press_event_cb (GtkWidget *widget,
                                     GdkEventButton *event,
                                     EmpathyContactWidget *information)
{
  /* Ignore double-clicks and triple-clicks */
  if (event->button == 3 && event->type == GDK_BUTTON_PRESS)
    {
      popup_avatar_menu (information, widget, event);
      return TRUE;
    }

  return FALSE;
}

static void
set_avatar_cb (GObject *source,
    GAsyncResult *res,
    gpointer user_data)
{
  GError *error = NULL;

  if (!tp_account_set_avatar_finish (TP_ACCOUNT (source), res, &error)) {
      DEBUG ("Failed to set Account.Avatar: %s", error->message);
      g_error_free (error);
  }
}

static void
set_avatar_on_account (TpAccount *account,
    const gchar *data,
    gsize size,
    const gchar *mime_type)
{
  DEBUG ("%s Account.Avatar on %s", size > 0 ? "Set": "Clear",
      tp_proxy_get_object_path (account));

  tp_account_set_avatar_async (account, (const guchar *) data, size,
      mime_type, set_avatar_cb, NULL);
}

static void
contact_widget_avatar_changed_cb (EmpathyAvatarChooser *chooser,
                                  EmpathyContactWidget *information)
{
  const gchar *data;
  gsize size;
  const gchar *mime_type;
  TpAccount *account;

  empathy_avatar_chooser_get_image_data (
      EMPATHY_AVATAR_CHOOSER (information->widget_avatar),
      &data, &size, &mime_type);

  account = empathy_contact_get_account (information->contact);
  set_avatar_on_account (account, data, size, mime_type);
}

static void
set_nickname_cb (GObject *source,
    GAsyncResult *res,
    gpointer user_data)
{
  GError *error = NULL;

  if (!tp_account_set_nickname_finish (TP_ACCOUNT (source), res, &error))
    {
      DEBUG ("Failed to set Account.Nickname: %s", error->message);
      g_error_free (error);
    }
}

static gboolean
contact_widget_entry_alias_focus_event_cb (GtkEditable *editable,
                                           GdkEventFocus *event,
                                           EmpathyContactWidget *information)
{
  if (information->contact)
    {
      const gchar *alias;

      alias = gtk_entry_get_text (GTK_ENTRY (editable));

      if (empathy_contact_is_user (information->contact))
        {
          TpAccount * account;

          account = empathy_contact_get_account (information->contact);

          DEBUG ("Set Account.Nickname to %s", alias);
          tp_account_set_nickname_async (account, alias, set_nickname_cb, NULL);
        }
      else
        {
          empathy_contact_set_alias (information->contact, alias);
        }
    }

  return FALSE;
}

static void
update_avatar_chooser_account_cb (EmpathyAccountChooser *account_chooser,
                                  EmpathyAvatarChooser *avatar_chooser)
{
  TpAccount *account;

  account = empathy_account_chooser_get_account (account_chooser);
  if (account == NULL)
    return;

  empathy_avatar_chooser_set_account (avatar_chooser, account);
}

static void
contact_widget_avatar_notify_cb (EmpathyContactWidget *information)
{
  EmpathyAvatar *avatar = NULL;

  if (information->contact)
      avatar = empathy_contact_get_avatar (information->contact);

  if (information->flags & EMPATHY_CONTACT_WIDGET_EDIT_AVATAR)
    {
      g_signal_handlers_block_by_func (information->widget_avatar,
          contact_widget_avatar_changed_cb,
          information);
      empathy_avatar_chooser_set (
          EMPATHY_AVATAR_CHOOSER (information->widget_avatar), avatar);
      g_signal_handlers_unblock_by_func (information->widget_avatar,
          contact_widget_avatar_changed_cb, information);
    }
  else
      empathy_avatar_image_set (
          EMPATHY_AVATAR_IMAGE (information->widget_avatar), avatar);
}

static void
contact_widget_name_notify_cb (EmpathyContactWidget *information)
{
  if (GTK_IS_ENTRY (information->widget_alias))
      gtk_entry_set_text (GTK_ENTRY (information->widget_alias),
          empathy_contact_get_alias (information->contact));
  else
      gtk_label_set_label (GTK_LABEL (information->widget_alias),
          empathy_contact_get_alias (information->contact));
}

static void
contact_widget_presence_notify_cb (EmpathyContactWidget *information)
{
  const gchar *status;
  gchar *markup_text = NULL;

  status = empathy_contact_get_status (information->contact);
  if (status != NULL)
    markup_text = empathy_add_link_markup (status);
  gtk_label_set_markup (GTK_LABEL (information->label_status), markup_text);
  g_free (markup_text);

  gtk_image_set_from_icon_name (GTK_IMAGE (information->image_state),
      empathy_icon_name_for_contact (information->contact),
      GTK_ICON_SIZE_BUTTON);
  gtk_widget_show (information->image_state);
}

static void
contact_widget_favourites_changed_cb (EmpathyContactManager *manager,
    EmpathyContact *contact,
    gboolean is_favourite,
    EmpathyContactWidget *information)
{
  if (contact != information->contact)
    return;

  gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (
            information->favourite_checkbox), is_favourite);
}

static void
contact_widget_remove_contact (EmpathyContactWidget *information)
{
  if (information->contact)
    {
      TpContact *tp_contact;

      contact_widget_save (information);

      g_signal_handlers_disconnect_by_func (information->contact,
          contact_widget_name_notify_cb, information);
      g_signal_handlers_disconnect_by_func (information->contact,
          contact_widget_presence_notify_cb, information);
      g_signal_handlers_disconnect_by_func (information->contact,
          contact_widget_avatar_notify_cb, information);

      tp_contact = empathy_contact_get_tp_contact (information->contact);
      if (tp_contact != NULL)
        {
          g_signal_handlers_disconnect_by_func (tp_contact,
              contact_widget_details_notify_cb, information);
        }

      g_object_unref (information->contact);
      information->contact = NULL;
    }

  if (information->details_cancellable != NULL)
    {
      g_cancellable_cancel (information->details_cancellable);
      tp_clear_object (&information->details_cancellable);
    }
}

static void contact_widget_change_contact (EmpathyContactWidget *information);

static void
contact_widget_contact_update (EmpathyContactWidget *information)
{
  TpAccount *account = NULL;
  const gchar *id = NULL;

  /* Connect and get info from new contact */
  if (information->contact)
    {
      g_signal_connect_swapped (information->contact, "notify::name",
          G_CALLBACK (contact_widget_name_notify_cb), information);
      g_signal_connect_swapped (information->contact, "notify::presence",
          G_CALLBACK (contact_widget_presence_notify_cb), information);
      g_signal_connect_swapped (information->contact,
          "notify::presence-message",
          G_CALLBACK (contact_widget_presence_notify_cb), information);
      g_signal_connect_swapped (information->contact, "notify::avatar",
          G_CALLBACK (contact_widget_avatar_notify_cb), information);

      account = empathy_contact_get_account (information->contact);
      id = empathy_contact_get_id (information->contact);
    }

  /* Update account widget */
  if (information->flags & EMPATHY_CONTACT_WIDGET_EDIT_ACCOUNT)
    {
      if (account)
        {
          g_signal_handlers_block_by_func (information->widget_account,
                   contact_widget_change_contact,
                   information);
          empathy_account_chooser_set_account (
              EMPATHY_ACCOUNT_CHOOSER (information->widget_account), account);
          g_signal_handlers_unblock_by_func (information->widget_account,
              contact_widget_change_contact, information);
        }
    }
  else
    {
      if (account)
        {
          const gchar *name;

          name = tp_account_get_display_name (account);
          gtk_label_set_label (GTK_LABEL (information->label_account), name);

          name = tp_account_get_icon_name (account);
          gtk_image_set_from_icon_name (GTK_IMAGE (information->image_account),
              name, GTK_ICON_SIZE_MENU);
        }
    }

  /* Update id widget */
  if (information->flags & EMPATHY_CONTACT_WIDGET_EDIT_ID)
      gtk_entry_set_text (GTK_ENTRY (information->widget_id), id ? id : "");
  else
      gtk_label_set_label (GTK_LABEL (information->widget_id), id ? id : "");

  /* Update other widgets */
  if (information->contact)
    {
      contact_widget_name_notify_cb (information);
      contact_widget_presence_notify_cb (information);
      contact_widget_avatar_notify_cb (information);

      if (information->flags & EMPATHY_CONTACT_WIDGET_EDIT_FAVOURITE)
        {
          FolksPersona *persona = empathy_contact_get_persona (
              information->contact);

          if (persona != NULL && FOLKS_IS_FAVOURITE_DETAILS (persona))
            {
              gboolean is_favourite = folks_favourite_details_get_is_favourite (
                  FOLKS_FAVOURITE_DETAILS (persona));
              contact_widget_favourites_changed_cb (information->manager,
                  information->contact, is_favourite, information);
            }
        }

      gtk_widget_show (information->label_alias);
      gtk_widget_show (information->widget_alias);
      gtk_widget_show (information->hbox_presence);
      gtk_widget_show (information->widget_avatar);
    }
  else
    {
      gtk_widget_hide (information->label_alias);
      gtk_widget_hide (information->widget_alias);
      gtk_widget_hide (information->hbox_presence);
      gtk_widget_hide (information->widget_avatar);
    }
}

static void
contact_widget_set_contact (EmpathyContactWidget *information,
                            EmpathyContact *contact)
{
  if (contact == information->contact)
    return;

  contact_widget_remove_contact (information);
  if (contact)
    information->contact = g_object_ref (contact);

  /* set the selected account to be the account this contact came from */
  if (contact && EMPATHY_IS_ACCOUNT_CHOOSER (information->widget_account)) {
      empathy_account_chooser_set_account (
		      EMPATHY_ACCOUNT_CHOOSER (information->widget_account),
		      empathy_contact_get_account (contact));
  }

  /* Update information for widgets */
  contact_widget_contact_update (information);
  contact_widget_groups_update (information);
  contact_widget_details_update (information);
  contact_widget_client_update (information);
  contact_widget_location_update (information);
}

static void
contact_widget_got_contact_cb (TpConnection *connection,
                               EmpathyContact *contact,
                               const GError *error,
                               gpointer user_data,
                               GObject *weak_object)
{
  EmpathyContactWidget *information = user_data;

  if (error != NULL)
    {
      DEBUG ("Error: %s", error->message);
      return;
    }

  contact_widget_set_contact (information, contact);
}

static void
contact_widget_change_contact (EmpathyContactWidget *information)
{
  TpConnection *connection;

  connection = empathy_account_chooser_get_connection (
      EMPATHY_ACCOUNT_CHOOSER (information->widget_account));
  if (!connection)
      return;

  if (information->flags & EMPATHY_CONTACT_WIDGET_EDIT_ID)
    {
      const gchar *id;

      id = gtk_entry_get_text (GTK_ENTRY (information->widget_id));
      if (!EMP_STR_EMPTY (id))
        {
          empathy_tp_contact_factory_get_from_id (connection, id,
              contact_widget_got_contact_cb, information, NULL,
              G_OBJECT (information->vbox_contact_widget));
        }
    }
  else
    {
      empathy_tp_contact_factory_get_from_handle (connection,
          tp_connection_get_self_handle (connection),
          contact_widget_got_contact_cb, information, NULL,
          G_OBJECT (information->vbox_contact_widget));
    }
}

static gboolean
contact_widget_id_activate_timeout (EmpathyContactWidget *self)
{
  contact_widget_change_contact (self);
  return FALSE;
}

static void
contact_widget_id_changed_cb (GtkEntry *entry,
                              EmpathyContactWidget *self)
{
  if (self->widget_id_timeout != 0)
    {
      g_source_remove (self->widget_id_timeout);
    }

  self->widget_id_timeout =
    g_timeout_add_seconds (ID_CHANGED_TIMEOUT,
        (GSourceFunc) contact_widget_id_activate_timeout, self);
}

static gboolean
contact_widget_id_focus_out_cb (GtkWidget *widget,
                                GdkEventFocus *event,
                                EmpathyContactWidget *information)
{
  contact_widget_change_contact (information);
  return FALSE;
}

static void
favourite_toggled_cb (GtkToggleButton *button,
    EmpathyContactWidget *information)
{
  FolksPersona *persona = empathy_contact_get_persona (information->contact);

  if (persona != NULL && FOLKS_IS_FAVOURITE_DETAILS (persona))
    {
      gboolean active = gtk_toggle_button_get_active (button);
      folks_favourite_details_set_is_favourite (
          FOLKS_FAVOURITE_DETAILS (persona), active);
    }
}

static void
contact_widget_contact_setup (EmpathyContactWidget *information)
{
  /* Setup label_status as a KludgeLabel */
  information->label_status = empathy_kludge_label_new ("");
  gtk_label_set_line_wrap_mode (GTK_LABEL (information->label_status),
                                PANGO_WRAP_WORD_CHAR);
  gtk_label_set_line_wrap (GTK_LABEL (information->label_status),
                           TRUE);

  if (!(information->flags & EMPATHY_CONTACT_WIDGET_FOR_TOOLTIP))
    gtk_label_set_selectable (GTK_LABEL (information->label_status), TRUE);

  gtk_box_pack_start (GTK_BOX (information->hbox_presence),
        information->label_status, TRUE, TRUE, 0);
  gtk_widget_show (information->label_status);

  /* Setup account label/chooser */
  if (information->flags & EMPATHY_CONTACT_WIDGET_EDIT_ACCOUNT)
    {
      information->widget_account = empathy_account_chooser_new ();

      g_signal_connect_swapped (information->widget_account, "changed",
            G_CALLBACK (contact_widget_change_contact),
            information);
    }
  else
    {
      /* Pack the protocol icon with the account name in an hbox */
      information->widget_account = gtk_hbox_new (FALSE, 6);

      information->label_account = gtk_label_new (NULL);
      if (!(information->flags & EMPATHY_CONTACT_WIDGET_FOR_TOOLTIP)) {
        gtk_label_set_selectable (GTK_LABEL (information->label_account), TRUE);
      }
      gtk_misc_set_alignment (GTK_MISC (information->label_account), 0, 0.5);
      gtk_widget_show (information->label_account);

      information->image_account = gtk_image_new ();
      gtk_widget_show (information->image_account);

      gtk_box_pack_start (GTK_BOX (information->widget_account),
          information->image_account, FALSE, FALSE, 0);
      gtk_box_pack_start (GTK_BOX (information->widget_account),
          information->label_account, FALSE, TRUE, 0);
    }
  gtk_table_attach_defaults (GTK_TABLE (information->table_contact),
           information->widget_account,
           1, 2, 0, 1);
  gtk_widget_show (information->widget_account);

  /* Set up avatar chooser/display */
  if (information->flags & EMPATHY_CONTACT_WIDGET_EDIT_AVATAR)
    {
      information->widget_avatar = empathy_avatar_chooser_new ();
      g_signal_connect (information->widget_avatar, "changed",
            G_CALLBACK (contact_widget_avatar_changed_cb),
            information);
      if (information->flags & EMPATHY_CONTACT_WIDGET_EDIT_ACCOUNT)
        {
          g_signal_connect (information->widget_account, "changed",
              G_CALLBACK (update_avatar_chooser_account_cb),
              information->widget_avatar);
          update_avatar_chooser_account_cb (
              EMPATHY_ACCOUNT_CHOOSER (information->widget_account),
              EMPATHY_AVATAR_CHOOSER (information->widget_avatar));
        }
    }
  else
    {
      information->widget_avatar = empathy_avatar_image_new ();

      g_signal_connect (information->widget_avatar, "popup-menu",
          G_CALLBACK (widget_avatar_popup_menu_cb), information);
      g_signal_connect (information->widget_avatar, "button-press-event",
          G_CALLBACK (widget_avatar_button_press_event_cb), information);
    }

  gtk_box_pack_start (GTK_BOX (information->vbox_avatar),
          information->widget_avatar,
          FALSE, FALSE,
          6);
  gtk_widget_show (information->widget_avatar);

  /* Setup id label/entry */
  if (information->flags & EMPATHY_CONTACT_WIDGET_EDIT_ID)
    {
      information->widget_id = gtk_entry_new ();
      g_signal_connect (information->widget_id, "focus-out-event",
            G_CALLBACK (contact_widget_id_focus_out_cb),
            information);
      g_signal_connect (information->widget_id, "changed",
            G_CALLBACK (contact_widget_id_changed_cb),
            information);
    }
  else
    {
      information->widget_id = gtk_label_new (NULL);
      if (!(information->flags & EMPATHY_CONTACT_WIDGET_FOR_TOOLTIP)) {
        gtk_label_set_selectable (GTK_LABEL (information->widget_id), TRUE);
      }
      gtk_misc_set_alignment (GTK_MISC (information->widget_id), 0, 0.5);
    }
  gtk_table_attach_defaults (GTK_TABLE (information->table_contact),
           information->widget_id,
           1, 2, 1, 2);
  gtk_widget_show (information->widget_id);

  /* Setup alias label/entry */
  if (information->flags & EMPATHY_CONTACT_WIDGET_EDIT_ALIAS)
    {
      information->widget_alias = gtk_entry_new ();

      if (!(information->flags & EMPATHY_CONTACT_WIDGET_NO_SET_ALIAS))
        g_signal_connect (information->widget_alias, "focus-out-event",
              G_CALLBACK (contact_widget_entry_alias_focus_event_cb),
              information);

      /* Make return activate the window default (the Close button) */
      gtk_entry_set_activates_default (GTK_ENTRY (information->widget_alias),
          TRUE);
    }
  else
    {
      information->widget_alias = gtk_label_new (NULL);
      if (!(information->flags & EMPATHY_CONTACT_WIDGET_FOR_TOOLTIP)) {
        gtk_label_set_selectable (GTK_LABEL (information->widget_alias), TRUE);
      }
      gtk_misc_set_alignment (GTK_MISC (information->widget_alias), 0, 0.5);
    }
  gtk_table_attach_defaults (GTK_TABLE (information->table_contact),
           information->widget_alias,
           1, 2, 2, 3);
  if (information->flags & EMPATHY_CONTACT_WIDGET_FOR_TOOLTIP) {
    gtk_label_set_selectable (GTK_LABEL (information->label_status), FALSE);
  }
  gtk_widget_show (information->widget_alias);

  /* Favorite */
  if (information->flags & EMPATHY_CONTACT_WIDGET_EDIT_FAVOURITE)
    {
      information->favourite_checkbox = gtk_check_button_new_with_label (
          _("Favorite"));

      g_signal_connect (information->favourite_checkbox, "toggled",
          G_CALLBACK (favourite_toggled_cb), information);

      gtk_table_attach_defaults (GTK_TABLE (information->table_contact),
           information->favourite_checkbox, 0, 2, 3, 4);

      information->fav_sig_id = g_signal_connect (information->manager,
          "favourites-changed",
          G_CALLBACK (contact_widget_favourites_changed_cb), information);

      gtk_widget_show (information->favourite_checkbox);
    }
}

static void
contact_widget_destroy_cb (GtkWidget *widget,
                           EmpathyContactWidget *information)
{
  contact_widget_remove_contact (information);

  if (information->widget_id_timeout != 0)
    {
      g_source_remove (information->widget_id_timeout);
    }

  if (information->fav_sig_id != 0)
    g_signal_handler_disconnect (information->manager, information->fav_sig_id);

  g_object_unref (information->manager);

  g_slice_free (EmpathyContactWidget, information);
}

/**
 * empathy_contact_widget_new:
 * @contact: an #EmpathyContact
 * @flags: #EmpathyContactWidgetFlags for the new contact widget
 *
 * Creates a new #EmpathyContactWidget.
 *
 * Return value: a new #EmpathyContactWidget
 */
GtkWidget *
empathy_contact_widget_new (EmpathyContact *contact,
                            EmpathyContactWidgetFlags flags)
{
  EmpathyContactWidget *information;
  GtkBuilder *gui;
  gchar *filename;

  g_return_val_if_fail (contact == NULL || EMPATHY_IS_CONTACT (contact), NULL);

  information = g_slice_new0 (EmpathyContactWidget);
  information->flags = flags;

  filename = empathy_file_lookup ("empathy-contact-widget.ui",
      "libempathy-gtk");
  gui = empathy_builder_get_file (filename,
       "vbox_contact_widget", &information->vbox_contact_widget,
       "hbox_contact", &information->hbox_contact,
       "hbox_presence", &information->hbox_presence,
       "label_alias", &information->label_alias,
       "image_state", &information->image_state,
       "table_contact", &information->table_contact,
       "vbox_avatar", &information->vbox_avatar,
       "vbox_location", &information->vbox_location,
       "subvbox_location", &information->subvbox_location,
       "label_location", &information->label_location,
#ifdef HAVE_LIBCHAMPLAIN
       "viewport_map", &information->viewport_map,
#endif
       "groups_widget", &information->groups_widget,
       "vbox_details", &information->vbox_details,
       "table_details", &information->table_details,
       "hbox_details_requested", &information->hbox_details_requested,
       "vbox_client", &information->vbox_client,
       "table_client", &information->table_client,
       "hbox_client_requested", &information->hbox_client_requested,
       NULL);
  g_free (filename);

  empathy_builder_connect (gui, information,
      "vbox_contact_widget", "destroy", contact_widget_destroy_cb,
      NULL);
  information->table_location = NULL;

  g_object_set_data (G_OBJECT (information->vbox_contact_widget),
      "EmpathyContactWidget",
      information);

  information->manager = empathy_contact_manager_dup_singleton ();

  /* Create widgets */
  contact_widget_contact_setup (information);
  contact_widget_details_setup (information);
  contact_widget_client_setup (information);

  if (contact != NULL)
    contact_widget_set_contact (information, contact);
  else if (information->flags & EMPATHY_CONTACT_WIDGET_EDIT_ACCOUNT ||
      information->flags & EMPATHY_CONTACT_WIDGET_EDIT_ID)
    contact_widget_change_contact (information);

  return empathy_builder_unref_and_keep_widget (gui,
    information->vbox_contact_widget);
}

/**
 * empathy_contact_widget_get_contact:
 * @widget: an #EmpathyContactWidget
 *
 * Get the #EmpathyContact related with the #EmpathyContactWidget @widget.
 *
 * Returns: the #EmpathyContact associated with @widget
 */
EmpathyContact *
empathy_contact_widget_get_contact (GtkWidget *widget)
{
  EmpathyContactWidget *information;

  g_return_val_if_fail (GTK_IS_WIDGET (widget), NULL);

  information = g_object_get_data (G_OBJECT (widget), "EmpathyContactWidget");
  if (!information)
      return NULL;

  return information->contact;
}

const gchar *
empathy_contact_widget_get_alias (GtkWidget *widget)
{
  EmpathyContactWidget *information;

  g_return_val_if_fail (GTK_IS_WIDGET (widget), NULL);

  information = g_object_get_data (G_OBJECT (widget), "EmpathyContactWidget");
  if (!information)
      return NULL;

  return gtk_entry_get_text (GTK_ENTRY (information->widget_alias));
}

/**
 * empathy_contact_widget_set_contact:
 * @widget: an #EmpathyContactWidget
 * @contact: a different #EmpathyContact
 *
 * Change the #EmpathyContact related with the #EmpathyContactWidget @widget.
 */
void
empathy_contact_widget_set_contact (GtkWidget *widget,
                                    EmpathyContact *contact)
{
  EmpathyContactWidget *information;

  g_return_if_fail (GTK_IS_WIDGET (widget));
  g_return_if_fail (EMPATHY_IS_CONTACT (contact));

  information = g_object_get_data (G_OBJECT (widget), "EmpathyContactWidget");
  if (!information)
    return;

  contact_widget_set_contact (information, contact);
}

/**
 * empathy_contact_widget_set_account_filter:
 * @widget: an #EmpathyContactWidget
 * @filter: a #EmpathyAccountChooserFilterFunc
 * @user_data: user data to pass to @filter, or %NULL
 *
 * Set a filter on the #EmpathyAccountChooser included in the
 * #EmpathyContactWidget.
 */
void
empathy_contact_widget_set_account_filter (
    GtkWidget *widget,
    EmpathyAccountChooserFilterFunc filter,
    gpointer user_data)
{
  EmpathyContactWidget *information;
  EmpathyAccountChooser *chooser;

  g_return_if_fail (GTK_IS_WIDGET (widget));

  information = g_object_get_data (G_OBJECT (widget), "EmpathyContactWidget");
  if (!information)
    return;

  chooser = EMPATHY_ACCOUNT_CHOOSER (information->widget_account);
  if (chooser)
      empathy_account_chooser_set_filter (chooser, filter, user_data);
}

