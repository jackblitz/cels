#include "terminal.h"

#if defined(_WIN32)
    #include <windows.h>
    #include <conio.h>
    #if defined(MOUSE_MOVED)
        #undef MOUSE_MOVED
    #endif
#else
    #include <unistd.h>
#endif

#include <curses.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>

static bool s_interactive = false;
static bool s_cursesActive = false;
static int  s_currRow = 0;

void CelsTerminalRestore(void)
{
    if (s_cursesActive) {
        curs_set(1);
        endwin();
        s_cursesActive = false;
    }
}

void CelsTerminalInit(bool interactive)
{
    s_interactive = interactive;
    if (!interactive) {
        return;
    }

#if defined(_WIN32)
    HANDLE hIn = GetStdHandle(STD_INPUT_HANDLE);
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    if (hIn == INVALID_HANDLE_VALUE || hOut == INVALID_HANDLE_VALUE ||
        GetFileType(hIn) != FILE_TYPE_CHAR || GetFileType(hOut) != FILE_TYPE_CHAR) {
        /* Not an interactive console window (pipe or redirection) - fallback to stdout */
        return;
    }

    CONSOLE_SCREEN_BUFFER_INFO csbi;
    if (GetConsoleScreenBufferInfo(hOut, &csbi)) {
        SHORT cols = csbi.srWindow.Right - csbi.srWindow.Left + 1;
        SHORT rows = csbi.srWindow.Bottom - csbi.srWindow.Top + 1;
        bool needResize = false;
        if (cols < 80) { cols = 80; needResize = true; }
        if (rows < 28) { rows = 28; needResize = true; }
        if (needResize) {
            COORD maxCoord = GetLargestConsoleWindowSize(hOut);
            if (cols > maxCoord.X) cols = maxCoord.X;
            if (rows > maxCoord.Y) rows = maxCoord.Y;
            COORD bSize = { cols, csbi.dwSize.Y < 120 ? 120 : csbi.dwSize.Y };
            SetConsoleScreenBufferSize(hOut, bSize);
            SMALL_RECT wSize = { 0, 0, (SHORT)(cols - 1), (SHORT)(rows - 1) };
            SetConsoleWindowInfo(hOut, TRUE, &wSize);
        }
    }
#endif

    if (initscr() != NULL) {
        s_cursesActive = true;
        cbreak();
        noecho();
        nodelay(stdscr, TRUE);
        keypad(stdscr, TRUE);
        curs_set(0); /* Hide cursor */
        resize_term(28, 80);

        if (has_colors()) {
            start_color();
            use_default_colors();
            init_pair(1, COLOR_CYAN, -1);    /* Header and Border */
            init_pair(2, COLOR_GREEN, -1);   /* High Progress / Health */
            init_pair(3, COLOR_YELLOW, -1);  /* Medium Progress */
            init_pair(4, COLOR_WHITE, -1);   /* Regular content text */
            init_pair(5, COLOR_MAGENTA, -1); /* Footer controls */
        }

        atexit(CelsTerminalRestore);
    }
}

void CelsTerminalBeginFrame(void)
{
    if (s_cursesActive) {
        erase();
        s_currRow = 0;
    }
}

void CelsTerminalEndFrame(void)
{
    if (s_cursesActive) {
        refresh();
    } else {
        fflush(stdout);
    }
}

void CelsTerminalDrawDivider(void)
{
    if (s_cursesActive) {
        wattron(stdscr, COLOR_PAIR(1));
        mvwprintw(stdscr, s_currRow++, 0, "+----------------------------------------------------------------------------+");
        wattroff(stdscr, COLOR_PAIR(1));
    } else {
        printf("+----------------------------------------------------------------------------+\n");
    }
}

void CelsTerminalDrawHeader(const char *title, const char *profile)
{
    if (s_cursesActive) {
        CelsTerminalDrawDivider();
        wattron(stdscr, COLOR_PAIR(1) | A_BOLD);
        mvwprintw(stdscr, s_currRow, 0, "| ");
        mvwprintw(stdscr, s_currRow, 2, "%-54.54s", title ? title : "");
        mvwprintw(stdscr, s_currRow, 57, "%19.19s", profile ? profile : "");
        mvwprintw(stdscr, s_currRow, 77, "|");
        wattroff(stdscr, COLOR_PAIR(1) | A_BOLD);
        s_currRow++;
        CelsTerminalDrawDivider();
    } else {
        CelsTerminalDrawDivider();
        printf("| %-54.54s %19.19s |\n", title ? title : "", profile ? profile : "");
        CelsTerminalDrawDivider();
    }
}

void CelsTerminalDrawLine(const char *fmt, ...)
{
    char buf[256];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    if (s_cursesActive) {
        wattron(stdscr, COLOR_PAIR(1));
        mvwprintw(stdscr, s_currRow, 0, "| ");
        wattroff(stdscr, COLOR_PAIR(1));

        wattron(stdscr, COLOR_PAIR(4));
        mvwprintw(stdscr, s_currRow, 2, "%-74.74s", buf);
        wattroff(stdscr, COLOR_PAIR(4));

        wattron(stdscr, COLOR_PAIR(1));
        mvwprintw(stdscr, s_currRow, 77, "|");
        wattroff(stdscr, COLOR_PAIR(1));

        s_currRow++;
    } else {
        printf("| %-74.74s |\n", buf);
    }
}

void CelsTerminalDrawEmptyLine(void)
{
    if (s_cursesActive) {
        wattron(stdscr, COLOR_PAIR(1));
        mvwprintw(stdscr, s_currRow++, 0, "| %-74s |", "");
        wattroff(stdscr, COLOR_PAIR(1));
    } else {
        printf("| %-74s |\n", "");
    }
}

void CelsTerminalDrawProgressBar(const char *label, float current, float max, const char *extra)
{
    char bar[31];
    int pct = (max > 0.0f) ? (int)((current / max) * 100.0f) : 0;
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    int filled = (pct * 30) / 100;
    for (int i = 0; i < 30; ++i) {
        bar[i] = (i < filled) ? '#' : '.';
    }
    bar[30] = '\0';

    char extraBuf[32];
    if (extra && extra[0]) {
        snprintf(extraBuf, sizeof(extraBuf), " %s", extra);
    } else {
        extraBuf[0] = '\0';
    }

    if (s_cursesActive) {
        char fullLine[128];
        snprintf(fullLine, sizeof(fullLine), "%s: [%s] %3d%%%s",
                 label ? label : "Progress", bar, pct, extraBuf);

        wattron(stdscr, COLOR_PAIR(1));
        mvwprintw(stdscr, s_currRow, 0, "| ");
        wattroff(stdscr, COLOR_PAIR(1));

        int colorPair = (pct >= 50) ? 2 : 3;
        wattron(stdscr, COLOR_PAIR(colorPair) | A_BOLD);
        mvwprintw(stdscr, s_currRow, 2, "%-74.74s", fullLine);
        wattroff(stdscr, COLOR_PAIR(colorPair) | A_BOLD);

        wattron(stdscr, COLOR_PAIR(1));
        mvwprintw(stdscr, s_currRow, 77, "|");
        wattroff(stdscr, COLOR_PAIR(1));

        s_currRow++;
    } else {
        char content[128];
        snprintf(content, sizeof(content), "%s: [%s] %3d%%%s",
                 label ? label : "Progress", bar, pct, extraBuf);
        printf("| %-74.74s |\n", content);
    }
}

void CelsTerminalDrawFooter(const char *controls)
{
    if (s_cursesActive) {
        CelsTerminalDrawDivider();
        wattron(stdscr, COLOR_PAIR(1));
        mvwprintw(stdscr, s_currRow, 0, "| ");
        wattroff(stdscr, COLOR_PAIR(1));

        wattron(stdscr, COLOR_PAIR(5) | A_BOLD);
        mvwprintw(stdscr, s_currRow, 2, "%-74.74s", controls ? controls : "");
        wattroff(stdscr, COLOR_PAIR(5) | A_BOLD);

        wattron(stdscr, COLOR_PAIR(1));
        mvwprintw(stdscr, s_currRow, 77, "|");
        wattroff(stdscr, COLOR_PAIR(1));

        s_currRow++;
        CelsTerminalDrawDivider();
        CelsTerminalDrawDivider();
        printf("| %-74.74s |\n", controls ? controls : "");
        CelsTerminalDrawDivider();
    }
}

int CelsTerminalPollKey(void)
{
    if (s_cursesActive) {
        int ch = getch();
        if (ch == ERR) {
            return 0;
        }
        return ch;
    }

#if defined(_WIN32)
    if (_kbhit()) {
        int ch = _getch();
        if (ch == 0 || ch == 224) {
            int ext = _getch();
            if (ext == 72) return 0x103; /* KEY_UP */
            if (ext == 80) return 0x102; /* KEY_DOWN */
            if (ext == 75) return 0x104; /* KEY_LEFT */
            if (ext == 77) return 0x105; /* KEY_RIGHT */
            return 0;
        }
        return ch;
    }
#endif
    return 0;
}

void CelsTerminalSleepMs(int ms)
{
#if defined(_WIN32)
    Sleep(ms);
#else
    usleep((useconds_t)ms * 1000);
#endif
}
