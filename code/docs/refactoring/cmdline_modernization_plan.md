# Concrete Implementation Plan: Command Line Modernization

This document contains copy-paste ready code to refactor the Command Line Parser. Follow these steps in order.

## Step 0: Create the Config Header
**Action**: Create a new file `code/cmdline/CmdlineConfig.h` with the following content:

```cpp
#pragma once
#include <string>
#include <optional>
#include <cstdint>
#include <vector>
#include "globalincs/pstypes.h" 

namespace cmdline {

struct GraphicsConfig {
    bool windowed = false;                  
    bool fullscreenWindow = false;          
    std::optional<std::string> renderRes;   
    std::optional<std::pair<uint16_t, uint16_t>> windowRes; 
    bool centerRes = false;                 
    bool noScaleVid = false;                
    bool noVSync = false;                   

    float ambientPower = 1.0f;              
    float emissivePower = 1.0f;             
    float lightPower = 1.0f;                
    bool envMapEnabled = true;              
    bool glowMapEnabled = true;             
    bool specMapEnabled = true;             
    bool normalMapEnabled = true;           
    bool heightMapEnabled = true;           
    bool softParticlesEnabled = true;       
    bool deferredLightingEnabled = true;    
    bool deferredLightingCockpit = false;   
    bool emissiveEnabled = false;           
    bool shadowsEnabled = false;            
    bool postProcessEnabled = true;         
    bool framebufferExplosions = false;     
    bool framebufferThrusters = false;      
    
    int anisoLevel = 0;                     
    int msaaEnabled = 0;                    
    int textureMipmap = 1;                  
    bool setGamma = true;                   
    bool useFBO = true;                     
    bool usePBO = true;                     
    bool atiColorSwap = false;              
    bool glFinish = false;                  
    bool geoSdrEffects = true;              
    bool shaderCache = true;                
    bool largeShaders = true;               
    bool vulkan = false;                    
    bool vulkanStress = false;              
    bool drawElements = true;               
};

struct AudioConfig {
    bool musicEnabled = true;               
    bool soundEnabled = true;               
    bool voiceRecognitionEnabled = false;   
    bool enhancedSoundEnabled = true;       
    bool enable3dSound = true;              
};

struct NetworkConfig {
    std::optional<std::string> connectAddress; 
    int port = -1;                          
    std::optional<std::string> gatewayIp;   
    bool preferIpv4 = false;                
    bool preferIpv6 = false;                
    int timeout = 0;                        

    bool standalone = false;                
    bool startNetgame = false;              
    bool closedGame = false;                
    bool restrictedGame = false;            
    bool useLastPilot = false;              
    std::optional<std::string> gameName;    
    std::optional<std::string> gamePassword;
    std::optional<std::string> rankAbove;   
    std::optional<std::string> rankBelow;   

    bool multiLog = false;                  
    bool streamChatToFile = false;          
    bool inGameJoin = true;                 
    bool mpNoReturn = false;                
    bool objUpdate = false;                 
    bool dumpPacketType = false;            
    
    bool tableCrcs = false;                 
    bool missionCrcs = false;               
};

struct GameplayConfig {
    std::optional<std::string> activeMod;   
    std::optional<std::string> campaign;    
    std::optional<std::string> startMission;
    std::optional<std::string> pilot;       

    bool captureMouse = false;              
    bool nograb = false;                    
    int deadzone = 0;                       
    std::optional<std::string> keyboardLayout; 
    bool enableVr = false;                  
    bool headTracking = false;              

    bool autopilotInterruptable = true;     
    bool retailTimeCompression = false;     
    bool loadAllWeapons = false;            
    bool collisionsEnabled = true;          
    bool weaponsEnabled = true;             
    bool cheatLoadAllWeps = false;          
};

struct HUDConfig {
    bool stretchMenu = false;               
    bool showFps = false;                   
    bool dualScanlines = false;             
    bool orbRadar = false;                  
    bool rearmTimer = false;                
    bool ballisticGauge = false;            
    bool targetInfo = false;                
    bool mouseCoords = false;               
};

struct SystemConfig {
    bool fpsCapEnabled = true;              
    bool moviesEnabled = true;              
    bool parseErrorsEnabled = true;         
    bool extraWarnings = false;             
    bool unfocusPause = true;               
    bool setCpuAffinity = false;            
    bool multithreading = false;            
    bool portableMode = false;              
    std::optional<std::string> language;    
    bool overrideData = false;              

    bool showPos = false;                   
    bool showStats = false;                 
    bool debugWindow = false;               
    bool graphicsDebug = false;             
    bool imguiDebug = false;                
    bool logToStdout = false;               
    bool saveRenderTargets = false;         
    bool verifyVps = false;                 
    bool reparseMainhall = false;           
    bool outputSexpInfo = false;            
    bool outputScripting = false;           
    bool luaDevMode = false;                
    bool slowFramesOk = false;              
    bool benchmarkMode = false;             
    
    bool profileFrameTime = false;          
    bool profileWriteFile = false;          
    bool jsonProfiling = false;             
    bool frameProfile = false;              
    bool showVideoInfo = false;             
};

struct Config {
    GraphicsConfig graphics;
    AudioConfig audio;
    NetworkConfig network;
    GameplayConfig gameplay;
    HUDConfig hud;
    SystemConfig system;
};

extern const Config& Cfg;

} // namespace cmdline
```

---

## Step 1: Add Mod Parsing Logic to CFileSystem
We move the mod parsing logic from `cmdline.cpp` to `CFileSystem`.

**1.1 Edit `code/cfile/cfilesystem.h`**:
Add these lines inside the `class Cfilesystem` definition (preferably under the `public:` section near other methods):

```cpp
    void parse_mod_string(std::string_view mod_arg);
```

Add this line in the `private:` section:
```cpp
    std::vector<std::string> m_active_mods;
```

**1.2 Edit `code/cfile/cfilesystem.cpp`**:
Add this function implementation at the end of the file:

```cpp
#include "cmdline/CmdlineConfig.h" // Ensure this is included at top

// ... (existing code) ...

// Helper to remove extra chars (moved from cmdline.cpp logic)
static bool is_extra_space_local(char ch) {
    return ((ch == ' ') || (ch == '\t') || (ch == 0x0a) || (ch == '\'') || (ch == '"'));
}

void Cfilesystem::parse_mod_string(std::string_view mod_arg) {
    if (mod_arg.empty()) return;

    std::string mod_str(mod_arg);
    
    // Normalize logic (simplified from cmdline.cpp)
    // In a full implementation, you'd copy the exact 'drop_extra_chars' logic here if strictly needed, 
    // but std::string manipulation is safer.
    
    size_t pos = 0;
    while ((pos = mod_str.find(',')) != std::string::npos) {
        std::string token = mod_str.substr(0, pos);
        // Trim logic here if needed
        if (!token.empty()) {
             m_active_mods.push_back(token);
        }
        mod_str.erase(0, pos + 1);
    }
    if (!mod_str.empty()) {
        m_active_mods.push_back(mod_str);
    }
    
    // TODO: Add the specific unix path normalization if running on Linux/macOS
}
```

---

## Step 2: Implement Logic in `cmdline.cpp`
This step populates the `Config` struct from the parsed arguments.

**Edit `code/cmdline/cmdline.cpp`**:

**2.1**: Add the include at the top:
```cpp
#include "cmdline/CmdlineConfig.h"
```

**2.2**: Add the implementation of the config storage and mapping function. Place this **before** `os_init_cmdline`:

```cpp
namespace cmdline {

static Config internal_config;
const Config& Cfg = internal_config;

void map_parms_to_config() {
    // --- System ---
    internal_config.system.fpsCapEnabled = !no_fpscap.found(); 
    
    if (mod_arg.found()) internal_config.gameplay.activeMod = mod_arg.str();
    if (campaign_arg.found()) internal_config.gameplay.campaign = campaign_arg.str();

    // --- Network ---
    if (connect_arg.found()) internal_config.network.connectAddress = connect_arg.str();
    if (gamename_arg.found()) internal_config.network.gameName = gamename_arg.str();
    if (gamepassword_arg.found()) internal_config.network.gamePassword = gamepassword_arg.str();
    if (port_arg.found()) internal_config.network.port = port_arg.get_int();
    
    internal_config.network.standalone = standalone_arg.found();
    internal_config.network.startNetgame = startgame_arg.found();
    internal_config.network.closedGame = gameclosed_arg.found();
    internal_config.network.restrictedGame = gamerestricted_arg.found();

    // --- Graphics ---
    internal_config.graphics.windowed = window_arg.found();
    internal_config.graphics.fullscreenWindow = fullscreen_window_arg.found();
    internal_config.graphics.setGamma = !no_set_gamma_arg.found(); 
    internal_config.graphics.envMapEnabled = !env.found();
    internal_config.graphics.glowMapEnabled = !glow_arg.found();
    internal_config.graphics.specMapEnabled = !spec_arg.found();
    internal_config.graphics.normalMapEnabled = !normal_arg.found();
    internal_config.graphics.heightMapEnabled = !height_arg.found();
    internal_config.graphics.deferredLightingEnabled = !no_deferred_lighting_arg.found();
    internal_config.graphics.postProcessEnabled = !no_postprocess_arg.found();
    
    if (render_res_arg.found()) internal_config.graphics.renderRes = render_res_arg.str();
    
    // --- Audio ---
    internal_config.audio.soundEnabled = !nosound_arg.found();
    internal_config.audio.musicEnabled = !nomusic_arg.found();
    internal_config.audio.enhancedSoundEnabled = !noenhancedsound_arg.found();

    // --- Gameplay ---
    internal_config.gameplay.autopilotInterruptable = !allow_autpilot_interrupt.found();
    internal_config.gameplay.collisionsEnabled = !dis_collisions.found();
    internal_config.gameplay.weaponsEnabled = !dis_weapons.found();

    // --- HUD ---
    internal_config.hud.showFps = fps_arg.found();
    internal_config.hud.orbRadar = orb_radar.found();
}

} // namespace cmdline
```

**2.3**: Call the mapping function.
Find `os_init_cmdline(int argc, char *argv[])` and add the call at the very end:

```cpp
void os_init_cmdline(int argc, char *argv[])
{
    // ... existing code ...
    os_parse_parms(argc, argv);
    os_validate_parms(argc, argv);
    
    // ADD THIS LINE:
    cmdline::map_parms_to_config();
}
```

---

## Step 3: Local Handling for Special Flags
We need to handle `connect_addr`, `fps_cap`, and `window_mode` locally in their subsystems so they don't rely on the globals in `cmdline.cpp`.

### 3.1 Network (`code/network/multi.cpp`)
**Action**: Update `multi_init`.

1.  Add `#include "cmdline/CmdlineConfig.h"` at the top.
2.  In `multi_init()`, replace the usage of `Cmdline_connect_addr`:

```cpp
// OLD:
// if (Cmdline_connect_addr != NULL) ...

// NEW:
if (cmdline::Cfg.network.connectAddress.has_value()) {
    // Copy to local static buffer if needed, or use directly
    static char connect_addr_buf[256];
    strncpy(connect_addr_buf, cmdline::Cfg.network.connectAddress->c_str(), sizeof(connect_addr_buf));
    Multi_connect_addr = connect_addr_buf;
}
```

### 3.2 Movie Player (`code/cutscene/movie.cpp`)
**Action**: Update `movie_init` or class constructor.

1.  Add `#include "cmdline/CmdlineConfig.h"` at the top.
2.  Find where `Cmdline_NoFPSCap` is used.
3.  Replace with `cmdline::Cfg.system.fpsCapEnabled` (note logic inversion: `NoFPSCap` (int) vs `fpsCapEnabled` (bool)).
    *   If `Cmdline_NoFPSCap` was 1, `fpsCapEnabled` should be FALSE.
    *   *Correction*: In step 2.2 we mapped `internal_config.system.fpsCapEnabled = !no_fpscap.found();`
    *   So if `-no_fps_capping` is present, `fpsCapEnabled` is FALSE.
    *   Logic: `if (cmdline::Cfg.system.fpsCapEnabled) { /* limit fps */ }`

### 3.3 Graphics (`code/graphics/2d.cpp`)
**Action**: Update `gr_init`.

1.  Add `#include "cmdline/CmdlineConfig.h"` at the top.
2.  Initialize the local window mode variable:

```cpp
// Inside gr_init or equivalent
Gr_windowed_mode = cmdline::Cfg.graphics.windowed;
```

---

## Step 4: Verification (The "Search & Replace" Phase)
Now you can safely replace usages of the global variables.

**Example Replacement Table:**

| Global Variable | New Accessor |
| :--- | :--- |
| `Cmdline_freespace_no_sound` | `!cmdline::Cfg.audio.soundEnabled` |
| `Cmdline_freespace_no_music` | `!cmdline::Cfg.audio.musicEnabled` |
| `Cmdline_window` | `cmdline::Cfg.graphics.windowed` |
| `Cmdline_no_deferred_lighting` | `!cmdline::Cfg.graphics.deferredLightingEnabled` |

**Procedure:**
1.  Pick one variable (e.g., `Cmdline_freespace_no_music`).
2.  Search for it in the whole solution.
3.  Replace all **read** occurrences with the new accessor.
4.  Remove the `extern` declaration from `cmdline.h`.
5.  Remove the definition from `cmdline.cpp`.
6.  Compile.
7.  Repeat.

```