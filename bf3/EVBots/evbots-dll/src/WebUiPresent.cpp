// WebUiPresent.cpp — ★ 231: меню модов в кадре игры (перехват IDXGISwapChain::Present). Описание — WebUiPresent.h.
//
// Перехват — замена Present (индекс 8 таблицы методов IDXGISwapChain) в общей таблице класса swap chain DXGI: адрес
// таблицы берём у своего временного swap chain (окно 8×8), таблица общая для всех swap chain процесса, в том числе
// игры. Замена в таблице, а не в начале функции: если Present уже перехвачен в начале (оверлеи), наш вызов идёт в него.
// D3D11/DXGI грузим сами (GetProcAddress) — в импорте EVBots.dll их нет (сервер их не грузит).
// Рисование — как у оверлеев ImGui: состояние контекста игры сохраняется и возвращается целиком; свой треугольник
// (SV_VertexID, без буферов), смешивание premultiplied alpha. Шейдеры компилируются при первом кадре (d3dcompiler_47).
#include "WebUiPresent.h"

#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi.h>
#include <wrl.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

#include "EVBotsLog.h"
#include "InlineHook.h"

namespace evbots {
namespace webui_present {
namespace {

using Microsoft::WRL::ComPtr;
using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using D3DCompileFn = HRESULT(WINAPI*)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR, LPCSTR, UINT,
                                      UINT, ID3DBlob**, ID3DBlob**);

constexpr int kPresentSlot = 8;  // IUnknown 3 + IDXGIObject 4 + IDXGIDeviceSubObject 1 → Present

std::atomic<PresentFn> g_realPresent{nullptr};
std::atomic<bool> g_installed{false}, g_force{false}, g_fullscreen{false}, g_failed{false};
std::atomic<long> g_presents{0}, g_drawn{0};

struct Layer {
    int z = 0, w = 0, h = 0;
    std::shared_ptr<const std::vector<uint8_t>> px;  // ★ 239: кадр слоя забирает поток рендера без копии под замком
    unsigned seq = 0;
};
std::mutex g_gate;
std::map<int, Layer> g_layers;
unsigned g_seq = 0;

// ── только поток рендера (Present) ──
struct Tex {
    ComPtr<ID3D11Texture2D> tex;
    ComPtr<ID3D11ShaderResourceView> srv;
    int w = 0, h = 0, z = 0;
    unsigned seq = 0;
};
struct Gpu {
    ID3D11Device* dev = nullptr;  // сравнение «то же устройство»; держит ссылку ComPtr ниже
    ComPtr<ID3D11Device> hold;
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> ps;
    ComPtr<ID3D11SamplerState> sampler;
    ComPtr<ID3D11BlendState> blend;
    ComPtr<ID3D11RasterizerState> rs;
    ComPtr<ID3D11DepthStencilState> ds;
    std::map<int, Tex> tex;
    bool ready = false;
};
Gpu* g_gpu = nullptr;  // не освобождается при выгрузке: устройство игры к тому времени может быть уже разрушено

constexpr char kHlsl[] =
    "struct VO { float4 pos : SV_Position; float2 uv : TEXCOORD0; };\n"
    "VO vs(uint id : SV_VertexID) { VO o; float2 uv = float2(id & 1, id >> 1); o.uv = uv;\n"
    "  o.pos = float4(uv.x * 2 - 1, 1 - uv.y * 2, 0, 1); return o; }\n"
    "Texture2D t : register(t0); SamplerState s : register(s0);\n"
    "float4 ps(VO i) : SV_Target { return t.Sample(s, i.uv); }\n";

bool Compile(D3DCompileFn fn, const char* entry, const char* target, ComPtr<ID3DBlob>* out) {
    ComPtr<ID3DBlob> err;
    const HRESULT hr = fn(kHlsl, sizeof(kHlsl) - 1, "evbots-webui", nullptr, nullptr, entry, target, 0, 0, out->GetAddressOf(),
                          err.GetAddressOf());
    if (FAILED(hr)) {
        LOG_ERR("EVBots: WebUI в кадре: шейдер %s не собран (hr 0x%08lX): %s", entry, hr,
                err ? static_cast<const char*>(err->GetBufferPointer()) : "-");
        return false;
    }
    return true;
}

bool CreateGpu(Gpu& g) {
    HMODULE dc = ::LoadLibraryW(L"d3dcompiler_47.dll");
    if (dc == nullptr) dc = ::LoadLibraryW(L"d3dcompiler_43.dll");
    auto compile = dc ? reinterpret_cast<D3DCompileFn>(::GetProcAddress(dc, "D3DCompile")) : nullptr;
    if (compile == nullptr) {
        LOG_ERR("EVBots: WebUI в кадре: нет d3dcompiler_47/43 — меню в кадре не будет");
        return false;
    }
    ComPtr<ID3DBlob> vsb, psb;
    if (!Compile(compile, "vs", "vs_4_0", &vsb) || !Compile(compile, "ps", "ps_4_0", &psb)) return false;
    HRESULT hr = g.dev->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &g.vs);
    if (SUCCEEDED(hr)) hr = g.dev->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &g.ps);
    D3D11_SAMPLER_DESC sd{};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.ComparisonFunc = D3D11_COMPARISON_ALWAYS;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    if (SUCCEEDED(hr)) hr = g.dev->CreateSamplerState(&sd, &g.sampler);
    D3D11_BLEND_DESC bd{};
    auto& rt = bd.RenderTarget[0];
    rt.BlendEnable = TRUE;
    rt.SrcBlend = D3D11_BLEND_ONE;  // страница уже premultiplied (WIC 32bppPBGRA)
    rt.DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    rt.BlendOp = D3D11_BLEND_OP_ADD;
    rt.SrcBlendAlpha = D3D11_BLEND_ONE;
    rt.DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
    rt.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    if (SUCCEEDED(hr)) hr = g.dev->CreateBlendState(&bd, &g.blend);
    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    if (SUCCEEDED(hr)) hr = g.dev->CreateRasterizerState(&rd, &g.rs);
    D3D11_DEPTH_STENCIL_DESC dd{};
    dd.DepthEnable = FALSE;
    dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    dd.DepthFunc = D3D11_COMPARISON_ALWAYS;
    if (SUCCEEDED(hr)) hr = g.dev->CreateDepthStencilState(&dd, &g.ds);
    if (FAILED(hr)) {
        LOG_ERR("EVBots: WebUI в кадре: ресурсы D3D11 не созданы (hr 0x%08lX)", hr);
        return false;
    }
    LOG_INFO("EVBots: WebUI в кадре: ресурсы D3D11 готовы (уровень устройства 0x%X)",
             static_cast<unsigned>(g.dev->GetFeatureLevel()));
    return true;
}

/// Слои → текстуры (только изменившиеся). ★ 239: под замком — только снимок слоёв (указатели на кадры); Map/копия в
/// текстуру — без замка. Раньше всё шло под g_gate внутри SEH (DrawGuarded): исключение в Map оставляло мьютекс занятым
/// навсегда → поток меню вставал в SetLayer, а с ним окно игры (оверлей — его дочернее окно). Кандидат зависания клиента
/// у друга 27.10 (гипотеза, не прогнано).
void Upload(Gpu& g, ID3D11DeviceContext* ctx) {
    struct Snap {
        int id, z, w, h;
        unsigned seq;
        std::shared_ptr<const std::vector<uint8_t>> px;
    };
    std::vector<Snap> snap;
    {
        std::lock_guard<std::mutex> lock(g_gate);
        snap.reserve(g_layers.size());
        for (const auto& [id, l] : g_layers) snap.push_back(Snap{id, l.z, l.w, l.h, l.seq, l.px});
    }
    for (auto it = g.tex.begin(); it != g.tex.end();) {
        const bool live = std::any_of(snap.begin(), snap.end(), [&](const Snap& x) { return x.id == it->first; });
        if (!live)
            it = g.tex.erase(it);
        else
            ++it;
    }
    for (const Snap& l : snap) {
        Tex& t = g.tex[l.id];
        t.z = l.z;
        if (t.seq == l.seq || !l.px) continue;
        if (t.w != l.w || t.h != l.h || !t.tex) {
            t.tex.Reset();
            t.srv.Reset();
            D3D11_TEXTURE2D_DESC td{};
            td.Width = static_cast<UINT>(l.w);
            td.Height = static_cast<UINT>(l.h);
            td.MipLevels = td.ArraySize = 1;
            td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            td.SampleDesc.Count = 1;
            td.Usage = D3D11_USAGE_DYNAMIC;
            td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            if (FAILED(g.dev->CreateTexture2D(&td, nullptr, &t.tex)) ||
                FAILED(g.dev->CreateShaderResourceView(t.tex.Get(), nullptr, &t.srv))) {
                t.tex.Reset();
                t.srv.Reset();
                continue;
            }
            t.w = l.w;
            t.h = l.h;
        }
        D3D11_MAPPED_SUBRESOURCE m{};
        if (FAILED(ctx->Map(t.tex.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) continue;
        const size_t row = static_cast<size_t>(l.w) * 4;
        const uint8_t* src = l.px->data();
        for (int y = 0; y < l.h; ++y)
            std::memcpy(static_cast<uint8_t*>(m.pData) + static_cast<size_t>(y) * m.RowPitch, src + y * row, row);
        ctx->Unmap(t.tex.Get(), 0);
        t.seq = l.seq;
    }
}

/// Состояние контекста игры, которое трогаем (как imgui_impl_dx11).
struct Backup {
    UINT vpN = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    D3D11_VIEWPORT vp[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
    ID3D11RasterizerState* rs = nullptr;
    ID3D11BlendState* bs = nullptr;
    FLOAT bf[4]{};
    UINT sm = 0;
    ID3D11DepthStencilState* dss = nullptr;
    UINT sref = 0;
    ID3D11ShaderResourceView* srv = nullptr;
    ID3D11SamplerState* samp = nullptr;
    ID3D11PixelShader* ps = nullptr;
    ID3D11VertexShader* vs = nullptr;
    ID3D11GeometryShader* gs = nullptr;
    ID3D11HullShader* hs = nullptr;
    ID3D11DomainShader* dsh = nullptr;
    ID3D11ClassInstance* psI[256]{};
    ID3D11ClassInstance* vsI[256]{};
    ID3D11ClassInstance* gsI[256]{};
    UINT psN = 256, vsN = 256, gsN = 256;
    D3D11_PRIMITIVE_TOPOLOGY top{};
    ID3D11InputLayout* il = nullptr;
    ID3D11RenderTargetView* rtv[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
    ID3D11DepthStencilView* dsv = nullptr;

    void Save(ID3D11DeviceContext* c) {
        c->RSGetViewports(&vpN, vp);
        c->RSGetState(&rs);
        c->OMGetBlendState(&bs, bf, &sm);
        c->OMGetDepthStencilState(&dss, &sref);
        c->PSGetShaderResources(0, 1, &srv);
        c->PSGetSamplers(0, 1, &samp);
        c->PSGetShader(&ps, psI, &psN);
        c->VSGetShader(&vs, vsI, &vsN);
        c->GSGetShader(&gs, gsI, &gsN);
        c->HSGetShader(&hs, nullptr, nullptr);
        c->DSGetShader(&dsh, nullptr, nullptr);
        c->IAGetPrimitiveTopology(&top);
        c->IAGetInputLayout(&il);
        c->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtv, &dsv);
    }
    template <class T>
    static void Rel(T*& p) {
        if (p) p->Release();
        p = nullptr;
    }
    void Restore(ID3D11DeviceContext* c) {
        c->RSSetViewports(vpN, vp);
        c->RSSetState(rs);
        c->OMSetBlendState(bs, bf, sm);
        c->OMSetDepthStencilState(dss, sref);
        c->PSSetShaderResources(0, 1, &srv);
        c->PSSetSamplers(0, 1, &samp);
        c->PSSetShader(ps, psI, psN);
        c->VSSetShader(vs, vsI, vsN);
        c->GSSetShader(gs, gsI, gsN);
        c->HSSetShader(hs, nullptr, 0);
        c->DSSetShader(dsh, nullptr, 0);
        c->IASetPrimitiveTopology(top);
        c->IASetInputLayout(il);
        c->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtv, dsv);
        Rel(rs);
        Rel(bs);
        Rel(dss);
        Rel(srv);
        Rel(samp);
        Rel(ps);
        Rel(vs);
        Rel(gs);
        Rel(hs);
        Rel(dsh);
        Rel(il);
        Rel(dsv);
        for (UINT i = 0; i < psN && i < 256; ++i) Rel(psI[i]);
        for (UINT i = 0; i < vsN && i < 256; ++i) Rel(vsI[i]);
        for (UINT i = 0; i < gsN && i < 256; ++i) Rel(gsI[i]);
        for (auto& r : rtv) Rel(r);
    }
};

void Draw(IDXGISwapChain* sc) {
    {
        std::lock_guard<std::mutex> lock(g_gate);
        if (g_layers.empty()) {
            if (g_gpu != nullptr) g_gpu->tex.clear();
            return;
        }
    }
    ComPtr<ID3D11Device> dev;
    if (FAILED(sc->GetDevice(IID_PPV_ARGS(&dev)))) return;  // не D3D11 (чужой swap chain)
    if (g_gpu == nullptr) g_gpu = new Gpu();
    Gpu& g = *g_gpu;
    if (g.dev != dev.Get()) {
        *g_gpu = Gpu();
        g.hold = dev;
        g.dev = dev.Get();
        g.ready = CreateGpu(g);
        if (!g.ready) g_failed = true;
    }
    if (!g.ready) return;
    ComPtr<ID3D11DeviceContext> ctx;
    dev->GetImmediateContext(&ctx);
    Upload(g, ctx.Get());
    std::vector<std::pair<int, ID3D11ShaderResourceView*>> order;
    for (auto& [id, t] : g.tex)
        if (t.srv) order.emplace_back(t.z, t.srv.Get());
    if (order.empty()) return;
    std::sort(order.begin(), order.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    ComPtr<ID3D11Texture2D> bb;
    if (FAILED(sc->GetBuffer(0, IID_PPV_ARGS(&bb)))) return;
    D3D11_TEXTURE2D_DESC bd{};
    bb->GetDesc(&bd);
    ComPtr<ID3D11RenderTargetView> rtv;
    if (FAILED(dev->CreateRenderTargetView(bb.Get(), nullptr, &rtv))) return;
    Backup b;
    b.Save(ctx.Get());
    D3D11_VIEWPORT vp{0, 0, static_cast<float>(bd.Width), static_cast<float>(bd.Height), 0, 1};
    ctx->RSSetViewports(1, &vp);
    ctx->RSSetState(g.rs.Get());
    const float bf[4]{};
    ctx->OMSetBlendState(g.blend.Get(), bf, 0xFFFFFFFFu);
    ctx->OMSetDepthStencilState(g.ds.Get(), 0);
    ID3D11RenderTargetView* rt = rtv.Get();
    ctx->OMSetRenderTargets(1, &rt, nullptr);
    ctx->IASetInputLayout(nullptr);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    ctx->VSSetShader(g.vs.Get(), nullptr, 0);
    ctx->PSSetShader(g.ps.Get(), nullptr, 0);
    ctx->GSSetShader(nullptr, nullptr, 0);
    ctx->HSSetShader(nullptr, nullptr, 0);
    ctx->DSSetShader(nullptr, nullptr, 0);
    ID3D11SamplerState* s = g.sampler.Get();
    ctx->PSSetSamplers(0, 1, &s);
    for (auto& [z, srv] : order) {
        ctx->PSSetShaderResources(0, 1, &srv);
        ctx->Draw(4, 0);
    }
    b.Restore(ctx.Get());
    if (++g_drawn == 1)
        LOG_INFO("EVBots: WebUI в кадре: первый кадр с меню (буфер %ux%u, формат %d, слоёв %u)", bd.Width, bd.Height,
                 static_cast<int>(bd.Format), static_cast<unsigned>(order.size()));
}

void DrawGuarded(IDXGISwapChain* sc) {
    __try {
        Draw(sc);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_failed = true;  // больше не рисуем: кадр игры важнее меню
    }
}

HRESULT STDMETHODCALLTYPE HookPresent(IDXGISwapChain* sc, UINT sync, UINT flags) {
    const long n = ++g_presents;
    if ((flags & DXGI_PRESENT_TEST) == 0 && !g_failed.load()) {
        if (n % 30 == 1) {
            BOOL fs = FALSE;
            const bool was = g_fullscreen.load();
            const bool now = SUCCEEDED(sc->GetFullscreenState(&fs, nullptr)) && fs != FALSE;
            g_fullscreen = now;
            if (now != was) LOG_INFO("EVBots: WebUI в кадре: swap chain игры — %s", now ? "полноэкранный" : "в окне");
        }
        if (g_force.load() || g_fullscreen.load()) DrawGuarded(sc);
    }
    return g_realPresent.load()(sc, sync, flags);
}

}  // namespace

bool Install() {
    if (g_installed.load()) return true;
    wchar_t v[4] = L"";
    if (::GetEnvironmentVariableW(L"EVBOTS_WEBUI_INFRAME", v, 4) > 0) {
        if (v[0] == L'0') {
            LOG_INFO("EVBots: WebUI в кадре: выключено (EVBOTS_WEBUI_INFRAME=0)");
            return false;
        }
        if (v[0] == L'1') g_force = true;
    }
    HMODULE d3d = ::LoadLibraryW(L"d3d11.dll");
    auto create = d3d ? reinterpret_cast<PFN_D3D11_CREATE_DEVICE_AND_SWAP_CHAIN>(
                            ::GetProcAddress(d3d, "D3D11CreateDeviceAndSwapChain"))
                      : nullptr;
    if (create == nullptr) {
        LOG_ERR("EVBots: WebUI в кадре: нет D3D11CreateDeviceAndSwapChain — Present не перехвачен");
        return false;
    }
    HWND w = ::CreateWindowExW(0, L"STATIC", L"evbots-dxgi-probe", WS_POPUP, 0, 0, 8, 8, nullptr, nullptr,
                               ::GetModuleHandleW(nullptr), nullptr);
    DXGI_SWAP_CHAIN_DESC d{};
    d.BufferCount = 1;
    d.BufferDesc.Width = d.BufferDesc.Height = 8;
    d.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    d.OutputWindow = w;
    d.SampleDesc.Count = 1;
    d.Windowed = TRUE;
    d.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    ComPtr<IDXGISwapChain> sc;
    ComPtr<ID3D11Device> dev;
    ComPtr<ID3D11DeviceContext> ctx;
    HRESULT hr = E_FAIL;
    for (D3D_DRIVER_TYPE t : {D3D_DRIVER_TYPE_HARDWARE, D3D_DRIVER_TYPE_WARP}) {
        hr = create(nullptr, t, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &d, &sc, &dev, nullptr, &ctx);
        if (SUCCEEDED(hr)) break;
    }
    bool ok = false;
    if (SUCCEEDED(hr) && sc) {
        void** slot = *reinterpret_cast<void***>(sc.Get()) + kPresentSlot;
        // сборка 237: замена слота — ядром (ev-core). Оригинал запоминаем ДО замены: Present может прийти сразу.
        void* cur = *slot;
        g_realPresent = reinterpret_cast<PresentFn>(cur);
        if (PatchPointer(slot, reinterpret_cast<void*>(&HookPresent), cur, nullptr, "Present (WebUI в кадре)")) {
            ok = true;
            HMODULE owner = nullptr;
            ::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                 reinterpret_cast<LPCWSTR>(g_realPresent.load()), &owner);
            wchar_t mod[MAX_PATH] = L"?";
            if (owner) ::GetModuleFileNameW(owner, mod, MAX_PATH);
            LOG_INFO("EVBots: WebUI в кадре: Present перехвачен (таблица %p, было %p в %ls)%s", static_cast<void*>(slot),
                     reinterpret_cast<void*>(g_realPresent.load()), mod, g_force.load() ? " — всегда (INFRAME=1)" : "");
        }
    } else {
        LOG_ERR("EVBots: WebUI в кадре: временный swap chain не создан (hr 0x%08lX) — Present не перехвачен", hr);
    }
    sc.Reset();
    ctx.Reset();
    dev.Reset();
    if (w) ::DestroyWindow(w);
    g_installed = ok;
    return ok;
}

bool Wanted() { return g_installed.load() && !g_failed.load() && (g_force.load() || g_fullscreen.load()); }

void SetLayer(int id, int z, int w, int h, std::vector<uint8_t>&& pixels) {
    if (pixels.size() < static_cast<size_t>(w) * h * 4 || w <= 0 || h <= 0) return;
    std::lock_guard<std::mutex> lock(g_gate);
    Layer& l = g_layers[id];
    l.z = z;
    l.w = w;
    l.h = h;
    l.px = std::make_shared<const std::vector<uint8_t>>(std::move(pixels));
    l.seq = ++g_seq;
}

void HideLayer(int id) {
    std::lock_guard<std::mutex> lock(g_gate);
    g_layers.erase(id);
}

void Stats(long* presents, long* drawn) {
    if (presents) *presents = g_presents.load();
    if (drawn) *drawn = g_drawn.load();
}

}  // namespace webui_present
}  // namespace evbots
