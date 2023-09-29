/*** includes ***/

#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _GNU_SOURCE

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <termios.h>
#include <unistd.h>

/*** defines ***/

#define FEDIT_VERSION "0.0.1"

#define CTRL_KEY(k) ((k) & 0x1f)

enum editorKey{
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

typedef struct erow{
	int size;
	char *chars;
} erow;

struct editorConfig{
	int cx, cy;
	int rowoff;
	int coloff;
	int screenrows;
	int screencols;
	int numrows;
	erow *row;
	struct termios orig_termios;
};

struct editorConfig cfg;

/*** terminal ***/

void die(const char *s){
	write(STDOUT_FILENO, "\x1b[2J", 4);
	write(STDOUT_FILENO, "\x1b[H", 3);

	perror(s);
	exit(1);
}

void disableRawMode(){
	if(tcsetattr(STDIN_FILENO, TCSAFLUSH, &cfg.orig_termios) == -1)
		die("disableRawMode/tcsetattr");
}

void enableRawMode(){
	if(tcgetattr(STDIN_FILENO, &cfg.orig_termios) == -1) 
		die("enableRawMode/tcgetattr");
	atexit(disableRawMode);

	struct termios raw = cfg.orig_termios;
	raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
	raw.c_oflag &= ~(OPOST);
	raw.c_cflag |= (CS8);
	raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
	raw.c_cc[VMIN] = 0;
	raw.c_cc[VTIME] = 1;

	if(tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) 
		die("enableRawMode/tcsetattr");
}

int editorReadKey(){
	int nread;
	char c;
	while((nread = read(STDIN_FILENO, &c, 1)) != 1){
		if(nread == -1 && errno != EAGAIN) die("editorReadKey/read");
	}

	if(c == '\x1b'){
		char seq[3];

		if(read(STDIN_FILENO, &seq[0], 1) != 1) return '\x1b';
		if(read(STDIN_FILENO, &seq[1], 1) != 1) return '\x1b';

		if(seq[0] == '['){
			if(seq[1] >= '0' && seq[1] <= '9'){
				if(read(STDIN_FILENO, &seq[2], 1) != 1) return '\x1b';
				if(seq[2] == '~')
					switch(seq[1]){
						case '1': return HOME_KEY;
						case '3': return DEL_KEY;
						case '4': return END_KEY;
						case '5': return PAGE_UP;
						case '6': return PAGE_DOWN;
						case '7': return HOME_KEY;
						case '8': return END_KEY;
					}
			}
			else
				switch(seq[1]){
					case 'A': return ARROW_UP;
					case 'B': return ARROW_DOWN;
					case 'C': return ARROW_RIGHT;
					case 'D': return ARROW_LEFT;
					case 'F': return END_KEY;
					case 'H': return HOME_KEY;
				}
		}
		else if(seq[0] == 'O')
			switch(seq[1]){
				case 'F': return END_KEY;
				case 'H': return HOME_KEY;
			}

		return '\x1b';
	}
	else return c;
}

int getCursorPosition(int *rows, int *cols){
	char buf[32];
	unsigned int i = 0;

	if(write(STDOUT_FILENO, "\x1b[6n", 4) != 4) return -1;

	while(i < sizeof(buf) - 1){
		if(read(STDIN_FILENO, &buf[i], 1) != 1) break;
		if(buf[i] == 'R') break;
		i++;
	}
	buf[i] = '\0';

	if(buf[0] != '\x1b' || buf[1] != '[') return -1;
	if(sscanf(&buf[2], "%d;%d", rows, cols) != 2) return -1;

	return 0;
}

int getWindowSize(int *rows, int *cols){
	struct winsize ws;

	if(ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0){
		if(write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) != 12) return -1;
		return getCursorPosition(rows, cols);
	}
	else{
		*cols = ws.ws_col;
		*rows = ws.ws_row;
		return 0;
	}
}

/*** row operations ***/

void editorAppendRow(char *s, size_t len){
	cfg.row = realloc(cfg.row, sizeof(erow) * (cfg.numrows + 1));

	int at = cfg.numrows;
	cfg.row[at].size = len;
	cfg.row[at].chars = malloc(len + 1);
	memcpy(cfg.row[at].chars, s, len);
	cfg.row[at].chars[len] = '\0';
	cfg.numrows++;
}

/*** file i/o ***/

void editorOpen(char *filename){
	FILE *fp = fopen(filename, "r");
	if(!fp) die("editorOpen/fopen");

	char *line = NULL;
	size_t linecap = 0;
	ssize_t linelen;
	while((linelen = getline(&line, &linecap, fp)) != -1){
		while(linelen > 0 && (line[linelen - 1] == '\n' || 
							  line[linelen - 1] == '\r'))
			linelen--;
		editorAppendRow(line, linelen);
	}
	free(line);
	fclose(fp);
}

/*** append buffer ***/

struct abuf{
	char *b;
	int len;
};

#define ABUF_INIT {NULL, 0}

void abAppend(struct abuf *ab, const char *s, int len){
	char *new = realloc(ab->b, ab->len + len);

	if(new == NULL) return;
	memcpy(&new[ab->len], s, len);
	ab->b = new;
	ab->len += len;
}

void abFree(struct abuf *ab){
	free(ab->b);
}

/*** output ***/

void editorScroll(){
	if(cfg.cy < cfg.rowoff) 
		cfg.rowoff = cfg.cy;
	if(cfg.cy >= cfg.rowoff + cfg.screenrows) 
		cfg.rowoff = cfg.cy - cfg.screenrows + 1;
	if(cfg.cx < cfg.coloff)
		cfg.coloff = cfg.cx;
	if(cfg.cx >= cfg.coloff + cfg.screencols)
		cfg.coloff = cfg.cx - cfg.screencols + 1; 


}

void editorDrawRows(struct abuf *ab){
	for(int y = 0; y < cfg.screenrows; y++){
		int filerow = y + cfg.rowoff;
		if(filerow >= cfg.numrows){
			if(cfg.numrows == 0 && y == cfg.screenrows / 3){
				char welcome[80];
				int welcome_len = snprintf(welcome, sizeof(welcome), 
					"FEDIT --version %s", FEDIT_VERSION);
				if(welcome_len > cfg.screencols) welcome_len = cfg.screencols;
				int padding = (cfg.screencols - welcome_len) / 2;
				if(padding--) abAppend(ab, "~", 1);
				while(padding--) abAppend(ab, " ", 1);
				abAppend(ab, welcome, welcome_len);
			}
			else abAppend(ab, "~", 1);
		}
		else{
			int len = cfg.row[filerow].size - cfg.coloff;
			if(len < 0) len = 0;
			if(len > cfg.screencols) len = cfg.screencols;
			abAppend(ab, &cfg.row[filerow].chars[cfg.coloff], len);
		}

		abAppend(ab, "\x1b[K", 3);
		if(y < cfg.screenrows - 1)
			abAppend(ab, "\r\n", 2);
	}
}

void editorRefreshScreen(){
	editorScroll();

	struct abuf ab = ABUF_INIT;

	abAppend(&ab, "\x1b[?25l", 6);
	abAppend(&ab, "\x1b[H", 3);

	editorDrawRows(&ab);

	char buf[32];
	snprintf(buf, sizeof(buf), "\x1b[%d;%dH", (cfg.cy - cfg.rowoff) + 1, 
											  (cfg.cx - cfg.coloff) + 1);
	abAppend(&ab, buf, strlen(buf));

	abAppend(&ab, "\x1b[?25h", 6);

	write(STDOUT_FILENO, ab.b, ab.len);
	abFree(&ab);
}

/*** input ***/

void editorMoveCursor(int key)
{
	switch(key){
		case ARROW_LEFT:
			if(cfg.cx != 0) cfg.cx--;
			break;
		case ARROW_RIGHT:
			cfg.cx++;
			break;
		case ARROW_UP:
			if(cfg.cy != 0) cfg.cy--;
			break;
		case ARROW_DOWN:
			if(cfg.cy < cfg.numrows) cfg.cy++;
			break;
	}
}

void editorProcessKeypress(){
	int c = editorReadKey();

	switch(c){
		case CTRL_KEY('q'):
			write(STDOUT_FILENO, "\x1b[2J", 4);
			write(STDOUT_FILENO, "\x1b[H", 3);
			exit(0);
			break;

		case HOME_KEY:
			cfg.cx = 0;
			break;
		case END_KEY:
			cfg.cx = cfg.screencols - 1;
			break;

		case PAGE_UP:
		case PAGE_DOWN:{
			int times = cfg.screenrows;
			while(times--) 
				editorMoveCursor(c == PAGE_UP ? ARROW_UP : ARROW_DOWN);
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

/*** init ***/

void initEditor(){
	cfg.cx = 0;
	cfg.cy = 0;
	cfg.rowoff = 0;
	cfg.coloff = 0;
	cfg.numrows = 0;
	cfg.row = NULL;

	if(getWindowSize(&cfg.screenrows, &cfg.screencols) == -1)
		die("initEditor/getWindowSize");
}

int main(int argc, char *argv[]){
	enableRawMode();
	initEditor();
	if(argc >= 2){
		editorOpen(argv[1]);
	}
	
	while (1){
		editorRefreshScreen();
		editorProcessKeypress();
	}

	return 0;
}