/*
 *  menubox.c -- implements the menu box
 *
 *  ORIGINAL AUTHOR: Savio Lam (lam836@cs.cuhk.hk)
 *  MODIFIED FOR LINUX KERNEL CONFIG BY: William Roadcap (roadcapw@cfw.com)
 *
 *  This program is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU General Public License
 *  as published by the Free Software Foundation; either version 2
 *  of the License, or (at your option) any later version.
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

/*
 *  Changes by Clifford Wolf (god@clifford.at)
 *
 *  [ 1998-06-13 ]
 *
 *    *)  A bugfix for the Page-Down problem
 *
 *    *)  Formerly when I used Page Down and Page Up, the cursor would be set
 *        to the first position in the menu box.  Now lxdialog is a bit
 *        smarter and works more like other menu systems (just have a look at
 *        it).
 *
 *    *)  Formerly if I selected something my scrolling would be broken because
 *        lxdialog is re-invoked by the Menuconfig shell script, can't
 *        remember the last scrolling position, and just sets it so that the
 *        cursor is at the bottom of the box.  Now it writes the temporary file
 *        lxdialog.scrltmp which contains this information. The file is
 *        deleted by lxdialog if the user leaves a submenu or enters a new
 *        one, but it would be nice if Menuconfig could make another "rm -f"
 *        just to be sure.  Just try it out - you will recognise a difference!
 *
 *  [ 1998-06-14 ]
 *
 *    *)  Now lxdialog is crash-safe against broken "lxdialog.scrltmp" files
 *        and menus change their size on the fly.
 *
 *    *)  If for some reason the last scrolling position is not saved by
 *        lxdialog, it sets the scrolling so that the selected item is in the
 *        middle of the menu box, not at the bottom.
 *
 * 02 January 1999, Michael Elizabeth Chastain (mec@shout.net)
 * Reset 'scroll' to 0 if the value from lxdialog.scrltmp is bogus.
 * This fixes a bug in Menuconfig where using ' ' to descend into menus
 * would leave mis-synchronized lxdialog.scrltmp files lying around,
 * fscanf would read in 'scroll', and eventually that value would get used.
 */

#include <string.h>
#include "dialog.h"

#define ISEARCH_LEN 32
static char isearch_str[ISEARCH_LEN] = "";

static int menu_width, item_x;

static int focus_on_buttons;

static const char isearch_instructions[] =
	"I-search: Arrow keys navigate the menu.  "
	"<Enter> selects submenus and/or clears i-search string.  "
	"Type any character to search for menu items, "
	"press <\\> to find further matches, <Esc><Esc> to exit. "
	"Legend: [*] built-in  [ ] excluded  <M> module  < > module capable";
/*
 * Print menu item
 */
static void do_print_item(WINDOW * win, const char *item, int line_y,
			  int selected)
{
	int i;
	int isearch_match_pos;
	char *isearch_match;
	char *menu_item = malloc(menu_width + 1);

	strncpy(menu_item, item, menu_width - item_x);
	menu_item[menu_width - item_x] = '\0';

	isearch_match = strcasestr(menu_item, isearch_str);
	isearch_match_pos = isearch_match - menu_item;

	/* Clear 'residue' of last item */
	wattrset(win, dlg.menubox.atr);
	wmove(win, line_y, 0);
#if OLD_NCURSES
	{
		int i;
		for (i = 0; i < menu_width; i++)
			waddch(win, ' ');
	}
#else
	wclrtoeol(win);
#endif
	if (focus_on_buttons)
		wattrset(win, selected ? A_UNDERLINE : dlg.item.atr);
	else
		wattrset(win, selected ? dlg.item_selected.atr : dlg.item.atr);
	mvwaddstr(win, line_y, item_x, menu_item);

	if (selected) {
		/*
		 * Highlight i-search matching part of selected menu item
		 */
		if (isearch_match) {
			for (i = 0; i < strlen(isearch_str); i++) {
				wattrset(win, dlg.tag_key_selected.atr);
				mvwaddch(win, line_y, item_x + isearch_match_pos + i,
					 menu_item[isearch_match_pos + i]);
			}
		}

		wmove(win, line_y, item_x + 1);
	}
	free(menu_item);
	wrefresh(win);
}

#define print_item(index, choice, selected)				\
do {									\
	item_set(index);						\
	do_print_item(menu, item_str(), choice, selected); \
} while (0)


/*
* Print the isearch indicator.
*/
static void print_isearch(WINDOW * win, int y, int x, int height, bool isearch)
{
	unsigned char i = 0;
	int text_size = ISEARCH_LEN - 1;
	wmove(win, y, x);

	y = y + height + 1;
	wmove(win, y, x);

	if (isearch) {
		wattrset(win, dlg.button_key_inactive.atr);
		waddstr(win, "isearch: ");
		waddstr(win, isearch_str);
		i = strlen(isearch_str);
	} else {
		text_size += 9; /* also overwrite "isearch: " */
	}

	wattrset(win, dlg.menubox_border.atr);

	for ( ; i < text_size; i++ )
		waddch(win, ACS_HLINE);
}

/*
 * Print the scroll indicators.
 */
static void print_arrows(WINDOW * win, int item_no, int scroll, int y, int x,
			 int height)
{
	int cur_y, cur_x;

	getyx(win, cur_y, cur_x);

	wmove(win, y, x);

	if (scroll > 0) {
		wattrset(win, dlg.uarrow.atr);
		waddch(win, ACS_UARROW);
		waddstr(win, "(-)");
	} else {
		wattrset(win, dlg.menubox.atr);
		waddch(win, ACS_HLINE);
		waddch(win, ACS_HLINE);
		waddch(win, ACS_HLINE);
		waddch(win, ACS_HLINE);
	}

	y = y + height + 1;
	wmove(win, y, x);
	wrefresh(win);

	if ((height < item_no) && (scroll + height < item_no)) {
		wattrset(win, dlg.darrow.atr);
		waddch(win, ACS_DARROW);
		waddstr(win, "(+)");
	} else {
		wattrset(win, dlg.menubox_border.atr);
		waddch(win, ACS_HLINE);
		waddch(win, ACS_HLINE);
		waddch(win, ACS_HLINE);
		waddch(win, ACS_HLINE);
	}

	wmove(win, cur_y, cur_x);
	wrefresh(win);
}

/*
 * Display the termination buttons.
 */
static void print_buttons(WINDOW * win, int height, int width, int selected)
{
	int x = width / 2 - 28;
	int y = height - 2;
	int highlight;

	/*
	 * Don't highlight the selected button if the buttons don't have
	 * the focus.
	 */
	if (!focus_on_buttons)
		highlight = -1;
	else
		highlight = selected;

	print_button(win, "Select", y, x, highlight == 0);
	print_button(win, " Exit ", y, x + 12, highlight == 1);
	print_button(win, " Help ", y, x + 24, highlight == 2);
	print_button(win, " Save ", y, x + 36, highlight == 3);
	print_button(win, " Load ", y, x + 48, highlight == 4);

	wmove(win, y, x + 1 + 12 * selected);
	wrefresh(win);
}

/* scroll up n lines (n may be negative) */
static void do_scroll(WINDOW *win, int *scroll, int n)
{
	/* Scroll menu up */
	scrollok(win, TRUE);
	wscrl(win, n);
	scrollok(win, FALSE);
	*scroll = *scroll + n;
	wrefresh(win);
}

/*
 * Incremental search for text in dialog menu entries.
 * The search operates as a ring search, continuing at the top after
 * the last entry has been visited.
 *
 * Returned is -1 if no match was found, else the absolute index of
 * the matching item.
 */
int do_isearch(char *str, int choice, int scroll)
{
	int found = 0;
	int i;

	for (i = 0; i < item_count(); i++) {
		item_set((choice + scroll + i)%item_count());
		if (strcasestr(item_str(), str)) {
			found = 1;
			break;
		}
	}

	if (found)
		return (choice + scroll + i)%item_count();
	return -1;
}

/*
 * Display a menu for choosing among a number of options
 */
int dialog_menu(const char *title, const char *prompt,
		const void *selected, int *s_scroll)
{
	int i, x, y, box_x, box_y;
	int key_match;		/* remember match in switch statement */
	int height, width, menu_height;
	int key = 0, button = 0, scroll = 0, choice = 0;
	int first_item =  0, max_choice;
	WINDOW *dialog, *menu;

do_resize:
	height = getmaxy(stdscr);
	width = getmaxx(stdscr);
	if (height < MENUBOX_HEIGTH_MIN || width < MENUBOX_WIDTH_MIN)
		return -ERRDISPLAYTOOSMALL;

	height -= 4;
	width  -= 5;
	menu_height = height - 10;

	max_choice = MIN(menu_height, item_count());

	/* center dialog box on screen */
	x = (getmaxx(stdscr) - width) / 2;
	y = (getmaxy(stdscr) - height) / 2;

	draw_shadow(stdscr, y, x, height, width);

	dialog = newwin(height, width, y, x);
	keypad(dialog, TRUE);

	draw_box(dialog, 0, 0, height, width,
		 dlg.dialog.atr, dlg.border.atr);
	wattrset(dialog, dlg.border.atr);
	mvwaddch(dialog, height - 3, 0, ACS_LTEE);
	for (i = 0; i < width - 2; i++)
		waddch(dialog, ACS_HLINE);
	wattrset(dialog, dlg.dialog.atr);
	wbkgdset(dialog, dlg.dialog.atr & A_COLOR);
	waddch(dialog, ACS_RTEE);

	print_title(dialog, title, width);

	wattrset(dialog, dlg.dialog.atr);
	print_autowrap_fill(dialog,
			   focus_on_buttons ? prompt : isearch_instructions,
			   width - 2, 4, 1, 3);

	menu_width = width - 6;
	box_y = height - menu_height - 5;
	box_x = (width - menu_width) / 2 - 1;

	/* create new window for the menu */
	menu = subwin(dialog, menu_height, menu_width,
		      y + box_y + 1, x + box_x + 1);
	keypad(menu, TRUE);

	/* draw a box around the menu items */
	draw_box(dialog, box_y, box_x, menu_height + 2, menu_width + 2,
		 dlg.menubox_border.atr, dlg.menubox.atr);

	if (menu_width >= 80)
		item_x = (menu_width - 70) / 2;
	else
		item_x = 4;

	/* Set choice to default item */
	item_foreach()
		if (selected && (selected == item_data()))
			choice = item_n();
	/* get the saved scroll info */
	scroll = *s_scroll;
	if ((scroll <= choice) && (scroll + max_choice > choice) &&
	   (scroll >= 0) && (scroll + max_choice <= item_count())) {
		first_item = scroll;
		choice = choice - scroll;
	} else {
		scroll = 0;
	}
	if ((choice >= max_choice)) {
		if (choice >= item_count() - max_choice / 2)
			scroll = first_item = item_count() - max_choice;
		else
			scroll = first_item = choice - max_choice / 2;
		choice = choice - scroll;
	}

	/* Print the menu */
	for (i = 0; i < max_choice; i++) {
		print_item(first_item + i, i, i == choice);
	}

	wnoutrefresh(menu);

	print_arrows(dialog, item_count(), scroll,
		     box_y, box_x + item_x + 1, menu_height);

	print_isearch(dialog, box_y, box_x + item_x + 5, menu_height, !focus_on_buttons);
	print_buttons(dialog, height, width, 0);
	wmove(menu, choice, item_x + 1);
	wrefresh(menu);

	while (key != KEY_ESC) {
		key = wgetch(menu);

		if (key < 256 && isalpha(key))
			key = tolower(key);
		/*
		 * These keys are handled for the focus on both,
		 * menu and buttons.
		 */
		key_match = 0;
		switch (key) {
		case KEY_DC:	/* delete key clears i-search string */
			key_match = 1;
			isearch_str[0] = '\0';
			break;
		case TAB:
			key_match = 1;
			focus_on_buttons = 1 - focus_on_buttons;
			wattrset(dialog, dlg.dialog.atr);
			print_autowrap_fill(dialog,
					   focus_on_buttons ? prompt : isearch_instructions,
					   width - 2, 4, 1, 3);
			break;
		case KEY_LEFT:
		case KEY_RIGHT:
			key_match = 1;
			if (!focus_on_buttons) {
				focus_on_buttons = 1;
				wattrset(dialog, dlg.dialog.atr);
				print_autowrap_fill(dialog, prompt, width - 2, 4, 1, 3);
				wnoutrefresh(dialog);
			}
			button = ((key == KEY_LEFT ? --button : ++button) < 0)
			    ? 4 : (button > 4 ? 0 : button);
			break;
		case KEY_ESC:
			key = on_key_esc(menu);
			continue;
		case KEY_RESIZE:
			on_key_resize();
			delwin(menu);
			delwin(dialog);
			goto do_resize;
		}
		if (key_match) {
			print_isearch(dialog, box_y, box_x + item_x + 5, menu_height, !focus_on_buttons);
			print_item(scroll + choice, choice, TRUE);
			print_buttons(dialog, height, width, button);
			wrefresh(menu);
			continue;	/* wait for another key press */
		}

		key_match = 0;
		switch (key) {
		case KEY_UP:
			key_match = 1;
			/* Remove highlight of current item */
			print_item(scroll + choice, choice, FALSE);
			if (choice < 2 && scroll) {
				/* Scroll menu down */
				do_scroll(menu, &scroll, -1);
				print_item(scroll, 0, FALSE);
			} else
				choice = MAX(choice - 1, 0);
			break;
		case KEY_DOWN:
			key_match = 1;
			/* Remove highlight of current item */
			print_item(scroll + choice, choice, FALSE);

			if ((choice > max_choice - 3) &&
			    (scroll + max_choice < item_count())) {
				/* Scroll menu up */
				do_scroll(menu, &scroll, 1);
				print_item(scroll+max_choice - 1,
					   max_choice - 1, FALSE);
			} else
				choice = MIN(choice + 1, max_choice - 1);
			break;
		case KEY_PPAGE:
			key_match = 1;
			/* Remove highlight of current item */
			print_item(scroll + choice, choice, FALSE);

			scrollok(menu, TRUE);
			for (i = 0; (i < max_choice); i++) {
				if (scroll > 0) {
					do_scroll(menu, &scroll, -1);
					print_item(scroll, 0, FALSE);
				} else {
					if (choice > 0)
						choice--;
				}
			}
			break;
		case KEY_NPAGE:
			key_match = 1;
			/* Remove highlight of current item */
			print_item(scroll + choice, choice, FALSE);
			for (i = 0; (i < max_choice); i++) {
				if (scroll + max_choice < item_count()) {
					do_scroll(menu, &scroll, 1);
					print_item(scroll+max_choice-1,
						   max_choice - 1, FALSE);
				} else {
					if (choice + 1 < max_choice)
						choice++;
				}
			}
		}

		if (key_match) {
			print_item(scroll + choice, choice, TRUE);
			print_arrows(dialog, item_count(), scroll,
				     box_y, box_x + item_x + 1, menu_height);
			wnoutrefresh(dialog);
			wrefresh(menu);
			continue;	/* wait for another key press */
		}

		if (focus_on_buttons) {
			/*
			 * Focus is on buttons, handle appropriate keys.
			 */
			switch (key) {
			case '+':
				/* Remove highlight of current item */
				print_item(scroll + choice, choice, FALSE);

				if ((choice > max_choice - 3) &&
				    (scroll + max_choice < item_count())) {
					/* Scroll menu up */
					do_scroll(menu, &scroll, 1);

					print_item(scroll+max_choice - 1,
						   max_choice - 1, FALSE);
				} else
					choice = MIN(choice + 1, max_choice - 1);
				print_item(scroll + choice, choice, TRUE);
				print_arrows(dialog, item_count(), scroll,
					     box_y, box_x + item_x + 1, menu_height);
				wnoutrefresh(dialog);
				wrefresh(menu);
				continue;	/* wait for another key press */
			case '-':
				/* Remove highlight of current item */
				print_item(scroll + choice, choice, FALSE);
				if (choice < 2 && scroll) {
					/* Scroll menu down */
					do_scroll(menu, &scroll, -1);
					print_item(scroll, 0, FALSE);
				} else
					choice = MAX(choice - 1, 0);
				print_item(scroll + choice, choice, TRUE);
				print_arrows(dialog, item_count(), scroll,
					     box_y, box_x + item_x + 1, menu_height);
				wnoutrefresh(dialog);
				wrefresh(menu);
				continue;	/* wait for another key press */
			case '\n':
				isearch_str[0] = '\0';
				/* fallthrough */
			case ' ':
			case 's':
			case 'y':
			case 'n':
			case 'm':
			case '/':
			case 'h':
			case '?':
			case 'z':
				/* save scroll info */
				*s_scroll = scroll;
				delwin(menu);
				delwin(dialog);
				item_set(scroll + choice);
				item_set_selected(1);
				switch (key) {
				case 'h':
				case '?':
					return 2;
				case 's':
				case 'y':
					return 5;
				case 'n':
					return 6;
				case 'm':
					return 7;
				case ' ':
					return 8;
				case '/':
					return 9;
				case 'z':
					return 10;
				case '\n':
					return button;
				}
				return 0;
			case 'e':
			case 'x':
				key = KEY_ESC;
				break;
			}
			continue;
		} else {	/* !focus_on_buttons */
			if (key == '\n') {
				/* save scroll info */
				*s_scroll = scroll;
				delwin(menu);
				delwin(dialog);
				item_set(scroll + choice);
				item_set_selected(1);
				isearch_str[0] = '\0';
				return 0; /* 0 means first button "Select" */
			} else if ( key == KEY_BACKSPACE ) {
				if ( isearch_str[0] )
					isearch_str[i = (strlen(isearch_str) - 1)] = '\0';
				/* Remove highlight of current item */
				print_item(scroll + choice, choice, FALSE);
				i = do_isearch(isearch_str, choice + 1, scroll);
			} else if (key == '\\') {
				/*
				 * Check \ before printable chars,
				 * because it is reserved to search
				 * further matches.
				 */
				/* Remove highlight of current item */
				print_item(scroll + choice, choice, FALSE);
				i = do_isearch(isearch_str, choice + 1, scroll);
			} else if (key < 256 && (isprint(key) || key == ' ')) {
				if (strlen(isearch_str) < ISEARCH_LEN - 1) {
					isearch_str[i = strlen(isearch_str)] = key;
					isearch_str[i+1] = '\0';
					/* Remove highlight of current item */
					print_item(scroll + choice, choice, FALSE);
					i = do_isearch(isearch_str, choice, scroll);
				} else
					continue;
			} else
				continue;

			/*
			 * Handle matches
			 */
			if (i >= 0) {
				i -= scroll;

				if (i >= max_choice)
					/*
					 * Handle matches below the currently visible menu entries.
					 */
					while (i >= max_choice) {
						do_scroll(menu, &scroll, 1);
						i--;
						print_item(max_choice + scroll - 1, max_choice - 1, false);
					}
				else if (i < 0)
					/*
					 * Handle matches higher in the menu (ring search).
					 */
					while (i < 0) {
						do_scroll(menu, &scroll, -1);
						i++;
						print_item(scroll, 0, false);
					}
				choice = i;
			} else {
				i = choice;
			}

			print_item(scroll + choice, choice, TRUE);
			print_isearch(dialog, box_y, box_x + item_x + 5, menu_height, true);
			print_arrows(dialog, item_count(), scroll,
				     box_y, box_x + item_x + 1, menu_height);

			wnoutrefresh(dialog);
			wrefresh(menu);
			continue;
		}
	}
	delwin(menu);
	delwin(dialog);
	isearch_str[0] = '\0';
	return key;		/* ESC pressed */
}
