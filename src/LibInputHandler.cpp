#include "LibInputHandler.h"

#include <libinput.h>
#include <fcntl.h>
#include <unistd.h>
#include <poll.h>
#include <vector>
#include <string>
#include <errno.h>
#include <stdio.h>
#include <linux/input.h>
#include <sys/ioctl.h>

#include "log.hpp"
#include "backend.h"
#include "wlserver.hpp"
#include "Utils/Defer.h"

// Splitux input device filtering (defined in main.cpp)
extern std::vector<std::string> g_vecLibInputHoldDevices;
extern bool g_bBackendDisableKeyboard;
extern bool g_bBackendDisableMouse;

// Handles libinput in contexts where we don't have a session
// and can't use the wlroots libinput stuff.
//
// eg. in VR where we want global access to the m + kb
// without doing any seat dance.
//
// That may change in the future...
// but for now, this solves that problem.

namespace gamescope
{
    static LogScope log_input_stealer( "InputStealer" );

    const libinput_interface CLibInputHandler::s_LibInputInterface =
    {
        .open_restricted = []( const char *pszPath, int nFlags, void *pUserData ) -> int
        {
            int fd = open( pszPath, nFlags );
            if ( fd >= 0 )
            {
                // Grab the device exclusively so other processes (like the compositor)
                // cannot read from it. This prevents input cross-talk between instances.
                if ( ioctl( fd, EVIOCGRAB, 1 ) < 0 )
                {
                    fprintf( stderr, "[gamescope-splitux] Warning: Failed to grab device %s exclusively (errno=%d)\n",
                             pszPath, errno );
                }
                else
                {
                    fprintf( stderr, "[gamescope-splitux] Grabbed device %s exclusively\n", pszPath );
                }
            }
            return fd;
        },

        .close_restricted = []( int nFd, void *pUserData ) -> void
        {
            // Release the grab before closing
            ioctl( nFd, EVIOCGRAB, 0 );
            close( nFd );
        },
    };

    CLibInputHandler::CLibInputHandler()
    {
    }

    CLibInputHandler::~CLibInputHandler()
    {
        if ( m_pLibInput )
        {
            libinput_unref( m_pLibInput );
            m_pLibInput = nullptr;
        }

        if ( m_pUdev )
        {
            udev_unref( m_pUdev );
            m_pUdev = nullptr;
        }
    }

    bool CLibInputHandler::Init()
    {
        m_pUdev = udev_new();
        if ( !m_pUdev )
        {
            log_input_stealer.errorf( "Failed to create udev interface" );
            return false;
        }

        // Splitux: If specific devices are requested, use path-based context
        // instead of grabbing all devices on the seat
        if ( !g_vecLibInputHoldDevices.empty() )
        {
            log_input_stealer.infof( "Using path-based libinput with %zu device(s)", g_vecLibInputHoldDevices.size() );

            m_pLibInput = libinput_path_create_context( &s_LibInputInterface, nullptr );
            if ( !m_pLibInput )
            {
                log_input_stealer.errorf( "Failed to create path-based libinput context" );
                return false;
            }

            int nDevicesAdded = 0;
            for ( const auto& path : g_vecLibInputHoldDevices )
            {
                fprintf( stderr, "[gamescope-splitux] Attempting to add input device: %s\n", path.c_str() );
                libinput_device *pDevice = libinput_path_add_device( m_pLibInput, path.c_str() );
                if ( pDevice )
                {
                    fprintf( stderr, "[gamescope-splitux] Successfully added input device: %s\n", path.c_str() );
                    log_input_stealer.infof( "Added input device: %s", path.c_str() );
                    nDevicesAdded++;
                }
                else
                {
                    fprintf( stderr, "[gamescope-splitux] ERROR: Failed to add input device: %s (errno=%d: %s)\n",
                             path.c_str(), errno, strerror(errno) );
                    log_input_stealer.errorf( "Failed to add input device: %s", path.c_str() );
                }
            }

            if ( nDevicesAdded == 0 )
            {
                fprintf( stderr, "[gamescope-splitux] ERROR: No input devices were successfully added! Input will not work.\n" );
                return false;
            }
            fprintf( stderr, "[gamescope-splitux] LibInput initialized with %d device(s)\n", nDevicesAdded );
        }
        else
        {
            // Default behavior: use udev to grab all devices on seat0
            m_pLibInput = libinput_udev_create_context( &s_LibInputInterface, nullptr, m_pUdev );
            if ( !m_pLibInput )
            {
                log_input_stealer.errorf( "Failed to create libinput context" );
                return false;
            }

            const char *pszSeatName = "seat0";
            if ( libinput_udev_assign_seat( m_pLibInput, pszSeatName ) == -1 )
            {
                log_input_stealer.errorf( "Could not assign seat \"%s\"", pszSeatName );
                return false;
            }
        }

        return true;
    }

    int CLibInputHandler::GetFD()
    {
        if ( !m_pLibInput )
            return -1;

        return libinput_get_fd( m_pLibInput );
    }

    void CLibInputHandler::OnPollIn()
    {
        static uint32_t s_uSequence = 0;
        static uint32_t s_nEventCount = 0;
        static uint32_t s_nLastLoggedCount = 0;

        libinput_dispatch( m_pLibInput );

		while ( libinput_event *pEvent = libinput_get_event( m_pLibInput ) )
        {
            defer( libinput_event_destroy( pEvent ) );

            libinput_event_type eEventType = libinput_event_get_type( pEvent );
            s_nEventCount++;

            // Log first event to confirm we're receiving data
            if ( s_nEventCount == 1 )
            {
                fprintf( stderr, "[gamescope-splitux] LibInput: received FIRST event! (type=%d)\n", eEventType );
            }
            // Log every 100 events to show activity
            else if ( s_nEventCount - s_nLastLoggedCount >= 100 )
            {
                fprintf( stderr, "[gamescope-splitux] LibInput: processed %u events (type=%d)\n", s_nEventCount, eEventType );
                s_nLastLoggedCount = s_nEventCount;
            }

            switch ( eEventType )
            {
                case LIBINPUT_EVENT_POINTER_MOTION:
                {
                    if ( g_bBackendDisableMouse )
                        break;

                    libinput_event_pointer *pPointerEvent = libinput_event_get_pointer_event( pEvent );

                    double flDx = libinput_event_pointer_get_dx( pPointerEvent );
                    double flDy = libinput_event_pointer_get_dy( pPointerEvent );

                    GetBackend()->NotifyPhysicalInput( InputType::Mouse );

                    wlserver_lock();
                    wlserver_mousemotion( flDx, flDy, ++s_uSequence );
                    wlserver_unlock();
                }
                break;

                case LIBINPUT_EVENT_POINTER_MOTION_ABSOLUTE:
                {
                    if ( g_bBackendDisableMouse )
                        break;

                    libinput_event_pointer *pPointerEvent = libinput_event_get_pointer_event( pEvent );

                    double flX = libinput_event_pointer_get_absolute_x( pPointerEvent );
                    double flY = libinput_event_pointer_get_absolute_y( pPointerEvent );

                    GetBackend()->NotifyPhysicalInput( InputType::Mouse );

                    wlserver_lock();
                    wlserver_mousewarp( flX, flY, ++s_uSequence, true );
                    wlserver_unlock();
                }
                break;

                case LIBINPUT_EVENT_POINTER_BUTTON:
                {
                    if ( g_bBackendDisableMouse )
                        break;

                    libinput_event_pointer *pPointerEvent = libinput_event_get_pointer_event( pEvent );

                    uint32_t uButton = libinput_event_pointer_get_button( pPointerEvent );
                    libinput_button_state eButtonState = libinput_event_pointer_get_button_state( pPointerEvent );

                    wlserver_lock();
                    wlserver_mousebutton( uButton, eButtonState == LIBINPUT_BUTTON_STATE_PRESSED, ++s_uSequence );
                    wlserver_unlock();
                }
                break;

                case LIBINPUT_EVENT_POINTER_SCROLL_WHEEL:
                {
                    if ( g_bBackendDisableMouse )
                        break;

                    libinput_event_pointer *pPointerEvent = libinput_event_get_pointer_event( pEvent );

                    static constexpr libinput_pointer_axis eAxes[] =
                    {
                        LIBINPUT_POINTER_AXIS_SCROLL_HORIZONTAL,
                        LIBINPUT_POINTER_AXIS_SCROLL_VERTICAL,
                    };

                    for ( uint32_t i = 0; i < std::size( eAxes ); i++ )
                    {
                        libinput_pointer_axis eAxis = eAxes[i];

                        if ( !libinput_event_pointer_has_axis( pPointerEvent, eAxis ) )
                            continue;

                        double flScroll = libinput_event_pointer_get_scroll_value_v120( pPointerEvent, eAxis );
                        m_flScrollAccum[i] += flScroll / 120.0;
                    }
                }
                break;

                case LIBINPUT_EVENT_KEYBOARD_KEY:
                {
                    if ( g_bBackendDisableKeyboard )
                        break;

                    libinput_event_keyboard *pKeyboardEvent = libinput_event_get_keyboard_event( pEvent );
                    uint32_t uKey = libinput_event_keyboard_get_key( pKeyboardEvent );
                    libinput_key_state eState = libinput_event_keyboard_get_key_state( pKeyboardEvent );

                    wlserver_lock();
                    wlserver_key( uKey, eState == LIBINPUT_KEY_STATE_PRESSED, ++s_uSequence );
                    wlserver_unlock();
                }
                break;

                default:
                    break;
            }
		}

        // Handle scrolling
        {
            double flScrollX = m_flScrollAccum[0];
            double flScrollY = m_flScrollAccum[1];
            m_flScrollAccum[0] = 0.0;
            m_flScrollAccum[1] = 0.0;

            if ( flScrollX != 0.0 || flScrollY != 0.0 )
            {
                wlserver_lock();
                wlserver_mousewheel( flScrollX, flScrollY, ++s_uSequence );
                wlserver_unlock();
            }
        }
    }
}
