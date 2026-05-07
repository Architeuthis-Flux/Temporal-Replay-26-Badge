// Simplified MicroPython bridge: cooperative event REPL pumped from Arduino loop().
// Background Arduino work (OLED, LEDs, inputs) shares the same thread via Scheduler.
// Filesystem is provided by MicroPython's built-in VFS layer (FATFS on flash).

#include <Arduino.h>

extern "C" {
#if __has_include( <micropython_embed.h>)
#include <micropython_embed.h>
#define HAS_MICROPYTHON_EMBED 1
#else
#define HAS_MICROPYTHON_EMBED 0
#endif
#if HAS_MICROPYTHON_EMBED
#include "py/gc.h"
#include "py/runtime.h"
#include "py/stackctrl.h"
#include "shared/runtime/interrupt_char.h"
#include "shared/runtime/pyexec.h"
int replay_vfs_mount_fat( void );
#endif
#include "matrix_app_api.h"
}

// ── HAL hooks (called from mphalport.c) ─────────────────────────────────────

static Stream* s_repl_stream = &Serial;

extern "C" void mpy_hal_stdout_write( const char* str, size_t len ) {
    if ( s_repl_stream ) {
        s_repl_stream->write( reinterpret_cast<const uint8_t*>( str ), len );
    }
}

extern "C" int mpy_hal_stdin_read( void ) {
    if ( !s_repl_stream || s_repl_stream->available( ) <= 0 ) {
        return -1;
    }
    return s_repl_stream->read( );
}

extern "C" int mpy_hal_stdin_available( void ) {
    if ( !s_repl_stream )
        return 0;
    return s_repl_stream->available( ) > 0 ? 1 : 0;
}

extern "C" void mpy_service_pump( void ) __attribute__( ( weak ) );
extern "C" void mpy_service_pump( void ) { }

static void mpy_check_interrupt( void ) {
#if HAS_MICROPYTHON_EMBED
    if ( s_repl_stream && s_repl_stream->available( ) > 0 ) {
        int c = s_repl_stream->peek( );
        if ( c == mp_interrupt_char ) {
            s_repl_stream->read( );
            mp_sched_keyboard_interrupt( );

            extern pyexec_mode_kind_t pyexec_mode_kind;
            if ( pyexec_mode_kind == PYEXEC_MODE_RAW_REPL ) {
                s_repl_stream->write( '\x04' );
                s_repl_stream->write( '\x04' );
                s_repl_stream->write( '>' );
                s_repl_stream->flush( );
            }
        }
    }
    mp_handle_pending( MP_HANDLE_PENDING_CALLBACKS_AND_EXCEPTIONS );
#endif
}

extern volatile bool mpy_app_force_exit;

extern "C" void mpy_hal_delay_ms( unsigned int ms ) {
    if ( ms == 0 ) {
        ms = 1;
    }
    uint32_t start = millis( );
    while ( millis( ) - start < ms ) {
        mpy_check_interrupt( );
        mpy_service_pump( );
#if HAS_MICROPYTHON_EMBED
        if ( mpy_app_force_exit ) {
            mpy_app_force_exit = false;
            mp_raise_type( &mp_type_SystemExit );
        }
#endif
        delay( 1 );
    }
}

extern "C" unsigned long mpy_hal_ticks_ms( void ) {
    return millis( );
}

// ── Heap ────────────────────────────────────────────────────────────────────

namespace {
static constexpr size_t kMicroPythonHeapSize = 2 * 1024 * 1024;
static uint8_t* s_mp_heap = nullptr;
static bool s_mp_ready = false;
static bool s_prev_char_was_cr = false;
} // namespace

static void mpy_collect_now( void ) {
#if HAS_MICROPYTHON_EMBED
    if ( s_mp_ready ) {
        gc_collect( );
    }
#endif
}

extern "C" void mpy_collect( void ) {
    mpy_collect_now( );
}

// ── MicroPython init helper ─────────────────────────────────────────────────

#if HAS_MICROPYTHON_EMBED

static void mp_init_and_mount( void* stack_top ) {
    if ( !s_mp_heap ) {
        s_mp_heap = (uint8_t*)ps_malloc( kMicroPythonHeapSize );
        if ( !s_mp_heap ) {
            Serial.println( "[mpy] FATAL: could not allocate PSRAM heap" );
            return;
        }
        Serial.printf( "[mpy] heap: %u KB (%s)\n",
                       (unsigned)( kMicroPythonHeapSize / 1024 ),
                       esp_ptr_external_ram( s_mp_heap ) ? "PSRAM" : "SRAM" );
    }

    mp_embed_init( s_mp_heap, kMicroPythonHeapSize, stack_top );

    // Mount FAT filesystem via MicroPython's built-in VFS layer.
    if ( replay_vfs_mount_fat( ) != 0 ) {
        mp_printf( &mp_plat_print, "[mpy] WARNING: VFS mount failed — running without filesystem\n" );
    }

    // Add common paths to sys.path for convenience.
    mp_embed_exec_str(
        "import sys\n"
        "for p in ('/lib', '/matrixApps', '/'):\n"
        "    if p not in sys.path:\n"
        "        sys.path.append(p)\n" );

    // Auto-import badge API into global namespace so REPL users
    // can call oled_print(), button(), etc. without any import.
    mp_embed_exec_str( "from badge import *\n" );
}

static void mp_soft_reboot( void ) {
    mp_embed_deinit( );
    s_prev_char_was_cr = false;
    char new_stack_top;
    mp_init_and_mount( &new_stack_top );
    pyexec_event_repl_init( );
}

#endif // HAS_MICROPYTHON_EMBED

// ── Public API ──────────────────────────────────────────────────────────────

extern "C" void mpy_start( Stream* stream ) {
    if ( stream ) {
        s_repl_stream = stream;
    }

#if HAS_MICROPYTHON_EMBED
    if ( s_mp_ready ) {
        return;
    }
    s_prev_char_was_cr = false;
    char stack_top;
    mp_init_and_mount( &stack_top );
    pyexec_event_repl_init( );
    s_mp_ready = true;
#else
    Serial.println( "MicroPython not linked in this build" );
#endif
}

extern "C" void mpy_poll( void ) {
#if HAS_MICROPYTHON_EMBED
    if ( !s_mp_ready || !s_repl_stream ) {
        return;
    }

    char stack_top;
    mp_stack_set_top( &stack_top );

    static bool s_was_connected = false;
    bool connected = Serial;
    if ( connected && !s_was_connected ) {
        Serial.println( "[mpy] USB reconnect — reinitializing REPL" );
        // pyexec_event_repl_init( );
        s_prev_char_was_cr = false;
    }
    s_was_connected = connected;

    int processed = 0;
    while ( s_repl_stream->available( ) > 0 && processed < 8192 ) {
        int c = s_repl_stream->read( );
        if ( c < 0 ) {
            break;
        }
        bool is_cr = ( c == '\r' );
        bool is_lf = ( c == '\n' );
        if ( is_lf && s_prev_char_was_cr ) {
            s_prev_char_was_cr = false;
            continue;
        }
        if ( is_cr ) {
            c = '\n';
        }
        s_prev_char_was_cr = is_cr;

        if ( c == 0x03 ) {
            extern pyexec_mode_kind_t pyexec_mode_kind;
            if ( pyexec_mode_kind == PYEXEC_MODE_RAW_REPL && s_repl_stream ) {
                s_repl_stream->write( '\x04' );
                s_repl_stream->write( '\x04' );
                s_repl_stream->write( '>' );
                s_repl_stream->flush( );
            }
        }

        nlr_buf_t nlr;
        if ( nlr_push( &nlr ) == 0 ) {
            int result = pyexec_event_repl_process_char( c );
            nlr_pop( );

            if ( result & PYEXEC_FORCED_EXIT ) {
                pyexec_event_repl_init( );
                return;
            }
        } else {
            mp_hal_set_interrupt_char( -1 );
            mp_handle_pending( MP_HANDLE_PENDING_CALLBACKS_AND_CLEAR_EXCEPTIONS );

            extern pyexec_mode_kind_t pyexec_mode_kind;
            if ( pyexec_mode_kind == PYEXEC_MODE_RAW_REPL && s_repl_stream ) {
                s_repl_stream->write( '\x04' );
                s_repl_stream->write( '\x04' );
                s_repl_stream->write( '>' );
                s_repl_stream->flush( );
            }
            Serial.printf( "[mpy] Caught stray exception val=%p\n", nlr.ret_val );
            break;
        }

        processed++;
    }

    if ( processed > 0 && s_repl_stream ) {
        s_repl_stream->flush( );
    }
#endif
}

extern "C" void mpy_gui_exec_file( const char* path ) {
#if HAS_MICROPYTHON_EMBED
    if ( !s_mp_ready || !path ) return;

    char stack_top;
    mp_stack_set_top( &stack_top );

    char cmd[256];
    snprintf( cmd, sizeof( cmd ),
              "try:\n exec(open('%s').read())\n"
              "except SystemExit:\n pass\n"
              "except Exception as e:\n print('App error:', e)\n",
              path );

    Serial.printf( "[mpy] GUI exec: %s\n", path );
    mpy_app_force_exit = false;
    mpy_collect_now( );
    matrix_app_foreground_begin();
    mp_embed_exec_str( cmd );
    mpy_collect_now( );
    matrix_app_foreground_end();
    mpy_collect_now( );
    mpy_app_force_exit = false;
    Serial.println( "[mpy] GUI exec complete" );

    s_prev_char_was_cr = false;
    pyexec_event_repl_init( );
#else
    (void)path;
    Serial.println( "[mpy] MicroPython not available" );
#endif
}
