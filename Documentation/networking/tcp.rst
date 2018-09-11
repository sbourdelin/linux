.. SPDX-License-Identifier: GPL-2.0

======================
TCP Congestion Control
======================

The following variables are used in the tcp_sock for congestion control

.. flat-table:: Congestion Control
    :widths: 1 2

    * - tcp_sock struct member
      - Usage

    * - snd_cwnd
      - The size of the congestion window

    * - snd_ssthresh
      - Slow start threshold.  We are in slow start if snd_cwnd is less
	than this.

    * - snd_cwnd_cnt
      - A counter used to slow down the rate of increase once we exceed
	slow start threshold.

    * - snd_cwnd_clamp
      - This is the maximum size that snd_cwnd can grow to.

    * - snd_cwnd_stamp
      - Timestamp for when congestion window last validated.

    * - snd_cwnd_used
      - Used as a highwater mark for how much of the congestion window
	is in use.  It is used to adjust snd_cwnd down when the link is
	limited by the application rather than the network.

As of 2.6.13, Linux supports pluggable congestion control algorithms.  A
congestion control mechanism can be registered through functions in
tcp_cong.c.  The functions used by the congestion control mechanism are
registered via passing a tcp_congestion_ops struct to
tcp_register_congestion_control.  As a minimum, the congestion control
mechanism must provide a valid name and must implement either ssthresh,
cong_avoid and undo_cwnd hooks or the "omnipotent" cong_control hook.

Private data for a congestion control mechanism is stored in
tp->ca_priv.  tcp_ca(tp) returns a pointer to this space.  This is
preallocated space - it is important to check the size of your private
data will fit this space, or alternatively, space could be allocated
elsewhere and a pointer to it could be stored here.

There are three kinds of congestion control algorithms currently: The
simplest ones are derived from TCP reno (highspeed, scalable) and just
provide an alternative congestion window calculation.  More complex ones
like BIC try to look at other events to provide better heuristics.
There are also round trip time based algorithms like Vegas and
Westwood+.

Good TCP congestion control is a complex problem because the algorithm
needs to maintain fairness and performance.  Please review current
research and RFC's before developing new modules.

The default congestion control mechanism is chosen based on the
DEFAULT_TCP_CONG Kconfig parameter.  If you really want a particular
default value then you can set it using sysctl
net.ipv4.tcp_congestion_control.  The module will be autoloaded if
needed and you will get the expected protocol.  If you ask for an
unknown congestion method, then the sysctl attempt will fail.

If you remove a TCP congestion control module, then you will get the
next available one.  Since reno cannot be built as a module, and cannot
be removed, it will always be available.
