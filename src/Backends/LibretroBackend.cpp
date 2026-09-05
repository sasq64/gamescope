// A backend that hands each composited frame to a libretro core instead of a display.
//
// This is the headless backend with an exit. gamescope still runs a full Wayland/Xwayland
// session with no output device of its own, but where CHeadlessConnector::Present throws
// the frame away, CLibretroConnector::Present composites it into a dmabuf the core already
// has mapped, and says so down a socket.
//
// The core is the parent process: it made the socketpair, forked us, and passed our end as
// `--libretro-fd`. See src/libretro/gamescope_libretro_ipc.h for the protocol and
// src/libretro/core.cpp for the other side.
//
// Threading follows SDLBackend rather than the steamcompmgr waiter: Present() writes to the
// socket from the steamcompmgr thread, and a backend-owned thread reads input from it and
// injects under wlserver_lock(). The two directions of a SOCK_SEQPACKET socket are
// independent, and each has exactly one writer, so neither needs a lock of its own.

#include "backend.h"
#include "rendervulkan.hpp"
#include "wlserver.hpp"
#include "refresh_rate.h"
#include "log.hpp"
#include "main.hpp"

#include "../libretro/gamescope_libretro_ipc.h"

#include <atomic>
#include <thread>

#include <drm_fourcc.h>
#include <errno.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

extern int g_nPreferredOutputWidth;
extern int g_nPreferredOutputHeight;

// The inherited socket end, set from the --libretro-fd option in main.cpp.
int g_nLibretroFd = -1;

namespace gamescope
{
    static LogScope lr_log( "libretro" );

    // Ring slots the core is not holding. Present() claims one and the reader thread
    // hands it back, so both are touched from two threads and nothing else is.
    static std::atomic<bool> s_bSlotFree[ GSLR_NUM_BUFFERS ];

    static bool SendMessage( int nFd, uint32_t uType, const void *pPayload, size_t uLen,
                             const int *pFds = nullptr, size_t uNumFds = 0 )
    {
        gslr_header header = { uType, uint32_t( uLen ) };

        iovec iov[ 2 ] = {
            { &header, sizeof( header ) },
            { const_cast<void *>( pPayload ), uLen },
        };

        // Enough control space for the largest send we make, the hello's fd array.
        alignas( cmsghdr ) char cmsgBuf[ CMSG_SPACE( sizeof( int ) * GSLR_NUM_BUFFERS ) ] = {};

        msghdr msg = {};
        msg.msg_iov = iov;
        msg.msg_iovlen = uLen ? 2 : 1;

        if ( uNumFds )
        {
            msg.msg_control = cmsgBuf;
            msg.msg_controllen = CMSG_SPACE( sizeof( int ) * uNumFds );

            cmsghdr *pCmsg = CMSG_FIRSTHDR( &msg );
            pCmsg->cmsg_level = SOL_SOCKET;
            pCmsg->cmsg_type = SCM_RIGHTS;
            pCmsg->cmsg_len = CMSG_LEN( sizeof( int ) * uNumFds );
            memcpy( CMSG_DATA( pCmsg ), pFds, sizeof( int ) * uNumFds );
        }

        ssize_t ret;
        do {
            ret = sendmsg( nFd, &msg, MSG_NOSIGNAL );
        } while ( ret < 0 && errno == EINTR );

        return ret >= 0;
    }

    class CLibretroConnector final : public CBaseBackendConnector
    {
    public:
        CLibretroConnector()
        {
        }
        virtual ~CLibretroConnector()
        {
        }

        virtual gamescope::GamescopeScreenType GetScreenType() const override
        {
            return GAMESCOPE_SCREEN_TYPE_INTERNAL;
        }
        virtual GamescopePanelOrientation GetCurrentOrientation() const override
        {
            return GAMESCOPE_PANEL_ORIENTATION_0;
        }
        virtual bool SupportsHDR() const override
        {
            return false;
        }
        virtual bool IsHDRActive() const override
        {
            return false;
        }
        virtual const BackendConnectorHDRInfo &GetHDRInfo() const override
        {
            return m_HDRInfo;
        }
        virtual bool IsVRRActive() const override
        {
            return false;
        }
        virtual std::span<const BackendMode> GetModes() const override
        {
            return std::span<const BackendMode>{};
        }
        virtual bool SupportsVRR() const override
        {
            return false;
        }
        virtual std::span<const uint8_t> GetRawEDID() const override
        {
            return std::span<const uint8_t>{};
        }
        virtual std::span<const uint32_t> GetValidDynamicRefreshRates() const override
        {
            return std::span<const uint32_t>{};
        }

        virtual void GetNativeColorimetry(
            bool bHDR10,
            displaycolorimetry_t *displayColorimetry, EOTF *displayEOTF,
            displaycolorimetry_t *outputEncodingColorimetry, EOTF *outputEncodingEOTF ) const override
        {
            *displayColorimetry = displaycolorimetry_709;
            *displayEOTF = EOTF_Gamma22;
            *outputEncodingColorimetry = displaycolorimetry_709;
            *outputEncodingEOTF = EOTF_Gamma22;
        }

        virtual const char *GetName() const override
        {
            return "Libretro";
        }
        virtual const char *GetMake() const override
        {
            return "Gamescope";
        }
        virtual const char *GetModel() const override
        {
            return "Libretro Core";
        }

        // The whole point of this backend.
        //
        // pFrameInfo is the finished layer list paint_all built for us, so unlike
        // paint_pipewire() nothing has to be repainted first: composite it straight into
        // whichever ring slot the core is not holding, wait for the GPU, and post the slot
        // number. If the core is behind and holds all of them, drop this frame rather than
        // stall the compositor — the next one is 20ms away and will be more current anyway.
        virtual int Present( const FrameInfo_t *pFrameInfo, bool bAsync ) override
        {
            if ( g_nLibretroFd < 0 )
                return 0;

            int nSlot = -1;
            for ( uint32_t i = 0; i < GSLR_NUM_BUFFERS; i++ )
            {
                bool bExpected = true;
                if ( s_bSlotFree[i].compare_exchange_strong( bExpected, false ) )
                {
                    nSlot = int( i );
                    break;
                }
            }

            if ( nSlot < 0 )
                return 0;

            std::optional<uint64_t> oSequence =
                vulkan_screenshot( pFrameInfo, m_Ring[ nSlot ], nullptr );

            if ( !oSequence )
            {
                lr_log.errorf( "vulkan_screenshot failed" );
                s_bSlotFree[ nSlot ] = true;
                return 0;
            }

            vulkan_wait( *oSequence, true );

            gslr_frame frame = {};
            frame.slot = uint32_t( nSlot );
            frame.serial = ++m_ulSerial;

            if ( !SendMessage( g_nLibretroFd, GSLR_MSG_FRAME, &frame, sizeof( frame ) ) )
            {
                // The core is gone. Nothing to present to any more.
                s_bSlotFree[ nSlot ] = true;
                lr_log.infof( "core hung up, shutting down" );
                ShutdownGamescope();
            }

            return 0;
        }

        gamescope::Rc<CVulkanTexture> m_Ring[ GSLR_NUM_BUFFERS ];

    private:
        BackendConnectorHDRInfo m_HDRInfo{};
        uint64_t m_ulSerial = 0;
    };

    class CLibretroBackend final : public CBaseBackend
    {
    public:
        CLibretroBackend()
        {
        }

        virtual ~CLibretroBackend()
        {
            m_bShuttingDown = true;

            if ( g_nLibretroFd >= 0 )
            {
                SendMessage( g_nLibretroFd, GSLR_MSG_BYE, nullptr, 0 );
                // Wake the reader out of recvmsg so it can see m_bShuttingDown.
                shutdown( g_nLibretroFd, SHUT_RDWR );
            }

            if ( m_InputThread.joinable() )
                m_InputThread.join();
        }

        virtual bool Init() override
        {
            if ( g_nLibretroFd < 0 )
            {
                lr_log.errorf( "The libretro backend needs --libretro-fd; it is spawned by a core, not run by hand." );
                return false;
            }

            g_nOutputWidth = g_nPreferredOutputWidth;
            g_nOutputHeight = g_nPreferredOutputHeight;
            g_nOutputRefresh = g_nNestedRefresh;

            if ( g_nOutputHeight == 0 )
            {
                if ( g_nOutputWidth != 0 )
                {
                    fprintf( stderr, "Cannot specify -W without -H\n" );
                    return false;
                }
                g_nOutputHeight = 720;
            }
            if ( g_nOutputWidth == 0 )
                g_nOutputWidth = g_nOutputHeight * 16 / 9;
            if ( g_nOutputRefresh == 0 )
                g_nOutputRefresh = ConvertHztomHz( 60 );

            if ( !vulkan_init( vulkan_get_instance(), VK_NULL_HANDLE ) )
            {
                return false;
            }

            if ( !wlsession_init() )
            {
                fprintf( stderr, "Failed to initialize Wayland session\n" );
                return false;
            }

            return true;
        }

        // The ring cannot be built in Init(): vulkan_init() has run, but allocating
        // textures wants the rest of the render setup main() does right after us.
        virtual bool PostInit() override
        {
            if ( !AllocateRing() )
                return false;

            if ( !SendHello() )
                return false;

            m_InputThread = std::thread( [this]{ InputThread(); } );

            return true;
        }

        virtual std::span<const char *const> GetInstanceExtensions() const override
        {
            return std::span<const char *const>{};
        }
        virtual std::span<const char *const> GetDeviceExtensions( VkPhysicalDevice pVkPhysicalDevice ) const override
        {
            return std::span<const char *const>{};
        }
        virtual VkImageLayout GetPresentLayout() const override
        {
            return VK_IMAGE_LAYOUT_GENERAL;
        }
        virtual void GetPreferredOutputFormat( uint32_t *pPrimaryPlaneFormat, uint32_t *pOverlayPlaneFormat ) const override
        {
            *pPrimaryPlaneFormat = VulkanFormatToDRM( VK_FORMAT_B8G8R8A8_UNORM );
            *pOverlayPlaneFormat = VulkanFormatToDRM( VK_FORMAT_B8G8R8A8_UNORM );
        }
        virtual bool ValidPhysicalDevice( VkPhysicalDevice pVkPhysicalDevice ) const override
        {
            return true;
        }

        virtual void DirtyState( bool bForce, bool bForceModeset ) override
        {
        }

        virtual bool PollState() override
        {
            return false;
        }

        virtual std::shared_ptr<BackendBlob> CreateBackendBlob( const std::type_info &type, std::span<const uint8_t> data ) override
        {
            return std::make_shared<BackendBlob>( data );
        }

        virtual OwningRc<IBackendFb> ImportDmabufToBackend( wlr_dmabuf_attributes *pDmaBuf ) override
        {
            return new CBaseBackendFb();
        }

        virtual bool UsesModifiers() const override
        {
            return false;
        }
        virtual std::span<const uint64_t> GetSupportedModifiers( uint32_t uDrmFormat ) const override
        {
            return std::span<const uint64_t>{};
        }

        virtual IBackendConnector *GetCurrentConnector() override
        {
            return &m_Connector;
        }
        virtual IBackendConnector *GetConnector( GamescopeScreenType eScreenType ) override
        {
            if ( eScreenType == GAMESCOPE_SCREEN_TYPE_INTERNAL )
                return &m_Connector;

            return nullptr;
        }

        virtual bool SupportsPlaneHardwareCursor() const override
        {
            // No display, so no hardware cursor plane. Compositing the cursor in means
            // the core actually sees a pointer in the picture.
            return false;
        }

        virtual bool SupportsTearing() const override
        {
            return false;
        }

        virtual bool UsesVulkanSwapchain() const override
        {
            return false;
        }

        virtual bool IsSessionBased() const override
        {
            return false;
        }

        virtual bool SupportsExplicitSync() const override
        {
            return true;
        }

        virtual bool IsPaused() const override
        {
            return false;
        }

        virtual bool IsVisible() const override
        {
            return true;
        }

        virtual glm::uvec2 CursorSurfaceSize( glm::uvec2 uvecSize ) const override
        {
            return uvecSize;
        }

        virtual bool HackTemporarySetDynamicRefresh( int nRefresh ) override
        {
            return false;
        }

        virtual void HackUpdatePatchedEdid() override
        {
        }

    protected:

        virtual void OnBackendBlobDestroyed( BackendBlob *pBlob ) override
        {
        }

    private:

        // Mappable so the memory is HOST_CACHED and linear; exportable so it carries a
        // dmabuf the core can mmap. Storage and sampled are what the compositing shaders
        // need to write and (for the RGB->NV12 path) read it.
        bool AllocateRing()
        {
            for ( uint32_t i = 0; i < GSLR_NUM_BUFFERS; i++ )
            {
                CVulkanTexture::createFlags flags;
                flags.bMappable = true;
                flags.bTransferDst = true;
                flags.bStorage = true;
                flags.bSampled = true;
                flags.bExportable = true;
                flags.bLinear = true;

                gamescope::OwningRc<CVulkanTexture> pTexture = new CVulkanTexture();
                if ( !pTexture->BInit( g_nOutputWidth, g_nOutputHeight, 1u, DRM_FORMAT_XRGB8888, flags ) )
                {
                    lr_log.errorf( "Failed to allocate capture buffer %u (%ux%u)", i, g_nOutputWidth, g_nOutputHeight );
                    return false;
                }

                m_Connector.m_Ring[i] = pTexture.get();
                s_bSlotFree[i] = true;
            }

            return true;
        }

        bool SendHello()
        {
            gslr_hello hello = {};
            hello.version = GSLR_PROTOCOL_VERSION;
            hello.width = uint32_t( g_nOutputWidth );
            hello.height = uint32_t( g_nOutputHeight );
            hello.drm_format = DRM_FORMAT_XRGB8888;
            hello.num_buffers = GSLR_NUM_BUFFERS;
            hello.refresh_mhz = uint32_t( g_nOutputRefresh );

            int nFds[ GSLR_NUM_BUFFERS ];

            for ( uint32_t i = 0; i < GSLR_NUM_BUFFERS; i++ )
            {
                const wlr_dmabuf_attributes &dma = m_Connector.m_Ring[i]->dmabuf();
                if ( dma.n_planes < 1 || dma.fd[0] < 0 )
                {
                    lr_log.errorf( "Capture buffer %u has no dmabuf to export", i );
                    return false;
                }

                nFds[i] = dma.fd[0];
                hello.stride[i] = dma.stride[0];
                hello.offset[i] = dma.offset[0];
                hello.size[i] = uint64_t( m_Connector.m_Ring[i]->totalSize() );
                hello.modifier = dma.modifier;
            }

            if ( !SendMessage( g_nLibretroFd, GSLR_MSG_HELLO, &hello, sizeof( hello ), nFds, GSLR_NUM_BUFFERS ) )
            {
                lr_log.errorf( "Failed to send hello to the core: %s", strerror( errno ) );
                return false;
            }

            lr_log.infof( "%ux%u @ %u.%03u Hz, %u buffers",
                g_nOutputWidth, g_nOutputHeight,
                g_nOutputRefresh / 1000, g_nOutputRefresh % 1000,
                GSLR_NUM_BUFFERS );

            return true;
        }

        // Reads the core's half of the conversation. Input is injected under
        // wlserver_lock() from here, exactly as SDLBackend does from its own thread.
        void InputThread()
        {
            pthread_setname_np( pthread_self(), "gamescope-lr" );

            // A timestamp for wlserver, which only requires that it increase.
            uint32_t uSequence = 0;

            for ( ;; )
            {
                struct {
                    gslr_header header;
                    union {
                        gslr_input input;
                        gslr_release release;
                    } payload;
                } msg;

                ssize_t ret = recv( g_nLibretroFd, &msg, sizeof( msg ), 0 );

                if ( ret < 0 && errno == EINTR )
                    continue;

                if ( ret <= 0 )
                {
                    if ( !m_bShuttingDown )
                    {
                        lr_log.infof( "core closed the socket, shutting down" );
                        ShutdownGamescope();
                    }
                    return;
                }

                if ( size_t( ret ) < sizeof( gslr_header ) )
                    continue;

                switch ( msg.header.type )
                {
                    case GSLR_MSG_RELEASE:
                    {
                        if ( msg.payload.release.slot < GSLR_NUM_BUFFERS )
                            s_bSlotFree[ msg.payload.release.slot ] = true;
                    }
                    break;

                    case GSLR_MSG_INPUT:
                    {
                        InjectInput( msg.payload.input, ++uSequence );
                    }
                    break;

                    default:
                        break;
                }
            }
        }

        static void InjectInput( const gslr_input &input, uint32_t uSequence )
        {
            switch ( input.type )
            {
                case GSLR_INPUT_KEY:
                    wlserver_lock();
                    wlserver_key( input.code, input.value != 0, uSequence );
                    wlserver_unlock();
                    break;

                case GSLR_INPUT_BUTTON:
                    wlserver_lock();
                    wlserver_mousebutton( int( input.code ), input.value != 0, uSequence );
                    wlserver_unlock();
                    break;

                case GSLR_INPUT_MOTION:
                    wlserver_lock();
                    wlserver_mousemotion( input.x, input.y, uSequence );
                    wlserver_unlock();
                    break;

                case GSLR_INPUT_WARP:
                    wlserver_lock();
                    wlserver_mousewarp( input.x * g_nOutputWidth, input.y * g_nOutputHeight, uSequence, true );
                    wlserver_unlock();
                    break;

                case GSLR_INPUT_WHEEL:
                    wlserver_lock();
                    wlserver_mousewheel( input.x, input.y, uSequence );
                    wlserver_unlock();
                    break;

                default:
                    break;
            }
        }

        CLibretroConnector m_Connector;
        std::thread m_InputThread;
        std::atomic<bool> m_bShuttingDown = false;
    };

    /////////////////////////
    // Backend Instantiator
    /////////////////////////

    template <>
    bool IBackend::Set<CLibretroBackend>()
    {
        return Set( new CLibretroBackend{} );
    }

}
