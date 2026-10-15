# Storm Editor

Окна инструментов встроенного редактора [Storm Engine](https://github.com/POMXARK/storm-engine) (ветка `miniaudio-sound`). Движок запускается с `--editor` и открывает рабочее место на ImGui: игра в окне Viewport, вокруг панели иерархии, инспектора, ассетов и консоли, сверху Pause/Step и свободная камера (сама раскладка — в движке, `src/libs/editor`). Работает на Windows и Linux (d3d9 через dxvk-native).

![Рабочее место](media/workspace.jpg)

![Инспектор атрибутов](media/inspector.jpg)

## Окна

| Окно | Что делает |
|---|---|
| Entities | все сущности движка с классом, фильтр по классу |
| Entity Properties | слои и приоритеты выбранной сущности, дерево её атрибутов с правкой на лету: числа тянутся мышью, цвета `argb` — палитрой, текст — по Enter; правка доходит до сущности через `AttributeChanged`, как присваивание из скрипта |
| Gizmo | двигает и поворачивает выбранную модель или локатор локации (ветка **Locators** в Entity Properties) гизмо ImGuizmo в кадре или числами, оси модели или мира, шаг; **Copy as script** — строка `MSG_MODEL_SET_POSITION` для скрипта (в буфер и в `compile.log`) |
| Console | логи движка по вкладкам: system, error (красная, если не пуст), compile (скрипты и Trace); фильтр, «Errors only», Clear |
| Assets | дерево `resource/` и `program/` с типом и размером, текст .ini/.ani/.c/.fx справа |
| Animation | анимация выбранной модели или персонажа (`EngineEditor::ModelOf`): проигрыватели с действием, позицией, скоростью и Play/Pause/Stop; действия .ani полосами на кадрах .an с событиями и данными, щелчок играет действие; граф переходов, увиденных пока окно открыто |

![Гизмо](media/gizmo.jpg)

![Гизмо на локаторе](media/gizmo-locator.jpg)

Для локатора **Copy as script** даёт метку генератора (`("item", "crate2", (x, y, z), (dx, dz)),`). Свой объект для гизмо модуль движка отдаёт через `EngineEditor::SetGizmoTarget` — так это делает `Location::ShowEditor`.

![Анимация](media/animation.jpg)

Окна, которые рисуют в сцене, берут кадр игры из `EngineEditor::GetViewportRect()` и `GetViewportDrawList()`: в рабочем месте это окно Viewport, в оверлее — весь экран.

Модуль, у сущности которого модель лежит внутри (персонаж), сообщает о ней через `EngineEditor::RegisterModelOf`, тогда Animation и другие окна находят модель по выбранной сущности.

Дальше: окна игровых данных.

## Подключение

Репозиторий — submodule движка в `src/libs/editor_tools`, собирается как его модуль:

```bash
git submodule update --init src/libs/editor_tools
cmake ../storm-engine -G Ninja -DCMAKE_BUILD_TYPE=Debug     # -DSTORM_EDITOR_TOOLS=OFF — без окон
```

Без submodule (или с `STORM_EDITOR_TOOLS=OFF`) движок собирается и работает как прежде, в оверлее остаются меню Debug и окна самих модулей.

## Своё окно

Модули движка линкуются целиком, поэтому окно регистрирует себя само:

```cpp
#include "storm/editor/engine_editor.hpp"

namespace
{
void ShowMyTool(bool &active)
{
    if (ImGui::Begin("My Tool", &active))
    {
        // ...
    }
    ImGui::End();  // и для свёрнутого окна
}

[[maybe_unused]] const bool registered = [] {
    storm::editor::EngineEditor::RegisterEditorTool("My Tool", ShowMyTool);
    return true;
}();
}
```

Состояние между запусками — в `imgui.ini` рабочей папки, секция `[Storm][Editor]`: открытые окна (`Open=`) и значения окон (`EngineEditor::SetSetting/GetSetting`, например `Entities.Class=`, `Entities.Index=`, `Gizmo.Operation=`, `Animation.Filter=`). Так редактор проверяется без нажатий: тест кладёт `imgui.ini` и снимает окно.

Движок отдаёт редактору только общий API `core` (`ForEachEntity`, `GetClassCode`, `GetLayerPriority`, атрибуты сущностей) и `Entity::ShowEditor()` — поля, которые класс показывает сам.

## Лицензия

GPL-3.0, как и движок. ImGuizmo (MIT) приходит из Conan движка.
