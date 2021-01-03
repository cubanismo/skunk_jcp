#!/bin/sh
#
# Run this after running notarize_submit.sh and waiting for the resulting
# notarization request to be completed by Apple.
#

if [ ! -r "$1" ]; then
  echo "Usage: notarize_staple.sh <installer_name.pkg>"
  exit 1
fi

xcrun stapler staple "$1"
