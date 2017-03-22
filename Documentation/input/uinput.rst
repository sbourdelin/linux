=============
uinput module
=============

Introduction
============

uinput is a kernel module that makes possible create and handle input devices from userspace. By using /dev/uinput (or /dev/input/uinput), a process can create virtual devices and emit events like key pressing, mouse movements and joystick buttons.

Interface
=========

::

  linux/uinput.h

The uinput header defines ioctl request keys to create, setup and destroy virtual devices, along with ioctls specific to uinput devices, like enabling events and keys to be send to the kernel.

Examples
========

1.0 Keyboard events
-------------------

This first example shows how to create a new virtual device and how to send a key event as well as a physical keyboard. All default imports and error handlers were removed for the sake of simplicity.

.. code-block:: c

   #include <linux/uinput.h>

   int fd;

   void emit(int type, int code, int val)
   {
        struct input_event ie;
        memset(&ie, 0, sizeof(ie));
        ie.type = type;
        ie.code = code;
        ie.value = val;

        if (write(fd, &ie, sizeof(ie)) < 0) {
                perror("write2");
                exit(1);
        }
   }

   int main() {
        struct input_id uid;
        struct uinput_setup usetup;

        fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
        ioctl(fd, UI_SET_EVBIT, EV_KEY);
        ioctl(fd, UI_SET_KEYBIT, KEY_SPACE);

        memset(&uid, 0, sizeof(iod));
        memset(&usetup, 0, sizeof(usetup));
        usetup.id = uid;
        strcpy(usetup.name, "ex_device");

        ioctl(fd, UI_DEV_SETUP, &usetup);
        ioctl(fd, UI_DEV_CREATE);

        /* wait some time until the Window Manager can get the reference for the
         * new virtual device to receive data from
         * */
        sleep(1);

        /* send key press, report the event, send key release, and report again */
        emit(EV_KEY, KEY_SPACE, 1);
        emit(EV_SYN, SYN_REPORT, 0);
        emit(EV_KEY, KEY_SPACE, 0);
        emit(EV_SYN, SYN_REPORT, 0);

        close(fd);

        return 0;
   }

2.0 Mouse movements
-------------------

This example shows how to create a virtual device who behaves like a physical mouse.

.. code-block:: c

    int i = 50;

    /* emit function is the same of the example above */

    void emit_rel(int code, int val, int syn)
    {
            emit(EV_REL, code, val);
            if (syn)
                    emit(EV_SYN, SYN_REPORT, 0);
    }

    /* ...open uinput file as shown in the previous example... */

    /* enable mouse button left and relative events. This makes the Window Manager to interpret this
     * device as a physical mouse
     */
    if (ioctl(fd, UI_SET_EVBIT, EV_KEY) == -1) {
            perror("ioctl0");
            exit(1);
    }

    if (ioctl(fd, UI_SET_KEYBIT, BTN_LEFT) == -1) {
            perror("ioctl0.1");
            exit(1);
    }

    if (ioctl(fd, UI_SET_EVBIT, EV_REL) == -1) {
            perror("ioctl1");
            exit(1);
    }

    if (ioctl(fd, UI_SET_RELBIT, REL_X) == -1) {
            perror("ioctl2");
            exit(1);
    }

    if (ioctl(fd, UI_SET_RELBIT, REL_Y) == -1) {
            perror("ioctl3");
            exit(1);
    }

    /* ...device setup, device create... */

    /* Give some time for the Window Manager to get events of the new virtual device */
    sleep(1);

    /* moves the mouse diagonally, 5 units per axis */
    while (i--) {
                emit_rel(REL_X, 5, 0);
                emit_rel(REL_Y, 5, 1);
                usleep(15000);
    }

    /* device destroy, device close */
    return 0;

3.0 uinput old interface
------------------------

Before kernel 4.5, uinput didn't have an ioctl to setup a virtual device. When running a version prior to 4.5, the user needs to fill a different struct and call write on the uinput file descriptor.

.. code-block:: c

        /* add include of uinput header */
        struct uinput_user_dev uud;

        /* open uinput device, and set the proper events */

        memset(&uud, 0 sizeof(uud));
        snprintf(uud.name, UINPUT_MAX_NAME_SIZE, "uinput_old_style");
        write(fd, &uud, sizeof(uud));

        /* call DEV_CREATE ioctl, and emit the events */
