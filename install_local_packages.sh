#!/bin/sh
# Installs the packages the build has just produced, into the container that built them - the test
# suite is run against the installed copies, not against the build tree.
#
# POSIX sh, and run as "sh install_local_packages.sh" with no shebang honoured, so it must stay
# that way: on Debian and Ubuntu that sh is dash, which has no "[[". The test that guarded the
# .deb branch was written with one, so it failed on exactly the distributions that produce .debs -
# "install_local_packages.sh: 7: [[: not found" - and the packages went uninstalled while the
# build reported nothing wrong.
#
# Each candidate is tested with -f rather than listed with ls: an unmatched glob comes back as the
# pattern itself, and asking ls about it is where the "cannot access '*.rpm'" lines in every
# build log came from - an error message for the ordinary case of a distribution that packages
# the other way.

PACKAGES=""
for package_file in *.deb; do
    [ -f "$package_file" ] && PACKAGES="$PACKAGES ./$package_file"
done

if [ -n "$PACKAGES" ]; then
    apt -y install $PACKAGES
    rc=$?
    rm -f install_manifest_*
    exit $rc
fi

for package_file in *.rpm; do
    [ -f "$package_file" ] && PACKAGES="$PACKAGES ./$package_file"
done

if [ -n "$PACKAGES" ]; then
    yum -y install $PACKAGES
    rc=$?
    rm -f install_manifest_*
    exit $rc
fi

echo "No .deb or .rpm package to install here - the build produced none." >&2
exit 1
