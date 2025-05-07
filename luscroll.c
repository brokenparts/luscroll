#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <libevdev/libevdev.h>
#include <libevdev/libevdev-uinput.h>
#include <libudev.h>
#include <linux/input-event-codes.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define DEV_INPUT "/dev/input"

#define LUSCROLL_UNUSED(var) ((void)(var))

enum {
  MOUSE_STATE_NONE      = 0,
  MOUSE_STATE_DEAD      = 1 << 0,
  MOUSE_STATE_SCROLLING = 1 << 2,
};

typedef struct LUMouse LUMouse;
struct LUMouse {
  int                     fd;
  struct libevdev*        dev;
  struct libevdev_uinput* dev_uinput;
  int                     state;                    
  LUMouse*                next;
};

static struct {
  struct udev*          udev;
  struct udev_monitor*  udev_mon;
  LUMouse*              mice;
  bool                  stopped;
} g = { };

#define msg(...) printf("luscroll | " __VA_ARGS__); printf("\n")

static bool start_with(const char* str, const char* substr) {
  return !strncmp(str, substr, strlen(substr));
}

static void unregister_mouse(LUMouse* mouse) {
  if (!mouse) {
    return;
  }
  if (g.mice == mouse) {
    g.mice = mouse->next;
  } else {
    for (LUMouse* m = g.mice; m; m = m->next) {
      if (m->next == mouse) {
        m->next = m->next->next;
        break;
      }
    }
  }
  libevdev_grab(mouse->dev, LIBEVDEV_UNGRAB);
  libevdev_free(mouse->dev);
  close(mouse->fd);
  free(mouse);
}

static void check_register_mouse(const char* path) {
  if (!path || !start_with(path, DEV_INPUT "/event")) {
    return;
  }

  bool is_ok = false;

  LUMouse* mouse = calloc(1, sizeof(*mouse));
  if (mouse) {
    mouse->fd = open(path, O_RDONLY | O_NONBLOCK);

    int ev_status = libevdev_new_from_fd(mouse->fd, &mouse->dev);
    if (ev_status >= 0) {
      const bool is_mouse =
        libevdev_has_event_type(mouse->dev, EV_REL) &&
        libevdev_has_event_code(mouse->dev, EV_KEY, BTN_MIDDLE);

      if (is_mouse) {
        ev_status = libevdev_uinput_create_from_device(mouse->dev, LIBEVDEV_UINPUT_OPEN_MANAGED, &mouse->dev_uinput);
        if (ev_status >= 0) {
          ev_status = libevdev_grab(mouse->dev, LIBEVDEV_GRAB);
          if (ev_status >= 0) {
            is_ok = true;
          } else {
            msg("Failed to grab device %s: %s", path, strerror(-ev_status));  
          }
        } else {
          msg("Failed to create uinput from device %s: %s", path, strerror(-ev_status));
        }
      }
    } else {
      msg("Failed to open device %s: %s", path, strerror(-ev_status));
    }
  }

  if (!is_ok) {
    unregister_mouse(mouse);
    return;
  }

  mouse->next = g.mice;
  g.mice = mouse;
  msg("Registered mouse %s (%s)", libevdev_get_name(mouse->dev), path);
}

static void unregister_dead_mice(void) {
  LUMouse* next_mouse = 0;
  for (LUMouse* mouse = g.mice; mouse; mouse = next_mouse) {
    next_mouse = mouse->next;
    if (mouse->state & MOUSE_STATE_DEAD) {
      unregister_mouse(mouse);
    }
  }
}

static void sig_handler(int sig) {
  LUSCROLL_UNUSED(sig);
  g.stopped = true;
}

int main(int argc, const char* argv[]) {
  LUSCROLL_UNUSED(argc);
  LUSCROLL_UNUSED(argv);

  if (getuid() != 0) {
    msg("Program must run as root");
    return EXIT_FAILURE;
  }

  signal(SIGINT, sig_handler);

  DIR* input_dir = opendir(DEV_INPUT);
  if (!input_dir) {
    msg("Could not access " DEV_INPUT);
    return EXIT_FAILURE;
  }
  struct dirent* input_dir_entry = 0;
  while ((input_dir_entry = readdir(input_dir))) {
    char full_path[512];
    snprintf(full_path, sizeof(full_path), "%s/%s", DEV_INPUT, input_dir_entry->d_name);
    check_register_mouse(full_path);
  }
  closedir(input_dir);

  if (!(g.udev = udev_new())) {
    msg("Failed to initialize udev");
    return EXIT_FAILURE;
  }

  if (!(g.udev_mon = udev_monitor_new_from_netlink(g.udev, "udev"))) {
    msg("Failed to initialize udev monitor");
    return EXIT_FAILURE;
  }

  if (udev_monitor_enable_receiving(g.udev_mon) < 0) {
    msg("Failed to start udev monitor");
    return EXIT_FAILURE;
  }

  while (!g.stopped) {
    struct udev_device* udev_dev = 0;
    while ((udev_dev = udev_monitor_receive_device(g.udev_mon))) {
      if (!strcmp(udev_device_get_action(udev_dev), "add")) {
        check_register_mouse(udev_device_get_devnode(udev_dev));
      }
      
      udev_device_unref(udev_dev);
    }

    for (LUMouse* mouse = g.mice; mouse; mouse = mouse->next) {
      struct input_event evt = { 0 };
      int status = 0;
      do {
        if ((status = libevdev_next_event(mouse->dev, LIBEVDEV_READ_FLAG_NORMAL, &evt)) < 0) {
          break;
        }

#if 0
        msg("Event from %s:\n\t.type = %s\n\t.code = %s\n\t.value = %d",
          libevdev_get_name(mouse->dev),
          libevdev_event_type_get_name(evt.type),
          libevdev_event_code_get_name(evt.type, evt.code),
          evt.value
        );
#endif

        // Enter/exit scrolling state
        if (evt.type == EV_KEY && evt.code == BTN_MIDDLE) {
          if (evt.value) {
            mouse->state |= MOUSE_STATE_SCROLLING;
          } else {
            mouse->state &= ~MOUSE_STATE_SCROLLING;
          }
        }

        if (mouse->state & MOUSE_STATE_SCROLLING) {
          if (evt.type == EV_REL && evt.code == REL_Y) {
            libevdev_uinput_write_event(mouse->dev_uinput, EV_REL, REL_WHEEL_HI_RES, -evt.value * 2);
          }
        } else {
          libevdev_uinput_write_event(mouse->dev_uinput, evt.type, evt.code, evt.value);
        }
      } while (true);
      if (status != -EAGAIN) {
        msg("libevdev_next_event returned %d (%s), dropping...", status, strerror(-status));
        mouse->state |= MOUSE_STATE_DEAD;
      }
    }

    unregister_dead_mice();
  }

  for (LUMouse* mouse = g.mice; mouse; mouse = mouse->next) {
    mouse->state |= MOUSE_STATE_DEAD;
  }
  unregister_dead_mice();
}