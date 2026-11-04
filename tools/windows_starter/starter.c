/* Uncharted Launcher.exe: starts the launcher (launcher\uncharted_launcher.pyw) with the Python that
 * ships next to it (python\pythonw.exe), without a console window. Built by tools/package_windows.sh. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wchar.h>

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE previous, PWSTR arguments, int show)
{
    (void)instance, (void)previous, (void)show;
    static wchar_t dir[32768], python[32768], line[32768];
    DWORD length = GetModuleFileNameW(NULL, dir, 32768);
    if (length == 0 || length >= 32768)
        return 1;
    wchar_t *slash = wcsrchr(dir, L'\\');
    if (slash)
        *slash = 0;
    swprintf(python, 32768, L"%ls\\python\\pythonw.exe", dir);
    /* Its own arguments go to the launcher: "Uncharted Launcher.exe" --play u2 starts that game. */
    swprintf(line, 32768, L"\"%ls\" \"%ls\\launcher\\uncharted_launcher.pyw\" %ls", python, dir,
             arguments ? arguments : L"");

    STARTUPINFOW startup = {.cb = sizeof(startup)};
    PROCESS_INFORMATION process;
    if (!CreateProcessW(python, line, NULL, NULL, FALSE, 0, NULL, dir, &startup, &process)) {
        MessageBoxW(NULL, L"python\\pythonw.exe was not found next to this file. Extract the whole package.",
                    L"Uncharted: The Nathan Drake Collection", MB_OK | MB_ICONERROR);
        return 1;
    }
    CloseHandle(process.hThread);
    /* 28 Oct 2026 (#267): stay until the launcher closes. Steam counts a non-Steam game's play
     * time while the program it started runs; a starter that ended at once left the per-game
     * entries (--play: the launcher lives as long as the game) at zero. */
    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(process.hProcess, &code);
    CloseHandle(process.hProcess);
    return (int)code;
}
