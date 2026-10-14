#!/usr/bin/env fish

set -gx PATH "$HOME/opt/cross/bin" $PATH

set PROJECT_ROOT (path resolve (dirname (status filename))/..)
make -C "$PROJECT_ROOT" $argv
