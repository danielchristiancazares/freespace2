#pragma once

#ifdef _WIN32

#include <glad/glad.h>

#include <cstdint>
#include <vector>

struct IDXGIFactory6;
struct IDXGISwapChain1;
struct IDXGISwapChain4;
struct IDXGIAdapter1;
struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11Texture2D;

struct HWND__;
using HWND = HWND__*;

class OGLHDRPresenter {
  public:
	OGLHDRPresenter();
	~OGLHDRPresenter();

	bool initialize(HWND hwnd, int width, int height);
	bool resize(int width, int height);
	bool presentFromGL(GLuint sourceTexture, int width, int height, bool vsync);
	void shutdown();

	bool isInitialized() const { return _initialized; }
	bool wasHDRActiveLastFrame() const { return _lastPresentSucceeded; }

  private:
	bool _initialized = false;
	bool _interopAvailable = false;
	bool _pboPrimed = false;
	bool _lastPresentSucceeded = false;

	HWND _hwnd = nullptr;
	int _width = 0;
	int _height = 0;

	IDXGIFactory6* _factory = nullptr;
	IDXGISwapChain4* _swapChain = nullptr;
	ID3D11Device* _device = nullptr;
	ID3D11DeviceContext* _context = nullptr;
	ID3D11Texture2D* _backBuffer = nullptr;
	ID3D11Texture2D* _stagingTexture = nullptr;

	GLuint _readbackFBO = 0;
	GLuint _pbo[2] = {0, 0};
	int _currentPBO = 0;
	std::vector<uint32_t> _cpuBuffer;

	bool createD3D11Device();
	bool createSwapChain();
	bool setupHDRMetadata();
	bool createBackBufferResources();
	bool createPBOs();
	void destroyPBOs();
	bool setupWGLInterop();
	void destroyWGLInterop();
	bool readbackViaPBO(GLuint sourceTexture, int width, int height);
	bool readbackViaInterop(GLuint sourceTexture, int width, int height);
	bool uploadToBackBuffer(const uint32_t* data, int width, int height);
	void convertRGBA16FToR10G10B10A2(const void* src, void* dst, int pixelCount);
	void releaseCOMObjects();
	void updatePresentState(bool success);
};

#endif // _WIN32


