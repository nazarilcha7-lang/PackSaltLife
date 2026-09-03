# HeliosAuraPack — ресурспак для «Ауры Гелиоса»

Текстура 16x16 в золотисто-оранжевой палитре: осколок солнца со светлым ядром
и двумя искрами по диагонали.

## Структура

```
HeliosAuraPack/
├── pack.mcmeta                                     # версия формата пака
├── pack.png                                        # иконка пака
└── assets/heliosaura/
    ├── items/helios_aura.json                      # определение модели предмета (1.21.4+)
    ├── models/item/helios_aura.json                # сама модель
    └── textures/item/helios_aura.png               # текстура 16x16
```

Идентификатор модели: `heliosaura:helios_aura` — ровно то, что стоит в
`config.yml` плагина в поле `item.item-model`.

## Как подключить на сервере

1. Запаковать содержимое папки `HeliosAuraPack` в zip. **Важно:** `pack.mcmeta`
   и `assets` должны лежать в корне архива, а не внутри вложенной папки.
2. Залить zip на прямую ссылку (Dropbox с `?dl=1`, GitHub Releases, любой
   файлхостинг с прямой отдачей файла).
3. Взять SHA-1 архива:
   - Windows: `certutil -hashfile HeliosAuraPack.zip SHA1`
   - Linux: `sha1sum HeliosAuraPack.zip`
4. В `server.properties` **Velocity-бэкенда** (или в конфиге прокси, если
   раздаёте пак оттуда):

```properties
resource-pack=https://прямая-ссылка/HeliosAuraPack.zip
resource-pack-sha1=полученный_хеш
require-resource-pack=false
resource-pack-prompt=Текстуры SaltLife
```

Пак раздаётся с Velocity один раз на вход, чтобы игрок не перекачивал его при
каждом переходе между hub и an1.

## Как проверить локально

Положить папку (или zip) в `%appdata%\.minecraft\resourcepacks`, включить в
настройках, зайти на сервер, взять предмет через `/aura`. Должна появиться
золотая текстура вместо незеритовой звезды.

## Клиенты до 1.21.4

`item-model` работает только на 1.21.4 и новее. Для старых клиентов нужен
устаревший способ через `custom_model_data`:

1. В `config.yml` плагина: `item-model: ""` и `custom-model-data: 1`.
2. Добавить в пак файл `assets/minecraft/models/item/nether_star.json`:

```json
{
  "parent": "minecraft:item/generated",
  "textures": { "layer0": "minecraft:item/nether_star" },
  "overrides": [
    {
      "predicate": { "custom_model_data": 1 },
      "model": "heliosaura:item/helios_aura"
    }
  ]
}
```

3. `pack_format` в `pack.mcmeta` поменять на значение своей версии клиента
   (для 1.20.x это 15, для 1.21–1.21.1 — 34).

Если у вас через ViaVersion сидят игроки разных версий, проще оставить
незеритовую звезду без своей модели, чем поддерживать два пака.

## Как перерисовать текстуру

Файл `textures/item/helios_aura.png` — обычный PNG 16x16 с прозрачностью.
Открывается в Aseprite, Paint.NET, Piskel или любом пиксельном редакторе.
Использованная палитра:

| Цвет | Роль |
|---|---|
| `#6B3A0E` | контур |
| `#A85C13` | тень |
| `#D98324` | средний оранжевый |
| `#FFB347` | светлый оранжевый |
| `#FFD86B` | золото, искры |
| `#FFF6D8` | ядро |

Размер можно увеличить до 32x32 или 64x64 — Minecraft это принимает, менять в
JSON ничего не нужно.
