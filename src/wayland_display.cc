// Copyright 2018 The Flutter Authors. All rights reserved.
// Copyright 2019 Damian Wrobel <dwrobel@ertelnet.rybnik.pl>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef WL_EGL_PLATFORM
#define WL_EGL_PLATFORM 1
#endif

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/timerfd.h>

#include <chrono>
#include <sstream>
#include <vector>
#include <functional>

#include <dlfcn.h>
#include <climits>
#include <cstring>
#include <cassert>
#include <stdlib.h>
#include <unistd.h>
#include <poll.h>
#include <errno.h>
#include <sys/mman.h>
#include <linux/input-event-codes.h>

#include "debug.h"
#include "elf.h"
#include "keys.h"
#include "utils.h"
#include "egl_utils.h"
#include "wayland_display.h"

#include <sstream>

#include <unistd.h>
#include <sys/syscall.h>

#include <sys/eventfd.h>
#include <fcntl.h>

#define DBG_TIMING(x)

#define MEMWATCHTAG "memwatcher: "

DBG_TIMING(
#define gettid() syscall(SYS_gettid)
    static uint64_t t00;)

namespace flutter {

static double get_pixel_ratio(int32_t physical_width, int32_t physical_height, int32_t pixels_width, int32_t pixels_height) {

  if (pixels_width == 0 || physical_height == 0 || pixels_width == 0 || pixels_height == 0) {
    return 1.0;
  }

  return getEnv("FLUTTER_WAYLAND_PIXEL_RATIO", 1.0);
}

static inline WaylandDisplay *get_wayland_display(void *data, const bool check_non_null = true) {
  WaylandDisplay *const wd = static_cast<WaylandDisplay *>(data);

  if (check_non_null) {
    assert(wd);
    if (wd == nullptr)
      abort();
  }

  return wd;
}

const wl_registry_listener WaylandDisplay::kRegistryListener = {
    .global = [](void *data, struct wl_registry *wl_registry, uint32_t name, const char *interface, uint32_t version) -> void {
      WaylandDisplay *const wd = get_wayland_display(data);

      dbgI("registry: name:%2u, interface:%s, version:%u\n", name, interface, version);

      if (strcmp(interface, "wl_compositor") == 0) {
        wd->compositor_ = static_cast<decltype(compositor_)>(wl_registry_bind(wl_registry, name, &wl_compositor_interface, 1));
        return;
      }

      if (strcmp(interface, "wl_shell") == 0) {
        wd->shell_ = static_cast<decltype(shell_)>(wl_registry_bind(wl_registry, name, &wl_shell_interface, 1));
        return;
      }

      if (strcmp(interface, "wl_seat") == 0) {
        wd->seat_ = static_cast<decltype(seat_)>(wl_registry_bind(wl_registry, name, &wl_seat_interface, 4));
        wl_seat_add_listener(wd->seat_, &kSeatListener, wd);
        return;
      }

      if (strcmp(interface, "wl_output") == 0) {
        wd->output_ = static_cast<decltype(output_)>(wl_registry_bind(wl_registry, name, &wl_output_interface, 1));
        wl_output_add_listener(wd->output_, &kOutputListener, wd);
        return;
      }

      if (strcmp(interface, wp_presentation_interface.name) == 0) {
        int use_presentation_timing_interface = getEnv("FLUTTER_LAUNCHER_USE_PRESENTATION_INTERFACE", 1);
        dbgI("launcher: use wayland presentation timing interface: %d\n", use_presentation_timing_interface);
        if (use_presentation_timing_interface) {
          wd->presentation_ = static_cast<decltype(presentation_)>(wl_registry_bind(wl_registry, name, &wp_presentation_interface, 1));
          wp_presentation_add_listener(wd->presentation_, &kPresentationListener, wd);
        }
        return;
      }

      if (strcmp(interface, zwp_xwayland_keyboard_grab_manager_v1_interface.name) == 0) {
        wd->kbd_grab_manager_ = static_cast<decltype(kbd_grab_manager_)>(wl_registry_bind(wl_registry, name, &zwp_xwayland_keyboard_grab_manager_v1_interface, 1));
        return;
      }
    },

    .global_remove = [](void *data, struct wl_registry *wl_registry, uint32_t name) -> void {

    },
};

const wl_shell_surface_listener WaylandDisplay::kShellSurfaceListener = {
    .ping = [](void *data, struct wl_shell_surface *wl_shell_surface, uint32_t serial) -> void {
      WaylandDisplay *const wd = get_wayland_display(data);

      wl_shell_surface_pong(wd->shell_surface_, serial);
    },

    .configure = [](void *data, struct wl_shell_surface *wl_shell_surface, uint32_t edges, int32_t width, int32_t height) -> void {
      WaylandDisplay *const wd = get_wayland_display(data, false);

      if (wd == nullptr)
        return;

      if (wd->window_ == nullptr)
        return;

      wl_egl_window_resize(wd->window_, wd->screen_width_ = width, wd->screen_height_ = height, 0, 0);

      FlutterWindowMetricsEvent event = {};
      event.struct_size               = sizeof(event);
      event.width                     = wd->screen_width_;
      event.height                    = wd->screen_height_;
      event.pixel_ratio               = get_pixel_ratio(wd->physical_width_, wd->physical_height_, wd->screen_width_, wd->screen_height_);

      auto success = FlutterEngineSendWindowMetricsEvent(wd->engine_, &event) == kSuccess;

      dbgI("shell.configure: %zdx%zd par: %.3g status: %s\n", event.width, event.height, event.pixel_ratio, (success ? "success" : "failed"));
    },

    .popup_done = [](void *data, struct wl_shell_surface *wl_shell_surface) -> void {
      // Nothing to do.
    },
};

const wl_pointer_listener WaylandDisplay::kPointerListener = {
    .enter = [](void *data, struct wl_pointer *wl_pointer, uint32_t serial, struct wl_surface *surface, wl_fixed_t surface_x, wl_fixed_t surface_y) {},

    .leave =
        [](void *data, struct wl_pointer *wl_pointer, uint32_t serial, struct wl_surface *surface) {
          WaylandDisplay *const wd = get_wayland_display(data);

          wd->key_modifiers = static_cast<GdkModifierType>(0);
        },

    .motion =
        [](void *data, struct wl_pointer *wl_pointer, uint32_t time, wl_fixed_t surface_x, wl_fixed_t surface_y) {
          WaylandDisplay *const wd = get_wayland_display(data);

          // just store raw values
          wd->surface_x = surface_x;
          wd->surface_y = surface_y;
        },

    .button =
        [](void *data, struct wl_pointer *wl_pointer, uint32_t serial, uint32_t time, uint32_t button, uint32_t state) {
          WaylandDisplay *const wd = get_wayland_display(data);

          uint32_t button_number = button - BTN_LEFT;
          button_number          = button_number == 1 ? 2 : button_number == 2 ? 1 : button_number;

          FlutterPointerEvent event = {
              .struct_size    = sizeof(event),
              .phase          = state == WL_POINTER_BUTTON_STATE_PRESSED ? FlutterPointerPhase::kDown : FlutterPointerPhase::kUp,
              .timestamp      = time * 1'000,
              .x              = wl_fixed_to_double(wd->surface_x),
              .y              = wl_fixed_to_double(wd->surface_y),
              .device         = 0,
              .signal_kind    = kFlutterPointerSignalKindNone,
              .scroll_delta_x = 0,
              .scroll_delta_y = 0,
              .device_kind    = static_cast<FlutterPointerDeviceKind>(0), // dw: TODO: Why kFlutterPointerDeviceKindMouse does not work?
              .buttons        = 0,
          };

          FlutterEngineSendPointerEvent(wd->engine_, &event, 1);
        },

    .axis = [](void *data, struct wl_pointer *wl_pointer, uint32_t time, uint32_t axis, wl_fixed_t value) {},

    .frame = [](void *data, struct wl_pointer *wl_pointer) {},

    .axis_source = [](void *data, struct wl_pointer *wl_pointer, uint32_t axis_source) {},

    .axis_stop = [](void *data, struct wl_pointer *wl_pointer, uint32_t time, uint32_t axis) {},

    .axis_discrete = [](void *data, struct wl_pointer *wl_pointer, uint32_t axis, int32_t discrete) {},

};

const wl_keyboard_listener WaylandDisplay::kKeyboardListener = {
    .keymap =
        [](void *data, struct wl_keyboard *wl_keyboard, uint32_t format, int32_t fd, uint32_t size) {
          WaylandDisplay *const wd = get_wayland_display(data);

          wd->keymap_format         = static_cast<wl_keyboard_keymap_format>(format);
          char *const keymap_string = reinterpret_cast<char *>(mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0));
          xkb_keymap_unref(wd->keymap);
          wd->keymap = xkb_keymap_new_from_string(wd->xkb_context, keymap_string, XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS);
          munmap(keymap_string, size);
          close(fd);
          xkb_state_unref(wd->xkb_state);
          wd->xkb_state = xkb_state_new(wd->keymap);
        },

    .enter = [](void *data, struct wl_keyboard *wl_keyboard, uint32_t serial, struct wl_surface *surface, struct wl_array *keys) { dbgI("key: keyboard enter\n"); },

    .leave = [](void *data, struct wl_keyboard *wl_keyboard, uint32_t serial, struct wl_surface *surface) { dbgI("key: keyboard leave\n"); },

    .key =
        [](void *data, struct wl_keyboard *wl_keyboard, uint32_t serial, uint32_t time, uint32_t key, uint32_t state_w) {
          WaylandDisplay *const wd = get_wayland_display(data);

          uint32_t repeat_delay_ms_   = 0;
          uint32_t repeat_interval_ms = 0;

          if (wd->key_handler(wd->key.last_ = key, wd->key.state_ = state_w)) {
            repeat_delay_ms_   = wd->key.repeat_delay_ms_;
            repeat_interval_ms = wd->key.repeat_interval_ms_;
          }

          struct itimerspec ts;
          timespec_from_msec(&ts.it_value, repeat_delay_ms_);
          timespec_from_msec(&ts.it_interval, repeat_interval_ms);
          const int rv = timerfd_settime(wd->key.timer_fd_, 0, &ts, nullptr);

          if (rv == -1) {
            dbgE("timerfd_settime returned: %d (errno: %d)\n", rv, errno);
          }
        },

    .modifiers =
        [](void *data, struct wl_keyboard *wl_keyboard, uint32_t serial, uint32_t mods_depressed, uint32_t mods_latched, uint32_t mods_locked, uint32_t group) {
          WaylandDisplay *const wd = get_wayland_display(data);

          xkb_state_update_mask(wd->xkb_state, mods_depressed, mods_latched, mods_locked, group, 0, 0);
        },

    .repeat_info =
        [](void *data, struct wl_keyboard *wl_keyboard, int32_t rate, int32_t delay) {
          WaylandDisplay *const wd = get_wayland_display(data);

          if (rate > 0) {
            wd->key.repeat_interval_ms_ = 1'000 / rate;
          }

          if (delay > 0) {
            wd->key.repeat_delay_ms_ = delay;
          }

          dbgI("key: repeat_info delay:%d, rate:%d -> delay:%d[ms], repeat-interval:%d[ms]\n", delay, rate, wd->key.repeat_delay_ms_, wd->key.repeat_interval_ms_);
        },
};

const wl_seat_listener WaylandDisplay::kSeatListener = {
    .capabilities =
        [](void *data, struct wl_seat *seat, uint32_t capabilities) {
          WaylandDisplay *const wd = get_wayland_display(data);
          assert(seat == wd->seat_);

          dbgI("seat.capabilities(data:%p, seat:%p, capabilities:0x%x)\n", data, static_cast<void *>(seat), capabilities);

          if (capabilities & WL_SEAT_CAPABILITY_POINTER) {
            dbgI("seat.capabilities: pointer\n");
            struct wl_pointer *pointer = wl_seat_get_pointer(seat);
            wl_pointer_add_listener(pointer, &kPointerListener, wd);
          }

          if (capabilities & WL_SEAT_CAPABILITY_KEYBOARD) {
            dbgI("seat.capabilities: keyboard\n");
            struct wl_keyboard *keyboard = wl_seat_get_keyboard(seat);
            wl_keyboard_add_listener(keyboard, &kKeyboardListener, wd);
          }

          if (capabilities & WL_SEAT_CAPABILITY_TOUCH) {
            dbgI("seat.capabilities: touch\n");
          }
        },

    .name =
        [](void *data, struct wl_seat *wl_seat, const char *name) {
          // Nothing to do.
        },
};

const wl_output_listener WaylandDisplay::kOutputListener = {
    .geometry =
        [](void *data, struct wl_output *wl_output, int32_t x, int32_t y, int32_t physical_width, int32_t physical_height, int32_t subpixel, const char *make, const char *model, int32_t transform) {
          WaylandDisplay *const wd = get_wayland_display(data);

          wd->physical_width_  = physical_width;
          wd->physical_height_ = physical_height;

          dbgI("output.geometry(data:%p, wl_output:%p, x:%d, y:%d, physical_width:%d, physical_height:%d, subpixel:%d, make:%s, model:%s, transform:%d)\n", data, static_cast<void *>(wl_output), x, y, physical_width, physical_height,
               subpixel, make, model, transform);
        },
    .mode =
        [](void *data, struct wl_output *wl_output, uint32_t flags, int32_t width, int32_t height, int32_t refresh) {
          WaylandDisplay *const wd = get_wayland_display(data);

          wd->vsync.vblank_time_ns_ = 1'000'000'000'000 / refresh;

          dbgI("output.mode(data:%p, wl_output:%p, flags:%d, width:%d->%d, height:%d->%d, refresh:%d)\n", data, static_cast<void *>(wl_output), flags, wd->screen_width_, width, wd->screen_height_, height, refresh);

          if (wd->engine_) {
            FlutterWindowMetricsEvent event = {};
            event.struct_size               = sizeof(event);
            event.width                     = (wd->screen_width_ = width);
            event.height                    = (wd->screen_height_ = height);
            event.pixel_ratio               = get_pixel_ratio(wd->physical_width_, wd->physical_height_, wd->screen_width_, wd->screen_height_);

            auto success = FlutterEngineSendWindowMetricsEvent(wd->engine_, &event) == kSuccess;

            wl_egl_window_resize(wd->window_, wd->screen_width_, wd->screen_height_, 0, 0);

            dbgI("Window resized: %zdx%zd par: %.3g status: %s.\n", event.width, event.height, event.pixel_ratio, (success ? "success" : "failed"));
          } else {
            wd->window_metrix_skipped_ = true;
            dbgI("Window resized: %dx%d status: %s.\n", wd->screen_width_, wd->screen_width_, "skipped");
          }
        },
    .done  = [](void *data, struct wl_output *wl_output) { printf("output.done(data:%p, wl_output:%p)\n", data, static_cast<void *>(wl_output)); },
    .scale = [](void *data, struct wl_output *wl_output, int32_t factor) { printf("output.scale(data:%p, wl_output:%p, factor:%d)\n", data, static_cast<void *>(wl_output), factor); },
};

const struct wp_presentation_feedback_listener WaylandDisplay::kPresentationFeedbackListener = {
    .sync_output = [](void *data, struct wp_presentation_feedback *wp_presentation_feedback, struct wl_output *output) {},
    .presented =
        [](void *data, struct wp_presentation_feedback *wp_presentation_feedback, uint32_t tv_sec_hi, uint32_t tv_sec_lo, uint32_t tv_nsec, uint32_t refresh, uint32_t seq_hi, uint32_t seq_lo, uint32_t flags) {
          WaylandDisplay *const wd = get_wayland_display(data);

          const uint64_t new_last_frame_ns = (((static_cast<uint64_t>(tv_sec_hi) << 32) + tv_sec_lo) * 1'000'000'000) + tv_nsec;

          if (refresh != wd->vsync.vblank_time_ns_) {
            static auto displayed = false;

            if (!displayed) {
              dbgW("Variable display rate output: vblank_time_ns: %ju refresh: %u\n", wd->vsync.vblank_time_ns_, refresh);
              displayed = true;
            }
          }

          DBG_TIMING({
            static auto t0 = FlutterEngineGetCurrentTime();
            const auto t1  = FlutterEngineGetCurrentTime();
            // dbgI("[%09.4f][%ld] presented: %09lldns flags:%08x p2p-diff:%5lldus rerfresh:%u\n", (t1-t00)/1e9, gettid(), (new_last_frame_ns-t00), flags, (new_last_frame_ns - wd->vsync.last_frame_)/1000, refresh);
            dbgI("[%09.4f][%ld] presented %09.4f f:%08x dc:%.5f df:%.5f r:%u\n", (t1 - t00) / 1e9, gettid(), (new_last_frame_ns - t00) / 1e9, flags, (t1 - t0) / 1e9, (new_last_frame_ns - wd->vsync.last_frame_) / 1e9, refresh);
            t0 = t1;
          })

          wd->vsync.last_frame_ = new_last_frame_ns;
        },
    .discarded =
        [](void *data, struct wp_presentation_feedback *wp_presentation_feedback) {
          // TODO: remove it
          dbgW("presentation.frame dropped\n");
        },
}; // namespace flutter

const struct wp_presentation_listener WaylandDisplay::kPresentationListener = {
    .clock_id =
        [](void *data, struct wp_presentation *wp_presentation, uint32_t clk_id) {
          WaylandDisplay *const wd = get_wayland_display(data);

          wd->vsync.presentation_clk_id_ = clk_id;

          dbgI("presentation.clk_id: %u\n", clk_id);
        },
};

bool WaylandDisplay::key_handler(const uint32_t key, const uint32_t state_w, const bool is_repeat) {
  if (keymap_format == WL_KEYBOARD_KEYMAP_FORMAT_NO_KEYMAP) {
    dbgW("key: Hmm - no keymap, no key event\n");
    return false;
  }

  const xkb_keycode_t hardware_keycode = key + (keymap_format * 8);
  const xkb_keysym_t keysym            = xkb_state_key_get_one_sym(xkb_state, hardware_keycode);

  if (keysym == XKB_KEY_NoSymbol) {
    dbgW("key: Hmm - no key symbol, no key event\n");
    return false;
  }

  xkb_mod_mask_t mods = xkb_state_serialize_mods(xkb_state, XKB_STATE_MODS_EFFECTIVE);
  key_modifiers       = toGDKModifiers(keymap, mods);

  // Remove lock states from state mask.
  guint state = key_modifiers & ~(GDK_LOCK_MASK | GDK_MOD2_MASK);

  static bool shift_lock_pressed = false;
  static bool caps_lock_pressed  = false;
  static bool num_lock_pressed   = false;

  const GdkEventType type = state_w == WL_KEYBOARD_KEY_STATE_PRESSED ? GDK_KEY_PRESS : GDK_KEY_RELEASE;

  switch (keysym) {
  case GDK_KEY_Num_Lock:
    num_lock_pressed = type == GDK_KEY_PRESS;
    break;
  case GDK_KEY_Caps_Lock:
    caps_lock_pressed = type == GDK_KEY_PRESS;
    break;
  case GDK_KEY_Shift_Lock:
    shift_lock_pressed = type == GDK_KEY_PRESS;
    break;
  }

  // Add back in the state matching the actual pressed state of the lock keys,
  // not the lock states.
  state |= (shift_lock_pressed || caps_lock_pressed) ? GDK_LOCK_MASK : 0x0;
  state |= num_lock_pressed ? GDK_MOD2_MASK : 0x0;

  const uint32_t utf32 = xkb_keysym_to_utf32(keysym); // TODO: double check if it fully mimics gdk_keyval_to_unicode()

  if (utf32) {
    if (utf32 >= 0x21 && utf32 <= 0x7E) {
      dbgT("key: %c %s%s\n", (char)utf32, type == GDK_KEY_PRESS ? "pressed" : "released", is_repeat ? " [r]" : "");
    } else {
      dbgT("key: U+%04X %s%s\n", utf32, type == GDK_KEY_PRESS ? "pressed" : "released", is_repeat ? " [r]" : "");
    }
  } else {
    char name[64];
    xkb_keysym_get_name(keysym, name, sizeof(name));

    dbgT("key: %s %s%s\n", name, type == GDK_KEY_PRESS ? "pressed" : "released", is_repeat ? " [r]" : "");
  }

  std::string message;

  // dw: if you do not like so many backslashes,
  // please consider to rerwite it using RapidJson.
  message += "{";
  message += " \"type\":" + std::string(type == GDK_KEY_PRESS ? "\"keydown\"" : "\"keyup\"");
  message += ",\"keymap\":" + std::string("\"linux\"");
  message += ",\"scanCode\":" + std::to_string(hardware_keycode);
  message += ",\"toolkit\":" + std::string("\"gtk\"");
  message += ",\"keyCode\":" + std::to_string(keysym);
  message += ",\"modifiers\":" + std::to_string(state);
  if (utf32) {
    message += ",\"unicodeScalarValues\":" + std::to_string(utf32);
  }

  message += "}";

  if (!message.empty()) {
    bool success = FlutterSendMessage(engine_, "flutter/keyevent", reinterpret_cast<const uint8_t *>(message.c_str()), message.size());

    if (!success) {
      dbgE("Error sending PlatformMessage: %s\n", message.c_str());
    }
  }

  return xkb_keymap_key_repeats(keymap, hardware_keycode) && type == GDK_KEY_PRESS;
}

WaylandDisplay::WaylandDisplay(size_t width, size_t height, const std::string &bundle_path, const std::vector<std::string> &command_line_args)
    : xkb_context(xkb_context_new(XKB_CONTEXT_NO_FLAGS))
    , screen_width_(width)
    , screen_height_(height) {
  if (screen_width_ == 0 || screen_height_ == 0) {
    dbgE("Invalid screen dimensions\n");
    return;
  }

  if (socketpair(AF_LOCAL, SOCK_DGRAM | SOCK_CLOEXEC, 0, &vsync.sv_[0]) == -1) {
    dbgE("socketpair() failed, errno: %d\n", errno);
    return;
  }

  key.timer_fd_ = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC);

  if (key.timer_fd_ < 0) {
    dbgE("Could not create timer\n");
    return;
  }

  display_ = wl_display_connect(nullptr);

  if (!display_) {
    dbgE("Could not connect to the wayland display\n");
    return;
  }

  registry_ = wl_display_get_registry(display_);

  if (!registry_) {
    dbgE("Could not get the wayland registry\n");
    return;
  }

  wl_registry_add_listener(registry_, &kRegistryListener, this);

  wl_display_roundtrip(display_);

  if (!SetupEGL()) {
    dbgE("Could not setup EGL.\n");
    return;
  }

  if (!SetupEngine(bundle_path, command_line_args)) {
    dbgE("Could not setup Flutter Engine\n");
    return;
  }

  DBG_TIMING({ t00 = FlutterEngineGetCurrentTime(); });
  valid_ = true;
}

bool WaylandDisplay::SetupEngine(const std::string &bundle_path, const std::vector<std::string> &command_line_args) {
  FlutterRendererConfig config = {};
  config.type                  = kOpenGL;
  config.open_gl.struct_size   = sizeof(config.open_gl);
  config.open_gl.make_current  = [](void *data) -> bool {
    WaylandDisplay *const wd = get_wayland_display(data);

    if (eglMakeCurrent(wd->egl_display_, wd->egl_surface_, wd->egl_surface_, wd->egl_context_) != EGL_TRUE) {
      LogLastEGLError();
      dbgE("Could not make the onscreen context current\n");
      return false;
    }

    return true;
  };
  config.open_gl.clear_current = [](void *data) -> bool {
    WaylandDisplay *const wd = get_wayland_display(data);

    if (eglMakeCurrent(wd->egl_display_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT) != EGL_TRUE) {
      LogLastEGLError();
      dbgE("Could not clear the context\n");
      return false;
    }

    return true;
  };
  config.open_gl.present = [](void *data) -> bool {
    WaylandDisplay *const wd = get_wayland_display(data);

    DBG_TIMING(static auto tprev = FlutterEngineGetCurrentTime(); auto tb = FlutterEngineGetCurrentTime(); dbgI("[%09.4f][%ld] >>> swap buffer [%09.4f]\n", (tb - t00) / 1e9, gettid(), (tb - tprev) / 1e9););

    eglSwapInterval(wd->egl_display_, 0);
    if (eglSwapBuffers(wd->egl_display_, wd->egl_surface_) != EGL_TRUE) {
      LogLastEGLError();
      dbgE("Could not swap the EGL buffer\n");
      return false;
    }

    DBG_TIMING(auto ta = FlutterEngineGetCurrentTime(); dbgI("[%09.4f][%ld] <<< swap buffer [dur:%09.4f]\n", (ta - t00) / 1e9, gettid(), (ta - tb) / 1e9); tprev = tb;);

    return true;
  };
  config.open_gl.fbo_callback          = [](void *data) -> uint32_t { return 0; };
  config.open_gl.make_resource_current = [](void *data) -> bool {
    WaylandDisplay *const wd = get_wayland_display(data);

    if (eglMakeCurrent(wd->egl_display_, wd->resource_egl_surface_, wd->resource_egl_surface_, wd->resource_egl_context_) != EGL_TRUE) {
      LogLastEGLError();
      dbgE("Could not make the RESOURCE context current\n");
      return false;
    }

    return true;
  };

  config.open_gl.gl_proc_resolver = [](void *data, const char *name) -> void * {
    auto address = eglGetProcAddress(name);
    if (address != nullptr) {
      return reinterpret_cast<void *>(address);
    }

    dbgW("Using dlsym fallback to resolve: %s\n.", name ? name : "");

    address = reinterpret_cast<void (*)()>(dlsym(RTLD_DEFAULT, name));

    if (address != nullptr) {
      return reinterpret_cast<void *>(address);
    }

    dbgW("Tried unsuccessfully to resolve: %s\n.", name ? name : "");
    return nullptr;
  };

  auto icu_data_path = GetICUDataPath();

  if (icu_data_path == "") {
    return false;
  }

  std::vector<const char *> command_line_args_c;

  for (const auto &arg : command_line_args) {
    command_line_args_c.push_back(arg.c_str());
  }

  FlutterProjectArgs args = {
      .struct_size       = sizeof(FlutterProjectArgs),
      .assets_path       = bundle_path.c_str(),
      .icu_data_path     = icu_data_path.c_str(),
      .command_line_argc = static_cast<int>(command_line_args_c.size()),
      .command_line_argv = command_line_args_c.data(),
      .vsync_callback    = [](void *data, intptr_t baton) -> void {
        WaylandDisplay *const wd = get_wayland_display(data);

        DBG_TIMING({
          auto tx = FlutterEngineGetCurrentTime();
          dbgI("[%09.4f][%ld] vsync callback [%jx]\n", (tx - t00) / 1e9, gettid(), static_cast<uintmax_t>(baton));
        });

        if (wd->vsync.baton_ != 0) {
          dbgE("vsync.wait: New baton arrived, but old was not sent\n");
          exit(1);
        }

        wd->vsync.baton_ = baton;

        if (wd->vSyncSendNotifyData() != 1) {
          exit(1);
        }
      },
      .compute_platform_resolved_locale_callback = [](const FlutterLocale **supported_locales, size_t number_of_locales) -> const FlutterLocale * {
        dbgI("locale.resolved.callback: number_of_locales: %zu\n", number_of_locales);

        if (number_of_locales > 0) {
          const FlutterLocale *fl = supported_locales[0];
          dbgI("locale.resolved: %s_%s.%s@%s\n", fl->language_code ? fl->language_code : "", fl->country_code ? fl->country_code : "", fl->script_code ? fl->script_code : "", fl->variant_code ? fl->variant_code : "");
          return fl;
        }

        return nullptr;
      },
  };

  if (getEnv("FLUTTER_LAUNCHER_USE_PRESENTATION_INTERFACE", 1) == 0) {
    args.vsync_callback = nullptr;
  }

  std::string libapp_aot_path = bundle_path + "/" + FlutterGetAppAotElfName(); // dw: TODO: There seems to be no convention name we could use, so let's temporary hardcode the path.

  if (FlutterEngineRunsAOTCompiledDartCode()) {
    dbgI("Using AOT precompiled runtime\n");

    if (std::ifstream(libapp_aot_path)) {
      dbgI("Loading AOT snapshot: %s\n", libapp_aot_path.c_str());

      const char *error;
      auto handle = Aot_LoadELF(libapp_aot_path.c_str(), 0, &error, &args.vm_snapshot_data, &args.vm_snapshot_instructions, &args.isolate_snapshot_data, &args.isolate_snapshot_instructions);

      if (!handle) {
        dbgE("Could not load AOT library: %s (error: %s).\n", libapp_aot_path.c_str(), error ? error : "");
        return false;
      }
    }
  }

  auto result = FlutterEngineInitialize(FLUTTER_ENGINE_VERSION, &config, &args, this /* userdata */, &engine_);

  if (result != kSuccess) {
    dbgE("Could not initialize the Flutter engine.\n");
    return false;
  }

  result = FlutterEngineRunInitialized(engine_);

  if (result != kSuccess) {
    dbgE("Could not run the Flutter engine.\n");
    return false;
  }

  {
    MyFlutterLocale fl;
    std::string lang = getEnv("LANG", std::string(""));
    FlutterParseLocale(lang, &fl);

    if (fl.language_code != nullptr) {
      dbgI("locale.parsed: %s_%s.%s@%s\n", fl.language_code ? fl.language_code : "", fl.country_code ? fl.country_code : "", fl.script_code ? fl.script_code : "", fl.variant_code ? fl.variant_code : "");
      const FlutterLocale *flutter_locales[] = {&fl};

      const auto rv = FlutterEngineUpdateLocales(engine_, flutter_locales, std::size(flutter_locales));

      if (rv != kSuccess) {
        dbgE("Could not update locales for the Flutter engine\n");
        return false;
      }
    }
  }

  SetupMemoryWatcher();

  if (window_metrix_skipped_) {
    FlutterWindowMetricsEvent event = {};

    event.struct_size = sizeof(event);
    event.width       = screen_width_;
    event.height      = screen_height_;
    event.pixel_ratio = get_pixel_ratio(physical_width_, physical_height_, screen_width_, screen_height_);

    const auto success = FlutterEngineSendWindowMetricsEvent(engine_, &event) == kSuccess;

    dbgI("Window metric: %zdx%zd par: %.3g status: %s\n", event.width, event.height, event.pixel_ratio, (success ? "success" : "failed"));

    return success;
  }

  return true;
}

void WaylandDisplay::HandleMemoryWatcherEvent() {
  // minimum 20 sec between FlutterEngineNotifyLowMemoryWarning calls
  constexpr auto min_time_between_warnings_ns_ = uint64_t(2e10);
  const uint64_t now_ns                        = FlutterEngineGetCurrentTime();
  constexpr int readsz                         = 32;

  if (now_ns < memory_watcher_.last_warning_sent_ns + min_time_between_warnings_ns_) {
    return;
  }

  lseek(memory_watcher_.memory_file_fd, 0, SEEK_SET);

  char memstr[readsz];
  int pos = 0;
  int rv  = 0;
  do {
    rv = read(memory_watcher_.memory_file_fd, memstr + pos, readsz - 1 - pos);
    if (rv != -1)
      pos += rv;
  } while ((rv == -1 && errno == EINTR) || rv > 0);
  memstr[pos] = '\0';
  if (rv == -1) {
    dbgW(MEMWATCHTAG "problem reading memory_usage, errno:%d\n", errno);
    return;
  }

  dbgT(MEMWATCHTAG "current mem: %s\n", memstr);
  const long mem = atol(memstr);
  if (mem > 0) {
    long level = 0;
    for (auto memwatchlevel : memory_watcher_.levels) {
      if (memwatchlevel > mem)
        break;
      else
        ++level;
    }
    dbgT(MEMWATCHTAG "current memory level: %ld\n", level);
    if (level > memory_watcher_.current_level) {
      dbgW(MEMWATCHTAG "sending FlutterEngineNotifyLowMemoryWarning\n");
      auto ret = FlutterEngineNotifyLowMemoryWarning(engine_);
      if (ret != kSuccess) {
        dbgE(MEMWATCHTAG "FlutterEngineNotifyLowMemoryWarning failed with %d\n", int(ret));
        return;
      }
      memory_watcher_.last_warning_sent_ns = now_ns;
    }
    memory_watcher_.current_level = level;
  }
}

void WaylandDisplay::CleanupMemoryWatcher() {
  if (memory_watcher_.memory_file_fd != -1) {
    close(memory_watcher_.memory_file_fd);
    memory_watcher_.memory_file_fd = -1;
  }
  if (memory_watcher_.event_fd != -1) {
    close(memory_watcher_.event_fd);
    memory_watcher_.event_fd = -1;
  }
}

void WaylandDisplay::SetupMemoryWatcher() {
  const std::string cgroup_memory_path = getEnv("FLUTTER_LAUNCHER_WAYLAND_CGROUP_MEMORY_PATH", std::string());
  if (cgroup_memory_path.empty()) {
    dbgI(MEMWATCHTAG "Memory watcher will not run - no FLUTTER_LAUNCHER_WAYLAND_CGROUP_MEMORY_PATH env var defined\n");
  } else {
    dbgT("SetupMemoryWatcher start, cgroup_memory_path: %s\n", cgroup_memory_path.c_str());
    const std::string memory_usage_path         = cgroup_memory_path + "/memory.usage_in_bytes";
    const std::string cgroup_event_control_path = cgroup_memory_path + "/cgroup.event_control";
    const std::string memoryWarningLevels       = getEnv("FLUTTER_LAUNCHER_WAYLAND_MEMORY_WARNING_WATERMARK_BYTES", std::string());
    if (memoryWarningLevels.empty()) {
      dbgI(MEMWATCHTAG "Memory watcher will not run - no FLUTTER_LAUNCHER_WAYLAND_MEMORY_WARNING_WATERMARK_BYTES env var defined\n");
    } else {
      std::stringstream ss{memoryWarningLevels};
      std::string level;
      while (std::getline(ss, level, ',')) {
        if (const long l = atol(level.c_str())) {
          memory_watcher_.levels.push_back(l);
        } else {
          dbgE(MEMWATCHTAG "Memory watcher will not run - could not perform conversion to long for %s; FLUTTER_LAUNCHER_WAYLAND_MEMORY_WARNING_WATERMARK_BYTES: %s\n", level.c_str(), memoryWarningLevels.c_str());
          return;
        }
      }
      if (memory_watcher_.levels.size() < 1) {
        dbgW(MEMWATCHTAG "Memory watcher will not run - needs at least 1 memory level; FLUTTER_LAUNCHER_WAYLAND_MEMORY_WARNING_WATERMARK_BYTES: %s\n", memoryWarningLevels.c_str());
      } else {
        int event_control = -1;

        bool all_succeeded = false;
        auto cleanup       = [&](bool *) {
          if (!all_succeeded) {
            CleanupMemoryWatcher();
          }
          if (event_control != -1) {
            close(event_control);
            event_control = -1;
          }
        };
        const auto cleanup_watcher = std::unique_ptr<bool, decltype(cleanup)>(&all_succeeded, cleanup);

        memory_watcher_.memory_file_fd = open(memory_usage_path.c_str(), O_RDONLY);
        if (memory_watcher_.memory_file_fd == -1) {
          dbgE(MEMWATCHTAG "Cannot open %s, errno: %d\n", memory_usage_path.c_str(), errno);
          return;
        }

        event_control = open(cgroup_event_control_path.c_str(), O_WRONLY);
        if (event_control == -1) {
          dbgE(MEMWATCHTAG "Cannot open %s, errno: %d\n", cgroup_event_control_path.c_str(), errno);
          return;
        }

        memory_watcher_.event_fd = eventfd(0, 0);
        if (memory_watcher_.event_fd == -1) {
          dbgE(MEMWATCHTAG "eventfd() failed\n");
          return;
        }

        for (auto level : memory_watcher_.levels) {
          char line[LINE_MAX];
          snprintf(line, LINE_MAX, "%d %d %ld", memory_watcher_.event_fd, memory_watcher_.memory_file_fd, level);
          dbgT(MEMWATCHTAG "event_control: writing line '%s'\n", line);
          const int ret = write(event_control, line, strlen(line) + 1);
          if (ret == -1) {
            dbgE(MEMWATCHTAG "Cannot write to cgroup.event_control, errno: %d\n", errno);
            return;
          }
        }
        dbgI(MEMWATCHTAG "starting memory watcher\n");
        all_succeeded = true;
      }
    }
  }
}

WaylandDisplay::~WaylandDisplay() {
  CleanupMemoryWatcher();

  if (engine_) {
    auto result = FlutterEngineShutdown(engine_);
    if (result == kSuccess) {
      engine_ = nullptr;
    } else {
      dbgE("Could not shutdown the Flutter engine.\n");
    }
  }

  if (shell_surface_) {
    wl_shell_surface_destroy(shell_surface_);
    shell_surface_ = nullptr;
  }

  if (shell_) {
    wl_shell_destroy(shell_);
    shell_ = nullptr;
  }

  if (output_) {
    wl_output_destroy(output_);
    output_ = nullptr;
  }

  if (seat_) {
    wl_seat_destroy(seat_);
    seat_ = nullptr;
  }

  xkb_keymap_unref(keymap);
  keymap = nullptr;

  xkb_context_unref(xkb_context);
  xkb_context = nullptr;

  if (egl_surface_) {
    eglDestroySurface(egl_display_, egl_surface_);
    egl_surface_ = nullptr;
  }

  if (egl_display_) {
    eglTerminate(egl_display_);
    egl_display_ = nullptr;
  }

  if (window_) {
    wl_egl_window_destroy(window_);
    window_ = nullptr;
  }

  if (surface_) {
    wl_surface_destroy(surface_);
    surface_ = nullptr;
  }

  if (compositor_) {
    wl_compositor_destroy(compositor_);
    compositor_ = nullptr;
  }

  if (registry_) {
    wl_registry_destroy(registry_);
    registry_ = nullptr;
  }

  if (display_) {
    wl_display_flush(display_);
    wl_display_disconnect(display_);
    display_ = nullptr;
  }
}

bool WaylandDisplay::IsValid() const {
  return valid_;
}

ssize_t WaylandDisplay::vSyncHandler() {
  if (vsync.baton_ == 0) {
    return 0;
  }

  const auto t_now_ns                      = FlutterEngineGetCurrentTime();
  const uint64_t after_vsync_time_ns       = (t_now_ns - vsync.last_frame_) % vsync.vblank_time_ns_;
  const uint64_t before_next_vsync_time_ns = vsync.vblank_time_ns_ - after_vsync_time_ns;
  const uint64_t current_ns                = t_now_ns + before_next_vsync_time_ns;
  const uint64_t finish_time_ns            = current_ns + vsync.vblank_time_ns_;
  intptr_t baton                           = std::atomic_exchange(&vsync.baton_, 0);

  DBG_TIMING({
    auto tx = FlutterEngineGetCurrentTime();
    dbgI("[%09.4f][%ld] flutterEngineOnVsync [%jx]  (t:%09.4f d:%09.4f vb:%09.4f c:%09.4f f:%09.4f)\n", (tx - t00) / 1e9, gettid(), static_cast<uintmax_t>(baton), (t_now_ns - t00) / 1e9, (t_now_ns - vsync.last_frame_) / 1e9,
         vsync.vblank_time_ns_ / 1e9, (current_ns - t00) / 1e9, (finish_time_ns - t00) / 1e9);
  });

  const auto status = FlutterEngineOnVsync(engine_, baton, current_ns, finish_time_ns);

  if (status != kSuccess) {
    printf("[%ju]: ERROR: vsync.ntfy: FlutterEngineOnVsync failed(%d): baton: %p\n", t_now_ns, status, reinterpret_cast<void *>(baton));
    return -1;
  }

  return 1;
}

const struct wl_callback_listener WaylandDisplay::kFrameListener = {

    .done =
        [](void *data, struct wl_callback *cb, uint32_t callback_data) {
          WaylandDisplay *const wd = get_wayland_display(data);

          /* check if we have presentation time extension interface working */
          if (wd->vsync.presentation_clk_id_ != UINT32_MAX) {
            return;
          }

          wd->vsync.last_frame_ = FlutterEngineGetCurrentTime();
          wl_callback_destroy(cb);
          wl_callback_add_listener(wl_surface_frame(wd->surface_), &kFrameListener, data);
        }

};

ssize_t WaylandDisplay::vSyncReadNotifyData() {
  ssize_t rv;

  do {
    char c;
    rv = read(vsync.sv_[vsync.SOCKET_READER], &c, sizeof c);
  } while (rv == -1 && errno == EINTR);

  if (rv != 1) {
    printf("ERROR: Read error from vsync socket (rv: %zd, errno: %d)\n", rv, errno);
  }

  return rv;
}

ssize_t WaylandDisplay::vSyncSendNotifyData() {
  static unsigned char c = 0;
  ssize_t rv;

  c++;

  do {
    rv = write(vsync.sv_[vsync.SOCKET_WRITER], &c, sizeof c);
  } while (rv == -1 && errno == EINTR);

  if (rv != 1) {
    printf("ERROR: Write error to vsync socket (rv: %zd, errno: %d)\n", rv, errno);
  }

  return rv;
}

bool WaylandDisplay::Run() {
  if (!valid_) {
    dbgE("Could not run an invalid display.\n");
    return false;
  }

  const int fd = wl_display_get_fd(display_);

  wl_callback_add_listener(wl_surface_frame(surface_), &kFrameListener, this);

  if (kbd_grab_manager_ && getEnv("FLUTTER_WAYLAND_MAIN_UI", 0.) != 0.) {
    /* It's the main UI application, so check if we can receive all keys */
    printf("kbd_grab_manager: grabbing keyboard...\n");
    xwayland_keyboard_grab = zwp_xwayland_keyboard_grab_manager_v1_grab_keyboard(kbd_grab_manager_, surface_, seat_);
  }

  while (valid_) {
    while (wl_display_prepare_read(display_) != 0) {
      wl_display_dispatch_pending(display_);
    }

    wl_display_flush(display_);

    do {
      int rv;

      struct pollfd fds[4] = {
          {.fd = vsync.sv_[vsync.SOCKET_READER], .events = POLLIN, .revents = 0},
          {.fd = fd, .events = POLLIN | POLLERR, .revents = 0},
          {.fd = key.timer_fd_, .events = POLLIN | POLLERR, .revents = 0},
          {.fd = memory_watcher_.event_fd, .events = POLLIN | POLLERR, .revents = 0},
      };

      do {
        static const struct timespec ts = {
            .tv_sec  = LONG_MAX,
            .tv_nsec = 0,
        };

        rv = ppoll(&fds[0], std::size(fds), &ts, nullptr);
      } while (rv == -1 && errno == EINTR);

      if (rv == -1) {
        printf("ERROR: ppoll returned -1 (errno: %d)\n", errno);
        return false;
      }

      if (fds[2].revents & POLLIN) {
        uint64_t count;
        do {
          rv = read(fds[2].fd, &count, sizeof count);
        } while (rv == -1 && errno == EINTR);

        if (rv == -1) {
          printf("ERROR: read returned -1 (errno: %d)\n", errno);
          return false;
        }

        key_handler(key.last_, key.state_, true);
      }

      if (fds[0].revents & POLLIN) {
        auto rv = vSyncReadNotifyData();

        if (rv != 1) {
          return false;
        }

        if (vsync.presentation_clk_id_ != UINT32_MAX && presentation_ != nullptr) {
          DBG_TIMING({
            auto tx = FlutterEngineGetCurrentTime();
            dbgI("[%09.4f][%ld] add listener\n", (tx - t00) / 1e9, gettid());
          });
          wp_presentation_feedback_add_listener(::wp_presentation_feedback(presentation_, surface_), &kPresentationFeedbackListener, this);
          wl_display_dispatch_pending(display_);
        }

        rv = vSyncHandler();

        if (rv != 1) {
          return false;
        }

        continue;
      }

      if (fds[1].revents & POLLIN) {
        wl_display_read_events(display_);
      } else {
        wl_display_cancel_read(display_);
      }

      if (fds[3].revents & POLLIN) {
        uint64_t result;
        do {
          rv = read(fds[3].fd, &result, sizeof result);
        } while (rv == -1 && errno == EINTR);
        if (rv == -1) {
          dbgE(MEMWATCHTAG "problems reading event fd, errno=%d\n", errno);
        } else {
          HandleMemoryWatcherEvent();
        }
      }

      break;
    } while (true);

    wl_display_dispatch_pending(display_);
  }

  return true;
}

bool WaylandDisplay::SetupEGL() {

  egl_display_ = eglGetDisplay(display_);
  if (egl_display_ == EGL_NO_DISPLAY) {
    LogLastEGLError();
    dbgE("Could not access EGL display.\n");
    return false;
  }

  if (eglInitialize(egl_display_, nullptr, nullptr) != EGL_TRUE) {
    LogLastEGLError();
    dbgE("Could not initialize EGL display.\n");
    return false;
  }

  if (eglBindAPI(EGL_OPENGL_ES_API) != EGL_TRUE) {
    LogLastEGLError();
    dbgE("Could not bind the ES API.\n");
    return false;
  }

  EGLConfig egl_config = nullptr;

  // Choose an EGL config to use for the surface and context.
  {
    EGLint attribs[] = {
        // clang-format off
      EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
      EGL_SURFACE_TYPE,    EGL_WINDOW_BIT,
      EGL_RED_SIZE,        8,
      EGL_GREEN_SIZE,      8,
      EGL_BLUE_SIZE,       8,
      EGL_ALPHA_SIZE,      8,
      EGL_DEPTH_SIZE,      0,
      EGL_STENCIL_SIZE,    0,
      EGL_NONE,            // termination sentinel
        // clang-format on
    };

    EGLint config_count = 0;

    if (eglChooseConfig(egl_display_, attribs, &egl_config, 1, &config_count) != EGL_TRUE) {
      LogLastEGLError();
      dbgE("Error when attempting to choose an EGL surface config.\n");
      return false;
    }

    if (config_count == 0 || egl_config == nullptr) {
      LogLastEGLError();
      dbgE("No matching configs.\n");
      return false;
    }
  }

  const EGLint ctx_attribs[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};

  // Create an EGL context with the match config.
  {
    egl_context_ = eglCreateContext(egl_display_, egl_config, nullptr /* share group */, ctx_attribs);

    if (egl_context_ == EGL_NO_CONTEXT) {
      LogLastEGLError();
      dbgE("Could not create an onscreen context.\n");
      return false;
    }
  }

  if (!compositor_ || !shell_) {
    dbgE("EGL setup needs missing compositor and shell connection.\n");
    return false;
  }

  surface_ = wl_compositor_create_surface(compositor_);

  if (!surface_) {
    dbgE("Could not create compositor surface.\n");
    return false;
  }

  shell_surface_ = wl_shell_get_shell_surface(shell_, surface_);

  if (!shell_surface_) {
    dbgE("Could not shell surface.\n");
    return false;
  }

  wl_shell_surface_add_listener(shell_surface_, &kShellSurfaceListener, this);

  wl_shell_surface_set_title(shell_surface_, "Flutter");

  wl_shell_surface_set_toplevel(shell_surface_);

  window_ = wl_egl_window_create(surface_, screen_width_, screen_height_);

  if (!window_) {
    dbgE("Could not create EGL window.\n");
    return false;
  }

  const EGLint pbuffer_config_attribs[] = {EGL_HEIGHT, 64, EGL_WIDTH, 64, EGL_NONE};

  resource_egl_context_ = eglCreateContext(egl_display_, egl_config, egl_context_ /* share group */, ctx_attribs);
  resource_egl_surface_ = eglCreatePbufferSurface(egl_display_, egl_config, pbuffer_config_attribs);

  // Create an EGL window surface with the matched config.
  {
    const EGLint attribs[] = {EGL_NONE};

    egl_surface_ = eglCreateWindowSurface(egl_display_, egl_config, window_, attribs);

    if (egl_surface_ == EGL_NO_SURFACE) {
      LogLastEGLError();
      dbgE("EGL surface was null during surface selection.\n");
      return false;
    }
  }

  return true;
}
} // namespace flutter
