/*** includes ***/

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

/*** defines ***/

#define KILO_VERSION "0.0.1"
#define CTRL_KEY(k) ((k) & 0x1f)

enum editorKey {
	ARROW_LEFT = 1000,
	ARROW_RIGHT,
	ARROW_UP,
	ARROW_DOWN,
	DEL_KEY,
	HOME_KEY,
	END_KEY,
	PAGE_UP,
	PAGE_DOWN
};

/*** data ***/

// struct for editor config
struct editorConfig {
	int cx, cy;
	int screenrows;
	int screencols;
	struct termios orig_termios;
};

struct editorConfig E;

/*** terminal ***/

// function to handle exiting when error occurs
void die(const char *s) {
	// clear screen and reposition cursor
	write(STDOUT_FILENO, "\x1b[2J", 4);
	write(STDOUT_FILENO, "\x1b[H", 3);

	// display error and exit program
	perror(s);
	exit(1);
}

// function to restore canonical mode after exiting program
void disableRawMode() {
	if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.orig_termios) == -1) { 
		die("tcsetattr");
	}
}

// function to enable raw mode
// in raw mode, data is read byte-by-byte and other things
void enableRawMode() {
	// get the current attributes
	if (tcgetattr(STDIN_FILENO, &E.orig_termios) == -1) die("tcgetattr");
	atexit(disableRawMode);

	// struct for manipulating terminal attributes
	struct termios raw = E.orig_termios;
	// the following flags turn off the corresponding items:
 
	// ixon -- ctrl-s and ctrl-q = XOFF and XON
	// icrnl -- ctrl-m = carriage return
	// brkint -- sends sigint signals to the program
	// inpck -- enables parity checking
	// istrip -- strips 8th bit of each input byte
	raw.c_iflag &= ~(IXON | ICRNL | BRKINT | INPCK | ISTRIP);

	// opost -- turns off output processing
	raw.c_oflag &= ~(OPOST);
	
	// cs8 -- sets character size to 8 bits per byte
	raw.c_cflag |= (CS8);
	
	// echo -- showing output as the user types 
	// icanon -- canonical mode = reading input line-by-line
	// iexten -- ctrl-v
	// isig -- sigint signals
	raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);

	// vmin sets the min no of bytes of input needed before read can return
	raw.c_cc[VMIN] = 0;
	// vtime sets max time to wait before read returns (unit 1/10 seconds)
	raw.c_cc[VTIME] = 1;

	if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) die("tcsetattr");
}

// function to read key presses
int editorReadKey() {
	int nread;
	char c;
	while ((nread = read(STDIN_FILENO, &c, 1)) != 1) {
		if (nread == -1 && errno != EAGAIN) die ("read");
	}

	if (c == '\x1b') {
		char seq[3];

		if (read(STDIN_FILENO, &seq[0], 1) != 1) return '\x1b';
		if (read(STDIN_FILENO, &seq[1], 1) != 1) return '\x1b';

		if (seq[0] == '[') {
			if (seq[1] >= '0' && seq[1] <= '9') {
				if (read(STDIN_FILENO, &seq[2], 1) != 1) return '\x1b';
				if (seq[2] == '~') {
					switch (seq[1]) {
						case '1': return HOME_KEY;
						case '3': return DEL_KEY;
						case '4': return END_KEY;
						case '5': return PAGE_UP;
						case '6': return PAGE_DOWN;
						case '7': return HOME_KEY;
						case '8': return END_KEY;
					}
				}
			} else {
				switch (seq[1]) {
					case 'A': return ARROW_UP;
					case 'B': return ARROW_DOWN;
					case 'C': return ARROW_RIGHT;
					case 'D': return ARROW_LEFT;
					case 'H': return HOME_KEY;
					case 'F': return END_KEY;
				}
			}
		} else if (seq[0] == 'O') {
			switch (seq[1]) {
				case 'H': return HOME_KEY;
				case 'F': return END_KEY;
			}
		}

		return '\x1b';
	} else {
		return c;
	}
}

int getCursorPosition(int *rows, int *cols) {
	char buf[32];
	unsigned int i = 0;

	if (write(STDOUT_FILENO, "\x1b[6n", 4) != 4) return -1;

	while (i < sizeof(buf) - 1) {
		if (read(STDIN_FILENO, &buf[i], 1) != 1) break;
		if (buf[i] == 'R') break;
		i++;
	}
	buf[i] = '\0';

	if (buf[0] != '\x1b' || buf[1] != '[') return -1;
	if (sscanf(&buf[2], "%d;%d", rows, cols) != 2) return -1;

	return 0;
}

int getWindowSize(int *rows, int *cols) {
	struct winsize ws;

	if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
		if (write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) != 12) return -1;
		return getCursorPosition(rows, cols);
	} else {
		*cols = ws.ws_col;
		*rows = ws.ws_row;
		return 0;
	}
}

/*** append buffer ***/

// append buffer contains a pointer to our buffer in memory and a length
struct abuf {
	char *b;
	int len;
};

// constructor for append buffer
#define ABUF_INIT {NULL, 0}

// append method for buffer
void abAppend(struct abuf *ab, const char *s, int len) {
	char *new = realloc(ab->b, ab->len + len);

	if (new == NULL) return;
	memcpy(&new[ab->len], s, len);
	ab->b = new;
	ab->len += len;
}

// destructor for deallocating dynamic memory
void abFree(struct abuf *ab) {
	free(ab->b);
}

/*** input ***/

// function to move the cursor
void editorMoveCursor(int key) {
	switch (key) {
		case ARROW_LEFT:
			if (E.cx != 0) {
				E.cx--;
			}
			break;
		case ARROW_RIGHT:
			if (E.cx != E.screencols - 1) {
				E.cx++;
			}
			break;
		case ARROW_UP:
			if (E.cy != 0) {
				E.cy--;
			}
			break;
		case ARROW_DOWN:
			if (E.cy != E.screenrows - 1) {
				E.cy++;
			}
			break;
	}
}

// function to process key presses
void editorProcessKeyPress() {
	int c = editorReadKey();

	switch (c) {
		case CTRL_KEY('q'):
			// clear screen and reposition cursor
			write(STDOUT_FILENO, "\x1b[2J", 4);
			write(STDOUT_FILENO, "\x1b[H", 3);

			// exit the program
			exit(0);
			break;

		case HOME_KEY:
			E.cx = 0;
			break;

		case END_KEY:
			E.cx = E.screencols - 1;
			break;

		case PAGE_UP:
		case PAGE_DOWN:
			{
				int times = E.screenrows;
				while (times--) editorMoveCursor(c == PAGE_UP ? ARROW_UP:ARROW_DOWN);
			}
			break;

		case ARROW_UP:
		case ARROW_DOWN:
		case ARROW_LEFT:
		case ARROW_RIGHT:
			editorMoveCursor(c);
			break;
	}
}

/*** output ***/

// function to draw tildes in empty rows
void editorDrawRows(struct abuf *ab) {
	int y;
	for (y = 0; y < E.screenrows; y++) {
		// display the welcome message
		if (y == E.screenrows / 3) {
			char welcome[80];
			int welcomelen = snprintf(welcome, sizeof(welcome),
					"Kilo editor -- version %s", KILO_VERSION);
			if (welcomelen > E.screencols) welcomelen = E.screencols;
			// centering the message
			int padding = (E.screencols - welcomelen) / 2;
			if (padding) {
				abAppend(ab, "~", 1);
				padding--;
			}
			while (padding--) abAppend(ab, " ", 1);
			abAppend(ab, welcome, welcomelen);
		} else {
			abAppend(ab, "~", 1);
		}

		// clears the line
		abAppend(ab, "\x1b[K", 3);
		if (y < E.screenrows - 1) {
			abAppend(ab, "\r\n", 2);
		}
	}
}

// function to refresh editor screen
void editorRefreshScreen() {
	// initialize append buffer
	struct abuf ab = ABUF_INIT;

	// hide the cursor
	abAppend(&ab, "\x1b[?25l", 6);
	// reposition cursor to the start
	abAppend(&ab, "\x1b[1;1H", 6);

	// draw tildes
	editorDrawRows(&ab);

	// reposition cursor
	char buf[32];
	snprintf(buf, sizeof(buf), "\x1b[%d;%dH", E.cy + 1, E.cx + 1);
	abAppend(&ab, buf, strlen(buf));

	// show the cursor
	abAppend(&ab, "\x1b[?25h", 6);

	// print out the append buffer to screen and free memory
	write(STDOUT_FILENO, ab.b, ab.len);
	abFree(&ab);
}

/*** init ***/

void initEditor() {
	E.cx = 0;
	E.cy = 0;
	if (getWindowSize(&E.screenrows, &E.screencols) == -1) die("getWindowSize");
}

int main() {
	enableRawMode();
	initEditor();

	char c;
	while (1){
		editorRefreshScreen();
		editorProcessKeyPress();
	}

	return 0;
}
