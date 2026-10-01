/* PDCurses */

#include "pdcwin.h"

/* get the cursor size/shape */

int PDC_get_cursor_mode(void)
{
    CONSOLE_CURSOR_INFO ci;

    PDC_LOG(("PDC_get_cursor_mode() - called\n"));

    GetConsoleCursorInfo(pdc_con_out, &ci);

    return ci.dwSize;
}

/* return number of screen rows */

int PDC_get_rows(void)
{
    CONSOLE_SCREEN_BUFFER_INFO scr;

    PDC_LOG(("PDC_get_rows() - called\n"));

    if (GetConsoleScreenBufferInfo(pdc_con_out, &scr)) {
        int rows = scr.srWindow.Bottom - scr.srWindow.Top + 1;
        if (rows < 24) {
            if (scr.dwSize.Y >= 24) {
                rows = (scr.dwSize.Y < 50) ? (int)scr.dwSize.Y : 25;
            } else {
                rows = 25;
            }
        }
        return rows;
    }

    return 25;
}

/* return width of screen/viewport */

int PDC_get_columns(void)
{
    CONSOLE_SCREEN_BUFFER_INFO scr;

    PDC_LOG(("PDC_get_columns() - called\n"));

    if (GetConsoleScreenBufferInfo(pdc_con_out, &scr)) {
        int cols = scr.srWindow.Right - scr.srWindow.Left + 1;
        if (cols < 40) {
            if (scr.dwSize.X >= 40) {
                cols = (int)scr.dwSize.X;
            } else {
                cols = 80;
            }
        }
        return cols;
    }

    return 80;
}
