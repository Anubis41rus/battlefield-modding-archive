// WebUiPresent.h — ★ 231: меню модов (WebUI) в кадре игры, как VU (Gameface рисует в кадр движка).
//
// В полноэкранном эксклюзивном режиме BF3 кадр идёт на экран мимо композиции Windows — дочернее окно меню
// (DirectComposition) не видно (прогоны 229–230). Поэтому: перехват IDXGISwapChain::Present (замена в таблице методов
// swap chain DXGI), картинка страницы (CapturePreview WebView2 → 32bppPBGRA) — текстурой поверх кадра игры перед Present.
// В окне меню по-прежнему рисует DirectComposition; в кадре — только когда swap chain в полноэкранном режиме
// (или EVBOTS_WEBUI_INFRAME=1 — всегда, для проверки). EVBOTS_WEBUI_INFRAME=0 — не перехватывать Present.
#pragma once

#include <cstdint>
#include <vector>

namespace evbots {
namespace webui_present {

/// Перехват Present (один раз). Только в клиенте, из потока WebUI (не из DllMain). false — не встал (в журнале — почему).
bool Install();

/// Рисовать ли меню в кадре: swap chain игры сейчас полноэкранный (или режим принудительно включён).
bool Wanted();

/// Слой (фрейм мода) — картинка 32bppPBGRA w×h, z — порядок (больше — выше). Пустой pixels — слой скрыт.
void SetLayer(int id, int z, int w, int h, std::vector<uint8_t>&& pixels);
void HideLayer(int id);

/// Для журнала: вызовов Present через перехват, кадров с меню.
void Stats(long* presents, long* drawn);

}  // namespace webui_present
}  // namespace evbots
