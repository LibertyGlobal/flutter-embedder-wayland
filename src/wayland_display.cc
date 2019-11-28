// Copyright 2018 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef WL_EGL_PLATFORM
#define WL_EGL_PLATFORM 1
#endif

#include "wayland_display.h"

#include <cassert>
#include <stdlib.h>
#include <unistd.h>
#include <poll.h>
#include <errno.h>

#include <cstring>

namespace flutter {

#define DISPLAY reinterpret_cast<WaylandDisplay *>(data)

const wl_registry_listener WaylandDisplay::kRegistryListener = {
    .global = [](void *data, struct wl_registry *wl_registry, uint32_t name, const char *interface, uint32_t version) -> void { DISPLAY->AnnounceRegistryInterface(wl_registry, name, interface, version); },

    .global_remove = [](void *data, struct wl_registry *wl_registry, uint32_t name) -> void { DISPLAY->UnannounceRegistryInterface(wl_registry, name); },
};

const wl_shell_surface_listener WaylandDisplay::kShellSurfaceListener = {
    .ping = [](void *data, struct wl_shell_surface *wl_shell_surface, uint32_t serial) -> void { wl_shell_surface_pong(DISPLAY->shell_surface_, serial); },

    .configure = [](void *data, struct wl_shell_surface *wl_shell_surface, uint32_t edges, int32_t width, int32_t height) -> void {
      WaylandDisplay *wd = reinterpret_cast<WaylandDisplay *>(data);

      if (wd == nullptr)
        return;

      if (wd->window_ == nullptr)
        return;

      wl_egl_window_resize(wd->window_, width, height, 0, 0);

      FLWAY_LOG << "Window resized: " << width << "x" << height << std::endl;
    },

    .popup_done = [](void *data, struct wl_shell_surface *wl_shell_surface) -> void {
      // Nothing to do.
    },
};

const wl_pointer_listener WaylandDisplay::kPointerListener = {
    .enter =
        [](void *data, struct wl_pointer *wl_pointer, uint32_t serial, struct wl_surface *surface, wl_fixed_t surface_x, wl_fixed_t surface_y) {

        },
    .leave =
        [](void *data, struct wl_pointer *wl_pointer, uint32_t serial, struct wl_surface *surface) {

        },
    .motion =
        [](void *data, struct wl_pointer *wl_pointer, uint32_t time, wl_fixed_t surface_x, wl_fixed_t surface_y) {

        },
    .button =
        [](void *data, struct wl_pointer *wl_pointer, uint32_t serial, uint32_t time, uint32_t button, uint32_t state) {

        },
    .axis =
        [](void *data, struct wl_pointer *wl_pointer, uint32_t time, uint32_t axis, wl_fixed_t value) {

        },
    .frame =
        [](void *data, struct wl_pointer *wl_pointer) {

        },

    .axis_source =
        [](void *data, struct wl_pointer *wl_pointer, uint32_t axis_source) {

        },
    .axis_stop =
        [](void *data, struct wl_pointer *wl_pointer, uint32_t time, uint32_t axis) {

        },
    .axis_discrete =
        [](void *data, struct wl_pointer *wl_pointer, uint32_t axis, int32_t discrete) {

        },

};

const wl_keyboard_listener WaylandDisplay::kKeyboardListener = {
    .keymap      = [](void *data, struct wl_keyboard *wl_keyboard, uint32_t format, int32_t fd, uint32_t size) {},
    .enter       = [](void *data, struct wl_keyboard *wl_keyboard, uint32_t serial, struct wl_surface *surface, struct wl_array *keys) {},
    .leave       = [](void *data, struct wl_keyboard *wl_keyboard, uint32_t serial, struct wl_surface *surface) {},
    .key         = [](void *data, struct wl_keyboard *wl_keyboard, uint32_t serial, uint32_t time, uint32_t key, uint32_t state) {},
    .modifiers   = [](void *data, struct wl_keyboard *wl_keyboard, uint32_t serial, uint32_t mods_depressed, uint32_t mods_latched, uint32_t mods_locked, uint32_t group) {},
    .repeat_info = [](void *data, struct wl_keyboard *wl_keyboard, int32_t rate, int32_t delay) {},
};

const wl_seat_listener WaylandDisplay::kSeatListener = {
    .capabilities =
        [](void *data, struct wl_seat *seat, uint32_t capabilities) {
          WaylandDisplay *wd = DISPLAY;

          if (wd == nullptr)
            return;

          if (wd->window_ == nullptr)
            return;

          assert(seat == wd->seat_);

          if (capabilities & WL_SEAT_CAPABILITY_POINTER) {
            printf("seat_capabilities - pointer\n");
            struct wl_pointer *pointer = wl_seat_get_pointer(seat);
            wl_pointer_add_listener(pointer, &kPointerListener, NULL);
          }

          if (capabilities & WL_SEAT_CAPABILITY_KEYBOARD) {
            printf("seat_capabilities - keyboard\n");
            struct wl_keyboard *keyboard = wl_seat_get_keyboard(seat);
            wl_keyboard_add_listener(keyboard, &kKeyboardListener, NULL);
          }

          if (capabilities & WL_SEAT_CAPABILITY_TOUCH) {
            printf("seat_capabilities - touch\n");
          }
        },

    .name =
        [](void *data, struct wl_seat *wl_seat, const char *name) {
          // Nothing to do.
        },
};

WaylandDisplay::WaylandDisplay(size_t width, size_t height)
    : screen_width_(width)
    , screen_height_(height) {
  if (screen_width_ == 0 || screen_height_ == 0) {
    FLWAY_ERROR << "Invalid screen dimensions." << std::endl;
    return;
  }

  display_ = wl_display_connect(nullptr);

  if (!display_) {
    FLWAY_ERROR << "Could not connect to the wayland display." << std::endl;
    return;
  }

  registry_ = wl_display_get_registry(display_);
  if (!registry_) {
    FLWAY_ERROR << "Could not get the wayland registry." << std::endl;
    return;
  }

  wl_registry_add_listener(registry_, &kRegistryListener, this);

  wl_display_roundtrip(display_);

  if (!SetupEGL()) {
    FLWAY_ERROR << "Could not setup EGL." << std::endl;
    return;
  }

  valid_ = true;
}

WaylandDisplay::~WaylandDisplay() {
  if (shell_surface_) {
    wl_shell_surface_destroy(shell_surface_);
    shell_surface_ = nullptr;
  }

  if (shell_) {
    wl_shell_destroy(shell_);
    shell_ = nullptr;
  }

  if (seat_) {
    wl_seat_destroy(seat_);
    seat_ = nullptr;
  }

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

bool WaylandDisplay::Run() {
  if (!valid_) {
    FLWAY_ERROR << "Could not run an invalid display." << std::endl;
    return false;
  }

  const int fd = wl_display_get_fd(display_);

  while (valid_) {
    while (wl_display_prepare_read(display_) < 0) {
      wl_display_dispatch_pending(display_);
    }

    wl_display_flush(display_);

    int rv;

    do {
      struct pollfd fds = {.fd = fd, .events = POLLIN};

      rv = poll(&fds, 1, 1);
    } while (rv == -1 && rv == EINTR);

    if (rv <= 0) {
      wl_display_cancel_read(display_);
    } else {
      wl_display_read_events(display_);
    }

    wl_display_dispatch_pending(display_);
  }

  return true;
}

static void LogLastEGLError() {
  struct EGLNameErrorPair {
    const char *name;
    EGLint code;
  };

#define _EGL_ERROR_DESC(a)                                                                                                                                                                                                                     \
  { #a, a }

  const EGLNameErrorPair pairs[] = {
      _EGL_ERROR_DESC(EGL_SUCCESS),     _EGL_ERROR_DESC(EGL_NOT_INITIALIZED), _EGL_ERROR_DESC(EGL_BAD_ACCESS),          _EGL_ERROR_DESC(EGL_BAD_ALLOC),         _EGL_ERROR_DESC(EGL_BAD_ATTRIBUTE),
      _EGL_ERROR_DESC(EGL_BAD_CONTEXT), _EGL_ERROR_DESC(EGL_BAD_CONFIG),      _EGL_ERROR_DESC(EGL_BAD_CURRENT_SURFACE), _EGL_ERROR_DESC(EGL_BAD_DISPLAY),       _EGL_ERROR_DESC(EGL_BAD_SURFACE),
      _EGL_ERROR_DESC(EGL_BAD_MATCH),   _EGL_ERROR_DESC(EGL_BAD_PARAMETER),   _EGL_ERROR_DESC(EGL_BAD_NATIVE_PIXMAP),   _EGL_ERROR_DESC(EGL_BAD_NATIVE_WINDOW), _EGL_ERROR_DESC(EGL_CONTEXT_LOST),
  };

#undef _EGL_ERROR_DESC

  const auto count = sizeof(pairs) / sizeof(EGLNameErrorPair);

  EGLint last_error = eglGetError();

  for (size_t i = 0; i < count; i++) {
    if (last_error == pairs[i].code) {
      FLWAY_ERROR << "EGL Error: " << pairs[i].name << " (" << pairs[i].code << ")" << std::endl;
      return;
    }
  }

  FLWAY_ERROR << "Unknown EGL Error" << std::endl;
}

bool WaylandDisplay::SetupEGL() {

  egl_display_ = eglGetDisplay(display_);
  if (egl_display_ == EGL_NO_DISPLAY) {
    LogLastEGLError();
    FLWAY_ERROR << "Could not access EGL display." << std::endl;
    return false;
  }

  if (eglInitialize(egl_display_, nullptr, nullptr) != EGL_TRUE) {
    LogLastEGLError();
    FLWAY_ERROR << "Could not initialize EGL display." << std::endl;
    return false;
  }

  if (eglBindAPI(EGL_OPENGL_ES_API) != EGL_TRUE) {
    LogLastEGLError();
    FLWAY_ERROR << "Could not bind the ES API." << std::endl;
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
      FLWAY_ERROR << "Error when attempting to choose an EGL surface config." << std::endl;
      return false;
    }

    if (config_count == 0 || egl_config == nullptr) {
      LogLastEGLError();
      FLWAY_ERROR << "No matching configs." << std::endl;
      return false;
    }
  }

  // Create an EGL context with the match config.
  {
    const EGLint attribs[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};

    egl_context_ = eglCreateContext(egl_display_, egl_config, nullptr /* share group */, attribs);

    if (egl_context_ == EGL_NO_CONTEXT) {
      LogLastEGLError();
      FLWAY_ERROR << "Could not create an onscreen context." << std::endl;
      return false;
    }
  }

  if (!compositor_ || !shell_) {
    FLWAY_ERROR << "EGL setup needs missing compositor and shell connection." << std::endl;
    return false;
  }

  surface_ = wl_compositor_create_surface(compositor_);

  if (!surface_) {
    FLWAY_ERROR << "Could not create compositor surface." << std::endl;
    return false;
  }

  shell_surface_ = wl_shell_get_shell_surface(shell_, surface_);

  if (!shell_surface_) {
    FLWAY_ERROR << "Could not shell surface." << std::endl;
    return false;
  }

  wl_shell_surface_add_listener(shell_surface_, &kShellSurfaceListener, this);

  wl_shell_surface_set_title(shell_surface_, "Flutter");

  wl_shell_surface_set_toplevel(shell_surface_);

  window_ = wl_egl_window_create(surface_, screen_width_, screen_height_);

  if (!window_) {
    FLWAY_ERROR << "Could not create EGL window." << std::endl;
    return false;
  }

  // Create an EGL window surface with the matched config.
  {
    const EGLint attribs[] = {EGL_NONE};

    egl_surface_ = eglCreateWindowSurface(egl_display_, egl_config, window_, attribs);

    if (egl_surface_ == EGL_NO_SURFACE) {
      LogLastEGLError();
      FLWAY_ERROR << "EGL surface was null during surface selection." << std::endl;
      return false;
    }
  }

  if (seat_) {
    wl_seat_add_listener(seat_, &kSeatListener, this);
  }

  return true;
}

void WaylandDisplay::AnnounceRegistryInterface(struct wl_registry *wl_registry, uint32_t name, const char *interface_name, uint32_t version) {
  if (strcmp(interface_name, "wl_compositor") == 0) {
    compositor_ = static_cast<decltype(compositor_)>(wl_registry_bind(wl_registry, name, &wl_compositor_interface, 1));
    return;
  }

  if (strcmp(interface_name, "wl_shell") == 0) {
    shell_ = static_cast<decltype(shell_)>(wl_registry_bind(wl_registry, name, &wl_shell_interface, 1));
    return;
  }

  if (strcmp(interface_name, "wl_seat") == 0) {
    seat_ = static_cast<decltype(seat_)>(wl_registry_bind(wl_registry, name, &wl_seat_interface, 1));
    return;
  }
}

void WaylandDisplay::UnannounceRegistryInterface(struct wl_registry *wl_registry, uint32_t name) {
}

// |flutter::FlutterApplication::RenderDelegate|
bool WaylandDisplay::OnApplicationContextMakeCurrent() {
  if (!valid_) {
    FLWAY_ERROR << "Invalid display." << std::endl;
    return false;
  }

  if (eglMakeCurrent(egl_display_, egl_surface_, egl_surface_, egl_context_) != EGL_TRUE) {
    LogLastEGLError();
    FLWAY_ERROR << "Could not make the onscreen context current" << std::endl;
    return false;
  }

  return true;
}

// |flutter::FlutterApplication::RenderDelegate|
bool WaylandDisplay::OnApplicationContextClearCurrent() {
  if (!valid_) {
    FLWAY_ERROR << "Invalid display." << std::endl;
    return false;
  }

  if (eglMakeCurrent(egl_display_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT) != EGL_TRUE) {
    LogLastEGLError();
    FLWAY_ERROR << "Could not clear the context." << std::endl;
    return false;
  }

  return true;
}

// |flutter::FlutterApplication::RenderDelegate|
bool WaylandDisplay::OnApplicationPresent() {
  if (!valid_) {
    FLWAY_ERROR << "Invalid display." << std::endl;
    return false;
  }

  if (eglSwapBuffers(egl_display_, egl_surface_) != EGL_TRUE) {
    LogLastEGLError();
    FLWAY_ERROR << "Could not swap the EGL buffer." << std::endl;
    return false;
  }

  return true;
}

// |flutter::FlutterApplication::RenderDelegate|
uint32_t WaylandDisplay::OnApplicationGetOnscreenFBO() {
  if (!valid_) {
    FLWAY_ERROR << "Invalid display." << std::endl;
    return 999;
  }

  return 0; // FBO0
}

} // namespace flutter
