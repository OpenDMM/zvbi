/*
 *  libzvbi - Teletext formatter
 *
 *  Copyright (C) 2000, 2001 Michael H. Schimek
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 2 as
 *  published by the Free Software Foundation.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

/* $Id$ */

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#include "site_def.h"

#include <stdarg.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <strings.h> /* strncasecmp */
#include <ctype.h>
#include <assert.h>

#include "bcd.h"
#include "vt.h"
#include "export.h"
#include "vbi.h"
#include "hamm.h"
#include "lang.h"

extern const char _zvbi_intl_domainname[];

#ifndef _
#  ifdef ENABLE_NLS
#    include <libintl.h>
#    define _(String) dgettext (_zvbi_intl_domainname, String)
#    ifdef gettext_noop
#      define N_(String) gettext_noop (String)
#    else
#      define N_(String) (String)
#    endif
#  else /* Stubs that do something close enough.  */
#    define gettext(Msgid) ((const char *) (Msgid))
#    define dgettext(Domainname, Msgid) ((const char *) (Msgid))
#    define dcgettext(Domainname, Msgid, Category) ((const char *) (Msgid))
#    define ngettext(Msgid1, Msgid2, N) \
       ((N) == 1 ? (const char *) (Msgid1) : (const char *) (Msgid2))
#    define dngettext(Domainname, Msgid1, Msgid2, N) \
       ((N) == 1 ? (const char *) (Msgid1) : (const char *) (Msgid2))
#    define dcngettext(Domainname, Msgid1, Msgid2, N, Category) \
       ((N) == 1 ? (const char *) (Msgid1) : (const char *) (Msgid2))
#    define textdomain(Domainname) ((const char *) (Domainname))
#    define bindtextdomain(Domainname, Dirname) ((const char *) (Dirname))
#    define bind_textdomain_codeset(Domainname, Codeset) ((const char *) (Codeset))
#    define _(String) (String)
#    define N_(String) (String)
#  endif
#endif

#define DEBUG 0

#if DEBUG
#define printv(templ, args...) fprintf(stderr, templ ,##args)
#else
#define printv(templ, args...)						\
do {									\
	if (0)								\
		fprintf(stderr, templ ,##args);				\
} while (0)
#endif

#define ROWS			25
#define COLUMNS			40
#define EXT_COLUMNS		41
#define LAST_ROW		((ROWS - 1) * EXT_COLUMNS)

/*
 *  FLOF navigation
 */

static const vbi_color
flof_link_col[4] = { VBI_RED, VBI_GREEN, VBI_YELLOW, VBI_CYAN };

static inline void
flof_navigation_bar(vbi_page *pg, vt_page *vtp)
{
	vbi_char ac;
	int n, i, k, ii;

	memset(&ac, 0, sizeof(ac));

	ac.foreground	= VBI_WHITE;
	ac.background	= VBI_BLACK;
	ac.opacity	= pg->page_opacity[1];
	ac.unicode	= 0x0020;

	for (i = 0; i < EXT_COLUMNS; i++)
		pg->text[LAST_ROW + i] = ac;

	ac.link = TRUE;

	for (i = 0; i < 4; i++) {
		ii = i * 10 + 3;

		for (k = 0; k < 3; k++) {
			n = ((vtp->data.lop.link[i].pgno >> ((2 - k) * 4)) & 15) + '0';

			if (n > '9')
				n += 'A' - '9';

			ac.unicode = n;
			ac.foreground = flof_link_col[i];
			pg->text[LAST_ROW + ii + k] = ac;
			pg->nav_index[ii + k] = i;
		}

		pg->nav_link[i].pgno = vtp->data.lop.link[i].pgno;
		pg->nav_link[i].subno = vtp->data.lop.link[i].subno;
	}
}

static inline void
flof_links(vbi_page *pg, vt_page *vtp)
{
	vbi_char *acp = pg->text + LAST_ROW;
	int i, j, k, col = -1, start = 0;

	for (i = 0; i < COLUMNS + 1; i++) {
		if (i == COLUMNS || (acp[i].foreground & 7) != col) {
			for (k = 0; k < 4; k++)
				if ((int) flof_link_col[k] == col)
					break;

			if (k < 4 && !NO_PAGE(vtp->data.lop.link[k].pgno)) {
				/* Leading and trailing spaces not sensitive */

				for (j = i - 1; j >= start && acp[j].unicode == 0x0020; j--);

				for (; j >= start; j--) {
					acp[j].link = TRUE;
					pg->nav_index[j] = k;
				}

		    		pg->nav_link[k].pgno = vtp->data.lop.link[k].pgno;
		    		pg->nav_link[k].subno = vtp->data.lop.link[k].subno;
			}

			if (i >= COLUMNS)
				break;

			col = acp[i].foreground & 7;
			start = i;
		}

		if (start == i && acp[i].unicode == 0x0020)
			start++;
	}
}

/*
 *  TOP navigation
 */

static void character_set_designation(struct vbi_font_descr **font,
				      vt_extension *ext, vt_page *vtp);
static void screen_color(vbi_page *pg, int flags, int color);

static vbi_bool
top_label(vbi_decoder *vbi, vbi_page *pg, struct vbi_font_descr *font,
	  int index, int pgno, int foreground, int ff)
{
	int column = index * 13 + 1;
	vt_page *vtp;
	vbi_char *acp;
	ait_entry *ait;
	int i, j;

	acp = &pg->text[LAST_ROW + column];

	for (i = 0; i < 8; i++)
		if (vbi->vt.btt_link[i].type == 2) {
			vtp = vbi_cache_get(vbi,
					    vbi->vt.btt_link[i].pgno,
					    vbi->vt.btt_link[i].subno, 0x3f7f);
			if (!vtp) {
				printv("top ait page %x not cached\n", vbi->vt.btt_link[i].pgno);
				continue;
			} else if (vtp->function != PAGE_FUNCTION_AIT) {
				printv("no ait page %x\n", vtp->pgno);
				continue;
			}

			for (ait = vtp->data.ait, j = 0; j < 46; ait++, j++)
				if (ait->page.pgno == pgno) {
					pg->nav_link[index].pgno = pgno;
					pg->nav_link[index].subno = VBI_ANY_SUBNO;

					for (i = 11; i >= 0; i--)
						if (ait->text[i] > 0x20)
							break;

					if (ff && (i <= (11 - ff))) {
						acp += (11 - ff - i) >> 1;
						column += (11 - ff - i) >> 1;

						acp[i + 1].link = TRUE;
						pg->nav_index[column + i + 1] = index;

						acp[i + 2].unicode = 0x003E;
						acp[i + 2].foreground = foreground;
						acp[i + 2].link = TRUE;
						pg->nav_index[column + i + 2] = index;

						if (ff > 1) {
							acp[i + 3].unicode = 0x003E;
							acp[i + 3].foreground = foreground;
							acp[i + 3].link = TRUE;
							pg->nav_index[column + i + 3] = index;
						}
					} else {
						acp += (11 - i) >> 1;
						column += (11 - i) >> 1;
					}

					for (; i >= 0; i--) {
						acp[i].unicode = vbi_teletext_unicode(font->G0, font->subset,
							(ait->text[i] < 0x20) ? 0x20 : ait->text[i]);
						acp[i].foreground = foreground;
						acp[i].link = TRUE;
						pg->nav_index[column + i] = index;
					}

					return TRUE;
				}
		}

	return FALSE;
}

static __inline__ vbi_pgno
add_modulo			(vbi_pgno		pgno,
				 int			incr)
{
	return ((pgno - 0x100 + incr) & 0x7FF) + 0x100;
}

static inline void
top_navigation_bar(vbi_decoder *vbi, vbi_page *pg,
		   vt_page *vtp)
{
	vbi_char ac;
	vbi_pgno pgno1;
	int i, got;

	printv("PAGE MIP/BTT: %d\n", vbi->vt.page_info[vtp->pgno - 0x100].code);

	memset(&ac, 0, sizeof(ac));

	ac.foreground	= 32 + VBI_WHITE;
	ac.background	= 32 + VBI_BLACK;
	ac.opacity	= pg->page_opacity[1];
	ac.unicode	= 0x0020;

	for (i = 0; i < EXT_COLUMNS; i++)
		pg->text[LAST_ROW + i] = ac;

	if (pg->page_opacity[1] != VBI_OPAQUE)
		return;

	pgno1 = add_modulo (vtp->pgno, 1);

	for (i = vtp->pgno; i != pgno1; i = add_modulo (i, -1))
		if (vbi->vt.page_info[i - 0x100].code == VBI_TOP_BLOCK ||
		    vbi->vt.page_info[i - 0x100].code == VBI_TOP_GROUP) {
			top_label(vbi, pg, pg->font[0], 0, i, 32 + VBI_WHITE, 0);
			break;
		}

	for (i = pgno1, got = FALSE; i != vtp->pgno; i = add_modulo (i, 1))
		switch (vbi->vt.page_info[i - 0x100].code) {
		case VBI_TOP_BLOCK:
			top_label(vbi, pg, pg->font[0], 2, i, 32 + VBI_YELLOW, 2);
			return;

		case VBI_TOP_GROUP:
			if (!got) {
				top_label(vbi, pg, pg->font[0], 1, i, 32 + VBI_GREEN, 1);
				got = TRUE;
			}

			break;
		}
}

static ait_entry *
next_ait(vbi_decoder *vbi, int pgno, int subno, vt_page **mvtp)
{
	vt_page *vtp;
	ait_entry *ait, *mait = NULL;
	int mpgno = 0xFFF, msubno = 0xFFFF;
	int i, j;

	*mvtp = NULL;

	for (i = 0; i < 8; i++) {
		if (vbi->vt.btt_link[i].type == 2) {
			vtp = vbi_cache_get(vbi,
					    vbi->vt.btt_link[i].pgno, 
					    vbi->vt.btt_link[i].subno, 0x3f7f);

			if (!vtp) {
				printv("top ait page %x not cached\n", vbi->vt.btt_link[i].pgno);
				continue;
			} else if (vtp->function != PAGE_FUNCTION_AIT) {
				printv("no ait page %x\n", vtp->pgno);
				continue;
			}

			for (ait = vtp->data.ait, j = 0; j < 46; ait++, j++) {
				if (!ait->page.pgno)
					break;

				if (ait->page.pgno < pgno
				    || (ait->page.pgno == pgno && ait->page.subno <= subno))
					continue;

				if (ait->page.pgno > mpgno
				    || (ait->page.pgno == mpgno && ait->page.subno > msubno))
					continue;

				mait = ait;
				mpgno = ait->page.pgno;
				msubno = ait->page.subno;
				*mvtp = vtp;
			}
		}
	}

	return mait;
}

static int
top_index(vbi_decoder *vbi, vbi_page *pg, int subno)
{
	vt_page *vtp;
	vbi_char ac, *acp;
	ait_entry *ait;
	int i, j, k, n, lines;
	int xpgno, xsubno;
	vt_extension *ext;
	char *index_str;

	pg->vbi = vbi;

	subno = vbi_bcd2dec(subno);

	pg->rows = ROWS;
	pg->columns = EXT_COLUMNS;

	pg->dirty.y0 = 0;
	pg->dirty.y1 = ROWS - 1;
	pg->dirty.roll = 0;

	ext = &vbi->vt.magazine[0].extension;

	screen_color(pg, 0, 32 + VBI_BLUE);

	vbi_transp_colormap(vbi, pg->color_map, ext->color_map, 40);

	pg->drcs_clut = ext->drcs_clut;

	pg->page_opacity[0] = VBI_OPAQUE;
	pg->page_opacity[1] = VBI_OPAQUE;
	pg->boxed_opacity[0] = VBI_OPAQUE;
	pg->boxed_opacity[1] = VBI_OPAQUE;

	memset(pg->drcs, 0, sizeof(pg->drcs));

	memset(&ac, 0, sizeof(ac));

	ac.foreground	= VBI_BLACK; // 32 + VBI_BLACK;
	ac.background	= 32 + VBI_BLUE;
	ac.opacity	= VBI_OPAQUE;
	ac.unicode	= 0x0020;
	ac.size		= VBI_NORMAL_SIZE;

	for (i = 0; i < EXT_COLUMNS * ROWS; i++)
		pg->text[i] = ac;

	ac.size = VBI_DOUBLE_SIZE;

	/* FIXME */
	/* TRANSLATORS: Title of TOP Index page,
	   for now please Latin-1 or ASCII only */
	index_str = _("TOP Index");

	for (i = 0; index_str[i]; i++) {
		ac.unicode = index_str[i];
		pg->text[1 * EXT_COLUMNS + 2 + i * 2] = ac;
	}

	ac.size = VBI_NORMAL_SIZE;

	acp = &pg->text[4 * EXT_COLUMNS];
	lines = 17;
	xpgno = 0;
	xsubno = 0;

	while ((ait = next_ait(vbi, xpgno, xsubno, &vtp))) {
		xpgno = ait->page.pgno;
		xsubno = ait->page.subno;

		/* No docs, correct? */
		character_set_designation(pg->font, ext, vtp);

		if (subno > 0) {
			if (lines-- == 0) {
				subno--;
				lines = 17;
			}

			continue;
		} else if (lines-- <= 0)
			continue;

		for (i = 11; i >= 0; i--)
			if (ait->text[i] > 0x20)
				break;

		switch (vbi->vt.page_info[ait->page.pgno - 0x100].code) {
		case VBI_TOP_GROUP:
			k = 3;
			break;

		default:
    			k = 1;
		}

		for (j = 0; j <= i; j++) {
			acp[k + j].unicode = vbi_teletext_unicode(pg->font[0]->G0,
				pg->font[0]->subset, (ait->text[j] < 0x20) ? 0x20 : ait->text[j]);
		}

		for (k += i + 2; k <= 33; k++)
			acp[k].unicode = '.';

		for (j = 0; j < 3; j++) {
			n = ((ait->page.pgno >> ((2 - j) * 4)) & 15) + '0';

			if (n > '9')
				n += 'A' - '9';

			acp[j + 35].unicode = n;
 		}

		acp += EXT_COLUMNS;
	}

	return 1;
}

struct pex26 {
	signed			month		: 8;  /* 0 ... 11 */
	signed			day		: 8;  /* 0 ... 30 */
	signed			at1		: 16; /* min since 00:00 */
	signed			at2		: 16; /* min since 00:00 */
	signed			length		: 16; /* min */
	unsigned		x26_cni		: 16; /* see tables.c */
	unsigned		pty		: 8;
	signed			lto		: 8;  /* +- 1/4 hr */
	signed			row		: 8;  /* title 1 ... 23 */
	signed			column		: 8;  /* title 0 ... 39 */
	unsigned		caf		: 1;
	unsigned				: 15;
};

static void
dump_pex26(struct pex26 *pt, int n)
{
	int i;

	for (i = 0; i < n; i++, pt++)
		fprintf(stderr, "%2d: %02d-%02d %d:%02d (%d:%02d) +%d, "
			"cni=%04x pty=%02x lto=%d tit=%d:%d caf=%d\n",
			i, pt->month, pt->day,
			pt->at1 / 60, pt->at1 % 60,
			pt->at2 / 60, pt->at2 % 60,
			pt->length,
			pt->x26_cni, pt->pty, pt->lto,
			pt->row, pt->column,
			pt->caf);
}

#if 0

/*

type	pre	text		____      post       ____
                               /                         \
AT-1	+	zz.zz		+	%		<
AT-1	+	zz.zz-zz.zz	+	%		<
PTL	++	title		++	%%	::	<
AT-1	%	zz.zz		+	%		<
PW*)	%	hh
LTO	%	0zz		+	%		<
LTO	%	9zz		+	%		<
AT-2	%	zzzz		+	%		<
CNI*)	%	hhzzz		+	%		<
AD*)	%	zzzzzz		+	%		<
PTL	%%	title		++	%%	::	<
AT-2	:	zzzz		+	%		<
AD	:	zzzzzz		+	%		<
PW	:%	hh		+	%		<
PTY	:%	Fhh		+	%		<
AT-2	:%	zzzz		+	%		<
CNI	:%	hhzzz		+	%		<
AD	:%	zzzzzz		+	%		<

+  colour code
:  magenta
%  conceal

*) permitted when CNI, AD, PW combine

Note ETS 300 231 Table 4 is wrong: '%' = 0x18; ',' = '+' | '%' | '<'

*/

/* to be rewritten */

struct program_entry {
	int			start;
	int			stop;
	int			at2;
	int			ad;
	int			cni;
	int			pty;
	int			lto;
	uint16_t		title[200];
};

#define PMA_COLOUR /* 0x01, 0x02, 0x03, 0x04, 0x06, 0x07 */
#define PMA_MAGENTA 0x05
#define PMA_CONCEAL 0x18

#define IS_PMA_CTRL(c) (((c) >= 0x01 && (c) <= 0x07) || (c) == PMA_CONCEAL)

static int
bcd2time(int bcd)
{
	int sec = bcd & 15;
	int min = (bcd >> 8) & 15;

	if (sec > 9 || min > 9 || (bcd & 0x00FF) > 0x0059)
		return -1;
//#warning hour check

	return sec * 1 + min * 60
		+ ((bcd >> 4) & 15) * 10
		+ ((bcd >> 12) & 15) * 600;
}

static int
pdc_method_a(vbi_page *pg, vt_page *vtp, struct program_entry *pe)
{
	int row, column;
	int i;

//	memset(pe, -1, sizeof(*pe));

	i = 40;

	for (row = 1; row <= 23; row++) {
		for (column = 0; column <= 38;) {
			int ctrl1 = vbi_parity(vtp->data.lop.raw[row][column]);
			int ctrl2 = vbi_parity(vtp->data.lop.raw[row][column + 1]);

fprintf(stderr, "%d %d %02x %02x\n", row, column, ctrl1, ctrl2);

			if ((ctrl1 | ctrl2) < 0) {
				return 0; /* hamming error */
			} else if (!IS_PMA_CTRL(ctrl1)) {
				column++;
				continue;
			}

			if (ctrl1 == ctrl2 && ctrl1 != PMA_MAGENTA) {
fprintf(stderr, "PTL %d %d\n", row, column);
				/* title */
				column += 2;fprintf(stderr, "%d %d %02x %02x\n", row, column, ctrl1, ctrl2);


			} else {
				/* numeral */
				int digits, sep, value;
fprintf(stderr, "NUM %d %d\n", row, column);
				column += (ctrl1 == PMA_MAGENTA && ctrl2 == PMA_CONCEAL) ? 2 : 1;

				sep = 0;
				value = 0;

				for (digits = 0; column < 40; column++) {
					int c = vbi_parity(vtp->data.lop.raw[row][column]);

					if (IS_PMA_CTRL(c)) {
						break;
					} else if (c >= 0x30 && c <= 0x39) {
						value = value * 16 + c - 0x30;
						digits++;
					} else if (c >= 0x41 && c <= 0x46) {
						if (digits >= 3)
							goto invalid_pattern;
						value = value * 16 + c + (0x0A - 0x41);
						digits++;
					} else if (c == 0x2E) {
						if (digits != 2 && digits != 6)
							goto invalid_pattern;
						sep |= 1 << digits;
					} else if (c == 0x2D) {
						if (digits != 4)
							goto invalid_pattern;
						sep |= 1 << 4;
					} else
						goto invalid_pattern;
				}

				if (sep) {
					if (ctrl1 == PMA_MAGENTA)
						goto invalid_pattern;
					if (ctrl1 == PMA_CONCEAL && digits != 4)
						goto invalid_pattern;
				}

				switch (digits) {
					int start, stop;

				case 2:
					/* Actually ctrl1 only permitted when combined */
					if (ctrl1 != PMA_CONCEAL && ctrl2 != PMA_CONCEAL)
						goto invalid_pattern;
fprintf(stderr, "PW %02x\n", value);
					/* PW */
					break;

				case 3:
					if (ctrl1 == PMA_CONCEAL) {
						if (value >= 0x100 && value < 0x900)
							goto invalid_pattern;
fprintf(stderr, "LTO %03x\n", value);
						/* LTO */
					} else if (ctrl2 == PMA_CONCEAL) {
						if ((value -= 0xF00) < 0)
						goto invalid_pattern;
fprintf(stderr, "PTY %02x\n", value);
						/* PTY */
					} else
						goto invalid_pattern;

				case 4:
					start = bcd2time(value);

					if (start < 0)
						goto invalid_pattern;

					if (sep) {
						if (ctrl1 == PMA_MAGENTA)
							goto invalid_pattern;
fprintf(stderr, "AT-1 %04x\n", value);
						; /* AT-1 short */
					} else if (ctrl1 == PMA_MAGENTA || ctrl1 == PMA_CONCEAL) {
fprintf(stderr, "AT-2 %04x\n", value);
						; /* AT-2 */
					} else
						goto invalid_pattern;

					break;

				case 5:
					/* Actually ctrl1 only permitted when combined */
					if ((ctrl1 != PMA_CONCEAL && ctrl2 != PMA_CONCEAL)
					    || (value & 0x00F00) > 0x00900)
						goto invalid_pattern;

					/* CNI */
fprintf(stderr, "CNI %05x\n", value);
					break;

				case 6:
					/* Actually ctrl1 only permitted when combined */
					if (ctrl1 != PMA_CONCEAL && ctrl2 != PMA_CONCEAL)
						goto invalid_pattern;
					/* AD */
fprintf(stderr, "AD %06x\n", value);
					break;

				case 8:
					start = bcd2time(value >> 16); 
					stop = bcd2time(value);

					if ((start | stop) < 0
					    || ctrl1 == PMA_MAGENTA || ctrl1 == PMA_CONCEAL
					    || sep != ((1 << 2) + (1 << 4) + (1 << 6)))
						goto invalid_pattern;

					/* AT-1 long */
fprintf(stderr, "AT1 %08x\n", value);
					break;

				default:
				invalid_pattern:
					continue;
				}
			}
		}
	}

	return 0; /* invalid */
}

#endif

/*
 *  Zapzilla navigation
 */

static int
keyword(vbi_link *ld, uint8_t *p, int column,
	int pgno, int subno, int *back)
{
	uint8_t *s = p + column;
	int i, j, k, l;

	ld->type = VBI_LINK_NONE;
	ld->name[0] = 0;
	ld->url[0] = 0;
	ld->pgno = 0;
	ld->subno = VBI_ANY_SUBNO;
	*back = 0;

	if (isdigit(*s)) {
		for (i = 0; isdigit(s[i]); i++)
			ld->pgno = ld->pgno * 16 + (s[i] & 15);

		if (isdigit(s[-1]) || i > 3)
			return i;

		if (i == 3) {
			if (ld->pgno >= 0x100 && ld->pgno <= 0x899)
				ld->type = VBI_LINK_PAGE;

			return i;
		}

		if (s[i] != '/' && s[i] != ':')
			return i;

		s += i += 1;

		for (ld->subno = j = 0; isdigit(s[j]); j++)
			ld->subno = ld->subno * 16 + (s[j] & 15);

		if (j > 1 || subno != ld->pgno || ld->subno > 0x99)
			return i + j;

		if (ld->pgno == ld->subno)
			ld->subno = 0x01;
		else
			ld->subno = vbi_add_bcd(ld->pgno, 0x01);

		ld->type = VBI_LINK_SUBPAGE;
		ld->pgno = pgno;

		return i + j;
	} else if (!strncasecmp((char *) s, "https://", i = 8)) {
		ld->type = VBI_LINK_HTTP;
	} else if (!strncasecmp((char *) s, "http://", i = 7)) {
		ld->type = VBI_LINK_HTTP;
	} else if (!strncasecmp((char *) s, "www.", i = 4)) {
		ld->type = VBI_LINK_HTTP;
		strcpy((char *) ld->url, "http://");
	} else if (!strncasecmp((char *) s, "ftp://", i = 6)) {
		ld->type = VBI_LINK_FTP;
	} else if (*s == '@' || *s == 0xA7) {
		ld->type = VBI_LINK_EMAIL;
		strcpy((char *) ld->url, "mailto:");
		i = 1;
	} else if (!strncasecmp((char *) s, "(at)", i = 4)) {
		ld->type = VBI_LINK_EMAIL;
		strcpy((char *) ld->url, "mailto:");
	} else if (!strncasecmp((char *) s, "(a)", i = 3)) {
		ld->type = VBI_LINK_EMAIL;
		strcpy((char *) ld->url, "mailto:");
	} else
		return 1;

	for (j = k = l = 0;;) {
		// RFC 1738
		while (isalnum(s[i + j]) || strchr("%&/=?+-~:;@_", s[i + j])) {
			j++;
			l++;
		}

		if (s[i + j] == '.') {
			if (l < 1)
				return i;		
			l = 0;
			j++;
			k++;
		} else
			break;
	}

	if (k < 1 || l < 1) {
		ld->type = VBI_LINK_NONE;
		return i;
	}

	k = 0;

	if (ld->type == VBI_LINK_EMAIL) {
		for (; isalnum(s[k - 1]) || strchr("-~._", s[k - 1]); k--);

		if (k == 0) {
			ld->type = VBI_LINK_NONE;
			return i;
		}

		*back = k;

		strncat((char *) ld->url, (char *) s + k, -k);
		strcat((char *) ld->url, "@");
		strncat((char *) ld->url, (char *) s + i, j);
	} else
		strncat((char *) ld->url, (char *) s + k, i + j - k);

	return i + j;
}

static inline void
zap_links(vbi_page *pg, int row)
{
	unsigned char buffer[43]; /* One row, two spaces on the sides and NUL */
	vbi_link ld;
	vbi_char *acp;
	vbi_bool link[43];
	int i, j, n, b;

	acp = &pg->text[row * EXT_COLUMNS];

	for (i = j = 0; i < COLUMNS; i++) {
		if (acp[i].size == VBI_OVER_TOP || acp[i].size == VBI_OVER_BOTTOM)
			continue;
		buffer[j + 1] = (acp[i].unicode >= 0x20 && acp[i].unicode <= 0xFF) ?
			acp[i].unicode : 0x20;
		j++;
	}

	buffer[0] = ' '; 
	buffer[j + 1] = ' ';
	buffer[j + 2] = 0;

	for (i = 0; i < COLUMNS; i += n) { 
		n = keyword(&ld, buffer, i + 1,
			pg->pgno, pg->subno, &b);

		for (j = b; j < n; j++)
			link[i + j] = (ld.type != VBI_LINK_NONE);
	}

	for (i = j = 0; i < COLUMNS; i++) {
		acp[i].link = link[j];

		if (acp[i].size == VBI_OVER_TOP || acp[i].size == VBI_OVER_BOTTOM)
			continue;
		j++;
	}
}

/**
 * @param pg With vbi_fetch_vt_page() obtained vbi_page.
 * @param column Column 0 ... pg->columns - 1 of the character in question.
 * @param row Row 0 ... pg->rows - 1 of the character in question.
 * @param ld Place to store information about the link.
 * 
 * A vbi_page (in practice only Teletext pages) may contain hyperlinks
 * such as HTTP URLs, e-mail addresses or links to other pages. Characters
 * being part of a hyperlink have a set vbi_char->link flag, this function
 * returns a more verbose description of the link.
 */
void
vbi_resolve_link(vbi_page *pg, int column, int row, vbi_link *ld)
{
	unsigned char buffer[43];
	vbi_char *acp;
	int i, j, b;

	assert(column >= 0 && column < EXT_COLUMNS);

	ld->nuid = pg->nuid;

	acp = &pg->text[row * EXT_COLUMNS];

	if (row == (ROWS - 1) && acp[column].link) {
		i = pg->nav_index[column];

		ld->type = VBI_LINK_PAGE;
		ld->pgno = pg->nav_link[i].pgno;
		ld->subno = pg->nav_link[i].subno;

		return;
	}

	if (row < 1 || row > 23 || column >= COLUMNS || pg->pgno < 0x100) {
		ld->type = VBI_LINK_NONE;
		return;
	}

	for (i = j = b = 0; i < COLUMNS; i++) {
		if (acp[i].size == VBI_OVER_TOP || acp[i].size == VBI_OVER_BOTTOM)
			continue;
		if (i < column && !acp[i].link)
			j = b = -1;

		buffer[j + 1] = (acp[i].unicode >= 0x20 && acp[i].unicode <= 0xFF) ?
			acp[i].unicode : 0x20;

		if (b <= 0) {
			if (buffer[j + 1] == ')' && j > 2) {
				if (!strncasecmp((char *) buffer + j + 1 - 3, "(at", 3))
					b = j - 3;
				else if (!strncasecmp((char *) buffer + j + 1 - 2, "(a", 2))
					b = j - 2;
			} else if (buffer[j + 1] == '@' || buffer[j + 1] == 167)
				b = j;
		}

		j++;
	}

	buffer[0] = ' ';
	buffer[j + 1] = ' ';
	buffer[j + 2] = 0;

	keyword(ld, buffer, 1, pg->pgno, pg->subno, &i);

	if (ld->type == VBI_LINK_NONE)
		keyword(ld, buffer, b + 1, pg->pgno, pg->subno, &i);
}

/**
 * @param pg With vbi_fetch_vt_page() obtained vbi_page.
 * @param ld Place to store information about the link.
 * 
 * All Teletext pages have a built-in home link, by default
 * page 100, but can also be the magazine intro page or another
 * page selected by the editor.
 */
void
vbi_resolve_home(vbi_page *pg, vbi_link *ld)
{
	if (pg->pgno < 0x100) {
		ld->type = VBI_LINK_NONE;
		return;
	}

	ld->type = VBI_LINK_PAGE;
	ld->pgno = pg->nav_link[5].pgno;
	ld->subno = pg->nav_link[5].subno;
}

static inline void
ait_title(vbi_decoder *vbi, vt_page *vtp, ait_entry *ait, char *buf)
{
	struct vbi_font_descr *font[2];
	int i;

	character_set_designation(font, &vbi->vt.magazine[0].extension, vtp);

	for (i = 11; i >= 0; i--)
		if (ait->text[i] > 0x20)
			break;
	buf[i + 1] = 0;

	for (; i >= 0; i--) {
		unsigned int unicode = vbi_teletext_unicode(
			font[0]->G0, font[0]->subset,
			(ait->text[i] < 0x20) ?	0x20 : ait->text[i]);

		buf[i] = (unicode >= 0x20 && unicode <= 0xFF) ? unicode : 0x20;
	}
}

/**
 * @param vbi Initialized vbi decoding context.
 * @param pgno Page number, see vbi_pgno.
 * @param subno Subpage number.
 * @param buf Place to store the title, Latin-1 format, at least
 *   41 characters including the terminating zero.
 * 
 * Given a Teletext page number this function tries to deduce a
 * page title for bookmarks or other purposes, mainly from navigation
 * data. (XXX TODO: FLOF)
 * 
 * @return
 * @c TRUE if a title has been found.
 */
vbi_bool
vbi_page_title(vbi_decoder *vbi, int pgno, int subno, char *buf)
{
	vt_page *vtp;
	ait_entry *ait;
	int i, j;

	subno = subno;

	if (vbi->vt.top) {
		for (i = 0; i < 8; i++)
			if (vbi->vt.btt_link[i].type == 2) {
				vtp = vbi_cache_get(vbi,
						    vbi->vt.btt_link[i].pgno, 
						    vbi->vt.btt_link[i].subno, 0x3f7f);

				if (!vtp) {
					printv("p/t top ait page %x not cached\n", vbi->vt.btt_link[i].pgno);
					continue;
				} else if (vtp->function != PAGE_FUNCTION_AIT) {
					printv("p/t no ait page %x\n", vtp->pgno);
					continue;
				}

				for (ait = vtp->data.ait, j = 0; j < 46; ait++, j++)
					if (ait->page.pgno == pgno) {
						ait_title(vbi, vtp, ait, buf);
						return TRUE;
					}
			}
	} else {
		/* find a FLOF link and the corresponding label */
	}

	return FALSE;
}

/*
 *  Teletext page formatting
 */

static void
character_set_designation(struct vbi_font_descr **font,
			  vt_extension *ext, vt_page *vtp)
{
	int i;

#ifdef libzvbi_TTX_OVERRIDE_CHAR_SET

	font[0] = vbi_font_descriptors + libzvbi_TTX_OVERRIDE_CHAR_SET;
	font[1] = vbi_font_descriptors + libzvbi_TTX_OVERRIDE_CHAR_SET;

	fprintf(stderr, "override char set with %d\n",
		libzvbi_TTX_OVERRIDE_CHAR_SET);
#else

	font[0] = vbi_font_descriptors + 0;
	font[1] = vbi_font_descriptors + 0;

	for (i = 0; i < 2; i++) {
		int char_set = ext->char_set[i];

		if (VALID_CHARACTER_SET(char_set))
			font[i] = vbi_font_descriptors + char_set;

		char_set = (char_set & ~7) + vtp->national;

		if (VALID_CHARACTER_SET(char_set))
			font[i] = vbi_font_descriptors + char_set;
	}
#endif
}

static void
screen_color(vbi_page *pg, int flags, int color)
{ 
	pg->screen_color = color;

	if (color == VBI_TRANSPARENT_BLACK
	    || (flags & (C5_NEWSFLASH | C6_SUBTITLE)))
		pg->screen_opacity = VBI_TRANSPARENT_SPACE;
	else
		pg->screen_opacity = VBI_OPAQUE;
}

#define elements(array) (sizeof(array) / sizeof(array[0]))

static vt_triplet *
resolve_obj_address(vbi_decoder *vbi, object_type type,
	int pgno, object_address address, page_function function,
	int *remaining)
{
	int s1, packet, pointer;
	vt_page *vtp;
	vt_triplet *trip;
	int i;

	s1 = address & 15;
	packet = ((address >> 7) & 3);
	i = ((address >> 5) & 3) * 3 + type;

	printv("obj invocation, source page %03x/%04x, "
		"pointer packet %d triplet %d\n", pgno, s1, packet + 1, i);

	vtp = vbi_cache_get(vbi, pgno, s1, 0x000F);

	if (!vtp) {
		printv("... page not cached\n");
		return 0;
	}

	if (vtp->function == PAGE_FUNCTION_UNKNOWN) {
		if (!(vtp = vbi_convert_page(vbi, vtp, TRUE, function))) {
			printv("... no g/pop page or hamming error\n");
			return 0;
		}
	} else if (vtp->function == PAGE_FUNCTION_POP)
		vtp->function = function;
	else if (vtp->function != function) {
		printv("... source page wrong function %d, expected %d\n",
			vtp->function, function);
		return 0;
	}

	pointer = vtp->data.pop.pointer[packet * 24 + i * 2 + ((address >> 4) & 1)];

	printv("... triplet pointer %d\n", pointer);

	if (pointer > 506) {
		printv("... triplet pointer out of bounds (%d)\n", pointer);
		return 0;
	}

	if (DEBUG) {
		packet = (pointer / 13) + 3;

		if (packet <= 25)
			printv("... object start in packet %d, triplet %d (pointer %d)\n",
				packet, pointer % 13, pointer);
		else
			printv("... object start in packet 26/%d, triplet %d (pointer %d)\n",
				packet - 26, pointer % 13, pointer);	
	}

	trip = vtp->data.pop.triplet + pointer;
	*remaining = elements(vtp->data.pop.triplet) - (pointer+1);

	printv("... obj def: ad 0x%02x mo 0x%04x dat %d=0x%x\n",
		trip->address, trip->mode, trip->data, trip->data);

	address ^= trip->address << 7;
	address ^= trip->data;

	if (trip->mode != (type + 0x14) || (address & 0x1FF)) {
		printv("... no object definition\n");
		return 0;
	}

	return trip + 1;
}

/* FIXME: panels */

static vbi_bool
enhance(vbi_decoder *vbi, vt_magazine *mag, vt_extension *ext,
	vbi_page *pg, vt_page *vtp,
	object_type type, vt_triplet *p,
	int max_triplets,
	int inv_row, int inv_column,
	vbi_wst_level max_level, vbi_bool header_only,
	struct pex26 *ptable)
{
	vbi_char ac, mac, *acp;
	int active_column, active_row;
	int offset_column, offset_row;
	int row_color, next_row_color;
	struct vbi_font_descr *font;
	int invert;
	int drcs_s1[2];
	struct pex26 *pt, ptmp;
	int pdc_hr;

	/* XXX nested function not portable, to be removed */
	void
	flush(int column)
	{
		int row = inv_row + active_row;
		int i;

		if (row >= ROWS)
			return;

		if (type == OBJ_TYPE_PASSIVE && !mac.unicode) {
			active_column = column;
			return;
		}

		printv("flush [%04x%c,F%d%c,B%d%c,S%d%c,O%d%c,H%d%c] %d ... %d\n",
			ac.unicode, mac.unicode ? '*' : ' ',
			ac.foreground, mac.foreground ? '*' : ' ',
			ac.background, mac.background ? '*' : ' ',
			ac.size, mac.size ? '*' : ' ',
			ac.opacity, mac.opacity ? '*' : ' ',
			ac.flash, mac.flash ? '*' : ' ',
			active_column, column - 1);

		for (i = inv_column + active_column; i < inv_column + column;) {
			vbi_char c;

			if (i > 39)
				break;

			c = acp[i];

			if (mac.underline) {
				int u = ac.underline;

				if (!mac.unicode)
					ac.unicode = c.unicode;

				if (vbi_is_gfx(ac.unicode)) {
					if (u)
						ac.unicode &= ~0x20; /* separated */
					else
						ac.unicode |= 0x20; /* contiguous */
					mac.unicode = ~0;
					u = 0;
				}

				c.underline = u;
			}
			if (mac.foreground)
				c.foreground = (ac.foreground == VBI_TRANSPARENT_BLACK) ?
					row_color : ac.foreground;
			if (mac.background)
				c.background = (ac.background == VBI_TRANSPARENT_BLACK) ?
					row_color : ac.background;
			if (invert) {
				int t = c.foreground;

				c.foreground = c.background;
				c.background = t;
			}
			if (mac.opacity)
				c.opacity = ac.opacity;
			if (mac.flash)
				c.flash = ac.flash;
			if (mac.conceal)
				c.conceal = ac.conceal;
			if (mac.unicode) {
				c.unicode = ac.unicode;
				mac.unicode = 0;

				if (mac.size)
					c.size = ac.size;
				else if (c.size > VBI_DOUBLE_SIZE)
					c.size = VBI_NORMAL_SIZE;
			}

			acp[i] = c;

			if (type == OBJ_TYPE_PASSIVE)
				break;

			i++;

			if (type != OBJ_TYPE_PASSIVE
			    && type != OBJ_TYPE_ADAPTIVE) {
				int raw;

				raw = (row == 0 && i < 9) ?
					0x20 : vbi_unpar8 (vtp->data.lop.raw[row][i - 1]);

				/* set-after spacing attributes cancelling non-spacing */

				switch (raw) {
				case 0x00 ... 0x07:	/* alpha + foreground color */
				case 0x10 ... 0x17:	/* mosaic + foreground color */
					printv("... fg term %d %02x\n", i, raw);
					mac.foreground = 0;
					mac.conceal = 0;
					break;

				case 0x08:		/* flash */
					mac.flash = 0;
					break;

				case 0x0A:		/* end box */
				case 0x0B:		/* start box */
					if (i < COLUMNS && vbi_unpar8 (vtp->data.lop.raw[row][i]) == raw) {
						printv("... boxed term %d %02x\n", i, raw);
						mac.opacity = 0;
					}

					break;

				case 0x0D:		/* double height */
				case 0x0E:		/* double width */
				case 0x0F:		/* double size */
					printv("... size term %d %02x\n", i, raw);
					mac.size = 0;
					break;
				}

				if (i > 39)
					break;

				raw = (row == 0 && i < 8) ?
					0x20 : vbi_unpar8 (vtp->data.lop.raw[row][i]);

				/* set-at spacing attributes cancelling non-spacing */

				switch (raw) {
				case 0x09:		/* steady */
					mac.flash = 0;
					break;

				case 0x0C:		/* normal size */
					printv("... size term %d %02x\n", i, raw);
					mac.size = 0;
					break;

				case 0x18:		/* conceal */
					mac.conceal = 0;
					break;

					/*
					 *  Non-spacing underlined/separated display attribute
					 *  cannot be cancelled by a subsequent spacing attribute.
					 */

				case 0x1C:		/* black background */
				case 0x1D:		/* new background */
					printv("... bg term %d %02x\n", i, raw);
					mac.background = 0;
					break;
				}
			}
		}

		active_column = column;
	}

	/* XXX nested function not portable, to be removed */
	void
	flush_row(void)
	{
		if (type == OBJ_TYPE_PASSIVE || type == OBJ_TYPE_ADAPTIVE)
			flush(active_column + 1);
		else
			flush(COLUMNS);

		if (type != OBJ_TYPE_PASSIVE)
			memset(&mac, 0, sizeof(mac));
	}

	active_column = 0;
	active_row = 0;

	acp = &pg->text[(inv_row + 0) * EXT_COLUMNS];

	offset_column = 0;
	offset_row = 0;

	row_color =
	next_row_color = ext->def_row_color;

	drcs_s1[0] = 0; /* global */
	drcs_s1[1] = 0; /* normal */

	memset(&ac, 0, sizeof(ac));
	memset(&mac, 0, sizeof(mac));

	invert = 0;

	if (type == OBJ_TYPE_PASSIVE) {
		ac.foreground = VBI_WHITE;
		ac.background = VBI_BLACK;
		ac.opacity = pg->page_opacity[1];

		mac.foreground = ~0;
		mac.background = ~0;
		mac.opacity = ~0;
		mac.size = ~0;
		mac.underline = ~0;
		mac.conceal = ~0;
		mac.flash = ~0;
	}

	font = pg->font[0];

	if (ptable) {
		ptmp.month	= -1;
		ptmp.at1	= -1; /* n/a */
		ptmp.length	= 0;
		ptmp.x26_cni	= 0;
		ptmp.pty	= 0;
		ptmp.lto	= 0;

		pt = ptable - 1;
	} else
		pt = &ptmp;

	pdc_hr = 0;

	for (; max_triplets>0; p++, max_triplets--) {
		if (p->address >= COLUMNS) {
			/*
			 *  Row address triplets
			 */
			int s = p->data >> 5;
			int row = (p->address - COLUMNS) ? : (ROWS - 1);
			int column = 0;

			if (pdc_hr)
				return FALSE; /* invalid */

			switch (p->mode) {
			case 0x00:		/* full screen color */
				if (max_level >= VBI_WST_LEVEL_2p5
				    && s == 0 && type <= OBJ_TYPE_ACTIVE)
					screen_color(pg, vtp->flags, p->data & 0x1F);

				break;

			case 0x07:		/* address display row 0 */
				if (p->address != 0x3F)
					break; /* reserved, no position */

				row = 0;

				/* fall through */

			case 0x01:		/* full row color */
				row_color = next_row_color;

				if (s == 0) {
					row_color = p->data & 0x1F;
					next_row_color = ext->def_row_color;
				} else if (s == 3) {
					row_color =
					next_row_color = p->data & 0x1F;
				}

				goto set_active;

			case 0x02:		/* reserved */
			case 0x03:		/* reserved */
				break;

			case 0x04:		/* set active position */
				if (max_level >= VBI_WST_LEVEL_2p5) {
					if (p->data >= COLUMNS)
						break; /* reserved */

					column = p->data;
				}

				if (row > active_row)
					row_color = next_row_color;

			set_active:
				if (header_only && row > 0) {
					for (;max_triplets>1; p++, max_triplets--)
						if (p[1].address >= COLUMNS) {
							if (p[1].mode == 0x07)
								break;
							else if ((unsigned int) p[1].mode >= 0x1F)
								goto terminate;
						}
					break;
				}

				printv("enh set_active row %d col %d\n", row, column);

				if (row > active_row)
					flush_row();

				active_row = row;
				active_column = column;

				acp = &pg->text[(inv_row + active_row) * EXT_COLUMNS];

				break;

			case 0x05:		/* reserved */
			case 0x06:		/* reserved */
				break;

			case 0x08:		/* PDC data - Country of Origin and Programme Source */
				ptmp.x26_cni = p->address * 256 + p->data;
				break;

			case 0x09:		/* PDC data - Month and Day */
				ptmp.month = (p->address & 15) - 1;
				ptmp.day = (p->data >> 4) * 10 + (p->data & 15) - 1;
				break;

			case 0x0A:		/* PDC data - Cursor Row and Announced Starting Time Hours */
				if (!ptable) {
					break;
				} else if ((ptmp.month | ptmp.x26_cni) < 0) {
					return FALSE;
				} else if ((ptable - pt) > 22) {
					return FALSE;
				}

				*++pt = ptmp;

				/* fall through */

			case 0x0B:		/* PDC data - Cursor Row and Announced Finishing Time Hours */
				s = (p->data & 15) * 60;

				if (p->mode == 0x0A) {
					pt->at2 = ((p->data & 0x30) >> 4) * 600 + s;
					pt->length = 0;
					pt->row = row;
					pt->caf = !!(p->data & 0x40);
				} else {
					pt->length = ((p->data & 0x70) >> 4) * 600 + s;
				}

				pdc_hr = p->mode;

				break;

			case 0x0C:		/* PDC data - Cursor Row and Local Time Offset */
				ptmp.lto = (p->data & 0x40) ? ((~0x7F) | p->data) : p->data;
				break;

			case 0x0D:		/* PDC data - Series Identifier and Series Code */
				if (p->address == 0x30) {
					break;
				}
				pt->pty = 0x80 + p->data;
				break;

			case 0x0E:		/* reserved */
			case 0x0F:		/* reserved */
				break;

			case 0x10:		/* origin modifier */
				if (max_level < VBI_WST_LEVEL_2p5)
					break;

				if (p->data >= 72)
					break; /* invalid */

				offset_column = p->data;
				offset_row = p->address - COLUMNS;

				printv("enh origin modifier col %+d row %+d\n",
					offset_column, offset_row);

				break;

			case 0x11 ... 0x13:	/* object invocation */
			{
				int source = (p->address >> 3) & 3;
				object_type new_type = p->mode & 3;
				vt_triplet *trip;
				int remaining_max_triplets = 0;

				if (max_level < VBI_WST_LEVEL_2p5)
					break;

				printv("enh obj invocation source %d type %d\n", source, new_type);

				if (new_type <= type) { /* 13.2++ */
					printv("... priority violation\n");
					break;
				}

				if (source == 0) /* illegal */
					break;
				else if (source == 1) { /* local */
					int designation = (p->data >> 4) + ((p->address & 1) << 4);
					int triplet = p->data & 15;

					if (type != LOCAL_ENHANCEMENT_DATA || triplet > 12)
						break; /* invalid */

					printv("... local obj %d/%d\n", designation, triplet);

					if (!(vtp->enh_lines & 1)) {
						printv("... no packet %d\n", designation);
						return FALSE;
					}

					trip = vtp->data.enh_lop.enh + designation * 13 + triplet;
					remaining_max_triplets = elements(vtp->data.enh_lop.enh) - (designation* 13 + triplet);
				}
				else /* global / public */
				{
					page_function function;
					int pgno, i = 0;

					if (source == 3) {
						function = PAGE_FUNCTION_GPOP;
						pgno = vtp->data.lop.link[24].pgno;

						if (NO_PAGE(pgno)) {
							if (max_level < VBI_WST_LEVEL_3p5
							    || NO_PAGE(pgno = mag->pop_link[8].pgno))
								pgno = mag->pop_link[0].pgno;
						} else
							printv("... X/27/4 GPOP overrides MOT\n");
					} else {
						function = PAGE_FUNCTION_POP;
						pgno = vtp->data.lop.link[25].pgno;

						if (NO_PAGE(pgno)) {
							if ((i = mag->pop_lut[vtp->pgno & 0xFF]) == 0) {
								printv("... MOT pop_lut empty\n");
								return FALSE; /* has no link (yet) */
							}

							if (max_level < VBI_WST_LEVEL_3p5
							    || NO_PAGE(pgno = mag->pop_link[i + 8].pgno))
								pgno = mag->pop_link[i + 0].pgno;
						} else
							printv("... X/27/4 POP overrides MOT\n");
					}

					if (NO_PAGE(pgno)) {
						printv("... dead MOT link %d\n", i);
						return FALSE; /* has no link (yet) */
					}

					printv("... %s obj\n", (source == 3) ? "global" : "public");

					trip = resolve_obj_address(vbi, new_type, pgno,
						(p->address << 7) + p->data, function,
						&remaining_max_triplets);

					if (!trip)
						return FALSE;
				}

				row = inv_row + active_row;
				column = inv_column + active_column;

				if (!enhance(vbi, mag, ext, pg, vtp, new_type, trip,
					     remaining_max_triplets,
					     row + offset_row, column + offset_column,
					     max_level, header_only, NULL))
					return FALSE;

				printv("... object done\n");

				offset_row = 0;
				offset_column = 0;

				break;
			}

			case 0x14:		/* reserved */
				break;

			case 0x15 ... 0x17:	/* object definition */
				flush_row();
				printv("enh obj definition 0x%02x 0x%02x\n", p->mode, p->data);
				printv("enh terminated\n");
				goto swedish;

			case 0x18:		/* drcs mode */
				printv("enh DRCS mode 0x%02x\n", p->data);
				drcs_s1[p->data >> 6] = p->data & 15;
				break;

			case 0x19 ... 0x1E:	/* reserved */
				break;

			case 0x1F:		/* termination marker */
			default:
	                terminate:
				flush_row();
				printv("enh terminated %02x\n", p->mode);
				goto swedish;
			}
		} else {
			/*
			 *  Column address triplets
			 */
			int s = p->data >> 5;
			int column = p->address;
			int unicode;

			switch (p->mode) {
			case 0x00:		/* foreground color */
				if (max_level >= VBI_WST_LEVEL_2p5 && s == 0) {
					if (column > active_column)
						flush(column);

					ac.foreground = p->data & 0x1F;
					mac.foreground = ~0;

					printv("enh col %d foreground %d\n", active_column, ac.foreground);
				}

				break;

			case 0x01:		/* G1 block mosaic character */
				if (max_level >= VBI_WST_LEVEL_2p5) {
					if (column > active_column)
						flush(column);

					if (p->data & 0x20) {
						unicode = 0xEE00 + p->data; /* G1 contiguous */
						goto store;
					} else if (p->data >= 0x40) {
						unicode = vbi_teletext_unicode(
							font->G0, NO_SUBSET, p->data);
						goto store;
					}
				}

				break;

			case 0x0B:		/* G3 smooth mosaic or line drawing character */
				if (max_level < VBI_WST_LEVEL_2p5)
					break;

				/* fall through */

			case 0x02:		/* G3 smooth mosaic or line drawing character */
				if (p->data >= 0x20) {
					if (column > active_column)
						flush(column);

					unicode = 0xEF00 + p->data;
					goto store;
				}

				break;

			case 0x03:		/* background color */
				if (max_level >= VBI_WST_LEVEL_2p5 && s == 0) {
					if (column > active_column)
						flush(column);

					ac.background = p->data & 0x1F;
					mac.background = ~0;

					printv("enh col %d background %d\n", active_column, ac.background);
				}

				break;

			case 0x04:		/* reserved */
			case 0x05:		/* reserved */
				break;

			case 0x06:		/* PDC data - Cursor Column and Announced Starting */
						/* and Finishing Time Minutes */
				if (!ptable)
					break;

				s = (p->data >> 4) * 10 + (p->data & 15);

				if (pdc_hr == 0x0A) {
					pt->at2 += s;

					if (pt > ptable && pt[-1].length == 0) {
						pt[-1].length = pt->at2 - pt[-1].at2;

						if (pt->at2 < pt[-1].at2)
							pt[-1].length += 24 * 60;

						if (pt[-1].length >= 12 * 60) {
							/* bullshit */
							pt[-1] = pt[0];
							pt--;
						}
					}
				} else if (pdc_hr == 0x0B) {
					pt->length += s;

					if (pt->length >= 4 * 600) {
						pt->length -= 4 * 600;
					} else {
						if (pt->length < pt->at2)
							pt->length += 24 * 60;

						pt->length -= pt->at2;
					}
				} else {
					return FALSE;
				}

				pt->column = column;
				pdc_hr = 0;

				break;

			case 0x07:		/* additional flash functions */
				if (max_level >= VBI_WST_LEVEL_2p5) {
					if (column > active_column)
						flush(column);

					/*
					 *  Only one flash function (if any) implemented:
					 *  Mode 1 - Normal flash to background color
					 *  Rate 0 - Slow rate (1 Hz)
					 */
					ac.flash = !!(p->data & 3);
					mac.flash = ~0;

					printv("enh col %d flash 0x%02x\n", active_column, p->data);
				}

				break;

			case 0x08:		/* modified G0 and G2 character set designation */
				if (max_level >= VBI_WST_LEVEL_2p5) {
					if (column > active_column)
						flush(column);

					if (VALID_CHARACTER_SET(p->data))
						font = vbi_font_descriptors + p->data;
					else
						font = pg->font[0];

					printv("enh col %d modify character set %d\n", active_column, p->data);
				}

				break;

			case 0x09:		/* G0 character */
				if (max_level >= VBI_WST_LEVEL_2p5 && p->data >= 0x20) {
					if (column > active_column)
						flush(column);

					unicode = vbi_teletext_unicode(font->G0, NO_SUBSET, p->data);
					goto store;
				}

				break;

			case 0x0A:		/* reserved */
				break;

			case 0x0C:		/* display attributes */
				if (max_level < VBI_WST_LEVEL_2p5)
					break;

				if (column > active_column)
					flush(column);

				ac.size = ((p->data & 0x40) ? VBI_DOUBLE_WIDTH : 0)
					+ ((p->data & 1) ? VBI_DOUBLE_HEIGHT : 0);
				mac.size = ~0;

				if (p->data & 2) {
					if (vtp->flags & (C5_NEWSFLASH | C6_SUBTITLE))
						ac.opacity = VBI_SEMI_TRANSPARENT;
					else
						ac.opacity = VBI_TRANSPARENT_SPACE;
				} else
					ac.opacity = pg->page_opacity[1];
				mac.opacity = ~0;

				ac.conceal = !!(p->data & 4);
				mac.conceal = ~0;

				/* (p->data & 8) reserved */

				invert = p->data & 0x10;

				ac.underline = !!(p->data & 0x20);
				mac.underline = ~0;

				printv("enh col %d display attr 0x%02x\n", active_column, p->data);

				break;

			case 0x0D:		/* drcs character invocation */
			{
				int normal = p->data >> 6;
				int offset = p->data & 0x3F;
				vt_page *dvtp;
				page_function function;
				int pgno, page, i = 0;

				if (max_level < VBI_WST_LEVEL_2p5)
					break;

				if (offset >= 48)
					break; /* invalid */

				if (column > active_column)
					flush(column);

				page = normal * 16 + drcs_s1[normal];

				printv("enh col %d DRCS %d/0x%02x\n", active_column, page, p->data);

				/* if (!pg->drcs[page]) */ {
					if (!normal) {
						function = PAGE_FUNCTION_GDRCS;
						pgno = vtp->data.lop.link[26].pgno;

						if (NO_PAGE(pgno)) {
							if (max_level < VBI_WST_LEVEL_3p5
							    || NO_PAGE(pgno = mag->drcs_link[8]))
								pgno = mag->drcs_link[0];
						} else
							printv("... X/27/4 GDRCS overrides MOT\n");
					} else {
						function = PAGE_FUNCTION_DRCS;
						pgno = vtp->data.lop.link[25].pgno;

						if (NO_PAGE(pgno)) {
							if ((i = mag->drcs_lut[vtp->pgno & 0xFF]) == 0) {
								printv("... MOT drcs_lut empty\n");
								return FALSE; /* has no link (yet) */
							}

							if (max_level < VBI_WST_LEVEL_3p5
							    || NO_PAGE(pgno = mag->drcs_link[i + 8]))
								pgno = mag->drcs_link[i + 0];
						} else
							printv("... X/27/4 DRCS overrides MOT\n");
					}

					if (NO_PAGE(pgno)) {
						printv("... dead MOT link %d\n", i);
						return FALSE; /* has no link (yet) */
					}

					printv("... %s drcs from page %03x/%04x\n",
						normal ? "normal" : "global", pgno, drcs_s1[normal]);

					dvtp = vbi_cache_get(vbi,
						pgno, drcs_s1[normal], 0x000F);

					if (!dvtp) {
						printv("... page not cached\n");
						return FALSE;
					}

					if (dvtp->function == PAGE_FUNCTION_UNKNOWN) {
						if (!(dvtp = vbi_convert_page(vbi, dvtp, TRUE, function))) {
							printv("... no g/drcs page or hamming error\n");
							return FALSE;
						}
					} else if (dvtp->function == PAGE_FUNCTION_DRCS) {
						dvtp->function = function;
					} else if (dvtp->function != function) {
						printv("... source page wrong function %d, expected %d\n",
							dvtp->function, function);
						return FALSE;
					}

					if (dvtp->data.drcs.invalid & (1ULL << offset)) {
						printv("... invalid drcs, prob. tx error\n");
						return FALSE;
					}

					pg->drcs[page] = dvtp->data.drcs.bits[0];
				}

				unicode = 0xF000 + (page << 6) + offset;
				goto store;
			}

			case 0x0E:		/* font style */
			{
				int italic, bold, proportional;
				int col, row, count;
				vbi_char *acp;

				if (max_level < VBI_WST_LEVEL_3p5)
					break;

				row = inv_row + active_row;
				count = (p->data >> 4) + 1;
				acp = &pg->text[row * EXT_COLUMNS];

				proportional = (p->data >> 0) & 1;
				bold = (p->data >> 1) & 1;
				italic = (p->data >> 2) & 1;

				while (row < ROWS && count > 0) {
					for (col = inv_column + column; col < COLUMNS; col++) {
						acp[col].italic = italic;
		    				acp[col].bold = bold;
						acp[col].proportional = proportional;
					}

					acp += EXT_COLUMNS;
					row++;
					count--;
				}

				printv("enh col %d font style 0x%02x\n", active_column, p->data);

				break;
			}

			case 0x0F:		/* G2 character */
				if (p->data >= 0x20) {
					if (column > active_column)
						flush(column);

					unicode = vbi_teletext_unicode(font->G2, NO_SUBSET, p->data);
					goto store;
				}

				break;

			case 0x10 ... 0x1F:	/* characters including diacritical marks */
				if (p->data >= 0x20) {
					if (column > active_column)
						flush(column);

					unicode = vbi_teletext_composed_unicode(
						p->mode - 0x10, p->data);
			store:
					printv("enh row %d col %d print 0x%02x/0x%02x -> 0x%04x\n",
						active_row, active_column, p->mode, p->data,
						unicode);

					ac.unicode = unicode;
					mac.unicode = ~0;
				}

				break;
			}
		}
	}

swedish:

	if (ptable) {
		if (pt >= ptable && (pdc_hr || pt->length == 0))
			pt--; /* incomplete start or end tag */

		if (1)
			dump_pex26(ptable, pt - ptable + 1);
	}

	if (0) {
		acp = pg->text;

		for (active_row = 0; active_row < ROWS; active_row++) {
			printv("%2d: ", active_row);

			for (active_column = 0; active_column < COLUMNS; acp++, active_column++) {
				printv("%04x ", acp->unicode);
			}

			printv("\n");

			acp += EXT_COLUMNS - COLUMNS;
		}
	}

	return TRUE;
}

static void
post_enhance(vbi_page *pg, int display_rows)
{
	int last_row = MIN(display_rows, ROWS) - 2;
	vbi_char ac, *acp;
	int column, row;

	acp = pg->text;

	for (row = 0; row <= last_row; row++) {
		for (column = 0; column < COLUMNS; acp++, column++) {
			if (1)
				printv("%c", vbi_printable (acp->unicode));
			else
				printv("%04xF%dB%dS%dO%d ", acp->unicode,
				       acp->foreground, acp->background,
				       acp->size, acp->opacity);

			if (acp->opacity == VBI_TRANSPARENT_SPACE
			    || (acp->foreground == VBI_TRANSPARENT_BLACK
				&& acp->background == VBI_TRANSPARENT_BLACK)) {
				acp->opacity = VBI_TRANSPARENT_SPACE;
				acp->unicode = 0x0020;
			} else if (acp->background == VBI_TRANSPARENT_BLACK) {
				acp->opacity = VBI_SEMI_TRANSPARENT;
			}
			/* transparent foreground not implemented */

			switch (acp->size) {
			case VBI_NORMAL_SIZE:
				if (row < last_row
				    && (acp[EXT_COLUMNS].size == VBI_DOUBLE_HEIGHT2
					|| acp[EXT_COLUMNS].size == VBI_DOUBLE_SIZE2)) {
					acp[EXT_COLUMNS].unicode = 0x0020;
					acp[EXT_COLUMNS].size = VBI_NORMAL_SIZE;
				}

				if (column < 39
				    && (acp[1].size == VBI_OVER_TOP
					|| acp[1].size == VBI_OVER_BOTTOM)) {
					acp[1].unicode = 0x0020;
					acp[1].size = VBI_NORMAL_SIZE;
				}

				break;

			case VBI_DOUBLE_HEIGHT:
				if (row < last_row) {
					ac = acp[0];
					ac.size = VBI_DOUBLE_HEIGHT2;
					acp[EXT_COLUMNS] = ac;
				}
				break;

			case VBI_DOUBLE_SIZE:
				if (row < last_row) {
					ac = acp[0];
					ac.size = VBI_DOUBLE_SIZE2;
					acp[EXT_COLUMNS] = ac;
					ac.size = VBI_OVER_BOTTOM;
					acp[EXT_COLUMNS + 1] = ac;
				}

				/* fall through */

			case VBI_DOUBLE_WIDTH:
				if (column < 39) {
					ac = acp[0];
					ac.size = VBI_OVER_TOP;
					acp[1] = ac;
				}
				break;

			default:
				break;
			}
		}

		printv("\n");

		acp += EXT_COLUMNS - COLUMNS;
	}
}

static inline vbi_bool
default_object_invocation(vbi_decoder *vbi, vt_magazine *mag,
	vt_extension *ext, vbi_page *pg, vt_page *vtp,
	vbi_wst_level max_level, vbi_bool header_only)
{
	vt_pop_link *pop;
	int i, order;

	if (!(i = mag->pop_lut[vtp->pgno & 0xFF]))
		return FALSE; /* has no link (yet) */

	pop = mag->pop_link + i + 8;

	if (max_level < VBI_WST_LEVEL_3p5 || NO_PAGE(pop->pgno)) {
		pop = mag->pop_link + i;

		if (NO_PAGE(pop->pgno)) {
			printv("default object has dead MOT pop link %d\n", i);
			return FALSE;
		}
	}

	order = pop->default_obj[0].type > pop->default_obj[1].type;

	for (i = 0; i < 2; i++) {
		object_type type = pop->default_obj[i ^ order].type;
		vt_triplet *trip;
		int remaining_max_triplets;

		if (type == OBJ_TYPE_NONE)
			continue;

		printv("default object #%d invocation, type %d\n", i ^ order, type);

		trip = resolve_obj_address(vbi, type, pop->pgno,
			pop->default_obj[i ^ order].address, PAGE_FUNCTION_POP,
			&remaining_max_triplets);

		if (!trip)
			return FALSE;

		if (!enhance(vbi, mag, ext, pg, vtp, type, trip,
			     remaining_max_triplets, 0, 0, max_level,
			     header_only, NULL))
			return FALSE;
	}

	return TRUE;
}

/**
 * @internal
 * @param vbi Initialized vbi_decoder context.
 * @param pg Place to store the formatted page.
 * @param vtp Raw Teletext page. 
 * @param max_level Format the page at this Teletext implementation level.
 * @param display_rows Number of rows to format, between 1 ... 25.
 * @param navigation Analyse the page and add navigation links,
 *   including TOP and FLOF.
 * 
 * Format a page @a pg from a raw Teletext page @a vtp. This function is
 * used internally by libzvbi only.
 * 
 * @return
 * @c TRUE if the page could be formatted.
 */
int
vbi_format_vt_page(vbi_decoder *vbi,
		   vbi_page *pg, vt_page *vtp,
		   vbi_wst_level max_level,
		   int display_rows, vbi_bool navigation)
{
	char buf[16];
	vt_magazine *mag;
	vt_extension *ext;
	int column, row, i;

	if (vtp->function != PAGE_FUNCTION_LOP &&
	    vtp->function != PAGE_FUNCTION_TRIGGER)
		return FALSE;

	printv("\nFormatting page %03x/%04x pg=%p lev=%d rows=%d nav=%d\n",
	       vtp->pgno, vtp->subno, pg, max_level, display_rows, navigation);

	display_rows = SATURATE(display_rows, 1, ROWS);

	pg->vbi = vbi;

	pg->nuid = vbi->network.ev.network.nuid;

	pg->pgno = vtp->pgno;
	pg->subno = vtp->subno;

	pg->rows = display_rows;
	pg->columns = EXT_COLUMNS;

	pg->dirty.y0 = 0;
	pg->dirty.y1 = ROWS - 1;
	pg->dirty.roll = 0;

	mag = (max_level <= VBI_WST_LEVEL_1p5) ?
		vbi->vt.magazine : vbi->vt.magazine + (vtp->pgno >> 8);

	if (vtp->data.lop.ext)
		ext = &vtp->data.ext_lop.ext;
	else
		ext = &mag->extension;

	/* Character set designation */

	character_set_designation(pg->font, ext, vtp);

	/* Colors */

	screen_color(pg, vtp->flags, ext->def_screen_color);

	vbi_transp_colormap(vbi, pg->color_map, ext->color_map, 40);

	pg->drcs_clut = ext->drcs_clut;

	/* Opacity */

	pg->page_opacity[1] =
		(vtp->flags & (C5_NEWSFLASH | C6_SUBTITLE | C10_INHIBIT_DISPLAY)) ?
			VBI_TRANSPARENT_SPACE : VBI_OPAQUE;
	pg->boxed_opacity[1] =
		(vtp->flags & C10_INHIBIT_DISPLAY) ?
			VBI_TRANSPARENT_SPACE : VBI_SEMI_TRANSPARENT;

	if (vtp->flags & C7_SUPPRESS_HEADER) {
		pg->page_opacity[0] = VBI_TRANSPARENT_SPACE;
		pg->boxed_opacity[0] = VBI_TRANSPARENT_SPACE;
	} else {
		pg->page_opacity[0] = pg->page_opacity[1];
		pg->boxed_opacity[0] = pg->boxed_opacity[1];
	}

	/* DRCS */

	memset(pg->drcs, 0, sizeof(pg->drcs));

	/* Current page number in header */

	sprintf(buf, "\2%x.%02x\7", vtp->pgno, vtp->subno & 0xff);

	/* Level 1 formatting */

	i = 0;
	pg->double_height_lower = 0;

	for (row = 0; row < display_rows; row++) {
		struct vbi_font_descr *font;
		int mosaic_unicodes; /* 0xEE00 separate, 0xEE20 contiguous */
		int held_mosaic_unicode;
		int esc;
		vbi_bool hold, mosaic;
		vbi_bool double_height, wide_char;
		vbi_char ac, *acp = &pg->text[row * EXT_COLUMNS];

		held_mosaic_unicode = 0xEE20; /* G1 block mosaic, blank, contiguous */

		memset(&ac, 0, sizeof(ac));

		ac.unicode      = 0x0020;
		ac.foreground	= ext->foreground_clut + VBI_WHITE;
		ac.background	= ext->background_clut + VBI_BLACK;
		mosaic_unicodes	= 0xEE20; /* contiguous */
		ac.opacity	= pg->page_opacity[row > 0];
		font		= pg->font[0];
		esc		= 0;
		hold		= FALSE;
		mosaic		= FALSE;

		double_height	= FALSE;
		wide_char	= FALSE;

		acp[COLUMNS] = ac; /* artificial column 41 */

		for (column = 0; column < COLUMNS; ++column) {
			int raw;

			if (row == 0 && column < 8) {
				raw = buf[column];
				i++;
			} else if ((raw = vbi_unpar8 (vtp->data.lop.raw[0][i++])) < 0)
				raw = ' ';

			/* set-at spacing attributes */

			switch (raw) {
			case 0x09:		/* steady */
				ac.flash = FALSE;
				break;

			case 0x0C:		/* normal size */
				ac.size = VBI_NORMAL_SIZE;
				break;

			case 0x18:		/* conceal */
				ac.conceal = TRUE;
				break;

			case 0x19:		/* contiguous mosaics */
				mosaic_unicodes = 0xEE20;
				break;

			case 0x1A:		/* separated mosaics */
				mosaic_unicodes = 0xEE00;
				break;

			case 0x1C:		/* black background */
				ac.background = ext->background_clut + VBI_BLACK;
				break;

			case 0x1D:		/* new background */
				ac.background = ext->background_clut + (ac.foreground & 7);
				break;

			case 0x1E:		/* hold mosaic */
				hold = TRUE;
				break;
			}

			if (raw <= 0x1F)
				ac.unicode = (hold & mosaic) ? held_mosaic_unicode : 0x0020;
			else
				if (mosaic && (raw & 0x20)) {
					held_mosaic_unicode = mosaic_unicodes + raw - 0x20;
					ac.unicode = held_mosaic_unicode;
				} else
					ac.unicode = vbi_teletext_unicode(font->G0,
									  font->subset, raw);

			if (!wide_char) {
				acp[column] = ac;

				wide_char = /*!!*/(ac.size & VBI_DOUBLE_WIDTH);

				if (wide_char && column < (COLUMNS - 1)) {
					acp[column + 1] = ac;
					acp[column + 1].size = VBI_OVER_TOP;
				}
			} else
				wide_char = FALSE;

			/* set-after spacing attributes */

			switch (raw) {
			case 0x00 ... 0x07:	/* alpha + foreground color */
				ac.foreground = ext->foreground_clut + (raw & 7);
				ac.conceal = FALSE;
				mosaic = FALSE;
				break;

			case 0x08:		/* flash */
				ac.flash = TRUE;
				break;

			case 0x0A:		/* end box */
				if (column < (COLUMNS - 1)
				    && vbi_unpar8 (vtp->data.lop.raw[0][i]) == 0x0a)
					ac.opacity = pg->page_opacity[row > 0];
				break;

			case 0x0B:		/* start box */
				if (column < (COLUMNS - 1)
				    && vbi_unpar8 (vtp->data.lop.raw[0][i]) == 0x0b)
					ac.opacity = pg->boxed_opacity[row > 0];
				break;

			case 0x0D:		/* double height */
				if (row <= 0 || row >= 23)
					break;
				ac.size = VBI_DOUBLE_HEIGHT;
				double_height = TRUE;
				break;

			case 0x0E:		/* double width */
				printv("spacing col %d row %d double width\n", column, row);
				if (column < (COLUMNS - 1))
					ac.size = VBI_DOUBLE_WIDTH;
				break;

			case 0x0F:		/* double size */
				printv("spacing col %d row %d double size\n", column, row);
				if (column >= (COLUMNS - 1) || row <= 0 || row >= 23)
					break;
				ac.size = VBI_DOUBLE_SIZE;
				double_height = TRUE;

				break;

			case 0x10 ... 0x17:	/* mosaic + foreground color */
				ac.foreground = ext->foreground_clut + (raw & 7);
				ac.conceal = FALSE;
				mosaic = TRUE;
				break;

			case 0x1F:		/* release mosaic */
				hold = FALSE;
				break;

			case 0x1B:		/* ESC */
				font = pg->font[esc ^= 1];
				break;
			}
		}

		if (double_height) {
			for (column = 0; column < EXT_COLUMNS; column++) {
				ac = acp[column];

				switch (ac.size) {
				case VBI_DOUBLE_HEIGHT:
					ac.size = VBI_DOUBLE_HEIGHT2;
					acp[EXT_COLUMNS + column] = ac;
					break;
		
				case VBI_DOUBLE_SIZE:
					ac.size = VBI_DOUBLE_SIZE2;
					acp[EXT_COLUMNS + column] = ac;
					ac.size = VBI_OVER_BOTTOM;
					acp[EXT_COLUMNS + (++column)] = ac;
					break;

				default: /* NORMAL, DOUBLE_WIDTH, OVER_TOP */
					ac.size = VBI_NORMAL_SIZE;
					ac.unicode = 0x0020;
					acp[EXT_COLUMNS + column] = ac;
					break;
				}
			}

			i += COLUMNS;
			row++;

			pg->double_height_lower |= 1 << row;
		}
	}

	if (0) {
		if (row < ROWS) {
			vbi_char ac;

			memset(&ac, 0, sizeof(ac));

			ac.foreground	= ext->foreground_clut + VBI_WHITE;
			ac.background	= ext->background_clut + VBI_BLACK;
			ac.opacity	= pg->page_opacity[1];
			ac.unicode	= 0x0020;

			for (i = row * EXT_COLUMNS; i < ROWS * EXT_COLUMNS; i++)
				pg->text[i] = ac;
		}
	}

	/* Local enhancement data and objects */

	if (max_level >= VBI_WST_LEVEL_1p5 && display_rows > 0) {
		vbi_page page;
		vbi_bool success;

		memcpy(&page, pg, sizeof(page));

		if (!(vtp->flags & (C5_NEWSFLASH | C6_SUBTITLE))) {
			pg->boxed_opacity[0] = VBI_TRANSPARENT_SPACE;
			pg->boxed_opacity[1] = VBI_TRANSPARENT_SPACE;
		}

		if (vtp->enh_lines & 1) {
			printv("enhancement packets %08x\n", vtp->enh_lines);
			success = enhance(vbi, mag, ext, pg, vtp, LOCAL_ENHANCEMENT_DATA,
				vtp->data.enh_lop.enh, elements(vtp->data.enh_lop.enh),
				0, 0, max_level, display_rows == 1, NULL);
							   // here --^
		} else
			success = default_object_invocation(vbi, mag, ext, pg, vtp,
							    max_level, display_rows == 1);

		if (success) {
			if (max_level >= VBI_WST_LEVEL_2p5)
				post_enhance(pg, display_rows);
		} else
			memcpy(pg, &page, sizeof(*pg));
	}

	/* Navigation */

	if (navigation) {
		pg->nav_link[5].pgno = vbi->vt.initial_page.pgno;
		pg->nav_link[5].subno = vbi->vt.initial_page.subno;

		for (row = 1; row < MIN(ROWS - 1, display_rows); row++)
			zap_links(pg, row);

		if (display_rows >= ROWS) {
			if (vtp->data.lop.flof) {
				if (vtp->data.lop.link[5].pgno >= 0x100
				    && vtp->data.lop.link[5].pgno <= 0x899
				    && (vtp->data.lop.link[5].pgno & 0xFF) != 0xFF) {
					pg->nav_link[5].pgno = vtp->data.lop.link[5].pgno;
					pg->nav_link[5].subno = vtp->data.lop.link[5].subno;
				}

				if (vtp->lop_lines & (1 << 24))
					flof_links(pg, vtp);
				else
					flof_navigation_bar(pg, vtp);
			} else if (vbi->vt.top)
				top_navigation_bar(vbi, pg, vtp);

//			pdc_method_a(pg, vtp, NULL);
		}
	}

	if (0) {
		vbi_char *acp;

		for (row = ROWS - 1, acp = pg->text + EXT_COLUMNS * row; row < ROWS; row++) {
			fprintf(stderr, "%2d: ", row);

			for (column = 0; column < COLUMNS; acp++, column++) {
				fprintf(stderr, "%04x ", acp->unicode);
			}

			fprintf(stderr, "\n");

			acp += EXT_COLUMNS - COLUMNS;
		}
	}

	return TRUE;
}

/**
 * @param vbi Initialized vbi_decoder context.
 * @param pg Place to store the formatted page.
 * @param pgno Page number of the page to fetch, see vbi_pgno.
 * @param subno Subpage number to fetch (optional @c VBI_ANY_SUBNO).
 * @param max_level Format the page at this Teletext implementation level.
 * @param display_rows Number of rows to format, between 1 ... 25.
 * @param navigation Analyse the page and add navigation links,
 *   including TOP and FLOF.
 * 
 * Fetches a Teletext page designated by @a pgno and @a subno from the
 * cache, formats and stores it in @a pg. Formatting is limited to row
 * 0 ... @a display_rows - 1 inclusive. The really useful values
 * are 1 (format header only) or 25 (everything). Likewise
 * @a navigation can be used to save unnecessary formatting time.
 * 
 * Although safe to do, this function is not supposed to be called from
 * an event handler since rendering may block decoding for extended
 * periods of time.
 *
 * @return
 * @c FALSE if the page is not cached or could not be formatted
 * for other reasons, for instance is a data page not intended for
 * display. Level 2.5/3.5 pages which could not be formatted e. g.
 * due to referencing data pages not in cache are formatted at a
 * lower level.
 */
vbi_bool
vbi_fetch_vt_page(vbi_decoder *vbi, vbi_page *pg,
		  vbi_pgno pgno, vbi_subno subno,
		  vbi_wst_level max_level,
		  int display_rows, vbi_bool navigation)
{
	vt_page *vtp;
	int row;

	switch (pgno) {
	case 0x900:
		if (subno == VBI_ANY_SUBNO)
			subno = 0;

		if (!vbi->vt.top || !top_index(vbi, pg, subno))
			return FALSE;

		pg->nuid = vbi->network.ev.network.nuid;
		pg->pgno = 0x900;
		pg->subno = subno;

		post_enhance(pg, ROWS);

		for (row = 1; row < ROWS; row++)
			zap_links(pg, row);

		return TRUE;

	default:
		vtp = vbi_cache_get(vbi, pgno, subno, -1);

		if (!vtp)
			return FALSE;

		return vbi_format_vt_page(vbi, pg, vtp,
					  max_level, display_rows, navigation);
	}
}
