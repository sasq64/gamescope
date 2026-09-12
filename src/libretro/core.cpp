// gamescope as a libretro core.
//
// The core owns no emulation of its own. It forks a gamescope built with the libretro
// backend (src/Backends/LibretroBackend.cpp), which runs a real Wayland/Xwayland session
// with no display, and hands back each composited frame in a dmabuf this process has
// mapped. retro_run copies nothing: video_refresh is pointed straight at the mapping.
//
// What runs inside that session is decided from the file the frontend loaded -- a .exe
// goes to wine, an .html or a URL to Chrome, anything executable runs as-is -- or is
// named outright by the `gamescope_command` core option.
//
// Two rules from docs/PCEM.md in demarc's tree apply here and are worth restating:
// a core lives in the frontend's address space, so exit() is a bug and every failure has
// to come back as a black frame and a message; and options are legacy v0 SET_VARIABLES
// strings, because demarc answers GET_CORE_OPTIONS_VERSION with 0.

#include "libretro.h"
#include "gamescope_libretro_ipc.h"

#include <algorithm>
#include <string>
#include <vector>

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/dma-buf.h>
#include <linux/input-event-codes.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

// ---------------------------------------------------------------------------
// Frontend callbacks
// ---------------------------------------------------------------------------

retro_environment_t        env_cb;
retro_video_refresh_t      video_cb;
retro_audio_sample_t       audio_cb;
retro_audio_sample_batch_t audio_batch_cb;
retro_input_poll_t         input_poll_cb;
retro_input_state_t        input_state_cb;
retro_log_printf_t         log_cb;

void log_line( retro_log_level level, const char *fmt, ... )
{
    char szBuf[ 1024 ];
    va_list args;
    va_start( args, fmt );
    vsnprintf( szBuf, sizeof( szBuf ), fmt, args );
    va_end( args );

    if ( log_cb )
        log_cb( level, "%s\n", szBuf );
    else
        fprintf( stderr, "[gamescope-core] %s\n", szBuf );
}

// ---------------------------------------------------------------------------
// Core options -- legacy v0 SET_VARIABLES, see the note at the top.
// ---------------------------------------------------------------------------

const retro_variable k_Variables[] = {
    { "gamescope_resolution", "Resolution; 800x600|640x480|960x720|1024x768|1280x720|1280x1024|1920x1080" },
    { "gamescope_refresh",    "Refresh rate (Hz); 60|50|30|72|75|100|120" },
    // Free-form on purpose: the announced list is only what a picker would show, and a
    // value the frontend already holds (demarc's -x gamescope_command=...) beats it.
    { "gamescope_command",    "Command to run; |wine|chrome" },
    // No wine_desktop option: a virtual desktop is `explorer /desktop=` in front of the
    // command, and the command is the frontend's to build -- demarc's capture_meta()
    // puts it there. An option here would be a second way to say it that this end could
    // only honour by rewriting somebody else's argv.
    { "gamescope_expose_wayland", "Give the client gamescope's Wayland socket; false|true" },
    // No sensible list to offer, so the announced default is empty and the value
    // comes from the frontend. demarc points it at the same prefix wine_emu uses.
    { "gamescope_wineprefix", "WINEPREFIX for a wine client; " },
    // Same again: wine's own syntax ("d3dx9_37=n"), passed through unread. demarc fills
    // it in from the DLLs a release ships beside its .exe -- see wine_dll_overrides in
    // src/newsys/windows.rs.
    { "gamescope_wine_dll_overrides", "WINEDLLOVERRIDES for a wine client; " },
    // MESA_GL_VERSION_OVERRIDE for the client, passed through unread. The value that
    // matters is "4.6COMPAT": wine's wglGetProcAddress only hands back the legacy
    // ARB/EXT aliases (glActiveTextureARB and friends) when the current context
    // advertises the extension they belong to, which a core-profile context does not,
    // and a GL demo that resolves its entry points without checking them calls the
    // resulting NULL. Asking Mesa for a compatibility profile puts the strings back.
    // demarc's capture_meta() sets it from wine_gl_compat -- see src/newsys/windows.rs.
    { "gamescope_mesa_gl_version_override", "MESA_GL_VERSION_OVERRIDE for the client; " },
    // Whether teardown ends wine in the prefix -- see StopWineServer. True on its own,
    // because a core left to itself is the only thing that can. A frontend running
    // several sessions in one prefix sets this false and closes the prefix itself once
    // the last of them has gone; demarc's capture_meta() does exactly that.
    { "gamescope_close_prefix", "Shut the WINEPREFIX down on unload; true|false" },
    { nullptr, nullptr },
};

std::string GetOption( const char *pszKey, const char *pszFallback )
{
    retro_variable var = { pszKey, nullptr };
    if ( env_cb && env_cb( RETRO_ENVIRONMENT_GET_VARIABLE, &var ) && var.value && *var.value )
        return var.value;
    return pszFallback;
}

// ---------------------------------------------------------------------------
// Key mapping: RETROK_* (which is SDL1's table) -> Linux evdev KEY_*.
//
// wlserver_key takes evdev codes, and the frontend speaks retro_key, so somebody has to
// own this. Entries left out simply do not reach the guest.
// ---------------------------------------------------------------------------

struct KeyMap { unsigned retro; unsigned evdev; };

const KeyMap k_KeyMap[] = {
    { RETROK_BACKSPACE, KEY_BACKSPACE }, { RETROK_TAB, KEY_TAB },
    { RETROK_RETURN, KEY_ENTER },        { RETROK_PAUSE, KEY_PAUSE },
    { RETROK_ESCAPE, KEY_ESC },          { RETROK_SPACE, KEY_SPACE },
    { RETROK_QUOTE, KEY_APOSTROPHE },    { RETROK_COMMA, KEY_COMMA },
    { RETROK_MINUS, KEY_MINUS },         { RETROK_PERIOD, KEY_DOT },
    { RETROK_SLASH, KEY_SLASH },
    { RETROK_0, KEY_0 }, { RETROK_1, KEY_1 }, { RETROK_2, KEY_2 }, { RETROK_3, KEY_3 },
    { RETROK_4, KEY_4 }, { RETROK_5, KEY_5 }, { RETROK_6, KEY_6 }, { RETROK_7, KEY_7 },
    { RETROK_8, KEY_8 }, { RETROK_9, KEY_9 },
    { RETROK_SEMICOLON, KEY_SEMICOLON }, { RETROK_EQUALS, KEY_EQUAL },
    { RETROK_LEFTBRACKET, KEY_LEFTBRACE }, { RETROK_BACKSLASH, KEY_BACKSLASH },
    { RETROK_RIGHTBRACKET, KEY_RIGHTBRACE }, { RETROK_BACKQUOTE, KEY_GRAVE },
    { RETROK_a, KEY_A }, { RETROK_b, KEY_B }, { RETROK_c, KEY_C }, { RETROK_d, KEY_D },
    { RETROK_e, KEY_E }, { RETROK_f, KEY_F }, { RETROK_g, KEY_G }, { RETROK_h, KEY_H },
    { RETROK_i, KEY_I }, { RETROK_j, KEY_J }, { RETROK_k, KEY_K }, { RETROK_l, KEY_L },
    { RETROK_m, KEY_M }, { RETROK_n, KEY_N }, { RETROK_o, KEY_O }, { RETROK_p, KEY_P },
    { RETROK_q, KEY_Q }, { RETROK_r, KEY_R }, { RETROK_s, KEY_S }, { RETROK_t, KEY_T },
    { RETROK_u, KEY_U }, { RETROK_v, KEY_V }, { RETROK_w, KEY_W }, { RETROK_x, KEY_X },
    { RETROK_y, KEY_Y }, { RETROK_z, KEY_Z },
    { RETROK_DELETE, KEY_DELETE },
    { RETROK_KP0, KEY_KP0 }, { RETROK_KP1, KEY_KP1 }, { RETROK_KP2, KEY_KP2 },
    { RETROK_KP3, KEY_KP3 }, { RETROK_KP4, KEY_KP4 }, { RETROK_KP5, KEY_KP5 },
    { RETROK_KP6, KEY_KP6 }, { RETROK_KP7, KEY_KP7 }, { RETROK_KP8, KEY_KP8 },
    { RETROK_KP9, KEY_KP9 },
    { RETROK_KP_PERIOD, KEY_KPDOT },     { RETROK_KP_DIVIDE, KEY_KPSLASH },
    { RETROK_KP_MULTIPLY, KEY_KPASTERISK }, { RETROK_KP_MINUS, KEY_KPMINUS },
    { RETROK_KP_PLUS, KEY_KPPLUS },      { RETROK_KP_ENTER, KEY_KPENTER },
    { RETROK_UP, KEY_UP }, { RETROK_DOWN, KEY_DOWN },
    { RETROK_RIGHT, KEY_RIGHT }, { RETROK_LEFT, KEY_LEFT },
    { RETROK_INSERT, KEY_INSERT }, { RETROK_HOME, KEY_HOME }, { RETROK_END, KEY_END },
    { RETROK_PAGEUP, KEY_PAGEUP }, { RETROK_PAGEDOWN, KEY_PAGEDOWN },
    { RETROK_F1, KEY_F1 }, { RETROK_F2, KEY_F2 }, { RETROK_F3, KEY_F3 },
    { RETROK_F4, KEY_F4 }, { RETROK_F5, KEY_F5 }, { RETROK_F6, KEY_F6 },
    { RETROK_F7, KEY_F7 }, { RETROK_F8, KEY_F8 }, { RETROK_F9, KEY_F9 },
    { RETROK_F10, KEY_F10 }, { RETROK_F11, KEY_F11 }, { RETROK_F12, KEY_F12 },
    { RETROK_NUMLOCK, KEY_NUMLOCK }, { RETROK_CAPSLOCK, KEY_CAPSLOCK },
    { RETROK_SCROLLOCK, KEY_SCROLLLOCK },
    { RETROK_RSHIFT, KEY_RIGHTSHIFT }, { RETROK_LSHIFT, KEY_LEFTSHIFT },
    { RETROK_RCTRL, KEY_RIGHTCTRL },   { RETROK_LCTRL, KEY_LEFTCTRL },
    { RETROK_RALT, KEY_RIGHTALT },     { RETROK_LALT, KEY_LEFTALT },
    { RETROK_LSUPER, KEY_LEFTMETA },   { RETROK_RSUPER, KEY_RIGHTMETA },
    { RETROK_MENU, KEY_MENU },         { RETROK_PRINT, KEY_SYSRQ },
};

unsigned RetroKeyToEvdev( unsigned uRetroKey )
{
    for ( const KeyMap &map : k_KeyMap )
    {
        if ( map.retro == uRetroKey )
            return map.evdev;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// The session
// ---------------------------------------------------------------------------

struct Buffer
{
    uint8_t *pMap = nullptr;
    size_t   uSize = 0;
    uint32_t uStride = 0;
    uint32_t uOffset = 0;
    int      nFd = -1;
};

struct Session
{
    int   nSocket = -1;
    pid_t nChild = -1;

    Buffer   Buffers[ GSLR_NUM_BUFFERS ];
    uint32_t uNumBuffers = 0;

    unsigned uWidth = 800;
    unsigned uHeight = 600;
    double   flFps = 60.0;

    // What the last frame's client actually covered of that, reported to the frontend
    // as the geometry's base size. 0 until gamescope has told us.
    unsigned uUsedWidth = 0;
    unsigned uUsedHeight = 0;

    // The last frame we handed to video_refresh, kept so a tick with nothing new can
    // repeat it rather than flash black.
    int nLastSlot = -1;

    bool bRunning = false;
    bool bReportedExit = false;

    // Set when the client was wine, so teardown knows to shut the prefix down.
    std::string strWinePrefix;

    // Frames of silence owed to the frontend, and the pointer state we last sent.
    std::vector<int16_t> Silence;
    int16_t  nLastMouseButtons = 0;
} g_Session;

// A frame's worth of pixels converted into a scratch buffer only when the compositor
// hands us a stride that XRGB8888 video_refresh cannot describe. Normally unused:
// video_refresh takes a pitch, so the mapping is passed through untouched.
std::vector<uint8_t> g_Scratch;

bool SendControl( uint32_t uType, const void *pPayload, size_t uLen )
{
    if ( g_Session.nSocket < 0 )
        return false;

    gslr_header header = { uType, uint32_t( uLen ) };
    iovec iov[ 2 ] = {
        { &header, sizeof( header ) },
        { const_cast<void *>( pPayload ), uLen },
    };

    msghdr msg = {};
    msg.msg_iov = iov;
    msg.msg_iovlen = uLen ? 2 : 1;

    ssize_t ret;
    do {
        ret = sendmsg( g_Session.nSocket, &msg, MSG_NOSIGNAL );
    } while ( ret < 0 && errno == EINTR );

    return ret >= 0;
}

void SendInput( uint32_t uType, uint32_t uCode, int32_t nValue, float flX, float flY )
{
    gslr_input input = {};
    input.type = uType;
    input.code = uCode;
    input.value = nValue;
    input.x = flX;
    input.y = flY;
    SendControl( GSLR_MSG_INPUT, &input, sizeof( input ) );
}

// ---------------------------------------------------------------------------
// Finding the compositor
//
// A downloaded core has the compositor unpacked beside it (see
// .github/workflows/libretro.yml, which zips the two together); in a build tree
// gamescope sits beside this library too, which is how a locally built core is tested
// (demarc's DEMARC_CORE_DIR points straight at the meson build directory). An installed
// core falls back to PATH.
// ---------------------------------------------------------------------------

// The directory the frontend loaded this library from, empty if it will not say.
//
// Asked of the frontend rather than worked out from `dladdr`, because the two answer
// different questions: a frontend may load us from a private copy in a temp directory
// -- which is exactly what demarc does so that two instances of a core do not share
// globals -- and nothing useful sits next to that copy. GET_LIBRETRO_PATH names the
// library as it lives on disk, which is where the compositor was unpacked.
std::string LibraryDir()
{
    const char *pszPath = nullptr;
    if ( !env_cb || !env_cb( RETRO_ENVIRONMENT_GET_LIBRETRO_PATH, &pszPath ) || !pszPath )
        return {};

    const char *pszSlash = strrchr( pszPath, '/' );
    if ( !pszSlash )
        return {};

    return std::string( pszPath, pszSlash - pszPath );
}

std::string FindGamescope()
{
    if ( const char *pszOverride = getenv( "GAMESCOPE_LIBRETRO_BIN" ) )
        return pszOverride;

    std::vector<std::string> candidates;

    std::string strDir = LibraryDir();
    if ( !strDir.empty() )
        candidates.push_back( strDir + "/gamescope" );

    // The build and install locations are baked in, and are what a core loaded from a
    // copy of itself has left to go on.
    candidates.push_back( GAMESCOPE_BUILD_BIN );
    candidates.push_back( GAMESCOPE_INSTALL_BIN );

    for ( const std::string &strCandidate : candidates )
    {
        if ( access( strCandidate.c_str(), X_OK ) == 0 )
            return strCandidate;
    }

    return "gamescope";
}

// ---------------------------------------------------------------------------
// What to run inside the session
// ---------------------------------------------------------------------------

std::string ToLower( std::string str )
{
    std::transform( str.begin(), str.end(), str.begin(),
                    []( unsigned char c ){ return char( tolower( c ) ); } );
    return str;
}

bool EndsWith( const std::string &str, const char *pszSuffix )
{
    size_t uLen = strlen( pszSuffix );
    return str.size() >= uLen && str.compare( str.size() - uLen, uLen, pszSuffix ) == 0;
}

// Read the requested session size into the Session, once.
//
// Both the client command and the compositor's arguments need it, and the client is built
// first -- so this cannot wait until SpawnCompositor, which is where it used to happen and
// where Chrome's --window-size ended up quietly reading the 800x600 default instead of
// whatever gamescope_resolution asked for.
void ReadSessionSize()
{
    std::string strRes = GetOption( "gamescope_resolution", "800x600" );

    unsigned uWidth = 0, uHeight = 0;
    if ( sscanf( strRes.c_str(), "%ux%u", &uWidth, &uHeight ) == 2 && uWidth && uHeight )
    {
        g_Session.uWidth = uWidth;
        g_Session.uHeight = uHeight;
    }
    else
    {
        log_line( RETRO_LOG_WARN, "unreadable gamescope_resolution '%s', using %ux%u",
                  strRes.c_str(), g_Session.uWidth, g_Session.uHeight );
    }

    double flRefresh = atof( GetOption( "gamescope_refresh", "60" ).c_str() );
    g_Session.flFps = flRefresh > 0.0 ? flRefresh : 60.0;
}

// Whichever Chrome is installed. Chromium first: it is what a distro package provides,
// and Google's build is the more likely of the two to be missing.
const char *ChromeBinary()
{
    for ( const char *pszCandidate : { "/usr/bin/chromium", "/usr/bin/google-chrome-stable",
                                       "/usr/bin/google-chrome", "/usr/bin/chrome" } )
    {
        if ( access( pszCandidate, X_OK ) == 0 )
            return pszCandidate;
    }
    return "chromium";
}

// A scratch profile under the frontend's save directory, so nothing here lands in the
// user's own browser profile and a crashed session has somewhere to leave its lock.
std::string ProfileDir()
{
    const char *pszSaveDir = nullptr;
    if ( env_cb )
        env_cb( RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY, &pszSaveDir );

    std::string strBase = ( pszSaveDir && *pszSaveDir ) ? pszSaveDir : "/tmp";
    std::string strDir = strBase + "/gamescope-chrome";
    mkdir( strDir.c_str(), 0700 );
    return strDir;
}

// The separator demarc puts between the words of a command it built itself. ASCII
// US: it exists for exactly this and cannot occur in a path, which spaces very much
// can -- demo filenames are full of them, and the wine command demarc sends has a
// demo path and a driver path in it.
const char k_chArgSeparator = '\x1f';

// Split a command option into an argv.
//
// On the separator when there is one, which is a command assembled by the frontend
// and already split; on whitespace otherwise, which is a command someone typed
// (`-x gamescope_command="vkcube --gpu 0"`). Deliberately naive in the second case:
// it exists for testing, not to be a shell.
std::vector<std::string> SplitWords( const std::string &str )
{
    std::vector<std::string> words;

    if ( str.find( k_chArgSeparator ) != std::string::npos )
    {
        size_t start = 0;
        while ( start <= str.size() )
        {
            size_t at = str.find( k_chArgSeparator, start );
            if ( at == std::string::npos )
                at = str.size();
            if ( at > start )
                words.push_back( str.substr( start, at - start ) );
            start = at + 1;
        }
        return words;
    }

    size_t i = 0;
    while ( i < str.size() )
    {
        while ( i < str.size() && isspace( (unsigned char)str[i] ) ) i++;
        size_t start = i;
        while ( i < str.size() && !isspace( (unsigned char)str[i] ) ) i++;
        if ( i > start )
            words.push_back( str.substr( start, i - start ) );
    }
    return words;
}

struct Client
{
    std::vector<std::string> argv;

    // Ask gamescope to publish its Wayland socket to the client.
    //
    // Off by default because gamescope's Xwayland is what most things want, and because
    // gamescope otherwise sets WAYLAND_DISPLAY to the empty string -- which is fine for
    // anything that checks it properly. Chrome does not: Ozone reads "set" as "Wayland
    // is available", tries to connect to "", and exits. Giving it a real socket is both
    // the fix and the better path, since it skips Xwayland entirely.
    bool bExposeWayland = false;

    // Where the client should be started from; empty leaves it wherever we are.
    std::string strWorkDir;
};

Client BuildClient( const std::string &strPath )
{
    std::string strCommand = GetOption( "gamescope_command", "" );

    // An explicit command wins, and is the only way to run something that is not a file
    // -- which is how the backend gets tested against glxgears or a terminal, and how
    // demarc sends the whole wine command it would otherwise have run on top of itself:
    // the dialog driver, the demo, and a virtual desktop around them if one was asked
    // for. See capture_meta() in demarc's src/newsys/windows.rs.
    if ( !strCommand.empty() && strCommand != "wine" && strCommand != "chrome" )
    {
        Client client;
        client.argv = SplitWords( strCommand );
        client.bExposeWayland = GetOption( "gamescope_expose_wayland", "false" ) == "true";
        // The release's own directory, for the same reason as below: a demo that ships
        // a data/ folder or its own fmod.dll finds neither from anywhere else. The
        // command may name a program that has nothing to do with the loaded file, but
        // the directory of the file we were given is still the best guess there is, and
        // for a command wrapped around that very file it is the right one.
        size_t uSlash = strPath.rfind( '/' );
        if ( uSlash != std::string::npos )
            client.strWorkDir = strPath.substr( 0, uSlash );
        return client;
    }

    std::string strLower = ToLower( strPath );

    bool bWine   = strCommand == "wine"   || EndsWith( strLower, ".exe" );
    bool bChrome = strCommand == "chrome" || EndsWith( strLower, ".html" ) ||
                   EndsWith( strLower, ".htm" ) || strLower.rfind( "http", 0 ) == 0;

    if ( bChrome )
    {
        std::string strUrl = strPath;
        if ( strLower.rfind( "http", 0 ) != 0 )
            strUrl = "file://" + strPath;

        Client client;
        // Xwayland rather than gamescope's own Wayland socket, for two reasons that
        // both showed up the moment this was tried the other way round: Chrome's Ozone
        // Wayland backend refuses to use Vulkan, and it draws its own decorations and
        // ignores being told to go fullscreen. Under X11, gamescope's window manager --
        // which exists to make exactly one window fill exactly one screen -- does it.
        client.argv = {
            ChromeBinary(),
            "--ozone-platform=x11",
            // Its own profile, so a demo never touches the user's browser, and two of
            // them never fight over one profile directory.
            "--user-data-dir=" + ProfileDir(),
            "--app=" + strUrl,
            // --app drops the tab strip and omnibox but still gives the window a
            // title bar and its own idea of how big to be; --kiosk is what actually
            // fills the session, which is the only thing being captured.
            "--kiosk",
            "--start-fullscreen",
            // The session is exactly the size we asked for and every CSS pixel in it is
            // one captured pixel. Without this Chrome inherits whatever HiDPI scale the
            // desktop that launched demarc is using, and a page written for 800x600
            // arrives cropped.
            "--force-device-scale-factor=1",
            "--window-size=" + std::to_string( g_Session.uWidth ) + "," +
                               std::to_string( g_Session.uHeight ),
            "--window-position=0,0",
            "--no-first-run",
            "--no-default-browser-check",
            "--noerrdialogs",
            "--disable-infobars",
            "--disable-session-crashed-bubble",
            // A demo is not a user gesture, and nothing here can click "play".
            "--autoplay-policy=no-user-gesture-required",
        };
        return client;
    }

    Client client;
    client.argv = bWine ? std::vector<std::string>{ "wine", strPath }
                        : std::vector<std::string>{ strPath };

    // A release that ships a data/ folder or its own fmod.dll finds neither when
    // started from wherever the frontend happened to be launched, and fails
    // silently. Same reasoning as wine_emu.rs's current_dir().
    size_t uSlash = strPath.rfind( '/' );
    if ( uSlash != std::string::npos )
        client.strWorkDir = strPath.substr( 0, uSlash );

    return client;
}

// ---------------------------------------------------------------------------
// Session lifetime
// ---------------------------------------------------------------------------

// wine's services are not ours to kill.
//
// winedevice.exe calls setsid() and leaves the session's process group entirely, so no
// signal aimed at the group reaches it, and one accumulates per demo -- wine_emu.rs found
// "thirty-seven of them left by earlier sessions". `wineserver -k` is wine's own way to
// end a prefix, so use that rather than hunting processes. Must run while the session is
// still up; see the call site.
//
// Wholesale, like wine_emu.rs's close_prefix: it ends every wine process in the prefix,
// so two wine sessions sharing one prefix cannot be closed independently. Which is why
// the frontend can take it over: `gamescope_close_prefix=false` leaves strWinePrefix
// empty and this a no-op, and demarc -- who is the only one that knows how many sessions
// it has in there -- closes the prefix once the last of them is gone. See PrefixGuard in
// src/wine_emu.rs.
void StopWineServer()
{
    if ( g_Session.strWinePrefix.empty() )
        return;

    pid_t nPid = fork();
    if ( nPid < 0 )
        return;

    if ( nPid == 0 )
    {
        setenv( "WINEPREFIX", g_Session.strWinePrefix.c_str(), 1 );
        // Nothing should see this; the point is to be quiet, not to be watched.
        int nNull = open( "/dev/null", O_WRONLY );
        if ( nNull >= 0 )
        {
            dup2( nNull, STDOUT_FILENO );
            dup2( nNull, STDERR_FILENO );
        }
        execlp( "wineserver", "wineserver", "-k", nullptr );
        _exit( 127 );
    }

    // It exits as soon as it has told the server to go; a bounded wait keeps a missing
    // wineserver from holding up the unload.
    for ( int i = 0; i < 300; i++ )
    {
        if ( waitpid( nPid, nullptr, WNOHANG ) == nPid )
            return;
        usleep( 10 * 1000 );
    }

    kill( nPid, SIGKILL );
    waitpid( nPid, nullptr, 0 );
}

void CloseSession()
{
    if ( g_Session.nSocket >= 0 )
    {
        shutdown( g_Session.nSocket, SHUT_RDWR );
        close( g_Session.nSocket );
        g_Session.nSocket = -1;
    }

    if ( g_Session.nChild > 0 )
    {
        // gamescope is a session leader, so its whole tree -- Xwayland, gamescopereaper,
        // the client -- shares its process group and can be signalled at once.
        const pid_t nGroup = g_Session.nChild;

        // Before any of that, though: wine has to be shut down through wine.
        //
        // `wineserver -k` talks to the running wineserver, and wineserver is inside the
        // group. Kill the group first and the server dies with it, leaving winedevice.exe
        // -- which setsid()s out of the group and so survives -- orphaned with nothing
        // left that can reach it. Verified the wrong way round first: the process stayed
        // up, and a later `wineserver -k` by hand could not touch it either.
        StopWineServer();

        kill( -nGroup, SIGTERM );

        bool bLeaderGone = false;
        for ( int i = 0; i < 200 && !bLeaderGone; i++ )
        {
            if ( waitpid( g_Session.nChild, nullptr, WNOHANG ) == g_Session.nChild )
                bLeaderGone = true;
            else
                usleep( 10 * 1000 );
        }

        if ( !bLeaderGone )
        {
            kill( -nGroup, SIGKILL );
            waitpid( g_Session.nChild, nullptr, 0 );
        }

        g_Session.nChild = -1;

        // The leader dying is not the group dying, and this is where it went wrong the
        // first time: gamescope exits promptly on SIGTERM, we reap it, and gamescopereaper
        // is left running because nothing ever escalated. Kill the group unconditionally
        // once the leader is gone -- ESRCH here just means it was already empty.
        kill( -nGroup, SIGKILL );
    }

    for ( Buffer &buffer : g_Session.Buffers )
    {
        if ( buffer.pMap )
            munmap( buffer.pMap, buffer.uSize );
        if ( buffer.nFd >= 0 )
            close( buffer.nFd );
        buffer = Buffer{};
    }

    g_Session.uNumBuffers = 0;
    g_Session.nLastSlot = -1;
    g_Session.bRunning = false;
    g_Session.strWinePrefix.clear();
}

bool SpawnCompositor( const Client &client )
{
    int sv[ 2 ] = { -1, -1 };
    if ( socketpair( AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sv ) != 0 )
    {
        log_line( RETRO_LOG_ERROR, "socketpair failed: %s", strerror( errno ) );
        return false;
    }

    std::string strBin = FindGamescope();
    log_line( RETRO_LOG_INFO, "compositor: %s", strBin.c_str() );

    const unsigned uWidth = g_Session.uWidth;
    const unsigned uHeight = g_Session.uHeight;
    const std::string strRefresh = GetOption( "gamescope_refresh", "60" );

    // The child needs the socket on a fixed, non-CLOEXEC descriptor.
    const int k_nChildFd = 3;

    std::vector<std::string> args = {
        strBin,
        "--backend", "libretro",
        "--libretro-fd", std::to_string( k_nChildFd ),
        // -W/-H size the output we capture; -w/-h size what the client is told it has.
        // They are the same here: there is no display to letterbox into, so scaling the
        // session would only cost sharpness. -f makes the client fullscreen, without
        // which an --app window keeps its decorations and sits in the middle.
        "-W", std::to_string( uWidth ),
        "-H", std::to_string( uHeight ),
        "-w", std::to_string( uWidth ),
        "-h", std::to_string( uHeight ),
        "-r", strRefresh,
        "-f",
    };

    if ( client.bExposeWayland )
        args.push_back( "--expose-wayland" );

    args.push_back( "--" );
    args.insert( args.end(), client.argv.begin(), client.argv.end() );

    std::vector<char *> argv;
    for ( std::string &arg : args )
        argv.push_back( const_cast<char *>( arg.c_str() ) );
    argv.push_back( nullptr );

    {
        std::string strLine;
        for ( const std::string &arg : args )
            strLine += arg + " ";
        log_line( RETRO_LOG_INFO, "spawning: %s", strLine.c_str() );
    }

    // Assembled before the fork: between fork and exec only async-signal-safe calls
    // are allowed, and building these strings is not one.
    std::vector<std::pair<std::string, std::string>> childEnv;

    std::string strPrefix = GetOption( "gamescope_wineprefix", "" );
    if ( !strPrefix.empty() )
    {
        childEnv.emplace_back( "WINEPREFIX", strPrefix );
        // Recording it is what arms StopWineServer, so a frontend that owns the prefix's
        // lifetime turns teardown off simply by not letting it be recorded.
        const bool bClosePrefix = GetOption( "gamescope_close_prefix", "true" ) != "false";
        if ( bClosePrefix && !client.argv.empty() && client.argv[0] == "wine" )
            g_Session.strWinePrefix = strPrefix;
        else if ( !bClosePrefix )
            log_line( RETRO_LOG_INFO, "Leaving the wine prefix %s to the frontend to close",
                      strPrefix.c_str() );
    }

    std::string strDllOverrides = GetOption( "gamescope_wine_dll_overrides", "" );
    if ( !strDllOverrides.empty() )
    {
        childEnv.emplace_back( "WINEDLLOVERRIDES", strDllOverrides );
        log_line( RETRO_LOG_INFO, "WINEDLLOVERRIDES=%s", strDllOverrides.c_str() );
    }

    // Only the client gets this, not the gamescope we are running inside: the child is
    // the only thing forked here, and gamescope composites through Vulkan anyway.
    std::string strGlVersion = GetOption( "gamescope_mesa_gl_version_override", "" );
    if ( !strGlVersion.empty() )
    {
        childEnv.emplace_back( "MESA_GL_VERSION_OVERRIDE", strGlVersion );
        log_line( RETRO_LOG_INFO, "MESA_GL_VERSION_OVERRIDE=%s", strGlVersion.c_str() );
    }

    // wine is loud enough on its own to fill a pipe. The frontend can turn it back
    // up by exporting WINEDEBUG before it starts.
    if ( !getenv( "WINEDEBUG" ) )
        childEnv.emplace_back( "WINEDEBUG", "-all" );

    pid_t nPid = fork();
    if ( nPid < 0 )
    {
        log_line( RETRO_LOG_ERROR, "fork failed: %s", strerror( errno ) );
        close( sv[0] );
        close( sv[1] );
        return false;
    }

    if ( nPid == 0 )
    {
        // Child. Nothing here may allocate or touch the frontend's state.
        close( sv[0] );

        if ( dup2( sv[1], k_nChildFd ) < 0 )
            _exit( 127 );
        if ( sv[1] != k_nChildFd )
            close( sv[1] );

        // Its own session, so killing the group takes the whole tree; and if the
        // frontend dies without unloading us, the kernel cleans up.
        setsid();
        prctl( PR_SET_PDEATHSIG, SIGKILL );

        for ( const auto &env : childEnv )
            setenv( env.first.c_str(), env.second.c_str(), 1 );

        if ( !client.strWorkDir.empty() )
            (void)chdir( client.strWorkDir.c_str() );

        execvp( argv[0], argv.data() );
        _exit( 127 );
    }

    close( sv[1] );

    g_Session.nSocket = sv[0];
    g_Session.nChild = nPid;

    return true;
}

// Blocks for the hello, which cannot arrive until gamescope has a Vulkan device and an
// Xwayland up. That is seconds on a cold start, so the timeout is generous; it is paid
// once, on the loading screen, and a core that gave up early would simply never work.
bool AwaitHello( int nTimeoutMs )
{
    pollfd pfd = { g_Session.nSocket, POLLIN, 0 };

    for ( int nElapsed = 0; nElapsed < nTimeoutMs; nElapsed += 100 )
    {
        int ret = poll( &pfd, 1, 100 );

        if ( ret < 0 && errno == EINTR )
            continue;

        if ( ret < 0 )
        {
            log_line( RETRO_LOG_ERROR, "poll failed: %s", strerror( errno ) );
            return false;
        }

        // The compositor dying before it says hello is the common failure -- a missing
        // Xwayland, no Vulkan device, a bad option. Its stderr is the real message.
        if ( g_Session.nChild > 0 &&
             waitpid( g_Session.nChild, nullptr, WNOHANG ) == g_Session.nChild )
        {
            g_Session.nChild = -1;
            log_line( RETRO_LOG_ERROR, "gamescope exited before it produced a frame" );
            return false;
        }

        if ( ret == 0 )
            continue;

        struct {
            gslr_header header;
            gslr_hello  hello;
        } msg = {};

        int nFds[ GSLR_NUM_BUFFERS ] = { -1, -1, -1 };

        iovec iov = { &msg, sizeof( msg ) };
        alignas( cmsghdr ) char cmsgBuf[ CMSG_SPACE( sizeof( int ) * GSLR_NUM_BUFFERS ) ] = {};

        msghdr hdr = {};
        hdr.msg_iov = &iov;
        hdr.msg_iovlen = 1;
        hdr.msg_control = cmsgBuf;
        hdr.msg_controllen = sizeof( cmsgBuf );

        ssize_t got = recvmsg( g_Session.nSocket, &hdr, MSG_CMSG_CLOEXEC );
        if ( got < 0 && errno == EINTR )
            continue;

        if ( got <= 0 )
        {
            log_line( RETRO_LOG_ERROR, "gamescope closed the socket before saying hello" );
            return false;
        }

        if ( msg.header.type != GSLR_MSG_HELLO || size_t( got ) < sizeof( msg ) )
        {
            log_line( RETRO_LOG_ERROR, "expected a hello, got message type %u", msg.header.type );
            return false;
        }

        if ( msg.hello.version != GSLR_PROTOCOL_VERSION )
        {
            log_line( RETRO_LOG_ERROR, "protocol mismatch: core %u, gamescope %u",
                      GSLR_PROTOCOL_VERSION, msg.hello.version );
            return false;
        }

        cmsghdr *pCmsg = CMSG_FIRSTHDR( &hdr );
        if ( !pCmsg || pCmsg->cmsg_level != SOL_SOCKET || pCmsg->cmsg_type != SCM_RIGHTS )
        {
            log_line( RETRO_LOG_ERROR, "hello arrived without its buffers" );
            return false;
        }

        unsigned uNumFds = ( pCmsg->cmsg_len - CMSG_LEN( 0 ) ) / sizeof( int );
        uNumFds = std::min<unsigned>( uNumFds, GSLR_NUM_BUFFERS );
        memcpy( nFds, CMSG_DATA( pCmsg ), sizeof( int ) * uNumFds );

        if ( uNumFds != msg.hello.num_buffers )
        {
            log_line( RETRO_LOG_ERROR, "hello promised %u buffers but sent %u",
                      msg.hello.num_buffers, uNumFds );
            for ( unsigned i = 0; i < uNumFds; i++ )
                close( nFds[i] );
            return false;
        }

        for ( unsigned i = 0; i < uNumFds; i++ )
        {
            void *pMap = mmap( nullptr, msg.hello.size[i], PROT_READ, MAP_SHARED, nFds[i], 0 );
            if ( pMap == MAP_FAILED )
            {
                log_line( RETRO_LOG_ERROR, "could not map buffer %u (%llu bytes): %s",
                          i, (unsigned long long)msg.hello.size[i], strerror( errno ) );
                for ( unsigned j = 0; j < uNumFds; j++ )
                    close( nFds[j] );
                return false;
            }

            g_Session.Buffers[i].pMap = static_cast<uint8_t *>( pMap );
            g_Session.Buffers[i].uSize = msg.hello.size[i];
            g_Session.Buffers[i].uStride = msg.hello.stride[i];
            g_Session.Buffers[i].uOffset = msg.hello.offset[i];
            g_Session.Buffers[i].nFd = nFds[i];
        }

        g_Session.uNumBuffers = uNumFds;
        g_Session.uWidth = msg.hello.width;
        g_Session.uHeight = msg.hello.height;
        if ( msg.hello.refresh_mhz )
            g_Session.flFps = msg.hello.refresh_mhz / 1000.0;

        log_line( RETRO_LOG_INFO, "session up: %ux%u @ %.3f Hz, %u buffers",
                  g_Session.uWidth, g_Session.uHeight, g_Session.flFps, uNumFds );
        return true;
    }

    log_line( RETRO_LOG_ERROR, "gamescope produced no frame within %d seconds", nTimeoutMs / 1000 );
    return false;
}

// A dmabuf mapping is not coherent by contract; the sync ioctls are how the kernel is
// told a CPU read is starting and finishing. On a linear host-visible allocation this
// costs nothing, and skipping it is the kind of bug that only shows on other hardware.
void DmaSync( const Buffer &buffer, uint64_t uFlags )
{
    if ( buffer.nFd < 0 )
        return;

    dma_buf_sync sync = {};
    sync.flags = uFlags | DMA_BUF_SYNC_READ;

    int ret;
    do {
        ret = ioctl( buffer.nFd, DMA_BUF_IOCTL_SYNC, &sync );
    } while ( ret < 0 && errno == EINTR );
}

void PublishFrame( int nSlot )
{
    const Buffer &buffer = g_Session.Buffers[ nSlot ];
    if ( !buffer.pMap )
        return;

    DmaSync( buffer, DMA_BUF_SYNC_START );
    video_cb( buffer.pMap + buffer.uOffset, g_Session.uWidth, g_Session.uHeight, buffer.uStride );
    DmaSync( buffer, DMA_BUF_SYNC_END );

    // The frontend copied the pixels inside video_refresh, so the slot is free again.
    gslr_release release = {};
    release.slot = uint32_t( nSlot );
    SendControl( GSLR_MSG_RELEASE, &release, sizeof( release ) );
}

// A client that changes mode changes how much of the session it covers, which is the
// only thing the frontend can use to tell a 4:3 release in a 16:9 session from a
// borderless one. The frame keeps arriving whole, border included, so the geometry's
// display aspect stays the session's and only the base size follows the client.
void SetUsedSize( unsigned uWidth, unsigned uHeight )
{
    if ( !uWidth || !uHeight )
        return;

    if ( uWidth == g_Session.uUsedWidth && uHeight == g_Session.uUsedHeight )
        return;

    g_Session.uUsedWidth = uWidth;
    g_Session.uUsedHeight = uHeight;

    if ( !env_cb )
        return;

    retro_game_geometry geom = {};
    geom.base_width = uWidth;
    geom.base_height = uHeight;
    geom.max_width = g_Session.uWidth;
    geom.max_height = g_Session.uHeight;
    geom.aspect_ratio = float( g_Session.uWidth ) / float( g_Session.uHeight );

    env_cb( RETRO_ENVIRONMENT_SET_GEOMETRY, &geom );

    log_line( RETRO_LOG_INFO, "client covers %ux%u of %ux%u",
              uWidth, uHeight, g_Session.uWidth, g_Session.uHeight );
}

// Reads whatever the compositor has queued, keeping only the newest frame: if we fell
// behind, the older ones are stale by definition and showing them would only add lag.
// Returns the slot to present, or -1 for nothing new.
int DrainFrames()
{
    int nNewest = -1;
    unsigned uUsedWidth = 0, uUsedHeight = 0;

    for ( ;; )
    {
        pollfd pfd = { g_Session.nSocket, POLLIN, 0 };
        int ret = poll( &pfd, 1, 0 );

        if ( ret < 0 && errno == EINTR )
            continue;
        if ( ret <= 0 )
            break;

        struct {
            gslr_header header;
            gslr_frame  frame;
        } msg = {};

        ssize_t got = recv( g_Session.nSocket, &msg, sizeof( msg ), 0 );

        if ( got < 0 && errno == EINTR )
            continue;

        if ( got <= 0 )
        {
            if ( !g_Session.bReportedExit )
            {
                log_line( RETRO_LOG_INFO, "gamescope has gone" );
                g_Session.bReportedExit = true;
                g_Session.bRunning = false;
            }
            break;
        }

        if ( msg.header.type == GSLR_MSG_FRAME )
        {
            // Give back the frame we are about to skip over rather than hold two.
            if ( nNewest >= 0 )
            {
                gslr_release release = {};
                release.slot = uint32_t( nNewest );
                SendControl( GSLR_MSG_RELEASE, &release, sizeof( release ) );
            }
            nNewest = int( msg.frame.slot );
            uUsedWidth = msg.frame.used_width;
            uUsedHeight = msg.frame.used_height;
        }
        else if ( msg.header.type == GSLR_MSG_BYE )
        {
            g_Session.bRunning = false;
            break;
        }
    }

    if ( nNewest >= 0 && unsigned( nNewest ) >= g_Session.uNumBuffers )
        return -1;

    if ( nNewest >= 0 )
        SetUsedSize( uUsedWidth, uUsedHeight );

    return nNewest;
}

void PumpInput()
{
    if ( !input_poll_cb || !input_state_cb )
        return;

    input_poll_cb();

    // Keyboard arrives through the keyboard callback, not from here -- polling 320 keys
    // a frame to rediscover edges the frontend already knows about would be silly.

    int16_t nDx = input_state_cb( 0, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_X );
    int16_t nDy = input_state_cb( 0, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_Y );

    if ( nDx || nDy )
        SendInput( GSLR_INPUT_MOTION, 0, 0, float( nDx ), float( nDy ) );

    const struct { unsigned id; unsigned btn; int16_t bit; } k_Buttons[] = {
        { RETRO_DEVICE_ID_MOUSE_LEFT,   BTN_LEFT,   1 },
        { RETRO_DEVICE_ID_MOUSE_RIGHT,  BTN_RIGHT,  2 },
        { RETRO_DEVICE_ID_MOUSE_MIDDLE, BTN_MIDDLE, 4 },
    };

    for ( const auto &button : k_Buttons )
    {
        bool bDown = input_state_cb( 0, RETRO_DEVICE_MOUSE, 0, button.id ) != 0;
        bool bWas = ( g_Session.nLastMouseButtons & button.bit ) != 0;

        if ( bDown != bWas )
        {
            SendInput( GSLR_INPUT_BUTTON, button.btn, bDown ? 1 : 0, 0.0f, 0.0f );
            if ( bDown )
                g_Session.nLastMouseButtons |= button.bit;
            else
                g_Session.nLastMouseButtons &= ~button.bit;
        }
    }
}

void KeyboardEvent( bool bDown, unsigned uKeycode, uint32_t uCharacter, uint16_t uKeyModifiers )
{
    (void)uCharacter;
    (void)uKeyModifiers;

    if ( !g_Session.bRunning )
        return;

    if ( unsigned uEvdev = RetroKeyToEvdev( uKeycode ) )
        SendInput( GSLR_INPUT_KEY, uEvdev, bDown ? 1 : 0, 0.0f, 0.0f );
}

} // namespace

// ---------------------------------------------------------------------------
// libretro entry points
// ---------------------------------------------------------------------------

extern "C" {

RETRO_API unsigned retro_api_version( void )
{
    return RETRO_API_VERSION;
}

RETRO_API void retro_get_system_info( retro_system_info *info )
{
    memset( info, 0, sizeof( *info ) );
    info->library_name = "gamescope";
    info->library_version = "0.1";
    info->valid_extensions = "exe|html|htm|sh";
    info->need_fullpath = true;
    info->block_extract = true;
}

RETRO_API void retro_get_system_av_info( retro_system_av_info *info )
{
    memset( info, 0, sizeof( *info ) );
    info->geometry.base_width = g_Session.uWidth;
    info->geometry.base_height = g_Session.uHeight;
    info->geometry.max_width = g_Session.uWidth;
    info->geometry.max_height = g_Session.uHeight;
    info->geometry.aspect_ratio = float( g_Session.uWidth ) / float( g_Session.uHeight );
    info->timing.fps = g_Session.flFps;
    info->timing.sample_rate = 48000.0;
}

RETRO_API void retro_set_environment( retro_environment_t cb )
{
    env_cb = cb;

    cb( RETRO_ENVIRONMENT_SET_VARIABLES, (void *)k_Variables );

    bool bNoGame = true;
    cb( RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME, &bNoGame );

    retro_keyboard_callback keyboard = { KeyboardEvent };
    cb( RETRO_ENVIRONMENT_SET_KEYBOARD_CALLBACK, &keyboard );
}

RETRO_API void retro_set_video_refresh( retro_video_refresh_t cb ) { video_cb = cb; }
RETRO_API void retro_set_audio_sample( retro_audio_sample_t cb ) { audio_cb = cb; }
RETRO_API void retro_set_audio_sample_batch( retro_audio_sample_batch_t cb ) { audio_batch_cb = cb; }
RETRO_API void retro_set_input_poll( retro_input_poll_t cb ) { input_poll_cb = cb; }
RETRO_API void retro_set_input_state( retro_input_state_t cb ) { input_state_cb = cb; }

RETRO_API void retro_init( void )
{
    retro_log_callback logging = {};
    if ( env_cb && env_cb( RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &logging ) )
        log_cb = logging.log;
}

RETRO_API void retro_deinit( void )
{
    CloseSession();
    log_cb = nullptr;
}

RETRO_API bool retro_load_game( const retro_game_info *game )
{
    unsigned uFormat = RETRO_PIXEL_FORMAT_XRGB8888;
    if ( !env_cb( RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &uFormat ) )
    {
        log_line( RETRO_LOG_ERROR, "frontend refused XRGB8888" );
        return false;
    }

    std::string strPath = ( game && game->path ) ? game->path : "";

    ReadSessionSize();

    Client client = BuildClient( strPath );
    if ( client.argv.empty() )
    {
        log_line( RETRO_LOG_ERROR, "nothing to run: no game path and no gamescope_command" );
        return false;
    }

    if ( !SpawnCompositor( client ) )
    {
        CloseSession();
        return false;
    }

    if ( !AwaitHello( 60 * 1000 ) )
    {
        CloseSession();
        return false;
    }

    // One frame of silence, so the frontend's audio clock advances even though nothing
    // in here makes a sound yet. See the audio milestone in docs/GAMESCOPE.md.
    size_t uFrames = size_t( 48000.0 / g_Session.flFps );
    g_Session.Silence.assign( uFrames * 2, 0 );

    g_Session.uUsedWidth = 0;
    g_Session.uUsedHeight = 0;

    g_Session.bRunning = true;
    g_Session.bReportedExit = false;

    return true;
}

RETRO_API bool retro_load_game_special( unsigned, const retro_game_info *, size_t )
{
    return false;
}

RETRO_API void retro_unload_game( void )
{
    CloseSession();
}

RETRO_API void retro_run( void )
{
    if ( !g_Session.bRunning )
    {
        // Nothing to show and nothing coming. Repeat the last frame so the view holds
        // its picture instead of going black the moment the demo exits.
        video_cb( nullptr, g_Session.uWidth, g_Session.uHeight, 0 );
        if ( audio_batch_cb && !g_Session.Silence.empty() )
            audio_batch_cb( g_Session.Silence.data(), g_Session.Silence.size() / 2 );
        return;
    }

    PumpInput();

    int nSlot = DrainFrames();

    if ( nSlot >= 0 )
    {
        PublishFrame( nSlot );
        g_Session.nLastSlot = nSlot;
    }
    else
    {
        // A dupe. The frontend advertises GET_CAN_DUPE, so a null frame means
        // "same as last time" and costs it no upload.
        video_cb( nullptr, g_Session.uWidth, g_Session.uHeight, 0 );
    }

    if ( audio_batch_cb && !g_Session.Silence.empty() )
        audio_batch_cb( g_Session.Silence.data(), g_Session.Silence.size() / 2 );
}

RETRO_API void retro_reset( void )
{
    // No emulated machine to reset; the honest equivalent would be relaunching the
    // client, which needs the path we were loaded with. Left for the wine milestone.
}

RETRO_API void retro_set_controller_port_device( unsigned, unsigned ) {}

RETRO_API size_t retro_serialize_size( void ) { return 0; }
RETRO_API bool retro_serialize( void *, size_t ) { return false; }
RETRO_API bool retro_unserialize( const void *, size_t ) { return false; }
RETRO_API void retro_cheat_reset( void ) {}
RETRO_API void retro_cheat_set( unsigned, bool, const char * ) {}
RETRO_API void *retro_get_memory_data( unsigned ) { return nullptr; }
RETRO_API size_t retro_get_memory_size( unsigned ) { return 0; }
RETRO_API unsigned retro_get_region( void ) { return RETRO_REGION_NTSC; }

} // extern "C"
