/* guest_display_mode: every display answer the guest can ask for must be the one mode the host set (the driver's
 * primary monitor, core/user: user::CurrentDisplayMode). argv[1] is the expected mode "WxH@Hz" (the integration test
 * sets WIN10XBOX_DISPLAY_MODE to the same text); without it the program only prints (the console picker runs it with
 * no argument and the run is judged by the four lines below).
 *
 * Output lines:
 *   display: GetSystemMetrics=<w>x<h>
 *   display: GetDeviceCaps=<w>x<h> vrefresh=<hz> bitspixel=<n>
 *   display: EnumDisplaySettings=<w>x<h>@<hz> bpp=<n>
 *   display: monitor=<w>x<h>
 *   display: done
 * Exits 42 when all four agree with each other (and with argv[1] if given), 1 otherwise (the line before says why). */
#include <windows.h>
#include <stdio.h>

int main(int argc, char **argv) {
    int sm_w = GetSystemMetrics(SM_CXSCREEN), sm_h = GetSystemMetrics(SM_CYSCREEN);
    HDC dc = GetDC(NULL);
    int dc_w = GetDeviceCaps(dc, HORZRES), dc_h = GetDeviceCaps(dc, VERTRES);
    int dc_hz = GetDeviceCaps(dc, VREFRESH), dc_bpp = GetDeviceCaps(dc, BITSPIXEL);
    DEVMODEA dm;
    MONITORINFO mi;
    HMONITOR mon;
    POINT origin;
    int ed_ok, mon_w = 0, mon_h = 0, ok = 1;
    unsigned ew = 0, eh = 0, ehz = 0;

    ReleaseDC(NULL, dc);
    memset(&dm, 0, sizeof dm);
    dm.dmSize = sizeof dm;
    ed_ok = EnumDisplaySettingsA(NULL, ENUM_CURRENT_SETTINGS, &dm);
    memset(&mi, 0, sizeof mi);
    mi.cbSize = sizeof mi;
    origin.x = origin.y = 0;
    mon = MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY);
    if (mon && GetMonitorInfoA(mon, &mi)) {
        mon_w = mi.rcMonitor.right - mi.rcMonitor.left;
        mon_h = mi.rcMonitor.bottom - mi.rcMonitor.top;
    }
    printf("display: GetSystemMetrics=%dx%d\n", sm_w, sm_h);
    printf("display: GetDeviceCaps=%dx%d vrefresh=%d bitspixel=%d\n", dc_w, dc_h, dc_hz, dc_bpp);
    printf("display: EnumDisplaySettings=%lux%lu@%lu bpp=%lu\n", dm.dmPelsWidth, dm.dmPelsHeight, dm.dmDisplayFrequency,
           dm.dmBitsPerPel);
    printf("display: monitor=%dx%d\n", mon_w, mon_h);
    fflush(stdout);

    if (!ed_ok) { printf("display: FAIL EnumDisplaySettings returned FALSE\n"); ok = 0; }
    if (dc_w != sm_w || dc_h != sm_h || (int)dm.dmPelsWidth != sm_w || (int)dm.dmPelsHeight != sm_h ||
        mon_w != sm_w || mon_h != sm_h) { printf("display: FAIL sizes differ between the four answers\n"); ok = 0; }
    if (dc_hz != (int)dm.dmDisplayFrequency) { printf("display: FAIL VREFRESH %d != EnumDisplaySettings %lu\n", dc_hz, dm.dmDisplayFrequency); ok = 0; }
    if (dc_bpp != 32 || dm.dmBitsPerPel != 32) { printf("display: FAIL not 32 bpp\n"); ok = 0; }
    if (argc > 1) {
        if (sscanf(argv[1], "%ux%u@%u", &ew, &eh, &ehz) != 3) { printf("display: FAIL bad argument '%s'\n", argv[1]); ok = 0; }
        else if (sm_w != (int)ew || sm_h != (int)eh || dc_hz != (int)ehz) {
            printf("display: FAIL expected %ux%u@%u\n", ew, eh, ehz);
            ok = 0;
        }
    }
    printf("display: done\n");
    fflush(stdout);
    return ok ? 42 : 1;
}
