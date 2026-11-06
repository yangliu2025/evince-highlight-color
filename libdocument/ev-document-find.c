/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8; c-indent-level: 8 -*- */
/*
 *  Copyright (C) 2004 Red Hat, Inc.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2, or (at your option)
 *  any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 */

#include "config.h"

#include <string.h>

#include "ev-document-find.h"
#include "ev-document-text.h"

G_DEFINE_INTERFACE (EvDocumentFind, ev_document_find, 0)

/* Regular expressions are matched against the page text, and each matched
 * character is located through the text layout, which must therefore have one
 * area per character of that text. */
static gboolean
ev_document_find_supports_regex (EvDocumentFind *document_find)
{
	EvDocumentTextInterface *text_iface;

	if (!EV_IS_DOCUMENT_TEXT (document_find))
		return FALSE;

	text_iface = EV_DOCUMENT_TEXT_GET_IFACE (document_find);

	return text_iface->get_text != NULL && text_iface->get_text_layout != NULL;
}

static gboolean
is_word_char (gunichar c)
{
	return g_unichar_isalnum (c) || c == '_';
}

/* VS Code's rule: each end of the match either borders a non-word character
 * or is one itself, so "-foo" is a whole-word match inside "a-foo". */
static gboolean
match_is_whole_word (const gchar *text,
		     gint         start,
		     gint         end)
{
	const gchar *first = text + start;
	const gchar *last = g_utf8_prev_char (text + end);

	if (start > 0 &&
	    is_word_char (g_utf8_get_char (first)) &&
	    is_word_char (g_utf8_get_char (g_utf8_prev_char (first))))
		return FALSE;

	if (text[end] != '\0' &&
	    is_word_char (g_utf8_get_char (last)) &&
	    is_word_char (g_utf8_get_char (text + end)))
		return FALSE;

	return TRUE;
}

/* Prepends one rectangle per text line the match covers. All but the last
 * are flagged next_line, like poppler does for a match that continues on the
 * following line, so the view and the find sidebar treat them as one result. */
static GList *
prepend_match_rectangles (GList             *retval,
			  const gchar       *match_start,
			  const gchar       *match_end,
			  guint              first_area,
			  const EvRectangle *areas,
			  guint              n_areas)
{
	EvFindRectangle *line_rect = NULL;
	GList           *segments = NULL;
	GList           *l;
	const gchar     *p;
	guint            i;

	for (p = match_start, i = first_area; p < match_end && i < n_areas; p = g_utf8_next_char (p), i++) {
		const EvRectangle *area = areas + i;

		if (*p == '\n') {
			if (line_rect)
				segments = g_list_prepend (segments, line_rect);
			line_rect = NULL;
			continue;
		}

		if (!line_rect) {
			line_rect = ev_find_rectangle_new ();
			line_rect->x1 = area->x1;
			line_rect->y1 = area->y1;
			line_rect->x2 = area->x2;
			line_rect->y2 = area->y2;
		} else {
			line_rect->x1 = MIN (line_rect->x1, area->x1);
			line_rect->y1 = MIN (line_rect->y1, area->y1);
			line_rect->x2 = MAX (line_rect->x2, area->x2);
			line_rect->y2 = MAX (line_rect->y2, area->y2);
		}
	}
	if (line_rect)
		segments = g_list_prepend (segments, line_rect);

	/* segments is in reverse order, so its head is the last line */
	for (l = g_list_last (segments); l; l = l->prev) {
		EvFindRectangle *segment = l->data;

		segment->next_line = l->prev != NULL;
		retval = g_list_prepend (retval, segment);
	}
	g_list_free (segments);

	return retval;
}

static GList *
ev_document_find_find_regex (EvDocumentFind *document_find,
			     EvPage         *page,
			     const gchar    *pattern,
			     EvFindOptions   options)
{
	EvDocumentText *document_text = EV_DOCUMENT_TEXT (document_find);
	GRegex         *regex;
	GError         *error = NULL;
	gchar          *text;
	EvRectangle    *areas = NULL;
	guint           n_areas = 0;
	GList          *retval = NULL;
	const gchar    *counted = NULL;
	guint           n_counted = 0;
	gint            len;
	gint            pos = 0;

	/* ^ and $ match at line ends, as in VS Code */
	regex = g_regex_new (pattern,
			     G_REGEX_MULTILINE |
			     ((options & EV_FIND_CASE_SENSITIVE) ? 0 : G_REGEX_CASELESS),
			     0, &error);
	if (!regex) {
		g_debug ("Invalid search pattern: %s", error->message);
		g_error_free (error);
		return NULL;
	}

	text = ev_document_text_get_text (document_text, page);
	if (!text || !ev_document_text_get_text_layout (document_text, page, &areas, &n_areas)) {
		g_free (text);
		g_regex_unref (regex);
		return NULL;
	}

	counted = text;
	len = strlen (text);

	while (pos <= len) {
		GMatchInfo *match_info;
		gint        start, end;
		gboolean    found;

		found = g_regex_match_full (regex, text, len, pos, 0, &match_info, NULL);
		if (found)
			g_match_info_fetch_pos (match_info, 0, &start, &end);
		g_match_info_free (match_info);
		if (!found)
			break;

		/* Empty matches have nothing to highlight */
		if (start == end ||
		    ((options & EV_FIND_WHOLE_WORDS_ONLY) && !match_is_whole_word (text, start, end))) {
			if (start >= len)
				break;
			pos = g_utf8_next_char (text + start) - text;
			continue;
		}

		/* Accepted matches only move forward, so count characters incrementally */
		n_counted += g_utf8_pointer_to_offset (counted, text + start);
		counted = text + start;

		retval = prepend_match_rectangles (retval, text + start, text + end,
						   n_counted, areas, n_areas);
		pos = end;
	}

	g_free (areas);
	g_free (text);
	g_regex_unref (regex);

	return g_list_reverse (retval);
}

static void
ev_document_find_default_init (EvDocumentFindInterface *klass)
{
}

/**
 * ev_document_find_find_text:
 * @document_find: an #EvDocumentFind
 * @page: an #EvPage
 * @text: text to find
 * @case_sensitive: whether to match the string case
 *
 * Returns: (transfer full) (element-type EvRectangle): a list of results
 */
GList *
ev_document_find_find_text (EvDocumentFind *document_find,
			    EvPage         *page,
			    const gchar    *text,
			    gboolean        case_sensitive)
{
	EvDocumentFindInterface *iface = EV_DOCUMENT_FIND_GET_IFACE (document_find);

	return iface->find_text (document_find, page, text, case_sensitive);
}

/**
 * ev_document_find_find_text_with_options:
 * @document_find: an #EvDocumentFind
 * @page: an #EvPage
 * @text: text to find
 * @options: a set of #EvFindOptions
 *
 * Returns: (transfer full) (element-type EvRectangle): a list of results
 */
GList *
ev_document_find_find_text_with_options (EvDocumentFind *document_find,
					 EvPage         *page,
					 const gchar    *text,
					 EvFindOptions   options)
{
	EvDocumentFindInterface *iface = EV_DOCUMENT_FIND_GET_IFACE (document_find);

	if (iface->find_text_with_options)
		return iface->find_text_with_options (document_find, page, text, options);

	return ev_document_find_find_text (document_find, page, text, options & EV_FIND_CASE_SENSITIVE);
}

/**
 * ev_document_find_find_text_extended:
 * @document_find: an #EvDocumentFind
 * @page: an #EvPage
 * @text: text to find
 * @options: a set of #EvFindOptions
 *
 * Returns: (transfer full) (element-type EvFindRectangle): a list of results
 */
GList *
ev_document_find_find_text_extended (EvDocumentFind *document_find,
				     EvPage         *page,
				     const gchar    *text,
				     EvFindOptions   options)
{
	EvDocumentFindInterface *iface = EV_DOCUMENT_FIND_GET_IFACE (document_find);

	if (options & EV_FIND_REGEX) {
		if (ev_document_find_supports_regex (document_find))
			return ev_document_find_find_regex (document_find, page, text, options);
		options &= ~EV_FIND_REGEX;
	}

	if (iface->find_text_extended)
		return iface->find_text_extended (document_find, page, text, options);

	g_warning ("Unimplemented find_text_extended() interface");
	return NULL;
}

/* EvFindRectangle */
G_DEFINE_BOXED_TYPE (EvFindRectangle, ev_find_rectangle, ev_find_rectangle_copy, ev_find_rectangle_free)

EvFindRectangle *
ev_find_rectangle_new (void)
{
	return g_slice_new0 (EvFindRectangle);
}

EvFindRectangle *
ev_find_rectangle_copy (EvFindRectangle *rectangle)
{
	g_return_val_if_fail (rectangle != NULL, NULL);
	return g_slice_dup (EvFindRectangle, rectangle);
}

void
ev_find_rectangle_free (EvFindRectangle *rectangle)
{
	g_slice_free (EvFindRectangle, rectangle);
}

EvFindOptions
ev_document_find_get_supported_options (EvDocumentFind *document_find)
{
	EvDocumentFindInterface *iface = EV_DOCUMENT_FIND_GET_IFACE (document_find);
	EvFindOptions options = 0;

	if (iface->get_supported_options)
		options = iface->get_supported_options (document_find);
	if (ev_document_find_supports_regex (document_find))
		options |= EV_FIND_REGEX;

	return options;
}
