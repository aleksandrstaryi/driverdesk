// DriverDesk — плагин ETS2/ATS: по клавише поверх игры окно: календарь, тахограф/HOS, запреты, журнал, ТО.
// Дата = игровой день профиля (game.time), меняется в полночь по часовому поясу страны/штата, где грузовик
// (ближайший город из cities.h). День 0 профиля = start_date из ini (по умолчанию понедельник 5.01.2026).
// Окно рисуется GDI+ в картинку и копируется прямо в кадр игры (перехват IDXGISwapChain::Present через vtable).

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <gdiplus.h>
#include <shlobj.h>
#include <atomic>
#include <climits>
#include <io.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>
#include <string>
#include <vector>

#include "scssdk_telemetry.h"
#include "eurotrucks2/scssdk_eut2.h"
#include "eurotrucks2/scssdk_telemetry_eut2.h"
#include "amtrucks/scssdk_ats.h"
#include "amtrucks/scssdk_telemetry_ats.h"
#include "common/scssdk_telemetry_common_channels.h"
#include "common/scssdk_telemetry_truck_common_channels.h"
#include "common/scssdk_telemetry_common_configs.h"
#include "common/scssdk_telemetry_common_gameplay_events.h"
#include <algorithm>
#include <set>

#include "cities.h"
#include "lang.h"
#include "lang_game.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")

using namespace Gdiplus;

// ---------------------------------------------------------------- настройки и журнал

static HMODULE g_module;
static std::wstring g_dir;               // папка плагина
static scs_log_t g_game_log;

struct Settings {
    // открыть / закрыть окно; F9 в играх и в TM Real Walk свободна (Ctrl+F9 — телепорт в режиме разработчика, Ctrl+F11 — QA Tool)
    int key = VK_F9, key_ctrl = 0, key_shift = 1, key_alt = 0;
    std::string key_text = "Shift+F9", prev_text, next_text;
    int prev_key = 0, next_key = 0;                               // листать месяцы клавишами (0 = нет)
    // включить / выключить тахограф (ETS2) или логбук HOS (ATS): Alt+F9 свободна в играх; состояние запоминается в DriverDesk_data.ini
    int tk_key = VK_F9, tk_ctrl = 1, tk_shift = 1, tk_alt = 0;
    std::string tk_text = "Ctrl+Shift+F9";
    // мини-окно тахографа в левом верхнем углу: F9 без модификаторов (в играх и в TM Real Walk свободна; Ctrl+F9 — телепорт, не срабатывает)
    int mini_key = VK_F9, mini_ctrl = 0, mini_shift = 0, mini_alt = 0;
    int help_key = VK_F9, help_ctrl = 0, help_shift = 1, help_alt = 1;   // краткая инструкция по тахографу: Shift+Alt+F9 (F7 занята игрой — «Услуги»; в семействе F9 свободны только комбинации с Alt)
    std::string help_text = "Shift+Alt+F9";
    std::string mini_text = "F9";
    int mini_style = 1;                                           // вид строк «вождение» и «смена» в мини-окне: 0 — a (общая цифра), 1 — b (крупный запас с подписью), 2 — v (подписи в строке)
    double fuel_price = 0;                                        // цена топлива за литр для оценки расходов (0 = по умолчанию: ETS2 1,55 €, ATS 1,00 $)
    bool tacho_on = true;
    int blinkers = 1;                                             // листать месяцы поворотниками
    int start_y = 2026, start_m = 1, start_d = 5;                 // день 0, если профиль не узнали
    int new_y = 2026, new_m = 7, new_d = 1;                       // с какой даты начинается новый профиль
    int time_shift = 0;                                           // поправка, минуты
    int hos_days = 8;                                             // ATS, логбук: всегда 70 ч за 8 дней (решение пользователя 05.11.2026)
    int tz_base = INT_MIN;                                        // пояс, в котором идёт game.time (мин от UTC; по умолчанию от игры)
    int time_zones = -1;                                          // -1 = как g_time_zones в config.cfg, 1 = вкл, 0 = выкл
    float scale = 1.0f;                                           // размер окна
    int position = 0;                                             // 0 = по центру, 1 = справа вверху
};
static Settings S;

// Журнал игры можно звать только из главного потока игры (правило SDK). Строки из потока рисования копятся
// и выводятся в ближайшем колбэке телеметрии (flush_log).
static DWORD g_main_thread = 0;
static std::mutex g_log_mx;
static std::vector<std::string> g_log_queue;

static void flush_log()
{
    std::vector<std::string> q;
    { std::lock_guard<std::mutex> l(g_log_mx); q.swap(g_log_queue); }
    if (g_game_log) for (auto &s : q) g_game_log(SCS_LOG_TYPE_message, s.c_str());
}

static void log_line(const char *fmt, ...)
{
    char buf[512];
    va_list a; va_start(a, fmt); vsnprintf(buf, sizeof buf, fmt, a); va_end(a);
    std::string s = std::string("[DriverDesk] ") + buf;
    if (GetCurrentThreadId() == g_main_thread) { flush_log(); if (g_game_log) g_game_log(SCS_LOG_TYPE_message, s.c_str()); }
    else { std::lock_guard<std::mutex> l(g_log_mx); if (g_log_queue.size() < 100) g_log_queue.push_back(s); }
}

static int parse_key(const std::string &v)
{
    if (v.size() > 2 && v[0] == '0' && (v[1] == 'x' || v[1] == 'X')) return (int)strtol(v.c_str() + 2, nullptr, 16);
    if (v.size() >= 2 && (v[0] == 'F' || v[0] == 'f') && isdigit((unsigned char)v[1])) return VK_F1 + atoi(v.c_str() + 1) - 1;
    if (v == "PageUp") return VK_PRIOR;
    if (v == "PageDown") return VK_NEXT;
    if (v == "Left") return VK_LEFT;
    if (v == "Right") return VK_RIGHT;
    if (v.size() == 7 && v.compare(0, 6, "Numpad") == 0 && isdigit((unsigned char)v[6])) return VK_NUMPAD0 + (v[6] - '0');
    if (v.size() == 1) return toupper((unsigned char)v[0]);
    return 0;
}

static const char *INI_DEFAULT =
    "; DriverDesk — рабочее место дальнобойщика поверх игры (ETS2 / ATS)\r\n"
    "; Строки после ';' — пояснения. После правки перезапустить игру.\r\n"
    "[keys]\r\n"
    "open = Shift+F9            ; открыть / закрыть календарь (Ctrl, Shift, Alt + F1..F12, буква или 0x-код);\r\n"
    "                           ; не ставить Ctrl+F11 (QA Tool игры) и Ctrl+F9 (телепорт в режиме разработчика)\r\n"
    "tachograph_key = Ctrl+Shift+F9 ; включить / выключить тахограф (ETS2) или логбук HOS (ATS); при выключении счёт и сообщения отключаются,\r\n"
    "                           ; при включении счёт начинается заново. Состояние запоминается\r\n"
    "mini_window = F9           ; маленькое окно тахографа в левом верхнем углу: сколько до конца смены и до перерыва\r\n"
    "help_window = Shift+Alt+F9 ; краткая инструкция по тахографу\r\n"
    "fuel_price = auto          ; цена топлива за литр в журнале; auto = по ценам игры в стране / штате, где ехали; число (например 1.55) — одна цена для всех\r\n"
    "tachograph = on            ; состояние при первом запуске: on / off (дальше работает кнопка)\r\n"
    "blinkers = on              ; при открытом окне: левый поворотник — прошлый месяц, правый — следующий (off = нет)\r\n"
    "prev_month = none          ; клавиша «прошлый месяц» вместо или вместе с поворотниками (например PageUp)\r\n"
    "next_month = none          ; клавиша «следующий месяц» (например PageDown)\r\n"
    "[calendar]\r\n"
    "new_profile_date = 2026-07-01 ; с какой даты начинается календарь в новом профиле. Если день недели в игре другой,\r\n"
    "                           ; берётся прошлый год, где эта дата — тот же день недели. Даты профилей — в DriverDesk_data.ini\r\n"
    "start_date = 2026-01-05    ; запасная: дата первого дня, если профиль не удалось узнать по game.log.txt\r\n"
    "time_zones = auto          ; часовые пояса: auto = как в настройках игры, on / off\r\n"
    "time_zone_base = auto      ; в каком поясе идут внутренние часы игры, минуты от UTC (auto: ATS -360, ETS2 120)\r\n"
    "time_shift = 0             ; поправка в минутах, если часы в окне расходятся с часами игры\r\n"
    "[window]\r\n"
    "scale = 1.0               ; размер окна (0.6 .. 2)\r\n"
    "position = center          ; center = по центру экрана, right = справа вверху\r\n";

static const char *PLUGIN_VERSION = "1.3";

// Проект раньше назывался SeasonCalendar: переносим файлы прежнего имени (данные и настройки), чтобы ничего не потерялось.
// Старая клавиша по умолчанию (Ctrl+F8 / Ctrl+F7) заменяется новой (Shift+F9); свои настройки остаются.
static void migrate_legacy_files()
{
    const std::wstring old_data = g_dir + L"SeasonCalendar_data.ini", new_data = g_dir + L"DriverDesk_data.ini";
    if (GetFileAttributesW(new_data.c_str()) == INVALID_FILE_ATTRIBUTES && GetFileAttributesW(old_data.c_str()) != INVALID_FILE_ATTRIBUTES)
        MoveFileW(old_data.c_str(), new_data.c_str());
    const std::wstring old_ini = g_dir + L"SeasonCalendar.ini", new_ini = g_dir + L"DriverDesk.ini";
    if (GetFileAttributesW(new_ini.c_str()) == INVALID_FILE_ATTRIBUTES && GetFileAttributesW(old_ini.c_str()) != INVALID_FILE_ATTRIBUTES) {
        std::string text;
        if (FILE *f = _wfopen(old_ini.c_str(), L"rb")) {
            char buf[4096]; size_t n;
            while ((n = fread(buf, 1, sizeof buf, f)) > 0) text.append(buf, n);
            fclose(f);
        }
        auto rep = [&](const char *a, const char *b) { size_t p = text.find(a); if (p != std::string::npos) text.replace(p, strlen(a), b); };
        rep("open = Ctrl+F8", "open = Shift+F9"); rep("open = Ctrl+F7", "open = Shift+F9");
        rep("SeasonCalendar_data.ini", "DriverDesk_data.ini");
        if (!text.empty()) {
            if (FILE *f = _wfopen(new_ini.c_str(), L"wb")) { fwrite(text.data(), 1, text.size(), f); fclose(f); DeleteFileW(old_ini.c_str()); }
        }
    }
}

static void load_settings()
{
    std::wstring path = g_dir + L"DriverDesk.ini";
    FILE *f = _wfopen(path.c_str(), L"rb");
    if (!f) {
        f = _wfopen(path.c_str(), L"wb");
        if (f) { fputs("\xEF\xBB\xBF", f); fputs(INI_DEFAULT, f); fclose(f); }
        f = _wfopen(path.c_str(), L"rb");
        if (!f) return;
    }
    char line[512];
    while (fgets(line, sizeof line, f)) {
        std::string s(line);
        if (s.compare(0, 3, "\xEF\xBB\xBF") == 0) s.erase(0, 3);
        auto sc = s.find(';'); if (sc != std::string::npos) s.erase(sc);
        auto eq = s.find('='); if (eq == std::string::npos) continue;
        auto trim = [](std::string t) { size_t a = t.find_first_not_of(" \t\r\n"), b = t.find_last_not_of(" \t\r\n");
                                        return a == std::string::npos ? std::string() : t.substr(a, b - a + 1); };
        std::string k = trim(s.substr(0, eq)), v = trim(s.substr(eq + 1));
        if (k == "open") {
            S.key_ctrl = v.find("Ctrl+") != std::string::npos;
            S.key_shift = v.find("Shift+") != std::string::npos;
            S.key_alt = v.find("Alt+") != std::string::npos;
            auto p = v.rfind('+'); int vk = parse_key(p == std::string::npos ? v : v.substr(p + 1));
            if (vk) { S.key = vk; S.key_text = v; }
        }
        else if (k == "tachograph_key" && v != "Alt+F9") {            // прежнее умолчание Alt+F9 заменено на Ctrl+Shift+F9: с Alt в играх бывает чёрный экран
            S.tk_ctrl = v.find("Ctrl+") != std::string::npos;
            S.tk_shift = v.find("Shift+") != std::string::npos;
            S.tk_alt = v.find("Alt+") != std::string::npos;
            auto p = v.rfind('+'); int vk = parse_key(p == std::string::npos ? v : v.substr(p + 1));
            if (v == "Alt+F9") { /* прежнее умолчание: с Alt в играх бывает чёрный экран — остаётся новое Ctrl+Shift+F9 */ }
            else if (vk) { S.tk_key = vk; S.tk_text = v; }
        }
        else if (k == "mini_window") {
            S.mini_ctrl = v.find("Ctrl+") != std::string::npos;
            S.mini_shift = v.find("Shift+") != std::string::npos;
            S.mini_alt = v.find("Alt+") != std::string::npos;
            auto p = v.rfind('+'); int vk = parse_key(p == std::string::npos ? v : v.substr(p + 1));
            if (vk) { S.mini_key = vk; S.mini_text = v; }
        }
        else if (k == "help_window" && v != "Ctrl+Shift+F7") {         // Ctrl+Shift+F7 (F7 — «Услуги» в игре) больше не умолчание
            S.help_ctrl = v.find("Ctrl+") != std::string::npos;
            S.help_shift = v.find("Shift+") != std::string::npos;
            S.help_alt = v.find("Alt+") != std::string::npos;
            auto p = v.rfind('+'); int vk = parse_key(p == std::string::npos ? v : v.substr(p + 1));
            if (vk) { S.help_key = vk; S.help_text = v; }
        }
        else if (k == "mini_style") S.mini_style = v == "a" ? 0 : v == "b" ? 1 : 2;
        else if (k == "fuel_price") { double p = atof(v.c_str()); if (p > 0 && p < 100) S.fuel_price = p; }
        else if (k == "tachograph") S.tacho_on = v != "off";
        else if (k == "blinkers") S.blinkers = v != "off";
        else if (k == "prev_month") { int vk = parse_key(v); if (vk) { S.prev_key = vk; S.prev_text = v; } }
        else if (k == "next_month") { int vk = parse_key(v); if (vk) { S.next_key = vk; S.next_text = v; } }
        else if (k == "start_date") sscanf(v.c_str(), "%d-%d-%d", &S.start_y, &S.start_m, &S.start_d);
        else if (k == "new_profile_date") sscanf(v.c_str(), "%d-%d-%d", &S.new_y, &S.new_m, &S.new_d);
        else if (k == "time_shift") S.time_shift = atoi(v.c_str());
        else if (k == "time_zone_base" && v != "auto") S.tz_base = atoi(v.c_str());
        else if (k == "time_zones") S.time_zones = v == "on" ? 1 : v == "off" ? 0 : -1;
        else if (k == "scale") { S.scale = (float)atof(v.c_str()); if (S.scale < 0.6f) S.scale = 0.6f; if (S.scale > 2) S.scale = 2; }
        else if (k == "position") S.position = v == "right" ? 1 : 0;
    }
    fclose(f);
}

// Документы\<игра>\<file>
static std::wstring documents_file(const char *game_folder, const wchar_t *file)
{
    PWSTR docs = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &docs))) return L"";
    std::wstring path = std::wstring(docs) + L"\\";
    CoTaskMemFree(docs);
    return path + std::wstring(game_folder, game_folder + strlen(game_folder)) + L"\\" + file;
}

// ---------------------------------------------------------------- данные игры (из телеметрии)

static std::mutex g_data_mx;
static std::string g_profile;              // текущий профиль ("" — не знаем); меняется под g_data_mx
static bool g_have_time = false, g_have_pos = false;
static bool g_blink_l = false, g_blink_r = false;
static scs_u32_t g_game_time = 0;
static double g_pos_x = 0, g_pos_z = 0;
static const City *g_cities = nullptr;
static size_t g_city_count = 0;
static bool g_ats = false;
static int g_tz_enabled = 1;

// g_time_zones из config.cfg игры: "0" — пояса выключены
static int read_game_time_zones(const std::wstring &cfg)
{
    FILE *f = _wfsopen(cfg.c_str(), L"rb", _SH_DENYNO);
    if (!f) return 1;
    char line[512]; int r = 1;
    while (fgets(line, sizeof line, f))
        if (strstr(line, "g_time_zones")) { const char *q = strchr(line, '"'); if (q) r = atoi(q + 1) != 0; }
    fclose(f);
    return r;
}

// ---------------------------------------------------------------- календарь

static long days_from_civil(int y, int m, int d)   // дни от 1970-01-01
{
    y -= m <= 2;
    const long era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (long)doe - 719468;
}

static void civil_from_days(long z, int &y, int &m, int &d)
{
    z += 719468;
    const long era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = (unsigned)(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    d = (int)(doy - (153 * mp + 2) / 5 + 1);
    m = (int)(mp < 10 ? mp + 3 : mp - 9);
    y = (int)yoe + (int)era * 400 + (m <= 2);
}

// Календарь свой: день недели берётся из игры (игровой день 0 — понедельник), а не из настоящего календаря.
// g_base — дата (дни от 1970) игрового дня 0 текущего профиля.
static long g_base = 0;
static int weekday(long z) { return (int)(((z - g_base) % 7 + 7) % 7); }   // 0 = понедельник

static int days_in_month(int y, int m)
{
    static const int dm[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    return m == 2 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0) ? 29 : dm[m - 1];
}

#include "store.h"
#include "bans.h"
#include "features.h"
#include "ui_text.h"
#include "holidays.h"
#include "cargo_ban.h"
#include "hos.h"
#include "tacho_text.h"
#include "tacho_text2.h"
#include "extra_text.h"
#include "fuel_prices.h"
#include "help_text.h"

static std::atomic<int> g_tz_now{0};       // поправка пояса места к game.time сейчас (минуты), считает окно
static std::string g_dest_city;            // город назначения рейса (код)

static const char *country_of_city(const std::string &city)
{
    for (size_t i = 0; i < g_city_count; ++i) if (city == g_cities[i].name) return g_cities[i].country;
    return nullptr;
}

// ---------------------------------------------------------------- язык (как в игре)
// Язык — из game.log.txt («Selected language: ru_ru»). Дни недели и страны — строки самой игры (lang_game.h),
// месяцы, времена года и подписи — lang.h. Языка нет в lang.h (японский, китайский и др.) — всё по-английски.

static std::string g_lang = "en_gb";       // код игры
static std::string g_glang = "en_gb";      // откуда брать строки игры
static const LangText *LT = &LANGS[0];

static void set_language(const std::string &code)
{
    g_lang = code;
    set_ui_language(code);
    LT = &LANGS[0]; g_glang = "en_gb";
    for (auto &t : LANGS)
        if (code.compare(0, strlen(t.code), t.code) == 0) { LT = &t; g_glang = code; break; }
}

static std::wstring game_str(const char *key)
{
    const GameStr *tab = g_ats ? game_str_ats : game_str_ets2;
    size_t n = g_ats ? sizeof game_str_ats / sizeof *game_str_ats : sizeof game_str_ets2 / sizeof *game_str_ets2;
    const wchar_t *en = nullptr;
    for (size_t i = 0; i < n; ++i) {
        if (strcmp(tab[i].key, key)) continue;
        if (g_glang == tab[i].lang) return tab[i].val;
        if (!strcmp(tab[i].lang, "en_gb")) en = tab[i].val;
    }
    return en ? en : std::wstring(key, key + strlen(key));
}

static const char *WD_SHORT_KEY[] = {"mon", "tue", "wed", "thu", "fri", "sat", "sun"};
static const char *WD_FULL_KEY[] = {"monday", "tuesday", "wednesday", "thursday", "friday", "saturday", "sunday"};

struct Season { int index; Color color; };
static Season season_of(int m)
{
    if (m == 12 || m <= 2) return {0, Color(255, 120, 190, 255)};
    if (m <= 5) return {1, Color(255, 120, 210, 110)};
    if (m <= 8) return {2, Color(255, 255, 200, 60)};
    return {3, Color(255, 240, 140, 50)};
}

static std::wstring place_name(const char *id) { return game_str(id); }

// строка даты по шаблону языка: {W} {d} {M} {Y}
static std::wstring format_date(int wd, int d, int m, int y)
{
    std::wstring w = game_str(WD_FULL_KEY[wd]);
    if (LT->lower_weekday && !w.empty()) { wchar_t c[2] = {w[0], 0}; CharLowerW(c); w[0] = c[0]; }
    std::wstring out, f = LT->date;
    for (size_t i = 0; i < f.size(); ++i) {
        if (f[i] == L'{' && i + 2 < f.size() && f[i + 2] == L'}') {
            switch (f[i + 1]) {
            case L'W': out += w; break;
            case L'd': out += std::to_wstring(d); break;
            case L'M': out += LT->months_date[m - 1]; break;
            case L'Y': out += std::to_wstring(y); break;
            }
            i += 2;
        }
        else out += f[i];
    }
    return out;
}
// ---------------------------------------------------------------- данные игры: колбэки телеметрии

// сообщение вверху экрана (смена сезона, тахограф, запрет); меняется под g_data_mx
static std::wstring g_notice_text;
static ARGB g_notice_color = 0xFF78BEFF;
static DWORD g_notice_until = 0;
static int g_notice_id = 0;

static void show_notice(const std::wstring &text, ARGB color)
{
    g_notice_text = text; g_notice_color = color; g_notice_until = GetTickCount() + 3500; ++g_notice_id;
}

// ---------------------------------------------------------------- нарушения режима труда и отдыха (смена / неделя / месяц)
// Каждое превышение лимита считается один раз за «эпизод» (флаг flags, пока лимит превышен). Смена сбрасывается с новым
// ежедневным отдыхом, неделя — со сменой недели, месяц — со сменой месяца игрового календаря. Учёт идёт только пока едем.
struct Viol {
    int shift = 0, week = 0, month = 0;
    long week_id = -1; int month_id = -1;
    long long shift_id = -1;            // ETS2: day_start тахографа; ATS: не используется
    unsigned flags = 0;
};
static Viol g_viol;

static void viol_add(unsigned bit)
{
    if (g_viol.flags & bit) return;
    g_viol.flags |= bit;
    ++g_viol.shift; ++g_viol.week; ++g_viol.month;
}
static void viol_clear(unsigned bit, bool cond) { if (cond) g_viol.flags &= ~bit; }

static void viol_update(scs_u32_t t, bool driving)
{
    long long local = (long long)t + g_tz_now;
    long week = (long)(local / 1440 / 7);
    int y, m, d; civil_from_days(g_base + (long)(local / 1440), y, m, d);
    int month_id = y * 12 + m;
    if (g_viol.week_id != week) { g_viol.week_id = week; g_viol.week = 0; g_viol.flags &= ~(8u | 16u | 64u); }
    if (g_viol.month_id != month_id) { g_viol.month_id = month_id; g_viol.month = 0; }
    if (!g_ats) {
        const Tacho &a = g_tacho;
        if (!a.init) return;
        if (g_viol.shift_id != a.day_start) { g_viol.shift_id = a.day_start; g_viol.shift = 0; g_viol.flags &= ~(2u | 4u); }
        if (a.weekly_viol) viol_add(32u);                                // после сокращённого еженедельного отдыха снова сокращённый
        viol_clear(1u, a.cont <= 270);
        if (!driving) return;
        int day_limit = (a.day_ext || a.ext_days < 2) ? 600 : 540;
        int win_limit = a.reduced < 3 ? 15 * 60 : 13 * 60;
        long long win_el = shift_elapsed(a, t);
        bool late_comp = false; for (auto &dd : a.debt) if (a.week >= dd.deadline_week) late_comp = true;
        if (a.cont > 270) viol_add(1u);                                  // больше 4:30 без перерыва
        if (a.day_drive > day_limit) viol_add(2u);                       // дневное вождение
        if (win_el > win_limit) viol_add(4u);                            // окно смены
        if (a.week_drive > 56 * 60) viol_add(8u);                        // неделя 56 ч
        if (a.week_drive + a.prev_week_drive > 90 * 60) viol_add(16u);   // две недели 90 ч
        if ((long long)t - a.weekly_end > 144 * 60) viol_add(64u);       // еженедельный отдых не взят вовремя
        if (late_comp) viol_add(128u);                                   // срок компенсации вышел
    } else {
        const Hos &h = g_hos;
        if (!h.init) return;
        if (!h.win_on) { g_viol.shift = 0; g_viol.flags &= ~(1u | 2u | 4u); }   // смена сброшена 10 ч отдыха
        viol_clear(1u, h.since_break <= 480);
        viol_clear(8u, hos_cycle(h) <= hos_limit());
        if (!driving || !h.win_on) return;
        if (h.since_break > 480) viol_add(1u);                           // 8 ч вождения без перерыва 30 мин
        if (h.drive > 660) viol_add(2u);                                 // 11 ч вождения
        if ((long long)t - h.win_start > 840) viol_add(4u);              // окно 14 ч
        if (hos_cycle(h) > hos_limit()) viol_add(8u);                    // цикл 70 ч / 8 дней
    }
}
static void viol_save(const std::string &profile)
{
    if (profile.empty()) return;
    char b[160];
    snprintf(b, sizeof b, "%d %d %d %ld %d %lld %u", g_viol.shift, g_viol.week, g_viol.month, g_viol.week_id, g_viol.month_id,
             g_viol.shift_id, g_viol.flags);
    store_set(sec("viol", profile), {b});
}
static void viol_load(const std::string &profile)
{
    g_viol = Viol();
    auto lines = store_get(sec("viol", profile));
    if (lines.empty()) return;
    Viol v;
    if (sscanf(lines[0].c_str(), "%d %d %d %ld %d %lld %u", &v.shift, &v.week, &v.month, &v.week_id, &v.month_id, &v.shift_id, &v.flags) == 7)
        g_viol = v;
}

static bool g_moved = false;               // грузовик ехал в эту игровую минуту
static float g_speed = 0;

static void tacho_warnings();
static void hos_warnings();
static bool profile_ready();

SCSAPI_VOID on_time(const scs_string_t, const scs_u32_t, const scs_value_t *const value, const scs_context_t)
{
    flush_log();                                   // строки из потока рисования
    std::lock_guard<std::mutex> l(g_data_mx);
    if (!value) return;
    scs_u32_t t = value->value_u32.value;
    bool changed = t != g_game_time;
    g_game_time = t; g_have_time = true;
    if (!changed || !profile_ready()) return;
    if (!S.tacho_on) {}                              // тахограф / логбук выключены кнопкой: счёт и сообщения не ведутся
    else if (!g_ats) {
        long long local = (long long)t + g_tz_now;
        tacho_step(t, g_moved, (long)(local / 1440 / 7));
        viol_update(t, g_moved);                       // до tacho_warnings: та сбрасывает weekly_viol
        tacho_warnings();
    } else {
        long long local = (long long)t + g_tz_now;
        hos_step(t, g_moved, (long)(local / 1440));
        viol_update(t, g_moved);
        hos_warnings();
    }
    g_moved = std::fabs(g_speed) > 0.28f;
    if (t % 5 == 0) { tacho_save(g_profile); hos_save(g_profile); viol_save(g_profile); days_save(g_profile);
                      if (!g_profile.empty()) store_set("stamp " + g_profile, {std::to_string((unsigned)t)}); }   // метка для слияния данных между устройствами
    store_flush(false);     // на диск — не чаще раза в 30 с
}

SCSAPI_VOID on_placement(const scs_string_t, const scs_u32_t, const scs_value_t *const value, const scs_context_t)
{
    std::lock_guard<std::mutex> l(g_data_mx);
    if (value) { g_pos_x = value->value_dplacement.position.x; g_pos_z = value->value_dplacement.position.z; g_have_pos = true; }
}

SCSAPI_VOID on_blinker(const scs_string_t, const scs_u32_t, const scs_value_t *const value, const scs_context_t context)
{
    std::lock_guard<std::mutex> l(g_data_mx);
    if (value) *static_cast<bool *>(context) = value->value_bool.value != 0;
}

SCSAPI_VOID on_speed(const scs_string_t, const scs_u32_t, const scs_value_t *const value, const scs_context_t)
{
    std::lock_guard<std::mutex> l(g_data_mx);
    if (!value) return;
    g_speed = value->value_float.value;
    if (std::fabs(g_speed) > 0.28f) g_moved = true;     // > 1 км/ч
}

static float g_fuel = -1;

// цена дизеля (€ или $ за литр) там, где сейчас грузовик: страна / штат — по ближайшему городу, значения fuel_price из игры
static const char *g_fc_country = nullptr;
static DWORD g_fc_tick = 0;
static double fuel_price_table(const char *country)
{
    if (country) {
        if (g_ats) { for (auto &f : FUEL_ATS) if (!strcmp(f.id, country)) return f.price; }
        else for (auto &f : FUEL_ETS2) if (!strcmp(f.id, country)) return f.price;
    }
    return g_ats ? FUEL_AVG_ATS : FUEL_AVG_ETS2;
}
static double fuel_price_here()
{
    DWORD n = GetTickCount();
    if ((n - g_fc_tick > 3000 || !g_fc_country) && g_have_pos && g_city_count) {
        double best = 1e30;
        for (size_t i = 0; i < g_city_count; ++i) {
            double dx = g_cities[i].x - g_pos_x, dz = g_cities[i].z - g_pos_z, dd = dx * dx + dz * dz;
            if (dd < best) { best = dd; g_fc_country = g_cities[i].country; }
        }
        g_fc_tick = n;
    }
    return fuel_price_table(g_fc_country);
}
static long local_day_now() { return (long)(((long long)g_game_time + g_tz_now) / 1440); }

SCSAPI_VOID on_fuel(const scs_string_t, const scs_u32_t, const scs_value_t *const value, const scs_context_t)
{
    std::lock_guard<std::mutex> l(g_data_mx);
    if (!value) return;
    float f = value->value_float.value;
    if (g_fuel >= 0 && f < g_fuel && g_fuel - f < 5 && profile_ready()) {   // расход (заправку и скачки не считаем)
        float used = g_fuel - f;
        g_days[local_day_now()].second += used;
        if (g_job.on) { g_job.fuel += used; g_job.fuel_cost += used * fuel_price_here(); }
    }
    g_fuel = f;
}

SCSAPI_VOID on_odometer(const scs_string_t, const scs_u32_t, const scs_value_t *const value, const scs_context_t)
{
    std::lock_guard<std::mutex> l(g_data_mx);
    if (!value) return;
    double o = value->value_float.value;
    if (g_odo >= 0 && o > g_odo && o - g_odo < 5 && profile_ready()) {
        double km = o - g_odo;
        g_days[local_day_now()].first += km;
        if (g_job.on) g_job.km += km;
    }
    g_odo = o;
}

static long today_date() { return g_base + local_day_now(); }

// износ двигателя / КПП / колёс (context — номер детали): упал — износ убрали в сервисе, пункты этой детали сначала
SCSAPI_VOID on_wear(const scs_string_t, const scs_u32_t, const scs_value_t *const value, const scs_context_t context)
{
    std::lock_guard<std::mutex> l(g_data_mx);
    if (!value) return;
    int part = (int)(intptr_t)context;
    float w = value->value_float.value;
    float &old = g_wear[part];
    if (old >= 0 && w < old - 0.002f && !g_truck_key.empty() && g_odo >= 0 && profile_ready()) {
        TruckService &s = service_of(g_truck_key, g_odo, today_date());
        service_visit(s, today_date());
        service_replaced(s, (ServicePart)part, g_odo);
        log_line("part %d repaired at %.0f km", part, g_odo);
        service_save(g_profile);
    }
    old = w;
}
static const scs_named_value_t *attr(const scs_named_value_t *a, const char *name)
{
    for (; a && a->name; ++a) if (!strcmp(a->name, name)) return a;
    return nullptr;
}
static std::string attr_str(const scs_named_value_t *a, const char *name)
{
    auto v = attr(a, name);
    return v && v->value.type == SCS_VALUE_TYPE_string && v->value.value_string.value ? v->value.value_string.value : "";
}
static long long attr_int(const scs_named_value_t *a, const char *name)
{
    auto v = attr(a, name);
    if (!v) return 0;
    switch (v->value.type) {
    case SCS_VALUE_TYPE_s64: return v->value.value_s64.value;
    case SCS_VALUE_TYPE_u64: return (long long)v->value.value_u64.value;
    case SCS_VALUE_TYPE_s32: return v->value.value_s32.value;
    case SCS_VALUE_TYPE_u32: return v->value.value_u32.value;
    case SCS_VALUE_TYPE_float: return (long long)v->value.value_float.value;
    default: return 0;
    }
}
static double attr_float(const scs_named_value_t *a, const char *name)
{
    auto v = attr(a, name);
    if (!v) return 0;
    if (v->value.type == SCS_VALUE_TYPE_float) return v->value.value_float.value;
    if (v->value.type == SCS_VALUE_TYPE_double) return v->value.value_double.value;
    return (double)attr_int(a, name);
}

SCSAPI_VOID on_config(const scs_event_t, const void *const event_info, const scs_context_t)
{
    auto *c = static_cast<const scs_telemetry_configuration_t *>(event_info);
    std::lock_guard<std::mutex> l(g_data_mx);
    if (!strcmp(c->id, SCS_TELEMETRY_CONFIG_truck)) {
        std::string key = attr_str(c->attributes, SCS_TELEMETRY_CONFIG_ATTRIBUTE_brand_id) + " " +
                          attr_str(c->attributes, SCS_TELEMETRY_CONFIG_ATTRIBUTE_name) + " " +
                          attr_str(c->attributes, SCS_TELEMETRY_CONFIG_ATTRIBUTE_license_plate);
        if (key.size() > 2 && key != g_truck_key) { g_truck_key = key; g_odo = -1; g_wear[0] = g_wear[1] = g_wear[2] = -1; }
    }
    else if (!strcmp(c->id, SCS_TELEMETRY_CONFIG_job)) {
        std::string cargo = attr_str(c->attributes, SCS_TELEMETRY_CONFIG_ATTRIBUTE_cargo);
        if (cargo.empty()) return;                    // рейса нет (кончился) — итог придёт событием
        std::string key = attr_str(c->attributes, SCS_TELEMETRY_CONFIG_ATTRIBUTE_cargo_id) + "|" +
                          attr_str(c->attributes, SCS_TELEMETRY_CONFIG_ATTRIBUTE_source_city_id) + "|" +
                          attr_str(c->attributes, SCS_TELEMETRY_CONFIG_ATTRIBUTE_destination_city_id) + "|" +
                          std::to_string(attr_int(c->attributes, SCS_TELEMETRY_CONFIG_ATTRIBUTE_income));
        g_dest_city = attr_str(c->attributes, SCS_TELEMETRY_CONFIG_ATTRIBUTE_destination_city_id);
        if (g_job.on && g_job.key == key) return;     // тот же рейс (загрузка сохранения)
        g_job = ActiveJob();
        g_job.on = true; g_job.key = key; g_job.cargo = cargo;
        g_job.from = attr_str(c->attributes, SCS_TELEMETRY_CONFIG_ATTRIBUTE_source_city);
        std::string sc = attr_str(c->attributes, SCS_TELEMETRY_CONFIG_ATTRIBUTE_source_company);
        if (!sc.empty()) g_job.from += " (" + sc + ")";
        g_job.to = attr_str(c->attributes, SCS_TELEMETRY_CONFIG_ATTRIBUTE_destination_city);
        std::string dc = attr_str(c->attributes, SCS_TELEMETRY_CONFIG_ATTRIBUTE_destination_company);
        if (!dc.empty()) g_job.to += " (" + dc + ")";
        g_job.income = attr_int(c->attributes, SCS_TELEMETRY_CONFIG_ATTRIBUTE_income);
        days_save(g_profile);
    }
}

static void finish_job(int status, long long revenue, double km_event, float damage)
{
    if (!g_job.on || !profile_ready()) return;
    Trip t;
    t.t_end_local = (long long)g_game_time + g_tz_now;
    t.status = status; t.cargo = g_job.cargo; t.from = g_job.from; t.to = g_job.to;
    t.km = g_job.km > 1 ? g_job.km : km_event; t.fuel = g_job.fuel; t.fuel_cost = g_job.fuel_cost; t.revenue = revenue;
    t.fines = g_job.fines; t.tolls = g_job.tolls; t.ferry = g_job.ferry; t.damage = damage;
    g_trips.push_back(t);
    journal_append(g_profile, t, g_base);
    g_job = ActiveJob(); g_dest_city.clear();
    days_save(g_profile);
}

SCSAPI_VOID on_gameplay(const scs_event_t, const void *const event_info, const scs_context_t)
{
    auto *e = static_cast<const scs_telemetry_gameplay_event_t *>(event_info);
    std::lock_guard<std::mutex> l(g_data_mx);
    const scs_named_value_t *a = e->attributes;
    if (!strcmp(e->id, SCS_TELEMETRY_GAMEPLAY_EVENT_job_delivered) || !strcmp(e->id, SCS_TELEMETRY_GAMEPLAY_EVENT_car_job_delivered))
        finish_job(0, attr_int(a, SCS_TELEMETRY_GAMEPLAY_EVENT_ATTRIBUTE_revenue),
                   attr_float(a, SCS_TELEMETRY_GAMEPLAY_EVENT_ATTRIBUTE_distance_km),
                   (float)attr_float(a, SCS_TELEMETRY_GAMEPLAY_EVENT_ATTRIBUTE_cargo_damage));
    else if (!strcmp(e->id, SCS_TELEMETRY_GAMEPLAY_EVENT_job_cancelled) || !strcmp(e->id, SCS_TELEMETRY_GAMEPLAY_EVENT_car_job_cancelled))
        finish_job(1, -attr_int(a, SCS_TELEMETRY_GAMEPLAY_EVENT_ATTRIBUTE_cancel_penalty), 0, 0);
    else if (!strcmp(e->id, SCS_TELEMETRY_GAMEPLAY_EVENT_player_fined)) {
        if (g_job.on) g_job.fines += attr_int(a, SCS_TELEMETRY_GAMEPLAY_EVENT_ATTRIBUTE_fine_amount);
    }
    else if (!strcmp(e->id, SCS_TELEMETRY_GAMEPLAY_EVENT_player_tollgate_paid)) {
        if (g_job.on) g_job.tolls += attr_int(a, SCS_TELEMETRY_GAMEPLAY_EVENT_ATTRIBUTE_pay_amount);
    }
    else if (!strcmp(e->id, SCS_TELEMETRY_GAMEPLAY_EVENT_player_use_ferry) || !strcmp(e->id, SCS_TELEMETRY_GAMEPLAY_EVENT_player_use_train)) {
        if (g_job.on) g_job.ferry += attr_int(a, SCS_TELEMETRY_GAMEPLAY_EVENT_ATTRIBUTE_pay_amount);
    }
}

// предупреждения тахографа (под g_data_mx, в потоке игры)
static void tacho_warnings()
{
    Tacho &a = g_tacho;
    if (!a.init || a.rest) return;
    if (a.weekly_viol) { a.weekly_viol = false; show_notice(tacho_text(UT->code)->viol, 0xFFEB5F5F); }
    {                                                                   // окно смены (13 ч при отдыхе 11 ч, 15 ч при сокращённом 9 ч)
        const TachoText *tw = tacho_text(UT->code);
        int win_limit = a.reduced < 3 ? 15 * 60 : 13 * 60;
        long long win_el = shift_elapsed(a, a.last_t);
        {                                                               // за час до конца основной смены 13 ч: «(смена 15 ч: N из 3)» — N уже использованных длинных смен
            wchar_t wb[300]; swprintf(wb, 300, w1h_text(UT->code)->shift, a.reduced);
            if (win_el >= 13 * 60 - 60 && win_el < 13 * 60 && !(a.warned & 64)) { a.warned |= 64; show_notice(wb, 0xFFFFC83C); }
            if (a.reduced < 3 && win_el >= 15 * 60 - 60 && win_el < 15 * 60 && !(a.warned & 262144)) { a.warned |= 262144; show_notice(with_minutes(tw->w_win15, 60), 0xFFFFC83C); }   // и за час до конца длинной смены (15 ч)
        }
        if (win_el >= win_limit - 15 && win_el < win_limit && !(a.warned & 16384)) { a.warned |= 16384; show_notice(tw->w_win15, 0xFFFFC83C); }   // ещё одно — за 15 минут
        if (win_el >= win_limit && !(a.warned & 128)) { a.warned |= 128; show_notice(tw->w_win, 0xFFEB5F5F); }
    }
    for (auto &d : a.debt)                                              // срок компенсации сокращённого отдыха вышел
        if (a.week >= d.deadline_week && !(a.warned & 32)) { a.warned |= 32; show_notice(tacho_text(UT->code)->late, 0xFFEB5F5F); break; }
    int day_limit = (a.day_ext || a.ext_days < 2) ? 600 : 540;
    {                                                                   // за час до еженедельного отдыха (он нужен не позже чем через 144 ч после прошлого)
        long long since = (long long)a.last_t - a.weekly_end;
        if (since >= 144 * 60 - 15 && since < 144 * 60 && !(a.warned & 32768)) { a.warned |= 32768; show_notice(n15_text(UT->code)->week_rest, 0xFFFFC83C); }   // за 15 минут
        if (since >= 144 * 60 - 60 && since < 144 * 60 && !(a.warned & 1024)) {
            a.warned |= 1024;
            const WeekWarnText *ww = weekw_text(UT->code);
            std::wstring m = a.weekly_reduced ? ww->full : ww->opt;
            int owed = 0; for (auto &d : a.debt) owed += d.owed;
            if (owed > 0) { wchar_t b[64], t[16]; swprintf(t, 16, L"%d:%02d", owed / 60, owed % 60); swprintf(b, 64, ww->comp, t); m += b; }
            show_notice(m, 0xFFFFC83C);
        }
    }
    {                                                                   // остался 1 час до 56 ч за неделю / 90 ч за две недели
        const WeekWarnText *ww = weekw_text(UT->code);
        int wl = 56 * 60 - a.week_drive, fl = 90 * 60 - (a.week_drive + a.prev_week_drive);
        if (wl <= 60 && wl > 0 && !(a.warned & 2048)) { a.warned |= 2048; show_notice(ww->lim_week, 0xFFFFC83C); }
        if (wl <= 15 && wl > 0 && !(a.warned & 65536)) { a.warned |= 65536; show_notice(n15_text(UT->code)->week_lim, 0xFFFFC83C); }   // за 15 минут
        if (fl <= 60 && fl > 0 && !(a.warned & 4096)) { a.warned |= 4096; show_notice(ww->lim_fort, 0xFFFFC83C); }
    }
    std::wstring brk30 =with_minutes(UT->w_break15, 30);
    wchar_t dr1h_buf[300]; swprintf(dr1h_buf, 300, w1h_text(UT->code)->drive, a.ext_days); std::wstring dr1h = dr1h_buf;
    struct W { int bit; bool cond; std::wstring text; ARGB color; } ws[] = {
        {1, a.cont >= 240 && a.cont < 270, brk30, 0xFFFFC83C},          // за 30 минут до конца 4:30 вождения
        {8192, a.cont >= 255 && a.cont < 270, with_minutes(UT->w_break15, 15), 0xFFFFC83C},   // и ещё раз за 15 минут
        {2, a.cont >= 270, UT->w_break, 0xFFEB5F5F},
        {131072, a.day_drive >= 540 - 60 && a.day_drive < 540, dr1h, 0xFFFFC83C},          // за час до конца 9 ч вождения: «(10-й час: N из 2)»
        {4, a.day_drive >= day_limit - 15 && a.day_drive < day_limit, UT->w_day15, 0xFFFFC83C},
        {8, a.day_drive >= day_limit, UT->w_day, 0xFFEB5F5F},
        {16, (long long)a.last_t - a.weekly_end >= 144 * 60, UT->w_week, 0xFFEB5F5F},
    };
    for (auto &w : ws)
        if (w.cond && !(a.warned & w.bit)) { a.warned |= w.bit; show_notice(w.text, w.color); }
}

// предупреждения HOS (ATS; под g_data_mx, в потоке игры)
static void hos_warnings()
{
    Hos &h = g_hos;
    if (!h.init || h.rest) return;
    const HosText *ht = hos_text(UT->code);
    int drive_left = 660 - h.drive, brk_left = 480 - h.since_break, cyc_left = hos_limit() - hos_cycle(h);
    std::wstring w6 = hos_cyc(ht->w[6]), w7 = hos_cyc(ht->w[7]);
    int win_left = h.win_on ? (int)(h.win_start + 840 - h.last_t) : 999;
    if (cyc_left > 60) h.warned &= ~(64 | 128 | 16384);
    // mins — за сколько минут предупреждение (в готовых текстах стоит «15», with_minutes подставляет нужное число)
    struct W { int bit; bool cond; const wchar_t *text; ARGB color; int mins; } ws[] = {
        {1, brk_left <= 30 && brk_left > 0, ht->w[0], 0xFFFFC83C, 30},
        {2, brk_left <= 0, ht->w[1], 0xFFEB5F5F, 15},
        {4, drive_left <= 60 && drive_left > 0, ht->w[2], 0xFFFFC83C, 60},
        {8, drive_left <= 0, ht->w[3], 0xFFEB5F5F, 15},
        {16, win_left <= 60 && win_left > 0, ht->w[4], 0xFFFFC83C, 60},
        {32, win_left <= 0, ht->w[5], 0xFFEB5F5F, 15},
        {64, cyc_left <= 60 && cyc_left > 0, nullptr, 0xFFFFC83C, 15},
        {128, cyc_left <= 0, nullptr, 0xFFEB5F5F, 15},
        // ещё по одному предупреждению — за 15 минут: перерыв, 11 ч вождения, окно 14 ч, цикл 70 ч
        {2048, brk_left <= 15 && brk_left > 0, ht->w[0], 0xFFFFC83C, 15},
        {4096, drive_left <= 15 && drive_left > 0, ht->w[2], 0xFFFFC83C, 15},
        {8192, win_left <= 15 && win_left > 0, ht->w[4], 0xFFFFC83C, 15},
        {16384, cyc_left <= 15 && cyc_left > 0, nullptr, 0xFFFFC83C, 15},
    };
    for (auto &w : ws)
        if (w.cond && !(h.warned & w.bit)) {
            h.warned |= w.bit;
            show_notice(w.text ? with_minutes(w.text, w.mins) : w.bit == 64 ? w6 : w.bit == 16384 ? std::wstring(n15_text(UT->code)->cycle) : w7, w.color);
        }
}

struct Now {
    bool valid = false;
    long day = 0;          // дни от 1970-01-01
    int y, m, d, wd, hh, mm;
    std::wstring place;
    const char *country = nullptr;   // код страны/штата, где грузовик
};

// ---------------------------------------------------------------- профили: какой загружен и его дата
// Профиль берём из game.log.txt (строка «Loading save … /profiles/<id>/save/…»). Для каждого профиля в
// DriverDesk_data.ini, разделе [profiles], хранится «<id> = <игровой день> <дата>»: какой дате соответствует этот игровой день.
// Новый профиль: текущему игровому дню даётся new_profile_date (1 июля) того года, где день недели совпадает с игрой.

struct Anchor { std::string id; long game_day; int y, m, d; };
static std::vector<Anchor> g_anchors;
static std::string g_game_folder;
static std::wstring g_log_path;
static long long g_log_pos = 0;
static scs_u32_t g_time_at_switch = 0;     // игровое время в момент смены профиля (ждём, пока придёт время нового)
static DWORD g_switch_tick = 0, g_log_tick = 0;
static bool profile_ready() { return !g_profile.empty(); }

static void load_anchors()
{
    for (auto &line : store_get("profiles")) {
        char id[128]; Anchor a;
        if (sscanf(line.c_str(), " %127[0-9A-Fa-f] = %ld %d-%d-%d", id, &a.game_day, &a.y, &a.m, &a.d) == 5) { a.id = id; g_anchors.push_back(a); }
    }
}

static void save_anchors()
{
    std::vector<std::string> lines;
    char b[256];
    for (auto &a : g_anchors) { snprintf(b, sizeof b, "%s = %ld %04d-%02d-%02d", a.id.c_str(), a.game_day, a.y, a.m, a.d); lines.push_back(b); }
    store_set("profiles", lines);
    store_flush(true);
}

// ---------------------------------------------------------------- синхронизация между устройствами
// Данные профиля (рейсы, тахограф, ТО …) дублируются в файл DriverDesk_data.ini в папке профиля игры
// (Документы\<игра>\steam_profiles\<профиль> или \profiles\<профиль>) — её Steam Cloud переносит между Steam Deck и ПК.
// При загрузке профиля данные из файла профиля и из папки плагина сливаются: рейсы и дни — объединением, остальное — берётся
// от более поздней стороны (по метке «stamp» — игровое время записи). Ничего вручную копировать не нужно.
static std::wstring g_sync_path;
static std::string g_sync_id;

static std::wstring g_sync_override;                                       // только для проверки без игры (tools\sync_test.cpp)
static std::wstring profile_sync_path(const std::string &id)
{
    if (!g_sync_override.empty()) return g_sync_override;
    for (const wchar_t *sub : {L"steam_profiles\\", L"profiles\\"}) {
        std::wstring dir = documents_file(g_game_folder.c_str(), (std::wstring(sub) + utf8w(id)).c_str());
        DWORD a = GetFileAttributesW(dir.c_str());
        if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY)) return dir + L"\\DriverDesk_data.ini";
    }
    return L"";
}

typedef std::map<std::string, std::vector<std::string>> SecMap;

static void sync_read(const std::wstring &path, SecMap &out)
{
    std::vector<std::string> lines; read_lines(path, lines);
    std::vector<std::string> *cur = nullptr;
    for (auto &s : lines) {
        if (s[0] == ';') continue;
        if (s[0] == '[' && s.back() == ']') { cur = &out[s.substr(1, s.size() - 2)]; continue; }
        if (cur) cur->push_back(s);
    }
}

static long long sync_stamp(const SecMap &m, const std::string &id)
{
    auto it = m.find("stamp " + id);
    return it == m.end() || it->second.empty() ? -1 : atoll(it->second[0].c_str());
}

static void profile_file_write()
{
    if (g_sync_path.empty() || g_sync_id.empty()) return;
    std::string suffix = " " + g_sync_id;
    std::wstring tmp = g_sync_path + L".tmp";
    FILE *f = _wfopen(tmp.c_str(), L"wb");
    if (!f) return;
    fputs("\xEF\xBB\xBF" "; DriverDesk: данные этого профиля (рейсы, тахограф, ТО). Файл синхронизируется вместе с профилем игры (Steam Cloud). Пишет сам плагин.\r\n", f);
    auto stv = g_store.find("stamp" + suffix);                              // метка времени записи (ставится вместе с сохранением данных)
    fprintf(f, "\r\n[stamp%s]\r\n%s\r\n", suffix.c_str(), stv != g_store.end() && !stv->second.empty() ? stv->second[0].c_str() : "0");
    for (auto &line : g_store["profiles"])                                    // дата профиля (какой игровой день какой дате соответствует)
        if (line.compare(0, g_sync_id.size(), g_sync_id) == 0) fprintf(f, "\r\n[anchor%s]\r\n%s\r\n", suffix.c_str(), line.c_str());
    for (auto &sec : g_store) {
        const std::string &name = sec.first;
        if (sec.second.empty() || name.size() <= suffix.size() || name.compare(name.size() - suffix.size(), suffix.size(), suffix) != 0) continue;
        if (name.compare(0, 5, "stamp") == 0 || name.compare(0, 6, "anchor") == 0) continue;
        fprintf(f, "\r\n[%s]\r\n", name.c_str());
        for (auto &line : sec.second) { fputs(line.c_str(), f); fputs("\r\n", f); }
    }
    bool ok = fflush(f) == 0;
    fclose(f);
    if (ok) MoveFileExW(tmp.c_str(), g_sync_path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
}

// при смене профиля (под g_data_mx): слить данные из папки профиля с локальными
static void profile_sync_load(const std::string &id)
{
    g_sync_id = id;
    g_sync_path = profile_sync_path(id);
    g_store_hook = profile_file_write;
    if (g_sync_path.empty()) return;
    std::lock_guard<std::recursive_mutex> lk(g_store_mx);
    SecMap P; sync_read(g_sync_path, P);
    if (P.empty()) { g_store_dirty = true; return; }                          // файла профиля ещё нет — будет создан при записи
    std::string suffix = " " + id;
    long long sp = sync_stamp(P, id);
    SecMap L; for (auto &s : g_store) if (s.first.size() > suffix.size() && s.first.compare(s.first.size() - suffix.size(), suffix.size(), suffix) == 0) L[s.first] = s.second;
    long long sl = sync_stamp(L, id);
    bool pnew = sp >= sl;                                                      // файл профиля новее (или равен) — он главный
    auto kind_of = [&](const std::string &n) { return n.substr(0, n.size() - suffix.size()); };
    std::set<std::string> names; for (auto &s : P) names.insert(s.first); for (auto &s : L) names.insert(s.first);
    for (auto &name : names) {
        std::string kind = kind_of(name);
        if (kind == "stamp" || kind == "anchor") continue;
        const std::vector<std::string> *a = P.count(name) ? &P[name] : nullptr, *b = L.count(name) ? &L[name] : nullptr;
        std::vector<std::string> res;
        if (kind == "trips") {                                                 // рейсы — объединение без повторов, по времени
            std::map<std::string, std::pair<long long, std::string>> m;
            for (auto *src : {a, b}) if (src) for (auto &line : *src) {
                auto v = split(line, ';');
                if (v.size() < 14) continue;
                std::string key = v[1] + ";" + v[3] + ";" + v[4] + ";" + v[5];
                m.emplace(key, std::make_pair(atoll(v[1].c_str()), line));
            }
            std::vector<std::pair<long long, std::string>> arr; for (auto &e : m) arr.push_back(e.second);
            std::sort(arr.begin(), arr.end(), [](auto &x, auto &y) { return x.first < y.first; });
            for (auto &e : arr) res.push_back(e.second);
        } else if (kind == "days") {                                           // дни — максимум км/литров, активный рейс — от более новой стороны
            std::map<long, std::pair<double, double>> d; std::string job;
            for (int pass = 0; pass < 2; ++pass) {
                const std::vector<std::string> *src = (pass == 0) == pnew ? b : a;      // сначала старая сторона, потом новая (её job перезапишет)
                if (!src) continue;
                for (auto &line : *src) {
                    auto v = split(line, ';');
                    if (v[0] == "job") job = line;
                    else if (v.size() >= 3) { auto &e = d[atol(v[0].c_str())]; e.first = std::max(e.first, atof(v[1].c_str())); e.second = std::max(e.second, atof(v[2].c_str())); }
                }
            }
            char buf[96];
            for (auto &e : d) { snprintf(buf, sizeof buf, "%ld;%.2f;%.2f", e.first, e.second.first, e.second.second); res.push_back(buf); }
            if (!job.empty()) res.push_back(job);
        } else {
            const std::vector<std::string> *pick = pnew ? (a ? a : b) : (b ? b : a);
            if (pick) res = *pick;
        }
        g_store[name] = res;
    }
    auto an = P.find("anchor" + suffix);                                      // дата профиля с другого устройства
    if (an != P.end() && !an->second.empty()) {
        bool have = false; for (auto &a2 : g_anchors) if (a2.id == id) have = true;
        if (!have) {
            Anchor a2; char idb[128];
            if (sscanf(an->second[0].c_str(), " %127[0-9A-Fa-f] = %ld %d-%d-%d", idb, &a2.game_day, &a2.y, &a2.m, &a2.d) == 5) {
                a2.id = idb; g_anchors.push_back(a2); g_store["profiles"].push_back(an->second[0]);
            }
        }
    }
    g_store_dirty = true;
}

// дочитывает game.log.txt и запоминает профиль из последней строки «Loading save»
static void update_profile(scs_u32_t t)
{
    DWORD now = GetTickCount();
    if (now - g_log_tick < 1000) return;
    g_log_tick = now;
    if (g_log_path.empty()) return;
    FILE *f = _wfsopen(g_log_path.c_str(), L"rb", _SH_DENYNO);
    if (!f) return;
    _fseeki64(f, 0, SEEK_END);
    long long size = _ftelli64(f);
    if (size < g_log_pos) g_log_pos = 0;                       // игра начала лог заново
    _fseeki64(f, g_log_pos, SEEK_SET);
    std::string found, lang, line;
    char buf[1024];
    while (fgets(buf, sizeof buf, f)) {
        line = buf;
        auto lp = line.find("Selected language: ");
        if (lp != std::string::npos) {
            lang = line.substr(lp + 19);
            lang.erase(lang.find_last_not_of(" \r\n") + 1);
            continue;
        }
        if (line.find("Loading save") == std::string::npos) continue;
        auto p = line.find("/profiles/");
        if (p == std::string::npos) continue;
        p += 10;
        auto e = line.find('/', p);
        if (e != std::string::npos) found = line.substr(p, e - p);
    }
    g_log_pos = size;
    fclose(f);
    if (!lang.empty() && lang != g_lang) { set_language(lang); log_line("language %s", lang.c_str()); }
    if (!found.empty() && found != g_profile) {
        std::lock_guard<std::mutex> l(g_data_mx);
        tacho_save(g_profile); hos_save(g_profile); viol_save(g_profile); days_save(g_profile); service_save(g_profile);   // данные прошлого профиля
        store_flush(true);
        g_profile = found; g_time_at_switch = t; g_switch_tick = now;
        profile_sync_load(g_profile);                                  // слияние с данными, пришедшими с другого устройства
        tacho_load(g_profile); hos_load(g_profile); viol_load(g_profile); journal_load(g_profile); service_load(g_profile);
        g_odo = -1; g_wear[0] = g_wear[1] = g_wear[2] = -1;
        log_line("profile %s: %u trips", g_profile.c_str(), (unsigned)g_trips.size());
    }
}

// дата (дни от 1970) для игрового дня 0 текущего профиля
static long base_day(long game_day, scs_u32_t t)
{
    long fallback = days_from_civil(S.start_y, S.start_m, S.start_d);
    if (g_profile.empty()) return fallback;
    for (auto &a : g_anchors)
        if (a.id == g_profile) return days_from_civil(a.y, a.m, a.d) - a.game_day;
    // новый профиль: ждём, пока телеметрия даст время этого профиля (или 20 с)
    if (t == 0 || (t == g_time_at_switch && GetTickCount() - g_switch_tick < 20000)) return fallback;
    // год: new_y или ближайший прошлый, где у даты тот же день недели, что сейчас в игре (настоящий календарь того года)
    int game_wd = (int)(game_day % 7);                         // игровой день 0 — понедельник
    int year = S.new_y;
    for (int y = S.new_y; y > S.new_y - 12; --y) {
        long z = days_from_civil(y, S.new_m, S.new_d);
        if ((int)((z % 7 + 7 + 3) % 7) == game_wd) { year = y; break; }   // 1970-01-01 — четверг
    }
    g_anchors.push_back({g_profile, game_day, year, S.new_m, S.new_d});
    save_anchors();
    log_line("new profile %s: game day %ld = %04d-%02d-%02d", g_profile.c_str(), game_day, year, S.new_m, S.new_d);
    return days_from_civil(year, S.new_m, S.new_d) - game_day;
}

static Now compute_now()
{
    Now n;
    scs_u32_t t; double x, z; bool ht, hp;
    { std::lock_guard<std::mutex> l(g_data_mx); t = g_game_time; x = g_pos_x; z = g_pos_z; ht = g_have_time; hp = g_have_pos; }
    update_profile(t);
    if (!ht) return n;
    int tz = 0;
    if (hp && g_city_count) {
        double best = 1e30; const City *bc = nullptr;
        for (size_t i = 0; i < g_city_count; ++i) {
            double dx = g_cities[i].x - x, dz = g_cities[i].z - z, dd = dx * dx + dz * dz;
            if (dd < best) { best = dd; bc = &g_cities[i]; }
        }
        if (bc) { n.place = place_name(bc->country); n.country = bc->country; if (g_tz_enabled) tz = bc->tz - S.tz_base; }
    }
    // game.time идёт в одном поясе (ATS — UTC−6: в Нью-Мексико часы игры = game.time, в Айове +1 ч);
    // часы игры = game.time + (пояс места − этот пояс)
    long long local = (long long)t + tz + S.time_shift;
    if (local < 0) local = 0;
    g_tz_now = tz + S.time_shift;
    long gd = (long)(local / 1440);
    int mins = (int)(local % 1440);
    g_base = base_day(gd, t);
    n.day = g_base + gd;
    civil_from_days(n.day, n.y, n.m, n.d);
    n.wd = weekday(n.day);
    n.hh = mins / 60; n.mm = mins % 60;
    n.valid = true;
    return n;
}

// ---------------------------------------------------------------- рисование окна (GDI+)

static ULONG_PTR g_gdip_token;

static const wchar_t *pick_font()
{
    static const wchar_t *names[] = {L"Segoe UI", L"Arial", L"Liberation Sans", L"Tahoma", L"DejaVu Sans"};
    for (auto n : names) { FontFamily ff(n); if (ff.IsAvailable()) return n; }
    return L"Arial";
}

static void round_rect(GraphicsPath &p, REAL x, REAL y, REAL w, REAL h, REAL r)
{
    p.AddArc(x, y, r * 2, r * 2, 180, 90);
    p.AddArc(x + w - r * 2, y, r * 2, r * 2, 270, 90);
    p.AddArc(x + w - r * 2, y + h - r * 2, r * 2, r * 2, 0, 90);
    p.AddArc(x, y + h - r * 2, r * 2, r * 2, 90, 90);
    p.CloseFigure();
}

// Копия данных для окна (берётся под g_data_mx, рисуется без него)
struct Snap {
    Tacho tv; bool tacho = false;
    Hos hv; bool hos = false; long long game_t = 0;
    Viol vv;                                // нарушения режима: смена / неделя / месяц
    std::vector<Trip> trips;                // рейсы показываемого месяца
    ActiveJob job;
    double month_km = 0, month_fuel = 0;
    std::set<int> trip_days;                 // числа месяца с рейсами
    bool svc = false; TruckService service; double odo = 0;
    long today = 0;
    std::string dest_country;
    long long now_local = 0;
};

static std::wstring fmt_num(double v)
{
    long long n = (long long)llround(std::fabs(v));
    std::wstring s = std::to_wstring(n), out;
    for (size_t i = 0; i < s.size(); ++i) { if (i && (s.size() - i) % 3 == 0) out += L'\u202F'; out += s[i]; }
    return (v < 0 ? L"−" : L"") + out;
}
static std::wstring fmt_hm(int minutes)
{
    if (minutes < 0) minutes = 0;
    wchar_t b[16]; swprintf(b, 16, L"%d:%02d", minutes / 60, minutes % 60); return b;
}
static std::wstring fmt1(double v) { wchar_t b[32]; swprintf(b, 32, L"%.1f", v); return b; }
static std::wstring wfmt(const wchar_t *f, const std::wstring &a) { wchar_t b[512]; swprintf(b, 512, f, a.c_str()); return b; }
static std::wstring wfmt(const wchar_t *f, int a) { wchar_t b[512]; swprintf(b, 512, f, a); return b; }
static std::wstring wfmt(const wchar_t *f, const std::wstring &a, int b2) { wchar_t b[512]; swprintf(b, 512, f, a.c_str(), b2); return b; }
static std::wstring wfmt(const wchar_t *f, const std::wstring &a, const std::wstring &b2) { wchar_t b[512]; swprintf(b, 512, f, a.c_str(), b2.c_str()); return b; }

static const wchar_t *currency() { return g_ats ? L"$" : L"€"; }
// единицы: ETS2 — км, литры, л/100 км; ATS — мили, галлоны (США), мили на галлон
static std::wstring unit_l() { if (g_ats) return UT->gal; std::wstring s = UT->l100; return s.substr(0, s.find(L'/')); }
static std::wstring unit_km() { if (g_ats) return UT->mi; std::wstring s = UT->l100; auto p = s.find(L"100 "); return p == std::wstring::npos ? L"km" : s.substr(p + 4); }
static double dist(double km) { return g_ats ? km / 1.609344 : km; }
static double vol(double l) { return g_ats ? l / 3.785411784 : l; }
static std::wstring consumption(double km, double l)
{
    if (g_ats) return fmt1(km / 1.609344 / (l / 3.785411784)) + L" " + UT->mpg;
    return fmt1(l * 100 / km) + L" " + UT->l100;
}

// расходы рейса: оценка топлива (литры × цена за литр) + штрафы + платные дороги + паромы
static double fuel_price_l() { return S.fuel_price > 0 ? S.fuel_price : (g_ats ? FUEL_AVG_ATS : FUEL_AVG_ETS2); }
// топливо: по ценам стран/штатов, где оно сгорело (fuel_cost); у старых записей и при заданном вручную fuel_price — литры × одна цена
static double trip_fuel_cost(const Trip &t) { return S.fuel_price <= 0 && t.fuel_cost >= 0 ? t.fuel_cost : t.fuel * fuel_price_l(); }
static double trip_spent(const Trip &t) { return trip_fuel_cost(t) + (double)t.fines + (double)t.tolls + (double)t.ferry; }
static std::wstring fmt_money(double v, bool sign = false)
{
    std::wstring s = fmt_num(v);
    if (sign && v > 0.5) s = L"+" + s;
    return s + L" " + currency();
}

// местное время (минуты профиля) → «пн 13.07 09:00» / «09:00» если сегодня
static std::wstring fmt_when(long long local, long today)
{
    long day = g_base + (long)(local / 1440);
    int y, m, d; civil_from_days(day, y, m, d);
    wchar_t b[64];
    int mm = (int)(local % 1440);
    if (day == today) swprintf(b, 64, L"%02d:%02d", mm / 60, mm % 60);
    else swprintf(b, 64, L"%s %02d.%02d %02d:%02d", game_str(WD_SHORT_KEY[weekday(day)]).c_str(), d, m, mm / 60, mm % 60);
    return b;
}

static std::wstring spans_text(const BanSpan *s, int n)
{
    std::wstring out;
    for (int i = 0; i < n; ++i) {
        wchar_t b[64]; swprintf(b, 64, L"%02d:%02d–%02d:%02d", s[i].from / 60, s[i].from % 60, s[i].to / 60, s[i].to % 60);
        if (!out.empty()) out += L", ";
        out += b;
        if (s[i].partial) out += std::wstring(L" (") + UT->partial + L")";
    }
    return out;
}

// причина запрета: «Успение», «канун праздника: Успение», «выходной, воскресенье», «ежедневный запрет»
static std::wstring why_text(const BanSpan &s, long z, bool with_weekday)
{
    std::wstring wd = game_str(WD_FULL_KEY[weekday(z)]);
    if (strcmp(UT->code, "en") && strcmp(UT->code, "de") && !wd.empty()) CharLowerBuffW(&wd[0], (DWORD)wd.size());   // «воскресенье» в середине фразы
    switch (s.why) {
    case W_HOL: case W_EASTER: return hol_name(s.name);
    case W_EVE: return std::wstring(UT->r_eve) + L": " + hol_name(s.name);
    case W_SUMMER: return with_weekday ? std::wstring(UT->r_summer) + L", " + wd : UT->r_summer;
    case W_WEEK: {
        bool weekend = weekday(z) >= 5;
        if (!with_weekday) return weekend ? UT->r_weekend : L"";
        return weekend ? std::wstring(UT->r_weekend) + L", " + wd : wd;
    }
    default: return UT->r_daily;
    }
}

// «Не действует для: скоропортящиеся (реф), молоко, …» — пусто, если по стране нет данных
static std::wstring exempt_text(const std::string &id)
{
    unsigned x = exempt_of(id);
    if (!x) return L"";
    if (x & X_NONE) return UT->ex_none;
    std::wstring s = std::wstring(UT->ex_prefix) + L": ";
    bool first = true;
    for (int i = 0; i < N_EXEMPT; ++i)
        if (x & (1u << i)) { if (!first) s += L", "; s += UT->ex[i]; first = false; }
    return s;
}

// старшая причина среди отрезков дня (сначала полные запреты)
static const BanSpan *main_span(const BanSpan *sp, int n)
{
    const BanSpan *best = nullptr;
    for (int i = 0; i < n; ++i)
        if (!best || (best->partial && !sp[i].partial) || (best->partial == sp[i].partial && sp[i].why > best->why)) best = &sp[i];
    return best;
}

// Окно целиком: календарь слева; ETS2 — тахограф и запреты посередине; журнал и ТО справа. Пиксели BGRA — в out.
static void draw_window(int w, int h, float k, const Now &now, int month_offset, const Snap &sn, std::vector<uint32_t> &out)
{
    Bitmap bmp(w, h, PixelFormat32bppARGB);
    {
        Graphics g(&bmp);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
        g.Clear(Color(255, 18, 22, 30));

        static const wchar_t *fname = pick_font();
        FontFamily ff(fname);
        Font fTitle(&ff, 30 * k, FontStyleBold, UnitPixel);
        Font fHead(&ff, 17 * k, FontStyleBold, UnitPixel);
        Font fDay(&ff, 18 * k, FontStyleRegular, UnitPixel);
        Font fDayB(&ff, 18 * k, FontStyleBold, UnitPixel);
        SolidBrush white(Color(255, 235, 238, 245)), grey(Color(255, 140, 148, 160)), dim(Color(255, 95, 100, 112)),
                   red(Color(255, 235, 95, 95)), dark(Color(255, 18, 22, 30)), orange(Color(255, 255, 170, 60)),
                   green(Color(255, 110, 200, 120)), yellow(Color(255, 255, 200, 60)), panel(Color(255, 24, 29, 39));
        StringFormat center; center.SetAlignment(StringAlignmentCenter); center.SetLineAlignment(StringAlignmentCenter);
        Pen border(Color(255, 60, 68, 84), 2 * k), line(Color(255, 45, 52, 66), 1 * k);
        g.DrawRectangle(&border, 1 * k, 1 * k, w - 2 * k, h - 2 * k);

        // текст в одну строку: если не влезает по ширине — шрифт меньше
        auto fit = [&](const std::wstring &s, REAL size, int style, REAL x, REAL y, REAL maxw, const Brush *b, bool centered) {
            for (; size > 8 * k; size -= 1 * k) {
                Font f(&ff, size, style, UnitPixel); RectF box;
                g.MeasureString(s.c_str(), -1, &f, PointF(0, 0), &box);
                if (box.Width <= maxw || size <= 9 * k) {
                    if (centered) { StringFormat c; c.SetAlignment(StringAlignmentCenter); g.DrawString(s.c_str(), -1, &f, RectF(x, y, maxw, size * 1.6f), &c, b); }
                    else g.DrawString(s.c_str(), -1, &f, PointF(x, y), b);
                    return;
                }
            }
        };
        // текст с переносом по ширине; возвращает y под ним
        auto wrap = [&](const std::wstring &s, REAL size, REAL x, REAL y, REAL maxw, const Brush *b) {
            Font f(&ff, size, FontStyleRegular, UnitPixel); RectF box;
            g.MeasureString(s.c_str(), -1, &f, RectF(0, 0, maxw, 1000 * k), nullptr, &box);
            g.DrawString(s.c_str(), -1, &f, RectF(x, y, maxw, box.Height + 2), nullptr, b);
            return y + box.Height;
        };
        // полоска заполнения: value из limit, красная, если больше
        auto bar = [&](REAL x, REAL y, REAL bw, double value, double limit) {
            SolidBrush back(Color(255, 40, 47, 60));
            g.FillRectangle(&back, x, y, bw, 6 * k);
            double f = limit > 0 ? value / limit : 0;
            const Brush *b = f > 1 ? (const Brush *)&red : f > 0.85 ? (const Brush *)&yellow : (const Brush *)&green;
            g.FillRectangle(b, x, y, (REAL)(bw * std::min(1.0, f)), 6 * k);
        };

        const REAL C1 = 470 * k, C2 = 865 * k, colw = 395 * k, pad = 22 * k, ppad = 18 * k;
        g.DrawLine(&line, C1, 14 * k, C1, h - 40 * k);
        g.DrawLine(&line, C2, 14 * k, C2, h - 40 * k);

        // ---------- календарь (левая колонка)
        const REAL cw0 = C1;
        if (!now.valid) {
            fit(LT->no_profile, 19 * k, FontStyleRegular, 10 * k, h / 2.f - 14 * k, w - 20 * k, &white, true);
        }
        else {
            int vy = now.y, vm = now.m + month_offset;
            while (vm < 1) { vm += 12; --vy; }
            while (vm > 12) { vm -= 12; ++vy; }
            Season se = season_of(vm);
            SolidBrush seBrush(se.color);
            const CountryBans *cb_here = now.country ? country_bans(now.country) : nullptr;   // ETS2: запреты и праздники; ATS: праздники штата

            REAL y = 18 * k;
            g.FillRectangle(&seBrush, 0.f, 0.f, (REAL)w, 6 * k);
            std::wstring sname = LT->seasons[se.index];
            RectF sbox; g.MeasureString(sname.c_str(), -1, &fTitle, PointF(0, 0), &sbox);
            StringFormat right; right.SetAlignment(StringAlignmentFar);
            g.DrawString(sname.c_str(), -1, &fTitle, RectF(0, y, cw0 - pad, 40 * k), &right, &seBrush);
            std::wstring title = std::wstring(LT->months[vm - 1]) + L" " + std::to_wstring(vy);
            fit(title, 30 * k, FontStyleBold, pad, y, cw0 - 2 * pad - sbox.Width - 10 * k, &white, false);
            y += 44 * k;

            wchar_t clock[16];
            swprintf(clock, 16, L" · %02d:%02d", now.hh, now.mm);
            std::wstring sub = std::wstring(LT->today) + L": " + format_date(now.wd, now.d, now.m, now.y) + clock;
            fit(sub, 19 * k, FontStyleRegular, pad, y, cw0 - 2 * pad, &white, false);
            y += 26 * k;
            if (!now.place.empty())
                fit(std::wstring(LT->where) + L": " + now.place, 19 * k, FontStyleRegular, pad, y, cw0 - 2 * pad, &grey, false);
            y += 30 * k;

            REAL cw = (cw0 - 2 * pad) / 7, ch = 32 * k;
            for (int i = 0; i < 7; ++i) {
                std::wstring wd = game_str(WD_SHORT_KEY[i]);
                g.DrawString(wd.c_str(), -1, &fHead, RectF(pad + i * cw, y, cw, 24 * k), &center, i >= 5 ? &red : &grey);
            }
            y += 28 * k;
            g.DrawLine(&line, pad, y - 2 * k, cw0 - pad, y - 2 * k);

            long first = days_from_civil(vy, vm, 1);
            int col = weekday(first), dim_ = days_in_month(vy, vm);
            for (int d = 1; d <= dim_; ++d) {
                RectF cell(pad + col * cw, y, cw, ch);
                long z = first + d - 1;
                bool today = z == now.day;
                std::wstring s = std::to_wstring(d);
                if (today) {
                    REAL r = std::min(cw, ch) * 0.46f;
                    g.FillEllipse(&seBrush, cell.X + cw / 2 - r, cell.Y + ch / 2 - r, r * 2, r * 2);
                    g.DrawString(s.c_str(), -1, &fDayB, cell, &center, &dark);
                }
                else {
                    bool hol = is_holiday(cb_here, z);
                    g.DrawString(s.c_str(), -1, hol ? &fDayB : &fDay, cell, &center, hol ? &orange : col >= 5 ? &red : &white);
                }
                if (cb_here && cb_here->rules) {                               // день запрета — рамка (без ежедневных ночных)
                    BanSpan sp[8];
                    int n = bans_of_day(cb_here, z, col, sp, 8, 1), full = 0;
                    bool any_partial = false;
                    for (int i = 0; i < n; ++i) { if (sp[i].partial) any_partial = true; else full += sp[i].to - sp[i].from; }
                    if (full || any_partial) {
                        Pen fr(full ? Color(255, 235, 95, 95) : Color(200, 160, 90, 90), (full >= 600 ? 2.4f : full ? 1.4f : 1.0f) * k);
                        if (!full) fr.SetDashStyle(DashStyleDash);
                        REAL ins = 3 * k;
                        g.DrawRectangle(&fr, cell.X + ins, cell.Y + ins, cw - 2 * ins, ch - 2 * ins);
                    }
                }
                if (sn.trip_days.count(d)) {                                   // день с рейсом — точка
                    REAL r = 2.6f * k;
                    g.FillEllipse(today ? &dark : &green, cell.X + cw / 2 - r, cell.Y + ch - 8 * k, r * 2, r * 2);
                }
                if (++col == 7) { col = 0; y += ch; }
            }
            if (col) y += ch;

            // ---------- праздники и запреты месяца в стране, где грузовик (ETS2)
            if (cb_here && (cb_here->rules || cb_here->hol)) {
                y += 10 * k;
                g.DrawLine(&line, pad, y - 4 * k, cw0 - pad, y - 4 * k);
                fit(std::wstring(g_ats ? hol_title() : UT->month_bans) + L" — " + game_str(now.country), 15 * k, FontStyleBold, pad, y, cw0 - 2 * pad, &white, false);
                y += 22 * k;
                struct Entry { std::wstring head, tail; std::vector<int> days; const Brush *b; };
                std::vector<Entry> list;
                std::map<std::wstring, size_t> groups;                        // повторяющиеся по дням недели
                BanSpan sp[8];
                std::wstring daily;
                for (int d = 1; d <= dim_; ++d) {
                    long z = first + d - 1;
                    int wd = weekday(z);
                    if (daily.empty()) { int n2 = bans_of_day(cb_here, z, wd, sp, 8, 2); if (n2) daily = spans_text(sp, n2); }
                    int n = bans_of_day(cb_here, z, wd, sp, 8, 1), hname = -1;
                    bool hol = is_holiday(cb_here, z, nullptr, &hname);
                    if (!n && !hol) continue;
                    const BanSpan *top = n ? main_span(sp, n) : nullptr;
                    std::wstring times = n ? spans_text(sp, n) : L"";
                    wchar_t dd[48]; swprintf(dd, 48, L"%02d.%02d %s", d, vm, game_str(WD_SHORT_KEY[wd]).c_str());
                    if (hol || (top && top->why >= W_EASTER)) {
                        std::wstring why = hol && (!top || top->why < W_HOL) ? std::wstring(hol_name(hname)) : why_text(*top, z, false);
                        list.push_back({std::wstring(dd) + L" — " + why + (times.empty() ? L"" : L": " + times), L"", {}, &orange});
                    }
                    else {
                        std::wstring why = why_text(*top, z, false);
                        std::wstring key = std::to_wstring(wd) + L"|" + times + L"|" + why;
                        auto it = groups.find(key);
                        if (it != groups.end()) { list[it->second].days.push_back(d); continue; }
                        groups[key] = list.size();
                        list.push_back({game_str(WD_SHORT_KEY[wd]), (why.empty() ? L"" : L" — " + why) + L": " + times, {d},
                                        top->partial ? &dim : &grey});
                    }
                }
                if (!daily.empty()) list.insert(list.begin(), {std::wstring(UT->every_day) + L": " + daily, L"", {}, &grey});
                if (list.empty() && (cb_here->rules || g_ats)) list.push_back({UT->none, L"", {}, &dim});
                REAL bottom = h - 46 * k - (g_ats ? 0 : 290 * k);       // ETS2: под списком — блок «Запреты движения»
                for (size_t i = 0; i < list.size(); ++i) {
                    if (y + 17 * k > bottom) { fit(L"…", 13 * k, FontStyleRegular, pad, y - 4 * k, cw0 - 2 * pad, &grey, false); break; }
                    std::wstring s = list[i].head;
                    if (!list[i].days.empty()) {
                        s += L" (";
                        for (size_t j = 0; j < list[i].days.size(); ++j) s += (j ? L", " : L"") + std::to_wstring(list[i].days[j]);
                        s += L")" + list[i].tail;
                    }
                    fit(s, 13 * k, FontStyleRegular, pad, y, cw0 - 2 * pad, list[i].b, false);
                    y += 17 * k;
                }
                if (!cb_here->rules && !g_ats) { if (y + 17 * k <= bottom) fit(UT->note_partial, 12 * k, FontStyleRegular, pad, y, cw0 - 2 * pad, &dim, false); }
            }

            // ---------- средняя колонка: ETS2 — тахограф и запреты; ATS — журнал
            auto title_at = [&](const std::wstring &s, REAL x, REAL yy, REAL ww) { fit(s, 19 * k, FontStyleBold, x, yy, ww, &white, false); };
            REAL x2 = C1 + ppad, w2 = colw - 2 * ppad, x3 = C2 + ppad, w3 = colw - 2 * ppad;

            auto draw_journal = [&](REAL x, REAL yy, REAL ww, REAL bottom) {
                title_at(std::wstring(UT->journal) + L" — " + LT->months[vm - 1], x, yy, ww);
                yy += 30 * k;
                if (sn.job.on) {
                    fit(std::wstring(UT->current) + L": " + utf8w(sn.job.cargo), 15 * k, FontStyleBold, x, yy, ww, &yellow, false);
                    yy += 20 * k;
                    fit(utf8w(sn.job.from) + L" → " + utf8w(sn.job.to), 14 * k, FontStyleRegular, x, yy, ww, &grey, false);
                    yy += 19 * k;
                    std::wstring jl = fmt_num(dist(sn.job.km)) + L" " + unit_km() + L" · " + fmt1(vol(sn.job.fuel)) + L" " + unit_l();
                    if (sn.job.km > 5 && sn.job.fuel > 0) jl += L" · " + consumption(sn.job.km, sn.job.fuel);
                    fit(jl, 14 * k, FontStyleRegular, x, yy, ww, &grey, false);
                    yy += 26 * k;
                }
                // итог месяца
                const FinText *ft = fin_text(UT->code);
                int n = 0; double km = 0, fuel = 0, profit = 0, f_cost = 0, fines = 0, roads = 0;
                for (auto &t : sn.trips) {
                    if (t.status == 0) ++n;
                    km += t.km; fuel += t.fuel; profit += (double)t.revenue;
                    f_cost += trip_fuel_cost(t); fines += (double)t.fines; roads += (double)(t.tolls + t.ferry);
                }
                double spent = f_cost + fines + roads, net = profit - spent;
                std::wstring tot = std::wstring(UT->month_total) + L": " + UT->trips + L" " + std::to_wstring(n) + L" · " +
                                   fmt_num(dist(sn.month_km)) + L" " + unit_km();
                fit(tot, 15 * k, FontStyleBold, x, yy, ww, &white, false);
                yy += 21 * k;
                fit(std::wstring(ft->profit) + L": " + fmt_money(profit, true), 16 * k, FontStyleBold, x, yy, ww, &green, false);   // прибыль за месяц — вверху
                yy += 21 * k;
                fit(std::wstring(ft->spent) + L": −" + fmt_money(spent), 15 * k, FontStyleBold, x, yy, ww, &red, false);
                yy += 19 * k;
                fit(std::wstring(L"≈ ") + ft->fuel + L" " + fmt_money(f_cost) + L" · " + ft->fines + L" " + fmt_money(fines) + L" · " +
                    ft->roads + L" " + fmt_money(roads), 12 * k, FontStyleRegular, x, yy, ww, &grey, false);
                yy += 18 * k;
                g.DrawLine(&line, x, yy, x + ww, yy);
                yy += 4 * k;
                fit(std::wstring(ft->net) + L": " + fmt_money(net, true), 17 * k, FontStyleBold, x, yy, ww, net >= 0 ? (const Brush *)&white : (const Brush *)&red, false);   // сухой остаток
                yy += 24 * k;
                std::wstring fl = std::wstring(UT->fuel) + L": " + fmt_num(vol(sn.month_fuel)) + L" " + unit_l();
                if (sn.month_km > 5 && sn.month_fuel > 0) fl += L" · " + consumption(sn.month_km, sn.month_fuel);
                if (sn.month_km - km > 1) fl += L" · " + std::wstring(UT->empty_km) + L" " + fmt_num(dist(sn.month_km - km)) + L" " + unit_km();
                fit(fl, 14 * k, FontStyleRegular, x, yy, ww, &grey, false);
                yy += 22 * k;
                yy += 4 * k;
                g.DrawLine(&line, x, yy - 4 * k, x + ww, yy - 4 * k);
                if (sn.trips.empty()) { fit(UT->no_trips, 14 * k, FontStyleRegular, x, yy, ww, &dim, false); return; }
                for (int i = (int)sn.trips.size() - 1; i >= 0 && yy + 58 * k < bottom; --i) {
                    const Trip &t = sn.trips[i];
                    int ty, tm, td; civil_from_days(g_base + (long)(t.t_end_local / 1440), ty, tm, td);
                    wchar_t dd[16]; swprintf(dd, 16, L"%02d.%02d  ", td, tm);
                    std::wstring l1 = dd + utf8w(t.from.substr(0, t.from.find(" ("))) + L" → " + utf8w(t.to.substr(0, t.to.find(" (")));
                    fit(l1, 14 * k, FontStyleBold, x, yy, ww, t.status ? &red : &white, false);
                    yy += 18 * k;
                    std::wstring l2 = utf8w(t.cargo) + L" · " + fmt_num(dist(t.km)) + L" " + unit_km();
                    if (t.km > 5 && t.fuel > 0) l2 += L" · " + consumption(t.km, t.fuel);
                    fit(l2, 13 * k, FontStyleRegular, x, yy, ww, &grey, false);
                    yy += 17 * k;
                    double sp = trip_spent(t), nt = (double)t.revenue - sp;       // по рейсу: доход − расходы = остаток
                    std::wstring l3 = (t.status ? std::wstring(UT->cancelled) + L" " : L"") + fmt_money((double)t.revenue, true) + L"  −" +
                                      fmt_money(sp) + L"  =  " + fmt_money(nt, true);
                    fit(l3, 13 * k, FontStyleBold, x, yy, ww, nt >= 0 ? (const Brush *)&green : (const Brush *)&red, false);
                    yy += 22 * k;
                }
            };

            auto draw_service = [&](REAL x, REAL yy, REAL ww) {
                title_at(UT->service, x, yy, ww);
                yy += 28 * k;
                if (!sn.svc) { wrap(UT->service_hint, 13 * k, x, yy, ww, &dim); return; }
                for (int i = 0; i < N_SERVICE; ++i) {
                    double interval = service_km(i);
                    double left = interval - (sn.odo - sn.service.last[i]);
                    const Brush *b = left < 0 ? (const Brush *)&red : left < interval * 0.2 ? (const Brush *)&yellow : (const Brush *)&grey;
                    std::wstring v = (left < 0 ? wfmt(UT->over_km, fmt_num(dist(-left))) : wfmt(UT->in_km, fmt_num(dist(left)))) +
                                     L" " + unit_km();
                    fit(UT->items[i], 14 * k, FontStyleRegular, x, yy, ww * 0.55f, &white, false);
                    fit(v, 14 * k, FontStyleRegular, x + ww * 0.56f, yy, ww * 0.44f, b, false);
                    yy += 19 * k;
                }
                auto date_line = [&](const wchar_t *name, long last, int period) {
                    int left = (int)(last + period - sn.today);
                    const Brush *b = left < 0 ? (const Brush *)&red : left < 30 ? (const Brush *)&yellow : (const Brush *)&grey;
                    fit(name, 14 * k, FontStyleRegular, x, yy, ww * 0.55f, &white, false);
                    fit(left < 0 ? std::wstring(UT->overdue) : wfmt(UT->in_days, left), 14 * k, FontStyleRegular, x + ww * 0.56f, yy, ww * 0.44f, b, false);
                    yy += 19 * k;
                };
                date_line(UT->inspect, sn.service.inspect_day, 365);
                if (!g_ats) date_line(UT->tacho_cal, sn.service.tacho_day, 730);
                yy += 4 * k;
                wrap(UT->service_hint, 12 * k, x, yy, ww, &dim);
            };

            // блок «Нарушения режима труда и отдыха»: за смену / неделю / месяц; возвращает y под блоком
            auto viol_block = [&](REAL x, REAL yy, REAL ww, float kk) -> REAL {
                const ExtraText *et = extra_text(UT->code);
                g.DrawLine(&line, x, yy, x + ww, yy);
                yy += 8 * kk;
                fit(et->v_title, 15 * kk, FontStyleBold, x, yy, ww, &white, false);
                yy += 24 * kk;
                struct { const wchar_t *name; int n; } cols[3] = {{et->v_shift, sn.vv.shift}, {et->v_week, sn.vv.week}, {et->v_month, sn.vv.month}};
                REAL cwid = ww / 3;
                for (int i = 0; i < 3; ++i) {
                    fit(cols[i].name, 12 * kk, FontStyleRegular, x + i * cwid, yy, cwid - 4 * kk, &grey, false);
                    fit(std::to_wstring(cols[i].n), 26 * kk, FontStyleBold, x + i * cwid, yy + 14 * kk, cwid - 4 * kk, cols[i].n ? (const Brush *)&red : (const Brush *)&green, false);
                }
                yy += 50 * kk;
                fit(et->v_hint, 11 * kk, FontStyleRegular, x, yy, ww, &dim, false);
                return yy + 16 * kk;
            };

            if (!g_ats) {
                // ---- тахограф (шрифт и отступы крупнее в 1,3 раза: колонка теперь только для него)
                const float k_base = k;
                {
                const float k = k_base * 1.3f;
                auto title_at = [&](const std::wstring &s, REAL x, REAL yy, REAL ww) { fit(s, 19 * k, FontStyleBold, x, yy, ww, &white, false); };
                REAL yy = 18 * k_base;
                title_at(UT->tacho, x2, yy, w2);
                yy += 30 * k;
                if (sn.tacho) {
                    const Tacho &a = sn.tv;
                    int day_limit = (a.day_ext || a.ext_days < 2) ? 600 : 540;
                    int ext_left = std::max(0, 2 - a.ext_days);
                    // полоса с запасом: основная часть (до base) зелёная, запас (до base+extra) — жёлтая
                    auto bar2 = [&](REAL x, REAL y0, REAL bw, double value, double base, double extra) {
                        double total = base + extra;
                        SolidBrush back(Color(255, 40, 47, 60)), ext_back(Color(255, 74, 62, 18));
                        g.FillRectangle(&back, x, y0, bw, 6 * k);
                        if (extra > 0) g.FillRectangle(&ext_back, (REAL)(x + bw * base / total), y0, (REAL)(bw * extra / total), 6 * k);
                        double f1 = std::min(value, base) / total;
                        g.FillRectangle(value > base + extra ? (const Brush *)&red : value / base > 0.85 && extra <= 0 ? (const Brush *)&yellow : (const Brush *)&green, x, y0, (REAL)(bw * f1), 6 * k);
                        if (value > base) g.FillRectangle(value > total ? (const Brush *)&red : (const Brush *)&yellow, (REAL)(x + bw * base / total), y0, (REAL)(bw * (std::min(value, total) - base) / total), 6 * k);
                    };
                    auto row = [&](const std::wstring &label, int v, int limit, const std::wstring &of, const std::wstring &extra, int bar_extra = 0, const std::wstring &ylw = L"") {
                        std::wstring line1 = label + L": " + fmt_hm(v) + L" " + of;
                        const Brush *bb = v > limit + bar_extra ? (const Brush *)&red : v > limit ? (const Brush *)&yellow : (const Brush *)&white;
                        Font mf(&ff, 14 * k, FontStyleRegular, UnitPixel); RectF mb;
                        g.MeasureString(line1.c_str(), -1, &mf, PointF(0, 0), &mb);
                        if (mb.Width > w2) {                              // длинная строка — в две строки, а не мельче
                            yy = wrap(line1, 14 * k, x2, yy, w2, bb) + 3 * k;
                        }
                        else {
                            fit(line1, 14 * k, FontStyleRegular, x2, yy, w2, bb, false);
                            yy += 19 * k;
                        }
                        if (bar_extra > 0) bar2(x2, yy, w2, v, limit, bar_extra); else bar(x2, yy, w2, v, limit);
                        yy += 10 * k;
                        if (!extra.empty()) { fit(extra, 13 * k, FontStyleRegular, x2, yy, w2, &grey, false); yy += 18 * k; }
                        if (!ylw.empty()) yy = wrap(ylw, 12 * k, x2, yy, w2, &yellow) + 2 * k;
                        yy += 4 * k;
                    };
                    const DrvText *dt = drv_text(UT->code);
                    // смена ещё не начиналась (после включения / обнуления / загрузки грузовик ни разу не ехал): отдыхать нечего — блок отдыха и «начать отдых до …» не показываем
                    const bool no_shift = a.rest ? !a.rest_show : !a.shift_on;
                    if (a.rest && a.rest_show) {                       // стоим на отдыхе: сколько нужно (11 ч или 9 ч; если сокращённые кончились — только 11 ч) и сколько осталось
                        const RestBig *rb = restbig_text(UT->code);
                        int left3 = std::max(0, 3 - a.reduced_orig);  // сколько сокращённых отдыхов (9 ч) ещё можно
                        wchar_t buf[400];
                        std::wstring need_line; const Brush *nb = &white;
                        if (a.rest_long) { swprintf(buf, 400, dt->rest_long, left3); need_line = buf; nb = &yellow; }   // после длинной смены засчитывается 9 ч
                        else if (left3 == 0) need_line = rb->only11;                                                      // 9 ч уже нельзя — только 11 ч
                        else { swprintf(buf, 400, dt->rest_need, left3); need_line = buf; }
                        yy = wrap(need_line, 13 * k, x2, yy, w2, nb) + 3 * k;
                        int to9 = std::max(0, 9 * 60 - a.rest), to11 = std::max(0, 11 * 60 - a.rest);
                        std::wstring left_line;
                        if (a.rest_long) { if (to9 > 0) { swprintf(buf, 400, rb->left9, fmt_hm(to9).c_str()); left_line = buf; } else left_line = rb->done; }
                        else if (left3 == 0) { if (to11 > 0) { swprintf(buf, 400, rb->left_one, fmt_hm(to11).c_str()); left_line = buf; } else left_line = rb->done; }
                        else if (to9 > 0) { swprintf(buf, 400, dt->rest_left, fmt_hm(to9).c_str(), fmt_hm(to11).c_str()); left_line = buf; }
                        else if (to11 > 0) { swprintf(buf, 400, dt->rest_left2, fmt_hm(to11).c_str()); left_line = buf; }
                        else left_line = rb->done;
                        yy = wrap(left_line, 13 * k, x2, yy, w2, &green) + 6 * k;
                    }
                    std::wstring brk = a.rest && a.rest_show ? wfmt(UT->rest_now, fmt_hm(a.rest)) : wfmt(UT->brk_in, fmt_hm(270 - a.cont));
                    row(UT->cont, a.cont, 270, L"/ 4:30", brk);
                    if (a.part15 && !a.rest)                          // перерыв 15+30: первые 15 мин уже были, нужно ещё 30
                        yy = wrap(tacho_text(UT->code)->p15, 12 * k, x2, yy - 4 * k, w2, &yellow) + 2 * k;
                    {                                                 // вождение за смену: 9 ч, жёлтым — запас +1 ч (10 ч можно дважды в неделю); исчерпан — запас больше не показывается
                        bool use_ext = a.day_drive > 540;
                        int extra_d = (a.day_ext || a.ext_days < 2) ? 60 : 0;
                        std::wstring ylw;
                        wchar_t buf[400];
                        std::wstring grey_line;
                        if (use_ext && extra_d) { swprintf(buf, 400, dt->ext_use, a.ext_days, fmt_hm(std::max(0, 600 - a.day_drive)).c_str()); ylw = buf; }
                        else if (extra_d) { swprintf(buf, 400, dt->ext_avail, fmt_hm(60).c_str(), a.ext_days); ylw = buf; }
                        else { swprintf(buf, 400, ext_done_fmt(UT->code), a.ext_days); grey_line = buf; }     // 10-й час использован дважды: «2 из 2»
                        row(UT->day, a.day_drive, 540, L"/ 9:00", grey_line, extra_d, ylw);
                    }
                    {                                                 // смена: от конца ежедневного отдыха; 13 ч, жёлтым — запас +2 ч (до 15 ч, не более 3 раз между недельными отдыхами)
                        const TachoText *tw = tacho_text(UT->code);
                        int extra_w = a.reduced < 3 ? 120 : 0;
                        int win_el = (int)shift_elapsed(a, sn.game_t);
                        std::wstring ex = win_el > 780 ? std::wstring() : std::wstring(tw->win_left) + L" " + fmt_hm(780 - win_el);
                        std::wstring ylw; wchar_t buf[400];
                        if (!extra_w) ylw = dt->sh_none;
                        else if (win_el > 780) { swprintf(buf, 400, dt->sh_use, fmt_hm(std::max(0, 900 - win_el)).c_str()); ylw = buf; }
                        else { swprintf(buf, 400, dt->sh_avail, fmt_hm(120).c_str(), 3 - a.reduced); ylw = buf; }
                        row(tw->win, win_el, 780, L"/ 13:00", ex, extra_w, ylw);
                    }
                    row(UT->week, a.week_drive, 56 * 60, UT->week_of, L"");
                    row(UT->fort, a.week_drive + a.prev_week_drive, 90 * 60, UT->fort_of, L"");
                    int need = a.reduced < 3 ? 540 : 660;
                    long long daily_by = (a.shift_on ? a.day_start : (long long)sn.game_t) + 24 * 60 - need + g_tz_now;
                    std::wstring dl = a.reduced < 3 ? wfmt(UT->daily, fmt_when(daily_by, sn.today), 3 - a.reduced)
                                                    : wfmt(UT->daily_red, fmt_when(daily_by, sn.today));
                    if (!no_shift) yy = wrap(dl, 14 * k, x2, yy, w2, daily_by < sn.now_local ? &red : &white) + 6 * k;
                    long long weekly_by = a.weekly_end + 144 * 60 + g_tz_now;
                    std::wstring wl = wfmt(UT->weekly, fmt_when(weekly_by, sn.today), a.weekly_reduced ? UT->weekly_full : UT->weekly_any);
                    yy = wrap(wl, 14 * k, x2, yy, w2, weekly_by < sn.now_local ? &red : &white) + 6 * k;
                    const TachoText *tt = tacho_text(UT->code);
                    if (a.must_full)                                  // прошлый еженедельный отдых был сокращённым — этот должен быть полным
                        yy = wrap(wfmt(tt->left, fmt_hm(45 * 60 - a.rest)), 13 * k, x2, yy, w2, &yellow) + 6 * k;
                    for (auto &d : a.debt) {                           // компенсация недобранного времени: сколько и до какого срока
                        bool late = a.week >= d.deadline_week;
                        std::wstring s = std::wstring(tt->comp) + L" " + fmt_hm(d.owed) + L" " + tt->until + L" " +
                                         fmt_when((long long)d.deadline_week * 7 * 1440, sn.today);
                        yy = wrap(s, 13 * k, x2, yy, w2, late ? &red : &orange) + 4 * k;
                    }
                    yy = viol_block(x2, yy + 4 * k_base, w2, k_base);
                }
                if (!S.tacho_on) {                                // тахограф выключен кнопкой
                    const TachoText *to = tacho_text(UT->code);
                    yy = wrap(std::wstring(to->tname) + L" " + to->off_hint + L" " + utf8w(S.tk_text), 13 * k, x2, yy, w2, &grey) + 6 * k;
                }
                }
                // ---- запреты движения — теперь в левой колонке под календарём (y — где кончился календарь и список месяца)
                {
                const REAL x2 = pad, w2 = cw0 - 2 * pad;
                REAL yy = y + 14 * k;
                g.DrawLine(&line, x2, yy - 8 * k, x2 + w2, yy - 8 * k);
                title_at(UT->bans, x2, yy, w2);
                yy += 28 * k;
                {                                                      // груз игрока: действует ли на него запрет — в любой стране (в т. ч. где запретов нет)
                    const CargoText *ct = cargo_text(UT->code);
                    std::wstring s; const Brush *cb;
                    if (now.country && !strcmp(now.country, "switzerland")) { s = std::wstring(ct->label) + L": " + ct->all_ch; cb = &orange; }
                    else if (!sn.job.on) { s = std::wstring(ct->label) + L": " + ct->none; cb = &grey; }
                    else {
                        bool free_ = cargo_ban_free(cargo_id_of_key(sn.job.key));
                        s = std::wstring(ct->label) + L": " + utf8w(sn.job.cargo) + L" — " + (free_ ? ct->free_ : ct->hit);
                        cb = free_ ? (const Brush *)&green : (const Brush *)&red;
                    }
                    fit(s, 14 * k, FontStyleBold, x2, yy, w2, cb, false);
                    yy += 22 * k;
                }
                auto country_block = [&](const char *id, const wchar_t *label, bool full) {
                    if (!id) return;
                    const CountryBans *c = country_bans(id);
                    fit(std::wstring(label) + L": " + game_str(id), 15 * k, FontStyleBold, x2, yy, w2, &white, false);
                    yy += 20 * k;
                    if (!c || !c->rules) {
                        const wchar_t *note = !c ? UT->no_bans : c->note == 1 ? UT->note_partial : c->note == 2 ? UT->note_adr
                                            : c->note == 3 ? UT->note_london : UT->no_bans;
                        fit(note, 13 * k, FontStyleRegular, x2, yy, w2, &grey, false);
                        yy += 22 * k;
                        return;
                    }
                    BanSpan sp[8];
                    for (int dd = 0; dd < (full ? 2 : 1); ++dd) {
                        long z = sn.today + dd;
                        int n = bans_of_day(c, z, weekday(z), sp, 8);
                        int hname = -1;
                        bool hol = is_holiday(c, z, nullptr, &hname);
                        std::wstring s = std::wstring(dd ? UT->tomorrow : UT->today) + L": " + (n ? spans_text(sp, n) : UT->none);
                        if (hol) s += std::wstring(L" · ") + hol_name(hname);
                        else if (const BanSpan *top = main_span(sp, n)) if (top->why == W_EVE || top->why == W_EASTER) s += L" · " + why_text(*top, z, false);
                        bool active = false;
                        int mins = now.hh * 60 + now.mm;
                        if (!dd) for (int i = 0; i < n; ++i) if (!sp[i].partial && mins >= sp[i].from && mins < sp[i].to) active = true;
                        fit(s, 14 * k, FontStyleRegular, x2, yy, w2, active ? &red : n ? &orange : &grey, false);
                        yy += 19 * k;
                        if (active) { fit(UT->active, 13 * k, FontStyleBold, x2, yy, w2, &red, false); yy += 18 * k; }
                    }
                    if (full) {                                           // ближайший запрет в 14 днях
                        for (int dd = 2; dd < 14; ++dd) {
                            long z = sn.today + dd;
                            int n = bans_of_day(c, z, weekday(z), sp, 8);
                            int full_n = 0; for (int i = 0; i < n; ++i) if (!sp[i].partial) ++full_n;
                            if (!full_n) continue;
                            int y2, m2, d2; civil_from_days(z, y2, m2, d2);
                            wchar_t b[64]; swprintf(b, 64, L"%s %02d.%02d ", game_str(WD_SHORT_KEY[weekday(z)]).c_str(), d2, m2);
                            std::wstring why = why_text(*main_span(sp, n), z, false);
                            if (dd > 1) fit(std::wstring(UT->next) + L": " + b + spans_text(sp, n) + (why.empty() ? L"" : L" (" + why + L")"),
                                            13 * k, FontStyleRegular, x2, yy, w2, &grey, false);
                            yy += 18 * k;
                            break;
                        }
                    }
                    std::wstring ex = exempt_text(id);                     // на какие грузы запрет не распространяется
                    if (!ex.empty()) yy = wrap(ex, 12 * k, x2, yy, w2, &dim) + 2 * k;
                    yy += 6 * k;
                };
                country_block(now.country, UT->here, true);
                // ---- соседние страны: по строке «Страна — сегодня: … · завтра: …»; без общих запретов — одной строкой
                REAL bottom2 = h - 46 * k;
                std::vector<std::string> nb = now.country ? neighbors_of(now.country) : std::vector<std::string>();
                bool dest_is_nb = false;
                if (!nb.empty() && yy + 40 * k < bottom2) {
                    fit(UT->neighbors, 15 * k, FontStyleBold, x2, yy, w2, &white, false);
                    yy += 20 * k;
                    auto low = [](std::wstring s) { if (!s.empty()) CharLowerBuffW(&s[0], (DWORD)s.size()); return s; };
                    std::map<std::wstring, std::wstring> quiet;              // пояснение → страны без общих запретов
                    std::vector<std::wstring> quiet_order;
                    for (auto &id : nb) {
                        if (id == sn.dest_country) dest_is_nb = true;
                        const CountryBans *c = country_bans(id.c_str());
                        const char *code = car_code(id);
                        std::wstring name = code ? utf8w(code) : game_str(id.c_str());
                        if (!c || !c->rules) {
                            std::wstring note = !c ? UT->no_bans : c->note == 2 ? UT->note_adr : c->note == 3 ? UT->note_london : UT->no_bans;
                            if (!quiet.count(note)) quiet_order.push_back(note);
                            quiet[note] += (quiet[note].empty() ? L"" : L", ") + name;
                            continue;
                        }
                        if (yy + 17 * k > bottom2) break;
                        BanSpan sp[8];
                        std::wstring s = name;
                        bool any = false, active = false;
                        int mins = now.hh * 60 + now.mm;
                        bool night = false;                                   // ежедневный общий запрет (Швейцария)
                        for (int i = 0; i < c->rules_n; ++i) if (rule_daily(c->rules[i]) && !c->rules[i].partial) night = true;
                        for (int dd = 0; dd < 2; ++dd) {
                            long z = sn.today + dd;
                            int n = bans_of_day(c, z, weekday(z), sp, 8, 1), hname = -1;
                            int m2 = 0; for (int i = 0; i < n; ++i) if (!sp[i].partial) sp[m2++] = sp[i];   // для соседей — только общие
                            n = m2;
                            bool hol = is_holiday(c, z, nullptr, &hname);
                            s += (dd ? L" · " : L":  ") + low(dd ? UT->tomorrow : UT->today) + L": " + (n ? spans_text(sp, n) : UT->none);
                            if (hol) s += std::wstring(L" (") + hol_name(hname) + L")";
                            for (int i = 0; i < n; ++i) if (!sp[i].partial) { any = true; if (!dd && mins >= sp[i].from && mins < sp[i].to) active = true; }
                        }
                        if (night) s += std::wstring(L" · ") + UT->night + L" 22–5";
                        fit(s, 14 * k, FontStyleRegular, x2, yy, w2, active ? &red : any ? &orange : &grey, false);
                        yy += 18 * k;
                    }
                    for (auto &note : quiet_order) {
                        if (yy + 17 * k > bottom2) break;
                        fit(quiet[note] + L" — " + note, 13 * k, FontStyleRegular, x2, yy, w2, &dim, false);
                        yy += 17 * k;
                    }
                    yy += 8 * k;
                }
                if (!sn.dest_country.empty() && (!now.country || sn.dest_country != now.country) && !dest_is_nb && yy + 60 * k < bottom2)
                    country_block(sn.dest_country.c_str(), UT->dest, false);
                }

                draw_journal(x3, 18 * k, w3, 470 * k);
                g.DrawLine(&line, x3, 488 * k, x3 + w3, 488 * k);
                draw_service(x3, 498 * k, w3);
            }
            else {
                // ---- ATS: логбук HOS посередине; журнал и ТО справа (как в ETS2)
                const HosText *ht = hos_text(UT->code);
                const float k_base = k;
                {
                const float k = k_base * 1.3f;
                auto title_at = [&](const std::wstring &s, REAL x, REAL yy, REAL ww) { fit(s, 19 * k, FontStyleBold, x, yy, ww, &white, false); };
                REAL yy = 18 * k_base;
                title_at(ht->title, x2, yy, w2);
                yy += 30 * k;
                if (sn.hos) {
                    const Hos &a = sn.hv;
                    auto row = [&](const std::wstring &label, int v, int limit, const std::wstring &of, const std::wstring &extra, bool warn) {
                        fit(label + L": " + fmt_hm(v) + L" " + of, 14 * k, FontStyleRegular, x2, yy, w2, v > limit ? &red : &white, false);
                        yy += 19 * k;
                        bar(x2, yy, w2, v, limit);
                        yy += 10 * k;
                        if (!extra.empty()) { fit(extra, 13 * k, warn ? FontStyleBold : FontStyleRegular, x2, yy, w2, warn ? &yellow : &grey, false); yy += 18 * k; }
                        yy += 4 * k;
                    };
                    {                                                 // стоим: смена была — сколько нужно отдыхать (перерыв 30 мин, 10 ч сбрасывают смену, 34 ч — цикл) и сколько осталось
                        const HosRestBig *hr = hosrest_text(UT->code);
                        wchar_t buf[400];
                        if (a.rest > 0 && a.win_on) {
                            if (a.rest < 30) { swprintf(buf, 400, hr->brk, fmt_hm(30 - a.rest).c_str()); yy = wrap(buf, 13 * k, x2, yy, w2, &green) + 4 * k; }
                            yy = wrap(hr->need10, 13 * k, x2, yy, w2, &white) + 3 * k;
                            swprintf(buf, 400, hr->left, fmt_hm(std::max(0, 600 - a.rest)).c_str(), fmt_hm(std::max(0, 34 * 60 - a.rest)).c_str());
                            yy = wrap(buf, 13 * k, x2, yy, w2, &green) + 8 * k;
                        }
                        else if (a.rest >= 600 && hos_cycle(a) > 0) {         // 10 ч уже отстояли: смена сброшена, цикл сбросится через 34 ч
                            if (a.rest < 34 * 60) { swprintf(buf, 400, hr->reset34, fmt_hm(34 * 60 - a.rest).c_str()); yy = wrap(buf, 13 * k, x2, yy, w2, &green) + 8 * k; }
                            else yy = wrap(hr->done, 13 * k, x2, yy, w2, &green) + 8 * k;
                        }
                    }
                    int drive_left = 660 - a.drive, brk_left = 480 - a.since_break, cyc = hos_cycle(a);
                    int win_el = a.win_on ? (int)(sn.game_t - a.win_start) : 0;
                    auto left = [&](int m) { return m < 0 ? std::wstring(UT->overdue) : wfmt(ht->left, fmt_hm(m)); };
                    row(ht->drive, a.drive, 660, ht->of11, left(drive_left), drive_left <= 60);
                    row(ht->win, win_el, 840, ht->of14, a.win_on ? left(840 - win_el) : L"", a.win_on && 840 - win_el <= 60);
                    std::wstring brk = a.rest ? wfmt(ht->rest_now, fmt_hm(a.rest)) : wfmt(ht->brk_in, fmt_hm(brk_left));
                    row(ht->brk, a.since_break, 480, ht->of8, brk, !a.rest && brk_left <= 30);
                    row(hos_cyc(ht->cycle), cyc, hos_limit(), hos_cyc(ht->of70), left(hos_limit() - cyc), hos_limit() - cyc <= 300);
                    yy = viol_block(x2, yy + 4 * k_base, w2, k_base);
                    wrap(hos_cyc(ht->hint), 12 * k, x2, yy + 4 * k, w2, &dim);
                }
                else if (!S.tacho_on) {                           // логбук выключен кнопкой
                    const TachoText *to = tacho_text(UT->code);
                    wrap(std::wstring(to->hname) + L" " + to->off_hint + L" " + utf8w(S.tk_text), 13 * k, x2, yy, w2, &grey);
                }
                }
                draw_journal(x3, 18 * k, w3, 470 * k);
                g.DrawLine(&line, x3, 488 * k, x3 + w3, 488 * k);
                draw_service(x3, 498 * k, w3);
            }
        }

        // подсказка внизу: названия клавиш — ярким цветом, пояснения — светло-серым
        auto wide = [](const std::string &s) { return std::wstring(s.begin(), s.end()); };
        struct Piece { std::wstring t; bool key; };
        std::vector<Piece> pieces;
        auto add_text = [&](const std::wstring &s) { pieces.push_back({s, false}); };
        auto add_key = [&](const std::wstring &s) { pieces.push_back({s, true}); };
        auto add_arrows = [&](const std::wstring &s) {                    // стрелки ← и → в тексте про поворотники — тоже клавиши
            std::wstring cur;
            for (wchar_t ch : s) {
                if (ch == L'←' || ch == L'→') { if (!cur.empty()) add_text(cur); cur.clear(); add_key(std::wstring(1, ch)); }
                else cur += ch;
            }
            if (!cur.empty()) add_text(cur);
        };
        if (S.blinkers) add_arrows(LT->blinkers);
        else if (!S.prev_text.empty()) { add_key(wide(S.prev_text)); add_text(L" / "); add_key(wide(S.next_text)); add_text(std::wstring(L" — ") + LT->other_month); }
        if (!pieces.empty()) add_text(L"      ");
        add_key(wide(S.key_text)); add_text(std::wstring(L" — ") + LT->close + L"      ");
        add_key(wide(S.mini_text)); add_text(std::wstring(L" — ") + mini_hint(UT->code) + L"      ");
        add_key(wide(S.tk_text)); add_text(std::wstring(L" — ") + tacho_text(UT->code)->toggle + (g_ats ? L" (HOS)" : L"") + L"      ");
        add_key(wide(S.help_text)); add_text(std::wstring(L" — ") + help_hint(UT->code));
        SolidBrush keyBrush(Color(255, 255, 214, 40)), txtBrush(Color(255, 160, 168, 182));
        StringFormat sfm(StringFormat::GenericTypographic());
        sfm.SetFormatFlags(sfm.GetFormatFlags() | StringFormatFlagsMeasureTrailingSpaces | StringFormatFlagsNoWrap);
        REAL fsz = 14 * k, total = 0;
        for (; fsz > 8 * k; fsz -= 1 * k) {                                // самый крупный шрифт, при котором вся подсказка влезает
            Font fr(&ff, fsz, FontStyleRegular, UnitPixel), fb(&ff, fsz, FontStyleBold, UnitPixel);
            total = 0;
            for (auto &p : pieces) { RectF bx; g.MeasureString(p.t.c_str(), -1, p.key ? &fb : &fr, PointF(0, 0), &sfm, &bx); total += bx.Width; }
            if (total <= w - 20 * k) break;
        }
        {
            Font fr(&ff, fsz, FontStyleRegular, UnitPixel), fb(&ff, fsz, FontStyleBold, UnitPixel);
            REAL px = (w - total) / 2, py = h - 30 * k;
            for (auto &p : pieces) {
                RectF bx; g.MeasureString(p.t.c_str(), -1, p.key ? &fb : &fr, PointF(0, 0), &sfm, &bx);
                g.DrawString(p.t.c_str(), -1, p.key ? &fb : &fr, PointF(px, py), &sfm, p.key ? &keyBrush : &txtBrush);
                px += bx.Width;
            }
        }
    }
    out.resize((size_t)w * h);
    BitmapData bd; Rect r(0, 0, w, h);
    if (bmp.LockBits(&r, ImageLockModeRead, PixelFormat32bppARGB, &bd) == Ok) {
        for (int yy = 0; yy < h; ++yy)
            memcpy(&out[(size_t)yy * w], (BYTE *)bd.Scan0 + (size_t)yy * bd.Stride, (size_t)w * 4);
        bmp.UnlockBits(&bd);
    }
    for (auto &p : out) p |= 0xFF000000u;
}

// данные для окна: месяц vy/vm
static Snap take_snapshot(const Now &now, int month_offset)
{
    Snap s;
    int vy = now.y, vm = now.m + month_offset;
    while (vm < 1) { vm += 12; --vy; }
    while (vm > 12) { vm -= 12; ++vy; }
    long first = days_from_civil(vy, vm, 1), last = first + days_in_month(vy, vm) - 1;
    std::lock_guard<std::mutex> l(g_data_mx);
    s.tacho = S.tacho_on && g_tacho.init; if (s.tacho) s.tv = tacho_view(g_game_time);
    s.hos = S.tacho_on && g_ats && g_hos.init; if (s.hos) s.hv = g_hos;
    s.game_t = g_game_time;
    s.vv = g_viol;
    for (auto &t : g_trips) {
        long day = g_base + (long)(t.t_end_local / 1440);
        if (day >= first && day <= last) { s.trips.push_back(t); s.trip_days.insert((int)(day - first + 1)); }
    }
    for (auto &d : g_days) {
        long day = g_base + d.first;
        if (day >= first && day <= last) { s.month_km += d.second.first; s.month_fuel += d.second.second; }
    }
    s.job = g_job;
    s.today = now.day;
    s.now_local = (long long)g_game_time + g_tz_now;
    if (!g_truck_key.empty() && g_odo >= 0) { s.svc = true; s.service = service_of(g_truck_key, g_odo, now.day); s.odo = g_odo; }
    if (const char *c = country_of_city(g_dest_city)) s.dest_country = c;
    return s;
}
// ---------------------------------------------------------------- перехват Present

typedef HRESULT(STDMETHODCALLTYPE *PresentFn)(IDXGISwapChain *, UINT, UINT);
typedef HRESULT(STDMETHODCALLTYPE *Present1Fn)(IDXGISwapChain1 *, UINT, UINT, const DXGI_PRESENT_PARAMETERS *);
static PresentFn g_orig_present;
static Present1Fn g_orig_present1;
static void **g_vtbl;
static void **g_vtbl1;
static std::atomic<int> g_in_hook{0};
static std::atomic<bool> g_active{false};

static bool g_open = false;
static int g_month_offset = 0;
static bool g_key_down = false, g_prev_down = false, g_next_down = false;
static ID3D11Texture2D *g_tex;
static int g_tex_w, g_tex_h;
static DXGI_FORMAT g_tex_fmt;
static std::wstring g_last_drawn;
static bool g_logged_fmt;

static bool down(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }

// ---------------------------------------------------------------- мини-окно тахографа (левый верхний угол)
static bool g_mini = false;
static bool g_help = false;                                        // окно «краткая инструкция по тахографу»
static ID3D11Texture2D *g_htex;
static int g_htex_w, g_htex_h;
static DXGI_FORMAT g_htex_fmt;
static std::wstring g_help_drawn;
static ID3D11Texture2D *g_mtex;
static int g_mtex_w, g_mtex_h;
static DXGI_FORMAT g_mtex_fmt;
static std::wstring g_mini_drawn;

struct Mini {
    bool on = false, has = false, ats = false, resting = false;
    int drive_left = 0, drive_extra = 0, shift_extra = 0;       // вождение за смену (остаток основного времени) и жёлтые запасы (+1 ч вождения, +2 ч смены)
    bool rest_show = false;                                          // показывать счёт отдыха в подписи: смена уже была (после включения / обнуления, пока грузовик не ехал, не показываем)
    int rest_need = 0; bool no_reduced = false;                      // минут нужного перерыва (45 или 30 после уже отстоянных 15) и «сокращённых отдыхов больше нет»
    int rest_kind = 0, rest_left = 0, rest_extra = 0;                // стоим: 1 перерыв 45 мин, 2 ежедневный отдых, 3 недельный (ETS2); 4 перерыв 30 мин, 5 отдых 10 ч, 6 отдых 34 ч (ATS)
    int drive_el = 0, shift_el = 0;                              // сколько уже отъездили за смену и сколько идёт смена (для счёта вверх на запасе)
    bool rest_long = false;                                      // отдых после смены дольше 13 ч: засчитывается 9 ч
    int rest = 0, shift_left = 0, brk_left = 0, week_left = 0;    // week_left: ETS2 — до еженедельного отдыха (144 ч после прошлого); ATS — до конца цикла 70 ч
};

static Mini mini_snapshot()
{
    Mini m;
    std::lock_guard<std::mutex> l(g_data_mx);
    m.on = S.tacho_on; m.ats = g_ats;
    if (!m.on) return m;
    if (!g_ats) {
        if (!g_tacho.init) return m;
        Tacho a = tacho_view(g_game_time);
        long long el = shift_elapsed(a, (long long)g_game_time);
        // смена: основное окно 13 ч, жёлтый запас +2 ч (до 15 ч), пока есть длинные смены; сверх разрешённого — отрицательное (красное) время
        m.drive_el = a.day_drive; m.shift_el = (int)el;
        int lim_w = a.reduced < 3 ? 15 * 60 : 13 * 60;
        if (el > lim_w) { m.shift_left = (int)(lim_w - el); m.shift_extra = 0; }
        else { m.shift_left = std::max(0, (int)(13 * 60 - el)); m.shift_extra = a.reduced < 3 ? std::min(120, (int)(15 * 60 - std::max<long long>(el, 13 * 60))) : 0; }
        // вождение за смену: основное 9 ч, жёлтый запас +1 ч (10-й час), пока он доступен; сверх разрешённого — красное
        bool ext_ok = a.day_ext || a.ext_days < 2;
        int lim_d = ext_ok ? 600 : 540;
        if (a.day_drive > lim_d) { m.drive_left = lim_d - a.day_drive; m.drive_extra = 0; }
        else { m.drive_left = std::max(0, 540 - a.day_drive); m.drive_extra = ext_ok ? std::min(60, 600 - std::max(a.day_drive, 540)) : 0; }
        m.brk_left = 270 - a.cont; m.rest = a.rest; m.rest_long = a.rest_long;
        m.rest_show = a.rest_show;                          // смена уже была — показываем счёт отдыха (после включения / обнуления, пока грузовик не ехал, не показываем)
        if (a.rest > 0 && a.rest_show) {                    // стоим после смены: что нужно отстоять
            m.rest_need = g_tacho.part15 ? 30 : 45; m.no_reduced = g_tacho.reduced >= 3;
            if (a.rest < m.rest_need) { m.rest_kind = 1; m.rest_left = m.rest_need - a.rest; }
            else if (a.rest < 24 * 60) {
                int target = a.rest_long ? 9 * 60 : 11 * 60;
                m.rest_kind = 2; m.rest_left = std::max(0, target - a.rest);
                m.rest_extra = a.rest_long ? 0 : std::max(0, 9 * 60 - a.rest);
            }
            else { m.rest_kind = 3; m.rest_left = std::max(0, 45 * 60 - a.rest); }
        }
        m.week_left = a.shift_on ? (int)(a.weekly_end + 144 * 60 - (long long)g_game_time) : 144 * 60;
    } else {
        if (!g_hos.init) return m;
        const Hos &h = g_hos;
        long long el = h.win_on ? (long long)g_game_time - h.win_start : 0;
        m.shift_left = (int)(840 - el); m.brk_left = 480 - h.since_break; m.rest = h.rest;
        m.drive_left = 660 - h.drive;
        m.rest_show = h.win_on && h.rest > 0;               // ATS: счёт отдыха, пока смена идёт (после 10 ч отдыха смена сбрасывается)
        if (h.rest > 0) {
            if (h.rest < 30) { m.rest_kind = 4; m.rest_need = 30; m.rest_left = 30 - h.rest; }
            else if (h.rest < 10 * 60) { m.rest_kind = 5; m.rest_left = 10 * 60 - h.rest; }
            else { m.rest_kind = 6; m.rest_left = std::max(0, 34 * 60 - h.rest); }
        }        m.week_left = hos_limit() - hos_cycle(h);
    }
    m.resting = m.rest > 0; m.has = true;
    return m;
}

static std::wstring fmt_hm_signed(int v) { return v < 0 ? L"-" + fmt_hm(-v) : fmt_hm(v); }

// маленькое окно «как тахограф»: тёмная панель, янтарные цифры; красные, когда время вышло, оранжевые, когда осталось мало
static void draw_mini(int w, int h, float k, const Mini &m, std::vector<uint32_t> &out)
{
    Bitmap bmp(w, h, PixelFormat32bppARGB);
    {
        Graphics g(&bmp);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
        g.Clear(Color(255, 12, 14, 18));
        static const wchar_t *fname = pick_font();
        static const wchar_t *dname = [] { FontFamily f(L"Consolas"); return f.IsAvailable() ? L"Consolas" : pick_font(); }();
        FontFamily ff(fname), df(dname);
        const ExtraText *et = extra_text(UT->code);
        SolidBrush grey(Color(255, 150, 158, 170)), white(Color(255, 225, 228, 235));
        Pen frame(Color(255, 96, 104, 118), 2 * k), sep(Color(255, 40, 46, 58), 1 * k);
        g.DrawRectangle(&frame, 1 * k, 1 * k, w - 2 * k, h - 2 * k);
        SolidBrush head(Color(255, 28, 32, 40));
        g.FillRectangle(&head, 2 * k, 2 * k, w - 4 * k, 26 * k);
        Font ft(&ff, 13 * k, FontStyleBold, UnitPixel);
        g.DrawString(m.ats ? et->m_title_us : et->m_title_eu, -1, &ft, PointF(10 * k, 6 * k), &white);
        StringFormat far_; far_.SetAlignment(StringAlignmentFar);
        StringFormat mid; mid.SetLineAlignment(StringAlignmentCenter);
        if (!m.on || !m.has) {
            Font fs(&ff, 16 * k, FontStyleBold, UnitPixel);
            StringFormat c; c.SetAlignment(StringAlignmentCenter); c.SetLineAlignment(StringAlignmentCenter);
            SolidBrush dimb(Color(255, 120, 126, 138));
            g.DrawString(!m.on ? et->m_off : L"--:--", -1, &fs, RectF(0, 28 * k, (REAL)w, h - 28 * k), &c, &dimb);
        } else {
            const DrvText *dt = drv_text(UT->code);
            const wchar_t *st = m.rest_show ? et->m_rest : et->m_drive;
            std::wstring status = m.rest_show ? std::wstring(st) + L" " + fmt_hm(m.rest) : m.resting ? std::wstring() : std::wstring(st);   // нужный отдых уже отстоян или смена не начиналась — счёт отдыха не показываем
            if (m.rest_show) {                                                  // сколько нужно отстоять: сначала перерыв (45 мин, или 30, если уже стояли 15), потом 11 ч (9 ч) и т. д.
                std::wstring hh = dt->h;
                switch (m.rest_kind) {
                case 1: case 4: status += L" / " + fmt_hm(m.rest_need); break;
                case 2: status += m.rest_long ? L" / 9 " + hh : m.no_reduced ? L" / 11 " + hh : L" / 11 (9) " + hh; break;
                case 3: status += L" / 45 " + hh; break;
                case 5: status += L" / 10 " + hh; break;
                case 6: status += L" / 34 " + hh; break;
                }
            }
            Font fsm(&ff, 12 * k, FontStyleRegular, UnitPixel);
            g.DrawString(status.c_str(), -1, &fsm, RectF(0, 7 * k, w - 28 * k, 20 * k), &far_, &grey);
            SolidBrush led(m.rest_show ? Color(255, 255, 190, 60) : m.resting ? Color(255, 120, 126, 138) : Color(255, 90, 220, 110));
            g.FillEllipse(&led, w - 20 * k, 9 * k, 10 * k, 10 * k);
            // три варианта вида строк «вождение» и «смена» (mini_style в ini: a / b / v)
            const MiniVar *mv = minivar_text(UT->code);
            struct Row { const wchar_t *label; int v; int warn; int extra; int kind; int el; } rows[4] = {
                {et->m_break, m.brk_left, 30, 0, 0, 0}, {S.mini_style == 2 ? mv->lbl_drive_v : dt->drive, m.drive_left, 0, m.drive_extra, 1, m.drive_el},
                {S.mini_style == 2 ? mv->lbl_shift_v : et->m_shift, m.shift_left, 0, m.shift_extra, 2, m.shift_el}, {et->m_week, m.week_left, 60, 0, 0, 0}};
            REAL y0 = 30 * k, rh = (h - 32 * k) / 4;
            Font fl(&ff, 13 * k, FontStyleRegular, UnitPixel), fv(&df, 30 * k, FontStyleBold, UnitPixel), fx(&df, S.mini_style == 1 ? 22 * k : 17 * k, FontStyleBold, UnitPixel);
            Font fc(&ff, 10 * k, FontStyleRegular, UnitPixel);
            Color cGreen(255, 110, 215, 120), cYellow(255, 255, 214, 40), cRed(255, 235, 85, 85), cOrange(255, 255, 150, 60);
            for (int i = 0; i < 4; ++i) {
                REAL y = y0 + i * rh;
                if (i) g.DrawLine(&sep, 8 * k, y, w - 8 * k, y);
                Row &r = rows[i];
                int shown = r.v;                                         // стиль «a»: одна общая цифра (основное + запас)
                if (S.mini_style == 0 && r.kind && r.kind < 3 && r.v >= 0) shown = r.v + r.extra;
                // зелёный — всё в порядке; жёлтый — уже идёт запас (10-й час / +2 ч смены); красный — превышение; оранжевый — мало времени до перерыва / недельного отдыха
                Color col = r.kind == 3 ? cGreen : r.v < 0 ? cRed : r.kind ? (r.v == 0 && r.extra > 0 ? cYellow : r.v == 0 ? cRed : cGreen)
                                                    : (r.v <= 0 ? cRed : r.v <= r.warn ? cOrange : cGreen);
                SolidBrush vb(col);
                StringFormat lf; lf.SetLineAlignment(StringAlignmentCenter);
                REAL lw = S.mini_style == 2 ? 200 * k : 135 * k;
                g.DrawString(r.label, -1, &fl, RectF(10 * k, y, lw, rh), &lf, &grey);
                StringFormat vf; vf.SetAlignment(StringAlignmentFar); vf.SetLineAlignment(StringAlignmentCenter);
                g.DrawString(fmt_hm_signed(shown).c_str(), -1, &fv, RectF(215 * k, y, w - 225 * k, rh), &vf, &vb);
                if (r.kind && r.kind < 3 && r.extra > 0) {
                    SolidBrush yb(cYellow);
                    if (S.mini_style == 0) {                              // «a»: под подписью мелко жёлтым «в т.ч. 10-й час 1:00»
                        StringFormat bf; bf.SetLineAlignment(StringAlignmentFar);
                        g.DrawString(((r.kind == 1 ? mv->incl_drive : mv->incl_shift) + std::wstring(L" ") + fmt_hm(r.extra)).c_str(), -1, &fc,
                                     RectF(10 * k, y, 200 * k, rh - 3 * k), &bf, &yb);
                    }
                    else {                                                // «б», «в»: жёлтое «+1:00» левее цифры; в «б» — крупнее, с подписью «10-й час» / «до 15 ч»
                        g.DrawString((L"+" + fmt_hm(r.extra)).c_str(), -1, &fx, RectF(135 * k, S.mini_style == 1 ? y - 5 * k : y, 83 * k, rh), &vf, &yb);
                        if (S.mini_style == 1) g.DrawString(r.kind == 1 ? mv->cap_drive : mv->cap_shift, -1, &fc, RectF(135 * k, y + rh * 0.58f, 83 * k, rh * 0.4f), &vf, &yb);
                    }
                }
            }
        }
    }
    out.resize((size_t)w * h);
    BitmapData bd; Rect r(0, 0, w, h);
    if (bmp.LockBits(&r, ImageLockModeRead, PixelFormat32bppARGB, &bd) == Ok) {
        for (int yy = 0; yy < h; ++yy) memcpy(&out[(size_t)yy * w], (BYTE *)bd.Scan0 + (size_t)yy * bd.Stride, (size_t)w * 4);
        bmp.UnlockBits(&bd);
    }
    for (auto &p : out) p |= 0xFF000000u;
}

// ---------------------------------------------------------------- краткая инструкция по тахографу (Shift+Alt+F9)
static void draw_help(int w, int h, float k, std::vector<uint32_t> &out)
{
    Bitmap bmp(w, h, PixelFormat32bppARGB);
    {
        Graphics g(&bmp);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
        g.Clear(Color(255, 18, 22, 30));
        static const wchar_t *fname = pick_font();
        FontFamily ff(fname);
        SolidBrush white(Color(255, 235, 238, 245)), grey(Color(255, 176, 184, 198)), head(Color(255, 255, 214, 40));
        Pen border(Color(255, 60, 68, 84), 2 * k), line(Color(255, 45, 52, 66), 1 * k);
        g.DrawRectangle(&border, 1 * k, 1 * k, w - 2 * k, h - 2 * k);
        const HelpDoc *d = help_doc(UT->code, g_ats);
        Font ft(&ff, 28 * k, FontStyleBold, UnitPixel);
        g.DrawString(d->title, -1, &ft, PointF(30 * k, 18 * k), &white);
        g.DrawLine(&line, 30 * k, 62 * k, w - 30 * k, 62 * k);
        // последний раздел («Запомните») — отдельным блоком внизу по центру, на всю ширину
        const int nsec = d->n - 1;
        const HelpSec &note = d->secs[d->n - 1];
        Font nh(&ff, 17 * k, FontStyleBold, UnitPixel), nb(&ff, 15 * k, FontStyleRegular, UnitPixel);
        const REAL note_w = w - 200 * k;
        RectF nbh, nbb;
        g.MeasureString(note.head, -1, &nh, RectF(0, 0, note_w, 1000 * k), nullptr, &nbh);
        g.MeasureString(note.body, -1, &nb, RectF(0, 0, note_w, 1000 * k), nullptr, &nbb);
        const REAL note_h = nbh.Height + nbb.Height + 26 * k;
        const REAL top = 74 * k, bottom = h - 52 * k - note_h - 10 * k, gap = 36 * k;
        const REAL colw = (w - 60 * k - gap) / 2;
        // самый крупный шрифт, при котором текст помещается в две колонки
        for (REAL sz = 16 * k; sz >= 10 * k; sz -= 0.5f * k) {
            Font fh(&ff, sz * 1.08f, FontStyleBold, UnitPixel), fb(&ff, sz, FontStyleRegular, UnitPixel);
            REAL y = top; int col = 0; bool fits = true;
            std::vector<REAL> ys; std::vector<int> cols;
            for (int i = 0; i < nsec; ++i) {
                RectF bh, bb;
                g.MeasureString(d->secs[i].head, -1, &fh, RectF(0, 0, colw, 1000 * k), nullptr, &bh);
                g.MeasureString(d->secs[i].body, -1, &fb, RectF(0, 0, colw, 1000 * k), nullptr, &bb);
                REAL need = bh.Height + bb.Height + 12 * k;
                if (y + need > bottom) { if (col == 1) { fits = false; break; } col = 1; y = top; }
                ys.push_back(y); cols.push_back(col);
                y += need;
            }
            if (!fits && sz > 10 * k + 0.1f) continue;
            for (int i = 0; i < (int)ys.size(); ++i) {
                REAL x = 30 * k + cols[i] * (colw + gap);
                RectF bh; g.MeasureString(d->secs[i].head, -1, &fh, RectF(0, 0, colw, 1000 * k), nullptr, &bh);
                g.DrawString(d->secs[i].head, -1, &fh, RectF(x, ys[i], colw, bh.Height + 2), nullptr, &head);
                g.DrawString(d->secs[i].body, -1, &fb, RectF(x, ys[i] + bh.Height, colw, 1000 * k), nullptr, &grey);
            }
            break;
        }
        {                                                                  // блок «Запомните»: по центру внизу, в рамке
            REAL ny = h - 52 * k - note_h;
            SolidBrush panel(Color(255, 26, 31, 42));
            Pen yb(Color(255, 255, 214, 40), 1.5f * k);
            g.FillRectangle(&panel, 80 * k, ny, w - 160 * k, note_h);
            g.DrawRectangle(&yb, 80 * k, ny, w - 160 * k, note_h);
            StringFormat cf; cf.SetAlignment(StringAlignmentCenter);
            g.DrawString(note.head, -1, &nh, RectF(100 * k, ny + 8 * k, note_w, nbh.Height + 2), &cf, &head);
            g.DrawString(note.body, -1, &nb, RectF(100 * k, ny + 12 * k + nbh.Height, note_w, nbb.Height + 4), &cf, &white);
        }        // подпись внизу: клавиша закрытия — ярко
        Font ff2(&ff, 14 * k, FontStyleRegular, UnitPixel), fb2(&ff, 14 * k, FontStyleBold, UnitPixel);
        std::wstring ks(S.help_text.begin(), S.help_text.end());
        StringFormat sf(StringFormat::GenericTypographic());
        sf.SetFormatFlags(sf.GetFormatFlags() | StringFormatFlagsMeasureTrailingSpaces | StringFormatFlagsNoWrap);
        std::wstring tail = std::wstring(L" — ") + d->close;
        RectF b1, b2; g.MeasureString(ks.c_str(), -1, &fb2, PointF(0, 0), &sf, &b1); g.MeasureString(tail.c_str(), -1, &ff2, PointF(0, 0), &sf, &b2);
        REAL px = (w - b1.Width - b2.Width) / 2, py = h - 34 * k;
        g.DrawString(ks.c_str(), -1, &fb2, PointF(px, py), &sf, &head);
        g.DrawString(tail.c_str(), -1, &ff2, PointF(px + b1.Width, py), &sf, &grey);
    }
    out.resize((size_t)w * h);
    BitmapData bd; Rect r(0, 0, w, h);
    if (bmp.LockBits(&r, ImageLockModeRead, PixelFormat32bppARGB, &bd) == Ok) {
        for (int yy = 0; yy < h; ++yy) memcpy(&out[(size_t)yy * w], (BYTE *)bd.Scan0 + (size_t)yy * bd.Stride, (size_t)w * 4);
        bmp.UnlockBits(&bd);
    }
    for (auto &p : out) p |= 0xFF000000u;
}
// Поворотники листают месяцы: каждое нажатие (включил или выключил) — шаг, левый назад, правый вперёд.
// Если выключение одного совпало с включением другого (переключили сразу с левого на правый) — шаг только один.
static bool g_seen_l = false, g_seen_r = false;
static int g_pending_off = 0;            // -1 / +1: выключился поворотник, шаг через 150 мс, если другой не включится
static DWORD g_pending_tick = 0;

static void handle_blinkers()
{
    bool l, r;
    { std::lock_guard<std::mutex> lk(g_data_mx); l = g_blink_l; r = g_blink_r; }
    if (!S.blinkers) { g_seen_l = l; g_seen_r = r; return; }
    DWORD now = GetTickCount();
    bool l_on = l && !g_seen_l, r_on = r && !g_seen_r, l_off = !l && g_seen_l, r_off = !r && g_seen_r;
    g_seen_l = l; g_seen_r = r;
    if (!g_open) { g_pending_off = 0; return; }
    if (l_on && r_on) { g_pending_off = 0; return; }            // аварийка
    if (l_on || r_on) { g_pending_off = 0; g_month_offset += l_on ? -1 : 1; return; }
    if (l_off || r_off) { g_pending_off = l_off ? -1 : 1; g_pending_tick = now; }
    if (g_pending_off && now - g_pending_tick >= 150) { g_month_offset += g_pending_off; g_pending_off = 0; }
}

static void handle_keys(HWND hwnd)
{
    handle_blinkers();
    if (GetForegroundWindow() != hwnd) return;
    // модификаторы совпадают точно: Shift+F9 не срабатывает на Ctrl+F9 (телепорт в режиме разработчика) и Ctrl+Shift+F9
    bool mods = (S.key_ctrl == (bool)down(VK_CONTROL)) && (S.key_shift == (bool)down(VK_SHIFT)) && (S.key_alt == (bool)down(VK_MENU));
    bool k = down(S.key) && mods;
    if (k && !g_key_down) { g_open = !g_open; g_month_offset = 0; g_pending_off = 0; if (g_open) { g_mini = false; g_help = false; } }   // новое окно закрывает предыдущее
    g_key_down = k;
    {                                                // мини-окно тахографа (Numpad5)
        static bool mk_down = false;
        bool mm = (S.mini_ctrl == (bool)down(VK_CONTROL)) && (S.mini_shift == (bool)down(VK_SHIFT)) && (S.mini_alt == (bool)down(VK_MENU));
        bool mk = down(S.mini_key) && mm;
        if (mk && !mk_down) { g_mini = !g_mini; if (g_mini) { g_open = false; g_help = false; } }
        mk_down = mk;
    }
    {                                                // краткая инструкция по тахографу (Shift+Alt+F9)
        static bool hk_down = false;
        bool hm = (S.help_ctrl == (bool)down(VK_CONTROL)) && (S.help_shift == (bool)down(VK_SHIFT)) && (S.help_alt == (bool)down(VK_MENU));
        bool hkk = down(S.help_key) && hm;
        if (hkk && !hk_down) { g_help = !g_help; if (g_help) { g_open = false; g_mini = false; } }
        hk_down = hkk;
    }
    {                                                // кнопка включения / выключения тахографа (ETS2) или логбука HOS (ATS)
        static bool tk_down = false;
        bool tm = (S.tk_ctrl == (bool)down(VK_CONTROL)) && (S.tk_shift == (bool)down(VK_SHIFT)) && (S.tk_alt == (bool)down(VK_MENU));
        bool tkk = down(S.tk_key) && tm;
        if (tkk && !tk_down) {
            std::lock_guard<std::mutex> l(g_data_mx);
            S.tacho_on = !S.tacho_on;
            if (S.tacho_on) { g_tacho = Tacho(); g_hos = Hos(); g_viol = Viol(); }       // при включении счёт начинается заново
            store_set("ui", {std::string("tacho ") + (S.tacho_on ? "1" : "0")});
            store_flush(true);
            const TachoText *tt = tacho_text(UT->code);
            std::wstring name = g_ats ? tt->hname : tt->tname;
            show_notice(name + L" " + (S.tacho_on ? tt->on_s : tt->off_s), S.tacho_on ? 0xFF6EC878 : 0xFFFFC83C);
        }
        tk_down = tkk;
    }
    if (g_open) {
        bool p = S.prev_key && down(S.prev_key), n = S.next_key && down(S.next_key);
        if (p && !g_prev_down) --g_month_offset;
        if (n && !g_next_down) ++g_month_offset;
        g_prev_down = p; g_next_down = n;
    }
}

static bool is_rgba(DXGI_FORMAT f)
{
    return f == DXGI_FORMAT_R8G8B8A8_UNORM || f == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || f == DXGI_FORMAT_R8G8B8A8_TYPELESS;
}
static bool is_bgra(DXGI_FORMAT f)
{
    return f == DXGI_FORMAT_B8G8R8A8_UNORM || f == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB || f == DXGI_FORMAT_B8G8R8A8_TYPELESS ||
           f == DXGI_FORMAT_B8G8R8X8_UNORM || f == DXGI_FORMAT_B8G8R8X8_UNORM_SRGB;
}

// Сообщения вверху экрана на 3,5 секунды: новое время года, тахограф, скорый запрет движения.
static int g_last_season = -1;
static std::string g_season_profile;
static ID3D11Texture2D *g_ntex;
static int g_ntex_w, g_ntex_h;
static DXGI_FORMAT g_ntex_fmt;
static std::wstring g_notice_drawn;
static long long g_ban_warned = -1;      // начало запрета (местные минуты), о котором уже сказали

static void watch_season(const Now &now)
{
    if (!now.valid) return;
    if (!g_profile.empty()) {                                  // новый профиль ещё без даты — дата сейчас прыгнет
        bool known = false;
        for (auto &a : g_anchors) if (a.id == g_profile) known = true;
        if (!known) return;
    }
    int s = season_of(now.m).index;
    if (g_season_profile != g_profile || g_last_season < 0) { g_season_profile = g_profile; g_last_season = s; return; }
    if (s != g_last_season) {
        g_last_season = s;
        std::lock_guard<std::mutex> l(g_data_mx);
        show_notice(LT->notice[s], season_of(s == 0 ? 1 : s == 1 ? 4 : s == 2 ? 7 : 10).color.GetValue());
        log_line("season changed: %d", s);
    }
}

// ETS2: за 30 минут до начала общего запрета в этой стране — сообщение
static void watch_bans(const Now &now)
{
    if (g_ats || !now.valid || !now.country) return;
    const CountryBans *c = country_bans(now.country);
    if (!c || !c->rules) return;
    if (strcmp(now.country, "switzerland")) {              // груз из списка исключений — сообщений нет (в Швейцарии — для всех)
        std::lock_guard<std::mutex> l(g_data_mx);
        if (g_job.on && cargo_ban_free(cargo_id_of_key(g_job.key))) return;
    }
    int mins = now.hh * 60 + now.mm;
    for (int dd = 0; dd < 2; ++dd) {
        long z = now.day + dd;
        BanSpan sp[8];
        int n = bans_of_day(c, z, weekday(z), sp, 8);
        for (int i = 0; i < n; ++i) {
            if (sp[i].partial) continue;
            int start = sp[i].from + dd * 1440;
            if (dd == 0 && sp[i].from == 0) continue;           // продолжение вчерашнего
            if (dd == 1 && sp[i].from == 0) {                    // продолжение сегодняшнего запрета до 24:00
                BanSpan t[8]; int tn = bans_of_day(c, now.day, weekday(now.day), t, 8);
                bool cont = false;
                for (int j = 0; j < tn; ++j) if (!t[j].partial && t[j].to >= 1440) cont = true;
                if (cont) continue;
            }
            if (start - mins > 0 && start - mins <= 30) {
                long long key = (long long)z * 1440 + sp[i].from;
                if (key == g_ban_warned) return;
                g_ban_warned = key;
                std::wstring what = game_str(now.country) + L" " + spans_text(&sp[i], 1) + L" (" + why_text(sp[i], z, true) + L")";
                std::wstring ex = exempt_text(now.country);
                if (!ex.empty()) what += L". " + ex;
                std::lock_guard<std::mutex> l(g_data_mx);
                show_notice(wfmt(UT->w_ban, what), 0xFFFFAA3C);
                return;
            }
        }
    }
}

static void draw_notice(int w, int h, float k, const std::wstring &text, ARGB color, std::vector<uint32_t> &out)
{
    Bitmap bmp(w, h, PixelFormat32bppARGB);
    {
        Graphics g(&bmp);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
        g.Clear(Color(255, 18, 22, 30));
        static const wchar_t *fname = pick_font();
        FontFamily ff(fname);
        SolidBrush seBrush((Color(color))), white(Color(255, 235, 238, 245));
        g.FillRectangle(&seBrush, 0.f, 0.f, 10 * k, (REAL)h);
        Pen border(Color(255, 60, 68, 84), 2 * k);
        g.DrawRectangle(&border, 1 * k, 1 * k, w - 2 * k, h - 2 * k);
        StringFormat c; c.SetAlignment(StringAlignmentCenter); c.SetLineAlignment(StringAlignmentCenter);
        for (REAL size = 24 * k; size > 9 * k; size -= 1 * k) {               // самый крупный шрифт, при котором текст влезает (с переносом)
            Font f(&ff, size, FontStyleBold, UnitPixel); RectF box;
            g.MeasureString(text.c_str(), -1, &f, RectF(0, 0, w - 50 * k, 1000 * k), &c, &box);
            if (box.Height <= h - 10 * k || size <= 10 * k) { g.DrawString(text.c_str(), -1, &f, RectF(20 * k, 0, w - 30 * k, (REAL)h), &c, &white); break; }
        }
    }
    out.resize((size_t)w * h);
    BitmapData bd; Rect r(0, 0, w, h);
    if (bmp.LockBits(&r, ImageLockModeRead, PixelFormat32bppARGB, &bd) == Ok) {
        for (int yy = 0; yy < h; ++yy)
            memcpy(&out[(size_t)yy * w], (BYTE *)bd.Scan0 + (size_t)yy * bd.Stride, (size_t)w * 4);
        bmp.UnlockBits(&bd);
    }
    for (auto &p : out) p |= 0xFF000000u;
}

// создаёт текстуру нужного размера и формата (или оставляет прежнюю); false — не вышло
static bool ensure_tex(ID3D11Device *dev, ID3D11Texture2D *&tex, int &tw, int &th, DXGI_FORMAT &tf, int w, int h, DXGI_FORMAT fmt,
                       std::wstring &drawn)
{
    if (tex && tw == w && th == h && tf == fmt) return true;
    if (tex) { tex->Release(); tex = nullptr; }
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1; td.Format = fmt;
    td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT;
    if (FAILED(dev->CreateTexture2D(&td, nullptr, &tex))) { tex = nullptr; return false; }
    tw = w; th = h; tf = fmt; drawn.clear();
    return true;
}

static void upload(ID3D11DeviceContext *ctx, ID3D11Texture2D *tex, std::vector<uint32_t> &px, int w, DXGI_FORMAT fmt)
{
    if (is_rgba(fmt))
        for (auto &p : px) p = (p & 0xFF00FF00u) | ((p & 0xFF) << 16) | ((p >> 16) & 0xFF);
    ctx->UpdateSubresource(tex, 0, nullptr, px.data(), w * 4, 0);
}

static std::atomic<int> g_rs{0};          // фоновая отрисовка окна: 0 — свободна, 1 — рисует, 2 — картинка готова
static std::vector<uint32_t> g_rpx;
static int g_rw, g_rh;
static std::wstring g_rkey;

static DWORD g_watch_tick = 0;
static const int WIN_W = 1260, WIN_H = 800;     // размер окна при 1080p

static void overlay(IDXGISwapChain *sc)
{
    DXGI_SWAP_CHAIN_DESC scd;
    if (FAILED(sc->GetDesc(&scd))) return;
    handle_keys(scd.OutputWindow);

    DWORD tick = GetTickCount();
    Now now;
    bool computed = false;
    if (g_open || tick - g_watch_tick >= 1000) {           // сезон и запреты — раз в секунду и при открытом окне
        g_watch_tick = tick;
        now = compute_now(); computed = true;
        watch_season(now);
        watch_bans(now);
    }
    std::wstring ntext; ARGB ncolor; int nid; bool notice;
    { std::lock_guard<std::mutex> l(g_data_mx); ntext = g_notice_text; ncolor = g_notice_color; nid = g_notice_id;
      notice = !ntext.empty() && (int)(g_notice_until - tick) > 0; }
    if (!g_open && !notice && !g_mini && !g_help) return;
    if (g_open && !computed) now = compute_now();

    ID3D11Device *dev = nullptr;
    if (FAILED(sc->GetDevice(__uuidof(ID3D11Device), (void **)&dev)) || !dev) return;
    ID3D11Texture2D *back = nullptr;
    if (FAILED(sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void **)&back)) || !back) { dev->Release(); return; }
    D3D11_TEXTURE2D_DESC bd; back->GetDesc(&bd);

    if (bd.SampleDesc.Count != 1 || (!is_rgba(bd.Format) && !is_bgra(bd.Format))) {
        if (!g_logged_fmt) { log_line("back buffer format %d / samples %d not supported", bd.Format, bd.SampleDesc.Count); g_logged_fmt = true; }
        back->Release(); dev->Release(); return;
    }

    ID3D11DeviceContext *ctx = nullptr;
    dev->GetImmediateContext(&ctx);
    float k = S.scale * bd.Height / 1080.0f;
    if (bd.Height < 1000) {                                              // Steam Deck и другие маленькие экраны: окно почти во весь экран, шрифт крупнее
        float kfit = std::min(bd.Width * 0.97f / WIN_W, bd.Height * 0.95f / WIN_H);
        k = kfit * std::min(S.scale, 1.0f);
    }
    if (WIN_W * k > bd.Width * 0.98f) k = bd.Width * 0.98f / WIN_W;      // узкий экран

    if (g_open) {
        int w = (int)(WIN_W * k), h = (int)(WIN_H * k);
        if (w <= (int)bd.Width && h <= (int)bd.Height &&
            ensure_tex(dev, g_tex, g_tex_w, g_tex_h, g_tex_fmt, w, h, bd.Format, g_last_drawn)) {
            Snap sn = take_snapshot(now, g_month_offset);
            wchar_t keybuf[256];
            swprintf(keybuf, 256, L"%d|%ld|%d|%d|%d|%s|%S|%d|%d|%.0f|%.1f|%d|%d|%.0f|%d|%d|%d|%d", now.valid, now.day, now.hh * 60 + now.mm,
                     g_month_offset, w, now.place.c_str(), g_lang.c_str(), (int)sn.trips.size(), sn.job.on, sn.job.km,
                     sn.month_fuel, sn.tv.rest, sn.tv.cont, sn.odo, sn.svc, sn.vv.shift, sn.vv.week, sn.vv.month);
            // окно рисуется в отдельном потоке (GDI+ на процессоре занимает десятки мс — в кадре игры это был бы рывок);
            // в кадре — только загрузка готовой картинки в текстуру
            if (g_rs == 2) {
                if (g_rw == w && g_rh == h) { upload(ctx, g_tex, g_rpx, w, bd.Format); g_last_drawn = g_rkey; }
                g_rs = 0;
            }
            if (g_last_drawn != keybuf && g_rs == 0) {
                g_rkey = keybuf; g_rw = w; g_rh = h; g_rs = 1;
                std::thread([w, h, k, now, off = g_month_offset, sn]() {
                    draw_window(w, h, k, now, off, sn, g_rpx);
                    g_rs = 2;
                }).detach();
            }
            UINT x = S.position == 1 ? bd.Width - w - (UINT)(40 * k) : (bd.Width - w) / 2;
            UINT y = S.position == 1 ? (UINT)(120 * k) : (bd.Height - h) / 2;
            ctx->CopySubresourceRegion(back, 0, x, y, 0, g_tex, 0, nullptr);
        }
    }

    if (g_help) {                                                // краткая инструкция по тахографу: по центру, статичный текст — рисуется один раз
        int w = (int)(WIN_W * k), h = (int)(WIN_H * k);
        if (w <= (int)bd.Width && h <= (int)bd.Height &&
            ensure_tex(dev, g_htex, g_htex_w, g_htex_h, g_htex_fmt, w, h, bd.Format, g_help_drawn)) {
            wchar_t kb[96]; swprintf(kb, 96, L"%d|%d|%S|%d", w, h, UT->code, (int)g_ats);
            if (g_help_drawn != kb) {
                std::vector<uint32_t> px;
                draw_help(w, h, k, px);
                upload(ctx, g_htex, px, w, bd.Format);
                g_help_drawn = kb;
            }
            ctx->CopySubresourceRegion(back, 0, (bd.Width - w) / 2, (bd.Height - h) / 2, 0, g_htex, 0, nullptr);
        }
    }

    if (g_mini) {                                                // мини-окно тахографа: левый верхний угол
        float km = bd.Height < 1000 ? std::max(0.8f, bd.Height / 800.0f * 0.8f) : bd.Height / 1080.0f;
        int w = (int)(340 * km * S.scale), h = (int)(215 * km * S.scale);
        if (w < 170) { w = 170; h = 107; }
        if (w <= (int)bd.Width && h <= (int)bd.Height &&
            ensure_tex(dev, g_mtex, g_mtex_w, g_mtex_h, g_mtex_fmt, w, h, bd.Format, g_mini_drawn)) {
            Mini mi = mini_snapshot();
            wchar_t kb[96];
            swprintf(kb, 96, L"%d|%d|%d|%d|%d|%d|%d|%S", w, mi.on, mi.has, mi.resting, mi.rest, mi.shift_left, mi.brk_left * 1000 + mi.week_left + mi.drive_left * 7 + mi.drive_extra * 13 + mi.shift_extra * 17 + mi.rest_long + S.mini_style * 100000, UT->code);
            if (g_mini_drawn != kb) {
                std::vector<uint32_t> px;
                draw_mini(w, h, w / 340.0f, mi, px);
                upload(ctx, g_mtex, px, w, bd.Format);
                g_mini_drawn = kb;
            }
            ctx->CopySubresourceRegion(back, 0, (UINT)(20 * km), (UINT)(20 * km), 0, g_mtex, 0, nullptr);
        }
    }

    if (notice) {
        int w = (int)(900 * k), h = (int)((ntext.size() > 80 ? 92 : 64) * k);   // длинное сообщение — в 2–3 строки
        if (w > (int)bd.Width) w = (int)bd.Width;
        if (ensure_tex(dev, g_ntex, g_ntex_w, g_ntex_h, g_ntex_fmt, w, h, bd.Format, g_notice_drawn)) {
            wchar_t keybuf[64];
            swprintf(keybuf, 64, L"%d|%d", nid, w);
            if (g_notice_drawn != keybuf) {
                std::vector<uint32_t> px;
                draw_notice(w, h, k, ntext, ncolor, px);
                upload(ctx, g_ntex, px, w, bd.Format);
                g_notice_drawn = keybuf;
            }
            ctx->CopySubresourceRegion(back, 0, (bd.Width - w) / 2, (UINT)(70 * k), 0, g_ntex, 0, nullptr);
        }
    }

    ctx->Release(); back->Release(); dev->Release();
}
static HRESULT STDMETHODCALLTYPE hk_present(IDXGISwapChain *sc, UINT sync, UINT flags)
{
    ++g_in_hook;
    if (g_active && !(flags & DXGI_PRESENT_TEST)) overlay(sc);
    HRESULT r = g_orig_present(sc, sync, flags);
    --g_in_hook;
    return r;
}

static HRESULT STDMETHODCALLTYPE hk_present1(IDXGISwapChain1 *sc, UINT sync, UINT flags, const DXGI_PRESENT_PARAMETERS *pp)
{
    ++g_in_hook;
    if (g_active && !(flags & DXGI_PRESENT_TEST)) overlay(sc);
    HRESULT r = g_orig_present1(sc, sync, flags, pp);
    --g_in_hook;
    return r;
}

static void patch(void **slot, void *fn, void **orig)
{
    DWORD old;
    if (!VirtualProtect(slot, sizeof(void *), PAGE_EXECUTE_READWRITE, &old)) return;
    if (orig) *orig = *slot;
    *slot = fn;
    VirtualProtect(slot, sizeof(void *), old, &old);
}

static bool install_hook()
{
    WNDCLASSEXW wc = {sizeof wc};
    wc.lpfnWndProc = DefWindowProcW; wc.hInstance = g_module; wc.lpszClassName = L"DriverDeskDummy";
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowW(wc.lpszClassName, L"", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, nullptr, nullptr, g_module, nullptr);
    if (!hwnd) return false;

    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferCount = 1; sd.BufferDesc.Width = 64; sd.BufferDesc.Height = 64;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd; sd.SampleDesc.Count = 1; sd.Windowed = TRUE; sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    IDXGISwapChain *sc = nullptr; ID3D11Device *dev = nullptr; ID3D11DeviceContext *ctx = nullptr;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
                                               &sd, &sc, &dev, nullptr, &ctx);
    bool ok = false;
    if (SUCCEEDED(hr) && sc) {
        g_vtbl = *(void ***)sc;
        patch(&g_vtbl[8], (void *)hk_present, (void **)&g_orig_present);
        IDXGISwapChain1 *sc1 = nullptr;
        if (SUCCEEDED(sc->QueryInterface(__uuidof(IDXGISwapChain1), (void **)&sc1)) && sc1) {
            g_vtbl1 = *(void ***)sc1;
            patch(&g_vtbl1[22], (void *)hk_present1, (void **)&g_orig_present1);
            sc1->Release();
        }
        ok = true;
    }
    else log_line("D3D11CreateDeviceAndSwapChain failed 0x%08X", (unsigned)hr);
    if (ctx) ctx->Release();
    if (dev) dev->Release();
    if (sc) sc->Release();
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, g_module);
    return ok;
}

static void remove_hook()
{
    g_active = false;
    if (g_vtbl && g_orig_present) patch(&g_vtbl[8], (void *)g_orig_present, nullptr);
    if (g_vtbl1 && g_orig_present1) patch(&g_vtbl1[22], (void *)g_orig_present1, nullptr);
    for (int i = 0; i < 200 && g_in_hook > 0; ++i) Sleep(5);
    for (int i = 0; i < 400 && g_rs == 1; ++i) Sleep(5);      // дождаться фоновой отрисовки окна
}

// ---------------------------------------------------------------- вход SDK

SCSAPI_RESULT scs_telemetry_init(const scs_u32_t version, const scs_telemetry_init_params_t *const params)
{
    // 1.01 нужна для событий рейса (job.delivered и др.); параметры у 1.00 и 1.01 одинаковые
    if (version != SCS_TELEMETRY_VERSION_1_01 && version != SCS_TELEMETRY_VERSION_1_00) return SCS_RESULT_unsupported;
    g_main_thread = GetCurrentThreadId();
    const auto *p = static_cast<const scs_telemetry_init_params_v100_t *>(params);
    g_game_log = p->common.log;

    wchar_t path[MAX_PATH];
    GetModuleFileNameW(g_module, path, MAX_PATH);
    g_dir = path; g_dir.erase(g_dir.find_last_of(L"\\/") + 1);
    migrate_legacy_files();
    load_settings();

    g_ats = strcmp(p->common.game_id, SCS_GAME_ID_ATS) == 0;
    if (g_ats) { g_cities = cities_ats; g_city_count = sizeof cities_ats / sizeof cities_ats[0]; }
    else { g_cities = cities_ets2; g_city_count = sizeof cities_ets2 / sizeof cities_ets2[0]; }
    g_game_folder = g_ats ? "American Truck Simulator" : "Euro Truck Simulator 2";
    g_log_path = documents_file(g_game_folder.c_str(), L"game.log.txt");
    store_load();
    for (auto &line : store_get("ui")) if (line.compare(0, 6, "tacho ") == 0) S.tacho_on = line[6] != '0';   // состояние кнопки тахографа с прошлого раза
    load_anchors();
    if (S.tz_base == INT_MIN) S.tz_base = g_ats ? -360 : 120;
    g_tz_enabled = S.time_zones >= 0 ? S.time_zones : read_game_time_zones(documents_file(g_game_folder.c_str(), L"config.cfg"));

    p->register_for_channel(SCS_TELEMETRY_CHANNEL_game_time, SCS_U32_NIL, SCS_VALUE_TYPE_u32,
                            SCS_TELEMETRY_CHANNEL_FLAG_none, on_time, nullptr);
    p->register_for_channel(SCS_TELEMETRY_TRUCK_CHANNEL_world_placement, SCS_U32_NIL, SCS_VALUE_TYPE_dplacement,
                            SCS_TELEMETRY_CHANNEL_FLAG_none, on_placement, nullptr);
    p->register_for_channel(SCS_TELEMETRY_TRUCK_CHANNEL_lblinker, SCS_U32_NIL, SCS_VALUE_TYPE_bool,
                            SCS_TELEMETRY_CHANNEL_FLAG_none, on_blinker, &g_blink_l);
    p->register_for_channel(SCS_TELEMETRY_TRUCK_CHANNEL_rblinker, SCS_U32_NIL, SCS_VALUE_TYPE_bool,
                            SCS_TELEMETRY_CHANNEL_FLAG_none, on_blinker, &g_blink_r);
    p->register_for_channel(SCS_TELEMETRY_TRUCK_CHANNEL_speed, SCS_U32_NIL, SCS_VALUE_TYPE_float,
                            SCS_TELEMETRY_CHANNEL_FLAG_none, on_speed, nullptr);
    p->register_for_channel(SCS_TELEMETRY_TRUCK_CHANNEL_fuel, SCS_U32_NIL, SCS_VALUE_TYPE_float,
                            SCS_TELEMETRY_CHANNEL_FLAG_none, on_fuel, nullptr);
    p->register_for_channel(SCS_TELEMETRY_TRUCK_CHANNEL_odometer, SCS_U32_NIL, SCS_VALUE_TYPE_float,
                            SCS_TELEMETRY_CHANNEL_FLAG_none, on_odometer, nullptr);
    p->register_for_channel(SCS_TELEMETRY_TRUCK_CHANNEL_wear_engine, SCS_U32_NIL, SCS_VALUE_TYPE_float,
                            SCS_TELEMETRY_CHANNEL_FLAG_none, on_wear, (scs_context_t)(intptr_t)PART_ENGINE);
    p->register_for_channel(SCS_TELEMETRY_TRUCK_CHANNEL_wear_transmission, SCS_U32_NIL, SCS_VALUE_TYPE_float,
                            SCS_TELEMETRY_CHANNEL_FLAG_none, on_wear, (scs_context_t)(intptr_t)PART_TRANSMISSION);
    p->register_for_channel(SCS_TELEMETRY_TRUCK_CHANNEL_wear_wheels, SCS_U32_NIL, SCS_VALUE_TYPE_float,
                            SCS_TELEMETRY_CHANNEL_FLAG_none, on_wear, (scs_context_t)(intptr_t)PART_WHEELS);
    p->register_for_event(SCS_TELEMETRY_EVENT_configuration, on_config, nullptr);
    if (version >= SCS_TELEMETRY_VERSION_1_01) p->register_for_event(SCS_TELEMETRY_EVENT_gameplay, on_gameplay, nullptr);

    GdiplusStartupInput gsi;
    GdiplusStartup(&g_gdip_token, &gsi, nullptr);

    if (!install_hook()) { log_line("overlay hook failed"); return SCS_RESULT_ok; }
    g_active = true;
    log_line("DriverDesk %s loaded: %s, %u cities, %u profiles, new profile starts %02d-%02d, time zones %s (base %d)",
             PLUGIN_VERSION, g_ats ? "ATS" : "ETS2", (unsigned)g_city_count, (unsigned)g_anchors.size(), S.new_m, S.new_d,
             g_tz_enabled ? "on" : "off", S.tz_base);
    return SCS_RESULT_ok;
}

SCSAPI_VOID scs_telemetry_shutdown(void)
{
    remove_hook();
    {
        std::lock_guard<std::mutex> l(g_data_mx);
        tacho_save(g_profile); hos_save(g_profile); viol_save(g_profile); days_save(g_profile); service_save(g_profile);
    }
    store_flush(true);
    g_game_log = nullptr;
    // текстуру и GDI+ не освобождаем: игра закрывается, а кадр мог ещё рисоваться в другом потоке
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) { g_module = module; DisableThreadLibraryCalls(module); }
    return TRUE;
}
