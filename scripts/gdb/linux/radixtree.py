#
# gdb helper commands and functions for Linux kernel debugging
#
#  Radix Tree Parser
#
# Copyright (c) 2016 Linaro Ltd
#
# Authors:
#  Kieran Bingham <kieran.bingham@linaro.org>
#
# This work is licensed under the terms of the GNU GPL version 2.
#

import gdb

from linux import utils
from linux import constants

radix_tree_root_type = utils.CachedType("struct radix_tree_root")
radix_tree_node_type = utils.CachedType("struct radix_tree_node")


def is_indirect_ptr(node):
    long_type = utils.get_long_type()
    return (node.cast(long_type) & constants.LX_RADIX_TREE_INDIRECT_PTR)


def indirect_to_ptr(node):
    long_type = utils.get_long_type()
    node_type = node.type
    indirect_ptr = node.cast(long_type) & ~constants.LX_RADIX_TREE_INDIRECT_PTR
    return indirect_ptr.cast(node_type)


def maxindex(height):
    height = height & constants.LX_RADIX_TREE_HEIGHT_MASK
    return gdb.parse_and_eval("height_to_maxindex["+str(height)+"]")


def lookup(root, index):
    node = root['rnode']
    if node is 0:
        return None

    if not (is_indirect_ptr(node)):
        if (index > 0):
            return None
        return node

    node = indirect_to_ptr(node)

    height = node['path'] & constants.LX_RADIX_TREE_HEIGHT_MASK
    if (index > maxindex(height)):
        return None

    shift = (height-1) * constants.LX_RADIX_TREE_MAP_SHIFT

    while True:
        new_index = (index >> shift) & constants.LX_RADIX_TREE_MAP_MASK
        slot = node['slots'][new_index]

        # Below needs a bit more verification ...
        # node = rcu_dereference_raw(*slot);
        node = slot.cast(node.type.pointer()).dereference()
        if node is 0:
            return None

        shift -= constants.LX_RADIX_TREE_MAP_SHIFT
        height -= 1

        if (height <= 0):
            break

    return node
