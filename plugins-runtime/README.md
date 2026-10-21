# Плагины exteraGram в AyuGram Desktop

Эта ветка (`plugins`) добавляет в AyuGram Desktop движок плагинов из exteraGram:
те же `.plugin`-файлы на Python и тот же SDK, что на Android.

Код написан с помощью ИИ (Claude Code).

## Как пользоваться

1. Нужен Python 3.10+. Сборка из GitHub Actions уже кладёт его рядом
   (`python\python.exe`), иначе используется установленный `py -3` / `python`.
2. Откройте `.plugin`-файл в любом чате и нажмите **«Установить плагин»**,
   или: Настройки → AyuGram → **Плагины** → «Установить из файла…».
3. Там же: включение/выключение, безопасный режим, настройки плагина, журнал,
   перезагрузка и удаление.

Плагины лежат в `<папка AyuGram>\plugins`, их данные — в `plugins_data`,
состояние (вкл/выкл и настройки) — в `tdata\ayu_plugins.json`.

## Как это устроено

```
AyuGram.exe ──stdin/stdout, JSON──► python plugin_host.py ──► ваши .plugin
     │                                     │
     ├─ фильтр MTProto-запросов            └─ sdk/: base_plugin, client_utils,
     ├─ фильтр апдейтов                        hook_utils, ui.*, org.telegram.*
     ├─ хук отправки сообщений                 tl.py — TL-кодек по api.tl клиента
     └─ меню, настройки, диалоги
```

Хуки получают сырые TL-байты, а `tl.py` превращает их в объекты `TLRPC` по
`api.tl` самого клиента. Поэтому имена классов и полей совпадают с Android:
`TLRPC.TL_messages_setTyping`, `request.peer`, `update.message.message` и т.д.

## Что работает

| Android API | Desktop |
|---|---|
| `add_hook("TL_…")`, `pre_request_hook`, `post_request_hook` | да, CANCEL / MODIFY / MODIFY_FINAL |
| `on_update_hook`, `on_updates_hook` | да |
| `on_send_message_hook` | да (текст, тихая отправка, отложка) |
| `send_request(TLRPC.…, callback)` | да, через MTProto-соединение клиента |
| `send_message` / `send_text` | да; `parse_mode` html и markdown |
| `edit_message` | текст — да, медиа — нет |
| `add_menu_item` | меню сообщения, меню чата/профиля, главное меню |
| `create_settings` | Header, Divider, Switch, Selector, Input, EditText, Text, вложенные страницы |
| `AlertDialogBuilder`, `BulletinHelper` | да |
| `get_setting` / `set_setting` | да |

## Чего нет

- Код, который трогает Java/Android напрямую (`hook_method`, Xposed,
  `find_class`, View, Activity) — на Windows такого нет.
- Отправка файлов из плагина (`send_photo`, `send_document`) — пока нет.
- Строки настроек `Custom` (из Android View) показываются заглушкой.

## Сборка

`.github/workflows/win-plugins.yml` собирает Windows x64 (Debug) при пуше в
ветку `plugins`. Первый запуск с `only_cache: true` собирает зависимости
(Qt, FFmpeg, …) и кладёт их в кэш, следующие сборки идут быстрее. Артефакт —
`AyuGram-plugins-x64-debug`: `AyuGram.exe`, `plugins-runtime\`, `python\`.

## Тесты

```bash
python plugins-runtime/tests/test_tl.py
python plugins-runtime/tests/test_host.py
python plugins-runtime/tests/test_markup.py
```
