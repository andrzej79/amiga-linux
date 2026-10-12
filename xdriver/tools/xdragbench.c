/*
 * Drag a window across the screen, the way a window manager's opaque move
 * does, and time it.  Run it over another client's window (an xterm full of
 * text) to include that client's expose redraws.
 *
 * usage: xdragbench x0 y0 x1 y1 [steps] [w h]
 */
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>

static double
now(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

int
main(int argc, char **argv)
{
	Display *d;
	Window win;
	XSetWindowAttributes a;
	GC gc;
	XEvent ev;
	int x0, y0, x1, y1, steps = 100, w = 320, h = 240, i;
	double t0, t1;

	if (argc < 5) {
		fprintf(stderr, "usage: %s x0 y0 x1 y1 [steps] [w h]\n", argv[0]);
		return 2;
	}
	x0 = atoi(argv[1]);
	y0 = atoi(argv[2]);
	x1 = atoi(argv[3]);
	y1 = atoi(argv[4]);
	if (argc > 5)
		steps = atoi(argv[5]);
	if (argc > 7) {
		w = atoi(argv[6]);
		h = atoi(argv[7]);
	}
	d = XOpenDisplay(NULL);
	if (!d) {
		fprintf(stderr, "cannot open display\n");
		return 1;
	}
	/* no window manager in the way: the server moves it directly */
	a.override_redirect = True;
	a.background_pixel = WhitePixel(d, DefaultScreen(d));
	win = XCreateWindow(d, DefaultRootWindow(d), x0, y0, w, h, 0, CopyFromParent,
			    InputOutput, CopyFromParent, CWOverrideRedirect | CWBackPixel, &a);
	XSelectInput(d, win, ExposureMask);
	gc = XCreateGC(d, win, 0, NULL);
	XMapRaised(d, win);
	do
		XNextEvent(d, &ev);
	while (ev.type != Expose);
	/* some content, so the copy is not of a blank window */
	for (i = 0; i < h; i += 12)
		XDrawLine(d, win, gc, 0, i, w, h - i);
	XDrawString(d, win, gc, 10, 20, "xdragbench", 10);
	XSync(d, False);

	t0 = now();
	for (i = 1; i <= steps; i++) {
		XMoveWindow(d, win, x0 + (x1 - x0) * i / steps, y0 + (y1 - y0) * i / steps);
		XSync(d, False);
	}
	t1 = now();
	printf("%d moves %dx%d (%d,%d)->(%d,%d): %.3f s, %.1f moves/s\n", steps, w, h,
	       x0, y0, x1, y1, t1 - t0, steps / (t1 - t0));
	XCloseDisplay(d);
	return 0;
}
