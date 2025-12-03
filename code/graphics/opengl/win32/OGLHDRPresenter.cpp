#ifdef _WIN32

#include "graphics/opengl/win32/OGLHDRPresenter.h"

#include "graphics/2d.h"
#include "osapi/osapi.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <vector>

#include <windows.h>
#include <d3d11.h>
#include <dxgi1_6.h>

#include <glad/glad_wgl.h>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

namespace
{
template <typename T>
void safe_release(T*& ptr)
{
	if (ptr != nullptr) {
		ptr->Release();
		ptr = nullptr;
	}
}

constexpr float pqC1 = 3424.0f / 4096.0f;
constexpr float pqC2 = 2413.0f / 128.0f;
constexpr float pqC3 = 2392.0f / 128.0f;
constexpr float pqM1 = 2610.0f / 16384.0f;
constexpr float pqM2 = 2523.0f / 32.0f;

float encodePQ(float luminanceNits)
{
	float norm = std::max(luminanceNits, 0.0f) / 10000.0f;
	float numerator = pqC1 + pqC2 * std::pow(norm, pqM1);
	float denominator = 1.0f + pqC3 * std::pow(norm, pqM1);
	return std::pow(numerator / denominator, pqM2);
}
} // namespace

OGLHDRPresenter::OGLHDRPresenter() = default;

OGLHDRPresenter::~OGLHDRPresenter()
{
	shutdown();
}

bool OGLHDRPresenter::initialize(HWND hwnd, int width, int height)
{
	_hwnd = hwnd;
	_width = width;
	_height = height;

	if (!createD3D11Device()) {
		mprintf(("OpenGL HDR: Failed to create D3D11 device\n"));
		fflush(nullptr);
		return false;
	}

	if (!createSwapChain()) {
		mprintf(("OpenGL HDR: Failed to create DXGI swap chain\n"));
		fflush(nullptr);
		return false;
	}

	if (!setupHDRMetadata()) {
		mprintf(("OpenGL HDR: Failed to configure HDR metadata\n"));
		fflush(nullptr);
		return false;
	}

	if (!createBackBufferResources()) {
		mprintf(("OpenGL HDR: Failed to allocate back buffer helpers\n"));
		fflush(nullptr);
		return false;
	}

	if (!createPBOs()) {
		mprintf(("OpenGL HDR: Failed to allocate OpenGL readback buffers\n"));
		fflush(nullptr);
		return false;
	}

	_interopAvailable = setupWGLInterop();
	if (!_interopAvailable) {
		mprintf(("OpenGL HDR: WGL_NV_DX_interop unavailable, falling back to PBO readback\n"));
		fflush(nullptr);
	}

	_initialized = true;
	return true;
}

bool OGLHDRPresenter::resize(int width, int height)
{
	if (!_initialized || _swapChain == nullptr) {
		return false;
	}

	if (width == _width && height == _height) {
		return true;
	}

	_width = width;
	_height = height;
	_pboPrimed = false;

	destroyPBOs();
	destroyWGLInterop();
	safe_release(_stagingTexture);
	safe_release(_backBuffer);

	UINT flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING | DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
	HRESULT hr = _swapChain->ResizeBuffers(0, static_cast<UINT>(_width), static_cast<UINT>(_height), DXGI_FORMAT_UNKNOWN, flags);
	if (FAILED(hr)) {
		mprintf(("OpenGL HDR: ResizeBuffers failed (0x%08X)\n", hr));
		fflush(nullptr);
		return false;
	}

	if (!createBackBufferResources()) {
		mprintf(("OpenGL HDR: Failed to recreate back buffer after resize\n"));
		fflush(nullptr);
		return false;
	}

	if (!createPBOs()) {
		mprintf(("OpenGL HDR: Failed to recreate PBOs after resize\n"));
		fflush(nullptr);
		return false;
	}

	if (!setupHDRMetadata()) {
		mprintf(("OpenGL HDR: Failed to reapply HDR metadata after resize\n"));
		fflush(nullptr);
		return false;
	}

	_interopAvailable = setupWGLInterop();

	return true;
}

bool OGLHDRPresenter::presentFromGL(GLuint sourceTexture, int width, int height, bool vsync)
{
	if (!_initialized) {
		return false;
	}

	if (width != _width || height != _height) {
		if (!resize(width, height)) {
			updatePresentState(false);
			return false;
		}
	}

	bool copied = false;
	if (_interopAvailable) {
		copied = readbackViaInterop(sourceTexture, width, height);
	}

	if (!copied) {
		copied = readbackViaPBO(sourceTexture, width, height);
	}

	if (!copied) {
		updatePresentState(false);
		return false;
	}

	UINT syncInterval = vsync ? 1u : 0u;
	UINT presentFlags = vsync ? 0u : DXGI_PRESENT_ALLOW_TEARING;
	HRESULT hr = _swapChain->Present(syncInterval, presentFlags);
	if (FAILED(hr)) {
		mprintf(("OpenGL HDR: Present failed (0x%08X)\n", hr));
		fflush(nullptr);
		updatePresentState(false);
		return false;
	}

	updatePresentState(true);
	return true;
}

void OGLHDRPresenter::shutdown()
{
	if (!_initialized) {
		return;
	}

	destroyWGLInterop();
	destroyPBOs();
	safe_release(_stagingTexture);
	safe_release(_backBuffer);
	safe_release(_swapChain);
	safe_release(_factory);
	safe_release(_context);
	safe_release(_device);

	if (_readbackFBO != 0) {
		glDeleteFramebuffers(1, &_readbackFBO);
		_readbackFBO = 0;
	}

	_initialized = false;
}

bool OGLHDRPresenter::createD3D11Device()
{
	UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#if defined(_DEBUG)
	flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

	const D3D_FEATURE_LEVEL requestedLevels[] = {
		D3D_FEATURE_LEVEL_11_1,
		D3D_FEATURE_LEVEL_11_0,
		D3D_FEATURE_LEVEL_10_1,
		D3D_FEATURE_LEVEL_10_0,
	};

	D3D_FEATURE_LEVEL createdLevel = {};
	HRESULT hr = D3D11CreateDevice(nullptr,
		D3D_DRIVER_TYPE_HARDWARE,
		nullptr,
		flags,
		requestedLevels,
		static_cast<UINT>(sizeof(requestedLevels) / sizeof(requestedLevels[0])),
		D3D11_SDK_VERSION,
		&_device,
		&createdLevel,
		&_context);

	if (FAILED(hr)) {
		mprintf(("OpenGL HDR: D3D11CreateDevice failed (0x%08X)\n", hr));
		fflush(nullptr);
		return false;
	}

	return true;
}

bool OGLHDRPresenter::createSwapChain()
{
	UINT factoryFlags = 0;
#if defined(_DEBUG)
	factoryFlags |= DXGI_CREATE_FACTORY_DEBUG;
#endif

	HRESULT hr = CreateDXGIFactory2(factoryFlags, IID_PPV_ARGS(&_factory));
	if (FAILED(hr)) {
		mprintf(("OpenGL HDR: CreateDXGIFactory2 failed (0x%08X)\n", hr));
		fflush(nullptr);
		return false;
	}

	DXGI_SWAP_CHAIN_DESC1 desc = {};
	desc.Width = static_cast<UINT>(_width);
	desc.Height = static_cast<UINT>(_height);
	desc.Format = DXGI_FORMAT_R10G10B10A2_UNORM;
	desc.Stereo = FALSE;
	desc.SampleDesc.Count = 1;
	desc.SampleDesc.Quality = 0;
	desc.BufferUsage = DXGI_USAGE_BACK_BUFFER | DXGI_USAGE_RENDER_TARGET_OUTPUT;
	desc.BufferCount = 2;
	desc.Scaling = DXGI_SCALING_STRETCH;
	desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
	desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
	desc.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING | DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;

	IDXGISwapChain1* swapChain1 = nullptr;
	hr = _factory->CreateSwapChainForHwnd(_device, _hwnd, &desc, nullptr, nullptr, &swapChain1);
	if (FAILED(hr)) {
		mprintf(("OpenGL HDR: CreateSwapChainForHwnd failed (0x%08X)\n", hr));
		fflush(nullptr);
		return false;
	}

	hr = swapChain1->QueryInterface(IID_PPV_ARGS(&_swapChain));
	safe_release(swapChain1);
	if (FAILED(hr)) {
		mprintf(("OpenGL HDR: Failed to acquire IDXGISwapChain4 (0x%08X)\n", hr));
		fflush(nullptr);
		return false;
	}

	_factory->MakeWindowAssociation(_hwnd, DXGI_MWA_NO_ALT_ENTER);

	return true;
}

bool OGLHDRPresenter::setupHDRMetadata()
{
	if (_swapChain == nullptr) {
		return false;
	}

	HRESULT hr = _swapChain->SetColorSpace1(DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020);
	if (FAILED(hr)) {
		mprintf(("OpenGL HDR: Failed to set HDR color space (0x%08X)\n", hr));
		fflush(nullptr);
		return false;
	}

	DXGI_HDR_METADATA_HDR10 meta = {};
	meta.RedPrimary[0] = 34000;
	meta.RedPrimary[1] = 16000;
	meta.GreenPrimary[0] = 13250;
	meta.GreenPrimary[1] = 34500;
	meta.BluePrimary[0] = 7500;
	meta.BluePrimary[1] = 3000;
	meta.WhitePoint[0] = 15635;
	meta.WhitePoint[1] = 16450;
	float maxNits = std::max(Gr_hdr_max_nits, Gr_hdr_paper_white_nits);
	meta.MaxMasteringLuminance = static_cast<UINT>(maxNits * 10000.0f + 0.5f);
	meta.MinMasteringLuminance = 50; // 0.005 nits
	// CLL/FALL are 16-bit fields; clamp to the representable range.
	float cll = std::min(maxNits, 65535.0f);
	float fall = std::min(std::min(maxNits, Gr_hdr_paper_white_nits), 65535.0f);
	meta.MaxContentLightLevel = static_cast<UINT16>(cll + 0.5f);
	meta.MaxFrameAverageLightLevel = static_cast<UINT16>(fall + 0.5f);

	hr = _swapChain->SetHDRMetaData(DXGI_HDR_METADATA_TYPE_HDR10, sizeof(meta), &meta);
	if (FAILED(hr)) {
		mprintf(("OpenGL HDR: Failed to upload HDR metadata (0x%08X)\n", hr));
		fflush(nullptr);
		return false;
	}

	return true;
}

bool OGLHDRPresenter::createBackBufferResources()
{
	if (_swapChain == nullptr) {
		return false;
	}

	safe_release(_backBuffer);
	HRESULT hr = _swapChain->GetBuffer(0, IID_PPV_ARGS(&_backBuffer));
	if (FAILED(hr)) {
		mprintf(("OpenGL HDR: Could not acquire swap chain buffer (0x%08X)\n", hr));
		fflush(nullptr);
		return false;
	}

	safe_release(_stagingTexture);

	D3D11_TEXTURE2D_DESC desc = {};
	desc.Width = static_cast<UINT>(_width);
	desc.Height = static_cast<UINT>(_height);
	desc.MipLevels = 1;
	desc.ArraySize = 1;
	desc.Format = DXGI_FORMAT_R10G10B10A2_UNORM;
	desc.SampleDesc.Count = 1;
	desc.SampleDesc.Quality = 0;
	desc.Usage = D3D11_USAGE_STAGING;
	desc.BindFlags = 0;
	desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
	desc.MiscFlags = 0;

	hr = _device->CreateTexture2D(&desc, nullptr, &_stagingTexture);
	if (FAILED(hr)) {
		mprintf(("OpenGL HDR: Failed to create staging texture (0x%08X)\n", hr));
		fflush(nullptr);
		return false;
	}

	return true;
}

bool OGLHDRPresenter::createPBOs()
{
	if (_readbackFBO == 0) {
		glGenFramebuffers(1, &_readbackFBO);
	}

	glBindFramebuffer(GL_READ_FRAMEBUFFER, _readbackFBO);
	glReadBuffer(GL_COLOR_ATTACHMENT0);
	glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);

	glGenBuffers(2, _pbo);
	size_t bufferSize = static_cast<size_t>(_width) * static_cast<size_t>(_height) * 4 * sizeof(float);
	for (int i = 0; i < 2; ++i) {
		glBindBuffer(GL_PIXEL_PACK_BUFFER, _pbo[i]);
		glBufferData(GL_PIXEL_PACK_BUFFER, bufferSize, nullptr, GL_STREAM_READ);
	}
	glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);

	_currentPBO = 0;
	_pboPrimed = false;

	return true;
}

void OGLHDRPresenter::destroyPBOs()
{
	if (_pbo[0] != 0 || _pbo[1] != 0) {
		glDeleteBuffers(2, _pbo);
		_pbo[0] = 0;
		_pbo[1] = 0;
	}
	_pboPrimed = false;
}

bool OGLHDRPresenter::setupWGLInterop()
{
	// WGL_NV_DX_interop is not in our glad_wgl build.
	// The presenter relies on the PBO readback path for now.
	return false;
}

void OGLHDRPresenter::destroyWGLInterop()
{
	// Not implemented (PBO fallback only)
}

bool OGLHDRPresenter::readbackViaPBO(GLuint sourceTexture, int width, int height)
{
	if (_readbackFBO == 0) {
		return false;
	}

	glBindFramebuffer(GL_READ_FRAMEBUFFER, _readbackFBO);
	glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, sourceTexture, 0);

	GLenum status = glCheckFramebufferStatus(GL_READ_FRAMEBUFFER);
	if (status != GL_FRAMEBUFFER_COMPLETE) {
		glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
		mprintf(("OpenGL HDR: Readback framebuffer incomplete (0x%X)\n", status));
		fflush(nullptr);
		return false;
	}

	glBindBuffer(GL_PIXEL_PACK_BUFFER, _pbo[_currentPBO]);
	glReadPixels(0, 0, width, height, GL_RGBA, GL_FLOAT, nullptr);
	glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);

	const int mappedIndex = _pboPrimed ? ((_currentPBO + 1) % 2) : _currentPBO;

	glBindBuffer(GL_PIXEL_PACK_BUFFER, _pbo[mappedIndex]);
	void* ptr = glMapBuffer(GL_PIXEL_PACK_BUFFER, GL_READ_ONLY);
	if (ptr == nullptr) {
		mprintf(("OpenGL HDR: Failed to map PBO for readback\n"));
		fflush(nullptr);
		glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
		return false;
	}

	size_t pixelCount = static_cast<size_t>(width) * static_cast<size_t>(height);
	_cpuBuffer.resize(pixelCount);
	convertRGBA16FToR10G10B10A2(ptr, _cpuBuffer.data(), static_cast<int>(pixelCount));

	glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
	glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);

	_pboPrimed = true;
	_currentPBO = (_currentPBO + 1) % 2;

	return uploadToBackBuffer(_cpuBuffer.data(), width, height);
}

bool OGLHDRPresenter::readbackViaInterop(GLuint /*sourceTexture*/, int /*width*/, int /*height*/)
{
	// Interop path not yet implemented.
	return false;
}

bool OGLHDRPresenter::uploadToBackBuffer(const uint32_t* data, int width, int height)
{
	if (!_context || !_stagingTexture || !_backBuffer) {
		return false;
	}

	D3D11_MAPPED_SUBRESOURCE mapped = {};
	HRESULT hr = _context->Map(_stagingTexture, 0, D3D11_MAP_WRITE, 0, &mapped);
	if (FAILED(hr)) {
		mprintf(("OpenGL HDR: Failed to map staging texture (0x%08X)\n", hr));
		fflush(nullptr);
		return false;
	}

	size_t rowPitch = static_cast<size_t>(width) * sizeof(uint32_t);
	const uint8_t* srcBytes = reinterpret_cast<const uint8_t*>(data);

	for (int y = 0; y < height; ++y) {
		std::memcpy(static_cast<uint8_t*>(mapped.pData) + mapped.RowPitch * y, srcBytes + rowPitch * y, rowPitch);
	}

	_context->Unmap(_stagingTexture, 0);

	_context->CopyResource(_backBuffer, _stagingTexture);

	return true;
}

void OGLHDRPresenter::convertRGBA16FToR10G10B10A2(const void* src, void* dst, int pixelCount)
{
	const float* floats = static_cast<const float*>(src);
	uint32_t* out = static_cast<uint32_t*>(dst);

	const float paperWhite = std::max(Gr_hdr_paper_white_nits, 1.0f);
	const float maxNits = std::max(Gr_hdr_max_nits, paperWhite);

	for (int i = 0; i < pixelCount; ++i) {
		float r = floats[i * 4 + 0];
		float g = floats[i * 4 + 1];
		float b = floats[i * 4 + 2];

		// Treat 1.0 as paper-white and clamp to the mastering peak before PQ encoding.
		float rNits = std::clamp(r * paperWhite, 0.0f, maxNits);
		float gNits = std::clamp(g * paperWhite, 0.0f, maxNits);
		float bNits = std::clamp(b * paperWhite, 0.0f, maxNits);

		float mappedR = encodePQ(rNits);
		float mappedG = encodePQ(gNits);
		float mappedB = encodePQ(bNits);

		uint32_t r10 = static_cast<uint32_t>(std::clamp(mappedR, 0.0f, 1.0f) * 1023.0f + 0.5f);
		uint32_t g10 = static_cast<uint32_t>(std::clamp(mappedG, 0.0f, 1.0f) * 1023.0f + 0.5f);
		uint32_t b10 = static_cast<uint32_t>(std::clamp(mappedB, 0.0f, 1.0f) * 1023.0f + 0.5f);
		uint32_t a2 = 3; // fully opaque

		out[i] = (b10 << 20) | (g10 << 10) | (r10 << 0) | (a2 << 30);
	}
}

void OGLHDRPresenter::updatePresentState(bool success)
{
	if (success == _lastPresentSucceeded) {
		return;
	}

	if (success) {
		mprintf(("OpenGL HDR: Presenter now active\n"));
	} else {
		mprintf(("OpenGL HDR: Presenter disabled, returning to SDR path\n"));
	}
	fflush(nullptr);

	_lastPresentSucceeded = success;
}

#endif // _WIN32


