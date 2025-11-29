// Stub implementations for DRM-related symbols when DRM backend is disabled
// This file is only compiled when HAVE_DRM=0

#include "convar.h"
#include "gamescope_shared.h"

#if !HAVE_DRM

// Stub for cv_drm_debug_disable_explicit_sync
namespace gamescope {
    ConVar<bool> cv_drm_debug_disable_explicit_sync( "drm_debug_disable_explicit_sync", false, "Force disable explicit sync on the DRM backend." );
}
using gamescope::cv_drm_debug_disable_explicit_sync;

// Stub for g_nDynamicRefreshHz
int g_nDynamicRefreshHz = 0;

// Stub for drm_sleep_screen
void drm_sleep_screen( gamescope::GamescopeScreenType eType, bool bSleep )
{
    // No-op when DRM is disabled
    (void)eType;
    (void)bSleep;
}

#endif // !HAVE_DRM
