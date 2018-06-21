/* Includes */
#include <panel.h>

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <fcntl.h>

#ifdef __APPLE__
#include <util.h>
#else
#include <pty.h>
#endif

/* Macro definitions */
#define CONFIG_LOC  "~/.config/.lewmrc"
#define START_WIDTH  80
#define START_HEIGHT 24
#define BLUEBLACK  1
#define REDBLACK   2
#define GREENBLACK 3
#define KEY_ESC 0x1b
#define SPEED 10 //refresh time in ms

/* Struct declarations */
typedef struct _PANEL_DATA
{
	unsigned short x, y, width, height, num;
	unsigned char hide : 1;
	
	char * title; 
	int    titlecolor;
	
	int   pty_fd;
	pid_t pty_pid;
	
	PANEL *next;
	
} PANEL_DATA;

/* Enum declarations */
enum status
{
	NORMAL,
	ESC,
	SUPER
};

enum sizemode
{
	MOVE,
	RESIZE
};

/* Function declarations */
int add_win(PANEL *next, char *title, int titlecolor, int height, int width, int y, int x, char *command);
void draw_win(PANEL *win_panel);
void move_resize(enum sizemode mode);
void signal_handle(int signum);
void pty_main(char *command); /* Needs to be linked in */
void update_pty_windows();

/* Global variable declarations */
WINDOW     ** windows;
WINDOW     ** inner;
PANEL      ** panels;
PANEL_DATA *  data_top;
PANEL      *  panel_top;
char       ** hidden;
int           window_num, max_y, max_x, childcount;

/* Function bodies */
int main(void)
{
	int keystroke;
	enum status keystatus; //For multi-key stuff
	char send[1];

	keystatus = NORMAL;
	initscr();            //start ncurses
	start_color();        //init colors
	timeout(SPEED);       //set getch() timeout
	noecho();             //don't print everything the user types
	keypad(stdscr, true); //get special keys
	init_pair(BLUEBLACK,  COLOR_BLUE,  COLOR_BLACK);
	init_pair(REDBLACK,   COLOR_RED,   COLOR_BLACK);
	init_pair(GREENBLACK, COLOR_GREEN, COLOR_BLACK);
	getmaxyx(stdscr, max_y, max_x);
	signal(SIGINT, SIG_IGN);
	signal(SIGHUP,  signal_handle);
	signal(SIGTERM, signal_handle);

	panel_top = NULL;
	data_top = panel_userptr(panel_top);
	
	/*       stack-next     title      title color    height       width     y  x   command*/
	add_win( panel_top, "window name", BLUEBLACK, START_HEIGHT, START_WIDTH, 1, 1 , "zsh");

	/* Main loop */
	while (1)
	{
		keystroke = getch();
		switch(keystroke)
		{
			case ERR: /* no key pressed */
				break;
			case 9: /* tab */
				if (keystatus == ESC) //For ALT+TAB, as ESC and ALT emit the same code
				{
					if (data_top->next == NULL)
					{
						panel_top = panels[window_num-1];
						top_panel(panels[window_num-1]);
					}
					else
					{
						panel_top = data_top->next;
						top_panel(data_top->next);
					}
					data_top = (PANEL_DATA *) panel_userptr(panel_top);
				}
				keystatus = NORMAL;
				break;
			case KEY_ESC: /* ALT and ESC generally emit the same code */
				if (keystatus != ESC)
					keystatus = ESC;
				else
					keystatus = NORMAL;
				break;
			case KEY_F(1):
				if (keystatus == ESC) /* ESC+F1 */
				{
					attron(COLOR_PAIR(GREENBLACK));
					mvprintw(max_y-1, 0, "%-50s", "Moving mode, press <ENTER> to exit");
					attroff(COLOR_PAIR(GREENBLACK));
					refresh();
					move_resize(MOVE);
				}
				keystatus = NORMAL;
				break;
			case KEY_F(2):
				if (keystatus == ESC) /* ESC+F2 */
				{
					attron(COLOR_PAIR(GREENBLACK));
					mvprintw(max_y-1, 0, "%-50s", "Resizing mode, press <ENTER> to exit");
					attroff(COLOR_PAIR(GREENBLACK));
					refresh();
					move_resize(RESIZE);
				}
				keystatus = NORMAL;
				break;
			case KEY_F(8):
				if (keystatus == ESC)
				{
					add_win( panel_top, "window name", BLUEBLACK, START_HEIGHT, START_WIDTH, 1, 1 , "zsh");
				}
				break;
			case KEY_F(12):
				if (keystatus == ESC) /* ESC+F12 */
				{
					endwin();
					signal_handle(SIGKILL);
				}
			default:
				keystatus = NORMAL;
				send[0] = keystroke;
				write(data_top->pty_fd, send, 1);
		}
		update_pty_windows();
		update_panels();
		doupdate();
		refresh();
	}
	return 0;
}

void signal_handle(int signum)
{
	kill(0, signum); //TODO: Kill children nicely
}

/* Creates a new window and draws it */
int add_win(PANEL *next, char *title, int titlecolor, int height, int width, int y, int x, char *command)
{
	PANEL_DATA *data;
	struct winsize size_struct;

	/* Create window and panel */
	window_num++;
	windows   = realloc(windows,   sizeof(WINDOW*) * window_num-1);
	panels    = realloc(panels,    sizeof(PANEL*)  * window_num-1);
	inner     = realloc(inner, sizeof(WINDOW*) * window_num-1);
	windows   [window_num-1] = newwin(height, width, y, x);
	panels    [window_num-1] = new_panel(windows[window_num-1]);
	inner [window_num-1] = derwin(windows[window_num-1], height - 4, width - 3, 3, 2);
	scrollok(inner[window_num-1], TRUE);
	update_panels();
	
	/* Create and populate panel data struct */
	data = malloc(sizeof(PANEL_DATA));
	data->x          = x;
	data->y          = y;
	data->height     = height;
	data->width      = width;
	data->next       = next;
	data->title      = title;
	data->titlecolor = titlecolor;
	data->hide       = 0;
	data->num        = window_num;

	/* Populate size_struct for the pty */
	size_struct.ws_row = height;
	size_struct.ws_col = width;

	/* Make pty */
	endwin(); //end ncurses temporarily so the child isn't cooked, this is not noticable
	data->pty_pid = forkpty(&(data->pty_fd), NULL, NULL, &size_struct); //forkpty(*amaster,*aslave,*name,termios *termp,winsize *winp) -- name should be NULL
	
	if (data->pty_pid == -1)
		return -1; //Fork failed
	else if (data->pty_pid == 0) //If 0, then we're in the child
		pty_main(command);
		
	refresh(); //go back to ncurses
	fcntl(data->pty_fd, F_SETFL, fcntl(data->pty_fd, F_GETFL, 0) | O_NONBLOCK); //Set the file descripter to non-blocking mode
	
	/* Finish up */
	set_panel_userptr(panels[window_num-1], data); //attach data to panel
	draw_win(panels[window_num-1]);

	data_top  = panel_userptr(panels[window_num-1]); 
	panel_top = panels[window_num-1];
	top_panel(panels[window_num-1]);
	return 0;
}

/* Draws a window */
void draw_win(PANEL *win_panel)
{
	unsigned short width, num;
	chtype titlecolor;
	char *title;
	WINDOW *win;
	PANEL_DATA *data;
	
	win = panel_window(win_panel);
	data = panel_userptr(win_panel);
	num        = data -> num;
	width      = data -> width;
	title      = data -> title;
	titlecolor = COLOR_PAIR(data -> titlecolor);

	/* Draw window border and titlebar */
	box(win, 0, 0);
	mvwaddch(win, 2, 0,       ACS_LTEE); //print connectors for bottom bar of titlebar
	mvwaddch(win, 2, width-1, ACS_RTEE);
	mvwhline(win, 2, 1, ACS_HLINE, width - 2); //print bottom bar of titlebar

	/* Print title in color */
	wattron(win, titlecolor);
	mvwprintw(win, 1, 2, "%s : %u", title, num);
	wattroff(win, titlecolor);
	touchwin(win);
	wrefresh(win);
	refresh();
}


/* Move/resize mode */
void move_resize(enum sizemode mode)
{
	int keystroke, max_x, max_y, title_len;
	char *temp_string;
	WINDOW *old_window;
	WINDOW *temp_window;

	asprintf(&temp_string, "%s : %u", data_top->title, data_top->num);
	title_len = strlen(temp_string);

	getmaxyx(stdscr, max_y, max_x);
	
	while ((keystroke = getch()))
	{
		switch (keystroke)
		{
			case KEY_UP:
				if (mode == MOVE)
				{
					if (data_top->y > 0)
						(data_top->y)--;
				}
				else
				{
					if (data_top->height > 5)
						(data_top->height)--;
				}
				break;
			case KEY_DOWN:
				if ((data_top->y + data_top->height) < max_y-1)
				{
					if (mode == MOVE)
						(data_top->y)++;
					else
						(data_top->height)++;
				}
				break;
			case KEY_LEFT:
				if (mode == MOVE)
				{
					if (data_top->x > 0)
						(data_top->x)--;
				}
				else
				{
					if (data_top->width > (title_len + 4))
						(data_top->width)--;
				}
				break;
			case KEY_RIGHT:
				if ((data_top->x + data_top->width) < max_x)
				{
					if (mode == MOVE)
						(data_top->x)++;
					else
						(data_top->width)++;
				}
				break;
			case '\n': /* Enter */
				move(max_y-1, 0);
				clrtoeol(); //clear to end of line
				return;
		}
		if (mode == MOVE)
		{
			move_panel(panel_top, data_top->y, data_top->x);
			mvwin(inner[data_top->num-1], data_top->y + 3, data_top->x + 2);
		}
		else
		{
			old_window = panel_window(panel_top);
			temp_window = newwin(data_top->height, data_top->width, data_top->y, data_top->x);
			replace_panel(panel_top, temp_window);
			delwin(old_window);
			refresh();
			draw_win(panel_top);
		}
		refresh();
		update_panels();
		doupdate();
	}
}

/* Outputs pty data to window */
void update_pty_windows()
{
	int ret = 1;
	char c[1024];
	WINDOW * win = inner[(data_top->num)-1];
	c[1023] = '\0';

	if ((ret = read(data_top->pty_fd, c, 1023)) == -1)
		return;
	
	wprintw(win, c);
	wrefresh(win);
	return;
}
