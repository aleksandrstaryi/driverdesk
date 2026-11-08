// Тахограф (ЕС 561/2006, только ETS2), путевой журнал + расход топлива, техобслуживание по пробегу.
// Вся логика — в потоке игры (колбэки телеметрии) под g_data_mx; окно читает под тем же мьютексом.
// Данные — в общем файле DriverDesk_data.ini (store.h), разделы [tacho|service|days|trips <профиль>]
#pragma once
#include <map>
#include <vector>
#include <algorithm>

static std::string sec(const char *kind, const std::string &profile) { return std::string(kind) + " " + profile; }

// ---------------------------------------------------------------- тахограф
// Время — игровые минуты (game.time). Минута «вождение», если грузовик ехал (>1 км/ч); иначе отдых.
// Скачок времени (сон, паром, поезд) — отдых.

struct Tacho {
    bool init = false;
    scs_u32_t last_t = 0;
    int cont = 0;               // непрерывное вождение после последнего перерыва
    bool part15 = false;        // был перерыв ≥15 мин (первая часть 15+30)
    int rest = 0;               // идущий сейчас отдых
    bool split3 = false;        // в эти сутки был отдых ≥3 ч (первая часть 3+9)
    int day_drive = 0;          // вождение после последнего ежедневного отдыха
    bool day_ext = false;       // эти сутки уже больше 9 ч
    long long day_start = 0;    // конец последнего ежедневного/еженедельного отдыха (начало рабочей смены)
    bool shift_on = true;       // рабочая смена идёт; false после включения/обнуления тахографа, пока грузовик ни разу не поехал: смена начинается с первой минуты вождения
    long week = -1;             // номер недели (пн–вс по календарю игры)
    int week_drive = 0, prev_week_drive = 0, ext_days = 0;
    int reduced = 0;            // сокращённых ежедневных отдыхов (9 ч) после еженедельного
    long long weekly_end = 0;   // конец последнего еженедельного отдыха
    bool weekly_reduced = false;   // прошлый еженедельный отдых был сокращённым (24–45 ч): следующий должен быть полным 45 ч
    int warned = 0;             // какие предупреждения уже показаны (биты), сбрасываются отдыхом
    bool weekly_viol = false;   // (не сохраняется) только что закончили отдых 24–45 ч после сокращённого — нарушение, показать сообщение
    bool must_full = false;     // (только в копии для окна) идущий отдых должен дойти до 45 ч
    bool rest_long = false;     // (только в копии для окна) идущий отдых — после смены дольше 13 ч: засчитывается 9 ч, сколько бы ни стояли
    int rest_shift_len = 0;     // (только в копии для окна) длина прошлой смены, мин
    int reduced_orig = 0;       // (только в копии для окна) сокращённых отдыхов было до идущего отдыха
    bool rest_show = false;     // (только в копии для окна) показывать счёт отдыха: смена уже была
    // долги по сокращённому еженедельному отдыху (ЕС 561/2006): недобранное до 45 ч время нужно компенсировать целым отдыхом
    // не позднее конца третьей недели после той, где был сокращённый отдых; присоединяется к любому отдыху ≥ 9 ч
    struct Debt { int owed; long deadline_week; };   // owed — минуты; deadline_week — номер недели, с начала которой срок вышел
    std::vector<Debt> debt;
};
static Tacho g_tacho;
// сколько идёт рабочая смена (мин): 0, пока смена не началась (после включения / обнуления грузовик ещё не ехал)
static long long shift_elapsed(const Tacho &a, long long t) { return a.shift_on ? std::max<long long>(0, t - a.day_start) : 0; }

// излишек отдыха (сверх 11 ч у ежедневного, сверх 45 ч у еженедельного) идёт в зачёт долгов, начиная с самого старого
static void tacho_apply_comp(Tacho &a, int extra)
{
    for (auto &d : a.debt) { int take = std::min(extra, d.owed); d.owed -= take; extra -= take; if (!extra) break; }
    a.debt.erase(std::remove_if(a.debt.begin(), a.debt.end(), [](const Tacho::Debt &d) { return d.owed <= 0; }), a.debt.end());
}

static void tacho_close_day(Tacho &a, long long t)
{
    a.day_drive = 0; a.day_ext = false; a.split3 = false; a.cont = 0; a.part15 = false; a.day_start = t; a.shift_on = true; a.warned &= (2048 | 4096 | 65536);   // сообщения о недельном/двухнедельном лимите сбрасывает только смена недели
}

static void tacho_end_rest(Tacho &a, long long t)
{
    int r = a.rest;
    a.rest = 0;
    if (r >= 24 * 60) {
        if (a.weekly_reduced && r < 45 * 60) a.weekly_viol = true;      // после сокращённого — снова сокращённый
        if (r > 45 * 60) tacho_apply_comp(a, r - 45 * 60);              // излишек сверх 45 ч — в зачёт старых долгов
        if (r < 45 * 60) a.debt.push_back({45 * 60 - r, a.week + 4});    // недобрано до 45 ч: компенсировать до конца 3-й недели после этой
        a.weekly_end = t; a.weekly_reduced = r < 45 * 60; a.reduced = 0;
        tacho_close_day(a, t);
        return;
    }
    if (r >= 9 * 60) {
        if (r > 11 * 60) tacho_apply_comp(a, r - 11 * 60);              // излишек сверх 11 ч ежедневного отдыха — тоже в зачёт
        bool long_shift = a.shift_on && ((t - r) - a.day_start) > 13 * 60;            // смена была дольше 13 ч (до 15 ч): отдых после неё засчитывается как 9 ч, даже если простояли 11 ч и больше
        if ((r < 11 * 60 && !a.split3) || long_shift) ++a.reduced;
        tacho_close_day(a, t);
        return;
    }
    if (r >= 3 * 60) a.split3 = true;
    if (r >= 45 || (r >= 30 && a.part15)) { a.cont = 0; a.part15 = false; a.warned &= ~(3 | 256 | 8192); }
    else if (r >= 15) a.part15 = true;
}
static void tacho_roll_week(long week)
{
    if (week == g_tacho.week) return;
    g_tacho.prev_week_drive = week == g_tacho.week + 1 ? g_tacho.week_drive : 0;
    g_tacho.week_drive = 0; g_tacho.ext_days = 0; g_tacho.week = week; g_tacho.warned &= ~(2048 | 4096 | 65536);
}

// минуты [last_t, t): driving — ехал ли грузовик; week — неделя по местному времени
static void tacho_step(scs_u32_t t, bool driving, long week)
{
    if (!g_tacho.init) {
        g_tacho = Tacho(); g_tacho.init = true; g_tacho.last_t = t; g_tacho.day_start = t; g_tacho.weekly_end = t; g_tacho.shift_on = false;   // смена начнётся с первой минуты вождения
        g_tacho.week = week;
        return;
    }
    if (t <= g_tacho.last_t) { g_tacho.last_t = t; return; }
    int dt = (int)(t - g_tacho.last_t);
    g_tacho.last_t = t;
    tacho_roll_week(week);
    if (!g_tacho.shift_on) g_tacho.weekly_end = (driving && dt <= 3) ? (long long)t - dt : (long long)t;   // пока грузовик ни разу не ехал после включения/обнуления — 144 ч не идут, отсчёт недели с первой минуты вождения
    if (dt > 3 || !driving) { g_tacho.rest += dt; return; }
    if (g_tacho.rest) tacho_end_rest(g_tacho, (long long)t - dt);
    if (!g_tacho.shift_on) { g_tacho.shift_on = true; g_tacho.day_start = (long long)t - dt; }   // первая минута вождения запускает рабочую смену
    if (g_tacho.day_drive > 540 && !g_tacho.day_ext) { g_tacho.day_ext = true; ++g_tacho.ext_days; }
}

// как выглядел бы тахограф, если идущий отдых сейчас закончить (для окна); rest — сколько он уже идёт
static Tacho tacho_view(scs_u32_t t)
{
    Tacho v = g_tacho;
    int r = v.rest;
    bool prev_reduced = v.weekly_reduced;
    if (r) {
        int slen = v.shift_on ? (int)(((long long)t - r) - v.day_start) : 0;               // длина смены, после которой идёт этот отдых
        tacho_end_rest(v, t); v.rest = r;
        v.rest_shift_len = slen; v.rest_long = slen > 13 * 60; v.reduced_orig = g_tacho.reduced;
        v.rest_show = slen > 0;                                          // смена была — счёт отдыха идёт, даже если простояли больше 11 ч
    }
    v.must_full = prev_reduced && r >= 20 * 60 && r < 45 * 60;   // идёт длинный отдых, а прошлый еженедельный был сокращённым
    return v;
}
static void tacho_save(const std::string &profile)
{
    if (profile.empty() || !g_tacho.init) return;
    const Tacho &a = g_tacho;
    char b[512];
    snprintf(b, sizeof b, "%u %d %d %d %d %d %d %lld %ld %d %d %d %d %lld %d %d %d", a.last_t, a.cont, a.part15, a.rest, a.split3,
             a.day_drive, a.day_ext, a.day_start, a.week, a.week_drive, a.prev_week_drive, a.ext_days, a.reduced,
             a.weekly_end, a.weekly_reduced, a.warned, a.shift_on);
    store_set(sec("tacho", profile), {b});
    std::vector<std::string> dl;
    for (auto &d : a.debt) { char c[64]; snprintf(c, sizeof c, "%d %ld", d.owed, d.deadline_week); dl.push_back(c); }
    store_set(sec("comp", profile), dl);
}

static void tacho_load(const std::string &profile)
{
    g_tacho = Tacho();
    auto lines = store_get(sec("tacho", profile));
    if (lines.empty()) return;
    Tacho a; int p15, s3, de, wr, so = 1;
    if (sscanf(lines[0].c_str(), "%u %d %d %d %d %d %d %lld %ld %d %d %d %d %lld %d %d %d", &a.last_t, &a.cont, &p15, &a.rest, &s3,
               &a.day_drive, &de, &a.day_start, &a.week, &a.week_drive, &a.prev_week_drive, &a.ext_days, &a.reduced,
               &a.weekly_end, &wr, &a.warned, &so) >= 16) {          // 17-е поле (смена идёт) есть не в старых данных — тогда смена считается идущей
        a.part15 = p15 != 0; a.split3 = s3 != 0; a.day_ext = de != 0; a.weekly_reduced = wr != 0; a.shift_on = so != 0; a.init = true;
        for (auto &line : store_get(sec("comp", profile))) {
            Tacho::Debt d;
            if (sscanf(line.c_str(), "%d %ld", &d.owed, &d.deadline_week) == 2 && d.owed > 0) a.debt.push_back(d);
        }
        g_tacho = a;
    }
}

// ---------------------------------------------------------------- путевой журнал и топливо

struct Trip {
    long long t_end_local = 0;  // местное игровое время сдачи (для даты)
    int status = 0;             // 0 — доставлен, 1 — отменён
    std::string cargo, from, to;
    double km = 0, fuel = 0;
    long long revenue = 0, fines = 0, tolls = 0, ferry = 0;
    float damage = 0;
    double fuel_cost = -1;      // стоимость сожжённого топлива по ценам стран/штатов, где ехали (-1 — не известна: старые записи)
};
struct ActiveJob {
    bool on = false;
    std::string key, cargo, from, to;
    double km = 0, fuel = 0;
    long long fines = 0, tolls = 0, ferry = 0;
    long long income = 0;
    double fuel_cost = 0;
};
static std::vector<Trip> g_trips;
static ActiveJob g_job;
static std::map<long, std::pair<double, double>> g_days;   // местный игровой день → км, литры
static bool g_journal_dirty = false;

// CSV: поля через ';', текст без ';'
static std::string csv_clean(std::string s) { for (auto &c : s) if (c == ';' || c == '\n' || c == '\r') c = ','; return s; }

static void days_save(const std::string &profile)
{
    if (profile.empty()) return;
    std::vector<std::string> lines;
    char b[2048];
    for (auto &d : g_days) { snprintf(b, sizeof b, "%ld;%.2f;%.2f", d.first, d.second.first, d.second.second); lines.push_back(b); }
    snprintf(b, sizeof b, "job;%d;%s;%s;%s;%s;%.2f;%.2f;%lld;%lld;%lld;%lld;%.2f", g_job.on, csv_clean(g_job.key).c_str(),
             csv_clean(g_job.cargo).c_str(), csv_clean(g_job.from).c_str(), csv_clean(g_job.to).c_str(), g_job.km, g_job.fuel,
             g_job.fines, g_job.tolls, g_job.ferry, g_job.income, g_job.fuel_cost);
    lines.push_back(b);
    store_set(sec("days", profile), lines);
}

static std::vector<std::string> split(const std::string &s, char c)
{
    std::vector<std::string> v; std::string cur;
    for (char ch : s) { if (ch == c) { v.push_back(cur); cur.clear(); } else if (ch != '\r' && ch != '\n') cur += ch; }
    v.push_back(cur);
    return v;
}

static void journal_load(const std::string &profile)
{
    g_trips.clear(); g_days.clear(); g_job = ActiveJob();
    for (auto &line : store_get(sec("trips", profile))) {
        auto v = split(line, ';');
        if (v.size() < 14) continue;
        Trip t;
        t.t_end_local = atoll(v[1].c_str()); t.status = atoi(v[2].c_str());
        t.cargo = v[3]; t.from = v[4]; t.to = v[5]; t.km = atof(v[6].c_str()); t.fuel = atof(v[7].c_str());
        t.revenue = atoll(v[9].c_str()); t.fines = atoll(v[10].c_str()); t.tolls = atoll(v[11].c_str());
        t.ferry = atoll(v[12].c_str()); t.damage = (float)atof(v[13].c_str());
        if (v.size() >= 15) t.fuel_cost = atof(v[14].c_str());
        g_trips.push_back(t);
    }
    for (auto &line : store_get(sec("days", profile))) {
        auto v = split(line, ';');
        if (v[0] == "job" && v.size() >= 12) {
            g_job.on = atoi(v[1].c_str()) != 0; g_job.key = v[2]; g_job.cargo = v[3]; g_job.from = v[4]; g_job.to = v[5];
            g_job.km = atof(v[6].c_str()); g_job.fuel = atof(v[7].c_str()); g_job.fines = atoll(v[8].c_str());
            g_job.tolls = atoll(v[9].c_str()); g_job.ferry = atoll(v[10].c_str()); g_job.income = atoll(v[11].c_str());
            if (v.size() >= 13) g_job.fuel_cost = atof(v[12].c_str());
        }
        else if (v.size() >= 3) g_days[atol(v[0].c_str())] = {atof(v[1].c_str()), atof(v[2].c_str())};
    }
}

// дата для чтения: ДД.ММ.ГГГГ ЧЧ:ММ по календарю профиля
static std::string trip_date_text(long long t_local, long base)
{
    int y, m, d; civil_from_days(base + (long)(t_local / 1440), y, m, d);
    char b[32]; snprintf(b, sizeof b, "%02d.%02d.%04d %02d:%02d", d, m, y, (int)(t_local % 1440) / 60, (int)(t_local % 60));
    return b;
}

static void journal_append(const std::string &profile, const Trip &t, long base)
{
    if (profile.empty()) return;
    // дата;t_local;статус;груз;откуда;куда;км;литры;л/100;доход;штрафы;дороги;паромы;повреждение груза
    double l100 = t.km > 1 ? t.fuel * 100 / t.km : 0;
    char b[2048];
    snprintf(b, sizeof b, "%s;%lld;%d;%s;%s;%s;%.1f;%.1f;%.1f;%lld;%lld;%lld;%lld;%.3f;%.2f", trip_date_text(t.t_end_local, base).c_str(),
             t.t_end_local, t.status, csv_clean(t.cargo).c_str(), csv_clean(t.from).c_str(), csv_clean(t.to).c_str(), t.km,
             t.fuel, l100, t.revenue, t.fines, t.tolls, t.ferry, t.damage, t.fuel_cost);
    store_add(sec("trips", profile), b);
    store_flush(true);
}

// ---------------------------------------------------------------- техобслуживание

// Пункты ТО и деталь, при ремонте износа которой в сервисе пункт начинается заново.
// Тормоза — к колёсам (в сервисе игры тормозные колодки меняются вместе с колёсами).
enum ServicePart { PART_ENGINE, PART_TRANSMISSION, PART_WHEELS };
struct ServiceItem { const char *key; double km; double mi; ServicePart part; };
static const ServiceItem SERVICE_ITEMS[] = {
    {"oil", 80000, 50000, PART_ENGINE}, {"fuel_filter", 80000, 50000, PART_ENGINE}, {"air_filter", 80000, 50000, PART_ENGINE},
    {"brakes", 50000, 50000, PART_WHEELS}, {"gearbox_axle", 100000, 100000, PART_TRANSMISSION},
};
static const int N_SERVICE = (int)(sizeof SERVICE_ITEMS / sizeof *SERVICE_ITEMS);
// ETS2 — в км (те же интервалы, что в ATS в милях), ATS — мили
static double service_km(int i) { return g_ats ? SERVICE_ITEMS[i].mi * 1.609344 : SERVICE_ITEMS[i].km; }
struct TruckService { double last[N_SERVICE] = {}; long inspect_day = 0, tacho_day = 0; };
static std::map<std::string, TruckService> g_service;
static std::string g_truck_key;
static double g_odo = -1;
static float g_wear[3] = {-1, -1, -1};      // двигатель, КПП, колёса
static void service_save(const std::string &profile)
{
    if (profile.empty()) return;
    std::vector<std::string> lines;
    for (auto &s : g_service) {
        char b[256];
        std::string line = csv_clean(s.first);
        snprintf(b, sizeof b, ";%ld;%ld", s.second.inspect_day, s.second.tacho_day); line += b;
        for (int i = 0; i < N_SERVICE; ++i) { snprintf(b, sizeof b, ";%.0f", s.second.last[i]); line += b; }
        lines.push_back(line);
    }
    store_set(sec("service", profile), lines);
}

static void service_load(const std::string &profile)
{
    g_service.clear();
    for (auto &line : store_get(sec("service", profile))) {
        auto v = split(line, ';');
        if ((int)v.size() < 3 + N_SERVICE) continue;
        TruckService s; s.inspect_day = atol(v[1].c_str()); s.tacho_day = atol(v[2].c_str());
        for (int i = 0; i < N_SERVICE; ++i) s.last[i] = atof(v[3 + i].c_str());
        g_service[v[0]] = s;
    }
}

// новый грузовик: считаем, что ТО и техосмотр только что пройдены (полные интервалы)
static TruckService &service_of(const std::string &key, double odo, long today)
{
    auto it = g_service.find(key);
    if (it != g_service.end()) return it->second;
    TruckService s;
    for (int i = 0; i < N_SERVICE; ++i) s.last[i] = odo;
    s.inspect_day = today; s.tacho_day = today;
    return g_service[key] = s;
}

// любой ремонт в сервисе: техосмотр и поверка тахографа, если до срока меньше месяца
static void service_visit(TruckService &s, long today)
{
    if (today - s.inspect_day >= 335) s.inspect_day = today;
    if (today - s.tacho_day >= 700) s.tacho_day = today;
}

// износ детали убрали в сервисе: пункты этой детали начинаются заново
static void service_replaced(TruckService &s, ServicePart part, double odo)
{
    for (int i = 0; i < N_SERVICE; ++i) if (SERVICE_ITEMS[i].part == part) s.last[i] = odo;
}