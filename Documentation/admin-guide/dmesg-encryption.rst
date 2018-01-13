Kernel message encryption
-------------------------

.. CONTENTS
..
.. - Overview
.. - Reason for encrypting dmesg
.. - Compile time and run time switches
.. - Limitations
.. - Decrypting dmesg


========
Overview
========

Similar to the module signing facility, it is also possible to have the kernel
perform public key encryption of the kernel messages that are being generated
by printk calls.

The encryption can be performed for one of the trusted public keys in the
kernel keyring, and by default will be performed against the kernel's module
signing key.

To prevent a run-time dependency inside printk itself, the encryption takes
place upon trying to read ``/dev/kmsg`` which is the mechanism currently used
by ``systemd`` to read kernel messages, and is also used by ``dmesg``
invocations.

The first line being read by a ``dmesg`` opener will be an artificial line
containing an encrypted symmetric encryption session key, in RSA PKCS#1 format.
The other lines are messages encrypted under an AES-128-GCM scheme. All binary
ciphertext is base64-encoded, so that the ciphertext solely comprises of
printable characters.

===========
Limitations
===========

There are various limitations one need to consider when enabling dmesg
encryption:

  * The metadata of kernel messages is not part of the encryption (timestamp,
    log facility, log severity).

  * The seldom accompanying dictionary is also not part of the encryption.

  * Any output to any system console, happening when printk() itself is
    executing, is also not encrypted. A potential attacker can load up
    ``netconsole`` and have kernel messages being sent as plaintext to other
    machines. Hopefully, on embedded devices, all system consoles are under
    strict control of the developers.

  * The syslog system call is barred from reading kmsg. Its present users are
    few, as the system call's interface is mostly a fallback to an inaccessible
    ``/dev/kmsg``. This is only an implementation limitation and that may be
    addressed.

  * kmsg buffers will still be saved as plaintext inside kdumps. The assumption
    is that having an access to read a kdump is equivalent to full kernel
    access anyway.

===========================
Reason for encryption dmesg
===========================

For years, dmesg has contained data which could be utilized by vulnerability
exploiters, allowing for privilege escalations. Developers may leave key data
such as pointers, indication of driver bugs, and more.

The feature is mostly aimed for device manufacturers who are not keen on
revealing the full details of kernel execution, bugs, and crashes to their
users, but only to their developers, so that local programs running on the
devices cannot use the data for 'rooting' and executing exploits.

==================================
Compile time and run time switches
==================================

In build time, this feature is controlled via the ``CONFIG_KMSG_ENCRYPTION``
configuration variable.

In run time, it can be turned off by providing `kmsg_encrypt=0` as a boot time
parameter.

================
Decrypting dmesg
================

A supplied program in the kernel tree named ``dmesg-decipher`` uses the OpenSSL
library along with the paired private key of the encryption in order to
decipher an encrypted dmesg.

An innocuous dmesg invocation will appear as such (with the ciphertexts
shortened here for the brevity of this document)::

    [    0.000000] K:Zzgt0ovlRvwH....fQgbQ2tdjOzgYFwrzHU00XO4=
    [    0.000000] M:ogoKk3kCb6q5....1z8BVLr903/w==,16,12
    [    0.000000] M:CcxUnMRIHrjD....o+c1Zes=,16,12
    ....

The artificial ``K:`` message is generated per opening of ``/dev/kmsg``. It
contains the encrypted session key. The encrypted dmesg lines follows it
(prefix ``M:``).

Provided with the private key, deciphering a dmesg output should be a
straightforward process.

For example, one can save an encrypted dmesg to ``dmesg.enc`` in one machine,
then transfer it to another machine which contains access to the PEM with the
decrypting private key, and use the the following command::

    cat dmesg.enc | ./tools/kmsg/dmesg-decipher certs/signing_key.pem

    [    0.000000] Linux version 4.15.0-rc5+ (dan@jupiter) (gcc version 7.2.1 20170915 (Red Hat 7.2.1-2) (GCC)) #109 SMP Sat Dec 30 18:32:25 IST 2017
    [    0.000000] Command line: BOOT_IMAGE=/vmlinuz-4.15.0-rc5-dan+ root=UUID=f48b37ec-fcb8-4689-b12e-58703db3cb21 ro rhgb quiet LANG=en_US.UTF-8
    [    0.000000] x86/fpu: Supporting XSAVE feature 0x001: 'x87 floating point registers'
    ...
