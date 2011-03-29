#!/bin/sh
git-buildpackage --git-upstream-branch=master --git-debian-branch=master -rfakeroot $@
