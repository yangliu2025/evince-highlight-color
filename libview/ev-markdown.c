/* ev-markdown.c
 *  this file is part of evince, a gnome document viewer
 *
 * Evince is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * Evince is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 */

#include "config.h"

#include <string.h>
#include <md4c.h>
#include <pango/pango.h>

#include "ev-markdown.h"

/* Pango markup only knows inline attributes, so block structure is
 * expressed with plain characters: bullets, "│ " bars for quotes, two
 * spaces of indentation per list level and a row of "─" for rules.
 * Nothing here sets a colour; the consumers draw on backgrounds we do
 * not control (highlight colour, tooltip theme, sidebar selection).
 */

#define QUOTE_BAR   "\xe2\x94\x82"  /* │ */
#define RULE        "\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80"
#define BULLET      "\xe2\x80\xa2 " /* • */
#define TASK_DONE   "\xe2\x98\x91 " /* ☑ */
#define TASK_OPEN   "\xe2\x98\x90 " /* ☐ */
#define CELL_SEP    " \xe2\x94\x82 "

typedef struct {
	gboolean ordered;
	gboolean tight;
	guint    next_index;
} ListLevel;

typedef struct {
	GString  *out;
	GArray   *lists;       /* ListLevel, innermost last */
	guint     quote_depth;
	guint     table_cell;  /* cells already written in the current row */
	gsize     blank_mark;  /* out->len right after the last blank line */
	gboolean  item_fresh;  /* bullet written, first block stays on its line */
	gboolean  line_start;  /* next output starts a new line: emit prefixes */
} Renderer;

static void
append_quote_bars (Renderer *r,
		   gboolean  trailing_space)
{
	guint i;

	for (i = 0; i < r->quote_depth; i++) {
		g_string_append (r->out, QUOTE_BAR);
		if (trailing_space || i + 1 < r->quote_depth)
			g_string_append_c (r->out, ' ');
	}
}

/* Emits the line prefix if nothing has been written on this line yet. */
static void
begin_line (Renderer *r)
{
	guint i;

	if (!r->line_start)
		return;
	r->line_start = FALSE;

	append_quote_bars (r, TRUE);
	for (i = 0; i < r->lists->len; i++)
		g_string_append (r->out, "  ");
}

static void
end_line (Renderer *r)
{
	if (r->out->len > 0 && r->out->str[r->out->len - 1] != '\n')
		g_string_append_c (r->out, '\n');
	r->line_start = TRUE;
}

/* Separates two blocks with one empty line (carrying the quote bars). */
static void
blank_line (Renderer *r)
{
	if (r->out->len == 0)
		return;

	end_line (r);
	if (r->out->len == r->blank_mark)
		return;

	append_quote_bars (r, FALSE);
	g_string_append_c (r->out, '\n');
	r->blank_mark = r->out->len;
	r->line_start = TRUE;
}

/* Called when a block-level element starts. */
static void
block_break (Renderer *r)
{
	if (r->item_fresh) {
		/* The first block of a list item shares the bullet's line. */
		r->item_fresh = FALSE;
		return;
	}

	if (r->lists->len > 0 &&
	    g_array_index (r->lists, ListLevel, r->lists->len - 1).tight)
		end_line (r);
	else
		blank_line (r);
}

static void
append_escaped (Renderer      *r,
		const MD_CHAR *text,
		MD_SIZE        size)
{
	gchar *escaped = g_markup_escape_text (text, size);

	g_string_append (r->out, escaped);
	g_free (escaped);
}

/* Verbatim text may span several lines; each needs its prefix. */
static void
append_code (Renderer      *r,
	     const MD_CHAR *text,
	     MD_SIZE        size)
{
	while (size > 0) {
		const MD_CHAR *nl = memchr (text, '\n', size);
		MD_SIZE        len = nl ? (MD_SIZE) (nl - text) : size;

		begin_line (r);
		append_escaped (r, text, len);
		if (!nl)
			break;

		g_string_append_c (r->out, '\n');
		r->line_start = TRUE;
		text += len + 1;
		size -= len + 1;
	}
}

/* Pango markup accepts only the XML entities, so decode the rest. */
static void
append_entity (Renderer      *r,
	       const MD_CHAR *text,
	       MD_SIZE        size)
{
	static const gchar *xml_entities[] = { "&amp;", "&lt;", "&gt;", "&quot;", "&apos;" };
	guint i;

	for (i = 0; i < G_N_ELEMENTS (xml_entities); i++) {
		if (strlen (xml_entities[i]) == size &&
		    strncmp (xml_entities[i], text, size) == 0) {
			g_string_append_len (r->out, text, size);
			return;
		}
	}

	/* md4c strings are not NUL-terminated; copy before parsing the number. */
	if (size > 3 && size < 16 && text[1] == '#') {
		gchar     buf[16];
		gchar    *end = NULL;
		gboolean  hex = text[2] == 'x' || text[2] == 'X';
		gunichar  c;

		memcpy (buf, text, size);
		buf[size] = '\0';
		c = g_ascii_strtoull (buf + (hex ? 3 : 2), &end, hex ? 16 : 10);

		if (end == buf + size - 1 && c != 0 && g_unichar_validate (c)) {
			gchar utf8[8];
			gint  len = g_unichar_to_utf8 (c, utf8);

			append_escaped (r, utf8, len);
			return;
		}
	}

	append_escaped (r, text, size);
}

static int
enter_block (MD_BLOCKTYPE  type,
	     void         *detail,
	     void         *userdata)
{
	Renderer *r = userdata;

	switch (type) {
	case MD_BLOCK_DOC:
	case MD_BLOCK_HTML:
	case MD_BLOCK_THEAD:
	case MD_BLOCK_TBODY:
		break;
	case MD_BLOCK_QUOTE:
		block_break (r);
		r->quote_depth++;
		break;
	case MD_BLOCK_UL:
	case MD_BLOCK_OL: {
		ListLevel level = { 0, };

		if (type == MD_BLOCK_OL) {
			MD_BLOCK_OL_DETAIL *ol = detail;

			level.ordered = TRUE;
			level.tight = ol->is_tight != 0;
			level.next_index = ol->start;
		} else {
			level.tight = ((MD_BLOCK_UL_DETAIL *) detail)->is_tight != 0;
		}

		if (r->lists->len == 0) {
			block_break (r);
		} else {
			r->item_fresh = FALSE;
			end_line (r);
		}
		g_array_append_val (r->lists, level);
		break;
	}
	case MD_BLOCK_LI: {
		MD_BLOCK_LI_DETAIL *li = detail;
		ListLevel          *level;
		guint               i;

		level = &g_array_index (r->lists, ListLevel, r->lists->len - 1);
		if (level->tight)
			end_line (r);
		else
			blank_line (r);

		append_quote_bars (r, TRUE);
		for (i = 1; i < r->lists->len; i++)
			g_string_append (r->out, "  ");

		if (li->is_task)
			g_string_append (r->out, (li->task_mark == 'x' || li->task_mark == 'X') ?
					 TASK_DONE : TASK_OPEN);
		else if (level->ordered)
			g_string_append_printf (r->out, "%u. ", level->next_index++);
		else
			g_string_append (r->out, BULLET);

		r->line_start = FALSE;
		r->item_fresh = TRUE;
		break;
	}
	case MD_BLOCK_HR:
		block_break (r);
		begin_line (r);
		g_string_append (r->out, RULE);
		end_line (r);
		break;
	case MD_BLOCK_H: {
		unsigned level = ((MD_BLOCK_H_DETAIL *) detail)->level;

		block_break (r);
		begin_line (r);
		if (level == 1)
			g_string_append (r->out, "<span size=\"x-large\" weight=\"bold\">");
		else if (level == 2)
			g_string_append (r->out, "<span size=\"large\" weight=\"bold\">");
		else
			g_string_append (r->out, "<b>");
		break;
	}
	case MD_BLOCK_CODE:
		block_break (r);
		begin_line (r);
		g_string_append (r->out, "<tt>");
		break;
	case MD_BLOCK_P:
	case MD_BLOCK_TABLE:
		block_break (r);
		break;
	case MD_BLOCK_TR:
		end_line (r);
		r->table_cell = 0;
		break;
	case MD_BLOCK_TH:
	case MD_BLOCK_TD:
		begin_line (r);
		if (r->table_cell++ > 0)
			g_string_append (r->out, CELL_SEP);
		if (type == MD_BLOCK_TH)
			g_string_append (r->out, "<b>");
		break;
	}

	return 0;
}

static int
leave_block (MD_BLOCKTYPE  type,
	     void         *detail,
	     void         *userdata)
{
	Renderer *r = userdata;

	switch (type) {
	case MD_BLOCK_DOC:
	case MD_BLOCK_HTML:
	case MD_BLOCK_THEAD:
	case MD_BLOCK_TBODY:
	case MD_BLOCK_HR:
	case MD_BLOCK_TD:
		break;
	case MD_BLOCK_QUOTE:
		r->quote_depth--;
		end_line (r);
		break;
	case MD_BLOCK_UL:
	case MD_BLOCK_OL:
		g_array_remove_index (r->lists, r->lists->len - 1);
		end_line (r);
		break;
	case MD_BLOCK_LI:
		r->item_fresh = FALSE;
		end_line (r);
		break;
	case MD_BLOCK_H:
		g_string_append (r->out, ((MD_BLOCK_H_DETAIL *) detail)->level <= 2 ?
				 "</span>" : "</b>");
		end_line (r);
		break;
	case MD_BLOCK_CODE:
		/* Every code line ends with '\n'; do not leave an empty one. */
		if (r->out->str[r->out->len - 1] == '\n')
			g_string_truncate (r->out, r->out->len - 1);
		g_string_append (r->out, "</tt>");
		end_line (r);
		break;
	case MD_BLOCK_P:
	case MD_BLOCK_TABLE:
	case MD_BLOCK_TR:
		end_line (r);
		break;
	case MD_BLOCK_TH:
		g_string_append (r->out, "</b>");
		break;
	}

	return 0;
}

static const gchar *
span_tag (MD_SPANTYPE type,
	  gboolean    open)
{
	switch (type) {
	case MD_SPAN_EM:
	case MD_SPAN_IMG:
		return open ? "<i>" : "</i>";
	case MD_SPAN_STRONG:
		return open ? "<b>" : "</b>";
	case MD_SPAN_A:
	case MD_SPAN_U:
	case MD_SPAN_WIKILINK:
		return open ? "<u>" : "</u>";
	case MD_SPAN_CODE:
	case MD_SPAN_LATEXMATH:
	case MD_SPAN_LATEXMATH_DISPLAY:
		return open ? "<tt>" : "</tt>";
	case MD_SPAN_DEL:
		return open ? "<s>" : "</s>";
	}

	return "";
}

static int
enter_span (MD_SPANTYPE  type,
	    void        *detail,
	    void        *userdata)
{
	Renderer *r = userdata;

	r->item_fresh = FALSE;
	begin_line (r);
	g_string_append (r->out, span_tag (type, TRUE));

	return 0;
}

static int
leave_span (MD_SPANTYPE  type,
	    void        *detail,
	    void        *userdata)
{
	Renderer *r = userdata;

	g_string_append (r->out, span_tag (type, FALSE));

	return 0;
}

static int
text (MD_TEXTTYPE    type,
      const MD_CHAR *text,
      MD_SIZE        size,
      void          *userdata)
{
	Renderer *r = userdata;

	r->item_fresh = FALSE;

	switch (type) {
	case MD_TEXT_NULLCHAR:
		begin_line (r);
		g_string_append_unichar (r->out, 0xFFFD);
		break;
	case MD_TEXT_BR:
	case MD_TEXT_SOFTBR:
		/* Notes are written with manual line breaks; keep them. */
		g_string_append_c (r->out, '\n');
		r->line_start = TRUE;
		break;
	case MD_TEXT_ENTITY:
		begin_line (r);
		append_entity (r, text, size);
		break;
	case MD_TEXT_CODE:
	case MD_TEXT_LATEXMATH:
		append_code (r, text, size);
		break;
	case MD_TEXT_NORMAL:
	case MD_TEXT_HTML:
	default:
		begin_line (r);
		append_escaped (r, text, size);
		break;
	}

	return 0;
}

/**
 * ev_markdown_to_pango_markup:
 * @markdown: text of an annotation note, possibly using Markdown
 *
 * Renders @markdown as Pango markup suitable for gtk_label_set_markup(),
 * gtk_tooltip_set_markup() or a #GtkCellRendererText markup column.
 *
 * Headings, emphasis, inline and fenced code, strikethrough, lists, task
 * lists, block quotes, tables and horizontal rules are supported; links
 * are underlined only. Indented code blocks and raw HTML are disabled so
 * that plain-text notes render as written, and single newlines are kept
 * as line breaks rather than being joined into paragraphs.
 *
 * Returns: (transfer full): the markup; valid even if @markdown is not,
 * since anything Pango would reject falls back to escaped plain text.
 */
gchar *
ev_markdown_to_pango_markup (const gchar *markdown)
{
	Renderer   r;
	MD_PARSER  parser = { 0, };
	gchar     *valid = NULL;
	gchar     *result;
	gboolean   ok;

	if (!markdown || !*markdown)
		return g_strdup ("");

	if (!g_utf8_validate (markdown, -1, NULL))
		markdown = valid = g_utf8_make_valid (markdown, -1);

	r.out = g_string_sized_new (strlen (markdown) * 2);
	r.lists = g_array_new (FALSE, FALSE, sizeof (ListLevel));
	r.quote_depth = 0;
	r.table_cell = 0;
	r.blank_mark = 0;
	r.item_fresh = FALSE;
	r.line_start = TRUE;

	parser.flags = MD_FLAG_NOINDENTEDCODEBLOCKS |
		       MD_FLAG_NOHTML |
		       MD_FLAG_STRIKETHROUGH |
		       MD_FLAG_TASKLISTS |
		       MD_FLAG_TABLES |
		       MD_FLAG_PERMISSIVEAUTOLINKS;
	parser.enter_block = enter_block;
	parser.leave_block = leave_block;
	parser.enter_span = enter_span;
	parser.leave_span = leave_span;
	parser.text = text;

	ok = md_parse (markdown, strlen (markdown), &parser, &r) == 0;

	while (ok && r.out->len > 0 && r.out->str[r.out->len - 1] == '\n')
		g_string_truncate (r.out, r.out->len - 1);

	/* Consumers show nothing at all on a markup error, so never hand one out. */
	if (ok)
		ok = pango_parse_markup (r.out->str, -1, 0, NULL, NULL, NULL, NULL);

	if (ok)
		result = g_string_free (r.out, FALSE);
	else {
		g_string_free (r.out, TRUE);
		result = g_markup_escape_text (markdown, -1);
	}

	g_array_free (r.lists, TRUE);
	g_free (valid);

	return result;
}
