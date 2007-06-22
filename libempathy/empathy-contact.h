/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*- */
/*
 * Copyright (C) 2004 Imendio AB
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
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */

#ifndef __EMPATHY_CONTACT_H__
#define __EMPATHY_CONTACT_H__

#include <glib-object.h>

#include <libmissioncontrol/mc-account.h>

#include "empathy-avatar.h"
#include "empathy-presence.h"

G_BEGIN_DECLS

#define EMPATHY_TYPE_CONTACT         (empathy_contact_get_gtype ())
#define EMPATHY_CONTACT(o)           (G_TYPE_CHECK_INSTANCE_CAST ((o), EMPATHY_TYPE_CONTACT, EmpathyContact))
#define EMPATHY_CONTACT_CLASS(k)     (G_TYPE_CHECK_CLASS_CAST ((k), EMPATHY_TYPE_CONTACT, EmpathyContactClass))
#define EMPATHY_IS_CONTACT(o)        (G_TYPE_CHECK_INSTANCE_TYPE ((o), EMPATHY_TYPE_CONTACT))
#define EMPATHY_IS_CONTACT_CLASS(k)  (G_TYPE_CHECK_CLASS_TYPE ((k), EMPATHY_TYPE_CONTACT))
#define EMPATHY_CONTACT_GET_CLASS(o) (G_TYPE_INSTANCE_GET_CLASS ((o), EMPATHY_TYPE_CONTACT, EmpathyContactClass))

typedef struct _EmpathyContact      EmpathyContact;
typedef struct _EmpathyContactClass EmpathyContactClass;

struct _EmpathyContact {
	GObject parent;
};

struct _EmpathyContactClass {
	GObjectClass parent_class;
};

typedef enum {
	EMPATHY_SUBSCRIPTION_NONE = 0,
	EMPATHY_SUBSCRIPTION_TO   = 1 << 0,	/* We send our presence to that contact */
	EMPATHY_SUBSCRIPTION_FROM = 1 << 1,	/* That contact sends his presence to us */
	EMPATHY_SUBSCRIPTION_BOTH = EMPATHY_SUBSCRIPTION_TO | EMPATHY_SUBSCRIPTION_FROM
} EmpathySubscription;

GType              empathy_contact_get_gtype                 (void) G_GNUC_CONST;

EmpathyContact *    empathy_contact_new                       (McAccount          *account);
EmpathyContact *    empathy_contact_new_full                  (McAccount          *account,
							     const gchar        *id,
							     const gchar        *name);
const gchar *      empathy_contact_get_id                    (EmpathyContact      *contact);
const gchar *      empathy_contact_get_name                  (EmpathyContact      *contact);
EmpathyAvatar *     empathy_contact_get_avatar                (EmpathyContact      *contact);
McAccount *        empathy_contact_get_account               (EmpathyContact      *contact);
EmpathyPresence *   empathy_contact_get_presence              (EmpathyContact      *contact);
GList *            empathy_contact_get_groups                (EmpathyContact      *contact);
EmpathySubscription empathy_contact_get_subscription          (EmpathyContact      *contact);
guint              empathy_contact_get_handle                (EmpathyContact      *contact);
gboolean           empathy_contact_is_user                   (EmpathyContact      *contact);
void               empathy_contact_set_id                    (EmpathyContact      *contact,
							     const gchar        *id);
void               empathy_contact_set_name                  (EmpathyContact      *contact,
							     const gchar        *name);
void               empathy_contact_set_avatar                (EmpathyContact      *contact,
							     EmpathyAvatar       *avatar);
void               empathy_contact_set_account               (EmpathyContact      *contact,
							     McAccount          *account);
void               empathy_contact_set_presence              (EmpathyContact      *contact,
							     EmpathyPresence     *presence);
void               empathy_contact_set_groups                (EmpathyContact      *contact,
							     GList              *categories);
void               empathy_contact_set_subscription          (EmpathyContact      *contact,
							     EmpathySubscription  subscription);
void               empathy_contact_set_handle                (EmpathyContact      *contact,
							     guint               handle);
void               empathy_contact_set_is_user               (EmpathyContact      *contact,
							     gboolean            is_user);
void               empathy_contact_add_group                 (EmpathyContact      *contact,
							     const gchar        *group);
void               empathy_contact_remove_group              (EmpathyContact      *contact,
							     const gchar        *group);
gboolean           empathy_contact_is_online                 (EmpathyContact      *contact);
gboolean           empathy_contact_is_in_group               (EmpathyContact      *contact,
							     const gchar        *group);
const gchar *      empathy_contact_get_status                (EmpathyContact      *contact);
gboolean           empathy_contact_equal                     (gconstpointer       v1,
							     gconstpointer       v2);
guint              empathy_contact_hash                      (gconstpointer       key);

G_END_DECLS

#endif /* __EMPATHY_CONTACT_H__ */

