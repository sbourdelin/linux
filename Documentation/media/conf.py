# -*- coding: utf-8; mode: python -*-

project = 'Linux Media Subsystem Documentation'

tags.add("subproject")

latex_documents = [
    ('media_uapi', 'media_uapi.tex', 'Linux Media Infrastructure userspace API',
     'The kernel development community', 'manual'),
    ('media_kapi', 'media_kapi.tex', 'Media subsystem kernel internal API',
     'The kernel development community', 'manual'),
    ('dvb-drivers/index', 'dvb_drivers.tex', 'Linux Digital TV driver-specific documentation',
     'The kernel development community', 'manual'),
    ('v4l-drivers/index', 'v4l_drivers.tex', 'Video4Linux (V4L) driver-specific documentation',
     'The kernel development community', 'manual'),
]

# Since intersphinx is not activated in the global Documentation/conf.py we
# activate it here. If times comes where it is activated in the global conf.py,
# we may have to drop these two lines.
extensions.append('sphinx.ext.intersphinx')
intersphinx_mapping = {}

# add intersphinx inventory of the *complete* documentation from linuxtv.org
intersphinx_mapping['media'] = ('https://www.linuxtv.org/downloads/v4l-dvb-apis-new/', None)
