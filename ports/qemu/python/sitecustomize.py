"""Compatibility for native Python build tools on Coast's RAMFS.

Linux xattr syscalls are absent on Coast. Python's shutil accepts ENOTSUP for
optional filesystem attributes, but not ENOSYS. Translate only that errno;
ordinary permission, path and I/O errors still propagate unchanged.
"""
import errno
import functools
import os
import platform


def unsupported_xattr(function):
    @functools.wraps(function)
    def call(*args, **kwargs):
        try:
            return function(*args, **kwargs)
        except OSError as error:
            if error.errno != errno.ENOSYS:
                raise
            raise OSError(errno.ENOTSUP, os.strerror(errno.ENOTSUP), error.filename) from error
    return call


if platform.system() == 'Coast':
    for name in ('listxattr', 'getxattr', 'setxattr', 'removexattr'):
        if hasattr(os, name):
            setattr(os, name, unsupported_xattr(getattr(os, name)))
