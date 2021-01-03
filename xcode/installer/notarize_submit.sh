#!/bin/sh
#
# Run this script on a ready-to-distribute installer to submit it to Apple for
# notarization. Once the notarization request is completed, "staple" the
# notarization to the installer with notarize_staple.sh
#

if [ -z "$1" -o ! -r "$2" ]; then
  echo "Usage: notarize_submit.sh <apple_id> <installer_name.pkg>"
  exit 1
fi

xcrun altool --notarize-app \
  --primary-bundle-id "com.theinnocuous.com.JaguarCopy.pkg" \
  --username "$1" \
  --password "@keychain:notarize-apple-pass" \
  --file "$2" | tee -a notarization.log
