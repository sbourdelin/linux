=============
uinput module
=============

Introduction
============

uinput is a kernel module that makes it possible to emulate input devices from
userspace. By writing to the module's /dev/uinput (or /dev/input/uinput), a
process can create a virtual device with specific capabilities.
Once created, the process can send events through that virtual device.

Interface
=========

::

  linux/uinput.h

The uinput header defines ioctls to create, setup and destroy virtual devices.

libevdev
========

libevdev is a wrapper library for evdev devices that provides interfaces to
create uinput devices and send events. libevdev is less error-prone than
accessing uinput directly and should be considered for new software

For examples and more information about libevdev:
https://www.freedesktop.org/software/libevdev/doc/latest/

Examples
========

Keyboard events
---------------

This first example shows how to create a new virtual device and how to send a
key event. All default imports and error handlers were removed for the sake of
simplicity.

.. code-block:: c

   #include <linux/uinput.h>

   int fd;

   void emit(int type, int code, int val)
   {
        struct input_event ie;

        ie.type = type;
        ie.code = code;
        ie.value = val;
        /* below timestamp values are ignored */
        ie.time.tv_sec = 0;
        ie.time.tv_usec = 0;

        write(fd, &ie, sizeof(ie));
   }

   int main() {
        struct uinput_setup usetup;

        fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);

        /* the ioctls below enables the to be created device to key
         * events, in this case the space key
         */
        ioctl(fd, UI_SET_EVBIT, EV_KEY);
        ioctl(fd, UI_SET_KEYBIT, KEY_SPACE);

        memset(&usetup, 0, sizeof(usetup));
        usetup.id.bustype = BUS_USB;
        usetup.id.vendor = 0x1234; /* sample vendor */
        usetup.id.product = 0x5678; /* sample product */
        strcpy(usetup.name, "Example device");

        ioctl(fd, UI_DEV_SETUP, &usetup);
        ioctl(fd, UI_DEV_CREATE);

        /*
         * On UI_DEV_CREATE the kernel creates the device nodes for this device.
         * Insert a pause so that userspace has time to detect, initialize the
         * new device, and can start to listen to events from this device
         */
        sleep(1);

        /* key press, report the event, send key release, and report again */
        emit(EV_KEY, KEY_SPACE, 1);
        emit(EV_SYN, SYN_REPORT, 0);
        emit(EV_KEY, KEY_SPACE, 0);
        emit(EV_SYN, SYN_REPORT, 0);

        ioctl(fd, UI_DEV_DESTROY);
        close(fd);

        return 0;
   }

Mouse movements
---------------

This example shows how to create a virtual device that behaves like a physical
mouse.

.. code-block:: c

    #include <linux/uinput.h>

    /* emit function is identical to of the first example */

    struct uinput_setup usetup;
    int i = 50;

    fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);

    /* enable mouse button left and relative events */
    ioctl(fd, UI_SET_EVBIT, EV_KEY);
    ioctl(fd, UI_SET_KEYBIT, BTN_LEFT);

    ioctl(fd, UI_SET_EVBIT, EV_REL);
    ioctl(fd, UI_SET_RELBIT, REL_X);
    ioctl(fd, UI_SET_RELBIT, REL_Y);

    memset(&usetup, 0, sizeof(usetup));
    usetup.id.bustype = BUS_USB;
    usetup.id.vendor = 0x1234; /* sample vendor */
    usetup.id.product = 0x5678; /* sample product */
    strcpy(usetup.name, "Example device");

    ioctl(fd, UI_DEV_SETUP, &usetup);
    ioctl(fd, UI_DEV_CREATE);

    /*
     * On UI_DEV_CREATE the kernel creates the device nodes for this device.
     * Insert a pause so that userspace has time to detect, initialize the
     * new device, and can start to listen to events from this device
     */
    sleep(1);

    /* moves the mouse diagonally, 5 units per axis */
    while (i--) {
        emit(EV_REL, REL_X, 5);
        emit(EV_REL, REL_Y, 5);
        emit(EV_SYN, SYN_REPORT, 0);
        usleep(15000);
    }

    ioctl(fd, UI_DEV_DESTROY);
    close(fd);

    return 0;

uinput old interface
--------------------

Before uinput version 5, there wasn't a proper ioctl to setup a virtual device.
In this case, the user neesa to fill a different struct and call write o the
uinput file descriptor to configure the new uinput device.

.. code-block:: c

    #include <linux/uinput.h>

    /* emit function is identical to of the first example */

    struct uinput_user_dev uud;
    int version;

    fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    ioctl(fd, UI_GET_VERSION, &version);

    if (version < 5) {
        /*
         * the ioctls below enables the to be created device to key
         * events, in this case the space key
         */
        ioctl(fd, UI_SET_EVBIT, EV_KEY);
        ioctl(fd, UI_SET_KEYBIT, KEY_SPACE);

        memset(&uud, 0, sizeof(uud));
        snprintf(uud.name, UINPUT_MAX_NAME_SIZE, "uinput old interface");
        write(fd, &uud, sizeof(uud));

        ioctl(fd, UI_DEV_CREATE);

        /*
         * On UI_DEV_CREATE the kernel creates the device nodes for this device.
         * Insert a pause so that userspace has time to detect, initialize the
         * new device, and can start to listen to events from this device
         */
        sleep(1);

        /* key press, report the event, send key release, and report again */
        emit(EV_KEY, KEY_SPACE, 1);
        emit(EV_SYN, SYN_REPORT, 0);
        emit(EV_KEY, KEY_SPACE, 0);
        emit(EV_SYN, SYN_REPORT, 0);

        ioctl(fd, UI_DEV_DESTROY);
    }

    close(fd);

    return 0;

