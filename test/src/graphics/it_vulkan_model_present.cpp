#include "cfile/cfile.h"
#include "graphics/2d.h"
#include "graphics/matrix.h"
#include "globalincs/pstypes.h"
#include "bmpman/bmpman.h"
#include "graphics/software/font.h"
#include "io/timer.h"
#include "lighting/lighting.h"
#include "math/floating.h"
#include "model/model.h"
#include "model/modelrender.h"
#include "osapi/osapi.h"
#include "osapi/osregistry.h"
#include "render/3d.h"
#include "mod_table/mod_table.h"
#include <SDL.h>
#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <thread>

namespace {

class TestViewport : public os::Viewport {
	SDL_Window* _window;
  public:
	explicit TestViewport(SDL_Window* wnd) : _window(wnd) {}
	~TestViewport() override
	{
		if (_window) {
			SDL_DestroyWindow(_window);
			_window = nullptr;
		}
	}
	SDL_Window* toSDLWindow() override { return _window; }
	std::pair<uint32_t, uint32_t> getSize() override
	{
		int w = 0, h = 0;
		SDL_GetWindowSize(_window, &w, &h);
		return {static_cast<uint32_t>(w), static_cast<uint32_t>(h)};
	}
	void swapBuffers() override {}
	void setState(os::ViewportState) override {}
	void minimize() override { SDL_MinimizeWindow(_window); }
	void restore() override { SDL_RestoreWindow(_window); }
};

class TestGraphicsOperations : public os::GraphicsOperations {
  public:
	TestGraphicsOperations()
	{
		SDL_InitSubSystem(SDL_INIT_VIDEO);
	}
	~TestGraphicsOperations() override { SDL_QuitSubSystem(SDL_INIT_VIDEO); }

	std::unique_ptr<os::OpenGLContext> createOpenGLContext(os::Viewport*, const os::OpenGLContextAttributes&) override
	{
		return nullptr;
	}
	void makeOpenGLContextCurrent(os::Viewport*, os::OpenGLContext*) override {}

	std::unique_ptr<os::Viewport> createViewport(const os::ViewPortProperties& props) override
	{
		uint32_t flags = SDL_WINDOW_SHOWN | SDL_WINDOW_VULKAN;
		if (props.flags[os::ViewPortFlags::Borderless]) flags |= SDL_WINDOW_BORDERLESS;
		if (props.flags[os::ViewPortFlags::Fullscreen]) flags |= SDL_WINDOW_FULLSCREEN;
		if (props.flags[os::ViewPortFlags::Resizeable]) flags |= SDL_WINDOW_RESIZABLE;
		if (props.flags[os::ViewPortFlags::Capture_Mouse]) flags |= SDL_WINDOW_INPUT_GRABBED;

		SDL_Window* wnd = SDL_CreateWindow(props.title.c_str(),
			SDL_WINDOWPOS_CENTERED_DISPLAY(props.display),
			SDL_WINDOWPOS_CENTERED_DISPLAY(props.display),
			static_cast<int>(props.width),
			static_cast<int>(props.height),
			flags);
		if (!wnd) {
			return nullptr;
		}
		return std::unique_ptr<os::Viewport>(new TestViewport(wnd));
	}
};

// Default to the Steam install path; can be overridden with FS2_STEAM_PATH.
std::string detect_fs2_root()
{
	if (const char* env = std::getenv("FS2_STEAM_PATH")) {
		return env;
	}
	return "C:\\Program Files (x86)\\Steam\\steamapps\\common\\Freespace 2";
}

// Load a specific model, returning -1 on absence or failure.
int load_specific_model(const char* filename)
{
	SCP_string name = filename;
	if (name.find('.') == SCP_string::npos) {
		name += ".pof";
	}

	if (!cf_exists_full(name.c_str(), CF_TYPE_MODELS)) {
		return -1;
	}

	auto handle = model_load(name.c_str(), nullptr, ErrorType::NONE, false);
	if (handle < 0) {
		return -1;
	}

	return handle;
}

void add_basic_light()
{
	light_reset();

	vec3d sun_dir{0.3f, -0.6f, -1.0f};
	vm_vec_normalize(&sun_dir);
	light_add_directional(&sun_dir, 0, true, 1.0f, 1.0f, 1.0f, 0.0f);
}

// Load a simple Terran thruster anim + glow; reuse across frames. Logs what path was found.
struct ThrusterAssets {
	int anim = -1;
	int glow = -1;
};

ThrusterAssets load_thruster_assets()
{
	static ThrusterAssets cache;
	if (cache.anim >= 0 && cache.glow >= 0) {
		return cache;
	}

	auto exists = [](const char* name) {
		const bool any = cf_exists_full(name, CF_TYPE_ANY);
		const bool eff = cf_exists_full(name, CF_TYPE_EFFECTS);
		return any || eff;
	};

	const char* anim_name = "thruster01";
	const char* glow_name = "thrusterglow01";
	std::cout << "[it] thruster exists any/effects? anim=" << exists(anim_name)
	          << " glow=" << exists(glow_name) << "\n" << std::flush;

	int nframes = 0, fps = 0;
	cache.anim = bm_load_animation(anim_name, &nframes, &fps, nullptr, nullptr, false, CF_TYPE_EFFECTS);
	nframes = fps = 0;
	cache.glow = bm_load_animation(glow_name, &nframes, &fps, nullptr, nullptr, false, CF_TYPE_EFFECTS);

	// Fallback: some installs may only have stills (e.g. PCX). Try bm_load so the test can still show thrusters.
	if (cache.anim < 0) {
		cache.anim = bm_load(anim_name);
		std::cout << "[it] thruster anim fallback bm_load -> " << cache.anim << "\n" << std::flush;
	}
	if (cache.glow < 0) {
		cache.glow = bm_load(glow_name);
		std::cout << "[it] thruster glow fallback bm_load -> " << cache.glow << "\n" << std::flush;
	}

	std::cout << "[it] thruster handles anim=" << cache.anim << " glow=" << cache.glow << "\n" << std::flush;
	return cache;
}

} // namespace

// Visible manual integration test. Requires a Vulkan-capable GPU and retail data.
TEST(VulkanModelVisible, VisibleShip)
{
	RecordProperty("LABELS", "manual-vulkan");

	if (!std::getenv("FS2_VULKAN_IT")) {
		GTEST_SKIP() << "Set FS2_VULKAN_IT=1 to run the visible Vulkan integration test.";
	}

	const std::string fs2_root = detect_fs2_root();
	if (!std::filesystem::exists(fs2_root)) {
		GTEST_SKIP() << "FS2 root not found at '" << fs2_root << "'. Set FS2_STEAM_PATH.";
	}

	// cfile_init expects a path to an executable; the filename itself is ignored for path building.
	const std::string exe_path = fs2_root + "\\fs2_open_22_0_0.exe";
	if (cfile_init(exe_path.c_str()) != 0) {
		GTEST_SKIP() << "cfile_init failed for root '" << fs2_root << "'.";
	}

	timer_init();
	os_init("VK Model IT", "VK Model IT");

	// Force the API selection to Vulkan so gr_init doesn't fall back to OpenGL based on prior config.
	os_config_write_string(nullptr, "VideocardFs2open", "VK  -(1280x720)x32 bit");
	Window_icon_path = "app_icon_sse";

	std::cout << "[it] calling gr_init...\n" << std::flush;
	std::cout << "[it] GR_VULKAN constant=" << GR_VULKAN << "\n" << std::flush;
	auto graphicsOps = std::make_unique<TestGraphicsOperations>();
	if (!gr_init(std::move(graphicsOps), GR_VULKAN, 1280, 720)) {
		cfile_close();
		timer_close();
		os_cleanup();
		GTEST_SKIP() << "Vulkan renderer failed to initialize (missing GPU/driver/features).";
	}
	std::cout << "[it] gr_init ok\n" << std::flush;
	std::cout << "[it] screen size " << gr_screen.max_w << "x" << gr_screen.max_h << "\n" << std::flush;
	ASSERT_TRUE(gr_screen.max_w > 0 && gr_screen.max_h > 0);
	// Kick off the first frame so recording is active before setup_frame.
	gr_flip(false);

	const int model_handle = load_specific_model("fighter01.pof");
	if (model_handle < 0) {
		gr_close();
		cfile_close();
		timer_close();
		os_cleanup();
		GTEST_SKIP() << "Required model 'fighter01.pof' not found or failed to load; point FS2_STEAM_PATH at a retail install.";
	}
	std::cout << "[it] model loaded\n" << std::flush;
	// Page textures up front to avoid fallback magenta if streaming lags.
	model_page_in_textures(model_handle);
	std::cout << "[it] textures paged\n" << std::flush;

	const float radius = std::max(model_get_radius(model_handle), 50.0f);
	matrix eye_orient = vmd_identity_matrix;
	vec3d eye_pos{0.0f, 0.0f, -radius * 2.5f};
	matrix obj_orient = vmd_identity_matrix;
	vec3d obj_pos{0.0f, 0.0f, 0.0f};
	const float fov = fl_radians(60.0f);

	add_basic_light();
	const auto thrusters = load_thruster_assets();
	if (thrusters.anim < 0 || thrusters.glow < 0) {
		model_unload(model_handle);
		gr_close();
		cfile_close();
		timer_close();
		os_cleanup();
		GTEST_FAIL() << "Thruster animation (thruster01 / thrusterglow01) not found; verify retail effects assets.";
		return;
	}
	std::cout << "[it] thruster assets ok\n" << std::flush;

	for (int frame = 0; frame < 360; ++frame) {
		os_poll();

		// Dark blue background for visibility.
		gr_set_clear_color(20, 30, 80);

		gr_setup_frame();
		g3_start_frame(1);
		g3_set_view_matrix(&eye_pos, &eye_orient, fov);

		// Route 3D rendering to scene texture (deferred rendering setup).
		gr_scene_texture_begin();

		// Push projection + view matrices to the GPU.
		gr_set_proj_matrix(fov, gr_screen.clip_aspect, 1.0f, 10000.0f);
		gr_set_view_matrix(&Eye_position, &Eye_matrix);

		model_render_params params;
		params.set_flags(params.get_model_flags() |
		                 MR_AUTOCENTER |
		                 MR_SHOW_THRUSTERS |
		                 MR_NO_CULL);
		params.set_color(255, 255, 255);
		// Keep thrusters visible: set a small constant plume length and textures.
		mst_info thruster{};
		thruster.length.xyz.z = 1.0f;
		thruster.primary_bitmap = thrusters.anim;
		thruster.primary_glow_bitmap = thrusters.glow;
		params.set_thruster_info(thruster);
		model_render_immediate(&params, model_handle, &obj_orient, &obj_pos);

		gr_end_view_matrix();
		gr_end_proj_matrix();
		gr_scene_texture_end();

		// 2D overlay to verify rendering path without fonts.
		gr_set_color(0, 0, 0);
		gr_rect(0, 0, gr_screen.max_w, 60);
		gr_set_color(255, 255, 0);
		gr_rect(0, 0, gr_screen.max_w, 30);
		gr_set_color(255, 0, 0);
		gr_rect(0, gr_screen.max_h - 30, gr_screen.max_w, 30);

		g3_end_frame();
		gr_flip();

		std::this_thread::sleep_for(std::chrono::milliseconds(16));
	}

	model_unload(model_handle);

	gr_close();
	cfile_close();
	timer_close();
	os_cleanup();
}
