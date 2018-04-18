//
// kdesk-blur.cpp  -  Blur the desktop and start an application on top
//
// Copyright (C) 2013-2014 Kano Computing Ltd.
// License: http://www.gnu.org/licenses/gpl-2.0.txt GNU General Public License v2
//
//

#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <Imlib2.h>

#include <stdio.h>
#include <stdlib.h>
#include <memory.h>
#include <pthread.h>
#include <unistd.h>

#include "logging.h"
#include "kdesk-blur.h"

void *BlurDesktop (void *pvnothing);
int main (int argc, char *argv[]);


// A printf macro sensitive to the -v (verbose) flag
// Use kprintf for regular stdout messages instead of printf or cout
bool verbose=false; // Mute by default, no stdout messages unless in Debug build
#define kprintf(fmt, ...) ( (verbose==false ? : printf(fmt, ##__VA_ARGS__) ))


//
// This function to be spawned in the background (p_thread)
// Will create a top-level window with a blurred snapshot of the desktop
//
void *BlurDesktop(void *pvnothing)
{
  Display *display=XOpenDisplay(NULL);
  if (!display) {
    log ("Could not connect to the XServer");
    return NULL;
  }

  Window winblur=0L;
  bool success = false;
  Pixmap pmap_blur = 0L;
  Imlib_Image img_blurred;
  Imlib_Color_Modifier colorTrans=NULL;

  int screen = DefaultScreen (display);
  Window root_window = RootWindow (display, screen);
  int deskw = DisplayWidth(display, screen);
  int deskh = DisplayHeight(display, screen);

  // Creating a desktop blur effect.
  pmap_blur = XCreatePixmap (display, root_window, deskw, deskh, DefaultDepth (display, DefaultScreen (display)));
  img_blurred = imlib_create_image(deskw, deskh);
  if (!pmap_blur || !img_blurred) {
    log ("no resources to create a blurred pixmap or imlib image buffer");
    success = false;
  }
  else {
    // Take a screenshot from the desktop and save it in a Imblib2 object
    // So we can use it's API to blur the image
    imlib_context_set_image(img_blurred);
    imlib_context_set_display(display);
    imlib_context_set_visual(DefaultVisual(display, 0));
    imlib_context_set_drawable(root_window);
    imlib_copy_drawable_to_image(0, 0, 0, deskw, deskh, 0, 0, 1);

    // Create RGB tables to blur the colors
    unsigned char iconR[256], iconG[256], iconB[256], iconTR[256];
    colorTrans = imlib_create_color_modifier();
    imlib_context_set_color_modifier(colorTrans);    
    imlib_get_color_modifier_tables(iconR, iconG, iconB, iconTR);
    imlib_reset_color_modifier();

    // Blur the RGB channels to a third of their intensities
    for (int n=0; n < 256; n++) {
      iconR[n] = iconR[n] / 3;
      iconG[n] = iconG[n] / 3;
      iconB[n] = iconB[n] / 3;
    }

    // At this point img_blurred contains a blurred copy of the desktop image
    imlib_set_color_modifier_tables (iconR, iconG, iconB, iconR);

    // create a top level window which will draw the blurred desktop on top
    XSetWindowAttributes attr;
    memset (&attr, 0x00, sizeof (attr));
    attr.background_pixmap = ParentRelative;

    // this will ensure the blurred image is repainted
    // should the top level unblurred window move along the desktop.
    attr.backing_store = Always;

    attr.event_mask = 0; // no relevant events we want to care about, XServer will do it for us
    attr.override_redirect = False;
    winblur = XCreateWindow (display, root_window, 0, 0,
			     //
			     // FIXME: 
			     // int 41 number needs to be fit with the amount of over-space used by the decorations.
			     //
			     deskw, deskh - 41, 0,
			     CopyFromParent, CopyFromParent, CopyFromParent,
			     CWEventMask, &attr);
    if (!winblur) {
      // Out of resources or problems with Xlib parameters
      log ("Could not create blur window");
      success = false;
    }
    else {
      // The "Hints" code below is needed to remove the window decorations
      // We don't want a title or border frames.
      // TODO: Remove the taskbar icon associated to the window.
      typedef struct Hints
      {
	unsigned long   flags;
	unsigned long   functions;
	unsigned long   decorations;
	long            inputMode;
	unsigned long   status;
      } Hints;
      
      Hints hints;
      Atom property_hints;
      hints.flags = 2;
      hints.decorations = 0;
      hints.inputMode = False;

      property_hints = XInternAtom(display, "_MOTIF_WM_HINTS", false);
      XChangeProperty (display, winblur, property_hints, property_hints, 32, PropModeReplace, (unsigned char *) &hints, 5);

      // _NET_WM_STATE_SKIP_TASKBAR Tells the window manager to not use an icon on the taskbar
      Atom net_wm_state = XInternAtom (display, "_NET_WM_STATE", false);
      Atom net_skip_taskbar = XInternAtom(display, "_NET_WM_STATE_SKIP_TASKBAR", false);
      XChangeProperty (display, winblur, net_wm_state,
		       XA_ATOM, 32, PropModeAppend,
		       (unsigned char *) &net_skip_taskbar, 1);

      // Give the blurred window a meaningful name.
      XStoreName (display, winblur, KDESK_BLUR_NAME);

      // Draw the blurred image into the pixmap, then apply the pixmap to the window
      // We do it this way so the XServer can have a copy for the backing store to repaint on Exposure events
      imlib_context_set_drawable(pmap_blur);
      imlib_render_image_on_drawable (0, 0);
      XSetWindowBackgroundPixmap(display, winblur, pmap_blur);

      // Map the window after setting the pixmap so it is displayed before the app that will run on top
      XMapWindow(display, winblur);
      XFlush(display);

      imlib_free_image();

      log1 ("Blur window created successfully (winid)", winblur);
      success = true;
    }
  }
  
  return NULL; // we are launched in the background, use IsDesktopBlurred to find blur result
}

bool IsDesktopBlurred (void)
{
  Display *display=XOpenDisplay(NULL);
  if (!display) {
    log ("Could not connect to the XServer");
    return false;
  }

  int screen = DefaultScreen (display);
  Window root = RootWindow (display, screen);
  Window root_return, parent_return, *children_return=NULL, *subchildren_return=NULL;
  unsigned int nchildren_return=0, nsubchildren_return=0;
  bool found = false;

  // Enumerate all top level windows in search for the Kdesk's blurred window
  if (XQueryTree(display, root, &root_return, &parent_return, &children_return, &nchildren_return))
    {
      char *windowname=NULL;
      for (int i=0; i < nchildren_return; i++)
	{
	  if (XFetchName (display, children_return[i], &windowname)) {
	    if (!strncmp (windowname, KDESK_BLUR_NAME, strlen (KDESK_BLUR_NAME))) {
	      log1 ("Blurred window was found level1 (winid)", children_return[i]);
	      found = true;
	      XFree (windowname);
	      break;
	    }
	  }

	  XQueryTree (display, children_return[i], &root_return, &parent_return, &subchildren_return, &nsubchildren_return);

	  for (int k=nsubchildren_return-1; k>=0; k--) {
	    if (XFetchName (display, subchildren_return[k], &windowname)) {
	      if (!strncmp (windowname, KDESK_BLUR_NAME, strlen (KDESK_BLUR_NAME))) {
		log1 ("Blurred window was found level2 (winid)", subchildren_return[i]);
		found=true;
		XFree (windowname);
		break;
	      }
	    }
	  }

	}
    }

  if (children_return) {
    XFree(children_return);
  }

  if (subchildren_return) {
    XFree(subchildren_return);
  }

  return found;
}

int main (int argc, char *argv[])
{
  bool blurred = false;
  char *cmdline=NULL;
  int timeout = 5; // seconds to wait for blur to become visible
  pthread_t t;

  // collect top application command-line
  if (argc < 2) {
    printf ("Syntax: kdesk-blur <app name> [-v]\n");
    printf (" Use double quotation marks and escaping for multiple arguments:\n");
    printf (" $ kdesk-blur 'lxterminal --command=\"/bin/bash -c \\\"ls -l ; sleep 5\\\"\"'\n");
    printf (" -v will emit messages during the process\n");
    printf (" Error level will be set to -1 if blur error, otherwise the app's rc will be set\n");
    exit (-1);
  }
  else {
    cmdline = strdup (argv[1]);
    if (argc > 2 && !strcasecmp (argv[2], "-v")) {
      verbose = true;
    }
  }

  // if desktop is not blurred yet, create the blur window
  if (!IsDesktopBlurred()) {
    kprintf ("Blurring the desktop\n");
    log ("Blurring the desktop");
    pthread_create (&t, NULL, BlurDesktop, NULL);
    while (!(blurred=IsDesktopBlurred()) && timeout-- > 0)
      {
	sleep(1);
      }

    if (!blurred) {
      // There was a problem blurring the desktop
      kprintf ("Error blurring the desktop\n");
      log ("Error blurring the desktop");
      exit (-1);
    }
  }
  else {
    log ("Desktop is already blurred");
  }

  // Now we can execute the application on top
  kprintf ("Starting app on top of desktop: %s\n", cmdline);
  log1 ("Starting app on top of the desktop (cmdline)", cmdline);
  int rc = system (cmdline);
  log1 ("App has terminated (rc)", rc);
  kprintf ("App has terminated with rc=%d\n", rc);
  free (cmdline);
  exit (rc);
}
