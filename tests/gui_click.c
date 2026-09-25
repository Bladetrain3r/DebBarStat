#define _POSIX_C_SOURCE 200809L
#include <X11/Xlib.h>
#include <X11/keysym.h>

#include <stdio.h>
#include <string.h>
#include <time.h>

int main(int argc, char **argv) {
    Display *display = XOpenDisplay(NULL);
    if (!display) return 1;
    Window root = DefaultRootWindow(display), target = None;
    struct timespec pause = {.tv_sec = 0, .tv_nsec = 100000000};
    for (int attempt = 0; attempt < 50 && target == None; attempt++) {
        Window parent, *children = NULL;
        unsigned count = 0;
        if (XQueryTree(display, root, &root, &parent, &children, &count)) {
            for (unsigned i = 0; i < count; i++) {
                char *name = NULL;
                if (XFetchName(display, children[i], &name) && name) {
                    if (!strncmp(name, "DebBarStat", 10)) target = children[i];
                    XFree(name);
                }
            }
            if (children) XFree(children);
        }
        if (target == None) nanosleep(&pause, NULL);
    }
    if (target == None) { fprintf(stderr, "DebBarStat window not found\n"); return 1; }
    nanosleep(&pause, NULL);
    XEvent event = {0};
    int sent;
    if (argc > 1 && !strcmp(argv[1], "export")) {
        event.xkey.type = KeyPress;
        event.xkey.display = display;
        event.xkey.window = target;
        event.xkey.root = DefaultRootWindow(display);
        event.xkey.keycode = XKeysymToKeycode(display, XK_e);
        event.xkey.same_screen = True;
        sent = XSendEvent(display, target, False, KeyPressMask, &event);
    } else {
        event.xbutton.type = ButtonPress;
        event.xbutton.display = display;
        event.xbutton.window = target;
        event.xbutton.root = DefaultRootWindow(display);
        event.xbutton.time = CurrentTime;
        event.xbutton.x = 20;
        event.xbutton.y = 158;
        event.xbutton.state = ControlMask;
        event.xbutton.button = Button1;
        event.xbutton.same_screen = True;
        sent = XSendEvent(display, target, False, ButtonPressMask, &event);
    }
    XFlush(display);
    XCloseDisplay(display);
    return sent ? 0 : 1;
}
