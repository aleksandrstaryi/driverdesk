# DriverDesk — рабочее место дальнобойщика для ETS2 и ATS
# DriverDesk — a trucker's desk for ETS2 and ATS

Плагин для одиночной игры **Euro Truck Simulator 2** и **American Truck Simulator** (Windows; Steam Deck — через Proton).
По одной клавише (**Shift+F9**) поверх игры открывается окно: календарь с реальной датой и праздниками, тахограф (ETS2) или логбук HOS (ATS), запреты движения грузовиков в Европе, путевой журнал и ТО.

A single-player plugin for **Euro Truck Simulator 2** and **American Truck Simulator** (Windows; Steam Deck through Proton).
One key (**Shift+F9**) opens a window over the game: a calendar with a real date and holidays, a tachograph (ETS2) or an HOS logbook (ATS), truck driving bans in Europe, a trip log and maintenance.

> **Важно / Note.** Это не мод `.scs`, а плагин `.dll`: его нельзя выложить в Мастерскую Steam, только скачать отсюда. Версия 1.2.
> This is a `.dll` plugin, not an `.scs` mod, so it is not on the Steam Workshop. Version 1.2.
>
> **Что нового в 1.2 / New in 1.2:** мини-окно тахографа на **F9** (до конца смены, до перерыва, до недельного отдыха); счётчик нарушений режима труда и отдыха за смену, неделю и месяц; ещё по одному сообщению за 15 минут до перерыва, конца смены, лимита 56 часов и недельного отдыха (ATS: перерыв, 11 ч, окно 14 ч, цикл 70 ч). / A small tachograph window on **F9** (time to shift end, to the break, to the weekly rest); a counter of driving and rest time violations per shift, week and month; one more on-screen warning 15 minutes before the break, the shift end, the 56 h limit and the weekly rest (ATS: break, 11 h, 14 h window, 70 h cycle).
>
> **Клавиши / Keys:** **Shift+F9** — большое окно / main window; **F9** — мини-окно тахографа / mini tachograph window; **Alt+F9** — тахограф (HOS) вкл/выкл / tachograph (HOS) on/off.

---

## Скриншоты / Screenshots

> Это пример окна на тестовых данных (рисуется самим плагином), а не снимок из игры. / A sample window with test data rendered by the plugin itself, not an in-game capture.

**ETS2** — календарь с днями запрета (красные рамки), тахограф, запреты в стране и у соседей, строка о грузе, журнал и ТО.

| Русский | English |
|---|---|
| ![ETS2 ru](docs/ets2_ru.png) | ![ETS2 en](docs/ets2_en.png) |

**ATS** — календарь, логбук HOS, журнал и ТО (мили, галлоны).

| Русский | English |
|---|---|
| ![ATS ru](docs/ats_ru.png) | ![ATS en](docs/ats_en.png) |

**Мини-окно тахографа (клавиша F9) / Mini tachograph window (F9)** — слева вверху экрана, в стиле тахографа. / Top-left corner of the screen, tachograph style.

| ETS2, русский | ATS, English |
|---|---|
| ![mini ru](docs/ets2_ru_mini.png) | ![mini en](docs/ats_en_mini.png) |

---
## Что умеет / Features

| | ETS2 | ATS |
|---|---|---|
| Календарь, часовой пояс страны/штата, праздники / Calendar, time zone, holidays | ✔ | ✔ |
| Тахограф ЕС 561/2006 / EU tachograph | ✔ | — |
| Логбук HOS (FMCSA) / US HOS logbook | — | ✔ |
| Запреты движения грузовиков, исключения для грузов / Truck bans, cargo exemptions | ✔ | — |
| Путевой журнал, расход топлива / Trip log, fuel | ✔ | ✔ (мили, галлоны) |
| ТО по пробегу / Maintenance by mileage | ✔ | ✔ |
| Мини-окно тахографа (F9) / Mini tachograph window (F9) | ✔ | ✔ |
| Счётчик нарушений за смену, неделю, месяц / Violation counter: shift, week, month | ✔ | ✔ |
| Сообщения на экране (за 60, 30 и 15 минут) / On-screen notices (60, 30 and 15 min) | ✔ | ✔ |

## Календарь / Calendar

**Зачем.** В играх нет настоящего календаря: идут часы и дни недели, но нет числа и месяца. Календарь сопоставляет игровые дни с реальными датами, чтобы вы знали, **какое сегодня число, какой день недели и какой праздник** в стране, где стоит грузовик. Это нужно для запретов движения (они привязаны к воскресеньям, праздникам и летним субботам), для тахографа (неделя считается по календарю) и просто для атмосферы.

**Как работает.** У каждого профиля игры своя дата: при первом запуске она берётся из `new_profile_date` в настройках (по умолчанию 1 июля), потом идёт сама вместе с игровым временем. День недели в календаре всегда совпадает с игрой. Время показывается **по часовому поясу той страны или штата, где сейчас грузовик** (страна определяется по ближайшему городу). Праздники страны — оранжевые числа, красные рамки — дни запрета движения грузовиков (ETS2). **Праздники.** В ETS2 — праздники стран Европы (с названиями, в том числе религиозные, от Пасхи). В ATS — **федеральные праздники США и праздники отдельных штатов**: например, День независимости Техаса и Сан-Хасинто в Техасе, Nevada Day в Неваде, Pioneer Day в Юте, Mardi Gras в Луизиане, César Chávez Day в Калифорнии. Названия — на языке игры (9 языков), в календаре — оранжевые числа и список под сеткой. Если праздник выпадает на выходной, перенос на понедельник/пятницу не учитывается. Месяцы листаются **поворотниками** (левый — назад, правый — вперёд). Когда наступает другое время года, на экране появляется сообщение — например, чтобы подключить зимний мод на графику.

**What it is for.** The games have no real calendar: you get the clock and the weekday, but no day of the month. The calendar maps game days to real dates so you know the date, weekday and **public holiday in the country where your truck is**. Bans, the tachograph week and seasons all depend on it. Each profile has its own date; the time follows the **time zone of the country/state you are in**. Months are flipped with the turn signals. Holidays: European national holidays in ETS2; US federal and state-specific holidays in ATS (e.g. Texas Independence Day, Nevada Day, Pioneer Day in Utah, Mardi Gras in Louisiana); an observed day off when a holiday falls on a weekend is not modelled.

## Тахограф ЕС (ETS2) / EU tachograph (ETS2)

Упрощённая модель регламента ЕС 561/2006. Плагин считает «вождением» каждую игровую минуту, когда грузовик едет быстрее 1 км/ч, а всё остальное время (стоянка, сон, паром, поезд) — отдыхом.

| Режим / Rule | Лимит / Limit |
|---|---|
| Непрерывное вождение / Continuous driving | 4 ч 30 мин → перерыв 45 мин (или 15 + 30) |
| Дневное вождение / Daily driving | 9 ч, дважды в неделю можно 10 ч |
| Смена (окно) / Shift window | 24 ч минус ежедневный отдых: **13 ч** при отдыхе 11 ч или **15 ч** при сокращённом 9 ч (не более 3 раз между еженедельными); вождение 9/10 ч должно уместиться в смену |
| За неделю / Per week | 56 ч |
| За две недели подряд / Per two weeks | 90 ч |
| Ежедневный отдых / Daily rest | 11 ч (или 3 + 9), сокращённый 9 ч — не более 3 раз между еженедельными отдыхами |
| Еженедельный отдых / Weekly rest | 45 ч или сокращённый 24 ч (после сокращённого — обязательно 45 ч); начать не позже чем через 6 × 24 ч |

**Выключить тахограф** (в ATS — логбук HOS) можно кнопкой **Alt+F9**: счёт и сообщения отключаются, при включении счёт начинается заново; состояние запоминается, клавишу можно поменять в `DriverDesk.ini` (`tachograph_key`). В окне — полосы с остатком времени и подсказки «начать отдых до …». Если прошлый еженедельный отдых был **сокращённым (24 ч)**, плагин пишет «следующий — обязательно полный 45 ч», во время отдыха показывает, **сколько осталось до 45 часов**, а если игрок снова отдохнёт меньше 45 часов, выводит красное предупреждение. **Компенсация:** недобранное до 45 ч время нужно компенсировать **не позднее конца третьей недели** после недели сокращённого отдыха. Плагин считает долг, показывает «осталось H:MM до <срок>» (оранжевым, красным — если срок вышел) и зачитывает излишек отдыха: сверх 45 ч у еженедельного или сверх 11 ч у ежедневного отдыха (компенсацию можно присоединить к любому отдыху от 9 ч). На экране появляются сообщения: за 30 минут до перерыва, при превышении 4:30, за 1 час до конца окна смены, за 15 минут до конца дневной нормы, при превышении нормы, **за 1 час до еженедельного отдыха** (если прошлый был сокращённым — «обязательно 45 часов», иначе «можно 24 или 45 часов»; при долге по компенсации дописывается, сколько осталось), **за 1 час до лимита 56 часов в неделю и 90 часов за две недели** и когда пора на еженедельный отдых. В ATS (HOS) перерыв — за 30 минут, 11 часов вождения и окно 14 часов — за 1 час.

**Версия 1.1:** новые сообщения о перерыве, окне смены, еженедельном отдыхе и лимитах 56/90 часов (см. выше).

**Версия 1.2 — ещё по одному сообщению за 15 минут:** до обязательного перерыва (конец 4:30 вождения), до конца окна смены, до начала еженедельного отдыха и до лимита 56 часов в неделю. В ATS (HOS): до перерыва, до конца 11 часов вождения, до конца окна 14 часов и до конца цикла 70 часов (потом нужен отдых 34 часа). Каждое сообщение показывается один раз за эпизод. / **Version 1.2 — one more warning 15 minutes before:** the required break (end of 4:30 driving), the end of the shift window, the weekly rest and the 56 h weekly limit. ATS (HOS): the break, the end of 11 h driving, the end of the 14 h window and the end of the 70 h cycle (a 34 h restart is needed after it). Each warning is shown once per episode.

### Мини-окно тахографа (F9) / Mini tachograph window (F9)

Маленькое окно **слева вверху** экрана в стиле тахографа (тёмная панель, янтарные цифры). Открывается и закрывается клавишей **F9** (без Shift/Ctrl/Alt; клавишу можно поменять строкой `mini_window` в `DriverDesk.ini`). Показывает три времени:
- **До конца смены** — окно смены: ETS2 13 или 15 ч, ATS 14 ч;
- **До перерыва** — ETS2 до конца 4:30 вождения, ATS до конца 8 ч без перерыва;
- **До недельного отдыха** — ETS2: сколько осталось до конца 144 часов после прошлого еженедельного отдыха; ATS: сколько осталось из 70 часов за 8 дней (потом нужен отдых 34 часа).

Цифры янтарные, **оранжевые** при малом остатке (до конца смены и недельного отдыха — меньше часа, до перерыва — меньше 30 минут), **красные со знаком «−»**, если время вышло. Справа вверху — «в пути» или «отдых H:MM»; если тахограф выключен (Alt+F9) — «выключен». Подсказка про клавишу F9 есть и в большом окне, внизу.

A small tachograph-style window in the **top-left corner** of the screen (dark panel, amber digits), opened and closed with **F9** (no modifiers; change the `mini_window` line in `DriverDesk.ini`). It shows three times: **time to the end of the shift** (shift window: 13/15 h in ETS2, 14 h in ATS), **time to the break** (end of 4:30 of driving in ETS2, 8 h without a break in ATS) and **time to the weekly rest** (ETS2: what is left of the 144 h after the previous weekly rest; ATS: what is left of the 70 h in 8 days, after which a 34 h restart is needed). Digits turn orange when little time is left (under 1 h for the shift and the weekly rest, under 30 min for the break) and red with a minus sign when the time is over. The top-right shows "driving" or "rest H:MM", or "off" when the tachograph is switched off (Alt+F9).

### Нарушения режима труда и отдыха / Driving and rest time violations

В большом окне, **под показаниями тахографа** (в ATS — под логбуком HOS), три счётчика: нарушения **за смену, за неделю и за месяц** (красные, если больше нуля). Каждое превышение лимита считается **один раз за эпизод**, и только пока вы едете.
- **ETS2:** больше 4:30 без перерыва, превышение дневного вождения, выход за окно смены, больше 56 ч за неделю, больше 90 ч за две недели, еженедельный отдых не начат за 144 часа, снова сокращённый еженедельный отдых подряд, просрочена компенсация.
- **ATS:** больше 8 часов без перерыва, больше 11 часов вождения, выход за окно 14 часов, превышение цикла 70 часов.

Счётчик смены обнуляется новым ежедневным отдыхом (в ATS — отдыхом 10 часов), недельный — со сменой недели, месячный — со сменой месяца в календаре игры. Данные хранятся по профилям в `DriverDesk_data.ini`; при включении тахографа кнопкой Alt+F9 счётчики обнуляются.

Under the tachograph readings (under the HOS logbook in ATS) the main window shows three counters: violations **per shift, per week and per month** (red when above zero). Each exceeded limit is counted **once per episode**, and only while driving. ETS2: more than 4:30 without a break, the daily driving limit, the shift window, 56 h per week, 90 h per two weeks, the weekly rest not started within 144 h, a reduced weekly rest twice in a row, a missed compensation deadline. ATS: more than 8 h without a break, more than 11 h of driving, the 14 h window, the 70 h cycle. The shift counter resets with a new daily rest (10 h off duty in ATS), the weekly one when the week changes, the monthly one when the calendar month changes. The data is stored per profile in `DriverDesk_data.ini`; switching the tachograph on with Alt+F9 resets the counters.

## Логбук HOS (ATS) / HOS logbook (ATS)

Американский аналог тахографа (правила FMCSA для грузовиков, перевозящих грузы): **11 часов** вождения за смену, **окно смены 14 часов** (идёт без остановки с первой минуты вождения после отдыха), перерыв **30 минут** после 8 часов вождения, **70 часов за 8 дней**. **10 часов** отдыха сбрасывают смену, **34 часа** — цикл 70/8. Упрощено: в смене считается только вождение; режимы «спальное место» и «личное пользование» не учитываются.

The US counterpart of the tachograph (FMCSA rules for property-carrying drivers): 11 h driving per shift, a 14-hour shift window, a 30-min break after 8 h of driving, 70 h in 8 days; 10 h off duty resets the shift, 34 h resets the cycle. Sleeper-berth split and personal conveyance are not modelled.

## Запреты движения грузовиков (ETS2) / Truck driving bans (ETS2)

**Зачем.** В Европе в воскресенья, праздники, а в ряде стран и по субботам летом тяжёлым грузовикам (> 7,5 т) нельзя ехать — иначе штраф. Плагин показывает это заранее, чтобы вы не встали на границе на сутки.

**Как работает.** По положению грузовика плагин определяет страну и показывает в окне блоки:
- **«Здесь»** — запреты сегодня и завтра, ближайший запрет в ближайшие 14 дней, праздники с названиями («15.08 — Успение: 00:00–22:00»). Идущий сейчас запрет подсвечен красным.
- **«Соседние страны»** — кратко по каждой соседней стране (международный код: D, A, I, F, PL …): запрет сегодня и завтра. Пригодится заранее выбрать маршрут.
- **«Пункт назначения»** — запреты в стране, куда вы везёте груз.
- Календарь отмечает дни запрета красной рамкой; ежедневные ночные запреты (Швейцария, Австрия, Черногория) идут отдельной строкой.
- За **30 минут до начала запрета** приходит сообщение на экран.

Данные есть по **Германии, Австрии, Швейцарии, Франции, Италии, Польше, Чехии, Словакии, Венгрии, Словении, Хорватии, Люксембургу и Черногории**. Для Испании показываются праздники и отдельные участки в особые дни, для Португалии — только ADR, для Лондона — зона LLCS; в Великобритании, Болгарии и Румынии общих запретов нет.

### На какие грузы запрет не распространяется / Cargo exemptions

В большинстве стран запрет **не касается** части грузов. Плагин смотрит, какой груз у вас в рейсе, и **в окне Shift+F9 (в разделе «Запреты движения», в любой стране, в том числе там, где запретов нет, например в Испании) пишет одну строку**: «Груз: Говядина — запрет не действует» (зелёная) или «Груз: Игрушки — запрет действует» (красная). Если груз из списка исключений, **сообщения о запрете не приходят** — вы едете без лишних отвлечений.

Освобождены (в игре около 80 грузов) в первую очередь:
- **скоропортящиеся продукты** — мясо, птица, рыба, молочные продукты и сыры, овощи, фрукты, цветы, вакцины;
- **молоко** (в цистерне и в тенте);
- **живые животные** — скот, свиньи, овцы;
- **опасные грузы (ADR)** — топливо, газы, кислоты и щёлочи, химикаты, взрывчатка, медицинские отходы и т. п.;
- **почта и авиапочта**.

Исключение — **Швейцария**: там запрет действует **для всех грузов без исключений** (нужно особое разрешение), поэтому для неё плагин всегда показывает «запрет для всех грузов».

Список — по правилам стран и отметкам автора, он упрощён: на деле условия зависят от страны, веса груза и пробега.

---

## Путевой журнал и ТО / Trip log and maintenance

- **Журнал:** каждый рейс — груз, откуда и куда, расстояние, доход, штрафы, платные дороги и паромы, расход топлива (л/100 км или миль/галлон) за рейс и за месяц, порожний пробег. В ATS — мили и галлоны США.
- **ТО по пробегу:** масло и фильтры, топливный и воздушный фильтры, тормоза, масло КПП и мостов, годовой техосмотр (в ETS2 ещё поверка тахографа). Счётчик сбрасывается, когда вы ремонтируете износ детали в сервисе.

## Установка / Installation

1. Скопируйте `DriverDesk.dll` в папку / Copy `DriverDesk.dll` to:
   - ETS2: `…\Euro Truck Simulator 2\bin\win_x64\plugins\`
   - ATS: `…\American Truck Simulator\bin\win_x64\plugins\`

   Если папки `plugins` нет — создайте. Один и тот же файл подходит для обеих игр / The same file works for both games.
2. Запустите игру. Рядом появятся `DriverDesk.ini` (настройки) и `DriverDesk_data.ini` (ваши данные: журнал, тахограф, ТО).
3. Нажмите **Shift+F9** / Press **Shift+F9**.

**Steam Deck:** та же папка `bin/win_x64/plugins`, запуск игры через Proton (не родная Linux-версия).

**Удаление / Uninstall:** удалите три файла `DriverDesk*` из `plugins`.

## Важно / Notes

- Только одиночная игра, **не для TruckersMP**. / Single-player only, **not for TruckersMP**.
- Игра не передаёт плагину погоду и страну: страна определяется по ближайшему городу, у границы возможны ошибки. / The country is taken from the nearest city, so it may be wrong near a border.
- Запреты, праздники, исключения и правила тахографа и HOS **упрощены и ориентировочны**, это не юридическая консультация. / Bans, holidays, exemptions, tachograph and HOS rules are **simplified and approximate**; this is not legal advice.
- DLL не подписана цифровой подписью, антивирус может предупредить о неизвестном файле. / The DLL is not code-signed; an antivirus may warn about it.

## Настройки / Settings (`DriverDesk.ini`)

Клавиши (`open`, `mini_window`, `tachograph_key`), размер и положение окна, поправка времени, дата начала для нового профиля — в файле каждая строка с пояснением. / Keys (`open`, `mini_window`, `tachograph_key`), window size and position, time shift, start date for a new profile — every line is commented.

## Лицензия / License

© Staryi, все права защищены. Исходный код закрыт; распространять DLL можно только в неизменном виде со ссылкой на эту страницу. Это неофициальный плагин, он не связан с SCS Software. / All rights reserved. The source code is not published; the DLL may be redistributed only unmodified, with a link to this page. Unofficial plugin, not affiliated with SCS Software.
