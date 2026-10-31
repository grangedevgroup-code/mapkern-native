#include "mapkern/mapkern.h"

#ifdef _WIN32

#include <d3d11.h>
#include <dxgi1_2.h>
#include <windows.h>

#include <mutex>

namespace {

struct Device {
  ID3D11Device* device = nullptr;
  ID3D11DeviceContext* context = nullptr;
  IDXGIFactory2* factory = nullptr;
};

Device& shared() {
  static Device d;
  static std::once_flag once;
  std::call_once(once, [] {
    D3D_FEATURE_LEVEL level;
    const UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, nullptr, 0, D3D11_SDK_VERSION, &d.device, &level, &d.context);
    if (FAILED(hr)) hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags, nullptr, 0, D3D11_SDK_VERSION, &d.device, &level, &d.context);
    if (FAILED(hr)) return;
    IDXGIDevice* dxgi = nullptr;
    IDXGIAdapter* adapter = nullptr;
    if (SUCCEEDED(d.device->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dxgi))) && SUCCEEDED(dxgi->GetAdapter(&adapter)))
      adapter->GetParent(__uuidof(IDXGIFactory2), reinterpret_cast<void**>(&d.factory));
    if (adapter) adapter->Release();
    if (dxgi) dxgi->Release();
  });
  return d;
}

template <typename T>
void release(T*& p) {
  if (p) p->Release();
  p = nullptr;
}

}

struct mk_view {
  HWND hwnd = nullptr;
  mk_map* map = nullptr;
  IDXGISwapChain1* swapchain = nullptr;
  ID3D11RenderTargetView* rtv = nullptr;
  ID3D11Texture2D* depth = nullptr;
  ID3D11DepthStencilView* dsv = nullptr;
  int width = 0, height = 0;
  double ratio = 1;

  void dropTargets() {
    release(rtv);
    release(dsv);
    release(depth);
  }

  bool targets() {
    if (rtv || !swapchain || width <= 0 || height <= 0) return rtv != nullptr;
    Device& d = shared();
    ID3D11Texture2D* back = nullptr;
    if (FAILED(swapchain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&back)))) return false;
    d.device->CreateRenderTargetView(back, nullptr, &rtv);
    back->Release();
    D3D11_TEXTURE2D_DESC td{};
    td.Width = static_cast<UINT>(width);
    td.Height = static_cast<UINT>(height);
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    if (SUCCEEDED(d.device->CreateTexture2D(&td, nullptr, &depth))) d.device->CreateDepthStencilView(depth, nullptr, &dsv);
    return rtv && dsv;
  }
};

extern "C" {

mk_view* mk_view_create_hwnd(void* hwnd, const mk_options* options) {
  Device& d = shared();
  if (!d.device || !d.factory || !hwnd) return nullptr;
  auto* v = new mk_view();
  v->hwnd = static_cast<HWND>(hwnd);
  RECT rc{};
  GetClientRect(v->hwnd, &rc);
  v->width = std::max<int>(1, rc.right - rc.left);
  v->height = std::max<int>(1, rc.bottom - rc.top);
  v->ratio = options && options->pixel_ratio > 0 ? options->pixel_ratio : GetDpiForWindow(v->hwnd) / 96.0;
  DXGI_SWAP_CHAIN_DESC1 sd{};
  sd.Width = static_cast<UINT>(v->width);
  sd.Height = static_cast<UINT>(v->height);
  sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  sd.SampleDesc.Count = 1;
  sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  sd.BufferCount = 2;
  sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
  sd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
  if (FAILED(d.factory->CreateSwapChainForHwnd(d.device, v->hwnd, &sd, nullptr, nullptr, &v->swapchain))) {
    delete v;
    return nullptr;
  }
  d.factory->MakeWindowAssociation(v->hwnd, DXGI_MWA_NO_ALT_ENTER);
  mk_options o;
  if (options) o = *options;
  else mk_options_default(&o);
  o.pixel_ratio = v->ratio;
  v->map = mk_map_create(&o);
  mk_graphics g{};
  g.d3d11_device = d.device;
  g.d3d11_device_context = d.context;
  g.color_format = MK_FORMAT_BGRA8;
  g.depth_format = MK_FORMAT_DEPTH_STENCIL;
  g.sample_count = 1;
  mk_map_init_graphics(v->map, &g);
  mk_map_resize(v->map, v->width / v->ratio, v->height / v->ratio, v->ratio);
  return v;
}

mk_map* mk_view_map(mk_view* v) { return v ? v->map : nullptr; }

void mk_view_resize(mk_view* v, int w, int h, double ratio) {
  if (!v || w <= 0 || h <= 0) return;
  if (ratio > 0) v->ratio = ratio;
  if (w != v->width || h != v->height) {
    v->dropTargets();
    shared().context->OMSetRenderTargets(0, nullptr, nullptr);
    v->swapchain->ResizeBuffers(0, static_cast<UINT>(w), static_cast<UINT>(h), DXGI_FORMAT_UNKNOWN, 0);
    v->width = w;
    v->height = h;
  }
  mk_map_resize(v->map, w / v->ratio, h / v->ratio, v->ratio);
}

int mk_view_render(mk_view* v) {
  if (!v || !v->targets()) return 0;
  mk_surface s{};
  s.width = v->width;
  s.height = v->height;
  s.sample_count = 1;
  s.color_format = MK_FORMAT_BGRA8;
  s.depth_format = MK_FORMAT_DEPTH_STENCIL;
  s.d3d11_render_view = v->rtv;
  s.d3d11_depth_stencil_view = v->dsv;
  const int more = mk_map_render(v->map, &s);
  v->swapchain->Present(1, 0);
  return more;
}

void mk_view_destroy(mk_view* v) {
  if (!v) return;
  mk_map_destroy(v->map);
  v->dropTargets();
  release(v->swapchain);
  delete v;
}

}

#else

extern "C" {
mk_view* mk_view_create_hwnd(void*, const mk_options*) { return nullptr; }
mk_map* mk_view_map(mk_view*) { return nullptr; }
void mk_view_resize(mk_view*, int, int, double) {}
int mk_view_render(mk_view*) { return 0; }
void mk_view_destroy(mk_view*) {}
}

#endif
